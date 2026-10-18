// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <functional>
#include <numeric>
#include "frontend/overlay/frame_stats.h"

namespace {
constexpr double kSlowMs = 1000.0 / 30.0;
}

void FrameStats::OnFrame(u64 ns) {
    times[next] = ns;
    next = (next + 1) % kCapacity;
    count = std::min(count + 1, kCapacity);
}

std::vector<float> FrameStats::Recent(u64 window_ns) const {
    std::vector<float> intervals;
    if (count < 2) return intervals;
    const auto at = [&](std::size_t age) { return times[(next + kCapacity - 1 - age) % kCapacity]; };
    const u64 newest = at(0);
    std::size_t age = 1;
    while (age < count && newest - at(age) <= window_ns) ++age;
    // Frames at ages [0, age) are inside the window; age - 1 intervals.
    intervals.reserve(age - 1);
    for (std::size_t i = age - 1; i > 0; --i) {
        intervals.push_back(static_cast<float>((at(i - 1) - at(i)) / 1e6));
    }
    return intervals;
}

FrameStats::Summary FrameStats::Summarize(u64 window_ns) const {
    const std::vector<float> intervals = Recent(window_ns);
    Summary summary;
    if (intervals.empty()) return summary;
    const double total = std::accumulate(intervals.begin(), intervals.end(), 0.0);
    summary.frame_ms = total / intervals.size();
    summary.fps = 1000.0 / summary.frame_ms;
    std::vector<float> slowest = intervals;
    const std::size_t n = std::max<std::size_t>(1, intervals.size() / 100);
    std::partial_sort(slowest.begin(), slowest.begin() + n, slowest.end(), std::greater<>());
    summary.low_1pct = 1000.0 * n / std::accumulate(slowest.begin(), slowest.begin() + n, 0.0);
    summary.slow_share = static_cast<double>(std::count_if(intervals.begin(), intervals.end(),
                                                           [](float ms) { return ms > kSlowMs; })) /
                         intervals.size();
    return summary;
}
