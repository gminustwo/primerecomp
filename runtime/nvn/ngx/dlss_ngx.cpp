// NVIDIA DLSS super resolution through the NGX SDK (see runtime/nvn/dlss.h): the only code that calls NGX. Built into
// primerecomp_ngx.dll with MSVC on Windows (tools/build_ngx_bridge_windows.bat), compiled into the executable on Linux
// (runtime/nvn/vulkan/dlss.cpp includes it).
#include "../dlss.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

#ifdef DLSS_BRIDGE_DLL
static void (*g_logfn)(const char *);
extern "C" void dlss_set_log(void (*f)(const char *)) { g_logfn = f; }
static void LOG(const char *fmt, ...) {
    char b[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    if (g_logfn) g_logfn(b);
}
#else
extern "C" void rt_log(const char *fmt, ...);
#define LOG(...) rt_log(__VA_ARGS__)
extern "C" void dlss_set_log(void (*)(const char *)) {}
#endif


#include <string>
#include <vector>
#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_helpers_vk.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <limits.h>
#include <wchar.h>
#endif

namespace {

// A GUID of this project's own for NGX_PROJECT_ID (NVSDK_NGX_ENGINE_TYPE_CUSTOM needs one; any stable one will do).
const char *kProjectId = "d3a1c7f2-6b54-4e0e-9c58-1f7a2b9e4d61";

NVSDK_NGX_Parameter *g_caps;      // NGX's parameter map, allocated once
NVSDK_NGX_Parameter *g_params;    // the feature's
NVSDK_NGX_Handle *g_feature;
bool g_ready;                    // NGX initialised and DLSS available
bool g_tried;
std::string g_status = "not started";
std::wstring g_dir;              // where nvngx_dlss.dll is looked for: next to the executable
const wchar_t *g_dirs[1];
NVSDK_NGX_FeatureCommonInfo g_info;
VkDevice g_dev;
PFN_vkDeviceWaitIdle g_wait;  // (through vkGetDeviceProcAddr: this code doesn't link the Vulkan loader)

// the feature's size and flags: it is made again when one of these changes
uint32_t g_in_w, g_in_h, g_out_w, g_out_h;
int g_flags = -1;

std::vector<std::string> g_inst_ext, g_dev_ext;

std::wstring exe_dir() {
#ifdef _WIN32
    wchar_t p[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, p, (DWORD)(sizeof p / sizeof *p));
    std::wstring s(p, n);
#else
    char c[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", c, sizeof c - 1);
    if (n < 0) n = 0;
    c[n] = 0;
    std::wstring s;
    for (ssize_t i = 0; i < n; i++) s.push_back((wchar_t)(unsigned char)c[i]);
#endif
    size_t k = s.find_last_of(L"/\\");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}

bool file_exists_next_to_exe() {
#ifdef _WIN32
    std::wstring f = g_dir + L"\\nvngx_dlss.dll";
    return GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    return true;  // (Linux: the driver ships libnvidia-ngx-dlss.so; NGX finds it itself)
#endif
}

NVSDK_NGX_FeatureDiscoveryInfo discovery() {
    NVSDK_NGX_FeatureDiscoveryInfo d = {};
    d.SDKVersion = NVSDK_NGX_Version_API;
    d.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    d.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    d.Identifier.v.ProjectDesc.ProjectId = kProjectId;
    d.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    d.Identifier.v.ProjectDesc.EngineVersion = "1.0";
    d.ApplicationDataPath = g_dir.c_str();
    d.FeatureInfo = &g_info;
    return d;
}

void prepare_paths() {
    if (!g_dir.empty()) return;
    g_dir = exe_dir();
    g_dirs[0] = g_dir.c_str();
    memset(&g_info, 0, sizeof g_info);
    g_info.PathListInfo.Path = g_dirs;
    g_info.PathListInfo.Length = 1;
}

void release_feature() {
    if (g_feature) {
        if (g_wait) g_wait(g_dev);
        NVSDK_NGX_VULKAN_ReleaseFeature(g_feature);
        g_feature = nullptr;
    }
    if (g_params) { NVSDK_NGX_VULKAN_DestroyParameters(g_params); g_params = nullptr; }
    g_flags = -1;
}

float halton(uint32_t i, uint32_t base) {
    float f = 1, r = 0;
    while (i > 0) { f /= (float)base; r += f * (float)(i % base); i /= base; }
    return r;
}

}  // namespace

extern "C" {

bool dlss_compiled(void) { return true; }
bool dlss_available(void) { return g_ready; }
const char *dlss_status(void) { return g_status.c_str(); }

static uint32_t copy_exts(std::vector<std::string> &keep, uint32_t n, VkExtensionProperties *p, const char **names, uint32_t cap) {
    keep.clear();
    for (uint32_t i = 0; i < n; i++) keep.push_back(p[i].extensionName);
    uint32_t k = 0;
    for (auto &s : keep) if (k < cap) names[k++] = s.c_str();
    return k;
}

uint32_t dlss_instance_extensions(const char **names, uint32_t cap) {
    prepare_paths();
    if (!file_exists_next_to_exe()) { g_status = "nvngx_dlss.dll not found next to primerecomp.exe"; return 0; }
    NVSDK_NGX_FeatureDiscoveryInfo d = discovery();
    uint32_t n = 0;
    VkExtensionProperties *p = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&d, &n, &p))) {
        g_status = "NGX can't say which Vulkan extensions it needs (no NVIDIA driver?)";
        return 0;
    }
    return copy_exts(g_inst_ext, n, p, names, cap);
}

