// NVIDIA DLSS (Video tab -> Upscaler). The NGX SDK is NVIDIA's and its license doesn't allow shipping it under this
// project's, so it is not in this repository, and the code that calls it is built on its own (nvn/ngx/dlss_ngx.cpp):
// - Windows: NVIDIA's static library is MSVC-only, so that code is a small DLL, primerecomp_ngx.dll
//   (tools/build_ngx_bridge_windows.bat, needs Visual Studio Build Tools and a clone of github.com/NVIDIA/DLSS).
//   The executable loads it if it sits next to it (nvn/vulkan/dlss.cpp); without it the menu doesn't offer DLSS.
//   nvngx_dlss.dll (from the SDK's lib/Windows_x86_64/rel) goes next to the executable too.
// - Linux: `make NGX_SDK=<clone of github.com/NVIDIA/DLSS>` links it in.
// The renderer side (jitter, depth, the pass) is in nvn/vulkan/exec.c ("DLSS").
#pragma once
#include <stdbool.h>
#include <stdint.h>
#if defined(DLSS_BRIDGE_DLL) && defined(_WIN32)
#define DLSS_API __declspec(dllexport)  // (building primerecomp_ngx.dll)
#else
#define DLSS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

DLSS_API bool dlss_compiled(void);     // built with the NGX SDK
DLSS_API bool dlss_available(void);    // NGX is up on this GPU and the feature DLL was found
DLSS_API const char *dlss_status(void);  // one line for the menu: why DLSS is or isn't usable

// device.c: extensions NGX wants. Instance ones before vkCreateInstance, device ones before vkCreateDevice; names
// stay valid for the process. Return how many were written (0: none / not compiled / no nvngx).
DLSS_API uint32_t dlss_instance_extensions(const char **names, uint32_t cap);
DLSS_API uint32_t dlss_device_extensions(void *instance, void *phys, const char **names, uint32_t cap);
DLSS_API void dlss_init(void *instance, void *phys, void *device, void *get_instance_proc, void *get_device_proc);
DLSS_API void dlss_shutdown(void *device);
// (the bridge DLL has no rt_log of its own: the executable hands it a function that logs one finished line)
DLSS_API void dlss_set_log(void (*log)(const char *line));

// This frame's sub-pixel camera offset in render pixels (x right, y down), from a Halton (2, 3) sequence of
// 8 * (output / input)^2 phases.
DLSS_API void dlss_jitter(uint32_t in_w, uint32_t out_w, float *x, float *y);

// One DLSS evaluation, recorded into `cmd` (outside any rendering). Handles are Vulkan's (VkImage, VkImageView...).
// color: the game image at in_w x in_h; depth: the scene's depth (aspect DEPTH); mv: RG16F motion vectors at
// in_w x in_h (pixels); out: the upscaled image at out_w x out_h, STORAGE usage. All in VK_IMAGE_LAYOUT_GENERAL.
typedef struct {
    void *cmd;
    void *color_image, *color_view;  uint32_t color_format, in_w, in_h;
    void *depth_image, *depth_view;  uint32_t depth_format;
    void *mv_image, *mv_view;        uint32_t mv_format;
    void *out_image, *out_view;      uint32_t out_format, out_w, out_h;
    float jitter_x, jitter_y;
    bool depth_inverted;  // reverse-Z: 1 at the camera, 0 far away
    bool reset;           // history is invalid (first frame, a size change, a cut)
} DlssEval;
DLSS_API bool dlss_evaluate(const DlssEval *e);

#ifdef __cplusplus
}
#endif
