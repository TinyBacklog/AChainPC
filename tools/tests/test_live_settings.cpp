// SPDX-License-Identifier: GPL-2.0-or-later
// Settings changed from the overlay (frontend/live_settings.h): which apply at
// once, which only after a restart, and that the live ones really change.
#include <cstdio>
#include <string>
#include "common/settings.h"
#include "frontend/acre_ini.h"
#include "frontend/live_settings.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

int main() {
    CHECK(EffectOf("display", "vsync") == SettingEffect::Live);
    CHECK(EffectOf("display", "fullscreen") == SettingEffect::Live);
    CHECK(EffectOf("performance", "scaling_filter") == SettingEffect::Live);
    CHECK(EffectOf("performance", "fastmem_demote_after") == SettingEffect::Live);
    CHECK(EffectOf("audio", "volume") == SettingEffect::Live);
    CHECK(EffectOf("controls", "preset") == SettingEffect::Live);
    CHECK(EffectOf("overlay", "fps_counter") == SettingEffect::Live);
    CHECK(EffectOf("performance", "reactive_flushing") == SettingEffect::Partial);
    CHECK(EffectOf("performance", "fps60") == SettingEffect::Restart);
    CHECK(EffectOf("performance", "resolution_scale") == SettingEffect::Restart);
    CHECK(EffectOf("performance", "fastmem") == SettingEffect::Restart);
    CHECK(EffectOf("display", "display_index") == SettingEffect::Restart);

    bool fullscreen = false;
    int demote_after = -1;
    std::string preset;
    bool fps_counter = false;
    LiveTargets targets;
    targets.fullscreen = [&](bool on) { fullscreen = on; };
    targets.fastmem_demote_after = [&](int n) { demote_after = n; };
    targets.controls_preset = [&](const std::string& p) { preset = p; };
    targets.fps_counter = [&](bool on) { fps_counter = on; };

    Settings::values.vsync_mode.SetValue(Settings::VSyncMode::Fifo);
    ApplyLiveSetting({"display", "vsync", "false"}, targets);
    CHECK(Settings::values.vsync_mode.GetValue() == Settings::VSyncMode::Immediate);
    ApplyLiveSetting({"display", "vsync", "true"}, targets);
    CHECK(Settings::values.vsync_mode.GetValue() == Settings::VSyncMode::Fifo);

    ApplyLiveSetting({"performance", "scaling_filter", "bilinear"}, targets);
    CHECK(Settings::values.scaling_filter.GetValue() == Settings::ScalingFilter::Bilinear);
    ApplyLiveSetting({"audio", "volume", "40"}, targets);
    CHECK(Settings::values.volume.GetValue() == 40);
    ApplyLiveSetting({"performance", "reactive_flushing", "true"}, targets);
    CHECK(Settings::values.use_reactive_flushing.GetValue());

    ApplyLiveSetting({"display", "fullscreen", "true"}, targets);
    CHECK(fullscreen);
    ApplyLiveSetting({"performance", "fastmem_demote_after", "4"}, targets);
    CHECK(demote_after == 4);
    ApplyLiveSetting({"controls", "preset", "steam_controller"}, targets);
    CHECK(preset == "steam_controller");
    ApplyLiveSetting({"overlay", "fps_counter", "true"}, targets);
    CHECK(fps_counter);

    // Restart-only settings are not touched now.
    const auto resolution = Settings::values.resolution_setup.GetValue();
    ApplyLiveSetting({"performance", "resolution_scale", "2"}, targets);
    CHECK(Settings::values.resolution_setup.GetValue() == resolution);

    // The overlay's copy of the ini follows the changes it makes.
    AcreIni ini;
    UpdateAcreIni(ini, {"display", "vsync", "false"});
    UpdateAcreIni(ini, {"performance", "fps60", "true"});
    UpdateAcreIni(ini, {"performance", "resolution_scale", "1.5"});
    UpdateAcreIni(ini, {"performance", "fastmem_demote_after", "3"});
    UpdateAcreIni(ini, {"audio", "volume", "70"});
    UpdateAcreIni(ini, {"controls", "preset", "steam_controller"});
    UpdateAcreIni(ini, {"overlay", "fps_counter", "true"});
    CHECK(!ini.vsync && ini.fps60 && ini.resolution_scale == "1.5");
    CHECK(ini.fastmem_demote_after == 3 && ini.volume == 70);
    CHECK(ini.preset == "steam_controller" && ini.fps_counter);

    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("live settings: effects and live application passed\n");
    return 0;
}
