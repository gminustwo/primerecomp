// What the renderer (runtime/nvn/vulkan) gives the rest of the runtime beyond runtime/nvn/nvn.h.
#pragma once
#include <SDL3/SDL_events.h>

bool renderer_is_gpu_owned(NvnTexture *t);
void renderer_flush(void);  // finish all recorded GPU work
bool renderer_origin_lower_left(void);
const char *renderer_stats(void);
bool renderer_format_is_depth(uint32_t f);
bool renderer_format_has_stencil(uint32_t f);
void renderer_mark_gpu_owned(NvnTexture *t);
void input_handle_event(const SDL_Event *e);
bool input_user_blocked(const SDL_Event *e);
