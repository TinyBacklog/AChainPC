// SPDX-License-Identifier: GPL-2.0-or-later
// Saving astralchain.ini from the overlay (frontend/ini_rewrite.h): only the
// changed values move; comments, spacing, order and line endings stay byte for
// byte. Runs in a temporary directory, never in the player's config.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <vector>
#include "frontend/ini_rewrite.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

namespace fs = std::filesystem;

static std::string Rewrite(std::string_view text, std::vector<IniChange> changes) {
    return RewriteIni(text, changes);
}

static std::string ReadFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

int main() {
    const std::string ini =
        "; AstralChainRe settings.\n"
        "\n"
        "[display]\n"
        "; Start in fullscreen.\n"
        "fullscreen = false\n"
        "vsync=true\n"
        "\n"
        "[performance]\n"
        "# 60fps\n"
        "fps60   =   false\n"
        "vsync = keep\n"
        "\n"
        "[controls]\n"
        "preset = keyboard\n"
        "; trailing comment with no key after it\n";

    // One value changes; every other byte stays.
    CHECK(Rewrite(ini, {{"display", "fullscreen", "true"}}) ==
          "; AstralChainRe settings.\n\n[display]\n; Start in fullscreen.\nfullscreen = true\nvsync=true\n"
          "\n[performance]\n# 60fps\nfps60   =   false\nvsync = keep\n\n[controls]\npreset = keyboard\n"
          "; trailing comment with no key after it\n");
    // Spacing around '=' is kept, and the same key in another section is untouched.
    {
        const std::string out = Rewrite(ini, {{"performance", "fps60", "true"}, {"display", "vsync", "false"}});
        CHECK(out.find("fps60   =   true\n") != std::string::npos);
        CHECK(out.find("vsync=false\n") != std::string::npos);
        CHECK(out.find("vsync = keep\n") != std::string::npos);
    }
    // A missing key goes at the end of its section, before the blank line.
    CHECK(Rewrite(ini, {{"display", "display_index", "1"}}).find("vsync=true\ndisplay_index = 1\n\n[performance]") !=
          std::string::npos);
    // A missing key in the last section goes after its last line.
    CHECK(Rewrite(ini, {{"controls", "custom_a", "x"}}).ends_with("; trailing comment with no key after it\ncustom_a = x\n"));
    // A missing section is appended at the end.
    CHECK(Rewrite(ini, {{"overlay", "fps_counter", "true"}}).ends_with(
              "; trailing comment with no key after it\n\n[overlay]\nfps_counter = true\n"));
    // CRLF files stay CRLF; a file without a final newline gets none added to old lines.
    CHECK(Rewrite("[a]\r\nk = 1\r\nj = 2\r\n", {{"a", "k", "3"}}) == "[a]\r\nk = 3\r\nj = 2\r\n");
    CHECK(Rewrite("[a]\nk = 1", {{"a", "k", "2"}}) == "[a]\nk = 2");
    // Comment lines that look like keys are not keys.
    CHECK(Rewrite("[a]\n; k = 1\nk = 1\n", {{"a", "k", "2"}}) == "[a]\n; k = 1\nk = 2\n");

    const fs::path dir = fs::temp_directory_path() / ("acre-ini-test-" + std::to_string(getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::string error;
    // No file yet: created from the template, with the change applied.
    {
        const fs::path file = dir / "sub" / "astralchain.ini";
        CHECK(SaveIniChanges(file, std::vector<IniChange>{{"display", "fullscreen", "true"}}, ini, error));
        CHECK(ReadFile(file) == Rewrite(ini, {{"display", "fullscreen", "true"}}));
        CHECK(!fs::exists(file.string() + ".tmp"));
        // An existing file is rewritten in place.
        CHECK(SaveIniChanges(file, std::vector<IniChange>{{"display", "vsync", "false"}}, "", error));
        CHECK(ReadFile(file).find("vsync=false\n") != std::string::npos);
        CHECK(ReadFile(file).find("fullscreen = true\n") != std::string::npos);
    }
    // Read-only directory: false with a message, the old file intact, no .tmp left.
    if (geteuid() != 0) {
        const fs::path locked = dir / "locked";
        fs::create_directories(locked);
        const fs::path file = locked / "astralchain.ini";
        { std::ofstream(file) << ini; }
        fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_exec);
        error.clear();
        CHECK(!SaveIniChanges(file, std::vector<IniChange>{{"display", "fullscreen", "true"}}, ini, error));
        CHECK(!error.empty());
        CHECK(ReadFile(file) == ini);
        CHECK(!fs::exists(file.string() + ".tmp"));
        fs::permissions(locked, fs::perms::owner_all);
    }
    fs::remove_all(dir);

    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("ini rewrite: values, spacing, sections, line endings, create and read-only passed\n");
    return 0;
}
