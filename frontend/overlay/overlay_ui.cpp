// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cstdio>
#include <imgui.h>
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
