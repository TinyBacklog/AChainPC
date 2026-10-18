// SPDX-License-Identifier: GPL-2.0-or-later
// astralchain.ini: the few player-facing settings, applied over the profile's
// full sdl2-config.ini.

#pragma once

#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct AcreIni {
    bool fullscreen = false;
    bool vsync = true;
    int display_index = 0;
    int volume = 100;
    /// 60fps patch (game/patches/fps60.patches). Read once at start-up.
    bool fps60 = false;
    /// Inline guest memory accesses in the recompiled code (FM2), in builds
    /// that have them. false = every access through the emulator's helpers.
    bool fastmem = true;
    /// Faults of one inline access before its function falls back to the
    /// helpers for the rest of the session; 0 = never.
    int fastmem_demote_after = 16;
    /// Sync GPU-written memory before the CPU reads it. Off by default: it costs
    /// about 10% in combat (texture cache lock on CPU reads) and a play test
    /// showed no stale readbacks without it.
    bool reactive_flushing = false;
    /// Internal resolution multiplier, 1-4.
    std::string resolution_scale = "1"; // 0.5, 0.75, 1, 1.5, 2, 3 or 4
    /// nearest, bilinear, bicubic, gaussian, scaleforce or fsr.
    std::string scaling_filter = "fsr";
    /// Opens and closes the overlay: an SDL key name, and gamepad buttons
    /// joined by '+' (see ParseChord).
    std::string overlay_key = "Insert";
    std::string overlay_pad = "l3+r3";
    /// Small fps counter in a corner, independent of the overlay.
    bool fps_counter = false;
    /// steam_controller, keyboard or custom.
    std::string preset = "keyboard";
    /// With preset = custom: input name (button_a, lstick, ...) -> suyu parameter string.
    std::map<std::string, std::string> custom;
    /// Problems found while parsing; the affected value keeps its default.
    std::vector<std::string> warnings;
};

AcreIni ParseAcreIni(const std::string& text);

/// How player 1 gets bound: optionally reset to the keyboard defaults, then the
/// listed input name -> suyu parameter string overrides.
struct ControlPlan {
    bool keyboard_base = true;
    std::vector<std::pair<std::string, std::string>> bindings;
};

/// Every preset starts from the keyboard defaults, so no binding from an earlier
/// preset or a removed custom line survives in the saved profile config.
ControlPlan PlanControls(const AcreIni& ini);

/// Player 1 bindings for a Steam Controller (Puck 28de:1304) through SDL HIDAPI,
/// as input name -> suyu parameter string.
std::span<const std::pair<std::string_view, std::string_view>> SteamControllerPreset();

/// The input names accepted in [controls]: Settings::NativeButton and
/// NativeAnalog mapping names.
std::span<const std::string_view> AcreInputNames();
