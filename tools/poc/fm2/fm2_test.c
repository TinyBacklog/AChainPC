/* SPDX-License-Identifier: GPL-2.0-or-later
 * FM2 proof of concept: correctness checks and microbenchmarks.
 *
 * Guest memory is one memfd mapped twice, like suyu's HostMemory: a "backing"
 * view that is always read-write (what Core::Memory and the slow path use)
 * and an "arena" view whose pages carry the protections the emulator would
 * set. Arena pages: p0 RW, p1 read-only (GPU-cached, reactive flushing off),
 * p2 none (GPU-cached with reactive flushing, debug or unmapped), p3 RW. A
 * private read-write sentinel page follows the arena to catch any access that
 * runs past its end. */
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
#include "fm2.h"

#define PAGE 4096u
#define NPAGES 4u
#define ASIZE (NPAGES * PAGE)

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
    g_ctx.limit = ASIZE - 7;
}

static uint64_t ref_load(uint64_t a, unsigned size) { uint64_t v = 0; memcpy(&v, g_backing + a, size); return v; }

typedef uint64_t (*LoadFn)(Ctx*, uint64_t);
typedef void (*StoreFn)(Ctx*, uint64_t, uint64_t);
static uint64_t l8(Ctx* c, uint64_t a) { return fm2_load8(c, a); }
static uint64_t l16(Ctx* c, uint64_t a) { return fm2_load16(c, a); }
static uint64_t l32(Ctx* c, uint64_t a) { return fm2_load32(c, a); }
static uint64_t l64(Ctx* c, uint64_t a) { return fm2_load64(c, a); }
static void s8(Ctx* c, uint64_t a, uint64_t v) { fm2_store8(c, a, v); }
static void s16(Ctx* c, uint64_t a, uint64_t v) { fm2_store16(c, a, v); }
static void s32(Ctx* c, uint64_t a, uint64_t v) { fm2_store32(c, a, v); }
static void s64(Ctx* c, uint64_t a, uint64_t v) { fm2_store64(c, a, v); }
static const LoadFn kLoads[] = {l8, l16, l32, l64};
static const StoreFn kStores[] = {s8, s16, s32, s64};
static const unsigned kSizes[] = {1, 2, 4, 8};

/* Expect a load at `a` to return the backing bytes and take `slow` slow calls
   and `faults` recovered faults. */
static void expect_load(unsigned w, uint64_t a, unsigned slow, unsigned faults, const char* what) {
    const unsigned long s0 = atomic_load(&g_slow_calls), f0 = fm2_faults();
    const uint64_t v = kLoads[w](&g_ctx, a);
    const uint64_t want = a < ASIZE && a + kSizes[w] <= ASIZE ? ref_load(a, kSizes[w]) : 0;
    CHECK(v == want, "%s: load%u @%#llx = %#llx, want %#llx", what, kSizes[w] * 8, (unsigned long long)a,
          (unsigned long long)v, (unsigned long long)want);
    CHECK(atomic_load(&g_slow_calls) - s0 == slow, "%s: load%u slow calls %lu, want %u", what, kSizes[w] * 8,
          atomic_load(&g_slow_calls) - s0, slow);
    CHECK(fm2_faults() - f0 == faults, "%s: load%u faults %lu, want %u", what, kSizes[w] * 8, fm2_faults() - f0, faults);
}

static void expect_store(unsigned w, uint64_t a, unsigned slow, unsigned faults, const char* what) {
    const unsigned long s0 = atomic_load(&g_slow_calls), f0 = fm2_faults();
    const uint64_t v = 0x1122334455667788ull ^ a;
    kStores[w](&g_ctx, a, v);
    if (a < ASIZE && a + kSizes[w] <= ASIZE) {
        CHECK(ref_load(a, kSizes[w]) == (v & (kSizes[w] == 8 ? ~0ull : (1ull << (8 * kSizes[w])) - 1)),
              "%s: store%u @%#llx not visible in backing", what, kSizes[w] * 8, (unsigned long long)a);
    }
    CHECK(atomic_load(&g_slow_calls) - s0 == slow, "%s: store%u slow calls %lu, want %u", what, kSizes[w] * 8,
          atomic_load(&g_slow_calls) - s0, slow);
    CHECK(fm2_faults() - f0 == faults, "%s: store%u faults %lu, want %u", what, kSizes[w] * 8, fm2_faults() - f0, faults);
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
    fm2_store64(&g_ctx, PAGE - 4, 0xFFFFFFFFFFFFFFFFull);
    CHECK(memcmp(g_p0_tail_at_slow, before, 8) == 0, "faulting crossing store wrote part of the first page");
}

