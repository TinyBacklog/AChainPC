// SPDX-License-Identifier: GPL-2.0-or-later
#include <system_error>
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "frontend/game_folder.h"

namespace fs = std::filesystem;

bool WriteDirectoryToHost(const FileSys::VirtualDir& source, const fs::path& destination,
                          std::string& error) {
    std::error_code ec;
    if (!source) {
        error = "nothing to copy";
        return false;
    }
    if (fs::exists(destination, ec)) {
        error = destination.string() + " already exists";
        return false;
    }
    fs::path tmp = destination;
    tmp += ".tmp";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    if (ec) {
        error = "cannot create " + tmp.string() + ": " + ec.message();
        return false;
    }

    FileSys::RealVfsFilesystem real;
    const auto target = real.CreateDirectory(tmp.generic_string(), FileSys::OpenMode::ReadWrite);
    if (!target || !FileSys::VfsRawCopyD(source, target)) {
        error = "cannot copy to " + tmp.string();
        return false;
    }
    fs::rename(tmp, destination, ec);
    if (ec) {
        error = "cannot rename " + tmp.string() + " to " + destination.string() + ": " + ec.message();
        return false;
    }
    return true;
}
