// SPDX-License-Identifier: GPL-2.0-or-later
// Live frame statistics of the overlay (frontend/overlay/frame_stats.h), with
// the bench's definitions (tools/acre/bench.py): 1% low over the slowest
// max(1, n/100) intervals, and a slow frame being one over 1000/30 ms.
#include <cmath>
#include <cstdio>
#include "frontend/overlay/frame_stats.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
#define NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

constexpr u64 kMs = 1'000'000;
constexpr u64 kFrame60 = 16'666'667;

int main() {
    {
        FrameStats stats;
        CHECK(stats.Summarize(10'000 * kMs).fps == 0);       // no frames
        stats.OnFrame(5 * kMs);
        CHECK(stats.Summarize(10'000 * kMs).fps == 0);       // one frame, no interval
    }
    {
        FrameStats stats;
        for (u64 i = 0; i < 61; ++i) stats.OnFrame(i * kFrame60);
        const auto s = stats.Summarize(10'000 * kMs);
        NEAR(s.fps, 60.0, 0.1);
        NEAR(s.frame_ms, 16.667, 0.01);
        NEAR(s.low_1pct, 60.0, 0.1);
        CHECK(s.slow_share == 0);
    }
    {
        FrameStats stats;
        u64 t = 0;
        stats.OnFrame(t);
        for (int i = 0; i < 99; ++i) stats.OnFrame(t += kFrame60);
        stats.OnFrame(t += 50 * kMs);                       // 100 intervals, one of 50 ms
        const auto s = stats.Summarize(10'000 * kMs);
        NEAR(s.low_1pct, 20.0, 0.1);
        NEAR(s.slow_share, 0.01, 1e-9);
    }
    {
        FrameStats stats;
        u64 t = 0;
        stats.OnFrame(t);
        for (int i = 0; i < 50; ++i) {
            stats.OnFrame(t += kFrame60);
            stats.OnFrame(t += 33'400'000);                   // just over 1000/30 ms
        }
        NEAR(stats.Summarize(10'000 * kMs).slow_share, 0.5, 1e-9);
    }
    {
        // The window keeps only recent frames: a slow start no longer counts.
        FrameStats stats;
        u64 t = 0;
        stats.OnFrame(t);
        for (int i = 0; i < 10; ++i) stats.OnFrame(t += 100 * kMs);
        for (int i = 0; i < 600; ++i) stats.OnFrame(t += kFrame60);
        const auto s = stats.Summarize(5'000 * kMs);
        CHECK(s.slow_share == 0);
        NEAR(s.fps, 60.0, 0.1);
        const auto recent = stats.Recent(1'000 * kMs);
        CHECK(recent.size() >= 59 && recent.size() <= 61);
        for (const float ms : recent) NEAR(ms, 16.667f, 0.01f);
    }
    {
        // The ring forgets the oldest frames once full.
        FrameStats stats;
        for (u64 i = 0; i < 10'000; ++i) stats.OnFrame(i * kFrame60);
        NEAR(stats.Summarize(1'000'000 * kMs).fps, 60.0, 0.1);
    }
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("frame stats: fps, 1%% low, slow share, window and ring passed\n");
    return 0;
}
