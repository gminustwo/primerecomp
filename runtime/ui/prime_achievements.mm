// Live progress, menu, and queued unlock banner. Animation adapted from the LAS Recomp prototype.
#include "../prime_achievements.h"
#include "../paths.h"
#include "prime_achievement_rules.h"
#include "imgui.h"
#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <deque>
#include <mutex>

extern "C" {
#include "../hle.h"
}

using namespace prime_achievements;
static constexpr size_t rule_count = sizeof rules / sizeof *rules;

struct LiveInventory {
    Progress values{};
    uint64_t player = 0, load = 0;
    bool valid = false, in_intro = false;
    IntroProgress intro;
};
static LiveInventory inventory;
static std::mutex inventory_lock;

void prime_achievements_save_read(void) {
    std::lock_guard<std::mutex> lock(inventory_lock);
    inventory.valid = inventory.in_intro = false;
    inventory.intro.reactor_seen = false;
    ++inventory.load;
}

void prime_achievements_inventory(uint64_t player, const int32_t *items, const char *room) {
    if (!room || !strncmp(room, "Ending", 6) || !strncmp(room, "Gallery", 7)) return;
    std::lock_guard<std::mutex> lock(inventory_lock);
    inventory.intro.observe(room);
    inventory.in_intro = !strncmp(room, "Frigate Orpheon / ", 18);
    // The frigate loans equipment. Its route can earn story achievements, while inventory starts at Tallon IV.
    if (inventory.in_intro) {
        inventory.valid = false;
        return;
    }
    Progress values = read_capacities(items);
    if (values[4] > 250 || values[7] > 8 || values[24] > 14) return;
    inventory.values = values;
    inventory.player = player;
    inventory.valid = true;
}

static local_achievements::Tracker tracker(rules, rule_count);
static std::deque<size_t> pending;
static LiveInventory shown;
static bool history_opened;
static double elapsed, clock_before, next_sample;

RT_ON_RESET(prime_achievements_reset) {
    std::lock_guard<std::mutex> lock(inventory_lock);
    inventory = {};
    shown = {};
    tracker.rebase();
    pending.clear();
    elapsed = clock_before = next_sample = 0;
}

static void queue_unlock(size_t index) {
    pending.push_back(index);
    LOG("[achievements] unlocked %s\n", rules[index].id);
}

static void open_history() {
    if (history_opened) return;
    const char *path = getenv("LARECOMP_ACHIEVEMENTS_FILE");
    history_opened = true;
    if (!tracker.open(path && *path ? path : std::string(rt_data_root()) + "/achievements-prime-v1.log"))
        LOG("[achievements] could not read local history\n");
}

bool prime_achievements_tick(bool visible) {
    using namespace std::chrono;
    double now = duration<double>(steady_clock::now().time_since_epoch()).count();
    double delta = clock_before ? now - clock_before : 0;
    clock_before = now;
    if (now >= next_sample) {
        next_sample = now + .1;
        LiveInventory current;
        {
            std::lock_guard<std::mutex> lock(inventory_lock);
            current = inventory;
        }
        if (!current.valid || !shown.valid || current.player != shown.player || current.load != shown.load)
            tracker.rebase();
        shown = current;
        if (shown.valid || shown.in_intro) open_history();
        if (history_opened) {
            for (size_t i = 0; i < rule_count; ++i) {
                bool occurred = (rules[i].metric == ReactorClear && shown.intro.reactor_clear) ||
                                (rules[i].metric == FrigateEscape && shown.intro.escaped);
                if (occurred && tracker.award(i)) queue_unlock(i);
            }
        }
        if (shown.valid)
            for (size_t i : tracker.sample(shown.values.data())) queue_unlock(i);
    }
    if (!visible || (!shown.valid && !shown.in_intro) || pending.empty()) return false;
    elapsed += std::min(delta, .1);
    if (elapsed >= 7) {
        pending.pop_front();
        elapsed = 0;
    }
    return !pending.empty();
}

