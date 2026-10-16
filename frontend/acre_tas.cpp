// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>
#include <vector>

#include <stb_image_write.h>

#include "common/fs/path_util.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/frontend/framebuffer_layout.h"
#include "frontend/acre_tas.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "input_common/drivers/tas_input.h"
#include "input_common/main.h"
#include "video_core/renderer_base.h"

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

// The frame the script ends on, as <tas dir>/finish.png: runs of a benchmark
// compare these to check that the script led every run to the same place.
void ScreenshotFinish() {
    constexpr u32 width = 1280, height = 720;
    auto pixels = std::make_shared<std::vector<u8>>(width * height * 4);
    const auto path = TasDir() + "/finish.png";
    g_system->Renderer().RequestScreenshot(
        pixels->data(),
        [pixels, path](bool invert_y) {
            auto& data = *pixels;
            for (std::size_t i = 0; i < data.size(); i += 4) {
                std::swap(data[i], data[i + 2]); // BGRA to RGBA
            }
            if (invert_y) {
                const std::size_t row = width * 4;
                for (u32 y = 0; y < height / 2; ++y) {
                    std::swap_ranges(data.begin() + y * row, data.begin() + (y + 1) * row,
                                     data.begin() + (height - 1 - y) * row);
                }
            }
            if (stbi_write_png(path.c_str(), width, height, 4, data.data(), width * 4)) {
                LOG_INFO(Frontend, "tas: screenshot written to {}", path);
            } else {
                LOG_ERROR(Frontend, "tas: could not write {}", path);
            }
        },
        Layout::DefaultFrameLayout(width, height));
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
            ScreenshotFinish();
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
