// codegen_safe.cpp: the --safe run-time checks (bounds, overflow, null) and the shared
// failure helper that reports a failed check (split from codegen_expr.cpp).
#include "codegen.h"

// A private constant C string, one per distinct text in the module (the messages and
// file names of the --safe checks repeat at every check).
llvm::Constant* CodeGen::safeString(const std::string& text) {
    auto it = safeStrings.find(text);
    if (it != safeStrings.end() && it->second->getParent() == module.get()) return it->second;
    llvm::Constant* init = llvm::ConstantDataArray::getString(*context, text);
    auto* gv = new llvm::GlobalVariable(*module, init->getType(), true, llvm::GlobalValue::PrivateLinkage,
                                        init, "__eskiu_safe_str");
    gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    gv->setAlignment(llvm::Align(1));
    safeStrings[text] = gv;
    return gv;
}

// `void __eskiu_safe_fail(ptr fmt, ptr file, i32 line, i32 col, i64 a, i64 b, i64 c)`,
// emitted once per module: flushes stdout, prints `fmt` (which starts with "%s:%d:%d: ")
// with the location and the values to stderr, and aborts. A check's failure block calls
// it, so the success path stays a compare and a branch.
llvm::Function* CodeGen::safeFailFn() {
    if (llvm::Function* f = module->getFunction("__eskiu_safe_fail")) return f;
    llvm::LLVMContext& c = *context;
    llvm::Type* ptrTy = llvm::PointerType::get(c, 0);
    llvm::Type* i32 = llvm::Type::getInt32Ty(c);
    llvm::Type* i64 = llvm::Type::getInt64Ty(c);
    llvm::Type* voidTy = llvm::Type::getVoidTy(c);
    auto* fty = llvm::FunctionType::get(voidTy, {ptrTy, ptrTy, i32, i32, i64, i64, i64}, false);
    auto* f = llvm::Function::Create(fty, llvm::Function::InternalLinkage, "__eskiu_safe_fail", module.get());
    f->addFnAttr(llvm::Attribute::Cold);
    f->addFnAttr(llvm::Attribute::NoReturn);
    f->addFnAttr(llvm::Attribute::NoInline);
    f->addFnAttr(llvm::Attribute::NoUnwind);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(c, "entry", f));
    llvm::FunctionCallee fflushFn = module->getOrInsertFunction("fflush", i32, ptrTy);
    b.CreateCall(fflushFn, {llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy))});
    const llvm::Triple& tt = module->getTargetTriple();
    llvm::Value* err;
    if (tt.isOSWindows()) {
        llvm::FunctionCallee iob = module->getOrInsertFunction("__acrt_iob_func", ptrTy, i32);
        err = b.CreateCall(iob, {b.getInt32(2)});
    } else {
        const char* name = tt.isOSDarwin() ? "__stderrp" : "stderr";
        llvm::GlobalVariable* gv = module->getNamedGlobal(name);
        if (!gv) gv = new llvm::GlobalVariable(*module, ptrTy, false, llvm::GlobalValue::ExternalLinkage, nullptr, name);
        err = b.CreateLoad(ptrTy, gv);
    }
    llvm::FunctionCallee fprintfFn = module->getOrInsertFunction("fprintf", llvm::FunctionType::get(i32, {ptrTy, ptrTy}, true));
    std::vector<llvm::Value*> args{err};
    for (llvm::Argument& a : f->args()) args.push_back(&a);
    b.CreateCall(fprintfFn, args);
    b.CreateCall(module->getOrInsertFunction("abort", voidTy));
    b.CreateUnreachable();
    return f;
}

// When `cond` holds (a failed --safe check), report it and abort: `msg` is the printf
// format of the message after the location, reading up to three i64 values `a`, `b`, `c`.
// A freestanding build has no C library to print with, so it traps (@llvm.trap) instead.
void CodeGen::emitSafeTrap(llvm::Value* cond, const ASTNode* at, const char* name, const std::string& msg,
                           llvm::Value* a, llvm::Value* b, llvm::Value* c) {
    llvm::Function* fn = builder->GetInsertBlock()->getParent();
    auto* trapBB = llvm::BasicBlock::Create(*context, std::string(name) + ".fail", fn);
    auto* contBB = llvm::BasicBlock::Create(*context, std::string(name) + ".ok", fn);
    builder->CreateCondBr(cond, trapBB, contBB);
    builder->SetInsertPoint(trapBB);
    if (freestanding) {
        builder->CreateCall(llvm::Intrinsic::getOrInsertDeclaration(module.get(), llvm::Intrinsic::trap));
    } else {
        llvm::Type* i64 = llvm::Type::getInt64Ty(*context);
        llvm::Value* zero = llvm::ConstantInt::get(i64, 0);
        int line = at ? at->line : 0, col = at ? at->col : 0;
        auto* call = builder->CreateCall(safeFailFn(), {
            safeString("%s:%d:%d: " + msg + "\n"), safeString(diagFile.empty() ? "<input>" : diagFile),
            builder->getInt32(line), builder->getInt32(col), a ? a : zero, b ? b : zero, c ? c : zero});
        call->setDoesNotReturn();
    }
    builder->CreateUnreachable();
    builder->SetInsertPoint(contBB);
}

