// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include "core/arm/recomp/recomp_exclusive.h"

// Standalone fixture: production links the destructor from core/exclusive_monitor.cpp.
Core::ExclusiveMonitor::~ExclusiveMonitor() = default;
class Monitor final : public Core::ExclusiveMonitor {
public:
    template<class T> T Read(std::size_t core, VAddr address) {
        std::scoped_lock lock{mutex};
        assert(address == 0x1000);
        reservations[core] = version;
        T value{}; std::memcpy(&value, bytes.data(), sizeof(T)); return value;
    }
    template<class T> bool Write(std::size_t core, VAddr address, T value) {
        std::scoped_lock lock{mutex};
        assert(address == 0x1000);
        bool success = reservations[core] == version;
        reservations[core] = ~u64{};
        if (success) { std::memcpy(bytes.data(), &value, sizeof(T)); ++version; }
        return success;
    }
#define WIDTH(bits, type) \
    type ExclusiveRead##bits(std::size_t c, VAddr a) override { return Read<type>(c, a); } \
    bool ExclusiveWrite##bits(std::size_t c, VAddr a, type v) override { return Write(c, a, v); }
    WIDTH(8, u8) WIDTH(16, u16) WIDTH(32, u32) WIDTH(64, u64) WIDTH(128, u128)
#undef WIDTH
    void ClearExclusive(std::size_t core) override {
        std::scoped_lock lock{mutex}; reservations[core] = ~u64{};
    }
    std::mutex mutex;
    std::array<u8,16> bytes{};
    std::array<u64,2> reservations{~u64{}, ~u64{}};
    u64 version{};
};
int main() {
    Monitor monitor;
    std::array<u64,32> regs{};
    regs[31] = 0x1000; // SP addressing must not be interpreted as XZR.
    for (u32 size = 0; size < 4; ++size) {
        const u32 load = (size << 30) | 0x085ffc00U | (31 << 5);
        const u32 store = (size << 30) | 0x0801fc00U | (31 << 5);
        assert(Core::ExecuteRecompExclusive(monitor, 0, regs, load));
        regs[0] = ~u64{};
        assert(Core::ExecuteRecompExclusive(monitor, 0, regs, store));
        assert(regs[1] == 0);
        assert(Core::ExecuteRecompExclusive(monitor, 0, regs, store));
        assert(regs[1] == 1); // A store consumes its reservation.
        assert(Core::ExecuteRecompExclusive(monitor, 0, regs, load));
        const auto bits = 8u << size;
        assert(regs[0] == (bits == 64 ? ~u64{} : (u64{1} << bits)-1));
        assert(Core::ExecuteRecompExclusive(monitor, 0, regs, 0xd5033f5f));
        assert(Core::ExecuteRecompExclusive(monitor, 0, regs, store));
        assert(regs[1] == 1); // CLREX invalidates the reservation.
    }
    for (u32 size = 2; size < 4; ++size) {
        const u32 load = (size << 30) | 0x087f8000U | (2 << 10) | (31 << 5);
        const u32 store = (size << 30) | 0x08218000U | (2 << 10) | (31 << 5);
        assert(Core::ExecuteRecompExclusive(monitor, 0, regs, load));
        regs[0] = 0x123456789abcdef0ULL; regs[2] = 0xfedcba9876543210ULL;
        assert(Core::ExecuteRecompExclusive(monitor, 0, regs, store));
        assert(regs[1] == 0);
        assert(Core::ExecuteRecompExclusive(monitor, 0, regs, load));
        assert(regs[0] == (size == 2 ? 0x9abcdef0ULL : 0x123456789abcdef0ULL));
        assert(regs[2] == (size == 2 ? 0x76543210ULL : 0xfedcba9876543210ULL));
    }
    monitor.bytes.fill(0);
    const auto increment = [&](std::size_t core) {
        std::array<u64,32> r{}; r[31] = 0x1000;
        for (unsigned n = 0; n < 10000; ++n) {
            do {
                assert(Core::ExecuteRecompExclusive(monitor, core, r, 0xc85fffe0));
                ++r[0];
                assert(Core::ExecuteRecompExclusive(monitor, core, r, 0xc801ffe0));
            } while (r[1]);
        }
    };
    std::thread first{increment, 0}, second{increment, 1};
    first.join(); second.join();
    assert(monitor.Read<u64>(0, 0x1000) == 20000);
    std::cout << "Exclusive widths, pairs, SP, CLREX and 20000 contended increments passed\n";
}
