// AMD FSR 3 / FSR 4 through AMD's FSR SDK runtime (runtime/nvn/fsr.h). Windows: amd_fidelityfx_loader_vk.dll is
// loaded from the executable's folder if it is there, and its five C entry points (the FSR API) are all that's used:
// the executable needs nothing from the SDK to build but its MIT headers (third_party/ffx-api). Elsewhere: stubs.
//
// SDK 2.x details (2.4.0, the first with FSR 4 on Vulkan):
// - the Vulkan backend descriptor is FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK (FFX_API_BACKEND_ID_VK | 3; the
//   old type 3 only reaches the FSR 3.1.4 provider), and context creation carries the upscaler API version
//   (ffxCreateContextDescUpscaleVersion, FFX_UPSCALER_VERSION);
// - the version list names FSR 4 only when the query is given the device (a VkDeviceContext);
// - its shaders use 16-bit types, the Vulkan memory model and compute shader derivatives: device.c enables those
//   where the GPU has them (fsr_wants_device_features).
// Seen in i2kiselev/fm4-static-recomp issue #81 and AMD's docs (Kits/FidelityFX/docs/getting-started/ffx-api.md).
#include "../fsr.h"
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)

#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdlib.h>
#include "../../../third_party/ffx-api/upscalers/include/ffx_upscale.h"
#include "../../../third_party/ffx-api/api/include/vk/ffx_api_vk.h"

extern "C" void rt_log(const char *fmt, ...);

namespace {

struct Runtime {
    bool tried;
    HMODULE mod;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxQuery query;
    PfnFfxDispatch dispatch;
} R;

char g_status[256] = "not looked for yet";
VkDeviceContext g_dc;
PFN_vkDeviceWaitIdle g_wait_idle;
struct Version { bool have; uint64_t id; char name[32]; } g_ver[2];  // [0] FSR 3, [1] FSR 4

// The context: one at a time, made again when the sizes, the version or the flags change
ffxContext g_ctx;
int g_major;
uint32_t g_in_w, g_in_h, g_out_w, g_out_h, g_flags;
bool g_failed;  // the last creation with these parameters failed: not tried every frame
LARGE_INTEGER g_last_t, g_freq;

bool exe_dir(wchar_t *p, DWORD cap) {
    DWORD n = GetModuleFileNameW(NULL, p, cap - 64);
    if (!n || n >= cap - 64) return false;
    while (n > 0 && p[n - 1] != L'\\' && p[n - 1] != L'/') n--;
    p[n] = 0;
    return true;
}

bool load() {
    if (R.tried) return R.mod != NULL;
    R.tried = true;
    wchar_t dir[MAX_PATH * 2], p[MAX_PATH * 2];
    if (!exe_dir(dir, MAX_PATH * 2)) return false;
    wcscpy(p, dir);
    wcscat(p, L"amd_fidelityfx_loader_vk.dll");
    if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) {
        snprintf(g_status, sizeof g_status, "amd_fidelityfx_loader_vk.dll and amd_fidelityfx_upscaler_vk.dll aren't next to primerecomp.exe");
        return false;
    }
    wchar_t up[MAX_PATH * 2];
    wcscpy(up, dir);
    wcscat(up, L"amd_fidelityfx_upscaler_vk.dll");
    if (GetFileAttributesW(up) == INVALID_FILE_ATTRIBUTES) {
        snprintf(g_status, sizeof g_status, "amd_fidelityfx_upscaler_vk.dll isn't next to primerecomp.exe");
        return false;
    }
    // (LOAD_WITH_ALTERED_SEARCH_PATH: its own folder first for what it loads; the loader finds the upscaler DLL there)
    R.mod = LoadLibraryExW(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!R.mod) {
        DWORD e = GetLastError();
        snprintf(g_status, sizeof g_status, e == ERROR_MOD_NOT_FOUND
                     ? "AMD's FSR runtime needs the Microsoft Visual C++ Redistributable (x64)"
                     : "amd_fidelityfx_loader_vk.dll didn't load (error %lu)", e);
        rt_log("[fsr] %s\n", g_status);
        return false;
    }
    R.create = (PfnFfxCreateContext)(void *)GetProcAddress(R.mod, "ffxCreateContext");
    R.destroy = (PfnFfxDestroyContext)(void *)GetProcAddress(R.mod, "ffxDestroyContext");
    R.query = (PfnFfxQuery)(void *)GetProcAddress(R.mod, "ffxQuery");
    R.dispatch = (PfnFfxDispatch)(void *)GetProcAddress(R.mod, "ffxDispatch");
    if (!R.create || !R.destroy || !R.query || !R.dispatch) {
        snprintf(g_status, sizeof g_status, "amd_fidelityfx_loader_vk.dll lacks the FSR API");
        rt_log("[fsr] %s\n", g_status);
        FreeLibrary(R.mod);
        R.mod = NULL;
        return false;
    }
    snprintf(g_status, sizeof g_status, "loaded; the GPU isn't asked yet");
    return true;
}

