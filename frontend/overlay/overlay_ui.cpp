// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <imgui_internal.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include "frontend/acre_ini.h"
#include "frontend/live_settings.h"
#include "frontend/overlay/overlay_ui.h"

namespace {
// Fixed texture id of the font atlas; the renderer has only this texture.
constexpr ImTextureID kFontTexture = 1;

void BuildFpsCounter(const OverlayModel& model) {
    // Drawn straight into the foreground list: no window, so it shows on its
    // first frame and costs a few quads.
    char text[32];
    std::snprintf(text, sizeof text, "%.0f fps", model.fps);
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 corner(ImGui::GetMainViewport()->Size.x - size.x - 12, 8);
    ImDrawList* list = ImGui::GetForegroundDrawList();
    list->AddRectFilled(ImVec2(corner.x - 4, corner.y - 2),
                        ImVec2(corner.x + size.x + 4, corner.y + size.y + 2), IM_COL32(0, 0, 0, 96), 3);
    list->AddText(corner, IM_COL32(255, 255, 255, 230), text);
}

void Convert(const ImDrawData& in, VideoCore::OverlayDrawData& out) {
    out.vertices.reserve(static_cast<size_t>(in.TotalVtxCount));
    out.indices.reserve(static_cast<size_t>(in.TotalIdxCount));
    for (const ImDrawList* list : in.CmdLists) {
        const u32 vertex_base = static_cast<u32>(out.vertices.size());
        const u32 index_base = static_cast<u32>(out.indices.size());
        for (const ImDrawVert& v : list->VtxBuffer) {
            out.vertices.push_back({v.pos.x, v.pos.y, v.uv.x, v.uv.y, v.col});
        }
        for (const ImDrawIdx index : list->IdxBuffer) {
            out.indices.push_back(static_cast<u16>(index));
        }
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback || cmd.ElemCount == 0) continue;
            const float left = std::clamp(cmd.ClipRect.x, 0.0f, out.width);
            const float top = std::clamp(cmd.ClipRect.y, 0.0f, out.height);
            const float right = std::clamp(cmd.ClipRect.z, 0.0f, out.width);
            const float bottom = std::clamp(cmd.ClipRect.w, 0.0f, out.height);
            if (right <= left || bottom <= top) continue;
            out.commands.push_back({{left, top, right, bottom}, index_base + cmd.IdxOffset,
                                    cmd.ElemCount, vertex_base + cmd.VtxOffset});
        }
    }
}
} // namespace

OverlayUi::OverlayUi() {
    context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    unsigned char* pixels = nullptr;
    int atlas_width = 0;
    int atlas_height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &atlas_width, &atlas_height);
    font.width = static_cast<u32>(atlas_width);
    font.height = static_cast<u32>(atlas_height);
    font.rgba.assign(pixels, pixels + static_cast<size_t>(atlas_width) * atlas_height * 4);
    io.Fonts->SetTexID(kFontTexture);
    ImGui::StyleColorsDark();
}

OverlayUi::~OverlayUi() {
    ImGui::DestroyContext(context);
}

