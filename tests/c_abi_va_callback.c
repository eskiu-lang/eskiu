#include <stdarg.h>

typedef int (*reporter_t)(void* ud, const char* fmt, va_list ap);

static int call(reporter_t cb, void* ud, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = cb(ud, fmt, ap);
    va_end(ap);
    return r;
}

int c_report(void* cb, void* ud) {
    return call((reporter_t)cb, ud, "%d %s %.2f %lld", 42, "hi", 2.5, 1LL << 40);
}

int c_sum(void* cb) {
    return call((reporter_t)cb, 0, "", 4, 10, 20, 30, 40);
}
