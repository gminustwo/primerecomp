// The Achievements tab (F1 menu; the start screen's Settings) and the popup when one is earned, in the pause screens'
// style (theme.h) - runtime/achievements.h. The popup's pictures and its timing started from kryptalivian's pull
// request #100.
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <chrono>
#include <string>

#include "imgui.h"
#include "option_rows.h"
#include "theme.h"

#include "../achievements.h"
extern "C" {
#include "../settings.h"
}

// ------------------------------------------------------------------ the pictures
// A small line drawing for each kind (r: its radius)
static void badge(ImDrawList *dl, int kind, ImVec2 c, float r, ImU32 ink) {
    auto P = [&](float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); };
    float t = fmaxf(1.0f, r * 0.13f);
    auto L = [&](float x0, float y0, float x1, float y1) { dl->AddLine(P(x0, y0), P(x1, y1), ink, t); };
    switch (kind) {
    case ACH_BADGE_BOSS:  // a skull-ish mask
        dl->AddCircle(P(0, -0.1f), r * 0.72f, ink, 24, t);
        dl->AddCircleFilled(P(-0.28f, -0.15f), r * 0.16f, ink), dl->AddCircleFilled(P(0.28f, -0.15f), r * 0.16f, ink);
        L(-0.3f, 0.62f, -0.3f, 0.9f), L(0, 0.62f, 0, 0.9f), L(0.3f, 0.62f, 0.3f, 0.9f);
        break;
    case ACH_BADGE_BEAM:  // a shot with its trail
        dl->AddCircle(P(0.35f, -0.35f), r * 0.32f, ink, 16, t);
        L(-0.85f, 0.85f, 0.12f, -0.12f), L(-0.85f, 0.45f, -0.25f, -0.15f), L(-0.45f, 0.85f, 0.15f, 0.25f);
        break;
    case ACH_BADGE_GEAR:  // the Morph Ball
        dl->AddCircle(c, r * 0.8f, ink, 24, t);
        L(-0.8f, 0, 0.8f, 0), L(0, -0.8f, 0, 0.8f);
        break;
    case ACH_BADGE_SUIT:  // a helmet's visor
        dl->AddCircle(P(0, 0.05f), r * 0.8f, ink, 24, t);
        dl->AddRect(P(-0.55f, -0.25f), P(0.55f, 0.25f), ink, r * 0.2f, 0, t);
        break;
    case ACH_BADGE_MISSILE:
        dl->AddTriangle(P(0.85f, -0.85f), P(0.15f, -0.45f), P(0.45f, -0.15f), ink, t);
        L(0.3f, -0.3f, -0.6f, 0.6f), L(-0.6f, 0.6f, -0.85f, 0.35f), L(-0.6f, 0.6f, -0.35f, 0.85f);
        break;
    case ACH_BADGE_BOMB:
        dl->AddCircle(P(-0.1f, 0.15f), r * 0.62f, ink, 20, t);
        L(0.3f, -0.3f, 0.6f, -0.65f), L(0.6f, -0.65f, 0.85f, -0.6f);
        break;
    case ACH_BADGE_ARTIFACT: {  // a Chozo Artifact's diamond
        ImVec2 q[4] = {P(0, -0.9f), P(0.62f, 0), P(0, 0.9f), P(-0.62f, 0)};
        dl->AddPolyline(q, 4, ink, ImDrawFlags_Closed, t);
        L(-0.62f, 0, 0.62f, 0);
        break;
    }
    case ACH_BADGE_ESCAPE:  // Samus's gunship from above
        dl->AddTriangle(P(0, -0.85f), P(0.75f, 0.6f), P(-0.75f, 0.6f), ink, t);
        L(0, -0.3f, 0, 0.6f);
        break;
    case ACH_BADGE_MAP: {  // a folded map
        ImVec2 q[6] = {P(-0.85f, -0.6f), P(-0.3f, -0.8f), P(0.3f, -0.6f), P(0.85f, -0.8f), P(0.85f, 0.6f), P(-0.85f, 0.8f)};
        dl->AddPolyline(q, 6, ink, ImDrawFlags_Closed, t);
        L(-0.3f, -0.8f, -0.3f, 0.65f), L(0.3f, -0.6f, 0.3f, 0.7f);
        break;
    }
    case ACH_BADGE_SCAN:  // the Scan Visor's brackets around a dot
        L(-0.85f, -0.4f, -0.85f, -0.85f), L(-0.85f, -0.85f, -0.4f, -0.85f), L(0.85f, -0.4f, 0.85f, -0.85f),
            L(0.85f, -0.85f, 0.4f, -0.85f), L(-0.85f, 0.4f, -0.85f, 0.85f), L(-0.85f, 0.85f, -0.4f, 0.85f),
            L(0.85f, 0.4f, 0.85f, 0.85f), L(0.85f, 0.85f, 0.4f, 0.85f);
        dl->AddCircleFilled(c, r * 0.22f, ink);
        break;
    case ACH_BADGE_TIME:
        dl->AddCircle(c, r * 0.82f, ink, 24, t);
        L(0, 0, 0, -0.55f), L(0, 0, 0.42f, 0.22f);
        break;
    }
}

