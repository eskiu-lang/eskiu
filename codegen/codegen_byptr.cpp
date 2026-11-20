// codegen_byptr.cpp: aggregate parameters of Eskiu functions passed by pointer.
#include "codegen.h"
#include "llvm/IR/IRBuilder.h"

// An aggregate argument over 16 bytes (the sret threshold) goes by pointer. As a
// first-class value LLVM spreads it into one argument per scalar at every call (on
// WebAssembly one parameter each, of which an engine accepts at most 1000) and the
// caller rebuilds it field by field.
static bool passedByPointer(llvm::Type* t, const llvm::DataLayout& dl) {
    return (t->isStructTy() || t->isArrayTy()) && dl.getTypeAllocSize(t).getFixedValue() > 16;
}

// The function type with each such parameter as a pointer, or null when it has none.
static llvm::FunctionType* byPointerType(llvm::FunctionType* ft, const llvm::DataLayout& dl) {
    bool any = false;
    std::vector<llvm::Type*> ps;
    for (llvm::Type* p : ft->params()) {
        if (passedByPointer(p, dl)) { ps.push_back(llvm::PointerType::get(ft->getContext(), 0)); any = true; }
        else ps.push_back(p);
    }
    return any ? llvm::FunctionType::get(ft->getReturnType(), ps, ft->isVarArg()) : nullptr;
}

static const char* kFreshTemp = "eskiu.fresh";

// A temporary that only the one load of its value reads (a struct literal, a variant, a
// sret result): passed by pointer it needs no copy of its own.
void CodeGen::markFreshTemp(llvm::Value* alloca) {
    if (auto* ai = llvm::dyn_cast<llvm::AllocaInst>(alloca))
        ai->setMetadata(kFreshTemp, llvm::MDNode::get(*context, {}));
}

