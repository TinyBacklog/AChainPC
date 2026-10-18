// SPDX-License-Identifier: GPL-2.0-or-later
// The overlay UI (frontend/overlay/overlay_ui.h) builds plain draw data for the
// renderer: nothing when closed with the fps counter off, a well-formed list of
// vertices, indices and clipped commands otherwise.
#include <cstdio>
#include "frontend/overlay/overlay_ui.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static void CheckWellFormed(const VideoCore::OverlayDrawData& data, float width, float height) {
    CHECK(data.width == width && data.height == height);
    for (const auto index : data.indices) CHECK(index < data.vertices.size());
    for (const auto& command : data.commands) {
        CHECK(command.clip[0] >= 0 && command.clip[1] >= 0);
        CHECK(command.clip[2] <= width && command.clip[3] <= height);
        CHECK(command.index_offset + command.index_count <= data.indices.size());
        CHECK(command.vertex_offset <= data.vertices.size());
    }
}

int main() {
    OverlayUi ui;
    ui.SetDisplaySize(1280, 720);

    OverlayModel closed;
    CHECK(ui.BuildFrame(closed).empty());

    OverlayModel counter;
    counter.fps_counter = true;
    counter.fps = 57.4f;
    const auto counter_data = ui.BuildFrame(counter);
    CHECK(!counter_data.empty());
    CheckWellFormed(counter_data, 1280, 720);

    OverlayModel open;
    open.open = true;
    const auto open_data = ui.BuildFrame(open);
    CHECK(!open_data.empty());
    CheckWellFormed(open_data, 1280, 720);

    const auto& font = ui.Font();
    CHECK(font.width > 0 && font.height > 0);
    CHECK(font.rgba.size() == static_cast<size_t>(font.width) * font.height * 4);

    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("overlay ui: closed, counter and open frames are well formed\n");
    return 0;
}
