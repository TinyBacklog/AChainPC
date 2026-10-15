/* SPDX-License-Identifier: GPL-2.0-or-later
 * Fastmem arena (FM2): the inline accesses of recomp_fastmem_access.h and
 * the runtime of recomp_fastmem.cpp, together. Built from
 * tools/poc/fm2/fm2_test.c; `--bench` prints the microbenchmarks.
 *
 * Guest memory is one memfd mapped twice, like suyu's HostMemory: a "backing"
 * view that is always read-write (what Core::Memory and the slow path use)
 * and an "arena" view whose pages carry the protections the emulator would
 * set. Arena pages: p0 RW, p1 read-only (GPU-cached, reactive flushing off),
 * p2 none (GPU-cached with reactive flushing, debug or unmapped), p3 RW. A
 * private read-write sentinel page follows the arena to catch any access that
 * runs past its end.
 *
 * Every function that accesses guest memory owns a limit word
 * (RECOMP_BLOCK_LIMIT). Demotion is off (threshold 0) except in the tests that
 * check it, so the other tests see one fault per protected access. */
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "core/arm/recomp/recomp_fastmem.h"

typedef struct Ctx { uint8_t* arena; } Ctx;
uint64_t fm2_slow_load(Ctx* c, uint64_t a, unsigned size);
void fm2_slow_store(Ctx* c, uint64_t a, unsigned size, uint64_t v);
#define RECOMP_FX_CTX Ctx
#define RECOMP_FX_ARENA(c) ((c)->arena)
#define RECOMP_FX_SLOW_LOAD(c, a, n) fm2_slow_load((c), (a), (n))
#define RECOMP_FX_SLOW_STORE(c, a, n, v) fm2_slow_store((c), (a), (n), (v))
#include "core/arm/recomp/recomp_fastmem_access.h"

/* dynarmic's InstallFaultHandler, through a C++ shim (test_fastmem_dynarmic.cpp). */
int test_install_dynarmic_handler(void);

#define PAGE 4096u
#define NPAGES 4u
#define ASIZE (NPAGES * PAGE)
#define LIMIT (ASIZE - 7)

static uint8_t* g_backing;
static uint8_t* g_sentinel;
static Ctx g_ctx;
static atomic_ulong g_slow_calls;
static int g_failures;

/* Snapshot of p0's last bytes taken when a slow store starts: a crossing store
   that faulted must not have written anything to the first page. */
static uint8_t g_p0_tail_at_slow[8];

#define CHECK(cond, ...) do { if (!(cond)) { ++g_failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

uint64_t fm2_slow_load(Ctx* c, uint64_t a, unsigned size) {
    (void)c;
    atomic_fetch_add_explicit(&g_slow_calls, 1, memory_order_relaxed);
    uint64_t v = 0;
    if (a < ASIZE && a + size <= ASIZE) memcpy(&v, g_backing + a, size);
    return v;
}

void fm2_slow_store(Ctx* c, uint64_t a, unsigned size, uint64_t v) {
    (void)c;
    atomic_fetch_add_explicit(&g_slow_calls, 1, memory_order_relaxed);
    memcpy(g_p0_tail_at_slow, g_backing + PAGE - 8, 8);
    if (a < ASIZE && a + size <= ASIZE) memcpy(g_backing + a, &v, size);
}