uint32_t dlss_device_extensions(void *instance, void *phys, const char **names, uint32_t cap) {
    if (g_inst_ext.empty() && !file_exists_next_to_exe()) return 0;
    NVSDK_NGX_FeatureDiscoveryInfo d = discovery();
    NVSDK_NGX_FeatureRequirement req = {};
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_GetFeatureRequirements((VkInstance)instance, (VkPhysicalDevice)phys, &d, &req)) &&
        req.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported) {
        g_status = "this GPU or driver doesn't support DLSS (needs a GeForce RTX)";
        return 0;
    }
    uint32_t n = 0;
    VkExtensionProperties *p = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements((VkInstance)instance, (VkPhysicalDevice)phys, &d, &n, &p)))
        return 0;
    return copy_exts(g_dev_ext, n, p, names, cap);
}

void dlss_init(void *instance, void *phys, void *device, void *gipa, void *gdpa) {
    if (g_tried) return;
    g_tried = true;
    prepare_paths();
    g_dev = (VkDevice)device;
    g_wait = (PFN_vkDeviceWaitIdle)((PFN_vkGetDeviceProcAddr)gdpa)((VkDevice)device, "vkDeviceWaitIdle");
    if (!file_exists_next_to_exe()) { g_status = "nvngx_dlss.dll not found next to primerecomp.exe"; return; }
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", g_dir.c_str(),
                                                              (VkInstance)instance, (VkPhysicalDevice)phys, (VkDevice)device,
                                                              (PFN_vkGetInstanceProcAddr)gipa, (PFN_vkGetDeviceProcAddr)gdpa, &g_info);
    if (NVSDK_NGX_FAILED(r)) {
        char b[96];
        snprintf(b, sizeof b, "NGX init failed (%#x): update the NVIDIA driver?", (unsigned)r);
        g_status = b;
        LOG("[dlss] %s\n", b);
        return;
    }
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&g_caps))) { g_status = "NGX gave no capabilities"; return; }
    int avail = 0, need_update = 0;
    NVSDK_NGX_Parameter_GetI(g_caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &need_update);
    NVSDK_NGX_Parameter_GetI(g_caps, NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
    if (need_update) { g_status = "needs a newer NVIDIA driver"; LOG("[dlss] %s\n", g_status.c_str()); return; }
    if (!avail) { g_status = "not available on this GPU (needs a GeForce RTX) or nvngx_dlss.dll is missing / wrong"; LOG("[dlss] %s\n", g_status.c_str()); return; }
    g_ready = true;
    g_status = "ready";
    LOG("[dlss] ready (nvngx_dlss.dll from %ls)\n", g_dir.c_str());
}

void dlss_shutdown(void *device) {
    release_feature();
    if (g_ready) NVSDK_NGX_VULKAN_Shutdown1((VkDevice)device);
    g_ready = false;
    g_tried = false;
}

void dlss_jitter(uint32_t in_w, uint32_t out_w, float *x, float *y) {
    static uint32_t frame;
    float ratio = in_w ? (float)out_w / (float)in_w : 1.0f;
    uint32_t phases = (uint32_t)(8.0f * ratio * ratio + 0.5f);
    if (phases < 8) phases = 8;
    if (phases > 256) phases = 256;
    uint32_t i = frame++ % phases + 1;
    *x = halton(i, 2) - 0.5f;
    *y = halton(i, 3) - 0.5f;
}

