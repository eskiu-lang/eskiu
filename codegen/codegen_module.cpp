#include "llvm/Support/CommandLine.h"
#include "codegen.h"
#include "../sema/type_checker.h"
#include "../ast/type_qual.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/Operator.h"
#include <functional>
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Transforms/Instrumentation/AddressSanitizer.h"
#include "llvm/Transforms/Instrumentation/BoundsChecking.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Support/raw_os_ostream.h"
#include <iostream>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

// Template type-name utilities (mangleTemplate / splitTemplateType / substType)
// are shared with the type checker; see template_utils.h.
#include "../template_utils.h"

// ============================================================================

// Map the --reloc string to an LLVM relocation model. 3DS .3dsx targets need
// "static": the loader applies static relocations and has no dynamic loader to
// populate a GOT, so PIC (GOT/PLT) code reads garbage and faults.
static std::optional<llvm::Reloc::Model> parseRelocModel(const std::string& s) {
    if (s == "static")         return llvm::Reloc::Static;
    if (s == "dynamic-no-pic") return llvm::Reloc::DynamicNoPIC;
    return llvm::Reloc::PIC_;   // default (empty or "pic")
}

// Register the code-emission backends eskiuc supports: AArch64 and 32-bit ARM
// always, X86 only when the build links it (dynamic LLVM, or a non-Apple static
// build; the static Apple release ships without X86 libs). `withAsm` also pulls in
// the assembly printer/parser, which the object-emitting paths need.
// The host triple. On macOS LLVM spells it with the Darwin kernel version, and its mapping
// from that to a macOS version can be off by one on a new release, so the linker would warn
// that every object targets a newer macOS. Use the product version, as clang does.
static std::string nativeTriple() {
    std::string t = llvm::sys::getDefaultTargetTriple();
#ifdef __APPLE__
    llvm::Triple triple(t);
    if (triple.isMacOSX()) {
        char buf[64] = {0};
        size_t len = sizeof(buf) - 1;
        if (sysctlbyname("kern.osproductversion", buf, &len, nullptr, 0) == 0 && buf[0]) {
            std::string ver = buf;
            size_t dot = ver.find('.');
            if (dot != std::string::npos) dot = ver.find('.', dot + 1);
            if (dot != std::string::npos) ver.resize(dot);
            triple.setOSName("macosx" + ver);
            t = triple.str();
        }
    }
#endif
    return t;
}

static void initCodegenTargets(bool withAsm) {
    LLVMInitializeAArch64Target();
    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeARMTarget();
    LLVMInitializeARMTargetInfo();
    LLVMInitializeARMTargetMC();
    LLVMInitializeWebAssemblyTarget();
    LLVMInitializeWebAssemblyTargetInfo();
    LLVMInitializeWebAssemblyTargetMC();
#ifdef ESKIU_HAS_X86
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86TargetMC();
#endif
    if (withAsm) {
        LLVMInitializeAArch64AsmPrinter();
        LLVMInitializeAArch64AsmParser();
        LLVMInitializeARMAsmPrinter();
        LLVMInitializeARMAsmParser();
        LLVMInitializeWebAssemblyAsmPrinter();
        LLVMInitializeWebAssemblyAsmParser();
#ifdef ESKIU_HAS_X86
        LLVMInitializeX86AsmPrinter();
        LLVMInitializeX86AsmParser();
#endif
    }
}

// emscripten's JavaScript-based C++ exceptions (clang's -fexceptions under emcc): the
// WebAssembly backend lowers each invoke and landingpad to calls into its runtime. The
// option is set as if given on the command line (it keeps its value elsewhere).
static void enableEmscriptenEH() {
    static bool done = false;
    if (done) return;
    done = true;
    auto& opts = llvm::cl::getRegisteredOptions();
    auto it = opts.find("enable-emscripten-cxx-exceptions");
    if (it != opts.end() && it->second->getNumOccurrences() == 0)
        it->second->addOccurrence(0, "enable-emscripten-cxx-exceptions", "true");
}

