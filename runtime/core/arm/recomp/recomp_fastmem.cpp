// SPDX-License-Identifier: GPL-2.0-or-later
// Fastmem arena (FM2) runtime. See recomp_fastmem.h.
//
// The handler reads RIP, binary-searches the sorted table, counts the fault
// against the access, demotes the access's function once the count reaches
// the threshold (stores 0 to its limit word) and moves RIP to the access's
// slow-path label. No locks, no allocation, no calls into the emulator, so it
// is safe on any thread and any stack, including a fiber stack without an
// alternate signal stack. A fault whose RIP is not in the table goes to the
// handler that was installed before.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>

#include <pthread.h>
#include <signal.h>
#include <ucontext.h>

#include "core/arm/recomp/recomp_fastmem.h"

// Provided by the linker for sections whose names are C identifiers; absent
// (null) when the binary has no inline accesses.
extern "C" {
extern const int32_t __start_acre_fmx[] __attribute__((weak));
extern const int32_t __stop_acre_fmx[] __attribute__((weak));
extern uint64_t __start_acre_fml[] __attribute__((weak));
extern uint64_t __stop_acre_fml[] __attribute__((weak));
}

namespace {

// One per inline access. insn and label: offsets from g_base; lim: index into
// acre_fml.
struct Entry {
    uint32_t insn, label, lim;
};

const Entry* g_table;
std::atomic<uint32_t>* g_counts; // faults per entry, same order as g_table
size_t g_count;
uintptr_t g_base;
std::atomic<unsigned long> g_faults{0}, g_demotions{0};
std::atomic<unsigned> g_demote_after{16};
std::atomic<int> g_enabled{1};
double g_init_seconds;
struct sigaction g_prev_segv, g_prev_bus;
int g_install_result = -1;
std::once_flag g_install_once;

size_t LimitWords() {
    return __start_acre_fml ? static_cast<size_t>(__stop_acre_fml - __start_acre_fml) : 0;
}

size_t Lookup(uintptr_t rip) {
    if (rip < g_base || rip - g_base > UINT32_MAX) return SIZE_MAX;
    const uint32_t key = static_cast<uint32_t>(rip - g_base);
    size_t lo = 0, hi = g_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (g_table[mid].insn < key) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_count && g_table[lo].insn == key ? lo : SIZE_MAX;
}

void Chain(int sig, siginfo_t* info, void* uc, const struct sigaction& prev) {
    if (prev.sa_flags & SA_SIGINFO) {
        prev.sa_sigaction(sig, info, uc);
    } else if (prev.sa_handler == SIG_DFL) {
        // Restore the default and return: the faulting instruction runs again
        // and the default action (core dump) happens with the right state.
        signal(sig, SIG_DFL);
    } else if (prev.sa_handler != SIG_IGN) {
        prev.sa_handler(sig);
    }
}

void Handler(int sig, siginfo_t* info, void* ucv) {
    auto* uc = static_cast<ucontext_t*>(ucv);
    const size_t i = Lookup(static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]));
    if (i == SIZE_MAX) {
        Chain(sig, info, ucv, sig == SIGBUS ? g_prev_bus : g_prev_segv);
        return;
    }
    const Entry& e = g_table[i];
    g_faults.fetch_add(1, std::memory_order_relaxed);
    const unsigned after = g_demote_after.load(std::memory_order_relaxed);
    if (g_counts[i].fetch_add(1, std::memory_order_relaxed) + 1 == after) {
        // Other threads may still use the old limit for one access; they fault
        // once more and come back here, past the threshold.
        __atomic_store_n(&__start_acre_fml[e.lim], uint64_t{0}, __ATOMIC_RELAXED);
        g_demotions.fetch_add(1, std::memory_order_relaxed);
    }
    uc->uc_mcontext.gregs[REG_RIP] = static_cast<greg_t>(g_base + e.label);
}

