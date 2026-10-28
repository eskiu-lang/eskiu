#include "eir_runtime.h"

#include <set>

namespace eir {

namespace {

// The C functions the runtime calls: name, the standard prototype, and the function
// pointer type a call casts to. `pthread_t` is spelled `uintptr_t` (a pointer-sized
// integer or pointer on every POSIX target the backends emit for).
struct LibcFn {
    const char* name;
    const char* proto;
    const char* cast;
    bool windows;
};

const LibcFn kLibcFns[] = {
    {"calloc", "void* calloc(size_t, size_t)", "void* (*)(size_t, size_t)", false},
    {"malloc", "void* malloc(size_t)", "void* (*)(size_t)", false},
    {"free", "void free(void*)", "void (*)(void*)", false},
    {"abort", "void abort(void)", "void (*)(void)", false},
    {"strcmp", "int strcmp(const char*, const char*)", "int (*)(const char*, const char*)", false},
    {"snprintf", "int snprintf(char*, size_t, const char*, ...)", "int (*)(char*, size_t, const char*, ...)", false},
    {"fflush", "int fflush(void*)", "int (*)(void*)", false},
    {"_write", "int _write(int, const void*, unsigned int)", "int (*)(int, const void*, unsigned int)", true},
    {"write", "intptr_t write(int, const void*, size_t)", "intptr_t (*)(int, const void*, size_t)", false},
    {"pthread_create", "int pthread_create(uintptr_t*, const void*, void* (*)(void*), void*)",
     "int (*)(uintptr_t*, const void*, void* (*)(void*), void*)", false},
    {"pthread_join", "int pthread_join(uintptr_t, void**)", "int (*)(uintptr_t, void**)", false},
    {"CreateThread", "void* CreateThread(void*, size_t, unsigned long (*)(void*), void*, unsigned long, unsigned long*)",
     "void* (*)(void*, size_t, unsigned long (*)(void*), void*, unsigned long, unsigned long*)", true},
    {"WaitForSingleObject", "unsigned long WaitForSingleObject(void*, unsigned long)",
     "unsigned long (*)(void*, unsigned long)", true},
    {"CloseHandle", "int CloseHandle(void*)", "int (*)(void*)", true},
};

const char* kRuntimeBody = R"RT(
#if defined(_MSC_VER)
#define ESK_TLS __declspec(thread)
#else
#define ESK_TLS __thread
#endif
#define ESK_MAX_CAUGHT 256

static ESK_TLS void* __eir_pending;
static ESK_TLS void* __eir_caught[ESK_MAX_CAUGHT];
static ESK_TLS int __eir_ncaught;
static ESK_TLS char __eir_text[512];

void* __eir_alloc_exception(int64_t size, const char* type) {
    char* ex = (char*)ESK_RT_calloc(1, (size_t)size);
    if (!ex) ESK_RT_abort();
    *(int64_t*)ex = size;
    *(const char**)(ex + 8) = type;
    return ex;
}
void __eir_throw(void* ex) { __eir_pending = ex; }
void __eir_free_exception(void* ex) { ESK_RT_free(ex); }
void* __eir_exc_pending(void) { return __eir_pending; }
void* __eir_take_exception(void) { void* ex = __eir_pending; __eir_pending = 0; return ex; }
void __eir_clear_exception(void) { __eir_pending = 0; }
void __eir_push_caught(void* ex) {
    if (__eir_ncaught < ESK_MAX_CAUGHT) __eir_caught[__eir_ncaught] = ex;
    __eir_ncaught++;
}
void __eir_pop_caught(void) { if (__eir_ncaught > 0) __eir_ncaught--; }
int32_t __eir_caught_depth(void) { return __eir_ncaught; }
void __eir_set_caught_depth(int32_t depth) { if (depth < __eir_ncaught) __eir_ncaught = depth; }

static int __eir_is(const char* a, const char* b) { return ESK_RT_strcmp(a, b) == 0; }

static int __eir_format(void* ex, const char* head, char* buf, size_t n) {
    const char* tn = ex ? *(const char**)((char*)ex + 8) : 0;
    const char* pay = ex ? (const char*)ex + 16 : 0;
    char fmt[64];
    if (!tn) tn = "unknown";
    ESK_RT_snprintf(fmt, sizeof fmt, "%s%%s", head);
    if (__eir_is(tn, "int") || __eir_is(tn, "int32")) { ESK_RT_snprintf(fmt, sizeof fmt, "%s%%s %%lld", head); return ESK_RT_snprintf(buf, n, fmt, tn, (long long)*(const int32_t*)pay); }
    if (__eir_is(tn, "int8")) { ESK_RT_snprintf(fmt, sizeof fmt, "%s%%s %%lld", head); return ESK_RT_snprintf(buf, n, fmt, tn, (long long)*(const int8_t*)pay); }
    if (__eir_is(tn, "int16")) { ESK_RT_snprintf(fmt, sizeof fmt, "%s%%s %%lld", head); return ESK_RT_snprintf(buf, n, fmt, tn, (long long)*(const int16_t*)pay); }
    if (__eir_is(tn, "int64")) { ESK_RT_snprintf(fmt, sizeof fmt, "%s%%s %%lld", head); return ESK_RT_snprintf(buf, n, fmt, tn, (long long)*(const int64_t*)pay); }
    if (__eir_is(tn, "uint") || __eir_is(tn, "uint32")) { ESK_RT_snprintf(fmt, sizeof fmt, "%s%%s %%llu", head); return ESK_RT_snprintf(buf, n, fmt, tn, (unsigned long long)*(const uint32_t*)pay); }
    if (__eir_is(tn, "uint8")) { ESK_RT_snprintf(fmt, sizeof fmt, "%s%%s %%llu", head); return ESK_RT_snprintf(buf, n, fmt, tn, (unsigned long long)*(const uint8_t*)pay); }
    if (__eir_is(tn, "uint16")) { ESK_RT_snprintf(fmt, sizeof fmt, "%s%%s %%llu", head); return ESK_RT_snprintf(buf, n, fmt, tn, (unsigned long long)*(const uint16_t*)pay); }
    if (__eir_is(tn, "uint64")) { ESK_RT_snprintf(fmt, sizeof fmt, "%s%%s %%llu", head); return ESK_RT_snprintf(buf, n, fmt, tn, (unsigned long long)*(const uint64_t*)pay); }
    if (__eir_is(tn, "double")) { ESK_RT_snprintf(fmt, sizeof fmt, "%s%%s %%g", head); return ESK_RT_snprintf(buf, n, fmt, tn, *(const double*)pay); }
    if (__eir_is(tn, "float")) { ESK_RT_snprintf(fmt, sizeof fmt, "%s%%s %%g", head); return ESK_RT_snprintf(buf, n, fmt, tn, (double)*(const float*)pay); }
    if (__eir_is(tn, "bool")) { ESK_RT_snprintf(fmt, sizeof fmt, "%s%%s %%s", head); return ESK_RT_snprintf(buf, n, fmt, tn, *(const uint8_t*)pay ? "true" : "false"); }
    if (__eir_is(tn, "char")) { ESK_RT_snprintf(fmt, sizeof fmt, "%s%%s '%%c'", head); return ESK_RT_snprintf(buf, n, fmt, tn, *(const char*)pay); }
    if (__eir_is(tn, "string") && *(const char* const*)pay) { ESK_RT_snprintf(fmt, sizeof fmt, "%s%%s \"%%s\"", head); return ESK_RT_snprintf(buf, n, fmt, tn, *(const char* const*)pay); }
    return ESK_RT_snprintf(buf, n, fmt, tn);
}

static void* __eir_current(void) {
    if (__eir_ncaught > 0 && __eir_ncaught <= ESK_MAX_CAUGHT) return __eir_caught[__eir_ncaught - 1];
    return __eir_pending;
}

char* __eir_exception_text(void) {
    void* ex = __eir_current();
    __eir_text[0] = 0;
    if (ex) __eir_format(ex, "", __eir_text, sizeof __eir_text);
    return __eir_text;
}

void __exception_text_into(char* dst) {
    void* ex = __eir_current();
    if (!dst) return;
    dst[0] = 0;
    if (ex) __eir_format(ex, "", dst, 512);
}

void __eir_uncaught(void) {
    char buf[512];
    int len = __eir_format(__eir_pending, "error: uncaught exception: ", buf, sizeof buf - 1);
    if (len < 0) len = 0;
    if (len > (int)sizeof buf - 2) len = (int)sizeof buf - 2;
    buf[len++] = '\n';
    ESK_RT_fflush(0);
#ifdef _WIN32
    ESK_RT__write(2, buf, (unsigned int)len);
#else
    ESK_RT_write(2, buf, (size_t)len);
#endif
    ESK_RT_abort();
}

typedef struct { void* fn; void* env; int32_t owned; } __eir_thread_start;

static void __eir_thread_run(__eir_thread_start* st) {
    ((void (*)(void*))st->fn)(st->env);
    if (__eir_pending) __eir_uncaught();
    if (st->owned && st->env) ESK_RT_free(st->env);
    ESK_RT_free(st);
}

#ifdef _WIN32
static unsigned long __eir_thread_entry(void* arg) { __eir_thread_run((__eir_thread_start*)arg); return 0; }
void* __eir_thread_create(void* closure, int32_t owned) {
    __eir_thread_start* st = (__eir_thread_start*)ESK_RT_malloc(sizeof *st);
    if (!closure || !st) return 0;
    st->fn = ((void**)closure)[0];
    st->env = ((void**)closure)[1];
    st->owned = owned;
    return ESK_RT_CreateThread(0, 0, __eir_thread_entry, st, 0, 0);
}
void __eir_thread_join(void* handle) {
    if (!handle) return;
    ESK_RT_WaitForSingleObject(handle, 0xFFFFFFFFul);
    ESK_RT_CloseHandle(handle);
}
#else
static void* __eir_thread_entry(void* arg) { __eir_thread_run((__eir_thread_start*)arg); return 0; }
void* __eir_thread_create(void* closure, int32_t owned) {
    __eir_thread_start* st = (__eir_thread_start*)ESK_RT_malloc(sizeof *st);
    uintptr_t* tid = (uintptr_t*)ESK_RT_malloc(sizeof *tid);
    if (!closure || !st || !tid) return 0;
    st->fn = ((void**)closure)[0];
    st->env = ((void**)closure)[1];
    st->owned = owned;
    if (ESK_RT_pthread_create(tid, 0, __eir_thread_entry, st) != 0) ESK_RT_abort();
    return tid;
}
void __eir_thread_join(void* handle) {
    if (!handle) return;
    ESK_RT_pthread_join(*(uintptr_t*)handle, 0);
    ESK_RT_free(handle);
}
#endif
)RT";

const char* kRuntimeNames[] = {
    "__eir_alloc_exception", "__eir_throw", "__eir_exc_pending", "__eir_take_exception",
    "__eir_clear_exception", "__eir_push_caught", "__eir_pop_caught", "__eir_caught_depth",
    "__eir_set_caught_depth", "__eir_exception_text", "__exception_text_into", "__eir_uncaught",
    "__eir_thread_create", "__eir_thread_join", "__eir_free_exception",
};

} // namespace

