// SPDX-License-Identifier: GPL-2.0-or-later
// A timeline of how each frame moves from the game to the screen, for finding
// out why a frame took two vsync periods instead of one. Off unless the
// ACRE_FRAME_TRACE environment variable names an output file; then every
// event becomes one "nanoseconds,event,value" line (steady clock).
#pragma once

#include <cstddef>
#include "common/common_types.h"

namespace Common::FrameTrace {

enum class Event {
    Queue,          // the game queued a finished frame; value = swap interval
    DequeueBegin,   // the game asks for a buffer to draw the next frame into
    DequeueEnd,     // ...and got one
    Tick,           // emulated vsync timer fired; value = GPU thread idle ns since the last tick
    ComposeBegin,   // vsync thread starts composing
    Acquire,        // composition took a newly queued frame
    ComposeEnd,     // composition handed the frame to the GPU thread
    VsyncSignal,    // the game's vsync event was signalled
    RenderBegin,    // GPU thread starts drawing the frame to the window
    RenderFrame,    // ...got a free presentation frame
    RenderEnd,      // ...submitted it for presentation
    PresentBegin,   // present thread starts acquiring a swapchain image
    PresentEnd,     // ...and presented it
};

bool Enabled();
void Record(Event event, s64 value = 0);
/// GPU thread time spent waiting for work, reported with the next Tick.
void AddGpuIdle(s64 ns);
/// Time a guest core had no thread to run, reported after the next Tick as
/// one "idle<core>" line per core.
void AddCoreIdle(std::size_t core, s64 ns);
s64 NowNs();

} // namespace Common::FrameTrace
