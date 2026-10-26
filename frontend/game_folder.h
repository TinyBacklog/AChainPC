// SPDX-License-Identifier: GPL-2.0-or-later
// The game as plain exefs/ + romfs/ folders on disk.
#pragma once

#include <filesystem>
#include <string>
#include "core/file_sys/vfs/vfs_types.h"

// Copies `source` to `destination`. It writes `destination` + ".tmp" first (a stale one is
// deleted) and renames it at the end, so an interrupted copy never leaves a half game folder.
// Fails, with the reason in `error`, if `destination` already exists.
bool WriteDirectoryToHost(const FileSys::VirtualDir& source, const std::filesystem::path& destination,
                          std::string& error);
