// Control remapping editor (runtime/input_bind.h), shared by the in-game menu's Controls tab (menu.mm) and the start
// screen's Controls page (launcher.mm): a table of Switch inputs with the controller button and the key bound to
// each. Select a binding, then press the new button / key (Esc or 5 s without a press cancels; a button already in
// use swaps over). Drawn in whichever ImGui context is current.
#include <SDL3/SDL.h>
#include <stdio.h>

#include "imgui.h"
#include "theme.h"

extern "C" {
#include "../glyphs.h"
#include "../input_bind.h"
#include "../settings.h"
}

static void cell(int dev, int slot, int fam, int cap_dev, int cap_slot, int ms_left) {
    char lbl[96];
    bool waiting = cap_dev == dev && cap_slot == slot;
    if (waiting) snprintf(lbl, sizeof lbl, "Press %s...  %d##%d_%d", dev == BIND_DEV_PAD ? "a button" : "a key / click",
                          (ms_left + 999) / 1000, dev, slot);
    else snprintf(lbl, sizeof lbl, "%s##%d_%d", dev == BIND_DEV_PAD ? bind_pad_label(g_bind_pad[slot], fam) :
                                                 bind_key_label(g_bind_key[slot]), dev, slot);
    // changed from the default: orange text; the same button / key on two inputs (a hand-edited primerecomp.ini): red
    const int16_t *t = dev == BIND_DEV_PAD ? g_bind_pad : g_bind_key;
    int n = dev == BIND_DEV_PAD ? BIND_PAD_COUNT : BIND_KEY_COUNT, pushed = 0;
    bool dup = false;
    for (int i = 0; i < n; i++) dup |= i != slot && t[i] == t[slot] && t[slot] > (dev == BIND_DEV_PAD ? -1 : 0);
    if (waiting) { ImGui::PushStyleColor(ImGuiCol_Button, theme::frame_on); pushed++; }
    else if (dup) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.12f, 1)); pushed++; }
    if (!bind_is_default_slot(dev, slot)) { ImGui::PushStyleColor(ImGuiCol_Text, theme::heading); pushed++; }
    if (ImGui::Button(lbl, ImVec2(-FLT_MIN, 0))) {
        if (waiting) bind_capture_cancel();
        else bind_capture_begin(dev, slot);
    }
    ImGui::PopStyleColor(pushed);
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
    {   // face layout: two buttons, the current one lit (bind_set_face_layout swaps the bindings below too)
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Face buttons");
        static const char *const lay[] = {"Nintendo: A on the right", "Xbox: A on the bottom"};
        for (int i = 0; i < 2; i++) {
            ImGui::SameLine();
            bool cur = g_settings.face_layout == i;
            if (cur) ImGui::PushStyleColor(ImGuiCol_Button, theme::frame_on);
            if (ImGui::Button(lay[i]) && !cur) {
                bind_capture_cancel();
                bind_set_face_layout(i);
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
    ImGui::TextDisabled("Face buttons: which button is A (and confirms in menus) - the Switch's layout, where A is "
                        "the right button and B the bottom one, or an Xbox controller's, A at the bottom and B on the "
                        "right. Select a binding, then press the new button or key. Esc (or 5 seconds) cancels. A "
                        "button that's already in use swaps with the old one.");
    ImGui::PopTextWrapPos();

    int cd, cs, ms;
    bind_capture_active(&cd, &cs, &ms);
    // a finished capture: say what happened, keep primerecomp.ini current
    static int seen_serial = -1;
    static char msg[200];
    int rdev, rslot, rother, serial;
    int kind = bind_last_result(&rdev, &rslot, &rother, &serial);
    if (seen_serial < 0) seen_serial = serial;
    if (serial != seen_serial) {
        seen_serial = serial;
        const char *now = rslot < 0 ? "" : rdev == BIND_DEV_PAD ? bind_pad_label(g_bind_pad[rslot], fam) : bind_key_label(g_bind_key[rslot]);
        if (kind == 5) snprintf(msg, sizeof msg, "%s bindings reset to the defaults.", rdev == BIND_DEV_PAD ? "Controller" : "Keyboard");
        else if (kind == 1) snprintf(msg, sizeof msg, "%s is now %s.", bind_input_name(rslot), now);
        else if (kind == 2)
            snprintf(msg, sizeof msg, "%s is now %s; %s moved to %s (swapped).", bind_input_name(rslot), now,
                     bind_input_name(rother), rdev == BIND_DEV_PAD ? bind_pad_label(g_bind_pad[rother], fam) : bind_key_label(g_bind_key[rother]));
        else if (kind == 3) snprintf(msg, sizeof msg, "Cancelled: %s unchanged.", bind_input_name(rslot));
        else if (kind == 4) snprintf(msg, sizeof msg, "Nothing pressed: %s unchanged.", bind_input_name(rslot));
        if (kind == 1 || kind == 2 || kind == 5) settings_save();
    }
    if (msg[0]) ImGui::TextColored(theme::accent, "%s", msg);

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
    ImGui::TextDisabled("Bindings apply to the game only. Menus select with A / Enter and go back with B / Esc (A "
                        "and B as the face buttons above place them), and the in-game menu always opens with F1, or both of "
                        "the controller's sticks pressed in, Back + Start held together, or its Home / Guide button - so this "
                        "page can't be "
                        "locked out. The game's button icons follow the controller bindings.");
    ImGui::PopTextWrapPos();
}
