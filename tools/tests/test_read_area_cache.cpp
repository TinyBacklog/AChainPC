// SPDX-License-Identifier: GPL-2.0-or-later
// The per-core cache of rasterizer read areas (core/rasterizer_read_cache.h):
// a hit skips asking the GPU whether a CPU read needs a flush, so an entry must
// not outlive the frame it was made in, or a buffer the GPU rewrites later is
// read stale forever.
#include <cstdio>
#include "core/rasterizer_read_cache.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

int main() {
    Core::RasterizerReadAreaCache cache;
    CHECK(!cache.Contains(0x1000, 0x1010, 0));          // empty
    cache.Insert(0x1000, 0x2000, 0);
    CHECK(cache.Contains(0x1000, 0x1010, 0));
    CHECK(cache.Contains(0x1ff0, 0x2000, 0));
    CHECK(!cache.Contains(0x1ff0, 0x2008, 0));          // runs past the area
    CHECK(!cache.Contains(0x0ff8, 0x1008, 0));          // starts before it

    // Several areas at once: a read alternating between pages keeps hitting.
    for (u64 i = 0; i < Core::RasterizerReadAreaCache::Size; ++i)
        cache.Insert(0x10000 + i * 0x1000, 0x10000 + i * 0x1000 + 0x1000, 0);
    for (u64 i = 0; i < Core::RasterizerReadAreaCache::Size; ++i)
        CHECK(cache.Contains(0x10000 + i * 0x1000, 0x10000 + i * 0x1000 + 8, 0));
    // The oldest goes first.
    CHECK(!cache.Contains(0x1000, 0x1010, 0));
    cache.Insert(0x40000, 0x41000, 0);
    CHECK(!cache.Contains(0x10000, 0x10008, 0));
    CHECK(cache.Contains(0x11000, 0x11008, 0));

    // A new frame forgets everything: the GPU may have written any of it again.
    CHECK(!cache.Contains(0x11000, 0x11008, 1));
    CHECK(!cache.Contains(0x40000, 0x40008, 1));
    cache.Insert(0x40000, 0x41000, 1);
    CHECK(cache.Contains(0x40000, 0x40008, 1));

    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("read area cache: hits, eviction and per-frame reset passed\n");
    return 0;
}