// Build a TargetMachine for `triple`, applying the CPU, feature, relocation, and
// float-ABI selection shared by every code-emitting path (returns nullptr when the
// triple has no registered target). Centralizing this keeps the three call sites
// from drifting: an earlier version set the hard-float ABI in module-gen and the
// optimizer but not the object emitter, so the emitted object dropped the
// Tag_ABI_VFP_args attribute that hard-float libraries like libctru require.
static std::unique_ptr<llvm::TargetMachine> makeTargetMachine(
        const CodeGen& cg, const llvm::Triple& triple, const std::string& tripleStr) {
    if (triple.isOSEmscripten()) enableEmscriptenEH();
    std::string err;
#if LLVM_VERSION_MAJOR >= 22
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, err);
#else
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(tripleStr, err);
#endif
    if (!target) return nullptr;
    // Without --mcpu the CPU is the target's baseline, native builds included, as in
    // clang: "apple-m1" for arm64 Apple targets (every Apple arm64 CPU has its features,
    // LSE atomics included), "generic" elsewhere. The host CPU's name alone (without its
    // feature list) could select instructions a VM or a masked host lacks (SIGILL).
    llvm::StringRef cpu = !cg.targetCPU.empty() ? llvm::StringRef(cg.targetCPU)
        : (triple.isAArch64() && triple.isOSDarwin() ? llvm::StringRef("apple-m1")
                                                     : llvm::StringRef("generic"));
    llvm::TargetOptions opt;
    // Constructors go in .init_array, as clang emits them: Android's bionic runs no
    // .ctors (the uncaught-exception reporter is one).
    opt.UseInitArray = true;
    // Hard-float ABI for hard-float ARM triples (those ending in "hf", e.g. the
    // 3DS's armv6k-none-eabihf). LLVM parses "eabihf" into the OS field, not the
    // environment, so match the triple string directly. This makes LLVM emit the
    // Tag_ABI_VFP_args "VFP registers" build attribute, required to link against
    // hard-float libraries like libctru.
    if (tripleStr.size() >= 2 && tripleStr.compare(tripleStr.size() - 2, 2, "hf") == 0)
        opt.FloatABIType = llvm::FloatABI::Hard;
    // The backend's optimization level follows -O, as in clang: at -O0 the machine
    // passes that scale with loop nesting depth (loop-invariant code motion, block
    // frequencies) do not run.
    llvm::CodeGenOptLevel level = cg.optLevel == 0 ? llvm::CodeGenOptLevel::None
                                : cg.optLevel == 1 ? llvm::CodeGenOptLevel::Less
                                : cg.optLevel == 2 ? llvm::CodeGenOptLevel::Default
                                                   : llvm::CodeGenOptLevel::Aggressive;
    return std::unique_ptr<llvm::TargetMachine>(
        target->createTargetMachine(triple, cpu, cg.targetFeatures, opt,
                                    parseRelocModel(cg.relocModel), std::nullopt, level));
}

bool unnamedBitfieldsAlign(const llvm::Triple& t) {
    if (t.isOSWindows() || t.isOSDarwin()) return false;
    return t.isARM() || t.isThumb() || t.isAArch64();
}