static void setup(void) {
    const int fd = memfd_create("fm2", 0);
    if (fd < 0 || ftruncate(fd, ASIZE) != 0) { perror("memfd"); exit(2); }
    g_backing = mmap(NULL, ASIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    /* Reserve arena + sentinel, then map the arena over the front. */
    uint8_t* region = mmap(NULL, ASIZE + PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t* arena = mmap(region, ASIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    g_sentinel = mmap(region + ASIZE, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (g_backing == MAP_FAILED || arena == MAP_FAILED || g_sentinel == MAP_FAILED) { perror("mmap"); exit(2); }
    mprotect(arena + 1 * PAGE, PAGE, PROT_READ);
    mprotect(arena + 2 * PAGE, PAGE, PROT_NONE);
    memset(g_sentinel, 0xA5, PAGE);
    for (unsigned i = 0; i < ASIZE; ++i) g_backing[i] = (uint8_t)(i * 7 + 3);
    g_ctx.arena = arena;
}

/* All limit words back to the arena limit, fault counts cleared. */
static void reset_limits(void) { acre_fm_set_all_limits(LIMIT); }

static uint64_t ref_load(uint64_t a, unsigned size) { uint64_t v = 0; memcpy(&v, g_backing + a, size); return v; }

typedef uint64_t (*LoadFn)(Ctx*, uint64_t);
typedef void (*StoreFn)(Ctx*, uint64_t, uint64_t);
static uint64_t l8(Ctx* c, uint64_t a) { RECOMP_BLOCK_LIMIT; return RECOMP_FX_LD8(c, a); }
static uint64_t l16(Ctx* c, uint64_t a) { RECOMP_BLOCK_LIMIT; return RECOMP_FX_LD16(c, a); }
static uint64_t l32(Ctx* c, uint64_t a) { RECOMP_BLOCK_LIMIT; return RECOMP_FX_LD32(c, a); }
static uint64_t l64(Ctx* c, uint64_t a) { RECOMP_BLOCK_LIMIT; return RECOMP_FX_LD64(c, a); }
static void s8(Ctx* c, uint64_t a, uint64_t v) { RECOMP_BLOCK_LIMIT; RECOMP_FX_ST8(c, a, v); }
static void s16(Ctx* c, uint64_t a, uint64_t v) { RECOMP_BLOCK_LIMIT; RECOMP_FX_ST16(c, a, v); }
static void s32(Ctx* c, uint64_t a, uint64_t v) { RECOMP_BLOCK_LIMIT; RECOMP_FX_ST32(c, a, v); }
static void s64(Ctx* c, uint64_t a, uint64_t v) { RECOMP_BLOCK_LIMIT; RECOMP_FX_ST64(c, a, v); }
static const LoadFn kLoads[] = {l8, l16, l32, l64};
static const StoreFn kStores[] = {s8, s16, s32, s64};
static const unsigned kSizes[] = {1, 2, 4, 8};

/* Expect a load at `a` to return the backing bytes and take `slow` slow calls
   and `faults` recovered faults. */
static void expect_load(unsigned w, uint64_t a, unsigned slow, unsigned faults, const char* what) {
    const unsigned long s0 = atomic_load(&g_slow_calls), f0 = acre_fm_faults();
    const uint64_t v = kLoads[w](&g_ctx, a);
    const uint64_t want = a < ASIZE && a + kSizes[w] <= ASIZE ? ref_load(a, kSizes[w]) : 0;
    CHECK(v == want, "%s: load%u @%#llx = %#llx, want %#llx", what, kSizes[w] * 8, (unsigned long long)a,
          (unsigned long long)v, (unsigned long long)want);
    CHECK(atomic_load(&g_slow_calls) - s0 == slow, "%s: load%u slow calls %lu, want %u", what, kSizes[w] * 8,
          atomic_load(&g_slow_calls) - s0, slow);
    CHECK(acre_fm_faults() - f0 == faults, "%s: load%u faults %lu, want %u", what, kSizes[w] * 8, acre_fm_faults() - f0, faults);
}

static void expect_store(unsigned w, uint64_t a, unsigned slow, unsigned faults, const char* what) {
    const unsigned long s0 = atomic_load(&g_slow_calls), f0 = acre_fm_faults();
    const uint64_t v = 0x1122334455667788ull ^ a;
    kStores[w](&g_ctx, a, v);
    if (a < ASIZE && a + kSizes[w] <= ASIZE) {
        CHECK(ref_load(a, kSizes[w]) == (v & (kSizes[w] == 8 ? ~0ull : (1ull << (8 * kSizes[w])) - 1)),
              "%s: store%u @%#llx not visible in backing", what, kSizes[w] * 8, (unsigned long long)a);
    }
    CHECK(atomic_load(&g_slow_calls) - s0 == slow, "%s: store%u slow calls %lu, want %u", what, kSizes[w] * 8,
          atomic_load(&g_slow_calls) - s0, slow);
    CHECK(acre_fm_faults() - f0 == faults, "%s: store%u faults %lu, want %u", what, kSizes[w] * 8, acre_fm_faults() - f0, faults);
}

static void test_widths_and_pages(void) {
    for (unsigned w = 0; w < 4; ++w) {
        expect_load(w, 0x10, 0, 0, "p0 RW");
        expect_store(w, 0x20, 0, 0, "p0 RW");
        expect_load(w, PAGE + 0x10, 0, 0, "p1 RO load");
        expect_store(w, PAGE + 0x20, 1, 1, "p1 RO store");
        expect_load(w, 2 * PAGE + 0x10, 1, 1, "p2 none load");
        expect_store(w, 2 * PAGE + 0x20, 1, 1, "p2 none store");
        expect_load(w, 3 * PAGE + 0x13, 0, 0, "p3 RW unaligned");
        expect_store(w, 3 * PAGE + 0x23, 0, 0, "p3 RW unaligned");
    }
    /* Page crossing: p0->p1 load works in hardware; store faults on p1. */
    expect_load(3, PAGE - 4, 0, 0, "cross p0->p1 load");
    expect_store(3, PAGE - 4, 1, 1, "cross p0->p1 store");
    /* p1->p2 load faults on the second page only. */
    expect_load(3, 2 * PAGE - 4, 1, 1, "cross p1->p2 load");
}

static void test_no_partial_store(void) {
    /* An 8-byte store at the end of p3... p3 is the last page, use p0->p1
       (p1 read-only): the first four bytes are on a writable page. */
    uint8_t before[8];
    memcpy(before, g_backing + PAGE - 8, 8);
    s64(&g_ctx, PAGE - 4, 0xFFFFFFFFFFFFFFFFull);
    CHECK(memcmp(g_p0_tail_at_slow, before, 8) == 0, "faulting crossing store wrote part of the first page");
}

static void test_limit(void) {
    const uint8_t want = 0xA5;
    const unsigned long f0 = acre_fm_faults();
    expect_load(3, ASIZE - 8, 0, 0, "last full word");
    expect_load(3, ASIZE - 7, 1, 0, "past limit-7");
    expect_store(3, ASIZE - 7, 1, 0, "store past limit-7");
    expect_load(0, ASIZE - 1, 1, 0, "last byte via slow");
    expect_load(3, ASIZE, 1, 0, "at arena end");
    expect_load(3, UINT64_MAX - 3, 1, 0, "near 2^64");
    expect_store(3, UINT64_MAX - 3, 1, 0, "store near 2^64");
    for (unsigned i = 0; i < PAGE; ++i) {
        if (g_sentinel[i] != want) { CHECK(0, "sentinel page written at %u", i); break; }
    }
    CHECK(acre_fm_faults() == f0, "limit checks faulted");
    acre_fm_set_all_limits(0);
    expect_load(3, 0x10, 1, 0, "limit 0 = off");
    expect_store(3, 0x10, 1, 0, "limit 0 = off");
    expect_load(3, 2 * PAGE, 1, 0, "limit 0 = off, protected page");
    reset_limits();
}

/* Many live values in integer and vector registers across a faulting access.
   The same function run with the fast path off must give the same answer. */
__attribute__((noinline)) static uint64_t live_values(Ctx* c, uint64_t a, uint64_t s) {
    RECOMP_BLOCK_LIMIT;
    uint64_t r[12];
    double d[6];
    for (int i = 0; i < 12; ++i) r[i] = s * (2 * i + 3) ^ (s >> i);
    for (int i = 0; i < 6; ++i) d[i] = (double)(s >> (i + 3)) * 1.25 + i;
    const uint64_t v = RECOMP_FX_LD64(c, a);
    RECOMP_FX_ST32(c, a + 0x100, v ^ r[3]);
    uint64_t acc = v;
    for (int i = 0; i < 12; ++i) acc = acc * 31 + r[i];
    for (int i = 0; i < 6; ++i) acc ^= (uint64_t)(d[i] * 3.0);
    return acc;
}

static void test_registers(void) {
    for (uint64_t s = 1; s < 2000; s += 7) {
        const uint64_t a = 2 * PAGE + (s % 0x700); /* p2: always faults */
        const unsigned long f0 = acre_fm_faults();
        const uint64_t fast = live_values(&g_ctx, a, s);
        const uint64_t faults = acre_fm_faults() - f0;
        acre_fm_set_all_limits(0);
        const uint64_t slow = live_values(&g_ctx, a, s);
        reset_limits();
        CHECK(fast == slow, "live values differ after fault (s=%llu)", (unsigned long long)s);
        CHECK(faults == 2, "expected two faults, got %llu", (unsigned long long)faults);
    }
}

/* Demotion: after `after` faults of one access, its function's limit word is
   0. The function then takes the slow path everywhere, without faulting, and
   other functions keep their fast path. Resetting the words re-promotes it. */
__attribute__((noinline)) static uint64_t demo_a(Ctx* c, uint64_t a) { RECOMP_BLOCK_LIMIT; return RECOMP_FX_LD32(c, a) + RECOMP_FX_LD64(c, a + 8); }
__attribute__((noinline)) static uint64_t demo_b(Ctx* c, uint64_t a) { RECOMP_BLOCK_LIMIT; return RECOMP_FX_LD32(c, a); }

static void test_demotion(void) {
    const unsigned after = 16;
    acre_fm_set_demote_after(after);
    reset_limits();
    const unsigned long d0 = acre_fm_demotions();
    unsigned long f0 = acre_fm_faults(), s0 = atomic_load(&g_slow_calls);
    /* Each call faults twice (both loads on p2), counted per access. */
    for (unsigned i = 0; i < after - 1; ++i) demo_a(&g_ctx, 2 * PAGE + 0x40);
    CHECK(acre_fm_demotions() == d0, "demoted before %u faults", after);
    CHECK(acre_fm_faults() - f0 == 2 * (after - 1), "faults %lu before demotion", acre_fm_faults() - f0);
    demo_a(&g_ctx, 2 * PAGE + 0x40);
    CHECK(acre_fm_demotions() - d0 == 1, "two accesses reaching the threshold together demoted %lu times",
          acre_fm_demotions() - d0);
    /* The second access of that call ran after the store of 0: no fault. */
    CHECK(acre_fm_faults() - f0 == 2 * after - 1, "faults %lu at demotion, want %u", acre_fm_faults() - f0, 2 * after - 1);
    f0 = acre_fm_faults(); s0 = atomic_load(&g_slow_calls);
    const uint64_t want = ref_load(0x40, 4) + ref_load(0x48, 8);
    CHECK(demo_a(&g_ctx, 0x40) == want, "demoted function gives wrong values on RW memory");
    demo_a(&g_ctx, 2 * PAGE + 0x40);
    CHECK(acre_fm_faults() == f0, "demoted function still faults");
    CHECK(atomic_load(&g_slow_calls) - s0 == 4, "demoted function slow calls %lu, want 4",
          atomic_load(&g_slow_calls) - s0);
    s0 = atomic_load(&g_slow_calls);
    CHECK(demo_b(&g_ctx, 0x40) == ref_load(0x40, 4), "other function value");
    CHECK(atomic_load(&g_slow_calls) == s0, "other function lost its fast path");
    reset_limits();
    s0 = atomic_load(&g_slow_calls);
    demo_a(&g_ctx, 0x40);
    CHECK(atomic_load(&g_slow_calls) == s0, "reset did not re-promote");
    acre_fm_set_demote_after(0);
}

/* A fault that is not ours goes to the handler installed before ours. */
static sigjmp_buf g_jump;
static volatile sig_atomic_t g_prev_called;
static void prev_handler(int sig, siginfo_t* info, void* uc) {
    (void)sig; (void)info; (void)uc;
    g_prev_called = 1;
    siglongjmp(g_jump, 1);
}

static void test_handler_order(void) {
    CHECK(acre_fm_handler_is_ours(), "SIGSEGV action is not ours after install");
    /* dynarmic registers once; asking again must not put it above us. */
    test_install_dynarmic_handler();
    CHECK(acre_fm_handler_is_ours(), "dynarmic re-registered above us");
    struct sigaction other, saved;
    memset(&other, 0, sizeof other);
    other.sa_sigaction = prev_handler;
    other.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &other, &saved);
    CHECK(!acre_fm_handler_is_ours(), "replaced handler still reported as ours");
    sigaction(SIGSEGV, &saved, NULL);
    CHECK(acre_fm_handler_is_ours(), "restored handler not reported as ours");
}

static void* masked_thread(void* arg) {
    (void)arg;
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, SIGSEGV);
    pthread_sigmask(SIG_BLOCK, &m, NULL);
    return (void*)(intptr_t)acre_fm_thread_can_fault();
}

static void test_signal_mask(void) {
    CHECK(acre_fm_thread_can_fault(), "main thread reported as unable to take faults");
    pthread_t t;
    void* r;
    pthread_create(&t, NULL, masked_thread, NULL);
    pthread_join(t, &r);
    CHECK(r == NULL, "thread with SIGSEGV blocked reported as able to take faults");
}

/* A foreign fault crosses dynarmic's handler (which prints "Unhandled SIGSEGV")
   and reaches the handler installed before both. */
static void test_chaining(void) {
    if (!sigsetjmp(g_jump, 1)) {
        volatile uint8_t* p = (volatile uint8_t*)(g_ctx.arena + 2 * PAGE);
        (void)*p; /* plain C access, not in the table */
        CHECK(0, "unlisted access did not fault");
    }
    CHECK(g_prev_called, "previous handler was not called for a foreign fault");
}

/* Faults from several threads at once, one of them on an alternate stack. */
static void* thread_main(void* arg) {
    RECOMP_BLOCK_LIMIT;
    const int alt = (int)(intptr_t)arg;
    if (alt) {
        stack_t ss = {.ss_sp = malloc(1 << 16), .ss_size = 1 << 16, .ss_flags = 0};
        sigaltstack(&ss, NULL);
    }
    uint64_t sum = 0;
    for (int i = 0; i < 200000; ++i) {
        sum += RECOMP_FX_LD32(&g_ctx, 2 * PAGE + (i & 0xFF) * 4); /* faults */
        sum += RECOMP_FX_LD32(&g_ctx, (i & 0xFF) * 4);            /* fast */
    }
    return (void*)(uintptr_t)sum;
}

static uint64_t run_threads(void) {
    pthread_t t[4];
    uint64_t sum = 0;
    for (int i = 0; i < 4; ++i) pthread_create(&t[i], NULL, thread_main, (void*)(intptr_t)(i == 0));
    for (int i = 0; i < 4; ++i) {
        void* r;
        pthread_join(t[i], &r);
        sum += (uint64_t)(uintptr_t)r;
    }
    return sum;
}

static void test_threads(void) {
    reset_limits();
    unsigned long f0 = acre_fm_faults();
    const uint64_t want = run_threads();
    CHECK(acre_fm_faults() - f0 == 4ul * 200000, "thread faults %lu, want %lu", acre_fm_faults() - f0, 4ul * 200000);
    /* Same work with demotion: four threads race on one access. One demotion,
       a few faults past the threshold at most, same results. */
    acre_fm_set_demote_after(16);
    reset_limits();
    const unsigned long d0 = acre_fm_demotions();
    f0 = acre_fm_faults();
    CHECK(run_threads() == want, "threads with demotion read different values");
    CHECK(acre_fm_demotions() - d0 == 1, "thread demotions %lu, want 1", acre_fm_demotions() - d0);
    CHECK(acre_fm_faults() - f0 >= 16 && acre_fm_faults() - f0 < 16 + 64, "thread faults with demotion %lu",
          acre_fm_faults() - f0);
    acre_fm_set_demote_after(0);
    reset_limits();
}

/* Benchmarks. The baseline helper mirrors today's FM-lite fast path: a
   bounds/page-cross check and a page-table entry load, behind a call. */
static uint64_t g_pt[NPAGES * 4];
__attribute__((noinline)) static uint64_t helper_load32(Ctx* c, uint64_t a) {
    (void)c;
    if (a >= ASIZE) return fm2_slow_load(c, a, 4);
    if ((a & 0xFFF) > 0x1000 - 4) return fm2_slow_load(c, a, 4);
    uintptr_t e;
    memcpy(&e, (const uint8_t*)g_pt + ((a >> 12) << 5), sizeof e);
    e &= ~(uintptr_t)3;
    if (!e) return fm2_slow_load(c, a, 4);
    uint32_t v; memcpy(&v, (const uint8_t*)(e + a), 4); return v;
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void bench(void) {
    RECOMP_BLOCK_LIMIT;
    for (unsigned p = 0; p < NPAGES; ++p) {
        uintptr_t e = (p == 2) ? 0 : (uintptr_t)g_backing; /* entry = host page - guest page */
        memcpy((uint8_t*)g_pt + p * 32, &e, sizeof e);
    }
    const long n = 200000000;
    volatile uint64_t sink = 0;
    uint64_t s = 0;
    double t0 = now();
    for (long i = 0; i < n; ++i) s += helper_load32(&g_ctx, (uint64_t)(i & 0x3FF) * 4);
    double t1 = now();
    sink = s; s = 0;
    for (long i = 0; i < n; ++i) s += RECOMP_FX_LD32(&g_ctx, (uint64_t)(i & 0x3FF) * 4);
    double t2 = now();
    sink = s; s = 0;
    const long nf = 2000000;
    for (long i = 0; i < nf; ++i) s += RECOMP_FX_LD32(&g_ctx, 2 * PAGE + (uint64_t)(i & 0x3FF) * 4);
    double t3 = now();
    sink = s;
    (void)sink;
    printf("bench: helper (FM-lite-like) %.2f ns/access, FM2 inline %.2f ns/access, FM2 fault %.0f ns/fault\n",
           (t1 - t0) / n * 1e9, (t2 - t1) / n * 1e9, (t3 - t2) / nf * 1e9);
}

int main(int argc, char** argv) {
    setup();
    /* Install a "previous" handler first, so chaining can be observed. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = prev_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    /* dynarmic's handler goes in first, below ours: a foreign fault then goes
       ours -> dynarmic -> prev_handler. */
    if (acre_fm_install(test_install_dynarmic_handler) != 0 || acre_fm_install(NULL) != 0) {
        printf("FAIL install\n");
        return 1;
    }
    printf("table: %zu entries, %zu limit words, init %.3f ms\n", acre_fm_table_entries(), acre_fm_limit_words(),
           acre_fm_init_seconds() * 1e3);
    acre_fm_set_demote_after(0);
    reset_limits();

    test_widths_and_pages();
    test_no_partial_store();
    test_limit();
    test_registers();
    test_demotion();
    test_handler_order();
    test_signal_mask();
    test_chaining();
    test_threads();
    if (argc > 1 && strcmp(argv[1], "--bench") == 0) bench();
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("ok\n");
    return 0;
}
