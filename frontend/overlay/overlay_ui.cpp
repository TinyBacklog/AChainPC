// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
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

void BuildPanel(const OverlayModel& model) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(std::min(560.0f, viewport->Size.x - 32), viewport->Size.y - 32),
                             ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("AstralChainRe", nullptr, flags)) {
        ImGui::Text("%.1f fps", model.fps);
    }
    ImGui::End();
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

VideoCore::OverlayDrawData OverlayUi::BuildFrame(const OverlayModel& model) {
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
    ImGui::NewFrame();
    if (model.fps_counter) {
        BuildFpsCounter(model);
    }
    if (model.open) {
        BuildPanel(model);
    }
    ImGui::Render();
    Convert(*ImGui::GetDrawData(), out);
    return out;
}