TargetLayoutInfo targetLayoutInfo(const std::string& tripleIn) {
    initCodegenTargets(/*withAsm=*/false);
    std::string tripleStr = tripleIn.empty() ? nativeTriple() : tripleIn;
    llvm::Triple triple(tripleStr);
    CodeGen probe;
    probe.targetTriple = tripleIn;
    llvm::DataLayout dl("");
    if (auto tm = makeTargetMachine(probe, triple, tripleStr)) dl = tm->createDataLayout();
    llvm::LLVMContext ctx;
    auto al = [&](llvm::Type* t) { return (unsigned)dl.getABITypeAlign(t).value(); };
    TargetLayoutInfo info;
    info.ptrSize  = (unsigned)dl.getPointerSize();
    info.ptrAlign = al(llvm::PointerType::get(ctx, 0));
    info.i16Align = al(llvm::Type::getInt16Ty(ctx));
    info.i32Align = al(llvm::Type::getInt32Ty(ctx));
    info.i64Align = al(llvm::Type::getInt64Ty(ctx));
    info.f32Align = al(llvm::Type::getFloatTy(ctx));
    info.f64Align = al(llvm::Type::getDoubleTy(ctx));
    info.msBitfields = triple.isOSWindows();
    info.msvcEnv = triple.isWindowsMSVCEnvironment();
    info.unnamedBitfieldsAlign = unnamedBitfieldsAlign(triple);
    return info;
}

// The scalars an aggregate holds, each array element counted.
static uint64_t aggregateLeaves(llvm::Type* t) {
    if (auto* st = llvm::dyn_cast<llvm::StructType>(t)) {
        uint64_t n = 0;
        for (llvm::Type* e : st->elements()) n += aggregateLeaves(e);
        return n;
    }
    if (auto* at = llvm::dyn_cast<llvm::ArrayType>(t)) return at->getNumElements() * aggregateLeaves(at->getElementType());
    return 1;
}

static bool largeAggregate(llvm::Type* t) {
    return (t->isStructTy() || t->isArrayTy()) && aggregateLeaves(t) > 64;
}

// The function type with each large aggregate parameter as a pointer, or null when it has none.
static llvm::FunctionType* byPointerType(llvm::FunctionType* ft) {
    bool any = false;
    std::vector<llvm::Type*> ps;
    for (llvm::Type* p : ft->params()) {
        if (largeAggregate(p)) { ps.push_back(llvm::PointerType::get(ft->getContext(), 0)); any = true; }
        else ps.push_back(p);
    }
    return any ? llvm::FunctionType::get(ft->getReturnType(), ps, ft->isVarArg()) : nullptr;
}

// A field of a packed struct (packed, #pragma pack, or a struct inside one) may sit at an
// address its type's alignment does not divide, but the IRBuilder gives each load and store
// the type's ABI alignment, which LLVM is entitled to rely on (wider or paired accesses; a
// trap on 32-bit ARM). Each access through a GEP chain that crosses a packed struct gets
// the alignment the chain proves instead: the base's (its own chain's, else the C alignment
// of the GEP's source type), lowered by each field offset and element stride. Chains with
// no packed struct are left alone. selfhost cg_align_packed applies the same rule to its IR.
void CodeGen::alignPackedAccesses(llvm::Module& m) {
    const llvm::DataLayout& dl = m.getDataLayout();
    auto lower = [](uint64_t a, uint64_t off) {
        return off == 0 ? a : std::min(a, off & (~off + 1));
    };
    for (llvm::Function& f : m) {
        std::map<llvm::Value*, uint64_t> known;
        std::function<bool(llvm::Value*, uint64_t&)> chainAlign = [&](llvm::Value* v, uint64_t& out) -> bool {
            auto* gep = llvm::dyn_cast<llvm::GEPOperator>(v);
            if (!gep) return false;
            auto hit = known.find(v);
            if (hit != known.end()) {
                out = hit->second;
                return true;
            }
            uint64_t a = 0;
            bool packed = chainAlign(gep->getPointerOperand(), a);
            if (!packed) a = cAlignOf(gep->getSourceElementType());
            for (auto it = llvm::gep_type_begin(gep), e = llvm::gep_type_end(gep); it != e; ++it) {
                auto* ci = llvm::dyn_cast<llvm::ConstantInt>(it.getOperand());
                if (llvm::StructType* st = it.getStructTypeOrNull()) {
                    if (st->isPacked()) packed = true;
                    a = lower(a, dl.getStructLayout(st)->getElementOffset((unsigned)ci->getZExtValue()));
                } else {
                    uint64_t stride = it.getSequentialElementStride(dl).getFixedValue();
                    if (!ci) a = lower(a, stride);
                    else a = lower(a, stride * (uint64_t)ci->getSExtValue());
                }
            }
            if (!packed) return false;
            known[v] = a;
            out = a;
            return true;
        };
        for (llvm::BasicBlock& bb : f)
            for (llvm::Instruction& inst : bb) {
                uint64_t a = 0;
                if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
                    if (chainAlign(ld->getPointerOperand(), a) && a < ld->getAlign().value())
                        ld->setAlignment(llvm::Align(a));
                } else if (auto* stI = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
                    if (chainAlign(stI->getPointerOperand(), a) && a < stI->getAlign().value())
                        stI->setAlignment(llvm::Align(a));
                }
            }
    }
}

