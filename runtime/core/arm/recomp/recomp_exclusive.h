// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <span>
#include "core/arm/exclusive_monitor.h"

namespace Core {
// The host and JIT share the same monitor. No instruction cache or code generation
// is involved; this implements the architectural register/status effects only.
inline bool ExecuteRecompExclusive(ExclusiveMonitor& monitor, std::size_t core,
                                    std::span<u64, 32> registers, u32 instruction) {
        if ((instruction & 0xfffff0ffU) == 0xd503305fU) {
            monitor.ClearExclusive(core);
            return true;
        }
        const u32 size = instruction >> 30;
        const bool load = (instruction >> 22) & 1;
        const bool pair = (instruction >> 21) & 1;
        const bool ordered = (instruction >> 15) & 1;
        const u32 rs = (instruction >> 16) & 31, rt2 = (instruction >> 10) & 31;
        const u32 rn = (instruction >> 5) & 31, rt = instruction & 31;
        if ((instruction & 0x3f800000U) != 0x08000000U ||
            (pair ? size < 2 : rt2 != 31) || (load && rs != 31)) return false;
        const u64 address = registers[rn];
        const auto get = [&](u32 reg) { return reg == 31 ? u64{0} : registers[reg]; };
        const auto set = [&](u32 reg, u64 value) { if (reg != 31) registers[reg] = value; };
        if (ordered && !load) std::atomic_thread_fence(std::memory_order_seq_cst);
        if (load) {
            if (pair && size == 3) {
                const auto value = monitor.ExclusiveRead128(core, address);
                set(rt, value[0]); set(rt2, value[1]);
            } else if (pair) {
                const auto value = monitor.ExclusiveRead64(core, address);
                set(rt, static_cast<u32>(value)); set(rt2, value >> 32);
            } else {
                u64 value{};
                switch (size) {
                case 0: value = monitor.ExclusiveRead8(core, address); break;
                case 1: value = monitor.ExclusiveRead16(core, address); break;
                case 2: value = monitor.ExclusiveRead32(core, address); break;
                case 3: value = monitor.ExclusiveRead64(core, address); break;
                }
                set(rt, value);
            }
        } else {
            bool success{};
            if (pair && size == 3) {
                success = monitor.ExclusiveWrite128(core, address, {get(rt), get(rt2)});
            } else if (pair) {
                success = monitor.ExclusiveWrite64(core, address,
                    static_cast<u32>(get(rt)) | (get(rt2) << 32));
            } else {
                switch (size) {
                case 0: success = monitor.ExclusiveWrite8(core, address, static_cast<u8>(get(rt))); break;
                case 1: success = monitor.ExclusiveWrite16(core, address, static_cast<u16>(get(rt))); break;
                case 2: success = monitor.ExclusiveWrite32(core, address, static_cast<u32>(get(rt))); break;
                case 3: success = monitor.ExclusiveWrite64(core, address, get(rt)); break;
                }
            }
            set(rs, success ? 0 : 1);
        }
        if (ordered && load) std::atomic_thread_fence(std::memory_order_seq_cst);
        return true;
}
} // namespace Core
