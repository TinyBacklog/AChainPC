// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <charconv>
#include <list>

#include <SimpleIni.h>

#include "frontend/acre_ini.h"

namespace {

#define PAD "engine:sdl,port:0,guid:03000000de2800000413000002006800,"
constexpr std::array<std::pair<std::string_view, std::string_view>, 19> kSteamController{{
    {"button_a", PAD "button:1"},
    {"button_b", PAD "button:0"},
    {"button_x", PAD "button:3"},
    {"button_y", PAD "button:2"},
    {"button_lstick", PAD "button:7"},
    {"button_rstick", PAD "button:8"},
    {"button_l", PAD "button:9"},
    {"button_r", PAD "button:10"},
    {"button_zl", PAD "axis:4,threshold:0.5,invert:+"},
    {"button_zr", PAD "axis:5,threshold:0.5,invert:+"},
    {"button_plus", PAD "button:6"},
    {"button_minus", PAD "button:4"},
    {"button_dleft", PAD "hat:0,direction:left"},
    {"button_dup", PAD "hat:0,direction:up"},
    {"button_dright", PAD "hat:0,direction:right"},
    {"button_ddown", PAD "hat:0,direction:down"},
    {"button_home", PAD "button:5"},
    {"lstick", PAD "axis_x:0,axis_y:1,offset_x:0.000000,offset_y:0.000000,invert_x:+,invert_y:+"},
    {"rstick", PAD "axis_x:2,axis_y:3,offset_x:0.000000,offset_y:0.000000,invert_x:+,invert_y:+"},
}};
#undef PAD

// Same names as Settings::NativeButton::mapping and NativeAnalog::mapping.
constexpr std::array<std::string_view, 24> kInputNames{
    "button_a",     "button_b",     "button_x",      "button_y",     "button_lstick",
    "button_rstick", "button_l",    "button_r",      "button_zl",    "button_zr",
    "button_plus",  "button_minus", "button_dleft",  "button_dup",   "button_dright",
    "button_ddown", "button_slleft", "button_srleft", "button_home", "button_screenshot",
    "button_slright", "button_srright", "lstick",  "rstick",
};

bool ParseBool(std::string_view value, bool& out) {
    if (value == "true" || value == "1" || value == "yes" || value == "on") {
        out = true;
        return true;
    }
    if (value == "false" || value == "0" || value == "no" || value == "off") {
        out = false;
        return true;
    }
    return false;
}

bool ParseInt(std::string_view value, int& out) {
    const auto result = std::from_chars(value.data(), value.data() + value.size(), out);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

bool IsInputName(std::string_view name) {
    return std::find(kInputNames.begin(), kInputNames.end(), name) != kInputNames.end();
}

} // namespace

std::span<const std::pair<std::string_view, std::string_view>> SteamControllerPreset() {
    return kSteamController;
}

std::span<const std::string_view> AcreInputNames() {
    return kInputNames;
}

AcreIni ParseAcreIni(const std::string& text) {
    AcreIni ini;
    CSimpleIniA reader;
    reader.SetUnicode(true);
    if (reader.LoadData(text.data(), text.size()) < 0) {
        ini.warnings.push_back("astralchain.ini could not be parsed; using defaults");
        return ini;
    }
    const auto warn = [&](const std::string& section, const std::string& key, const std::string& why) {
        ini.warnings.push_back("[" + section + "] " + key + ": " + why);
    };

    std::list<CSimpleIniA::Entry> sections;
    reader.GetAllSections(sections);
    for (const auto& section_entry : sections) {
        const std::string section = section_entry.pItem;
        std::list<CSimpleIniA::Entry> keys;
        reader.GetAllKeys(section_entry.pItem, keys);
        for (const auto& key_entry : keys) {
            const std::string key = key_entry.pItem;
            const std::string value = reader.GetValue(section_entry.pItem, key_entry.pItem, "");
            if (section == "display" && key == "fullscreen") {
                if (!ParseBool(value, ini.fullscreen)) warn(section, key, "expected true or false, got '" + value + "'");
            } else if (section == "display" && key == "vsync") {
                if (!ParseBool(value, ini.vsync)) warn(section, key, "expected true or false, got '" + value + "'");
            } else if (section == "display" && key == "display_index") {
                int index = 0;
                if (!ParseInt(value, index) || index < 0) {
                    warn(section, key, "expected a monitor number from 0, got '" + value + "'");
                } else {
                    ini.display_index = index;
                }
            } else if (section == "audio" && key == "volume") {
                int volume = 0;
                if (!ParseInt(value, volume)) {
                    warn(section, key, "expected 0-100, got '" + value + "'");
                } else {
                    ini.volume = std::clamp(volume, 0, 100);
                    if (ini.volume != volume) warn(section, key, "clamped " + value + " to 0-100");
                }
            } else if (section == "controls" && key == "preset") {
                if (value == "steam_controller" || value == "keyboard" || value == "custom") {
                    ini.preset = value;
                } else {
                    warn(section, key, "unknown preset '" + value + "'; using keyboard");
                }
            } else if (section == "controls" && IsInputName(key)) {
                ini.custom[key] = value;
            } else {
                warn(section, key, "unknown setting, ignored");
            }
        }
    }
    return ini;
}
