// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <charconv>
#include "common/settings.h"
#include "frontend/acre_ini.h"
#include "frontend/live_settings.h"

namespace {
bool ParseBool(std::string_view value, bool& out) {
    if (value == "true") {
        out = true;
        return true;
    }
    if (value == "false") {
        out = false;
        return true;
    }
    return false;
}

bool ParseInt(std::string_view value, int& out) {
    return std::from_chars(value.data(), value.data() + value.size(), out).ec == std::errc{};
}
} // namespace

SettingEffect EffectOf(std::string_view section, std::string_view key) {
    if (section == "performance" && key == "reactive_flushing") return SettingEffect::Partial;
    if ((section == "display" && (key == "vsync" || key == "fullscreen")) ||
        (section == "performance" && (key == "scaling_filter" || key == "fastmem_demote_after")) ||
        (section == "audio" && key == "volume") || (section == "controls" && key == "preset") ||
        (section == "overlay" && key == "fps_counter") || (section == "log" && key == "enabled")) {
        return SettingEffect::Live;
    }
    return SettingEffect::Restart;
}

void ApplyLiveSetting(const IniChange& change, const LiveTargets& targets) {
    const std::string_view section = change.section;
    const std::string_view key = change.key;
    if (EffectOf(section, key) == SettingEffect::Restart) return;
    bool flag = false;
    int number = 0;
    if (section == "display" && key == "vsync" && ParseBool(change.value, flag)) {
        Settings::values.vsync_mode.SetValue(flag ? Settings::VSyncMode::Fifo
                                                  : Settings::VSyncMode::Immediate);
    } else if (section == "display" && key == "fullscreen" && ParseBool(change.value, flag)) {
        if (targets.fullscreen) targets.fullscreen(flag);
    } else if (section == "performance" && key == "scaling_filter") {
        Settings::values.scaling_filter.SetValue(ScalingFilterFor(change.value));
    } else if (section == "performance" && key == "fastmem_demote_after" &&
               ParseInt(change.value, number) && number >= 0) {
        if (targets.fastmem_demote_after) targets.fastmem_demote_after(number);
    } else if (section == "performance" && key == "reactive_flushing" &&
               ParseBool(change.value, flag)) {
        Settings::values.use_reactive_flushing.SetValue(flag);
    } else if (section == "audio" && key == "volume" && ParseInt(change.value, number)) {
        Settings::values.volume.SetValue(static_cast<u8>(std::clamp(number, 0, 100)));
    } else if (section == "controls" && key == "preset") {
        if (targets.controls_preset) targets.controls_preset(change.value);
    } else if (section == "overlay" && key == "fps_counter" && ParseBool(change.value, flag)) {
        if (targets.fps_counter) targets.fps_counter(flag);
    } else if (section == "log" && key == "enabled" && ParseBool(change.value, flag)) {
        if (targets.log_enabled) targets.log_enabled(flag);
    }
}

void UpdateAcreIni(AcreIni& ini, const IniChange& change) {
    const std::string_view section = change.section;
    const std::string_view key = change.key;
    const std::string& value = change.value;
    int number = 0;
    if (section == "display") {
        if (key == "fullscreen") ParseBool(value, ini.fullscreen);
        else if (key == "vsync") ParseBool(value, ini.vsync);
        else if (key == "display_index" && ParseInt(value, number) && number >= 0) ini.display_index = number;
    } else if (section == "performance") {
        if (key == "fps60") ParseBool(value, ini.fps60);
        else if (key == "fastmem") ParseBool(value, ini.fastmem);
        else if (key == "reactive_flushing") ParseBool(value, ini.reactive_flushing);
        else if (key == "fastmem_demote_after" && ParseInt(value, number) && number >= 0) ini.fastmem_demote_after = number;
        else if (key == "resolution_scale") ini.resolution_scale = value;
        else if (key == "scaling_filter") ini.scaling_filter = value;
    } else if (section == "audio" && key == "volume" && ParseInt(value, number)) {
        ini.volume = std::clamp(number, 0, 100);
    } else if (section == "controls" && key == "preset") {
        ini.preset = value;
    } else if (section == "log" && key == "enabled") {
        ParseBool(value, ini.log_enabled);
    } else if (section == "overlay") {
        if (key == "fps_counter") ParseBool(value, ini.fps_counter);
        else if (key == "key") ini.overlay_key = value;
        else if (key == "pad") ini.overlay_pad = value;
    }
}

Settings::ScalingFilter ScalingFilterFor(std::string_view name) {
    if (name == "nearest") return Settings::ScalingFilter::NearestNeighbor;
    if (name == "bilinear") return Settings::ScalingFilter::Bilinear;
    if (name == "bicubic") return Settings::ScalingFilter::Bicubic;
    if (name == "gaussian") return Settings::ScalingFilter::Gaussian;
    if (name == "scaleforce") return Settings::ScalingFilter::ScaleForce;
    return Settings::ScalingFilter::Fsr;
}
