// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include "common/common_types.h"

namespace Core {
// Configured by the frontend before loading a process. Inactive in other frontends.
void ConfigureRecompDiagnostics(bool strict, bool statistics);
bool RecompStrict();
bool RecompStatisticsEnabled();
bool RecompHasFailure();
std::string RecompFailureReason();
void RecompFail(std::string_view reason);
void RecompGuestPanic();
void RecordRecompFallback(std::size_t core, u64 pc, std::string_view reason,
                          std::string_view module, u64 offset, u32 opcode);
void RecordRecompExecution(std::size_t core, bool jit, u64 nanoseconds, u64 blocks);
// Throws on IO errors, allowing the CLI to report failure instead of losing a report silently.
void WriteRecompReport(const std::string& path, std::string_view termination,
                       std::string_view game, std::string_view user_dir, u64 program_id);

class RecompExecutionTimer {
public:
    RecompExecutionTimer(std::size_t core, bool jit)
        : enabled{RecompStatisticsEnabled()}, core_index{core}, is_jit{jit} {
        if (enabled) start = std::chrono::steady_clock::now();
    }
    ~RecompExecutionTimer() {
        if (enabled) RecordRecompExecution(core_index, is_jit,
            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count()), is_jit ? 0 : 1);
    }
private:
    bool enabled;
    std::size_t core_index;
    bool is_jit;
    std::chrono::steady_clock::time_point start{};
};
} // namespace Core
