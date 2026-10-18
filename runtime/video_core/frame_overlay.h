// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_core/overlay_draw_data.h"

namespace VideoCore {

/// Something the frontend draws on top of every presented frame, at window
/// resolution. The renderer calls it on its presenting thread.
class FrameOverlay {
public:
    virtual ~FrameOverlay() = default;
    /// The overlay's font atlas; asked for once.
    virtual const OverlayFontAtlas& Font() = 0;
    /// What to draw on a frame of this size; empty for nothing.
    virtual OverlayDrawData Build(float width, float height) = 0;
};

} // namespace VideoCore
