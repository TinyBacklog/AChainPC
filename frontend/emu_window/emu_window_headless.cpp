// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "common/scm_rev.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/frontend/graphics_context.h"
#include "frontend/emu_window/emu_window_headless.h"

EmuWindow_Headless::EmuWindow_Headless() {
    // No real surface exists - the WindowSystemInfo comment on render_surface
    // spells out that a null surface is exactly what tells the video backend
    // to run headless. Left otherwise default (Headless is already
    // WindowSystemType's default value).
    window_info.render_surface = nullptr;

    // Nothing ever resizes this window, so seed a sane, non-zero framebuffer
    // layout once up front rather than leaving it at framebuffer_layout's
    // default-constructed (zeroed) state, in case any code path reads it
    // before/without ever presenting a frame.
    UpdateCurrentFramebufferLayout(Layout::ScreenUndocked::Width, Layout::ScreenUndocked::Height);

    LOG_INFO(Frontend, "suyu Version: {} | {}-{} (Headless)", Common::g_build_name,
             Common::g_scm_branch, Common::g_scm_desc);
}

EmuWindow_Headless::~EmuWindow_Headless() = default;

std::unique_ptr<Core::Frontend::GraphicsContext> EmuWindow_Headless::CreateSharedContext() const {
    // GraphicsContext has no pure-virtual methods - every operation on it
    // (SwapBuffers/MakeCurrent/DoneCurrent) is already a no-op by default,
    // exactly what a headless run wants. No DummyContext subclass needed.
    return std::make_unique<Core::Frontend::GraphicsContext>();
}
