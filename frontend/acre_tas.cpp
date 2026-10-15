// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>

#include "common/fs/path_util.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/core.h"
#include "frontend/acre_tas.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "input_common/drivers/tas_input.h"
#include "input_common/main.h"

namespace {

AcreTasMode g_mode = AcreTasMode::Off;
Core::System* g_system = nullptr;
std::atomic<InputCommon::TasInput::Tas*> g_tas{nullptr};
bool g_finish_announced = false; // only touched by the presenting thread

InputCommon::TasInput::TasAnalog Axis(const Core::HID::AnalogStickState& stick) {
    return {static_cast<float>(stick.x) / 32767.0f, static_cast<float>(stick.y) / 32767.0f};
}

// The profile's tas directory (--user-dir/tas). The config sets it during
// start-up, so a path chosen on the command line would not stick.
std::string TasDir() {
    return Common::FS::GetSuyuPath(Common::FS::SuyuPath::TASDir).string();
}

} // namespace

void AcreTasConfigure(AcreTasMode mode) {
    g_mode = mode;
    if (mode == AcreTasMode::Off) {
        return;
    }
    Settings::values.tas_enable.SetValue(true);
    Settings::values.tas_loop.SetValue(false);
}

void AcreTasStart(InputCommon::InputSubsystem& input, Core::System& system) {
    if (g_mode == AcreTasMode::Off) {
        return;
    }
    auto* const tas = input.GetTas();
    if (!tas) {
        // Headless runs never start the input subsystem.
        LOG_ERROR(Frontend, "tas: no input subsystem (headless?); script ignored");
        return;
    }
    g_system = &system;
    if (g_mode == AcreTasMode::Play) {
        tas->StartStop();
        const auto [state, frame, lengths] = tas->GetStatus();
        LOG_INFO(Frontend, "tas: playing {} frames from {}", lengths[0], TasDir());
    } else {
        tas->Record();
        LOG_INFO(Frontend, "tas: recording player 1 into {}", TasDir());
    }
    g_tas.store(tas, std::memory_order_release);
}

void AcreTasFrame() {
    auto* const tas = g_tas.load(std::memory_order_acquire);
    if (!tas) {
        return;
    }
    if (g_mode == AcreTasMode::Record) {
        const auto* pad = g_system->HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
        const auto sticks = pad->GetSticks();
        tas->RecordInput(static_cast<u64>(pad->GetNpadButtons().raw), Axis(sticks.left),
                         Axis(sticks.right));
    }
    tas->UpdateThread();
    if (g_mode == AcreTasMode::Play && !g_finish_announced) {
        const auto [state, frame, lengths] = tas->GetStatus();
        if (state == InputCommon::TasInput::TasState::Stopped) {
            // Benchmarks measure from this line: from here on nothing is pressed.
            LOG_INFO(Frontend, "tas: script finished");
            g_finish_announced = true;
        }
    }
}

void AcreTasFinish() {
    auto* const tas = g_tas.exchange(nullptr, std::memory_order_acq_rel);
    if (!tas || g_mode != AcreTasMode::Record) {
        return;
    }
    tas->Record(); // stops recording
    tas->SaveRecording(true);
    LOG_INFO(Frontend, "tas: recording written to {}/script0-1.txt", TasDir());
}
