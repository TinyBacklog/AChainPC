// SPDX-License-Identifier: GPL-2.0-or-later
// Applying an astralchain.ini setting while the game runs, from the overlay.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include "common/settings_enums.h"
#include "frontend/ini_rewrite.h"

struct AcreIni;

enum class SettingEffect {
    Live,     // applies at once
    Partial,  // applies to what is created from now on; fully after a restart
    Restart,  // only on the next start
};

SettingEffect EffectOf(std::string_view section, std::string_view key);

/// What a live setting reaches outside Settings::values.
struct LiveTargets {
    std::function<void(bool)> fullscreen;
    std::function<void(int)> fastmem_demote_after;
    std::function<void(const std::string&)> controls_preset;
    std::function<void(bool)> fps_counter;
    std::function<void(bool)> log_enabled;
};

/// Applies `change` now if its effect is Live or Partial; does nothing for
/// Restart settings or values it cannot read.
void ApplyLiveSetting(const IniChange& change, const LiveTargets& targets);

/// Brings `ini` in line with `change` (the overlay's copy of the settings).
void UpdateAcreIni(AcreIni& ini, const IniChange& change);

/// astralchain.ini scaling_filter name to the setting; fsr for anything else.
Settings::ScalingFilter ScalingFilterFor(std::string_view name);
