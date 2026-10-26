// SPDX-License-Identifier: GPL-2.0-or-later
// Copying a VFS directory to disk (frontend/game_folder.h) through a temporary folder.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/deconstructed_rom_directory.h"
#include "frontend/game_folder.h"

namespace fs = std::filesystem;

static int failures;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static FileSys::VirtualFile MakeFile(const std::string& name, std::vector<u8> bytes) {
    return std::make_shared<FileSys::VectorVfsFile>(std::move(bytes), name);
}

// exefs/main, exefs/main.npdm and romfs/data/a.bin, with made-up contents.
static FileSys::VirtualDir MakeTree() {
    using FileSys::VectorVfsDirectory;
    auto exefs = std::make_shared<VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{MakeFile("main", {1, 2, 3}), MakeFile("main.npdm", {4})},
        std::vector<FileSys::VirtualDir>{}, "exefs");
    auto data = std::make_shared<VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{MakeFile("a.bin", {5, 6})},
        std::vector<FileSys::VirtualDir>{}, "data");
    auto romfs = std::make_shared<VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{}, std::vector<FileSys::VirtualDir>{data}, "romfs");
    return std::make_shared<VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{}, std::vector<FileSys::VirtualDir>{exefs, romfs}, "");
}

// root/{exefs/main[, exefs/romfs.bin], romfs/[data/a.bin]} with the parents wired, as a real folder
// would have them. Returns exefs; root stays alive through its parent link.
static std::shared_ptr<FileSys::VectorVfsDirectory> MakeGame(bool romfs_bin, bool romfs_dir,
                                                             bool romfs_empty) {
    using FileSys::VectorVfsDirectory;
    auto root = std::make_shared<VectorVfsDirectory>();
    auto exefs = std::make_shared<VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{MakeFile("main", {1})}, std::vector<FileSys::VirtualDir>{},
        "exefs", root);
    if (romfs_bin) {
        exefs->AddFile(MakeFile("romfs.bin", {9}));
    }
    root->AddDirectory(exefs);
    if (romfs_dir) {
        auto romfs = std::make_shared<VectorVfsDirectory>(
            std::vector<FileSys::VirtualFile>{}, std::vector<FileSys::VirtualDir>{}, "romfs", root);
        if (!romfs_empty) {
            auto data = std::make_shared<VectorVfsDirectory>(
                std::vector<FileSys::VirtualFile>{MakeFile("a.bin", {5, 6})},
                std::vector<FileSys::VirtualDir>{}, "data", romfs);
            romfs->AddDirectory(data);
        }
        root->AddDirectory(romfs);
    }
    return exefs;
}

static std::vector<u8> ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

static bool Has(const fs::path& path, const std::vector<u8>& bytes) {
    return fs::is_regular_file(path) && ReadAll(path) == bytes;
}

static fs::path UniqueRoot() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / ("acre_test_game_folder_" + std::to_string(stamp));
}

static bool HasTmp(const fs::path& root) {
    for (const auto& entry : fs::directory_iterator(root)) {
        if (entry.path().extension() == ".tmp") {
            return true;
        }
    }
    return false;
}

int main() {
    const auto tree = MakeTree();

    {   // copies_tree
        const auto root = UniqueRoot();
        fs::create_directories(root);
        std::string error;
        CHECK(WriteDirectoryToHost(tree, root / "game", error));
        CHECK(Has(root / "game/exefs/main", {1, 2, 3}));
        CHECK(Has(root / "game/exefs/main.npdm", {4}));
        CHECK(Has(root / "game/romfs/data/a.bin", {5, 6}));
        CHECK(!HasTmp(root));
        fs::remove_all(root);
    }
    {   // refuses_existing_destination
        const auto root = UniqueRoot();
        fs::create_directories(root);
        std::string error;
        CHECK(WriteDirectoryToHost(tree, root / "game", error));
        error.clear();
        CHECK(!WriteDirectoryToHost(tree, root / "game", error));
        CHECK(!error.empty());
        CHECK(Has(root / "game/exefs/main", {1, 2, 3}));
        CHECK(Has(root / "game/romfs/data/a.bin", {5, 6}));
        fs::remove_all(root);
    }
    {   // clears_stale_tmp
        const auto root = UniqueRoot();
        fs::create_directories(root / "game.tmp");
        std::ofstream(root / "game.tmp/lixo") << "x";
        std::string error;
        CHECK(WriteDirectoryToHost(tree, root / "game", error));
        CHECK(!fs::exists(root / "game/lixo"));
        CHECK(Has(root / "game/exefs/main", {1, 2, 3}));
        CHECK(!HasTmp(root));
        fs::remove_all(root);
    }
    {   // sibling_dir_romfs
        const auto exefs = MakeGame(false, true, false);
        const auto result = Loader::FindDirectoryRomFS(exefs);
        CHECK(result != nullptr);
        const auto extracted = FileSys::ExtractRomFS(result);
        CHECK(extracted != nullptr);
        const auto data = extracted ? extracted->GetSubdirectory("data") : nullptr;
        const auto a = data ? data->GetFile("a.bin") : nullptr;
        CHECK(a != nullptr);
        CHECK(a && a->ReadAllBytes() == std::vector<u8>({5, 6}));
    }
    {   // file_beats_sibling_dir
        const auto exefs = MakeGame(true, true, false);
        CHECK(Loader::FindDirectoryRomFS(exefs) == exefs->GetFile("romfs.bin"));
    }
    {   // no_romfs
        CHECK(Loader::FindDirectoryRomFS(MakeGame(false, false, false)) == nullptr);
        CHECK(Loader::FindDirectoryRomFS(MakeGame(false, true, true)) == nullptr);
    }
    return failures != 0;
}
