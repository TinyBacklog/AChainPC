// SPDX-License-Identifier: GPL-2.0-or-later
// Regenerate one module from the immutable segments and roots of a Source export.
#include <iostream>
#include <iterator>
#include "core/recompiler/arm64_to_c.h"

static std::vector<uint8_t> Read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read " + path);
    return {std::istreambuf_iterator<char>{file}, {}};
}
int main(int argc, char** argv) {
    if (argc == 3 && std::string_view{argv[1]} == "--runtime-only") {
        // Only for caches with the current ABI; does not regenerate block bodies.
        const std::filesystem::path dir{argv[2]};
        std::ifstream identity{dir / "recomp_identity.json"};
        const std::string metadata{std::istreambuf_iterator<char>{identity}, {}};
        if (metadata.find("\"abi_version\": " + std::to_string(Core::RecompAbiVersion)) == std::string::npos) {
            std::cerr << "Runtime refresh requires matching ABI metadata\n";
            return 1;
        }
        const auto update = [&](const char* name, const char* content) {
            std::ifstream input{dir / name};
            const std::string old{std::istreambuf_iterator<char>{input}, {}};
            if (old == content) return true;
            std::ofstream output{dir / name};
            output << content;
            output.close();
            return static_cast<bool>(output);
        };
        return update("recomp_runtime.h", suyu::recomp::RuntimeH()) &&
               update("recomp_runtime.c", suyu::recomp::RuntimeC()) ? 0 : 1;
    }
    if (argc != 8) {
        std::cerr << "Usage: export_module NAME SEGMENT_DIR OUTPUT TEXT_BASE ENTRY ROOTS TITLE\n";
        return 1;
    }
    try {
        const auto text = Read(std::string{argv[2]} + "/text.bin");
        const auto rodata = Read(std::string{argv[2]} + "/rodata.bin");
        const auto data = Read(std::string{argv[2]} + "/data.bin");
        std::ifstream roots_file(argv[6]);
        if (!roots_file) throw std::runtime_error("Cannot read roots");
        std::vector<uint64_t> roots;
        uint64_t root;
        while (roots_file >> std::hex >> root) roots.push_back(root);
        const auto stats = suyu::recomp::EmitProject(argv[1], text.data(), text.size(),
            std::stoull(argv[4], nullptr, 0), argv[3], false,
            rodata.data(), rodata.size(), data.data(), data.size(),
            std::stoull(argv[5], nullptr, 0), argv[7], &roots);
        (void)stats;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