// The callee owns the copy it is given: it is the parameter's storage, so by-value
// semantics are unchanged (a change in the callee never reaches the caller). The caller
// makes the copy in its frame where the value is loaded, so a later argument cannot change
// it (with a memcpy, or freestanding a load and a store); a struct literal or a sret result
// is already a temporary nothing else reads, passed as it is. Functions with the C
// convention (lowered externs, C-ABI thunks) and their calls are left alone.
void CodeGen::passAggregatesByPointer(llvm::Module& m) {
    const llvm::DataLayout& dl = m.getDataLayout();
    auto cConv = [&](llvm::Function* f) { return cConvFns.count(f) > 0; };
    std::vector<llvm::Function*> fns;
    for (llvm::Function& f : m)
        if (!f.isIntrinsic() && !cConv(&f) && byPointerType(f.getFunctionType(), dl)) fns.push_back(&f);
    std::vector<llvm::CallBase*> calls;
    std::vector<llvm::Instruction*> fresh;
    for (llvm::Function& f : m)
        for (llvm::BasicBlock& bb : f)
            for (llvm::Instruction& i : bb) {
                if (i.hasMetadata(kFreshTemp)) fresh.push_back(&i);
                if (auto* cb = llvm::dyn_cast<llvm::CallBase>(&i)) {
                    if (cb->isInlineAsm()) continue;
                    llvm::Function* callee = cb->getCalledFunction();
                    if (callee && (callee->isIntrinsic() || cConv(callee))) continue;
                    if (byPointerType(cb->getFunctionType(), dl)) calls.push_back(cb);
                }
            }
    std::vector<llvm::Instruction*> dead;
    for (llvm::CallBase* cb : calls) {
        llvm::FunctionType* oldTy = cb->getFunctionType();
        llvm::FunctionType* newTy = byPointerType(oldTy, dl);
        llvm::Function* caller = cb->getFunction();
        llvm::IRBuilder<> eb(&caller->getEntryBlock(), caller->getEntryBlock().getFirstInsertionPt());
        llvm::IRBuilder<> b(cb);
        std::vector<llvm::Value*> args;
        for (unsigned i = 0; i < cb->arg_size(); ++i) {
            llvm::Value* a = cb->getArgOperand(i);
            llvm::Type* at = i < oldTy->getNumParams() ? oldTy->getParamType(i) : nullptr;
            if (!at || !passedByPointer(at, dl)) { args.push_back(a); continue; }
            auto* ld = llvm::dyn_cast<llvm::LoadInst>(a);
            if (ld && ld->isSimple() && ld->hasOneUse() && ld->getType() == at) {
                llvm::Value* src = ld->getPointerOperand();
                auto* srcSlot = llvm::dyn_cast<llvm::AllocaInst>(src);
                if (srcSlot && srcSlot->hasMetadata(kFreshTemp)) {
                    dead.push_back(ld);
                    args.push_back(src);
                    continue;
                }
                llvm::AllocaInst* slot = eb.CreateAlloca(at, nullptr, "arg.copy");
                if (freestanding) {
                    llvm::IRBuilder<> lb(ld->getNextNode());
                    lb.CreateStore(ld, slot);
                } else {
                    dead.push_back(ld);
                    llvm::IRBuilder<> lb(ld);
                    lb.CreateMemCpy(slot, slot->getAlign(), src, ld->getAlign(), dl.getTypeAllocSize(at).getFixedValue());
                }
                args.push_back(slot);
                continue;
            }
            llvm::AllocaInst* slot = eb.CreateAlloca(at, nullptr, "arg.copy");
            auto* c = llvm::dyn_cast<llvm::Constant>(a);
            if (c && c->isNullValue() && !freestanding)
                b.CreateMemSet(slot, b.getInt8(0), dl.getTypeAllocSize(at).getFixedValue(), slot->getAlign());
            else
                b.CreateStore(a, slot);
            args.push_back(slot);
        }
        llvm::SmallVector<llvm::OperandBundleDef, 1> bundles;
        cb->getOperandBundlesAsDefs(bundles);
        llvm::CallBase* nc = nullptr;
        if (auto* inv = llvm::dyn_cast<llvm::InvokeInst>(cb))
            nc = b.CreateInvoke(newTy, cb->getCalledOperand(), inv->getNormalDest(), inv->getUnwindDest(), args, bundles);
        else
            nc = b.CreateCall(newTy, cb->getCalledOperand(), args, bundles);
        nc->setCallingConv(cb->getCallingConv());
        llvm::AttributeList cal = cb->getAttributes();
        std::vector<llvm::AttributeSet> cpa;
        for (unsigned i = 0; i < cb->arg_size(); ++i)
            cpa.push_back(i < oldTy->getNumParams() && passedByPointer(oldTy->getParamType(i), dl)
                              ? llvm::AttributeSet() : cal.getParamAttrs(i));
        nc->setAttributes(llvm::AttributeList::get(m.getContext(), cal.getFnAttrs(), cal.getRetAttrs(), cpa));
        if (!cb->getType()->isVoidTy()) nc->takeName(cb);
        cb->replaceAllUsesWith(nc);
        cb->eraseFromParent();
    }
    for (llvm::Instruction* i : dead) i->eraseFromParent();
    for (llvm::Instruction* i : fresh) i->setMetadata(kFreshTemp, nullptr);

    // Then each function: a new one with the pointer parameters takes the old one's body.
    for (llvm::Function* f : fns) {
        llvm::FunctionType* oldTy = f->getFunctionType();
        llvm::FunctionType* newTy = byPointerType(oldTy, dl);
        llvm::Function* nf = llvm::Function::Create(newTy, f->getLinkage(), f->getAddressSpace(), "", &m);
        nf->copyAttributesFrom(f);
        llvm::AttributeList al = f->getAttributes();
        std::vector<llvm::AttributeSet> pa;
        for (unsigned i = 0; i < newTy->getNumParams(); ++i) {
            llvm::Type* pt = oldTy->getParamType(i);
            if (!passedByPointer(pt, dl)) { pa.push_back(al.getParamAttrs(i)); continue; }
            llvm::AttrBuilder ab(m.getContext());
            ab.addAttribute(llvm::Attribute::NoAlias);
            ab.addAttribute(llvm::Attribute::NoUndef);
            ab.addDereferenceableAttr(dl.getTypeAllocSize(pt).getFixedValue());
            ab.addAlignmentAttr(dl.getABITypeAlign(pt));
            pa.push_back(llvm::AttributeSet::get(m.getContext(), ab));
        }
        nf->setAttributes(llvm::AttributeList::get(m.getContext(), al.getFnAttrs(), al.getRetAttrs(), pa));
        nf->takeName(f);
        if (!f->isDeclaration()) {
            nf->splice(nf->begin(), f);
            llvm::BasicBlock& entry = nf->getEntryBlock();
            llvm::IRBuilder<> eb(&entry, entry.getFirstInsertionPt());
            auto na = nf->arg_begin();
            for (llvm::Argument& oa : f->args()) {
                na->setName(oa.getName());
                if (!passedByPointer(oa.getType(), dl)) {
                    oa.replaceAllUsesWith(&*na);
                    ++na;
                    continue;
                }
                // The prologue's `store %arg, %slot`: the copy is the slot.
                auto* st = oa.hasOneUse() ? llvm::dyn_cast<llvm::StoreInst>(oa.user_back()) : nullptr;
                auto* slot = st ? llvm::dyn_cast<llvm::AllocaInst>(st->getPointerOperand()) : nullptr;
                bool first = slot && st->getParent() == &entry && slot->getAllocatedType() == oa.getType();
                if (first)
                    for (llvm::Instruction& i : entry) {
                        if (&i == st) break;
                        for (llvm::Use& u : i.operands())
                            if (u.get() == slot) first = false;
                    }
                if (first) {
                    st->eraseFromParent();
                    slot->replaceAllUsesWith(&*na);
                    slot->eraseFromParent();
                } else {
                    oa.replaceAllUsesWith(eb.CreateLoad(oa.getType(), &*na, oa.getName() + ".val"));
                }
                ++na;
            }
        }
        f->replaceAllUsesWith(nf);
        f->eraseFromParent();
    }
}
