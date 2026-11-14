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

// The in-game menu (menu.mm, option_rows.mm, controls_ui.mm) as the game's own pause screens: orange frames with cut
// corners, faintly glowing, over a dark panel with a dot grid; buttons a dim orange-brown, the selected one lit with a
// bright outline; text a warm white.
static const ImVec4 mp_line(1.00f, 0.50f, 0.14f, 1);        // frames
static const ImVec4 mp_bright(1.00f, 0.80f, 0.42f, 1);      // the selected button's outline, the title
static const ImVec4 mp_fill(0.30f, 0.11f, 0.03f, 0.80f);    // a button
static const ImVec4 mp_fill_hot(0.60f, 0.24f, 0.05f, 0.92f);
static const ImVec4 mp_bg(0.014f, 0.016f, 0.022f, 0.94f);   // panels
static const ImVec4 mp_text(1.00f, 0.95f, 0.84f, 1);
static const ImVec4 mp_text_dim(0.88f, 0.62f, 0.40f, 1);    // section headings, hints

// A rectangle with its top-left and bottom-right corners cut off at 45 degrees (cut: how far)
inline void chamfer_path(ImDrawList *dl, ImVec2 a, ImVec2 b, float cut) {
    dl->PathLineTo(ImVec2(a.x + cut, a.y));
    dl->PathLineTo(ImVec2(b.x, a.y));
    dl->PathLineTo(ImVec2(b.x, b.y - cut));
    dl->PathLineTo(ImVec2(b.x - cut, b.y));
    dl->PathLineTo(ImVec2(a.x, b.y));
    dl->PathLineTo(ImVec2(a.x, a.y + cut));
}
inline void chamfer_fill(ImDrawList *dl, ImVec2 a, ImVec2 b, float cut, ImU32 col) {
    chamfer_path(dl, a, b, cut);
    dl->PathFillConvex(col);
}
inline void chamfer_line(ImDrawList *dl, ImVec2 a, ImVec2 b, float cut, ImU32 col, float thick) {
    chamfer_path(dl, a, b, cut);
    dl->PathStroke(col, ImDrawFlags_Closed, thick);
}
// the outline with a glow: wider, fainter copies of it around (k: the UI's scale)
inline void chamfer_glow(ImDrawList *dl, ImVec2 a, ImVec2 b, float cut, const ImVec4 &c, float thick, float k,
                         float strength = 1) {
    for (int i = 3; i >= 1; i--) {
        float g = i * 1.6f * k;
        chamfer_line(dl, ImVec2(a.x - g, a.y - g), ImVec2(b.x + g, b.y + g), cut + g * 0.6f,
                     u32(c, 0.07f * strength), thick + i * 1.5f * k);
    }
    chamfer_line(dl, a, b, cut, u32(c), thick);
}
// the pause screens' backdrop: small dots in a grid
inline void dot_grid(ImDrawList *dl, ImVec2 a, ImVec2 b, float step, ImU32 col) {
    float r = step * 0.07f > 0.6f ? step * 0.07f : 0.6f;
    for (float y = a.y + step * 0.5f; y < b.y; y += step)
        for (float x = a.x + step * 0.5f; x < b.x; x += step) dl->AddRectFilled(ImVec2(x - r, y - r), ImVec2(x + r, y + r), col);
}
// a check box (s: its side): a cut-corner box, filled and ticked when on; lit: selected; off: greyed out
inline void check_box(ImDrawList *dl, ImVec2 c, float s, bool on, bool lit, bool off, float k) {
    ImVec2 a(c.x - s * 0.5f, c.y - s * 0.5f), b(c.x + s * 0.5f, c.y + s * 0.5f);
    float cut = s * 0.22f;
    chamfer_fill(dl, a, b, cut, IM_COL32(0, 0, 0, 150));
    ImVec4 edge = off ? ImVec4(0.45f, 0.36f, 0.30f, 1) : lit ? mp_bright : mp_line;
    chamfer_line(dl, a, b, cut, u32(edge), (lit ? 2.2f : 1.6f) * k);
    if (!on) return;
    float in = s * 0.17f;
    chamfer_fill(dl, ImVec2(a.x + in, a.y + in), ImVec2(b.x - in, b.y - in), cut * 0.6f,
                 off ? IM_COL32(110, 80, 60, 255) : u32(lit ? mp_bright : mp_line));
    ImVec2 t[3] = {ImVec2(c.x - s * 0.22f, c.y + s * 0.01f), ImVec2(c.x - s * 0.05f, c.y + s * 0.17f),
                   ImVec2(c.x + s * 0.24f, c.y - s * 0.18f)};
    dl->AddPolyline(t, 3, IM_COL32(30, 12, 2, 255), ImDrawFlags_None, 2.6f * k);
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
