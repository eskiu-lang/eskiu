// codegen_aggcopy.cpp: copies of large records done in memory, not as first-class values.
#include "codegen.h"
#include "llvm/IR/IRBuilder.h"

// Over this many bytes a record or array is copied with memmove/memcpy/memset, as Clang
// does, and never loaded and stored as one value (the same threshold as passing by pointer).
static constexpr uint64_t kInMemoryCopy = 16;

// Whether an instruction between a load and the store of its value may change what the
// load read.
static bool mayWrite(const llvm::Instruction& i) {
    return i.mayWriteToMemory() || i.mayThrow();
}

// A record or array over 16 bytes was loaded and stored as one first-class value: a struct
// literal assigned to a place (`*self = State{}`), a copy (`a = b`, `xs[i] = x`). LLVM's
// optimizer turns such a value into one insertvalue per scalar (thousands for a record with
// arrays of records) and its instruction selection then takes time that grows far faster
// than the record: one assignment of a 30 KB record took a minute to compile at -O2. Each
// such store becomes a memmove from where its value was loaded (when nothing in between
// writes memory), a memset of a zero constant or a memcpy from a constant global, which
// the optimizer and the backend handle in time proportional to the record.
void CodeGen::copyAggregatesInMemory(llvm::Module& m) {
    if (freestanding) return;   // no memcpy/memset to call there
    const llvm::DataLayout& dl = m.getDataLayout();
    auto large = [&](llvm::Type* t) {
        return (t->isStructTy() || t->isArrayTy()) && dl.getTypeAllocSize(t).getFixedValue() > kInMemoryCopy;
    };
    std::vector<llvm::StoreInst*> stores;
    for (llvm::Function& f : m)
        for (llvm::BasicBlock& bb : f)
            for (llvm::Instruction& i : bb)
                if (auto* si = llvm::dyn_cast<llvm::StoreInst>(&i))
                    if (si->isSimple() && large(si->getValueOperand()->getType())) stores.push_back(si);
    unsigned constants = 0;
    for (llvm::StoreInst* si : stores) {
        llvm::Value* v = si->getValueOperand();
        llvm::Type* t = v->getType();
        uint64_t size = dl.getTypeAllocSize(t).getFixedValue();
        llvm::Align align = si->getAlign();
        llvm::IRBuilder<> b(si);
        if (auto* li = llvm::dyn_cast<llvm::LoadInst>(v)) {
            if (!li->isSimple() || li->getParent() != si->getParent()) continue;
            bool clean = true;
            for (auto it = std::next(li->getIterator()); &*it != si; ++it)
                if (mayWrite(*it)) { clean = false; break; }
            if (!clean) continue;
            b.CreateMemMove(si->getPointerOperand(), align, li->getPointerOperand(), li->getAlign(), size);
            si->eraseFromParent();
            if (li->use_empty()) li->eraseFromParent();
        } else if (auto* c = llvm::dyn_cast<llvm::Constant>(v)) {
            if (c->isNullValue()) {
                b.CreateMemSet(si->getPointerOperand(), b.getInt8(0), size, align);
            } else {
                auto* gv = new llvm::GlobalVariable(m, t, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage, c,
                                                    "agg.const." + std::to_string(constants++));
                gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
                gv->setAlignment(dl.getABITypeAlign(t));
                b.CreateMemCpy(si->getPointerOperand(), align, gv, dl.getABITypeAlign(t), size);
            }
            si->eraseFromParent();
        }
    }
}
