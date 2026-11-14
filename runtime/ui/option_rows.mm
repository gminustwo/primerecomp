// Two-pane settings pages (see option_rows.h): rows on the left, the selected / hovered row's description on the right.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"  // (SetKeyOwner, NavCursorVisible)

#include "option_rows.h"
#include "osk.h"
#include "theme.h"

static float g_scale = 1;
static std::string g_desc_hover, g_desc_focus, g_desc_shown;  // this frame's hovered / selected row, and the last shown
static std::string g_desc_first;  // the page's first row (shown until a row is selected or hovered)

static const ImU32 kAccent = theme::u32(theme::mp_bright);  // (a selected choice: theme.h's pause-screen orange)
static bool g_next_disabled;
void opt_disable_next(void) { g_next_disabled = true; }
static int g_indent;
static bool g_focus_req, g_take_first;  // (opt_focus_first: requested / this page's first row takes it)
void opt_focus_first(bool on) { g_focus_req = on; }
static bool g_held;  // the last row's left / right has been held a while (steppers go faster)
void opt_indent(int step) { g_indent = g_indent + step < 0 ? 0 : g_indent + step; }

void opt_begin(const char *id, float scale) {
    g_scale = scale;
    g_take_first = g_focus_req;
    g_focus_req = false;
    if (g_take_first) ImGui::NavMoveRequestCancel();  // (this frame's Down goes to the first row instead)
    g_indent = 0;
    g_desc_hover.clear(), g_desc_focus.clear();
    static std::string page;  // another page: its own first row's description, not the last page's
    if (page != id) page = id, g_desc_shown.clear();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::PushID(id);
    ImGui::BeginChild("##rows", ImVec2(avail.x * 0.48f, avail.y), ImGuiChildFlags_None, ImGuiWindowFlags_NavFlattened);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 4 * scale));  // (rows close)
    g_desc_first.clear();
}

void opt_section(const char *title) {
    std::string t = title;
    for (char &c : t) c = (char)toupper((unsigned char)c);
    if (ImGui::GetCursorPosY() > ImGui::GetStyle().WindowPadding.y + 1) ImGui::Dummy(ImVec2(0, 10 * g_scale));
    ImGui::TextColored(theme::mp_text_dim, "%s", t.c_str());
    {   // (a thin line on from the heading, as the pause screens' frames run on from their titles)
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        float x0 = b.x + 12 * g_scale, x1 = a.x + ImGui::GetContentRegionAvail().x, y = (a.y + b.y) * 0.5f;
        if (x1 > x0) ImGui::GetWindowDrawList()->AddLine(ImVec2(x0, y), ImVec2(x1, y), theme::u32(theme::mp_line, 0.35f), 1.0f);
    }
}

