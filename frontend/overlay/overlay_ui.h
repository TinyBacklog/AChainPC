// SPDX-License-Identifier: GPL-2.0-or-later
// The overlay's Dear ImGui side: owns an ImGui context, lays out one frame from
// an OverlayModel and turns ImGui's output into plain draw data for video_core.
// Used from one thread only (the GPU thread in the game).
#pragma once

#include "video_core/overlay_draw_data.h"

struct ImGuiContext;

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