std::string runtimeStandaloneSource() {
    std::string s = "/* Eskiu EIR native runtime */\n"
                    "#include <stdint.h>\n#include <stddef.h>\n#include <stdlib.h>\n"
                    "#include <stdio.h>\n#include <string.h>\n"
                    "#ifdef _WIN32\n#include <io.h>\n#include <windows.h>\n"
                    "#else\n#include <unistd.h>\n#include <pthread.h>\n#endif\n";
    for (const auto& f : kLibcFns) {
        std::string n = f.name;
        if (n == "pthread_create") {
            s += "#define ESK_RT_pthread_create(t, a, f, x) pthread_create((pthread_t*)(t), 0, f, x)\n";
        } else if (n == "pthread_join") {
            s += "#define ESK_RT_pthread_join(t, r) pthread_join((pthread_t)(t), r)\n";
        } else if (n == "CreateThread") {
            s += "#define ESK_RT_CreateThread(a, b, f, x, c, d) CreateThread(0, 0, (LPTHREAD_START_ROUTINE)(f), x, 0, 0)\n";
        } else {
            s += "#define ESK_RT_" + n + " " + n + "\n";
        }
    }
    return s + kRuntimeBody;
}

std::string runtimeInlineSource(const Module* mod) {
    std::set<std::string> declared;
    if (mod) {
        for (const auto& fn : mod->functions) declared.insert(fn->name);
    }
    std::string s = "/* Eskiu EIR native runtime */\n";
    std::string win, posix;
    for (const auto& f : kLibcFns) {
        std::string n = f.name;
        std::string lines;
        if (!declared.count(n)) lines += std::string("extern ") + f.proto + ";\n";
        lines += "#define ESK_RT_" + n + " ((" + f.cast + ")" + n + ")\n";
        if (f.windows) win += lines;
        else if (n == "pthread_create" || n == "pthread_join" || n == "write") posix += lines;
        else s += lines;
    }
    s += "#ifdef _WIN32\n" + win + "#else\n" + posix + "#endif\n";
    return s + kRuntimeBody;
}

