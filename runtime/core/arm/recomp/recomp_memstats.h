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

inline void Count(bool store, const void* page_table, u64 address_space_size, u64 va, u32 size) {
    auto& slot = (store ? g_counters.store : g_counters.load)[Classify(page_table, address_space_size, va, size)];
    slot.fetch_add(1, std::memory_order_relaxed);
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
}

} // namespace Core::RecompMemStats
#endif
