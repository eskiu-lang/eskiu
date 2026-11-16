// codegen_eh.h: a static helper shared by codegen_closure.cpp and codegen_try.cpp (internal).
#pragma once
#include "codegen.h"

// Ensure personality function and _ZTIPv type_info are declared in the module.
static void ensureEHDecls(llvm::Module* mod, llvm::LLVMContext& ctx,
                          const std::string& personalityName) {
    if (mod->getFunction(personalityName)) return;
    llvm::Type* i32  = llvm::Type::getInt32Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
    // Personality function
    llvm::FunctionType* persType = llvm::FunctionType::get(i32, true);
    llvm::Function::Create(persType, llvm::Function::ExternalLinkage,
        personalityName, mod);
    // _ZTIPv — void* type_info (from libc++)
    if (!mod->getNamedGlobal("_ZTIPv"))
        new llvm::GlobalVariable(*mod, ptrTy, true,
            llvm::GlobalValue::ExternalLinkage, nullptr, "_ZTIPv");
}
