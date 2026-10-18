// SPDX-License-Identifier: GPL-2.0-or-later
// The overlay's Dear ImGui side: owns an ImGui context, lays out one frame from
// an OverlayModel and turns ImGui's output into plain draw data for video_core.
// Used from one thread only (the GPU thread in the game).
#pragma once

#include <array>
#include <string>
#include <vector>
#include "frontend/ini_rewrite.h"
#include "frontend/overlay/frame_stats.h"
#include "frontend/overlay/thread_cpu.h"
#include "video_core/overlay_draw_data.h"

struct AcreIni;
struct ImGuiContext;

/// One input event for the overlay, in SDL terms (scancodes, gamepad buttons
/// and axes), converted to ImGui on the thread that builds the frames.
struct OverlayInput {
    /// Reset forgets every key, button and axis held: the overlay reopens clean.
    enum class Kind { Key, MouseMove, MouseButton, MouseWheel, PadButton, PadAxis, Reset };
    Kind kind = Kind::Key;
    int code = 0;      // scancode, mouse button, gamepad button or axis
    bool down = false;
    float x = 0;       // mouse position or wheel; axis value in -1..1
    float y = 0;
};

/// The AOT tab's numbers.
struct OverlayAotView {
    struct Site {
        std::string module;
        std::string reason;
        u64 offset = 0;
        u64 count = 0;
    };
    bool statistics = false;       // blocks are only counted with --recomp-stats
    std::array<u64, 4> blocks{};
    std::vector<Site> sites;
    u64 dropped_sites = 0;
    u64 fm_faults = 0;
    u64 fm_demotions = 0;
};

/// Everything the overlay shows in a frame.
struct OverlayModel {
    bool open = false;
    bool fps_counter = false;
    float fps = 0;
    FrameStats::Summary stats;
    std::vector<float> frame_times;   // ms, oldest first
    std::vector<ThreadLoad> threads;
    OverlayAotView aot;
    std::vector<std::string> log_lines;
    const AcreIni* settings = nullptr;
    std::string save_message;
};

/// What the player did in a frame.
struct OverlayActions {
    std::vector<IniChange> changes;
    bool save = false;
};

enum class OverlayTab { Performance, Settings, Aot, Log };

class OverlayUi {
public:
    OverlayUi();
    ~OverlayUi();
    OverlayUi(const OverlayUi&) = delete;
    OverlayUi& operator=(const OverlayUi&) = delete;

    void SetDisplaySize(float width, float height);
    /// Applied to the next BuildFrame.
    void QueueInput(const OverlayInput& input);
    /// Empty when the overlay is closed and the fps counter is off.
    VideoCore::OverlayDrawData BuildFrame(const OverlayModel& model, OverlayActions* actions = nullptr);
    void SelectTab(OverlayTab tab);
    /// Lowest level shown in the Log tab (Common::Log::Level as an int).
    int LogLevel() const {
        return log_level;
    }
    const VideoCore::OverlayFontAtlas& Font() const {
        return font;
    }

private:
    ImGuiContext* context = nullptr;
    VideoCore::OverlayFontAtlas font;
    float width = 0;
    float height = 0;
    float ui_scale = 0;
    OverlayTab tab = OverlayTab::Performance;
    bool tab_requested = true;
    int log_level = 2;
};