static void draw_badge(ImDrawList *draw, Rule::Badge badge, ImVec2 center, float radius, ImU32 ink) {
    auto point = [&](float x, float y) { return ImVec2(center.x + x * radius, center.y + y * radius); };
    float stroke = std::max(1.f, radius * .12f);
    auto line = [&](float x, float y, float bx, float by) { draw->AddLine(point(x,y), point(bx,by), ink, stroke); };
    switch (badge) {
    case Rule::Gear:
        draw->AddCircle(center, radius * .64f, ink, 20, stroke);
        line(-.9f, 0, -.4f, 0); line(.4f, 0, .9f, 0); line(0, -.9f, 0, -.4f); line(0, .4f, 0, .9f);
        break;
    case Rule::Beam:
        draw->AddTriangle(point(-.75f,.65f), point(-.16f,-.72f), point(.67f,.22f), ink, stroke);
        line(.74f, -.7f, .37f, -.38f);
        break;
    case Rule::Suit:
        draw->AddRect(point(-.72f, -.62f), point(.72f,.63f), ink, radius * .35f, 0, stroke);
        line(-.42f, -.15f,.42f,-.15f); line(-.25f,.31f,.25f,.31f);
        break;
    case Rule::Missile:
        draw->AddTriangle(point(-.67f,.35f), point(.65f,-.65f), point(.35f,.68f), ink, stroke);
        line(-.36f,.37f,-.74f,.75f);
        break;
    case Rule::Tank:
        draw->AddRect(point(-.7f,-.48f), point(.64f,.48f), ink, radius * .17f, 0, stroke);
        line(.68f,-.20f,.82f,-.20f); line(.68f,.20f,.82f,.20f);
        line(-.35f,0,.30f,0); line(-.03f,-.27f,-.03f,.27f);
        break;
    case Rule::Bomb:
        draw->AddCircle(point(0,.15f), radius * .57f, ink, 20, stroke);
        line(.23f,-.43f,.51f,-.79f);
        break;
    case Rule::Escape:
        line(-.87f, 0, .75f, 0);
        line(.22f, -.52f, .75f, 0);
        line(.22f, .52f, .75f, 0);
        break;
    case Rule::Artifact:
        for (ImVec2 p : {point(0,-.88f), point(.60f,0), point(0,.88f), point(-.60f,0)}) draw->PathLineTo(p);
        draw->PathStroke(ink, ImDrawFlags_Closed, stroke);
        line(-.60f,0,.60f,0);
        break;
    }
}

static float ease(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return t * t * (3 - 2 * t);
}

static void erase_text(ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 color,
                       const char *text, float edge) {
    for (const char *p = text; *p; ++p) {
        const ImFontGlyph *glyph = font->FindGlyph((ImWchar)(unsigned char)*p);
        float advance = (glyph ? glyph->AdvanceX : font->FallbackAdvanceX) * size / font->FontSize;
        if (pos.x + advance >= edge) break;
        dl->AddText(font, size, pos, color, p, p + 1);
        pos.x += advance;
    }
}

