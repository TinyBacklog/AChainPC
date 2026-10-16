// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "common/logging/log.h"
#include "common/settings.h"
#include "common/settings_input.h"
#include "core/arm/recomp/recomp_fastmem.h"
#include "frontend/acre_ini_apply.h"
#include "frontend/sdl_config.h"
#include "input_common/main.h"

// Read by the lifted code at every patched instruction (see TranslatePatched).
extern "C" {
volatile int acre_fps60 = 0;
}

namespace {
int g_display_index = 0;

Settings::ResolutionSetup ResolutionFor(std::string_view scale) {
    if (scale == "0.5") return Settings::ResolutionSetup::Res1_2X;
    if (scale == "0.75") return Settings::ResolutionSetup::Res3_4X;
    if (scale == "1.5") return Settings::ResolutionSetup::Res3_2X;
    if (scale == "2") return Settings::ResolutionSetup::Res2X;
    if (scale == "3") return Settings::ResolutionSetup::Res3X;
    if (scale == "4") return Settings::ResolutionSetup::Res4X;
    return Settings::ResolutionSetup::Res1X;
}

Settings::ScalingFilter FilterFor(std::string_view name) {
    if (name == "nearest") return Settings::ScalingFilter::NearestNeighbor;
    if (name == "bilinear") return Settings::ScalingFilter::Bilinear;
    if (name == "bicubic") return Settings::ScalingFilter::Bicubic;
    if (name == "gaussian") return Settings::ScalingFilter::Gaussian;
    if (name == "scaleforce") return Settings::ScalingFilter::ScaleForce;
    return Settings::ScalingFilter::Fsr;
}

template <std::size_t N>
int IndexOf(const std::array<const char*, N>& names, std::string_view name) {
    for (std::size_t i = 0; i < N; ++i) {
        if (name == names[i]) return static_cast<int>(i);
    }
    return -1;
}

void Bind(Settings::PlayerInput& player, std::string_view name, const std::string& param) {
    if (const int i = IndexOf(Settings::NativeButton::mapping, name); i >= 0) {
        player.buttons[i] = param;
    } else if (const int a = IndexOf(Settings::NativeAnalog::mapping, name); a >= 0) {
        player.analogs[a] = param;
    }
}

void BindKeyboard(Settings::PlayerInput& player) {
    for (int i = 0; i < Settings::NativeButton::NumButtons; ++i) {
        player.buttons[i] = InputCommon::GenerateKeyboardParam(SdlConfig::default_buttons[i]);
    }
    for (int i = 0; i < Settings::NativeAnalog::NumAnalogs; ++i) {
        const auto& keys = SdlConfig::default_analogs[i];
        player.analogs[i] = InputCommon::GenerateAnalogParamFromKeys(
            keys[0], keys[1], keys[2], keys[3], SdlConfig::default_stick_mod[i], 0.5f);
    }
}
} // namespace

std::optional<std::filesystem::path> FindAcreIni(const std::optional<std::filesystem::path>& explicit_path,
                                                 const std::filesystem::path& exe_dir) {
    if (explicit_path) return explicit_path;
    std::vector<std::filesystem::path> candidates{exe_dir / "astralchain.ini"};
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        candidates.push_back(std::filesystem::path(xdg) / "astralchain" / "astralchain.ini");
    } else if (const char* home = std::getenv("HOME"); home && *home) {
        candidates.push_back(std::filesystem::path(home) / ".config" / "astralchain" / "astralchain.ini");
    }
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate)) return candidate;
    }
    return std::nullopt;
}

AcreIni LoadAcreIni(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        AcreIni ini;
        ini.warnings.push_back("cannot read " + path.string() + "; using defaults");
        return ini;
    }
    std::stringstream text;
    text << file.rdbuf();
    return ParseAcreIni(text.str());
}

void ApplyAcreIni(const AcreIni& ini) {
    for (const auto& warning : ini.warnings) {
        LOG_WARNING(Frontend, "astralchain.ini: {}", warning);
    }
    g_display_index = ini.display_index;
    Settings::values.vsync_mode.SetValue(ini.vsync ? Settings::VSyncMode::Fifo
                                                   : Settings::VSyncMode::Immediate);
    Settings::values.volume.SetValue(static_cast<u8>(ini.volume));
    acre_fps60 = ini.fps60 ? 1 : 0;
    acre_fm_configure(ini.fastmem ? 1 : 0, static_cast<unsigned>(ini.fastmem_demote_after));
    Settings::values.resolution_setup.SetValue(ResolutionFor(ini.resolution_scale));
    Settings::values.scaling_filter.SetValue(FilterFor(ini.scaling_filter));

    auto& player = Settings::values.players.GetValue()[0];
    player.connected = true;
    const ControlPlan plan = PlanControls(ini);
    if (plan.keyboard_base) {
        BindKeyboard(player);
    }
    for (const auto& [name, param] : plan.bindings) {
        Bind(player, name, param);
    }
    LOG_INFO(Frontend, "astralchain.ini applied: preset={}, display={}, vsync={}, volume={}, "
             "fps60={}, fastmem={}, fastmem_demote_after={}, resolution_scale={}, scaling_filter={}",
             ini.preset, ini.display_index, ini.vsync, ini.volume, ini.fps60, ini.fastmem,
             ini.fastmem_demote_after,
             ini.resolution_scale, ini.scaling_filter);
}

int AcreDisplayIndex() {
    return g_display_index;
}
