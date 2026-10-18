// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include "common/common_types.h"

namespace Core {

/// Areas a CPU core recently read from GPU-tracked memory and that the GPU said
/// need nothing more. A hit skips GPU::OnCPURead, which locks the texture cache
/// that the GPU thread holds for long stretches. Games read back and forth
/// between a few pages (a memcmp of two buffers), so one remembered area per
/// core missed nearly every time.
///
/// Nothing tells the cache when the GPU writes an area again, so it only trusts
/// what it learned during the current frame: a new frame number empties it.
class RasterizerReadAreaCache {
public:
    static constexpr std::size_t Size = 8;

    /// Whether [start, end) lies inside one area remembered during `frame`.
    bool Contains(u64 start, u64 end, u64 frame) {
        if (frame != current_frame) {
            Clear(frame);
            return false;
        }
        for (const auto& area : areas) {
            if (area.start <= start && end <= area.end) {
                return true;
            }
        }
        return false;
    }

    /// Remembers [start, end) for `frame`, replacing the oldest area.
    void Insert(u64 start, u64 end, u64 frame) {
        if (frame != current_frame) {
            Clear(frame);
        }
        areas[next] = {start, end};
        next = (next + 1) % Size;
    }

private:
    struct Area {
        u64 start = 0;
        u64 end = 0;
    };

    void Clear(u64 frame) {
        areas = {};
        next = 0;
        current_frame = frame;
    }

    std::array<Area, Size> areas{};
    std::size_t next = 0;
    u64 current_frame = 0;
};

} // namespace Core