namespace {
ImGuiKey KeyFromScancode(int scancode) {
    switch (scancode) {
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromPadButton(int button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return ImGuiKey_GamepadL3;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return ImGuiKey_GamepadR3;
    default: return ImGuiKey_None;
    }
}

void AddStick(ImGuiIO& io, ImGuiKey negative, ImGuiKey positive, float value) {
    constexpr float kDeadZone = 0.25f;
    const auto scaled = [&](float v) { return v < kDeadZone ? 0.0f : (v - kDeadZone) / (1 - kDeadZone); };
    io.AddKeyAnalogEvent(negative, value < -kDeadZone, scaled(-value));
    io.AddKeyAnalogEvent(positive, value > kDeadZone, scaled(value));
}
} // namespace

void OverlayUi::QueueInput(const OverlayInput& input) {
    ImGui::SetCurrentContext(context);
    ImGuiIO& io = ImGui::GetIO();
    switch (input.kind) {
    case OverlayInput::Kind::Key:
        if (const ImGuiKey key = KeyFromScancode(input.code); key != ImGuiKey_None) {
            io.AddKeyEvent(key, input.down);
        }
        break;
    case OverlayInput::Kind::MouseMove:
        io.AddMousePosEvent(input.x, input.y);
        break;
    case OverlayInput::Kind::MouseButton:
        io.AddMousePosEvent(input.x, input.y);
        if (input.code >= SDL_BUTTON_LEFT && input.code <= SDL_BUTTON_MIDDLE) {
            // SDL: 1 left, 2 middle, 3 right; ImGui: 0 left, 1 right, 2 middle.
            const int button = input.code == SDL_BUTTON_LEFT ? 0 : input.code == SDL_BUTTON_RIGHT ? 1 : 2;
            io.AddMouseButtonEvent(button, input.down);
        }
        break;
    case OverlayInput::Kind::MouseWheel:
        io.AddMouseWheelEvent(input.x, input.y);
        break;
    case OverlayInput::Kind::PadButton:
        if (const ImGuiKey key = KeyFromPadButton(input.code); key != ImGuiKey_None) {
            io.AddKeyEvent(key, input.down);
        }
        break;
    case OverlayInput::Kind::PadAxis:
        switch (input.code) {
        case SDL_GAMEPAD_AXIS_LEFTX: AddStick(io, ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, input.x); break;
        case SDL_GAMEPAD_AXIS_LEFTY: AddStick(io, ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, input.x); break;
        case SDL_GAMEPAD_AXIS_RIGHTX: AddStick(io, ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight, input.x); break;
        case SDL_GAMEPAD_AXIS_RIGHTY: AddStick(io, ImGuiKey_GamepadRStickUp, ImGuiKey_GamepadRStickDown, input.x); break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: io.AddKeyAnalogEvent(ImGuiKey_GamepadL2, input.x > 0.5f, input.x); break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, input.x > 0.5f, input.x); break;
        default: break;
        }
        break;
    }
}

void OverlayUi::SetDisplaySize(float width_, float height_) {
    width = width_;
    height = height_;
}

namespace {
constexpr std::array<const char*, 4> kTabNames{"Performance", "Settings", "AOT", "Log"};

const char* EffectNote(std::string_view section, std::string_view key) {
    switch (EffectOf(section, key)) {
    case SettingEffect::Live: return nullptr;
    case SettingEffect::Partial: return "full effect after restart";
    case SettingEffect::Restart: return "restart required";
    }
    return nullptr;
}

void Note(std::string_view section, std::string_view key) {
    if (const char* note = EffectNote(section, key)) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", note);
    }
}

void Change(OverlayActions* actions, const char* section, const char* key, std::string value) {
    if (actions) actions->changes.push_back({section, key, std::move(value)});
}

void Checkbox(OverlayActions* actions, const char* section, const char* key, const char* label, bool value) {
    if (ImGui::Checkbox(label, &value)) Change(actions, section, key, value ? "true" : "false");
    Note(section, key);
}