bool dlss_evaluate(const DlssEval *e) {
    if (!g_ready) return false;
    VkCommandBuffer cb = (VkCommandBuffer)e->cmd;
    int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                (e->depth_inverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0);
    bool reset = e->reset;
    if (!g_feature || g_in_w != e->in_w || g_in_h != e->in_h || g_out_w != e->out_w || g_out_h != e->out_h || g_flags != flags) {
        release_feature();
        if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_AllocateParameters(&g_params))) { g_ready = false; g_status = "NGX: no parameters"; return false; }
        NVSDK_NGX_DLSS_Create_Params cp = {};
        cp.Feature.InWidth = e->in_w; cp.Feature.InHeight = e->in_h;
        cp.Feature.InTargetWidth = e->out_w; cp.Feature.InTargetHeight = e->out_h;
        float r = (float)e->in_w / (float)e->out_w;  // the quality class the model is tuned for (any ratio works)
        cp.Feature.InPerfQualityValue = r > 0.95f ? NVSDK_NGX_PerfQuality_Value_DLAA
                                      : r > 0.70f ? NVSDK_NGX_PerfQuality_Value_MaxQuality
                                      : r > 0.52f ? NVSDK_NGX_PerfQuality_Value_Balanced
                                      : r > 0.40f ? NVSDK_NGX_PerfQuality_Value_MaxPerf
                                                  : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        cp.InFeatureCreateFlags = flags;
        NVSDK_NGX_Result res = NGX_VULKAN_CREATE_DLSS_EXT1(g_dev, cb, 1, 1, &g_feature, g_params, &cp);
        if (NVSDK_NGX_FAILED(res)) {
            char b[96];
            snprintf(b, sizeof b, "creating the DLSS feature failed (%#x)", (unsigned)res);
            g_status = b;
            LOG("[dlss] %s: %ux%u -> %ux%u\n", b, e->in_w, e->in_h, e->out_w, e->out_h);
            g_feature = nullptr;
            g_ready = false;  // (back to FSR 1 for good: this would only repeat every frame)
            return false;
        }
        LOG("[dlss] feature %ux%u -> %ux%u%s\n", e->in_w, e->in_h, e->out_w, e->out_h, e->depth_inverted ? ", depth inverted" : "");
        g_in_w = e->in_w; g_in_h = e->in_h; g_out_w = e->out_w; g_out_h = e->out_h; g_flags = flags;
        reset = true;
    }
    VkImageSubresourceRange col = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageSubresourceRange dep = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    NVSDK_NGX_Resource_VK rc = NVSDK_NGX_Create_ImageView_Resource_VK((VkImageView)e->color_view, (VkImage)e->color_image, col, (VkFormat)e->color_format, e->in_w, e->in_h, false);
    NVSDK_NGX_Resource_VK rd = NVSDK_NGX_Create_ImageView_Resource_VK((VkImageView)e->depth_view, (VkImage)e->depth_image, dep, (VkFormat)e->depth_format, e->in_w, e->in_h, false);
    NVSDK_NGX_Resource_VK rm = NVSDK_NGX_Create_ImageView_Resource_VK((VkImageView)e->mv_view, (VkImage)e->mv_image, col, (VkFormat)e->mv_format, e->in_w, e->in_h, false);
    NVSDK_NGX_Resource_VK ro = NVSDK_NGX_Create_ImageView_Resource_VK((VkImageView)e->out_view, (VkImage)e->out_image, col, (VkFormat)e->out_format, e->out_w, e->out_h, true);
    NVSDK_NGX_VK_DLSS_Eval_Params ep = {};
    ep.Feature.pInColor = &rc;
    ep.Feature.pInOutput = &ro;
    ep.pInDepth = &rd;
    ep.pInMotionVectors = &rm;
    ep.InJitterOffsetX = e->jitter_x;
    ep.InJitterOffsetY = e->jitter_y;
    ep.InRenderSubrectDimensions.Width = e->in_w;
    ep.InRenderSubrectDimensions.Height = e->in_h;
    ep.InReset = reset ? 1 : 0;
    ep.InMVScaleX = 1.0f;
    ep.InMVScaleY = 1.0f;
    ep.InPreExposure = 1.0f;
    ep.InExposureScale = 1.0f;
    NVSDK_NGX_Result res = NGX_VULKAN_EVALUATE_DLSS_EXT(cb, g_feature, g_params, &ep);
    if (NVSDK_NGX_FAILED(res)) {
        static int logged;
        if (logged++ < 5) LOG("[dlss] evaluate failed: %#x\n", (unsigned)res);
        return false;
    }
    return true;
}

}  // extern "C"

