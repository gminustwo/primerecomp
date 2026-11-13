// Control remapping editor (runtime/input_bind.h), shared by the in-game menu's Controls tab (menu.mm) and the start
// screen's Controls page (launcher.mm): a table of Switch inputs with the controller button and the keys bound to
// each. Select a binding, then press the new button (5 s without one cancels), or hold the new key or keys (a combo:
// E + 1) until the 5 s countdown ends; Esc cancels; a button / keys already in use swap over; right-click puts one
// back to its default. Drawn in whichever ImGui context is current.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "theme.h"

extern "C" {
#include "../glyphs.h"
#include "../input_bind.h"
#include "../settings.h"
}

static const char *keys_name(const int *k, int n, char *buf, size_t size) {  // "E + 1"
    size_t at = 0;
    buf[0] = 0;
    for (int i = 0; i < n && at < size; i++) at += (size_t)snprintf(buf + at, size - at, "%s%s", i ? " + " : "", bind_key_label(k[i]));
    return buf;
}

static bool same_keys(int a, int b) {  // two keyboard bindings on the same keys, in any order
    int ka[BIND_COMBO], kb[BIND_COMBO], na = bind_key_combo(a, ka), nb = bind_key_combo(b, kb);
    if (!na || na != nb) return false;
    for (int i = 0; i < na; i++) {
        bool found = false;
        for (int j = 0; j < nb; j++) found |= ka[i] == kb[j];
        if (!found) return false;
    }
    return true;
}

static void cell(int dev, int slot, int fam, int cap_dev, int cap_slot, int ms_left) {
    char lbl[160], keys[96];
    bool waiting = cap_dev == dev && cap_slot == slot;
    int secs = (ms_left + 999) / 1000;
    if (waiting && dev == BIND_DEV_PAD) snprintf(lbl, sizeof lbl, "Press a button...  %d##%d_%d", secs, dev, slot);
    else if (waiting) {  // (the keys held so far: what the countdown's end takes)
        int k[BIND_COMBO], n = bind_capture_keys(k);
        if (n) snprintf(lbl, sizeof lbl, "%s  ...  %d##%d_%d", keys_name(k, n, keys, sizeof keys), secs, dev, slot);
        else snprintf(lbl, sizeof lbl, "Hold a key or keys...  %d##%d_%d", secs, dev, slot);
    } else
        snprintf(lbl, sizeof lbl, "%s##%d_%d", dev == BIND_DEV_PAD ? bind_pad_label(g_bind_pad[slot], fam) :
                                               bind_key_combo_name(slot, keys, sizeof keys), dev, slot);
    // changed from the default: orange text; the same button / keys on two inputs (a hand-edited primerecomp.ini): red
    int n = dev == BIND_DEV_PAD ? BIND_PAD_COUNT : BIND_KEY_COUNT, pushed = 0;
    bool dup = false;
    for (int i = 0; i < n; i++)
        dup |= i != slot && (dev == BIND_DEV_PAD ? g_bind_pad[i] == g_bind_pad[slot] && g_bind_pad[slot] > -1 : same_keys(i, slot));
    if (waiting) { ImGui::PushStyleColor(ImGuiCol_Button, theme::frame_on); pushed++; }
    else if (dup) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.12f, 1)); pushed++; }
    if (!bind_is_default_slot(dev, slot)) { ImGui::PushStyleColor(ImGuiCol_Text, theme::heading); pushed++; }
    if (ImGui::Button(lbl, ImVec2(-FLT_MIN, 0))) {
        if (waiting) bind_capture_cancel();
        else bind_capture_begin(dev, slot);
    }
    ImGui::PopStyleColor(pushed);
    if (!waiting && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {  // (one binding back to its default)
        bind_capture_cancel();
        bind_reset_slot(dev, slot);
    }
    if (!waiting && !bind_is_default_slot(dev, slot) && ImGui::IsItemHovered()) ImGui::SetTooltip("Right-click: back to the default");
    if (waiting && !ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);  // (a capture started by a test hook)
}

