// AMD FSR 3 and FSR 4 (Video tab -> Upscaler), through AMD's FSR SDK runtime: two signed DLLs from the SDK
// (github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, Kits/FidelityFX/signedbin), next to the executable:
//   amd_fidelityfx_loader_vk.dll    (the FSR API: ffxCreateContext, ffxDispatch...)
//   amd_fidelityfx_upscaler_vk.dll  (FSR 4.x, 3.1.x and 2.3.x; the loader finds it next to itself)
// tools/fetch_ffx.sh puts them in build/ffx. AMD's license lets them be shipped (in binary form, with its notice),
// and their API headers are MIT (third_party/ffx-api). The SDK has no Linux or macOS runtime: there FSR 3 / 4 aren't
// offered (FSR 1 stays). FSR 3 runs on any GPU; FSR 4 where the runtime lists it for this GPU (AMD Radeon RX 9000 /
// 7000 / 6000 and some Ryzen APUs in SDK 2.4).
// The renderer side is DLSS's (nvn/vulkan/exec.c: jitter, the scene's depth, zero motion vectors).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "dlss.h"  // (DlssEval: what a temporal upscaler is given)

#ifdef __cplusplus
extern "C" {
#endif

bool fsr_compiled(void);        // the runtime DLLs are next to the executable and loaded
bool fsr_available(int major);  // FSR `major` (3 or 4) runs on this GPU (known after fsr_init)
const char *fsr_status(void);   // one line for the menu: the versions on this GPU, or why there are none
const char *fsr_version(int major);  // "3.1.6", "4.1.2"; "" when there is none

// device.c, before vkCreateDevice: whether to enable the features FSR's shaders use (16-bit types, the Vulkan memory
// model, compute shader derivatives, ...) where the GPU has them
bool fsr_wants_device_features(void);
void fsr_init(void *instance, void *phys, void *device, void *get_device_proc);
void fsr_shutdown(void);

// This frame's sub-pixel camera offset in render pixels (x right, y down): AMD's Halton (2, 3) sequence of
// 8 * (output / input)^2 phases
void fsr_jitter(uint32_t in_w, uint32_t out_w, float *x, float *y);

// One FSR `major` upscale, recorded into e->cmd (outside any rendering): DLSS's inputs (all VK_IMAGE_LAYOUT_GENERAL).
// color_space: 0 linear, 1 sRGB-encoded, 2 PQ-encoded (an HDR frame). reactive: NULL, or a VkImage at the input size
// (reactive_format) of how much each pixel should follow this frame rather than its history (0..1: the HUD's)
bool fsr_evaluate(int major, const DlssEval *e, int color_space, void *reactive, uint32_t reactive_format);

#ifdef __cplusplus
}
#endif