// ------------------------------------------------------------------ the tab
static std::string when_text(int64_t t) {
    time_t tt = (time_t)t;
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    char buf[64];
    strftime(buf, sizeof buf, "%B %d, %Y", &tmv);
    return buf;
}

void achievements_tab(float scale) {
    opt_begin("##achievements", scale);
    int n = ach_count(), earned = 0;
    for (int i = 0; i < n; i++) earned += ach_earned(i) != 0;
    char sum[48];
    snprintf(sum, sizeof sum, "%d of %d", earned, n);
    std::string intro =
        "Metroid Prime's milestones, earned as they happen in play: its bosses, upgrades, maps and rooms, the Log Book's "
        "scans, Hard mode, and a few challenges. They follow RetroAchievements' Metroid Prime set for the GameCube.\n\n"
        "What a save already holds when you load it doesn't count: from there on, each one is earned as it happens. "
        "Cheats don't stop them.\n\nThey're kept in achievements.txt, next to the save folder, apart from your saves.";
    if (ach_file_error()) intro += "\n\nachievements.txt couldn't be read or written: new ones are shown but may not be kept.";
    if (opt_toggle("Achievements", &g_settings.achievements, false,
                   "Earn achievements as you play, each with a popup at the bottom of the screen as it's earned.\n\n"
                   "Off: none are earned and nothing pops up over the game. The ones already earned stay in the list.\n\n"
                   "Turned on in the middle of a game, they count from that moment on, as when a save is loaded."))
        settings_save();
    opt_action("Earned", sum, intro.c_str());
    static int filter = 0;
    static const char *const shows[] = {"All", "Earned", "Not yet"};
    opt_choice("Show", &filter, shows, 3, 0, "Which achievements the list shows: all of them, the ones you've earned, "
               "or the ones still to earn.", true);
    int shown = 0;
    for (int g = 0; g < ACH_GROUPS; g++) {
        bool any = false;
        for (int i = 0; i < n; i++) {
            const AchInfo *a = ach_info(i);
            int64_t when = ach_earned(i);
            if (a->group != g || (filter == 1 && !when) || (filter == 2 && when)) continue;
            if (!any) opt_section(ach_group_name(g)), any = true;
            shown++;
            int have = 0, need = 0;
            bool pct = false, counted = ach_progress(i, &have, &need, &pct);
            char value[32] = "";
            std::string desc = a->desc;
            if (when) {
                desc += "\n\nEarned " + when_text(when) + ".";
            } else {
                desc += "\n\nNot earned yet.";
                if (counted) {
                    if (pct) snprintf(value, sizeof value, "%d%%", have);
                    else snprintf(value, sizeof value, "%d / %d", have, need);
                    char so[64];
                    if (pct) snprintf(so, sizeof so, " So far: %d%%.", have);
                    else snprintf(so, sizeof so, " So far: %d of %d.", have, need);
                    desc += so;
                }
            }
            opt_check(a->title, when != 0, value, desc.c_str());
        }
    }
    if (!shown) {
        opt_disable_next();
        opt_action(filter == 1 ? "None earned yet" : "All earned", "", filter == 1 ? "Play on: each one shows up here "
                   "as it's earned." : "Every achievement is earned.");
    }
    opt_end();
}

// ------------------------------------------------------------------ the popup
static int g_pop = -1;     // the one shown (-1: none)
static double g_pop_t0;    // since when
static const double kPopSeconds = 6.0;

