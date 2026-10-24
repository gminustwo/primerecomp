// The recomp's own UI colours - start screen (launcher.mm), in-game menu (menu.mm, option_rows.mm, controls_ui.mm):
// Metroid Prime's. The visor's cyan marks what's selected, Samus's orange the headings, on the deep blue of the
// game's pause screens.
#pragma once
#include "imgui.h"

namespace theme {
static const ImVec4 accent(0.40f, 0.87f, 1.00f, 1);       // visor cyan: selection, focus, progress
static const ImVec4 accent_dim(0.22f, 0.55f, 0.70f, 1);
static const ImVec4 heading(1.00f, 0.64f, 0.27f, 1);      // Samus orange: titles, section headings
static const ImVec4 text(0.92f, 0.96f, 1.00f, 1);
static const ImVec4 text_dim(0.56f, 0.66f, 0.74f, 1);
static const ImVec4 panel(0.03f, 0.06f, 0.09f, 0.95f);    // windows
static const ImVec4 panel_inner(0.05f, 0.09f, 0.13f, 0.88f);
static const ImVec4 frame(0.08f, 0.15f, 0.21f, 1);        // buttons, fields, tabs
static const ImVec4 frame_hot(0.11f, 0.29f, 0.38f, 1);
static const ImVec4 frame_on(0.13f, 0.38f, 0.50f, 1);
static const ImVec4 line(0.18f, 0.32f, 0.40f, 1);         // separators
static const ImVec4 good(0.45f, 0.95f, 0.62f, 1);         // "ready", done
static const ImVec4 bad(1.00f, 0.50f, 0.40f, 1);          // failures
static const ImVec4 warn(1.00f, 0.74f, 0.40f, 1);

inline ImU32 u32(const ImVec4 &c, float alpha = -1) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, alpha < 0 ? c.w : alpha));
}

// ImGui's colours in this palette (the in-game menu's style, the start screen's buttons and popups)
inline void apply(ImGuiStyle &s) {
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg] = panel;
    c[ImGuiCol_ChildBg] = panel_inner;
    c[ImGuiCol_PopupBg] = ImVec4(panel.x, panel.y, panel.z, 0.98f);
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = text_dim;
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = frame_hot;
    c[ImGuiCol_FrameBgActive] = frame_on;
    c[ImGuiCol_Button] = frame;
    c[ImGuiCol_ButtonHovered] = frame_hot;
    c[ImGuiCol_ButtonActive] = frame_on;
    c[ImGuiCol_Header] = frame_hot;
    c[ImGuiCol_HeaderHovered] = frame_on;
    c[ImGuiCol_HeaderActive] = frame_on;
    c[ImGuiCol_Tab] = frame;
    c[ImGuiCol_TabHovered] = frame_hot;
    c[ImGuiCol_TabSelected] = frame_on;
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent_dim;
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_NavCursor] = accent;
    c[ImGuiCol_Separator] = line;
    c[ImGuiCol_Border] = ImVec4(line.x, line.y, line.z, 0.6f);
    c[ImGuiCol_TableHeaderBg] = frame;
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0.02f, 0.05f, 0.62f);  // (ImGui's own: a light grey veil)
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];
}
}  // namespace theme
