// SPDX-License-Identifier: GPL-2.0-or-later
// Unit test of the astralchain.ini parser and the built-in control presets.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "frontend/acre_ini.h"

static int failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                            \
        }                                                                          \
    } while (0)

static bool HasWarning(const AcreIni& ini, const std::string& needle) {
    for (const auto& w : ini.warnings) {
        if (w.find(needle) != std::string::npos) return true;
    }
    return false;
}

int main() {
    {
        const AcreIni ini = ParseAcreIni("");
        CHECK(!ini.fullscreen);
        CHECK(ini.vsync);
        CHECK(ini.display_index == 0);
        CHECK(ini.volume == 100);
        CHECK(ini.preset == "keyboard");
        CHECK(ini.warnings.empty());
    }
    {
        const AcreIni ini = ParseAcreIni("[display]\nfullscreen = true\ndisplay_index = 2\nvsync = false\n"
                                         "[audio]\nvolume = 40\n[controls]\npreset = steam_controller\n");
        CHECK(ini.fullscreen);
        CHECK(!ini.vsync);
        CHECK(ini.display_index == 2);
        CHECK(ini.volume == 40);
        CHECK(ini.preset == "steam_controller");
        CHECK(ini.warnings.empty());
    }
    {
        const AcreIni ini = ParseAcreIni("[audio]\nvolume = 150\n");
        CHECK(ini.volume == 100);
        CHECK(HasWarning(ini, "volume"));
    }
    {
        const AcreIni ini = ParseAcreIni("[controls]\npreset = foo\n");
        CHECK(ini.preset == "keyboard");
        CHECK(HasWarning(ini, "foo"));
    }
    {
        const AcreIni ini = ParseAcreIni("[display]\nfulscreen = true\n");
        CHECK(HasWarning(ini, "fulscreen"));
    }
    {
        const AcreIni ini = ParseAcreIni("[display]\ndisplay_index = two\n");
        CHECK(ini.display_index == 0);
        CHECK(HasWarning(ini, "display_index"));
    }
    {
        const AcreIni ini = ParseAcreIni("[controls]\npreset = custom\nbutton_a = engine:x\n");
        CHECK(ini.preset == "custom");
        CHECK(ini.custom.count("button_a") == 1);
        CHECK(ini.custom.at("button_a") == "engine:x");
        CHECK(ini.warnings.empty());
    }
    {
        const AcreIni ini = ParseAcreIni("[controls]\nbutton_q = engine:x\n");
        CHECK(HasWarning(ini, "button_q"));
    }
    {
        const auto preset = SteamControllerPreset();
        CHECK(preset.size() == 19);
        for (const auto& [key, value] : preset) {
            CHECK(std::string(value).find("guid:03000000de2800000413000002006800") != std::string::npos);
        }
    }
    {
        // Every preset starts from the keyboard defaults, so nothing from an
        // earlier preset or a removed custom line survives.
        AcreIni custom = ParseAcreIni("[controls]\npreset = custom\nbutton_a = engine:x\n");
        const ControlPlan plan = PlanControls(custom);
        CHECK(plan.keyboard_base);
        CHECK(plan.bindings.size() == 1);
        CHECK(plan.bindings[0].first == "button_a");

        const ControlPlan pad = PlanControls(ParseAcreIni("[controls]\npreset = steam_controller\n"));
        CHECK(pad.keyboard_base);
        CHECK(pad.bindings.size() == 19);

        const ControlPlan keys = PlanControls(ParseAcreIni(""));
        CHECK(keys.keyboard_base);
        CHECK(keys.bindings.empty());
    }
    {
        // The template acre.py installs must parse cleanly.
        std::ifstream file(ACRE_INI_TEMPLATE);
        std::stringstream text;
        text << file.rdbuf();
        CHECK(!text.str().empty());
        const AcreIni ini = ParseAcreIni(text.str());
        CHECK(ini.warnings.empty());
        CHECK(ini.preset == "keyboard");
    }
    if (failures == 0) std::puts("acre_ini: ok");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
