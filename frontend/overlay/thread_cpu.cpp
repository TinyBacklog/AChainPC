// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include "frontend/overlay/thread_cpu.h"

bool ParseTaskStat(std::string_view stat, std::string& name, u64& ticks) {
    // "<tid> (<comm>) <state> ...": the name may hold spaces and ')', so it
    // ends at the last ')'.
    const size_t open = stat.find('(');
    const size_t close = stat.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open) return false;
    name = std::string(stat.substr(open + 1, close - open - 1));
    // Fields after the name start at field 3 (state); utime is 14, stime 15.
    std::string_view rest = stat.substr(close + 1);
    u64 utime = 0;
    u64 stime = 0;
    for (int field = 3; field <= 15; ++field) {
        while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
        if (rest.empty()) return false;
        const size_t end = std::min(rest.find(' '), rest.size());
        if (field == 14 || field == 15) {
            u64 value = 0;
            if (std::from_chars(rest.data(), rest.data() + end, value).ec != std::errc{}) return false;
            (field == 14 ? utime : stime) = value;
        }
        rest.remove_prefix(end);
    }
    ticks = utime + stime;
    return true;
}

namespace {
bool Wanted(std::string_view name) {
    return name.starts_with("CPUCore_") || name == "GPU" || name == "VulkanWorker";
}
} // namespace

std::vector<ThreadLoad> ThreadCpu::Sample() {
    const u64 now = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    static const double ticks_per_second = static_cast<double>(sysconf(_SC_CLK_TCK));
    std::map<int, u64> ticks_now;
    std::vector<ThreadLoad> loads;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task", ec)) {
        int tid = 0;
        const std::string leaf = entry.path().filename().string();
        if (std::from_chars(leaf.data(), leaf.data() + leaf.size(), tid).ec != std::errc{}) continue;
        std::ifstream file(entry.path() / "stat");
        std::stringstream text;
        text << file.rdbuf();
        std::string name;
        u64 ticks = 0;
        if (!ParseTaskStat(text.str(), name, ticks) || !Wanted(name)) continue;
        ticks_now[tid] = ticks;
        const auto last = last_ticks.find(tid);
        if (last_ns != 0 && last != last_ticks.end() && now > last_ns) {
            const double seconds = (now - last_ns) / 1e9;
            loads.push_back({name, 100.0 * (ticks - last->second) / ticks_per_second / seconds});
        }
    }
    last_ticks = std::move(ticks_now);
    last_ns = now;
    std::sort(loads.begin(), loads.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return loads;
}