// One row: draws it, handles A / click and left / right while selected. Returns 2 (A / click), -1 / +1 (left / right) or 0.
// arrows: ◀ value ▶ (a choice of several, or a stepper): left / right step it, and the arrows click too
// check: a check box on the right (0 unticked, 1 ticked; -1 none), the value (if any) before it
static int row(const char *label, const std::string &value, bool changed, const char *desc, bool upper = true,
               bool arrows = false, int check = -1) {
    ImGui::PushID(label);
    if (g_indent) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + g_indent * 28 * g_scale);  // (an option of the row above)
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFontSize() * 1.9f;
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));  // (the row's own border shows the selection)
    bool clicked = ImGui::InvisibleButton("##row", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    if (g_take_first) {  // (opt_focus_first)
        g_take_first = false;
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true);
        ImGui::SetScrollHereY(0);
    }
    bool off = g_next_disabled;  // (greyed out: still selectable, so its description shows, but it can't change)
    g_next_disabled = false;
    bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
    int step = clicked && !off ? 2 : 0;
    g_held = false;
    if (foc && !off) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft) ||
            ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
            step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight) ||
            ImGui::IsKeyPressed(ImGuiKey_RightArrow))
            step = 1;
        for (ImGuiKey k : {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickLeft,
                           ImGuiKey_GamepadLStickRight, ImGuiKey_LeftArrow, ImGuiKey_RightArrow})
            if (ImGui::GetKeyData(k)->DownDuration > 1.0f) g_held = true;
        // left / right belong to the row (navigation would otherwise try to move sideways)
        for (ImGuiKey k : {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_LeftArrow, ImGuiKey_RightArrow})
            ImGui::SetKeyOwner(k, ImGui::GetItemID());
    }
    if (foc) g_desc_focus = desc ? desc : "";
    if (hov) g_desc_hover = desc ? desc : "";
    if (g_desc_first.empty() && desc) g_desc_first = desc;

    // (as the game's pause screen buttons: a cut-corner box, dim orange-brown; selected, lit with a glowing outline)
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float pad = 16 * g_scale, fs = ImGui::GetFontSize(), cut = h * 0.26f;
    ImVec2 q(p.x + w, p.y + h);
    bool lit = hov || foc;
    ImVec4 fill = lit && !off ? theme::mp_fill_hot : theme::mp_fill;
    if (off) fill.w *= 0.55f;
    theme::chamfer_fill(dl, p, q, cut, theme::u32(fill));
    if (lit) theme::chamfer_glow(dl, p, q, cut, off ? theme::mp_line : theme::mp_bright, 2.0f * g_scale, g_scale, off ? 0.5f : 1);
    else theme::chamfer_line(dl, p, q, cut, theme::u32(theme::mp_line, off ? 0.25f : 0.55f), 1.2f * g_scale);
    std::string t = label;
    if (upper)
        for (char &c : t) c = (char)toupper((unsigned char)c);
    float ty = p.y + (h - fs) * 0.5f;
    ImU32 label_col = off ? IM_COL32(135, 110, 90, 255) : theme::u32(theme::mp_text);  // (greyed out: clearly dim)
    dl->AddText(ImVec2(p.x + pad, ty), label_col, t.c_str());
    float right = p.x + w - pad;  // (the value's right edge)
    if (check >= 0) {  // a check box at the right end; the value (a number, say) before it
        float s = fs * 0.95f;
        theme::check_box(dl, ImVec2(right - s * 0.5f, p.y + h * 0.5f), s, check > 0, lit, off, g_scale);
        right -= s + fs * 0.5f;
    }
    if (arrows && !value.empty()) {  // ◀ value ▶, drawn (the fonts have no arrow glyphs); a click on one steps
        float aw = fs * 0.42f, gap = fs * 0.55f, cy = ty + fs * 0.52f;
        float rx = right - aw, lx = rx - gap - ImGui::CalcTextSize(value.c_str()).x - gap - aw;
        ImU32 ac = off ? IM_COL32(135, 110, 90, 255) : lit ? theme::u32(theme::mp_bright) : theme::u32(theme::mp_line);
        dl->AddTriangleFilled(ImVec2(lx, cy), ImVec2(lx + aw, cy - aw * 0.75f), ImVec2(lx + aw, cy + aw * 0.75f), ac);
        dl->AddTriangleFilled(ImVec2(rx + aw, cy), ImVec2(rx, cy - aw * 0.75f), ImVec2(rx, cy + aw * 0.75f), ac);
        if (clicked && !off) {
            float mx = ImGui::GetIO().MousePos.x, hit = gap;
            if (mx >= lx - hit && mx <= lx + aw + hit) step = -1;
            else if (mx >= rx - hit && mx <= rx + aw + hit) step = 1;
        }
        right = rx - gap;
    }
    if (!value.empty()) {
        float vw = ImGui::CalcTextSize(value.c_str()).x, vx = right - vw;
        dl->AddText(ImVec2(vx, ty), off ? IM_COL32(135, 110, 90, 255) : changed ? IM_COL32(255, 255, 255, 255)
                                                                                : IM_COL32(236, 214, 184, 255), value.c_str());
    }
    ImGui::PopID();
    return step;
}

bool opt_toggle(const char *label, bool *v, bool def, const char *desc) {
    if (!row(label, "", *v != def, desc, true, false, *v)) return false;  // (any step flips it)
    *v = !*v;
    return true;
}

bool opt_check(const char *label, bool on, const char *value, const char *desc, bool upper) {
    return row(label, value ? value : "", false, desc, upper, false, on) == 2;
}

bool opt_choice(const char *label, int *v, const char *const *names, int n, int def, const char *desc, bool wrap) {
    int cur = *v >= 0 && *v < n ? *v : 0;
    int step = row(label, names[cur], cur != def, desc, true, true);
    if (!step) return false;
    int nv = cur + (step == 2 ? 1 : step);
    nv = wrap ? (nv + n) % n : nv < 0 ? 0 : nv >= n ? n - 1 : nv;
    if (nv == cur) return false;
    *v = nv;
    return true;
}

bool opt_slider(const char *label, float *v, float lo, float hi, float step, float def, const char *fmt, const char *desc) {
    char buf[32];
    snprintf(buf, sizeof buf, fmt, *v);
    int s = row(label, buf, fabsf(*v - def) > step * 0.01f, desc, true, true);
    if (!s) return false;
    float eps = step * 0.01f, n = roundf(*v / step) * step + (s == 2 ? step : s * step);
    if (s == 2 && n > hi + eps) n = lo;  // (A / click past the top wraps to the bottom; left / right stop at the ends)
    *v = n < lo ? lo : n > hi ? hi : n;
    return true;
}

