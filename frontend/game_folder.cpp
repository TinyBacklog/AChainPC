// SPDX-License-Identifier: GPL-2.0-or-later
#include <memory>
#include <system_error>
#include "common/logging/log.h"
#include "core/core.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/submission_package.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
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
    // Big blocks: the default 4 KiB makes a multi-GB romfs crawl.
    constexpr std::size_t copy_block = 1024 * 1024;
    if (!target || !FileSys::VfsRawCopyD(source, target, copy_block)) {
        error = "cannot copy to " + tmp.string();
        fs::remove_all(tmp, ec);
        return false;
    }
    fs::rename(tmp, destination, ec);
    if (ec) {
        error = "cannot rename " + tmp.string() + " to " + destination.string() + ": " + ec.message();
        fs::remove_all(tmp, ec);
        return false;
    }
    return true;
}

int ExtractGame(Core::System& system, const std::string& nsp_path, const fs::path& out) {
    std::error_code ec;
    if (!fs::is_regular_file(nsp_path, ec)) {
        LOG_CRITICAL(Frontend, "Cannot extract '{}': no such file", nsp_path);
        return 1;
    }
    // The same setup as --install, so PatchManager finds the update in the profile NAND.
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    system.GetFileSystemController().CreateFactories(*system.GetFilesystem());

    const auto file = system.GetFilesystem()->OpenFile(nsp_path, FileSys::OpenMode::Read);
    if (!file) {
        LOG_CRITICAL(Frontend, "Cannot extract '{}': cannot open it", nsp_path);
        return 1;
    }
    const FileSys::NSP nsp{file};
    if (nsp.GetStatus() != Loader::ResultStatus::Success) {
        LOG_CRITICAL(Frontend, "Cannot extract '{}': {} (missing or wrong keys?)", nsp_path,
                     Loader::GetResultStatusString(nsp.GetStatus()));
        return 1;
    }
    const u64 title_id = nsp.GetProgramTitleID();
    const auto nca = nsp.GetNCA(title_id, FileSys::ContentRecordType::Program);
    if (!nca || nca->GetStatus() != Loader::ResultStatus::Success) {
        LOG_CRITICAL(Frontend, "Cannot extract '{}': no readable Program NCA for {:016X}"
                               " (missing or wrong keys?)", nsp_path, title_id);
        return 1;
    }

    const FileSys::PatchManager patches{title_id, system.GetFileSystemController(),
                                        system.GetContentProvider()};
    const auto exefs = patches.PatchExeFS(nca->GetExeFS());
    const auto romfs = FileSys::ExtractRomFS(
        patches.PatchRomFS(nca.get(), nca->GetRomFS(), FileSys::ContentRecordType::Program));
    if (!exefs || !romfs) {
        LOG_CRITICAL(Frontend, "Cannot extract '{}': no {} in the Program NCA", nsp_path,
                     exefs ? "romfs" : "exefs");
        return 1;
    }

    // The patched folders keep their own names; the game folder needs exactly these two.
    const auto game = std::make_shared<FileSys::VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{},
        std::vector<FileSys::VirtualDir>{
            std::make_shared<FileSys::VectorVfsDirectory>(exefs->GetFiles(),
                                                          exefs->GetSubdirectories(), "exefs"),
            std::make_shared<FileSys::VectorVfsDirectory>(romfs->GetFiles(),
                                                          romfs->GetSubdirectories(), "romfs"),
        });
    std::string error;
    if (!WriteDirectoryToHost(game, out, error)) {
        LOG_CRITICAL(Frontend, "Cannot extract '{}': {}", nsp_path, error);
        return 1;
    }
    LOG_INFO(Frontend, "Extracted {:016X} to '{}'", title_id, out.string());
    return 0;
}
