// SPDX-License-Identifier: GPL-2.0-or-later
// Thread load for the overlay (frontend/overlay/thread_cpu.h): parsing of
// /proc/<pid>/task/<tid>/stat, and a live sample of this process.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <pthread.h>
#include <thread>
#include "frontend/overlay/thread_cpu.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

int main() {
    {
        std::string name;
        u64 ticks = 0;
        // The name can hold spaces and ')'; utime and stime are fields 14 and 15.
        CHECK(ParseTaskStat("123 (Vulkan W) 1) S 1 2 3 4 5 6 7 8 9 10 250 50 0 0 20 0 1 0", name, ticks));
        CHECK(name == "Vulkan W) 1");
        CHECK(ticks == 300);
        CHECK(!ParseTaskStat("garbage", name, ticks));
        CHECK(!ParseTaskStat("1 (x) S 1 2", name, ticks));
    }
    {
        // A busy thread named like a guest core shows up with a high load.
        std::atomic<bool> stop{false};
        std::thread busy([&] {
            pthread_setname_np(pthread_self(), "CPUCore_0");
            while (!stop.load(std::memory_order_relaxed)) {}
        });
        ThreadCpu cpu;
        cpu.Sample();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto loads = cpu.Sample();
        stop = true;
        busy.join();
        double core0 = -1;
        for (const auto& load : loads) {
            CHECK(load.name != "test_thread_cpu");       // only emulator threads
            if (load.name == "CPUCore_0") core0 = load.percent;
        }
        CHECK(core0 > 50 && core0 <= 110);
    }
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("thread cpu: stat parsing and live sample passed\n");
    return 0;
}