// WebAssembly gives each scalar of an aggregate argument a parameter of its own, and an
// engine accepts at most 1000 (V8), so a function taking a large struct by value (an
// `Ok<T>(v)` of a big T) does not load. Such a parameter becomes a pointer to a copy the
// caller makes, which the callee reads at entry: the same by-value semantics, for every
// Eskiu function and every call, direct or through a function value.
void CodeGen::passLargeAggregatesByPointer(llvm::Module& m) {
    std::vector<llvm::Function*> fns;
    for (llvm::Function& f : m)
        if (byPointerType(f.getFunctionType())) fns.push_back(&f);
    // Calls first: rebuild each one whose type has a large aggregate parameter.
    std::vector<llvm::CallBase*> calls;
    for (llvm::Function& f : m)
        for (llvm::BasicBlock& bb : f)
            for (llvm::Instruction& i : bb)
                if (auto* cb = llvm::dyn_cast<llvm::CallBase>(&i))
                    if (byPointerType(cb->getFunctionType())) calls.push_back(cb);
    for (llvm::CallBase* cb : calls) {
        llvm::FunctionType* oldTy = cb->getFunctionType();
        llvm::FunctionType* newTy = byPointerType(oldTy);
        llvm::Function* caller = cb->getFunction();
        llvm::IRBuilder<> eb(&caller->getEntryBlock(), caller->getEntryBlock().getFirstInsertionPt());
        llvm::IRBuilder<> b(cb);
        std::vector<llvm::Value*> args;
        for (unsigned i = 0; i < cb->arg_size(); ++i) {
            llvm::Value* a = cb->getArgOperand(i);
            if (i < oldTy->getNumParams() && largeAggregate(oldTy->getParamType(i))) {
                llvm::AllocaInst* slot = eb.CreateAlloca(a->getType(), nullptr, "arg.copy");
                b.CreateStore(a, slot);
                a = slot;
            }
            args.push_back(a);
        }
        llvm::SmallVector<llvm::OperandBundleDef, 1> bundles;
        cb->getOperandBundlesAsDefs(bundles);
        llvm::CallBase* nc = nullptr;
        if (auto* inv = llvm::dyn_cast<llvm::InvokeInst>(cb))
            nc = b.CreateInvoke(newTy, cb->getCalledOperand(), inv->getNormalDest(), inv->getUnwindDest(), args, bundles);
        else
            nc = b.CreateCall(newTy, cb->getCalledOperand(), args, bundles);
        nc->setCallingConv(cb->getCallingConv());
        if (!cb->getType()->isVoidTy()) nc->takeName(cb);
        cb->replaceAllUsesWith(nc);
        cb->eraseFromParent();
    }
    // Then each function: a new one with the pointer parameters takes the old one's body.
    for (llvm::Function* f : fns) {
        llvm::FunctionType* newTy = byPointerType(f->getFunctionType());
        llvm::Function* nf = llvm::Function::Create(newTy, f->getLinkage(), f->getAddressSpace(), "", &m);
        nf->copyAttributesFrom(f);
        // The function's and the result's attributes stay; a pointer parameter has none.
        llvm::AttributeList al = f->getAttributes();
        std::vector<llvm::AttributeSet> pa;
        for (unsigned i = 0; i < newTy->getNumParams(); ++i)
            pa.push_back(largeAggregate(f->getFunctionType()->getParamType(i)) ? llvm::AttributeSet()
                                                                               : al.getParamAttrs(i));
        nf->setAttributes(llvm::AttributeList::get(m.getContext(), al.getFnAttrs(), al.getRetAttrs(), pa));
        nf->takeName(f);
        if (!f->isDeclaration()) {
            nf->splice(nf->begin(), f);
            llvm::IRBuilder<> eb(&nf->getEntryBlock(), nf->getEntryBlock().getFirstInsertionPt());
            auto na = nf->arg_begin();
            for (llvm::Argument& oa : f->args()) {
                llvm::Value* v = &*na;
                if (largeAggregate(oa.getType())) v = eb.CreateLoad(oa.getType(), &*na, oa.getName() + ".val");
                na->setName(oa.getName());
                oa.replaceAllUsesWith(v);
                ++na;
            }
        }
        f->replaceAllUsesWith(nf);
        f->eraseFromParent();
    }
}

