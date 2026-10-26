// SPDX-License-Identifier: GPL-2.0-or-later
// The game as plain exefs/ + romfs/ folders on disk.
#pragma once

#include <filesystem>
#include <string>
#include "core/file_sys/vfs/vfs_types.h"

namespace Core {
class System;
}

// Copies `source` to `destination`. It writes `destination` + ".tmp" first (a stale one is
// deleted) and renames it at the end, so an interrupted copy never leaves a half game folder.
// Fails, with the reason in `error`, if `destination` already exists.
bool WriteDirectoryToHost(const FileSys::VirtualDir& source, const std::filesystem::path& destination,
                          std::string& error);

// Development only: writes the effective game of `nsp_path` (base + the update installed in the
// profile NAND) to `out`/exefs and `out`/romfs. Returns 0, or 1 with the cause logged.
int ExtractGame(Core::System& system, const std::string& nsp_path, const std::filesystem::path& out);