bool moduleNeedsRuntime(const Module* mod) {
    if (!mod) return false;
    std::set<std::string> names(std::begin(kRuntimeNames), std::end(kRuntimeNames));
    for (const auto& fn : mod->functions) {
        for (const auto& bb : fn->blocks) {
            for (const auto& inst : bb->instructions) {
                if (inst->op == Opcode::Throw || inst->op == Opcode::LandingPad) return true;
                if (inst->op == Opcode::Call && !inst->operands.empty() && names.count(inst->operands[0].text))
                    return true;
            }
        }
    }
    return false;
}

bool moduleThrows(const Module* mod) {
    if (!mod) return false;
    for (const auto& fn : mod->functions) {
        for (const auto& bb : fn->blocks) {
            for (const auto& inst : bb->instructions) {
                if (inst->op == Opcode::Throw) return true;
            }
        }
    }
    return false;
}

bool moduleUsesThreads(const Module* mod) {
    if (!mod) return false;
    for (const auto& fn : mod->functions) {
        for (const auto& bb : fn->blocks) {
            for (const auto& inst : bb->instructions) {
                if (inst->op == Opcode::Call && !inst->operands.empty() &&
                    inst->operands[0].text == "__eir_thread_create")
                    return true;
            }
        }
    }
    return false;
}

} // namespace eir