bool opt_int(const char *label, int *v, int lo, int hi, int step, int def, const char *fmt, const char *desc,
             const char *lo_name) {
    char buf[48];
    if (lo_name && *v == lo) snprintf(buf, sizeof buf, "%s", lo_name);
    else snprintf(buf, sizeof buf, fmt, *v);
    int s = row(label, buf, *v != def, desc, true, true);
    if (!s) return false;
    int d = s == 2 ? step : s * step * (g_held && hi - lo > 40 * step ? 10 : 1);
    int n = *v + d;
    if (s == 2 && n > hi) n = lo;  // (A / click past the top wraps; left / right stop at the ends)
    n = n < lo ? lo : n > hi ? hi : n;
    if (n == *v) return false;
    *v = n;
    return true;
}

bool opt_button(const char *label, const char *desc) { return row(label, "", false, desc) == 2; }

bool opt_radio(const char *label, bool selected, const char *desc) {
    bool off = g_next_disabled;
    bool chosen = row(label, "", false, desc) == 2;
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    float r = ImGui::GetFontSize() * 0.32f, cx = b.x - 16 * g_scale - r, cy = (a.y + b.y) * 0.5f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 col = off ? IM_COL32(135, 110, 90, 255) : selected ? kAccent : theme::u32(theme::mp_line);
    dl->AddCircle(ImVec2(cx, cy), r, col, 0, 2.0f);
    if (selected) dl->AddCircleFilled(ImVec2(cx, cy), r * 0.55f, col);
    return chosen && !selected;
}

bool opt_action(const char *label, const char *value, const char *desc, bool upper) {
    return row(label, value ? value : "", false, desc, upper) == 2;
}

static ImGuiID g_text_editing;  // the text row turned into a field (one at a time)
bool opt_text_editing(const char *label) { return g_text_editing && g_text_editing == ImGui::GetID(label); }

bool opt_text(const char *label, char *buf, size_t cap, const char *hint, const char *desc) {
    ImGuiID &editing = g_text_editing;
    static int focus;        // frames left to give it the keyboard focus
    ImGuiID id = ImGui::GetID(label);
    if (editing != id) {
        if (row(label, buf[0] ? buf : (hint ? hint : ""), false, desc) == 2) editing = id, focus = 2;
        return false;
    }
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(-1);
    if (focus > 0) ImGui::SetKeyboardFocusHere(), focus--;
    bool done = ui_input_text("##text", buf, cap, ImGuiInputTextFlags_EnterReturnsTrue, hint, label);
    if (ImGui::IsItemFocused()) g_desc_focus = desc ? desc : "";
    if (done || ui_input_text_cancelled() || (focus == 0 && !ImGui::IsItemActive() && !osk_is_open())) editing = 0;
    ImGui::PopID();
    return done;
}

void opt_end(void) {
    ImGui::PopStyleVar();
    if (const char *t = getenv("LARECOMP_OPT_SCROLL")) ImGui::SetScrollY(ImGui::GetScrollMaxY() * (float)atof(t));  // (test)
    ImGui::EndChild();
    // the description: the hovered row (mouse) wins, else the selected one (controller), else the last shown
    if (!g_desc_hover.empty()) g_desc_shown = g_desc_hover;
    else if (!g_desc_focus.empty()) g_desc_shown = g_desc_focus;
    else if (g_desc_shown.empty()) g_desc_shown = g_desc_first;
    ImGui::SameLine(0, 0);
    ImVec2 p = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    float gap = 24 * g_scale;
    {   // the description in a pause-screen panel: a cut-corner frame with the dot grid behind
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 a(p.x + gap, p.y + 2 * g_scale), b(p.x + avail.x - 2 * g_scale, p.y + avail.y - 2 * g_scale);
        float cut = 22 * g_scale;
        theme::chamfer_fill(dl, a, b, cut, IM_COL32(0, 0, 0, 90));
        theme::dot_grid(dl, a, b, 22 * g_scale, theme::u32(theme::mp_line, 0.10f));
        theme::chamfer_line(dl, a, b, cut, theme::u32(theme::mp_line, 0.70f), 1.5f * g_scale);
        float in = 6 * g_scale;  // (a second, fainter line inside, as the game's frames)
        theme::chamfer_line(dl, ImVec2(a.x + in, a.y + in), ImVec2(b.x - in, b.y - in), cut - in * 0.4f,
                            theme::u32(theme::mp_line, 0.22f), 1.0f);
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + gap, p.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26 * g_scale, 18 * g_scale));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##desc", ImVec2(0, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoNav);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::SetWindowFontScale(1.3f);  // (the description reads at a glance)
    ImGui::PushTextWrapPos(0);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::mp_text);
    ImGui::TextUnformatted(g_desc_shown.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::EndChild();
    ImGui::PopID();
}