CodeGen::CodeGen()
    : context(std::make_unique<llvm::LLVMContext>()),
      module(std::make_unique<llvm::Module>("eskiu", *context)),
      builder(std::make_unique<llvm::IRBuilder<>>(*context)) {}

CodeGen::~CodeGen() = default;

llvm::Module* CodeGen::generateCode(std::shared_ptr<Program> program) {
    // Set target triple + data layout early so sizeof queries work in alloc()
    initCodegenTargets(/*withAsm=*/true);
    std::string tripleStr = targetTriple.empty()
        ? nativeTriple() : targetTriple;
    llvm::Triple triple(tripleStr);
    module->setTargetTriple(triple);
    if (auto tm = makeTargetMachine(*this, triple, tripleStr))
        module->setDataLayout(tm->createDataLayout());
    if (fpContract) {
        llvm::FastMathFlags fmf;
        fmf.setAllowContract(true);
        builder->setFastMathFlags(fmf);
    }
    if (semaInstanceArgs)
        for (const auto& kv : *semaInstanceArgs) templateInstanceArgs.emplace(kv.first, kv.second);

    program->accept(this);
    emitUncaughtReporter();
    alignPackedAccesses(*module);
    if (triple.isWasm()) passLargeAggregatesByPointer(*module);

    std::string errorStr;
    llvm::raw_string_ostream errorStream(errorStr);
    if (llvm::verifyModule(*module, &errorStream)) {
        std::cerr << "LLVM verification failed:\n" << errorStr << std::endl;
        return nullptr;
    }

    return module.get();
}

void CodeGen::printIR() const {
    if (module) {
        std::cerr << "Module has " << module->getFunctionList().size() << " functions\n";
        llvm::raw_os_ostream out(std::cout);
        module->print(out, nullptr);
        out.flush();
    } else {
        std::cerr << "Module is null!\n";
    }
}

