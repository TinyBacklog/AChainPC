// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "common/logging.h"

namespace Common {

/// The last `capacity` formatted log lines, for showing in the game's overlay.
/// Any thread may add or read.
class RecentLog {
public:
    explicit RecentLog(std::size_t capacity_) : capacity{capacity_} {}

    void Add(Log::Level level, std::string line) {
        std::scoped_lock lock{mutex};
        if (lines.size() == capacity) lines.pop_front();
        lines.emplace_back(level, std::move(line));
    }

    /// Up to `max` of the newest lines at `min_level` or above, oldest first.
    std::vector<std::string> Lines(std::size_t max, Log::Level min_level) const {
        std::scoped_lock lock{mutex};
        std::vector<std::string> out;
        for (auto it = lines.rbegin(); it != lines.rend() && out.size() < max; ++it) {
            if (it->first >= min_level) out.push_back(it->second);
        }
        return {out.rbegin(), out.rend()};
    }

private:
    std::size_t capacity;
    mutable std::mutex mutex;
    std::deque<std::pair<Log::Level, std::string>> lines;
};

} // namespace Common
