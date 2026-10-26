// In-game overlay menu (Dear ImGui). Open with F1 (Esc without a controller), or on a controller L3+R3, Minus+Plus
// (Back+Start) or Home / Guide.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union SDL_Event SDL_Event;

// Main (UI) thread: returns true if the event was consumed by the menu (the game must not see it).
bool menu_handle_event(const SDL_Event *e);
// Main thread: apply window requests (fullscreen) made from the menu.
void menu_apply_window_requests(void *sdl_window);

// Render thread, inside the window image's rendering (Vulkan command buffer cmd).
void menu_render(void *cmd, uint32_t target_w, uint32_t target_h);

#ifdef __cplusplus
}
#endif
