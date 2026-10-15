/* SPDX-License-Identifier: GPL-2.0-or-later
 * FM2 proof of concept: exception table and SIGSEGV/SIGBUS handler.
 *
 * The handler does three things and nothing else: read RIP, binary-search the
 * sorted table, write RIP. No locks, no allocation, no calls into anything
 * that is not async-signal-safe, so it is safe on any thread and any stack,
 * including a fiber stack without an alternate signal stack. A fault whose RIP
 * is not in the table is passed to the handler that was installed before. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include "fm2.h"

/* Provided by the linker for a section whose name is a C identifier. */
extern const int32_t __start_acre_fmx[] __attribute__((weak));
extern const int32_t __stop_acre_fmx[] __attribute__((weak));

typedef struct { uint32_t insn, label; } Entry; /* offsets from g_base */

static Entry* g_table;
static size_t g_count;
static uintptr_t g_base;
static atomic_ulong g_faults;
static double g_init_seconds;
static struct sigaction g_prev_segv, g_prev_bus;
static atomic_int g_state; /* 0 none, 1 installing, 2 ready, -1 failed */

static int by_insn(const void* a, const void* b) {
    const Entry* x = a; const Entry* y = b;
    return x->insn < y->insn ? -1 : x->insn > y->insn;
}

static uintptr_t lookup(uintptr_t rip) {
    size_t lo = 0, hi = g_count;
    if (rip < g_base || rip - g_base > UINT32_MAX) return 0;
    const uint32_t key = (uint32_t)(rip - g_base);
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (g_table[mid].insn < key) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_count && g_table[lo].insn == key ? g_base + g_table[lo].label : 0;
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
    const uintptr_t to = lookup((uintptr_t)uc->uc_mcontext.gregs[REG_RIP]);
    if (to) {
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)to;
        atomic_fetch_add_explicit(&g_faults, 1, memory_order_relaxed);
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
    const size_t n = words / 2;
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    for (size_t i = 0; i < n; ++i) {
        const int32_t* e = __start_acre_fmx + 2 * i;
        const uintptr_t insn = (uintptr_t)&e[0] + e[0], label = (uintptr_t)&e[1] + e[1];
        if (insn < lo) lo = insn;
        if (label < lo) lo = label;
        if (insn > hi) hi = insn;
        if (label > hi) hi = label;
    }
    if (n && hi - lo > UINT32_MAX) { atomic_store(&g_state, -1); return -1; }
    g_table = malloc((n ? n : 1) * sizeof *g_table);
    if (!g_table) { atomic_store(&g_state, -1); return -1; }
    g_base = lo;
    for (size_t i = 0; i < n; ++i) {
        const int32_t* e = __start_acre_fmx + 2 * i;
        g_table[i].insn = (uint32_t)((uintptr_t)&e[0] + e[0] - lo);
        g_table[i].label = (uint32_t)((uintptr_t)&e[1] + e[1] - lo);
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

unsigned long fm2_faults(void) { return atomic_load_explicit(&g_faults, memory_order_relaxed); }
size_t fm2_table_entries(void) { return g_count; }
double fm2_init_seconds(void) { return g_init_seconds; }