static void test_limit(void) {
    const uint8_t want = 0xA5;
    const unsigned long f0 = fm2_faults();
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
    CHECK(fm2_faults() == f0, "limit checks faulted");
    const uint64_t saved = g_ctx.limit;
    g_ctx.limit = 0;
    expect_load(3, 0x10, 1, 0, "limit 0 = off");
    expect_store(3, 0x10, 1, 0, "limit 0 = off");
    g_ctx.limit = saved;
}

/* Many live values in integer and vector registers across a faulting access.
   The same function run with the fast path off must give the same answer. */
__attribute__((noinline)) static uint64_t live_values(Ctx* c, uint64_t a, uint64_t s) {
    uint64_t r[12];
    double d[6];
    for (int i = 0; i < 12; ++i) r[i] = s * (2 * i + 3) ^ (s >> i);
    for (int i = 0; i < 6; ++i) d[i] = (double)(s >> (i + 3)) * 1.25 + i;
    const uint64_t v = fm2_load64(c, a);
    fm2_store32(c, a + 0x100, v ^ r[3]);
    uint64_t acc = v;
    for (int i = 0; i < 12; ++i) acc = acc * 31 + r[i];
    for (int i = 0; i < 6; ++i) acc ^= (uint64_t)(d[i] * 3.0);
    return acc;
}

static void test_registers(void) {
    for (uint64_t s = 1; s < 2000; s += 7) {
        const uint64_t a = 2 * PAGE + (s % 0x700); /* p2: always faults */
        const unsigned long f0 = fm2_faults();
        const uint64_t fast = live_values(&g_ctx, a, s);
        const uint64_t faults = fm2_faults() - f0;
        const uint64_t saved = g_ctx.limit;
        g_ctx.limit = 0;
        const uint64_t slow = live_values(&g_ctx, a, s);
        g_ctx.limit = saved;
        CHECK(fast == slow, "live values differ after fault (s=%llu)", (unsigned long long)s);
        CHECK(faults == 2, "expected two faults, got %llu", (unsigned long long)faults);
    }
}

/* A fault that is not ours goes to the handler installed before ours. */
static sigjmp_buf g_jump;
static volatile sig_atomic_t g_prev_called;
static void prev_handler(int sig, siginfo_t* info, void* uc) {
    (void)sig; (void)info; (void)uc;
    g_prev_called = 1;
    siglongjmp(g_jump, 1);
}

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
    const int alt = (int)(intptr_t)arg;
    if (alt) {
        stack_t ss = {.ss_sp = malloc(1 << 16), .ss_size = 1 << 16, .ss_flags = 0};
        sigaltstack(&ss, NULL);
    }
    uint64_t sum = 0;
    for (int i = 0; i < 200000; ++i) {
        sum += fm2_load32(&g_ctx, 2 * PAGE + (i & 0xFF) * 4); /* faults */
        sum += fm2_load32(&g_ctx, (i & 0xFF) * 4);            /* fast */
    }
    return (void*)(uintptr_t)sum;
}

static void test_threads(void) {
    pthread_t t[4];
    const unsigned long f0 = fm2_faults();
    for (int i = 0; i < 4; ++i) pthread_create(&t[i], NULL, thread_main, (void*)(intptr_t)(i == 0));
    for (int i = 0; i < 4; ++i) pthread_join(t[i], NULL);
    CHECK(fm2_faults() - f0 == 4ul * 200000, "thread faults %lu, want %lu", fm2_faults() - f0, 4ul * 200000);
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
    for (long i = 0; i < n; ++i) s += fm2_load32(&g_ctx, (uint64_t)(i & 0x3FF) * 4);
    double t2 = now();
    sink = s; s = 0;
    const long nf = 2000000;
    for (long i = 0; i < nf; ++i) s += fm2_load32(&g_ctx, 2 * PAGE + (uint64_t)(i & 0x3FF) * 4);
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
    if (fm2_install() != 0 || fm2_install() != 0) { printf("FAIL install\n"); return 1; }
    printf("table: %zu entries, init %.3f ms\n", fm2_table_entries(), fm2_init_seconds() * 1e3);

    test_widths_and_pages();
    test_no_partial_store();
    test_limit();
    test_registers();
    test_chaining();
    test_threads();
    if (argc > 1 && strcmp(argv[1], "--bench") == 0) bench();
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("ok\n");
    return 0;
}
