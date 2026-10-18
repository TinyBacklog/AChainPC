// SPDX-License-Identifier: GPL-2.0-or-later
// Presented-frame timing for the overlay, with the bench's definitions
// (tools/acre/bench.py). Written and read on the same thread.
#pragma once

#include <array>
#include <cstddef>
#include <vector>
#include "common/common_types.h"

class FrameStats {
public:
    struct Summary {
        double fps = 0;
        double frame_ms = 0;
        /// 1000 / mean of the slowest max(1, n/100) intervals.
        double low_1pct = 0;
        /// Share of intervals over 1000/30 ms.
        double slow_share = 0;
    };

    /// Records a presented frame at `ns` (any monotonic clock).
    void OnFrame(u64 ns);
    /// Over the frames of the last `window_ns` before the newest one.
    Summary Summarize(u64 window_ns) const;
    /// Frame times in ms over the last `window_ns`, oldest first.
    std::vector<float> Recent(u64 window_ns) const;

private:
    static constexpr std::size_t kCapacity = 4096;
    std::array<u64, kCapacity> times{};
    std::size_t count = 0;
    std::size_t next = 0;
};