void log_message(uint32_t type, const wchar_t *msg) {
    static int n;
    if (n >= 64) return;  // (a broken setup repeats itself every frame)
    n++;
    char s[512];
    WideCharToMultiByte(CP_UTF8, 0, msg ? msg : L"", -1, s, sizeof s, NULL, NULL);
    rt_log("[fsr] %s: %s\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", s);
}

float halton(uint32_t i, uint32_t base) {
    float f = 1, r = 0;
    while (i > 0) { f /= (float)base; r += f * (float)(i % base); i /= base; }
    return r;
}

void destroy_context() {
    if (!g_ctx) return;
    if (g_wait_idle) g_wait_idle(g_dc.vkDevice);  // (the last frames' commands may still use its images)
    R.destroy(&g_ctx, NULL);
    g_ctx = NULL;
}

FfxApiResource image(void *img, uint32_t vk_format, uint32_t w, uint32_t h, uint32_t usage, uint32_t state) {
    FfxApiResource r = {};
    r.resource = img;
    r.state = state;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = ffxApiGetSurfaceFormatVK((VkFormat)vk_format);
    r.description.width = w;
    r.description.height = h;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    r.description.usage = usage;
    return r;
}

}  // namespace

extern "C" {

bool fsr_compiled(void) { return load(); }
bool fsr_available(int major) { return R.mod && major >= 3 && major <= 4 && g_ver[major - 3].have; }
const char *fsr_status(void) { load(); return g_status; }
const char *fsr_version(int major) { return fsr_available(major) ? g_ver[major - 3].name : ""; }
bool fsr_wants_device_features(void) { return load(); }

void fsr_init(void *instance, void *phys, void *device, void *get_device_proc) {
    (void)instance;
    if (!load()) return;
    g_dc.vkDevice = (VkDevice)device;
    g_dc.vkPhysicalDevice = (VkPhysicalDevice)phys;
    g_dc.vkDeviceProcAddr = (PFN_vkGetDeviceProcAddr)get_device_proc;
    g_wait_idle = (PFN_vkDeviceWaitIdle)g_dc.vkDeviceProcAddr(g_dc.vkDevice, "vkDeviceWaitIdle");
    QueryPerformanceFrequency(&g_freq);
    // The upscaler versions this runtime has for this GPU (FSR 4 is listed only when given the device)
    ffxQueryDescGetVersions q = {};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = &g_dc;
    uint64_t count = 0;
    q.outputCount = &count;
    ffxReturnCode_t rc = R.query(NULL, &q.header);
    uint64_t ids[16] = {0};
    const char *names[16] = {0};
    if (rc == FFX_API_RETURN_OK && count) {
        if (count > 16) count = 16;
        q.versionIds = ids;
        q.versionNames = names;
        rc = R.query(NULL, &q.header);
    }
    if (rc != FFX_API_RETURN_OK || !count) {
        snprintf(g_status, sizeof g_status, "AMD's runtime has no upscaler for this GPU (error %u)", (unsigned)rc);
        rt_log("[fsr] %s\n", g_status);
        return;
    }
    char list[160] = "";
    for (uint64_t i = 0; i < count; i++) {
        const char *nm = names[i] ? names[i] : "?";
        size_t l = strlen(list);
        snprintf(list + l, sizeof list - l, "%s%s", l ? ", " : "", nm);
        int major = atoi(nm);
        if (major < 3 || major > 4) continue;  // (FSR 2.3: FSR 3 does better with the same inputs)
        Version &v = g_ver[major - 3];
        if (v.have) continue;  // (the list starts with the newest)
        v.have = true;
        v.id = ids[i];
        snprintf(v.name, sizeof v.name, "%s", nm);
    }
    rt_log("[fsr] AMD FSR runtime: upscalers %s\n", list);
    if (g_ver[1].have && g_ver[0].have)
        snprintf(g_status, sizeof g_status, "FSR %s and FSR %s on this GPU", g_ver[1].name, g_ver[0].name);
    else if (g_ver[0].have)
        snprintf(g_status, sizeof g_status, "FSR %s on this GPU (FSR 4 needs an AMD Radeon RX 6000 or newer, or a recent Ryzen APU)",
                 g_ver[0].name);
    else if (g_ver[1].have)
        snprintf(g_status, sizeof g_status, "FSR %s on this GPU", g_ver[1].name);
    else
        snprintf(g_status, sizeof g_status, "AMD's runtime offers only %s here", list);
}

void fsr_shutdown(void) {
    if (!R.mod) return;
    destroy_context();
}

void fsr_jitter(uint32_t in_w, uint32_t out_w, float *x, float *y) {
    // (= ffxQueryDescUpscaleGetJitterPhaseCount / GetJitterOffset, which want a context: 8 * ratio^2 phases)
    static uint32_t frame;
    float ratio = in_w ? (float)out_w / (float)in_w : 1.0f;
    uint32_t phases = (uint32_t)(8.0f * ratio * ratio);
    if (phases < 8) phases = 8;
    if (phases > 256) phases = 256;
    uint32_t i = frame++ % phases + 1;
    *x = halton(i, 2) - 0.5f;
    *y = halton(i, 3) - 0.5f;
}

bool fsr_evaluate(int major, const DlssEval *e, int color_space, void *reactive, uint32_t reactive_format) {
    if (!fsr_available(major) || !g_dc.vkDevice) return false;
    static int cs_ov = -2;  // (LARECOMP_FSR_COLOR=0 linear / 1 sRGB / 2 PQ: what FSR is told the colors are, for tests)
    if (cs_ov == -2) cs_ov = getenv("LARECOMP_FSR_COLOR") ? atoi(getenv("LARECOMP_FSR_COLOR")) : -1;
    if (cs_ov >= 0) color_space = cs_ov;
    // (the colors are 0..1 - sRGB-encoded bytes, or PQ in an HDR frame - but for a float image of linear light)
    uint32_t flags = (e->depth_inverted ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0) |
                     (color_space ? FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE : FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE);
    bool reset = e->reset;
    if (!g_ctx || g_major != major || g_in_w != e->in_w || g_in_h != e->in_h || g_out_w != e->out_w || g_out_h != e->out_h ||
        g_flags != flags) {
        bool same = g_major == major && g_in_w == e->in_w && g_in_h == e->in_h && g_out_w == e->out_w && g_out_h == e->out_h &&
                    g_flags == flags;
        if (same && g_failed) return false;
        destroy_context();
        g_major = major; g_in_w = e->in_w; g_in_h = e->in_h; g_out_w = e->out_w; g_out_h = e->out_h; g_flags = flags;
        g_failed = true;
        ffxCreateContextDescUpscale cd = {};
        cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        cd.flags = flags;
        cd.maxRenderSize = {e->in_w, e->in_h};
        cd.maxUpscaleSize = {e->out_w, e->out_h};
        cd.fpMessage = log_message;
        ffxCreateBackendVKDesc bd = {};
        bd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        bd.deviceContext = g_dc;
        ffxOverrideVersion ov = {};
        ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        ov.versionId = g_ver[major - 3].id;
        ffxCreateContextDescUpscaleVersion uv = {};
        uv.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        uv.version = FFX_UPSCALER_VERSION;
        cd.header.pNext = &bd.header;
        bd.header.pNext = &ov.header;
        ov.header.pNext = &uv.header;
        ffxReturnCode_t rc = R.create(&g_ctx, &cd.header, NULL);
        if (rc != FFX_API_RETURN_OK && (flags & FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE)) {
            g_ctx = NULL;  // (in case a provider doesn't take the non-linear flag)
            cd.flags = flags & ~FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
            rc = R.create(&g_ctx, &cd.header, NULL);
        }
        if (rc != FFX_API_RETURN_OK) {
            rt_log("[fsr] FSR %s: no context for %ux%u -> %ux%u (error %u)\n", g_ver[major - 3].name, e->in_w, e->in_h, e->out_w,
                   e->out_h, (unsigned)rc);
            g_ctx = NULL;
            return false;
        }
        g_failed = false;
        reset = true;
        rt_log("[fsr] FSR %s: %ux%u -> %ux%u%s%s\n", g_ver[major - 3].name, e->in_w, e->in_h, e->out_w, e->out_h,
               e->depth_inverted ? ", reverse-Z" : "", color_space == 2 ? ", PQ" : color_space == 1 ? ", sRGB" : ", linear");
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float ms = g_last_t.QuadPart && g_freq.QuadPart ? (float)((double)(now.QuadPart - g_last_t.QuadPart) * 1000.0 / (double)g_freq.QuadPart)
                                                    : 16.7f;
    g_last_t = now;
    if (ms <= 0.0f || ms > 100.0f) ms = 16.7f;

    // (every image is in VK_IMAGE_LAYOUT_GENERAL, which FFX's Vulkan backend reads as the UNORDERED_ACCESS state; it
    // puts them back in it when it is done)
    const uint32_t st = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = e->cmd;
    d.color = image(e->color_image, e->color_format, e->in_w, e->in_h, FFX_API_RESOURCE_USAGE_READ_ONLY, st);
    d.depth = image(e->depth_image, e->depth_format, e->in_w, e->in_h, FFX_API_RESOURCE_USAGE_READ_ONLY, st);
    d.motionVectors = image(e->mv_image, e->mv_format, e->in_w, e->in_h, FFX_API_RESOURCE_USAGE_READ_ONLY, st);
    d.output = image(e->out_image, e->out_format, e->out_w, e->out_h, FFX_API_RESOURCE_USAGE_UAV, st);
    if (reactive) d.reactive = image(reactive, reactive_format, e->in_w, e->in_h, FFX_API_RESOURCE_USAGE_READ_ONLY, st);
    d.jitterOffset = {e->jitter_x, e->jitter_y};
    d.motionVectorScale = {1.0f, 1.0f};  // (in pixels: the camera's, exec.c dlss_mv)
    d.renderSize = {e->in_w, e->in_h};
    d.upscaleSize = {e->out_w, e->out_h};
    d.enableSharpening = true;
    d.sharpness = 0.8f;  // (AMD's samples' default)
    d.frameTimeDelta = ms;
    d.preExposure = 1.0f;
    d.reset = reset;
    // Metroid Prime's first-person camera: near 0.2, far 750 (in meters), 55 degrees up and down (FSR orders near and
    // far itself; DEPTH_INVERTED says which end of the depth range is near)
    d.cameraNear = 0.2f;
    d.cameraFar = 750.0f;
    d.cameraFovAngleVertical = 55.0f * 3.14159265f / 180.0f;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = color_space == 2 ? FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ : color_space == 1 ? FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB : 0;
    ffxReturnCode_t rc = R.dispatch(&g_ctx, &d.header);
    if (rc != FFX_API_RETURN_OK) {
        static int n;
        if (n++ < 8) rt_log("[fsr] dispatch failed (error %u)\n", (unsigned)rc);
        return false;
    }
    return true;
}

}  // extern "C"

#else

bool fsr_compiled(void) { return false; }
bool fsr_available(int) { return false; }
const char *fsr_status(void) { return "AMD's FSR 3 / 4 runtime is for Windows only"; }
const char *fsr_version(int) { return ""; }
bool fsr_wants_device_features(void) { return false; }
void fsr_init(void *, void *, void *, void *) {}
void fsr_shutdown(void) {}
void fsr_jitter(uint32_t, uint32_t, float *x, float *y) { *x = *y = 0; }
bool fsr_evaluate(int, const DlssEval *, int, void *, uint32_t) { return false; }

#endif
