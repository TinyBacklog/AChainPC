// SPDX-License-Identifier: GPL-2.0-or-later
// Writes settings back into astralchain.ini without disturbing it: only the
// values of changed keys move, and comments, spacing, order and line endings
// stay as the player left them.
#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

struct IniChange {
    std::string section;
    std::string key;
    std::string value;
};

/// `text` with each change applied. An existing `key = value` line in the
/// section keeps everything but its value; a missing key goes after the
/// section's last setting or comment; a missing section is appended.
std::string RewriteIni(std::string_view text, std::span<const IniChange> changes);

/// Applies `changes` to `file`, or to `template_text` when the file does not
/// exist yet, through a temporary file renamed over it. Creates the directory.
/// On failure returns false with `error` set and leaves the old file intact.
bool SaveIniChanges(const std::filesystem::path& file, std::span<const IniChange> changes,
                    std::string_view template_text, std::string& error);

/// Where Save writes: the ini found at start-up, or the per-user one
/// ($XDG_CONFIG_HOME/astralchain/astralchain.ini, else ~/.config/...).
std::filesystem::path AcreIniSavePath(const std::optional<std::filesystem::path>& found);
