// SPDX-License-Identifier: GPL-2.0-or-later
// The overlay's Dear ImGui side: owns an ImGui context, lays out one frame from
// an OverlayModel and turns ImGui's output into plain draw data for video_core.
// Used from one thread only (the GPU thread in the game).
#pragma once

#include "video_core/overlay_draw_data.h"

struct ImGuiContext;

/// One input event for the overlay, in SDL terms (scancodes, gamepad buttons
/// and axes), converted to ImGui on the thread that builds the frames.
struct OverlayInput {
    enum class Kind { Key, MouseMove, MouseButton, MouseWheel, PadButton, PadAxis };
    Kind kind = Kind::Key;
    int code = 0;      // scancode, mouse button, gamepad button or axis
    bool down = false;
    float x = 0;       // mouse position or wheel; axis value in -1..1
    float y = 0;
};

/// Everything the overlay shows in a frame.
struct OverlayModel {
    bool open = false;
    bool fps_counter = false;
    float fps = 0;
};

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
    VideoCore::OverlayDrawData BuildFrame(const OverlayModel& model);
    const VideoCore::OverlayFontAtlas& Font() const {
        return font;
    }

private:
    ImGuiContext* context = nullptr;
    VideoCore::OverlayFontAtlas font;
    float width = 0;
    float height = 0;
};