template <size_t N>
void Combo(OverlayActions* actions, const char* section, const char* key, const char* label,
           const std::array<const char*, N>& options, const std::string& value) {
    if (ImGui::BeginCombo(label, value.c_str())) {
        for (const char* option : options) {
            const bool selected = value == option;
            if (ImGui::Selectable(option, selected) && !selected) Change(actions, section, key, option);
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    Note(section, key);
}

void IntSlider(OverlayActions* actions, const char* section, const char* key, const char* label, int value,
               int min, int max) {
    if (ImGui::SliderInt(label, &value, min, max)) Change(actions, section, key, std::to_string(value));
    Note(section, key);
}

void PerformanceTab(const OverlayModel& model) {
    ImGui::Text("%.1f fps   frame %.1f ms   1%% low %.0f", model.stats.fps, model.stats.frame_ms,
                model.stats.low_1pct);
    ImGui::Text("frames over 33 ms: %.0f%%", model.stats.slow_share * 100);
    if (!model.frame_times.empty()) {
        ImGui::PlotLines("##frametimes", model.frame_times.data(), static_cast<int>(model.frame_times.size()),
                         0, "frame time, last 10 s (0-50 ms)", 0.0f, 50.0f,
                         ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetTextLineHeight() * 6));
    }
    ImGui::SeparatorText("Threads (% of one core)");
    if (model.threads.empty()) ImGui::TextDisabled("n/a");
    for (const auto& thread : model.threads) {
        ImGui::Text("%-14s %5.0f%%", thread.name.c_str(), thread.percent);
    }
}

void SettingsTab(const OverlayModel& model, OverlayActions* actions) {
    if (!model.settings) {
        ImGui::TextDisabled("n/a");
        return;
    }
    const AcreIni& ini = *model.settings;
    ImGui::SeparatorText("Display");
    Checkbox(actions, "display", "fullscreen", "Fullscreen", ini.fullscreen);
    Checkbox(actions, "display", "vsync", "VSync", ini.vsync);
    IntSlider(actions, "display", "display_index", "Monitor", ini.display_index, 0, 3);
    ImGui::SeparatorText("Performance");
    Checkbox(actions, "performance", "fps60", "60 fps", ini.fps60);
    Combo(actions, "performance", "resolution_scale", "Resolution scale",
          std::array{"0.5", "0.75", "1", "1.5", "2", "3", "4"}, ini.resolution_scale);
    Combo(actions, "performance", "scaling_filter", "Scaling filter",
          std::array{"nearest", "bilinear", "bicubic", "gaussian", "scaleforce", "fsr"}, ini.scaling_filter);
    Checkbox(actions, "performance", "reactive_flushing", "Reactive flushing", ini.reactive_flushing);
    Checkbox(actions, "performance", "fastmem", "Fastmem", ini.fastmem);
    IntSlider(actions, "performance", "fastmem_demote_after", "Fastmem demote after", ini.fastmem_demote_after, 0, 64);
    ImGui::SeparatorText("Audio");
    IntSlider(actions, "audio", "volume", "Volume", ini.volume, 0, 100);
    ImGui::SeparatorText("Controls and overlay");
    Combo(actions, "controls", "preset", "Controls", std::array{"keyboard", "steam_controller", "custom"}, ini.preset);
    Checkbox(actions, "overlay", "fps_counter", "FPS counter", ini.fps_counter);
    Checkbox(actions, "log", "enabled", "Write log to file", ini.log_enabled);
    ImGui::TextDisabled("Overlay key %s, controller %s (edit in astralchain.ini)", ini.overlay_key.c_str(),
                        ini.overlay_pad.c_str());
    ImGui::Spacing();
    if (ImGui::Button("Save to ini") && actions) actions->save = true;
    if (!model.save_message.empty()) {
        ImGui::SameLine();
        ImGui::TextUnformatted(model.save_message.c_str());
    }
}

void AotTab(const OverlayModel& model) {
    const auto& aot = model.aot;
    ImGui::SeparatorText("Blocks dispatched per core");
    if (aot.statistics) {
        ImGui::Text("%llu  %llu  %llu  %llu", static_cast<unsigned long long>(aot.blocks[0]),
                    static_cast<unsigned long long>(aot.blocks[1]), static_cast<unsigned long long>(aot.blocks[2]),
                    static_cast<unsigned long long>(aot.blocks[3]));
    } else {
        ImGui::TextDisabled("n/a (run with --recomp-stats)");
    }
    ImGui::SeparatorText("Fallbacks to the JIT");
    size_t new_roots = 0;
    for (const auto& site : aot.sites) new_roots += site.reason == "missing_block";
    ImGui::Text("%zu sites, %zu new roots", aot.sites.size(), new_roots);
    if (aot.dropped_sites) ImGui::Text("%llu more not recorded", static_cast<unsigned long long>(aot.dropped_sites));
    if (!aot.sites.empty() && ImGui::BeginTable("sites", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                                                ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 8))) {
        ImGui::TableSetupColumn("Place");
        ImGui::TableSetupColumn("Reason");
        ImGui::TableSetupColumn("Count");
        ImGui::TableHeadersRow();
        for (const auto& site : aot.sites) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s+0x%llx", site.module.c_str(), static_cast<unsigned long long>(site.offset));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(site.reason.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(site.count));
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Fastmem");
    ImGui::Text("%llu faults, %llu functions demoted", static_cast<unsigned long long>(aot.fm_faults),
                static_cast<unsigned long long>(aot.fm_demotions));
}

void LogTab(const OverlayModel& model, int& log_level) {
    static constexpr std::array<const char*, 6> levels{"Trace", "Debug", "Info", "Warning", "Error", "Critical"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    ImGui::Combo("Level", &log_level, levels.data(), static_cast<int>(levels.size()));
    if (ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        for (const auto& line : model.log_lines) ImGui::TextUnformatted(line.c_str());
        // Right stick scrolls the log.
        const float scroll = ImGui::GetKeyData(ImGuiKey_GamepadRStickDown)->AnalogValue -
                             ImGui::GetKeyData(ImGuiKey_GamepadRStickUp)->AnalogValue;
        if (scroll != 0) ImGui::SetScrollY(ImGui::GetScrollY() + scroll * ImGui::GetFontSize() * 1.5f);
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}
} // namespace

void OverlayUi::SelectTab(OverlayTab tab_) {
    tab = tab_;
    tab_requested = true;
}

VideoCore::OverlayDrawData OverlayUi::BuildFrame(const OverlayModel& model, OverlayActions* actions) {
    VideoCore::OverlayDrawData out;
    out.width = width;
    out.height = height;
    if (!model.open && !model.fps_counter) {
        return out;
    }
    ImGui::SetCurrentContext(context);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(width, height);
    io.DeltaTime = 1.0f / 60.0f;
    // Sized for 1080p and scaled up with the window, so it reads the same on a 4K screen.
    const float scale = std::max(1.0f, height / 1080.0f);
    if (scale != ui_scale) {
        ui_scale = scale;
        ImGuiStyle& style = ImGui::GetStyle();
        style = ImGuiStyle();
        ImGui::StyleColorsDark(&style);
        style.ScaleAllSizes(scale);
        style.FontScaleMain = scale;
    }
    ImGui::NewFrame();
    if (model.fps_counter) {
        BuildFpsCounter(model);
    }
    if (model.open) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1)) SelectTab(static_cast<OverlayTab>((static_cast<int>(tab) + 3) % 4));
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1)) SelectTab(static_cast<OverlayTab>((static_cast<int>(tab) + 1) % 4));
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(16 * scale, 16 * scale), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(std::min(620.0f * scale, viewport->Size.x - 32 * scale),
                                        viewport->Size.y - 32 * scale),
                                 ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.85f);
        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
        if (ImGui::Begin("AstralChainRe   (Insert / L3+R3 closes, L/R change tab)", nullptr, flags)) {
            if (ImGui::BeginTabBar("tabs")) {
                for (int i = 0; i < 4; ++i) {
                    const bool select = tab_requested && static_cast<int>(tab) == i;
                    if (ImGui::BeginTabItem(kTabNames[i], nullptr, select ? ImGuiTabItemFlags_SetSelected : 0)) {
                        if (!tab_requested) tab = static_cast<OverlayTab>(i);
                        switch (static_cast<OverlayTab>(i)) {
                        case OverlayTab::Performance: PerformanceTab(model); break;
                        case OverlayTab::Settings: SettingsTab(model, actions); break;
                        case OverlayTab::Aot: AotTab(model); break;
                        case OverlayTab::Log: LogTab(model, log_level); break;
                        }
                        ImGui::EndTabItem();
                    }
                }
                tab_requested = false;
                ImGui::EndTabBar();
            }
        }
        ImGui::End();
    }
    ImGui::Render();
    Convert(*ImGui::GetDrawData(), out);
    return out;
}
