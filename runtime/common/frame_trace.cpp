// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include "common/frame_trace.h"

namespace Common::FrameTrace {
namespace {
constexpr const char* names[] = {
    "queue",  "dequeue0", "dequeue1", "tick",   "compose0", "acquire", "compose1",
    "vsync",  "render0",  "rframe",   "render1", "present0", "present1",
};

struct Output {
    std::FILE* file = nullptr;
    std::mutex mutex;
    std::atomic<s64> gpu_idle{0};
    std::atomic<s64> core_idle[4]{};

    Output() {
        if (const char* path = std::getenv("ACRE_FRAME_TRACE"); path && *path) {
            file = std::fopen(path, "w");
        }
    }
};

Output& Get() {
    static Output output;
    return output;
}
} // namespace

bool Enabled() {
    return Get().file != nullptr;
}

s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void AddGpuIdle(s64 ns) {
    Get().gpu_idle.fetch_add(ns, std::memory_order_relaxed);
}

void AddCoreIdle(std::size_t core, s64 ns) {
    if (core < 4) Get().core_idle[core].fetch_add(ns, std::memory_order_relaxed);
}

void Record(Event event, s64 value) {
    auto& output = Get();
    if (!output.file) return;
    const s64 now = NowNs();
    if (event == Event::Tick) value = output.gpu_idle.exchange(0, std::memory_order_relaxed);
    std::scoped_lock lock{output.mutex};
    std::fprintf(output.file, "%lld,%s,%lld\n", static_cast<long long>(now),
                 names[static_cast<int>(event)], static_cast<long long>(value));
    // The game is usually ended by a signal; flush once per frame so the
    // trace survives it.
    if (event == Event::Tick) {
        for (int core = 0; core < 4; ++core) {
            std::fprintf(output.file, "%lld,idle%d,%lld\n", static_cast<long long>(now), core,
                         static_cast<long long>(
                             output.core_idle[core].exchange(0, std::memory_order_relaxed)));
        }
        std::fflush(output.file);
    }
}

} // namespace Common::FrameTrace