void prime_achievements_popup(ImVec2 display, ImFont *font, float scale) {
    if (pending.empty()) return;
    const Rule &rule = rules[pending.front()];
    constexpr const char *label = "ACHIEVEMENT UNLOCKED";
    float u = scale * 1.25f, t = (float)elapsed;
    auto width = [&](float size, const char *text) { return font->CalcTextSizeA(size * u, FLT_MAX, 0, text).x; };
    float full = 118 * u + std::max({width(13, label), width(24, rule.title), width(17, rule.description)});
    float available = display.x - 24 * u;
    if (available <= 0) return;
    if (full > available) { u *= available / full; full = available; }
    float expand = t < 2.2f ? ease((t-1.4f)/.8f) : 1-ease((t-5.67f)/.7f);
    float diameter = 80 * u, w = diameter + (full - diameter) * expand;
    float alpha = t < .3f ? ease(t/.3f) : 1-ease((t-6.4f)/.6f);
    float x = (display.x - w) * .5f, y = std::max(0.f, display.y - 124 * u);
    auto color = [&](int r, int g, int b, float amount = 1.f) { return IM_COL32(r,g,b,(int)(255 * alpha * amount)); };
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(x,y), ImVec2(x+w,y+diameter), color(18,30,39), diameter * .5f);
    ImVec2 center(x + 40*u, y + 40*u);
    dl->AddCircleFilled(center, 32*u, color(29,47,59));
    float angle = std::min(t, 1.6f) * 8 + std::max(0.f, t-5.67f) * 9;
    for (int arc = 0; arc < 2; ++arc) {
        float start = angle + arc * 3.14159f;
        dl->PathArcTo(center, 35*u, start, start + 2.3f, 32);
        dl->PathStroke(color(235,148,64), 0, 3*u);
    }
    if (t > .6f) draw_badge(dl, rule.badge, center, 19*u, color(255,210,145,ease((t-.6f)/.45f)));
    if (t < 1.9f) return;
    float reveal = ease((t-1.9f)/.45f), edge = x + w - 18*u, tx = x + 94*u;
    erase_text(dl, font, 13*u, ImVec2(tx,y+9*u), color(255,193,124,reveal), label, edge);
    erase_text(dl, font, 24*u, ImVec2(tx,y+25*u), color(246,249,252,reveal), rule.title, edge);
    erase_text(dl, font, 17*u, ImVec2(tx,y+52*u), color(200,216,229,reveal), rule.description, edge);
}

void prime_achievements_menu() {
    open_history();
    if (tracker.has_error()) ImGui::TextUnformatted("Could not access local achievement history.");
    unsigned earned = 0;
    for (size_t i = 0; i < rule_count; ++i) earned += tracker.unlocked(i);
    char summary[64];
    snprintf(summary, sizeof summary, "%u of %u unlocked", earned, (unsigned)rule_count);
    ImGui::ProgressBar((float)earned / rule_count, ImVec2(-FLT_MIN,0), summary);
    enum { All, Unlocked, Locked };
    static int filter = All;
    ImGui::RadioButton("All", &filter, All);
    ImGui::SameLine();
    ImGui::RadioButton("Unlocked", &filter, Unlocked);
    ImGui::SameLine();
    ImGui::RadioButton("Locked", &filter, Locked);
    bool any = false;
    for (size_t i = 0; i < rule_count; ++i) {
        bool unlocked = tracker.unlocked(i);
        if ((filter == Unlocked && !unlocked) || (filter == Locked && unlocked)) continue;
        if (any) ImGui::Separator();
        any = true;
        const Rule &rule = rules[i];
        float size = 2 * ImGui::GetTextLineHeight();
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(size,size));
        if (ImGui::IsItemVisible()) {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            ImVec2 center(p.x + size*.5f, p.y + size*.5f);
            dl->AddCircleFilled(center, size*.48f, unlocked ? IM_COL32(100,57,23,255) : IM_COL32(47,60,70,255));
            draw_badge(dl, rule.badge, center, size*.30f, unlocked ? IM_COL32(255,211,149,255) : IM_COL32(167,182,196,255));
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextWrapped("%s", rule.title);
        ImGui::TextDisabled("%s", unlocked ? "Unlocked" : "Locked");
        ImGui::TextWrapped("%s", rule.description);
        if (!unlocked && shown.valid && rule.target > 1)
            ImGui::TextDisabled("%u / %u", std::min(shown.values[rule.metric], rule.target), rule.target);
        ImGui::EndGroup();
    }
    if (!any) ImGui::TextUnformatted(filter == Unlocked ? "No achievements unlocked yet." : "All achievements unlocked.");
}