// Under --safe, trap when the pointer about to be dereferenced (at `at`) is null.
llvm::Value* CodeGen::safeNonNull(llvm::Value* p, const ASTNode* at) {
    if (!(safeChecks & SafeNull) || !p->getType()->isPointerTy() || llvm::isa<llvm::AllocaInst>(p) || llvm::isa<llvm::GlobalValue>(p)) return p;
    emitSafeTrap(builder->CreateIsNull(p), at, "null", "null pointer dereference");
    return p;
}

// A signed `+ - *`: wrapping by default; under --safe an overflow traps (as Zig's
// ReleaseSafe and -fsanitize=signed-integer-overflow -fsanitize-trap). `id` is the
// matching llvm.s*.with.overflow intrinsic, `at` the operator's expression.
llvm::Value* CodeGen::safeArith(llvm::Intrinsic::ID id, llvm::Value* l, llvm::Value* r, bool uns, const ASTNode* at) {
    if (!(safeChecks & SafeOverflow) || uns || !l->getType()->isIntegerTy() || l->getType()->getIntegerBitWidth() < 32) {
        if (id == llvm::Intrinsic::sadd_with_overflow) return builder->CreateAdd(l, r);
        if (id == llvm::Intrinsic::ssub_with_overflow) return builder->CreateSub(l, r);
        return builder->CreateMul(l, r);
    }
    llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(module.get(), id, {l->getType()});
    llvm::Value* pr = builder->CreateCall(f, {l, r});
    const char* op = id == llvm::Intrinsic::sadd_with_overflow ? "+" : id == llvm::Intrinsic::ssub_with_overflow ? "-" : "*";
    emitSafeTrap(builder->CreateExtractValue(pr, {1}), at, "overflow", std::string("integer overflow in '") + op + "'");
    return builder->CreateExtractValue(pr, {0});
}

// Trap when `idx` (sext to i64) is < 0 or >= `len` (the index expression `at`).
void CodeGen::emitBoundsCheck(llvm::Value* idx, llvm::Value* len, const ASTNode* at) {
    llvm::Type* i64 = llvm::Type::getInt64Ty(*context);
    llvm::Value* idx64 = idx->getType()->isIntegerTy(64) ? idx : builder->CreateSExt(idx, i64);
    llvm::Value* lo  = builder->CreateICmpSLT(idx64, llvm::ConstantInt::get(i64, 0));
    llvm::Value* hi  = builder->CreateICmpSGE(idx64, len);
    llvm::Value* oob = builder->CreateOr(lo, hi);
    emitSafeTrap(oob, at, "bounds", "index %lld out of bounds for length %lld", idx64, len);
}

void CodeGen::emitSliceBoundsCheck(llvm::Value* lo, llvm::Value* hi, llvm::Value* len, const ASTNode* at) {
    llvm::Type* i64 = llvm::Type::getInt64Ty(*context);
    // 0 <= lo <= hi (<= len). `lo == len` is a valid empty slice at the end, so the upper
    // bound is `hi > len`, not `hi >= len` — this is what the element check got wrong.
    llvm::Value* bad = builder->CreateOr(
        builder->CreateICmpSLT(lo, llvm::ConstantInt::get(i64, 0)),
        builder->CreateICmpSLT(hi, lo));
    if (len) bad = builder->CreateOr(bad, builder->CreateICmpSGT(hi, len));
    if (len)
        emitSafeTrap(bad, at, "slice", "slice [%lld..%lld] out of bounds for length %lld", lo, hi, len);
    else
        emitSafeTrap(bad, at, "slice", "slice [%lld..%lld] out of bounds", lo, hi);
}
