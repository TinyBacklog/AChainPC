// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Measurement build only (-DACRE_MEMSTATS, CMake option ACRE_MEMSTATS): counts
// the guest accesses that leave the recompiled fast path and reach the
// emulator through HostLoad/HostStore, by the kind of page they touch. The
// count answers one question for the arena fastmem design (FM2): how many of
// these accesses would fault on the arena's protections, and how often. With
// the option off this header defines nothing and the production path is
// unchanged.
#ifdef ACRE_MEMSTATS
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <unwind.h>
#include <vector>
#include "common/common_types.h"
#include "common/page_table.h"

namespace Core::RecompMemStats {

// What the arena would do with the access. Memory = no fault (the slow path
// was taken only because the access crosses a page, or the page walk in the
// fast path declined for another reason); everything else is protected in the
// arena and faults under FM2. A page-crossing access is classed by the worse
// of its two pages.
enum Kind : unsigned { Memory, MemoryCrossing, Unmapped, Debug, Rasterizer, OutOfRange, kKinds };
inline constexpr const char* kKindNames[kKinds] = {"memory", "memory-cross", "unmapped",
                                                   "debug", "rasterizer", "out-of-range"};

struct Counters {
    std::array<std::atomic<u64>, kKinds> load{}, store{};
};
inline Counters g_counters;
inline std::atomic<long long> g_last_report_ns{0};

inline unsigned Rank(Common::PageType type) {
    switch (type) {
    case Common::PageType::Memory: return Memory;
    case Common::PageType::DebugMemory: return Debug;
    case Common::PageType::RasterizerCachedMemory: return Rasterizer;
    default: return Unmapped;
    }
}

inline Kind Classify(const void* page_table, u64 address_space_size, u64 va, u32 size) {
    if (!page_table || va >= address_space_size || va + size > address_space_size) return OutOfRange;
    const auto* entries = static_cast<const Common::PageTable::PageEntryData*>(page_table);
    const u64 first = va >> 12, last = (va + size - 1) >> 12;
    unsigned kind = Rank(entries[first].ptr.Type());
    if (last != first) {
        const unsigned second = Rank(entries[last].ptr.Type());
        kind = std::max(kind, second);
        if (kind == Memory) kind = MemoryCrossing;
    }
    return static_cast<Kind>(kind);
}

// Where the accesses that would fault come from. One in kSampleEvery of them
// is unwound a few frames up from HostLoad/HostStore and the return addresses
// are counted; the report prints the hottest stacks every kSiteReportSeconds.
// Symbolize offline with addr2line -f -e <binary> (the binary is not PIE).
inline constexpr unsigned kSampleEvery = 64;
inline constexpr unsigned kFrames = 8;
inline constexpr unsigned kSiteReportSeconds = 10;
inline constexpr unsigned kSiteReportTop = 40;
using Stack = std::array<uintptr_t, kFrames>;
struct Sites {
    std::mutex lock;
    std::map<std::pair<bool, Stack>, u64> hits;
    std::atomic<u64> tick{0};
    unsigned reports = 0;
};
inline Sites g_sites;

inline void SampleSite(bool store) {
    if (g_sites.tick.fetch_add(1, std::memory_order_relaxed) % kSampleEvery != 0) return;
    struct Walk {
        Stack stack{};
        unsigned depth = 0;
    } walk;
    // Innermost frames first; the first ones are this function and its
    // callers inside the emulator, the guest block follows them.
    _Unwind_Backtrace(
        [](_Unwind_Context* ctx, void* arg) -> _Unwind_Reason_Code {
            auto& w = *static_cast<Walk*>(arg);
            w.stack[w.depth] = _Unwind_GetIP(ctx);
            return ++w.depth < kFrames ? _URC_NO_REASON : _URC_END_OF_STACK;
        },
        &walk);
    std::scoped_lock guard{g_sites.lock};
    ++g_sites.hits[{store, walk.stack}];
}

inline void Count(bool store, const void* page_table, u64 address_space_size, u64 va, u32 size) {
    const Kind kind = Classify(page_table, address_space_size, va, size);
    (store ? g_counters.store : g_counters.load)[kind].fetch_add(1, std::memory_order_relaxed);
    // ACRE_MEMSTATS_SAMPLE_CROSS=1 also samples page-crossing accesses, which
    // a headless run produces; it checks the unwinding without a GPU.
    static const bool sample_cross = std::getenv("ACRE_MEMSTATS_SAMPLE_CROSS") != nullptr;
    if (kind == Unmapped || kind == Debug || kind == Rasterizer || (sample_cross && kind == MemoryCrossing)) {
        SampleSite(store);
    }
}

inline void ReportSites() {
    std::vector<std::pair<u64, std::pair<bool, Stack>>> top;
    {
        std::scoped_lock guard{g_sites.lock};
        for (const auto& [key, n] : g_sites.hits) top.emplace_back(n, key);
    }
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    u64 total = 0;
    for (const auto& t : top) total += t.first;
    std::fprintf(stderr, "acre-memstats-sites samples=%llu stacks=%zu every=%u\n",
                 static_cast<unsigned long long>(total), top.size(), kSampleEvery);
    for (size_t i = 0; i < top.size() && i < kSiteReportTop; ++i) {
        std::fprintf(stderr, "acre-memstats-site %llu %s", static_cast<unsigned long long>(top[i].first),
                     top[i].second.first ? "store" : "load");
        for (const uintptr_t ip : top[i].second.second) std::fprintf(stderr, " 0x%zx", static_cast<size_t>(ip));
        std::fprintf(stderr, "\n");
    }
}

// Prints and resets the counters about once a second; called from RunThread
// entries, which happen many times a second on every core.
inline void MaybeReport() {
    using namespace std::chrono;
    const long long now = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    long long last = g_last_report_ns.load(std::memory_order_relaxed);
    if (last == 0) {
        g_last_report_ns.compare_exchange_strong(last, now, std::memory_order_relaxed);
        return;
    }
    if (now - last < 1'000'000'000LL ||
        !g_last_report_ns.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        return;
    }
    char line[512];
    int n = std::snprintf(line, sizeof line, "acre-memstats dt=%.3fs", (now - last) / 1e9);
    for (unsigned k = 0; k < kKinds; ++k) {
        n += std::snprintf(line + n, sizeof line - n, " %s=%llu/%llu", kKindNames[k],
                           static_cast<unsigned long long>(g_counters.load[k].exchange(0, std::memory_order_relaxed)),
                           static_cast<unsigned long long>(g_counters.store[k].exchange(0, std::memory_order_relaxed)));
    }
    std::fprintf(stderr, "%s (load/store)\n", line);
    if (++g_sites.reports % kSiteReportSeconds == 0) ReportSites();
}

} // namespace Core::RecompMemStats
#endif
