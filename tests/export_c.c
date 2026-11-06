// The C side of export_c.esk: calls each exported function with the C convention.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct { int a; int b; } Small;
typedef struct { double x; double y; } Pair;
typedef struct { int64_t a; int64_t b; int64_t c; int k; } Big;
typedef union { int i; float f; } Num;
#pragma pack(push, 1)
typedef struct { uint8_t tag; int value; } Tight;
#pragma pack(pop)

int add(int a, int b);
bool positive(int8_t v);
uint16_t widen(uint8_t b);
int64_t length(const char* s);
Small swap(Small s);
Pair scale(Pair p, double k);
Big grow(Big b, int k);
double bits(Num n);
int tight_sum(Tight t);
void fill(Small* out, int v);

int c_drive(void) {
    Small s = swap((Small){7, 9});
    Pair p = scale((Pair){1.5, -2.0}, 4.0);
    Big b = grow((Big){100, 200, 300, 0}, 5);
    Num n;
    n.f = 2.5f;
    Small f;
    fill(&f, 41);
    printf("c: %d %d %d %u %lld\n", add(40, 2), positive(-3), positive(3), widen(200), (long long)length("export"));
    printf("c: %d %d %.1f %.1f\n", s.a, s.b, p.x, p.y);
    printf("c: %lld %lld %lld %d\n", (long long)b.a, (long long)b.b, (long long)b.c, b.k);
    printf("c: %.1f %d %d %d\n", bits(n), tight_sum((Tight){3, 1000}), f.a, f.b);
    return 0;
}
