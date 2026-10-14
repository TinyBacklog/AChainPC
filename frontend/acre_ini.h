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