extern "C" void controls_bindings_draw(float scale) {
    (void)scale;
    int fam = bind_label_family();
    static const char *const fam_name[] = {"Nintendo", "Xbox", "PlayStation"};
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    const char *pad_name = ids && n > 0 ? SDL_GetGamepadNameForID(ids[0]) : NULL;
    char line[200];
    if (n > 0) snprintf(line, sizeof line, "Controller: %s  (%s button names)", pad_name ? pad_name : "connected", fam_name[fam % 3]);
    else snprintf(line, sizeof line, "No controller connected  (%s button names shown)", fam_name[fam % 3]);
    SDL_free(ids);
    ImGui::TextUnformatted(line);
    {   // face buttons: three buttons, the current one lit (bind_set_face_buttons swaps the bindings below to or from Xbox)
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Face buttons");
        static const char *const lay[FACE_COUNT] = {"Labels: A confirms in menus", "Nintendo: A on the right",
                                                    "Xbox: A on the bottom"};
        for (int i = 0; i < FACE_COUNT; i++) {
            ImGui::SameLine();
            bool cur = g_settings.face_buttons == i;
            if (cur) ImGui::PushStyleColor(ImGuiCol_Button, theme::frame_on);
            if (ImGui::Button(lay[i]) && !cur) {
                bind_capture_cancel();
                bind_set_face_buttons(i);
                settings_save();
            }
            if (cur) ImGui::PopStyleColor();
        }
    }
    {   // button icons: the game's prompts drawn as this controller's (runtime/hle/glyphs.c)
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Button icons");
        static const char *const set[ICONS_COUNT] = {"Auto", "Switch", "Xbox", "PlayStation", "Keyboard & mouse", "Wii Remote"};
        for (int i = 0; i < ICONS_COUNT; i++) {
            ImGui::SameLine();
            bool cur = g_settings.button_icons == i;
            if (cur) ImGui::PushStyleColor(ImGuiCol_Button, theme::frame_on);
            char lbl[64];
            snprintf(lbl, sizeof lbl, "%s##icons%d", set[i], i);
            if (ImGui::Button(lbl) && !cur) {
                g_settings.button_icons = i;
                settings_save();
            }
            if (cur) ImGui::PopStyleColor();
        }
    }
    ImGui::PushTextWrapPos(0);
    if (g_settings.button_icons == ICONS_AUTO) {
        static const char *const now[ICONS_COUNT] = {"", "Switch", "Xbox", "PlayStation", "keyboard & mouse", "Wii Remote"};
        ImGui::TextDisabled("Button icons: the game's prompts show the buttons of whatever you used last (now: %s), "
                            "as you've bound them.", now[glyphs_current() % ICONS_COUNT]);
    } else {
        ImGui::TextDisabled("Button icons: the game's prompts show these buttons, as you've bound them.");
    }
    ImGui::TextDisabled("Face buttons: Labels (the default) - in menus the button marked A (Cross) confirms and B "
                        "(Circle) goes back, on any controller; in play they're the bindings below, by position as on "
                        "a Switch controller: the bottom button jumps, the right one shoots. Nintendo - by position "
                        "everywhere, as on a Switch: the right button is A in menus too, the bottom one B. Xbox - A is "
                        "the bottom button everywhere: it confirms in menus and shoots in play.");
    ImGui::TextDisabled("Select a binding, then press the new button. For the keyboard, hold the new key, or up to three "
                        "together (a combo, such as E + 1 for a visor while 1 alone selects a beam), until the 5-second "
                        "countdown ends; if you let go first, the last keys you held together count. Esc cancels. A "
                        "button or keys already in use swap with the old ones. Right-click a binding to put it back to "
                        "its default.");
    ImGui::PopTextWrapPos();

    int cd, cs, ms;
    bind_capture_active(&cd, &cs, &ms);
    // a finished capture: say what happened, keep primerecomp.ini current
    static int seen_serial = -1;
    static char msg[400];
    int rdev, rslot, rother, serial;
    int kind = bind_last_result(&rdev, &rslot, &rother, &serial);
    if (seen_serial < 0) seen_serial = serial;
    if (serial != seen_serial) {
        seen_serial = serial;
        char now[96] = "", other[96] = "";
        if (rslot >= 0) {
            if (rdev == BIND_DEV_PAD) snprintf(now, sizeof now, "%s", bind_pad_label(g_bind_pad[rslot], fam));
            else bind_key_combo_name(rslot, now, sizeof now);
        }
        if (rother >= 0) {
            if (rdev == BIND_DEV_PAD) snprintf(other, sizeof other, "%s", bind_pad_label(g_bind_pad[rother], fam));
            else bind_key_combo_name(rother, other, sizeof other);
        }
        if (kind == 5) snprintf(msg, sizeof msg, "%s bindings reset to the defaults.", rdev == BIND_DEV_PAD ? "Controller" : "Keyboard");
        else if (kind == 1) snprintf(msg, sizeof msg, "%s is now %s.", bind_input_name(rslot), now);
        else if (kind == 2)
            snprintf(msg, sizeof msg, "%s is now %s; %s moved to %s (swapped).", bind_input_name(rslot), now,
                     bind_input_name(rother), other);
        else if (kind == 3) snprintf(msg, sizeof msg, "Cancelled: %s unchanged.", bind_input_name(rslot));
        else if (kind == 4) snprintf(msg, sizeof msg, "Nothing pressed: %s unchanged.", bind_input_name(rslot));
        if ((kind == 1 || kind == 2) && rdev == BIND_DEV_KEY) {  // a combo's first keys bound by themselves too
            int k[BIND_COMBO], nk = bind_key_combo(rslot, k);
            for (int j = 0; j + 1 < nk; j++)
                for (int i = 0; i < BIND_KEY_COUNT; i++) {
                    int o[BIND_COMBO];
                    if (i == rslot || bind_key_combo(i, o) != 1 || o[0] != k[j]) continue;
                    size_t at = strlen(msg);
                    snprintf(msg + at, sizeof msg - at, " Note: %s by itself is still %s%s%s%s, so that's pressed too as %s starts.",
                             bind_key_label(k[j]), bind_input_name(i), *bind_input_action(i) ? " (" : "",
                             bind_input_action(i), *bind_input_action(i) ? ")" : "", now);
                }
        }
        if (kind == 1 || kind == 2 || kind == 5) settings_save();
    }
    if (msg[0]) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(theme::accent, "%s", msg);
        ImGui::PopTextWrapPos();
    }

    if (ImGui::BeginTable("##binds", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("Switch", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("In the game", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("Keyboard / mouse", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableHeadersRow();
        // in the order a player thinks of them: face buttons, shoulders, Plus / Minus, d-pad, sticks, beams
        static const int order[] = {BIND_A, BIND_B, BIND_X, BIND_Y, BIND_L, BIND_R, BIND_ZL, BIND_ZR, BIND_PLUS,
                                    BIND_MINUS, BIND_UP, BIND_DOWN, BIND_LEFT, BIND_RIGHT, BIND_LSTICK, BIND_RSTICK,
                                    BIND_LS_UP, BIND_LS_DOWN, BIND_LS_LEFT, BIND_LS_RIGHT, BIND_RS_UP, BIND_RS_DOWN,
                                    BIND_RS_LEFT, BIND_RS_RIGHT, BIND_BEAM_POWER, BIND_BEAM_WAVE, BIND_BEAM_ICE,
                                    BIND_BEAM_PLASMA};
        for (int slot : order) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(bind_input_name(slot));
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", bind_input_action(slot));
            ImGui::TableNextColumn();
            if (slot < BIND_PAD_COUNT) cell(BIND_DEV_PAD, slot, fam, cd, cs, ms);
            else ImGui::TextDisabled(slot < BIND_RS_UP ? "Left stick" : slot < BIND_BEAM_POWER ? "Right stick" :
                                                                          "X held, then the d-pad");
            ImGui::TableNextColumn();
            cell(BIND_DEV_KEY, slot, fam, cd, cs, ms);
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(bind_is_default(BIND_DEV_PAD));
    if (ImGui::Button("Reset controller to defaults")) { bind_capture_cancel(); bind_reset(BIND_DEV_PAD); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(bind_is_default(BIND_DEV_KEY));
    if (ImGui::Button("Reset keyboard to defaults")) { bind_capture_cancel(); bind_reset(BIND_DEV_KEY); }
    ImGui::EndDisabled();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Bindings apply to the game in play only. Menus select with A / Enter and go back with B / Esc "
                        "(A and B as the face buttons above place them), and the in-game menu always opens with F1, or both of "
                        "the controller's sticks pressed in, Back + Start held together, or its Home / Guide button - so this "
                        "page can't be "
                        "locked out. The game's button icons follow the controller bindings.");
    ImGui::PopTextWrapPos();
}
