#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int id; double score; char name[24]; } Item;

static int cmp_items(const void* a, const void* b)
{
    const Item* x = a;
    const Item* y = b;
    if (x->score < y->score) return 1;
    if (x->score > y->score) return -1;
    return x->id - y->id;
}

static __declspec(noinline) uint64_t fib(int n) { return n < 2 ? (uint64_t)n : fib(n - 1) + fib(n - 2); }

static __declspec(noinline) const char* kind(int v)
{
    switch (v) {
    case 0: return "zero";
    case 1: return "one";
    case 2: return "two";
    case 3: return "three";
    case 4: return "four";
    case 5: return "five";
    case 6: return "six";
    case 7: return "seven";
    default: return "many";
    }
}

typedef int (*op_fn)(int, int);
static int op_add(int a, int b) { return a + b; }
static int op_mul(int a, int b) { return a * b; }
static int op_xor(int a, int b) { return a ^ b; }

static __declspec(noinline) uint32_t crc32(const uint8_t* p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

int main(int argc, char** argv)
{
    printf("hello from recompiled x64 (argc=%d)\n", argc);
    printf("fib(30) = %llu\n", fib(30));

    for (int i = 0; i < 10; i++) printf("%s%s", kind(i), i < 9 ? " " : "\n");

    op_fn ops[3] = { op_add, op_mul, op_xor };
    int acc = 7;
    for (int i = 0; i < 30; i++) acc = ops[i % 3](acc, i + 3) & 0xFFFFF;
    printf("ops acc = %d\n", acc);

    int64_t big = 0x123456789ABCDEFLL;
    uint64_t ub = 0xFEDCBA9876543210ULL;
    printf("div %lld mod %lld udiv %llu mul %llx\n", big / 12345, big % 12345, ub / 777, ub * 0x9E3779B97F4A7C15ULL);
    printf("shifts %llx %llx %lld\n", ub >> 13, ub << 7, (int64_t)ub >> 9);

    double s = 0;
    for (int i = 1; i <= 1000; i++) s += sqrt((double)i) * sin(i * 0.01);
    float f = 1.5f;
    for (int i = 0; i < 20; i++) f = f * 1.1f - 0.05f;
    printf("math %.6f %.4f %.3e %d\n", s, f, pow(2.0, 0.5) * 1e10, (int)floor(-2.5));

    Item* items = malloc(sizeof(Item) * 8);
    const char* names[8] = { "neighbor", "ladder", "key", "crowbar", "glue", "box", "radio", "lamp" };
    for (int i = 0; i < 8; i++) {
        items[i].id = i;
        items[i].score = fmod(i * 37.5, 11.0);
        strcpy(items[i].name, names[i]);
    }
    qsort(items, 8, sizeof(Item), cmp_items);
    for (int i = 0; i < 8; i++) printf("  %d %-9s %5.2f\n", items[i].id, items[i].name, items[i].score);
    Item copy = items[3];
    free(items);
    printf("copy %d %s\n", copy.id, copy.name);

    char buf[128];
    snprintf(buf, sizeof buf, "%08X|%-6s|%+d|%c", 0xBEEFu, "ab", 42, 'z');
    printf("fmt %s len %zu\n", buf, strlen(buf));
    printf("crc %08x\n", crc32((const uint8_t*)buf, strlen(buf)));
    return 0;
}