void CodeGen::optimizeModule() {
    if (optLevel == 0) return;

    initCodegenTargets(/*withAsm=*/false);

    // Target-aware pipeline: give the PassBuilder a TargetMachine + DataLayout so
    // target heuristics (and the data layout the optimizer needs for, e.g., SROA)
    // are correct. Mirrors emitObjectFile's target setup.
    std::string tripleStr = targetTriple.empty()
        ? nativeTriple() : targetTriple;
    llvm::Triple triple(tripleStr);
    module->setTargetTriple(triple);

    std::unique_ptr<llvm::TargetMachine> tm = makeTargetMachine(*this, triple, tripleStr);
    if (tm) module->setDataLayout(tm->createDataLayout());

    llvm::OptimizationLevel lvl;
    switch (optLevel) {
        case 1:  lvl = llvm::OptimizationLevel::O1; break;
        case 2:  lvl = llvm::OptimizationLevel::O2; break;
        default: lvl = llvm::OptimizationLevel::O3; break;   // 3 and above
    }

    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;
    // Freestanding: there is no libc, so no library function may be assumed (clang
    // -ffreestanding). Registering the TargetLibraryInfo first makes it the one the
    // pipeline uses; "no-builtins" also covers the backend.
    llvm::TargetLibraryInfoImpl tlii(triple);
    if (freestanding) {
        tlii.disableAllFunctions();
        FAM.registerPass([&] { return llvm::TargetLibraryAnalysis(tlii); });
        for (llvm::Function& F : *module)
            if (!F.isDeclaration()) F.addFnAttr("no-builtins");
    }
    // PipelineTuningOptions leaves both vectorizers off; clang enables them from
    // -O2 (and -Os/-Oz), so do the same.
    llvm::PipelineTuningOptions pto;
    pto.LoopVectorization = optLevel >= 2;
    pto.SLPVectorization = optLevel >= 2;
    pto.LoopInterleaving = true;
    llvm::PassBuilder PB(tm.get(), pto);
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(lvl);
    MPM.run(*module, MAM);
}

bool CodeGen::emitObjectFile(const std::string& filename) {
    initCodegenTargets(/*withAsm=*/true);

    std::string tripleStr = targetTriple.empty()
        ? nativeTriple() : targetTriple;
    llvm::Triple triple(tripleStr);
    module->setTargetTriple(triple);

    std::unique_ptr<llvm::TargetMachine> tm = makeTargetMachine(*this, triple, tripleStr);
    if (!tm) {
        std::cerr << "error: no registered target for triple '" << tripleStr << "'" << std::endl;
        return false;
    }
    module->setDataLayout(tm->createDataLayout());

    // Sanitizer instrumentation (new pass manager), run over the whole module
    // before code generation. --asan instruments memory accesses (the asan
    // runtime is linked separately); --ubsan inserts trapping bounds checks.
    if (asan || ubsan) {
        llvm::LoopAnalysisManager LAM;
        llvm::FunctionAnalysisManager FAM;
        llvm::CGSCCAnalysisManager CGAM;
        llvm::ModuleAnalysisManager MAM;
        llvm::PassBuilder PB(tm.get());
        PB.registerModuleAnalyses(MAM);
        PB.registerCGSCCAnalyses(CGAM);
        PB.registerFunctionAnalyses(FAM);
        PB.registerLoopAnalyses(LAM);
        PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

        // AddressSanitizer only instruments functions marked sanitize_address
        // (Clang stamps this per function); add it to every defined function.
        if (asan) {
            for (llvm::Function& F : *module)
                if (!F.isDeclaration()) F.addFnAttr(llvm::Attribute::SanitizeAddress);
        }

        llvm::ModulePassManager MPM;
        if (ubsan) {
            llvm::BoundsCheckingPass::Options opts;   // empty Runtime => trap on OOB
            llvm::FunctionPassManager FPM;
            FPM.addPass(llvm::BoundsCheckingPass(opts));
            MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(FPM)));
        }
        if (asan) {
            MPM.addPass(llvm::AddressSanitizerPass(llvm::AddressSanitizerOptions{}));
        }
        MPM.run(*module, MAM);
    }

    std::error_code ec;
    llvm::raw_fd_ostream dest(filename, ec, llvm::sys::fs::OF_None);
    if (ec) {
        std::cerr << "error: cannot open '" << filename << "': " << ec.message() << std::endl;
        return false;
    }

    llvm::legacy::PassManager pm;
    if (tm->addPassesToEmitFile(pm, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        std::cerr << "error: target cannot emit object file" << std::endl;
        return false;
    }

    pm.run(*module);
    dest.flush();
    return true;
}
