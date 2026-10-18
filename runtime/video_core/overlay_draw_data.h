// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <vector>
#include "common/common_types.h"

namespace VideoCore {

/// One vertex of the frontend overlay, in window pixels; rgba is packed R in
/// the low byte, as Dear ImGui produces it.
struct OverlayVertex {
    float x, y, u, v;
    u32 rgba;
};

/// A run of indices drawn with one scissor rectangle (left, top, right, bottom
/// in window pixels). Indices are relative to vertex_offset.
struct OverlayCommand {
    float clip[4];
    u32 index_offset;
    u32 index_count;
    u32 vertex_offset;
};

/// What the overlay wants drawn on top of a presented frame. Plain data, so the
/// renderer needs no UI library and can keep it past the frame that built it.
struct OverlayDrawData {
    float width = 0;
    float height = 0;
    std::vector<OverlayVertex> vertices;
    std::vector<u16> indices;
    std::vector<OverlayCommand> commands;

    bool empty() const {
        return commands.empty();
    }
};

/// The overlay's only texture: its font atlas, RGBA8.
struct OverlayFontAtlas {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgba;
};

} // namespace VideoCore