int Install(int (*install_first)()) {
    const auto t0 = std::chrono::steady_clock::now();
    if (!__start_acre_fmx || +__stop_acre_fmx == +__start_acre_fmx) return 1;
    const size_t n = static_cast<size_t>(__stop_acre_fmx - __start_acre_fmx) / 3;
    const uintptr_t lim_lo = reinterpret_cast<uintptr_t>(__start_acre_fml);
    const uintptr_t lim_hi = reinterpret_cast<uintptr_t>(__stop_acre_fml);
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    for (size_t i = 0; i < n; ++i) {
        const int32_t* e = __start_acre_fmx + 3 * i;
        const uintptr_t insn = reinterpret_cast<uintptr_t>(&e[0]) + e[0];
        const uintptr_t label = reinterpret_cast<uintptr_t>(&e[1]) + e[1];
        lo = std::min({lo, insn, label});
        hi = std::max({hi, insn, label});
    }
    if (hi - lo > UINT32_MAX) return -1;
    auto table = std::make_unique<Entry[]>(n);
    for (size_t i = 0; i < n; ++i) {
        const int32_t* e = __start_acre_fmx + 3 * i;
        const uintptr_t lim = reinterpret_cast<uintptr_t>(&e[2]) + e[2];
        if (lim < lim_lo || lim >= lim_hi || (lim - lim_lo) % 8 != 0) return -1;
        table[i] = Entry{
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&e[0]) + e[0] - lo),
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&e[1]) + e[1] - lo),
            static_cast<uint32_t>((lim - lim_lo) / 8),
        };
    }
    std::sort(table.get(), table.get() + n, [](const Entry& a, const Entry& b) { return a.insn < b.insn; });
    // Value-initialised: the zero pages are only touched where faults happen.
    auto counts = std::make_unique<std::atomic<uint32_t>[]>(n);

    if (install_first) install_first();
    g_base = lo;
    g_count = n;
    g_table = table.release();
    g_counts = counts.release();
    struct sigaction sa {};
    sa.sa_sigaction = Handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &g_prev_segv) != 0) return -1;
    if (sigaction(SIGBUS, &sa, &g_prev_bus) != 0) {
        sigaction(SIGSEGV, &g_prev_segv, nullptr);
        return -1;
    }
    g_init_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return 0;
}

} // namespace

extern "C" {

int acre_fm_install(int (*install_first)(void)) {
    std::call_once(g_install_once, [install_first] {
        g_install_result = Install(install_first);
        if (g_install_result < 0) acre_fm_set_all_limits(0);
    });
    return g_install_result;
}

void acre_fm_set_all_limits(uint64_t v) {
    for (size_t i = 0, n = LimitWords(); i < n; ++i) {
        __atomic_store_n(&__start_acre_fml[i], v, __ATOMIC_RELAXED);
    }
    for (size_t i = 0; i < g_count; ++i) g_counts[i].store(0, std::memory_order_relaxed);
}

void acre_fm_set_demote_after(unsigned n) {
    g_demote_after.store(n, std::memory_order_relaxed);
}

void acre_fm_configure(int enabled, unsigned demote_after) {
    g_enabled.store(enabled != 0, std::memory_order_relaxed);
    acre_fm_set_demote_after(demote_after);
}

int acre_fm_configured_enabled(void) {
    return g_enabled.load(std::memory_order_relaxed);
}

unsigned long acre_fm_faults(void) {
    return g_faults.load(std::memory_order_relaxed);
}

unsigned long acre_fm_demotions(void) {
    return g_demotions.load(std::memory_order_relaxed);
}

size_t acre_fm_table_entries(void) {
    return g_count;
}

size_t acre_fm_limit_words(void) {
    return LimitWords();
}

double acre_fm_init_seconds(void) {
    return g_init_seconds;
}

int acre_fm_handler_is_ours(void) {
    struct sigaction cur {};
    return sigaction(SIGSEGV, nullptr, &cur) == 0 && (cur.sa_flags & SA_SIGINFO) &&
           cur.sa_sigaction == Handler;
}

int acre_fm_thread_can_fault(void) {
    sigset_t mask;
    if (pthread_sigmask(SIG_BLOCK, nullptr, &mask) != 0) return 0;
    return !sigismember(&mask, SIGSEGV) && !sigismember(&mask, SIGBUS);
}

} // extern "C"
