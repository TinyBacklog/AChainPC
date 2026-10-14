// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "core/frontend/emu_window.h"

/// A genuinely windowless EmuWindow implementation for `suyu-cmd --headless`.
///
/// Unlike EmuWindow_SDL2_Null (which still opens a real SDL_Window and needs
/// SDL_INIT_VIDEO / a working display driver, and only skips the GL/Vulkan
/// device underneath it), this class touches no windowing API at all - no
/// SDL, no X11/Wayland, nothing. It exists purely so the CPU/kernel/HLE stack
/// can boot and run a game for diagnostic purposes (does it get further? where
/// does it crash?) with zero visual output and no display server requirement,
/// on a headless machine with no $DISPLAY set at all.
///
/// It is deliberately NOT derived from EmuWindow_SDL2 - that base class's own
/// constructor unconditionally calls SDL_Init(SDL_INIT_VIDEO | ...), which is
/// exactly the dependency this class exists to avoid.
class EmuWindow_Headless final : public Core::Frontend::EmuWindow {
public:
    explicit EmuWindow_Headless();
    ~EmuWindow_Headless() override;

    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override;

    /// There is no window to be shown or hidden; report "not shown" since the
    /// only real caller (the Vulkan renderer) never runs when the Null
    /// backend is forced, as it always is for a headless run.
    bool IsShown() const override {
        return false;
    }
};
