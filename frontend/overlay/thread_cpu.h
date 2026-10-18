// SPDX-License-Identifier: GPL-2.0-or-later
// CPU load of the emulator's busy threads (CPUCore_N, GPU, VulkanWorker), from
// /proc/self/task, as a percentage of one core since the previous sample.
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>
#include "common/common_types.h"

struct ThreadLoad {
    std::string name;
    double percent;
};

/// Name and utime + stime (clock ticks) from one /proc/<pid>/task/<tid>/stat line.
bool ParseTaskStat(std::string_view stat, std::string& name, u64& ticks);

class ThreadCpu {
public:
    /// Empty on the first call, or when /proc cannot be read.
    std::vector<ThreadLoad> Sample();

private:
    std::map<int, u64> last_ticks;
    u64 last_ns = 0;
};
