// throw.cpp - the oracle's exception sampler: a throw caught by reference, a cleanup on the way, a rethrow and a
// thrown pointer, so rts6740_elf_eh.lib's unwinder (the review's D5) runs on TI's simulator with its output on record.
#include <cstdio>

struct E { int code; E(int c) : code(c) {} };
struct Guard { const char *n; Guard(const char *s) : n(s) {} ~Guard() { std::printf("cleanup %s\n", n); } };

static volatile int depth = 3;
static void deep(int n) { Guard g("deep"); if (n == 0) throw E(42); deep(n - 1); }
static void rethrow() { try { deep(depth); } catch (E &e) { std::printf("caught %d, rethrowing\n", e.code); throw; } }

int main() {
    try { rethrow(); } catch (const E &e) { std::printf("caught again %d\n", e.code); }
    static int x = 7;
    try { throw &x; } catch (int *p) { std::printf("pointer %d\n", *p); }
    return 0;
}
