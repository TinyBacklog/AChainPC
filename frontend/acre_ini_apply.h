// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>

#include "frontend/acre_ini.h"

/// Reads the ini named on the command line, or the first of
/// <exe dir>/astralchain.ini and $XDG_CONFIG_HOME/astralchain/astralchain.ini.
/// Returns defaults (and no path) when none exists.
std::optional<std::filesystem::path> FindAcreIni(const std::optional<std::filesystem::path>& explicit_path,
                                                 const std::filesystem::path& exe_dir);
AcreIni LoadAcreIni(const std::filesystem::path& path);

/// Writes the ini's values into Settings::values (vsync, volume, player 1 controls)
/// and remembers the display index for window creation.
void ApplyAcreIni(const AcreIni& ini);
/// Player 1's bindings from the ini's control preset (part of ApplyAcreIni).
void ApplyAcreControls(const AcreIni& ini);

/// Display chosen by the applied ini (0 = primary).
int AcreDisplayIndex();
