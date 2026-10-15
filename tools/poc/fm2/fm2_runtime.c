/* SPDX-License-Identifier: GPL-2.0-or-later
 * FM2 proof of concept: exception table and SIGSEGV/SIGBUS handler.
 *
 * The handler reads RIP, binary-searches the sorted table, counts the fault
 * against the access, demotes the access's function once the count reaches
 * the threshold (stores 0 to its limit word) and writes RIP. No locks, no
 * allocation, no calls into anything that is not async-signal-safe, so it is
 * safe on any thread and any stack, including a fiber stack without an
 * alternate signal stack. A fault whose RIP is not in the table is passed to
 * the handler that was installed before. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include "fm2.h"

/* Provided by the linker for sections whose names are C identifiers. */
extern const int32_t __start_acre_fmx[] __attribute__((weak));
extern const int32_t __stop_acre_fmx[] __attribute__((weak));
extern uint64_t __start_acre_fml[] __attribute__((weak));
extern uint64_t __stop_acre_fml[] __attribute__((weak));

/* insn and label: offsets from g_base; lim: index into acre_fml. */
typedef struct { uint32_t insn, label, lim; } Entry;

static Entry* g_table;
static _Atomic uint32_t* g_counts; /* faults per entry, same order as g_table */
static size_t g_count;
static uintptr_t g_base;
static atomic_ulong g_faults, g_demotions;
static atomic_uint g_demote_after = 16;
static double g_init_seconds;
static struct sigaction g_prev_segv, g_prev_bus;
static atomic_int g_state; /* 0 none, 1 installing, 2 ready, -1 failed */

static int by_insn(const void* a, const void* b) {
    const Entry* x = a; const Entry* y = b;
    return x->insn < y->insn ? -1 : x->insn > y->insn;
}

static size_t lookup(uintptr_t rip) {
    size_t lo = 0, hi = g_count;
    if (rip < g_base || rip - g_base > UINT32_MAX) return SIZE_MAX;
    const uint32_t key = (uint32_t)(rip - g_base);
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (g_table[mid].insn < key) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_count && g_table[lo].insn == key ? lo : SIZE_MAX;
}

static void chain(int sig, siginfo_t* info, void* uc, const struct sigaction* prev) {
    if (prev->sa_flags & SA_SIGINFO) {
        prev->sa_sigaction(sig, info, uc);
    } else if (prev->sa_handler == SIG_DFL || prev->sa_handler == SIG_IGN) {
        /* Restore the default and return: the faulting instruction runs again
           and the default action (core dump) happens with the right state. */
        signal(sig, SIG_DFL);
    } else {
        prev->sa_handler(sig);
    }
}

static void handler(int sig, siginfo_t* info, void* ucv) {
    ucontext_t* uc = ucv;
    const size_t i = lookup((uintptr_t)uc->uc_mcontext.gregs[REG_RIP]);
    if (i != SIZE_MAX) {
        const Entry* e = &g_table[i];
        atomic_fetch_add_explicit(&g_faults, 1, memory_order_relaxed);
        const unsigned after = atomic_load_explicit(&g_demote_after, memory_order_relaxed);
        if (atomic_fetch_add_explicit(&g_counts[i], 1, memory_order_relaxed) + 1 == after) {
            /* Other threads may still hold the old limit for one access; they
               fault once more and land here again, past the threshold. */
            __atomic_store_n(&__start_acre_fml[e->lim], 0, __ATOMIC_RELAXED);
            atomic_fetch_add_explicit(&g_demotions, 1, memory_order_relaxed);
        }
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(g_base + e->label);
        return;
    }
    chain(sig, info, ucv, sig == SIGBUS ? &g_prev_bus : &g_prev_segv);
}

int fm2_install(void) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_state, &expected, 1)) {
        while (atomic_load(&g_state) == 1) { }
        return atomic_load(&g_state) == 2 ? 0 : -1;
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const size_t words = (size_t)(__stop_acre_fmx - __start_acre_fmx);
    const size_t n = words / 3;
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    for (size_t i = 0; i < n; ++i) {
        const int32_t* e = __start_acre_fmx + 3 * i;
        const uintptr_t insn = (uintptr_t)&e[0] + e[0], label = (uintptr_t)&e[1] + e[1];
        if (insn < lo) lo = insn;
        if (label < lo) lo = label;
        if (insn > hi) hi = insn;
        if (label > hi) hi = label;
    }
    if (n && hi - lo > UINT32_MAX) { atomic_store(&g_state, -1); return -1; }
    g_table = malloc((n ? n : 1) * sizeof *g_table);
    g_counts = calloc(n ? n : 1, sizeof *g_counts);
    if (!g_table || !g_counts) { atomic_store(&g_state, -1); return -1; }
    g_base = lo;
    const uintptr_t lim_lo = (uintptr_t)__start_acre_fml, lim_hi = (uintptr_t)__stop_acre_fml;
    for (size_t i = 0; i < n; ++i) {
        const int32_t* e = __start_acre_fmx + 3 * i;
        const uintptr_t lim = (uintptr_t)&e[2] + e[2];
        if (lim < lim_lo || lim >= lim_hi || (lim - lim_lo) % 8) { atomic_store(&g_state, -1); return -1; }
        g_table[i].insn = (uint32_t)((uintptr_t)&e[0] + e[0] - lo);
        g_table[i].label = (uint32_t)((uintptr_t)&e[1] + e[1] - lo);
        g_table[i].lim = (uint32_t)((lim - lim_lo) / 8);
    }
    qsort(g_table, n, sizeof *g_table, by_insn);
    g_count = n;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &g_prev_segv) != 0 || sigaction(SIGBUS, &sa, &g_prev_bus) != 0) {
        atomic_store(&g_state, -1);
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    g_init_seconds = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    atomic_store(&g_state, 2);
    return 0;
}

void fm2_set_all_limits(uint64_t v) {
    for (uint64_t* w = __start_acre_fml; w < __stop_acre_fml; ++w) __atomic_store_n(w, v, __ATOMIC_RELAXED);
    for (size_t i = 0; i < g_count; ++i) atomic_store_explicit(&g_counts[i], 0, memory_order_relaxed);
}

void fm2_set_demote_after(unsigned n) { atomic_store_explicit(&g_demote_after, n, memory_order_relaxed); }
unsigned long fm2_faults(void) { return atomic_load_explicit(&g_faults, memory_order_relaxed); }
unsigned long fm2_demotions(void) { return atomic_load_explicit(&g_demotions, memory_order_relaxed); }
size_t fm2_table_entries(void) { return g_count; }
size_t fm2_limit_words(void) { return (size_t)(__stop_acre_fml - __start_acre_fml); }
double fm2_init_seconds(void) { return g_init_seconds; }