static double seconds(void) {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Is one showing (or waiting, and can show now: shown: the game is in view, not the menu)?
bool achievements_popup_active(bool shown) {
    double now = seconds();
    if (g_pop >= 0 && now - g_pop_t0 > kPopSeconds) g_pop = -1;
    if (!g_settings.achievements) {  // (off: nothing over the game, and none left waiting for later)
        g_pop = -1;
        while (ach_next_popup() >= 0) {}
    } else if (g_pop < 0 && shown) {
        g_pop = ach_next_popup();
        g_pop_t0 = now;
    }
    if (const char *e = getenv("LARECOMP_ACH_POPUP_TEST")) {  // (test: that one shows, from the start)
        static int test = -2;
        if (test == -2) test = atoi(e);
        if (test >= 0 && test < ach_count() && g_pop < 0) g_pop = test, g_pop_t0 = now - 1.0;
    }
    return g_pop >= 0;
}

static float ease(float x) {
    x = x < 0 ? 0 : x > 1 ? 1 : x;
    return x * x * (3 - 2 * x);
}

// Drawn over the game, at the bottom middle: a pause-screen panel, the picture in its box, "Achievement", the name and
// what it was for. Slides up and fades in, stays, fades out.
void achievements_popup_draw(ImVec2 disp, ImFont *font, ImFont *big, float scale) {
    if (g_pop < 0) return;
    const AchInfo *a = ach_info(g_pop);
    if (!a) return;
    float t = (float)(seconds() - g_pop_t0);
    float in = ease(t / 0.35f), out = 1 - ease((t - (float)kPopSeconds + 0.6f) / 0.6f), alpha = in * out;
    if (alpha <= 0) return;
    float k = scale, s_small = font->FontSize * k * 0.62f, s_title = big->FontSize * k * 0.62f, s_desc = font->FontSize * k * 0.72f;
    const char *label = "ACHIEVEMENT";
    float tw = fmaxf(font->CalcTextSizeA(s_small, FLT_MAX, 0, label).x, big->CalcTextSizeA(s_title, FLT_MAX, 0, a->title).x);
    float dw = font->CalcTextSizeA(s_desc, FLT_MAX, 0, a->desc).x;
    float maxw = disp.x * 0.62f, icon = 74 * k, pad = 18 * k;
    float textw = fminf(fmaxf(tw, dw), maxw - icon - 3 * pad);
    float w = icon + 3 * pad + textw;
    float h = fmaxf(icon + 2 * pad - 4 * k, s_small + s_title + s_desc + 2 * pad + 6 * k);
    float x = (disp.x - w) * 0.5f, y = disp.y * 0.80f - h + (1 - in) * 30 * k;
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImVec2 A(x, y), B(x + w, y + h);
    float cut = 16 * k;
    theme::chamfer_fill(dl, A, B, cut, theme::u32(theme::mp_bg, 0.94f * alpha));
    theme::dot_grid(dl, A, B, 18 * k, theme::u32(theme::mp_line, 0.10f * alpha));
    ImVec4 line = theme::mp_line;
    line.w = alpha;
    theme::chamfer_glow(dl, A, B, cut, line, 2.0f * k, k, alpha);
    // the picture in a box of its own
    ImVec2 ia(x + pad, y + (h - icon) * 0.5f), ib(ia.x + icon, ia.y + icon);
    theme::chamfer_fill(dl, ia, ib, icon * 0.2f, theme::u32(theme::mp_fill, 0.9f * alpha));
    theme::chamfer_line(dl, ia, ib, icon * 0.2f, theme::u32(theme::mp_bright, alpha), 1.6f * k);
    badge(dl, a->badge, ImVec2((ia.x + ib.x) * 0.5f, (ia.y + ib.y) * 0.5f), icon * 0.32f, theme::u32(theme::mp_bright, alpha));
    // the words
    float tx = ib.x + pad, ty = y + (h - (s_small + s_title + s_desc + 6 * k)) * 0.5f;
    dl->AddText(font, s_small, ImVec2(tx, ty), theme::u32(theme::mp_text_dim, alpha), label);
    dl->AddText(big, s_title, ImVec2(tx, ty + s_small + 2 * k), theme::u32(theme::mp_bright, alpha), a->title);
    dl->PushClipRect(ImVec2(tx, y), ImVec2(tx + textw, y + h), true);
    dl->AddText(font, s_desc, ImVec2(tx, ty + s_small + s_title + 6 * k), theme::u32(theme::mp_text, alpha), a->desc);
    dl->PopClipRect();
}
