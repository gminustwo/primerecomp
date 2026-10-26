// Vulkan textures: NVN formats, images, change-detected uploads from guest memory, views and samplers
// (the texture half of LAS Recomp's Metal renderer, ported).
#include <SDL3/SDL_timer.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "vk.h"
#include "../astc.h"
#include "../renderer.h"
#include "../../settings.h"

// ================================================================== formats
typedef struct { VkFormat vf; uint8_t expand_rgb, alpha_one; } VkFmt;

// DEPTH24_STENCIL8 as itself where the GPU has it for everything the game does with depth (NVIDIA, Intel): half the
// bytes of D32_SFLOAT_S8_UINT (4 instead of 8 per pixel) for every depth test, clear and copy - the game copies its
// full-size depth twice a frame. AMD has no D24S8 attachments: D32S8 there (and on Metal). LARECOMP_VK_D24=0: D32S8.
static VkFormat depth24_stencil8(void) {
    static VkFormat f;
    if (!f) {
        const char *e = getenv("LARECOMP_VK_D24");
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, VK_FORMAT_D24_UNORM_S8_UINT, &p);
        VkFormatFeatureFlags want = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                    VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        bool ok = (p.optimalTilingFeatures & want) == want && !(e && !atoi(e));
        f = ok ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
        LOG("[vk] depth24 stencil8: %s\n", ok ? "D24_UNORM_S8_UINT" : "D32_SFLOAT_S8_UINT");
    }
    return f;
}

static VkFmt fmt_of(uint32_t f) {
    switch (f) {
    case 0x01: return (VkFmt){VK_FORMAT_R8_UNORM};
    case 0x02: return (VkFmt){VK_FORMAT_R8_SNORM};
    case 0x03: return (VkFmt){VK_FORMAT_R8_UINT};
    case 0x04: return (VkFmt){VK_FORMAT_R8_SINT};
    case 0x05: return (VkFmt){VK_FORMAT_R16_SFLOAT};
    case 0x06: return (VkFmt){VK_FORMAT_R16_UNORM};
    case 0x07: return (VkFmt){VK_FORMAT_R16_SNORM};
    case 0x08: return (VkFmt){VK_FORMAT_R16_UINT};
    case 0x09: return (VkFmt){VK_FORMAT_R16_SINT};
    case 0x0A: return (VkFmt){VK_FORMAT_R32_SFLOAT};
    case 0x0B: return (VkFmt){VK_FORMAT_R32_UINT};
    case 0x0C: return (VkFmt){VK_FORMAT_R32_SINT};
    case 0x0D: return (VkFmt){VK_FORMAT_R8G8_UNORM};
    case 0x0E: return (VkFmt){VK_FORMAT_R8G8_SNORM};
    case 0x0F: return (VkFmt){VK_FORMAT_R8G8_UINT};
    case 0x10: return (VkFmt){VK_FORMAT_R8G8_SINT};
    case 0x11: return (VkFmt){VK_FORMAT_R16G16_SFLOAT};
    case 0x12: return (VkFmt){VK_FORMAT_R16G16_UNORM};
    case 0x13: return (VkFmt){VK_FORMAT_R16G16_SNORM};
    case 0x14: return (VkFmt){VK_FORMAT_R16G16_UINT};
    case 0x15: return (VkFmt){VK_FORMAT_R16G16_SINT};
    case 0x16: return (VkFmt){VK_FORMAT_R32G32_SFLOAT};
    case 0x17: return (VkFmt){VK_FORMAT_R32G32_UINT};
    case 0x18: return (VkFmt){VK_FORMAT_R32G32_SINT};
    case 0x19: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 1, 1};  // RGB8: expanded on upload
    case 0x25: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM};
    case 0x26: return (VkFmt){VK_FORMAT_R8G8B8A8_SNORM};
    case 0x27: return (VkFmt){VK_FORMAT_R8G8B8A8_UINT};
    case 0x28: return (VkFmt){VK_FORMAT_R8G8B8A8_SINT};
    case 0x29: return (VkFmt){VK_FORMAT_R16G16B16A16_SFLOAT};
    case 0x2A: return (VkFmt){VK_FORMAT_R16G16B16A16_UNORM};
    case 0x2B: return (VkFmt){VK_FORMAT_R16G16B16A16_SNORM};
    case 0x2C: return (VkFmt){VK_FORMAT_R16G16B16A16_UINT};
    case 0x2D: return (VkFmt){VK_FORMAT_R16G16B16A16_SINT};
    case 0x2E: return (VkFmt){VK_FORMAT_R32G32B32A32_SFLOAT};
    case 0x2F: return (VkFmt){VK_FORMAT_R32G32B32A32_UINT};
    case 0x30: return (VkFmt){VK_FORMAT_R32G32B32A32_SINT};
    case 0x31: return (VkFmt){VK_FORMAT_S8_UINT};
    case 0x32: return (VkFmt){VK_FORMAT_D16_UNORM};
    case 0x33: return (VkFmt){VK_FORMAT_D32_SFLOAT};  // DEPTH24 (as on Metal: no 24-bit depth there)
    case 0x34: return (VkFmt){VK_FORMAT_D32_SFLOAT};
    case 0x35: return (VkFmt){depth24_stencil8()};
    case 0x36: return (VkFmt){VK_FORMAT_D32_SFLOAT_S8_UINT};
    case 0x37: return (VkFmt){VK_FORMAT_R8G8B8A8_SRGB, 0, 1};
    case 0x38: return (VkFmt){VK_FORMAT_R8G8B8A8_SRGB};
    case 0x39: return (VkFmt){VK_FORMAT_R4G4B4A4_UNORM_PACK16};   // Metal ABGR4Unorm
    case 0x3A: return (VkFmt){VK_FORMAT_R5G6B5_UNORM_PACK16};     // Metal B5G6R5Unorm
    case 0x3B: return (VkFmt){VK_FORMAT_R5G5B5A1_UNORM_PACK16};   // Metal A1BGR5Unorm
    case 0x3C: return (VkFmt){VK_FORMAT_R5G6B5_UNORM_PACK16};
    case 0x3D: return (VkFmt){VK_FORMAT_A2B10G10R10_UNORM_PACK32};
    case 0x3E: return (VkFmt){VK_FORMAT_A2B10G10R10_UINT_PACK32};
    case 0x3F: return (VkFmt){VK_FORMAT_B10G11R11_UFLOAT_PACK32};
    case 0x40: return (VkFmt){VK_FORMAT_E5B9G9R9_UFLOAT_PACK32};
    case 0x41: case 0x42: return (VkFmt){VK_FORMAT_BC1_RGBA_UNORM_BLOCK};
    case 0x43: return (VkFmt){VK_FORMAT_BC2_UNORM_BLOCK};
    case 0x44: return (VkFmt){VK_FORMAT_BC3_UNORM_BLOCK};
    case 0x45: case 0x46: return (VkFmt){VK_FORMAT_BC1_RGBA_SRGB_BLOCK};
    case 0x47: return (VkFmt){VK_FORMAT_BC2_SRGB_BLOCK};
    case 0x48: return (VkFmt){VK_FORMAT_BC3_SRGB_BLOCK};
    case 0x49: return (VkFmt){VK_FORMAT_BC4_UNORM_BLOCK};
    case 0x4A: return (VkFmt){VK_FORMAT_BC4_SNORM_BLOCK};
    case 0x4B: return (VkFmt){VK_FORMAT_BC5_UNORM_BLOCK};
    // 0x4C = signed RGTC2; LARECOMP_BC5_UNORM=1 decodes it unsigned
    case 0x4C: return (VkFmt){getenv("LARECOMP_BC5_UNORM") ? VK_FORMAT_BC5_UNORM_BLOCK : VK_FORMAT_BC5_SNORM_BLOCK};
    case 0x4D: return (VkFmt){VK_FORMAT_BC7_UNORM_BLOCK};
    case 0x4E: return (VkFmt){VK_FORMAT_BC7_SRGB_BLOCK};
    case 0x4F: return (VkFmt){VK_FORMAT_BC6H_SFLOAT_BLOCK};
    case 0x50: return (VkFmt){VK_FORMAT_BC6H_UFLOAT_BLOCK};
    case 0x6D: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 0, 1};
    case 0x71: return (VkFmt){VK_FORMAT_R16G16B16A16_SFLOAT, 0, 1};
    case 0x76: return (VkFmt){VK_FORMAT_R32G32B32A32_SFLOAT, 0, 1};
    case 0x79: return (VkFmt){VK_FORMAT_ASTC_4x4_UNORM_BLOCK};
    case 0x7A: return (VkFmt){VK_FORMAT_ASTC_5x4_UNORM_BLOCK};
    case 0x7B: return (VkFmt){VK_FORMAT_ASTC_5x5_UNORM_BLOCK};
    case 0x7C: return (VkFmt){VK_FORMAT_ASTC_6x5_UNORM_BLOCK};
    case 0x7D: return (VkFmt){VK_FORMAT_ASTC_6x6_UNORM_BLOCK};
    case 0x7E: return (VkFmt){VK_FORMAT_ASTC_8x5_UNORM_BLOCK};
    case 0x7F: return (VkFmt){VK_FORMAT_ASTC_8x6_UNORM_BLOCK};
    case 0x80: return (VkFmt){VK_FORMAT_ASTC_8x8_UNORM_BLOCK};
    case 0x81: return (VkFmt){VK_FORMAT_ASTC_10x5_UNORM_BLOCK};
    case 0x82: return (VkFmt){VK_FORMAT_ASTC_10x6_UNORM_BLOCK};
    case 0x83: return (VkFmt){VK_FORMAT_ASTC_10x8_UNORM_BLOCK};
    case 0x84: return (VkFmt){VK_FORMAT_ASTC_10x10_UNORM_BLOCK};
    case 0x85: return (VkFmt){VK_FORMAT_ASTC_12x10_UNORM_BLOCK};
    case 0x86: return (VkFmt){VK_FORMAT_ASTC_12x12_UNORM_BLOCK};
    case 0x87: return (VkFmt){VK_FORMAT_ASTC_4x4_SRGB_BLOCK};
    case 0x88: return (VkFmt){VK_FORMAT_ASTC_5x4_SRGB_BLOCK};
    case 0x89: return (VkFmt){VK_FORMAT_ASTC_5x5_SRGB_BLOCK};
    case 0x8A: return (VkFmt){VK_FORMAT_ASTC_6x5_SRGB_BLOCK};
    case 0x8B: return (VkFmt){VK_FORMAT_ASTC_6x6_SRGB_BLOCK};
    case 0x8C: return (VkFmt){VK_FORMAT_ASTC_8x5_SRGB_BLOCK};
    case 0x8D: return (VkFmt){VK_FORMAT_ASTC_8x6_SRGB_BLOCK};
    case 0x8E: return (VkFmt){VK_FORMAT_ASTC_8x8_SRGB_BLOCK};
    case 0x8F: return (VkFmt){VK_FORMAT_ASTC_10x5_SRGB_BLOCK};
    case 0x90: return (VkFmt){VK_FORMAT_ASTC_10x6_SRGB_BLOCK};
    case 0x91: return (VkFmt){VK_FORMAT_ASTC_10x8_SRGB_BLOCK};
    case 0x92: return (VkFmt){VK_FORMAT_ASTC_10x10_SRGB_BLOCK};
    case 0x93: return (VkFmt){VK_FORMAT_ASTC_12x10_SRGB_BLOCK};
    case 0x94: return (VkFmt){VK_FORMAT_ASTC_12x12_SRGB_BLOCK};
    case 0x95: case 0x96: case 0x97: return (VkFmt){VK_FORMAT_R5G5B5A1_UNORM_PACK16};
    case 0x98: return (VkFmt){VK_FORMAT_B8G8R8A8_UNORM, 0, 1};
    case 0x99: return (VkFmt){VK_FORMAT_B8G8R8A8_UNORM};
    case 0x9A: return (VkFmt){VK_FORMAT_B8G8R8A8_SRGB, 0, 1};
    case 0x9B: return (VkFmt){VK_FORMAT_B8G8R8A8_SRGB};
    default: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM};
    }
}

bool renderer_format_is_depth(uint32_t f) { return f >= 0x31 && f <= 0x36; }
bool renderer_format_has_stencil(uint32_t f) { return f == 0x31 || f == 0x35 || f == 0x36; }
VkFormat vk_format(uint32_t f) { return fmt_of(f).vf; }
bool renderer_format_alpha_one(uint32_t f) { return fmt_of(f).alpha_one; }

static VkImageAspectFlags aspect_of(uint32_t f) {
    if (f == 0x31) return VK_IMAGE_ASPECT_STENCIL_BIT;
    if (f == 0x35 || f == 0x36) return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (renderer_format_is_depth(f)) return VK_IMAGE_ASPECT_DEPTH_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

static bool format_supports(VkFormat f, VkFormatFeatureFlags want) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    return (p.optimalTilingFeatures & want) == want;
}

// ================================================================== output resolution / per-frame clears
// (The rules of LAS Recomp's Metal renderer.)
static float g_rs_x = 1, g_rs_y = 1;
void renderer_set_scale(float sx, float sy) { g_rs_x = sx; g_rs_y = sy; }
uint64_t renderer_present_count(void);

static bool tex_scalable(const NvnTexture *t) {
    if (getenv("LARECOMP_NO_SCALE")) return false;
    if (t->target != 1 || t->samples > 1 || !(t->flags & 8) || t->width < 32 || t->height < 16) return false;
    float a = (float)t->width / (float)t->height;
    return a > 1.74f && a < 1.81f;
}

bool renderer_texture_first_write(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    uint64_t total_frames = renderer_present_count();
    if (!b || !b->presented || (b->sx == b->sy) || b->touched_frame == total_frames + 1) return false;
    b->touched_frame = total_frames + 1;
    return true;
}

static uint32_t g_level_gen = 1;
void vk_level_change_check(void) {
    extern const char *game_level_name(void);
    static char last[64];
    const char *lv = game_level_name();
    if (strncmp(lv, last, sizeof last - 1) != 0) {
        strlcpy(last, lv, sizeof last);
        if (getenv("LARECOMP_CLEAR_ALL_LEVELS")) g_level_gen++;
    }
}

bool renderer_texture_frame_fresh(NvnTexture *t) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_NO_FRAME_CLEAR") == NULL;
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    uint64_t total_frames = renderer_present_count();
    if (!on || !b || !b->scalable || b->frame_cleared == total_frames + 1) return false;
    b->frame_cleared = total_frames + 1;
    return true;
}

bool renderer_texture_level_fresh(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    static int keep = -1;
    if (keep < 0) keep = getenv("LARECOMP_KEEP_TARGETS") != NULL;
    if (!b || keep || b->level_gen == g_level_gen) return false;
    if (!b->scalable) { b->level_gen = g_level_gen; return false; }
    bool fresh = b->level_gen != 0;
    b->level_gen = g_level_gen;
    return fresh;
}

void renderer_texture_mark_presented(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (b) b->presented = true;
}

bool renderer_texture_presented(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    return b && b->presented;
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (b && b->scalable) { *sx = b->sx; *sy = b->sy; }
    else { *sx = 1; *sy = 1; }
}

// ================================================================== images
void vk_image_init_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .image = img,
                               .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

// Images destroyed by the guest are kept until the submissions that may use them have completed.
typedef struct { VkImage img; VmaAllocation mem; VkImageView view; uint64_t seq; } Dead;
static Dead *dead;
static int ndead, cap_dead;
static void views_forget(TexBackend *b);

void vk_collect_dead(void) {
    uint64_t done = vk_completed_seq();
    int k = 0;
    for (int i = 0; i < ndead; i++) {
        if (dead[i].seq > done) { dead[k++] = dead[i]; continue; }
        if (dead[i].view) vkDestroyImageView(g_vk.dev, dead[i].view, NULL);
        else vmaDestroyImage(g_vk.vma, dead[i].img, dead[i].mem);
    }
    ndead = k;
}

static void create_image(NvnTexture *t, TexBackend *b) {
    VkFmt mf = fmt_of(t->format);
    b->sx = b->scalable ? g_rs_x : 1;
    b->sy = b->scalable ? g_rs_y : 1;
    uint32_t tw = t->width ? t->width : 1, th = t->height ? t->height : 1;
    if (b->scalable) {
        tw = (uint32_t)lroundf(tw * b->sx); th = (uint32_t)lroundf(th * b->sy);
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
    }
    VkImageType type = VK_IMAGE_TYPE_2D;
    uint32_t layers = 1, depth = 1;
    switch (t->target) {
    case 0: type = VK_IMAGE_TYPE_1D; th = 1; break;
    case 2: type = VK_IMAGE_TYPE_3D; depth = t->depth ? t->depth : 1; break;
    case 3: type = VK_IMAGE_TYPE_1D; layers = t->height ? t->height : 1; th = 1; break;
    case 4: case 6: layers = t->depth ? t->depth : 1; break;
    case 8: layers = 6; break;
    case 9: layers = t->depth ? t->depth : 6; break;
    default: break;
    }
    uint32_t levels = t->levels ? t->levels : 1;
    {   // never more levels than the size allows (scaled targets, odd guest values)
        uint32_t m = 1, sz = tw > th ? tw : th;
        if (depth > sz) sz = depth;
        while (sz > 1) { sz >>= 1; m++; }
        if (levels > m) levels = m;
    }
    uint32_t samples = t->samples > 1 ? t->samples : 1;
    if (samples > 1) levels = 1;
    VkFormat vf = mf.vf;
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    b->astc_bw = b->astc_bh = 0;
    if (!format_supports(vf, need)) {
        bool astc = vf >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && vf <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
        const FormatInfo *fi = t->layout.fmt;
        if (astc && fi && fi->bpb == 16) {  // desktop GPUs: decoded on the CPU into an RGBA8 image of the same colour space
            b->astc_bw = fi->bw;
            b->astc_bh = fi->bh;
            b->astc_srgb = t->format >= 0x87;
            vf = b->astc_srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        } else {
            static int nlog;
            if (nlog++ < 20) LOG("[vk] format %d (nvn %#x) not supported: RGBA8 instead\n", vf, t->format);
            vf = VK_FORMAT_R8G8B8A8_UNORM;
        }
    }
    bool depthfmt = renderer_format_is_depth(t->format);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (depthfmt) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    else if (format_supports(vf, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (!depthfmt && format_supports(vf, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) usage |= VK_IMAGE_USAGE_STORAGE_BIT;  // (SUST)
    VkImageCreateFlags flags = 0;
    if (!depthfmt) flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;  // format views (NVN texture views, raw sRGB views)
    if (type == VK_IMAGE_TYPE_2D && layers >= 6 && tw == th && samples == 1) flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags, .imageType = type, .format = vf,
                            .extent = {tw, th, depth}, .mipLevels = levels, .arrayLayers = layers,
                            .samples = (VkSampleCountFlagBits)samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkResult r = vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL);
    if (r != VK_SUCCESS) {
        LOG("[vk] image creation failed (%d): nvn fmt %#x %ux%ux%u target %u\n", r, t->format, t->width, t->height, t->depth, t->target);
        ci.format = vf = VK_FORMAT_R8G8B8A8_UNORM;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.flags &= ~VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL));
        depthfmt = false;
    }
    b->format = vf;
    b->width = tw; b->height = th; b->depth = depth; b->layers = layers; b->levels = levels; b->samples = samples;
    b->aspect = depthfmt ? aspect_of(t->format) : VK_IMAGE_ASPECT_COLOR_BIT;
    b->base_type = type == VK_IMAGE_TYPE_1D ? (layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D)
                 : type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                 : t->target == 8 ? VK_IMAGE_VIEW_TYPE_CUBE : t->target == 9 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
                 : layers > 1 || t->target == 4 || t->target == 6 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    VkCommandBuffer icb = vk_upload_cmd();
    vk_image_init_layout(icb, b->image, b->aspect);
    bool cleared = false;
    if (depthfmt) {
        // New depth / stencil images start zeroed (as fresh memory reads on Apple GPUs). The game builds its depth
        // pyramid with depth-only copies, so the stencil of those images was never written: whatever the memory
        // held, different per image and per run, then failed or passed the depth-of-field passes' stencil test in
        // patches (flicker that came and went between runs, every other frame, below 1x internal resolution).
        VkClearDepthStencilValue zero = {0.0f, 0};
        VkImageSubresourceRange rg = {b->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearDepthStencilImage(icb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
        cleared = true;
    } else if (!(vf >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && vf <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) && samples == 1) {
        // Colour images too: a level or layer nothing writes (the cube faces a geometry shader picks, before that
        // was emulated) read uninitialized memory - garbage colours over whole rooms. (Compressed formats can't
        // be cleared; they are uploaded whole.)
        VkClearColorValue zero = {0};
        VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearColorImage(icb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
        cleared = true;
    }
    if (cleared) {
        VkCommandBuffer cb = icb;
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(cb, &di);
    }
}

static void defer(Dead d) {
    if (ndead == cap_dead) { cap_dead = cap_dead ? cap_dead * 2 : 256; dead = realloc(dead, cap_dead * sizeof *dead); }
    d.seq = vk_pending_seq();
    dead[ndead++] = d;
}

void vk_defer_image(VkImage img, VmaAllocation mem) { defer((Dead){.img = img, .mem = mem}); }

void vk_dl_note_upload(void *img, uint32_t w, uint32_t h, uint32_t lv);  // (exec.c: LARECOMP_VK_DEVLOST_TRACE)

static uint32_t level_w(const TexBackend *b, uint32_t lv) { uint32_t v = b->width >> lv; return v ? v : 1; }
static uint32_t level_h(const TexBackend *b, uint32_t lv) { uint32_t v = b->height >> lv; return v ? v : 1; }

static void destroy_image(TexBackend *b) {
    views_forget(b);
    if (b->image || b->mem) defer((Dead){.img = b->image, .mem = b->mem});  // (never the same allocation twice)
    b->image = VK_NULL_HANDLE;
    b->mem = VK_NULL_HANDLE;
}

// Live game textures: an in-place reboot (vk_textures_reset) destroys them all.
static NvnTexture **g_live;
static int g_nlive, g_cap_live;
static int g_nimages;  // TexBackends (images) alive
int vk_live_textures(void) { return g_nlive; }
int vk_live_images(void) { return g_nimages; }

static void live_add(NvnTexture *t) {
    if (g_nlive == g_cap_live) { g_cap_live = g_cap_live ? g_cap_live * 2 : 1024; g_live = realloc(g_live, g_cap_live * sizeof *g_live); }
    g_live[g_nlive++] = t;
    t->live_slot = g_nlive;
}
static void live_remove(NvnTexture *t) {
    if (!t->live_slot) return;
    NvnTexture *last = g_live[--g_nlive];
    g_live[t->live_slot - 1] = last;
    last->live_slot = t->live_slot;
    t->live_slot = 0;
}

// The game makes several texture objects over one allocation: MPR renders its refraction mip chain (the blue doors'
// shield samples it) one level per object and samples it through yet another. Each with an image of its own, the
// levels never met. Objects with the same storage and shape now share one image; other aliasing (another format or
// size over the same memory) still gets separate images.
#define SHARE_TAB 4096
static TexBackend *share_tab[SHARE_TAB];
static uint32_t share_slot(uint64_t addr) { return (uint32_t)((addr >> 9) * 0x9E3779B97F4A7C15ull >> 52) & (SHARE_TAB - 1); }

// gpu_write: t's first use writes it (a render target, a copy or dispatch destination).
static TexBackend *share_find(const NvnTexture *t, TexBackend *key_out, bool gpu_write) {
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_NO_TEX_SHARE") != NULL;  // (debug) every texture object its own image
    memset(&key_out->key, 0, sizeof key_out->key);  // (compared with memcmp)
    key_out->shared = false;
    if (off || !t->addr || t->target == 10 || (t->flags & 2) || (t->pool && (t->pool->flags & NVN_POOL_VIRTUAL))) return NULL;
    // GPU-written images only: a texture loaded from disc into memory an older texture object of the same shape
    // still covers must get its own image and upload (sharing left stale and half-updated texels: garbage
    // textures in the room after the first door). Compressed formats are never render targets.
    // The other way round joins: an object about to be rendered into takes over an uploaded image of its shape (that
    // image becomes GPU-written). MPR samples its reflection probe blend target (the arm cannon's and Samus's
    // reflections between two probes) through an object made, and uploaded from memory, before the blend first
    // renders: with an image of its own the blend never reached it - black reflections (user 2026-10-23).
    const FormatInfo *fi = nvn_format_info(t->format);
    if (!fi || fi->bw > 1 || fi->bh > 1) return NULL;
    key_out->key.addr = t->addr;
    key_out->key.stride = t->stride;
    key_out->key.target = t->target;
    key_out->key.scalable = tex_scalable(t);  // (flags differ between the objects; only this depends on them)
    key_out->key.width = t->width;
    key_out->key.height = t->height;
    key_out->key.depth = t->depth;
    key_out->key.levels = t->levels;
    key_out->key.format = t->format;
    key_out->key.samples = t->samples;
    key_out->shared = true;
    for (TexBackend *b = share_tab[share_slot(t->addr)]; b; b = b->share_next) {
        if (memcmp(&b->key, &key_out->key, sizeof b->key)) continue;
        if (gpu_write || !(b->uploaded && !b->gpu_owned)) return b;
        static int log_share = -1;
        if (log_share < 0) log_share = getenv("LARECOMP_LOG_TEX_SHARE") != NULL;
        if (log_share)
            LOG("[share] frame %llu %p @%llx %ux%u f%#x target %u: an uploaded image of that shape not joined\n",
                (unsigned long long)renderer_present_count(), (void *)t, (unsigned long long)t->addr, t->width, t->height,
                t->format, t->target);
    }
    return NULL;
}
static void share_unlink(TexBackend *b) {
    if (!b->shared) return;
    for (TexBackend **p = &share_tab[share_slot(b->key.addr)]; *p; p = &(*p)->share_next)
        if (*p == b) { *p = b->share_next; break; }
    b->shared = false;
}

static TexBackend *tex_backend_for(NvnTexture *t, bool gpu_write) {
    TexBackend *b = t->backend;
    if (b) {
        if (b->scalable && (b->sx != g_rs_x || b->sy != g_rs_y)) {  // output resolution changed: reallocate
            destroy_image(b);
            create_image(t, b);
        }
        return b;
    }
    vk_collect_dead();
    TexBackend key;
    b = share_find(t, &key, gpu_write);
    if (b) {
        static int log_share = -1;
        if (log_share < 0) log_share = getenv("LARECOMP_LOG_TEX_SHARE") != NULL;
        if (log_share)
            LOG("[share] frame %llu %p joins @%llx %ux%u f%#x flags %#x levels %u: refs %d, %s%s\n",
                (unsigned long long)renderer_present_count(), (void *)t, (unsigned long long)t->addr, t->width, t->height,
                t->format, t->flags, t->levels, b->refs, b->gpu_owned ? "gpu" : "", b->uploaded ? " uploaded" : "");
        b->refs++;
    } else {
        b = calloc(1, sizeof *b);
        b->scalable = tex_scalable(t);
        create_image(t, b);
        b->refs = 1;
        g_nimages++;
        if (key.shared) {
            b->key = key.key;
            b->shared = true;
            uint32_t s = share_slot(t->addr);
            b->share_next = share_tab[s];
            share_tab[s] = b;
        }
    }
    t->backend = b;
    live_add(t);
    return b;
}
static TexBackend *tex_backend(NvnTexture *t) { return tex_backend_for(t, false); }

void backend_texture_destroyed(NvnTexture *t) {
    TexBackend *b = t->backend;
    if (b) {
        extern void vk_alias_texture_destroyed(NvnTexture *t);  // (exec.c copy aliases: a source's contents handed over)
        vk_alias_texture_destroyed(t);
        live_remove(t);
        if (--b->refs == 0) {
            g_nimages--;
            share_unlink(b);
            destroy_image(b);
            free(b->pending_rgba);
            free(b->sparse);
            free(b);
        }
    }
    t->backend = NULL;
}

// Reboot (device.c backend_reset; GPU idle): every game texture and its views go.
void vk_textures_reset(void) {
    int n = g_nlive;
    extern void vk_alias_reset(void);
    vk_alias_reset();
    while (g_nlive > 0) backend_texture_destroyed(g_live[g_nlive - 1]);
    vk_collect_dead();
    LOG("[vk] reboot: %d textures released\n", n);
}

// ================================================================== uploads
static uint64_t full_hash_budget, full_hash_budget_frame;

// Four independent lanes (one serial multiply chain ran at ~1.5 bytes/cycle; dynamic textures are hashed every
// frame). Values are only compared within a run.
static uint64_t hash_full(const uint8_t *p, uint64_t n) {
    uint64_t h0 = 0x9E3779B97F4A7C15ull ^ n, h1 = 0xC2B2AE3D27D4EB4Full, h2 = 0x165667B19E3779F9ull, h3 = 0x27D4EB2F165667C5ull;
    uint64_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t v[4];
        memcpy(v, p + i, 32);
        h0 = (h0 ^ v[0]) * 0x100000001B3ull; h0 ^= h0 >> 29;
        h1 = (h1 ^ v[1]) * 0x100000001B3ull; h1 ^= h1 >> 29;
        h2 = (h2 ^ v[2]) * 0x100000001B3ull; h2 ^= h2 >> 29;
        h3 = (h3 ^ v[3]) * 0x100000001B3ull; h3 ^= h3 >> 29;
    }
    uint64_t h = h0 ^ (h1 * 0x9E3779B97F4A7C15ull) ^ (h2 * 0xC2B2AE3D27D4EB4Full) ^ (h3 * 0x165667B19E3779F9ull);
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ (h >> 31);
}

static uint64_t fnv_sample(const uint8_t *p, uint64_t n) {
    uint64_t h = 1469598103934665603ull;
    uint64_t step = n > 4096 ? n / 4096 : 1;
    for (uint64_t i = 0; i < n; i += step) { h ^= p[i]; h *= 1099511628211ull; }
    return h ^ n;
}

static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;

static void upload_pending(TexBackend *b) {
    pthread_mutex_lock(&pending_lock);
    void *px = b->pending_rgba;
    uint32_t w = b->pending_w, h = b->pending_h, stride = b->pending_stride;
    b->pending_rgba = NULL;
    pthread_mutex_unlock(&pending_lock);
    if (!px) return;
    if (w > b->width) w = b->width;
    if (h > b->height) h = b->height;
    VkSpan s = vk_transient_copy(px, (uint64_t)stride * h, 16);
    free(px);
    VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = stride / 4,
                            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    VkCommandBuffer cb = vk_upload_cmd();
    vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
}

// Sparse textures (storage in a virtual pool, objects.c nvnMemoryPoolMapVirtual): may the bytes have changed since
// the last gather? 0 no, 1 yes, -1 unknown (mapped pages the write tracking doesn't cover).
static int sparse_changed(NvnTexture *t, TexBackend *b, uint64_t total, uint64_t frame) {
    extern int vk_pages_changed(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now);
    NvnPool *p = t->pool;
    if (!p->pages) return 0;  // nothing mapped yet
    int ch = b->sparse_gen != p->map_gen;
    b->sparse_gen = p->map_gen;
    if (ch) b->sparse_hot = frame + 64;  // (the game fills the pages it just mapped: upload_texture)
    uint32_t now = b->wgen, since = b->wgen;
    for (uint64_t o = t->pool_offset & ~(uint64_t)(NVN_SPARSE_PAGE - 1); o < t->pool_offset + total; o += NVN_SPARSE_PAGE) {
        uint64_t a = p->pages[o / NVN_SPARSE_PAGE];
        if (!a) continue;
        int c = vk_pages_changed(a, NVN_SPARSE_PAGE, since, &now);
        if (c < 0 && ch == 0) ch = -1;
        if (c > 0) ch = 1;
    }
    b->wgen = now;
    return ch;
}

// Without write tracking (Linux, macOS; sparse_changed -1) a sample of the mapped bytes, read in place, decides
// whether to look closer: 8-byte words at least 8 to a page (unmapped: zero). Gathering and hashing every sparse
// texture whole at each check cost ~5 ms a frame on an i9 in the Landing Site - several times that on a Steam Deck.
static uint64_t sparse_sample(NvnTexture *t, uint64_t total) {
    NvnPool *p = t->pool;
    uint64_t h = 1469598103934665603ull, step = total / 4096;
    if (step > NVN_SPARSE_PAGE / 8) step = NVN_SPARSE_PAGE / 8;
    step = step < 8 ? 8 : step & ~7ull;
    for (uint64_t o = 0; o + 8 <= total; o += step) {
        uint64_t v = (t->pool_offset + o) & ~7ull, a = p->pages ? p->pages[v / NVN_SPARSE_PAGE] : 0, w = 0;
        if (a) memcpy(&w, (const void *)(uintptr_t)(a + v % NVN_SPARSE_PAGE), 8);
        h = (h ^ w) * 1099511628211ull;
    }
    return h ^ total;
}

// ... the closer look: the mapped pages' bytes hashed where they are (no gather, unmapped pages skipped - a mapping
// change is seen by sparse_changed). *bytes: how many that was.
static uint64_t sparse_mapped_hash(NvnTexture *t, uint64_t total, uint64_t *bytes) {
    NvnPool *p = t->pool;
    uint64_t h = 0x9E3779B97F4A7C15ull ^ total, n = 0;
    for (uint64_t o = 0; o < total;) {
        uint64_t v = t->pool_offset + o, in = v % NVN_SPARSE_PAGE, len = NVN_SPARSE_PAGE - in;
        if (len > total - o) len = total - o;
        uint64_t a = p->pages ? p->pages[v / NVN_SPARSE_PAGE] : 0;
        if (a) {
            h = (h ^ hash_full((const uint8_t *)(uintptr_t)(a + in), len) ^ o) * 0x100000001B3ull;
            n += len;
        }
        o += len;
    }
    *bytes = n;
    return h;
}

// ... and the bytes themselves: mapped pages copied, unmapped ones zero (what the hardware returns for them).
static const uint8_t *sparse_gather(NvnTexture *t, TexBackend *b, uint64_t total) {
    NvnPool *p = t->pool;
    if (!b->sparse) b->sparse = malloc(total);
    for (uint64_t o = 0; o < total;) {
        uint64_t v = t->pool_offset + o, in = v % NVN_SPARSE_PAGE, n = NVN_SPARSE_PAGE - in;
        if (n > total - o) n = total - o;
        uint64_t a = p->pages ? p->pages[v / NVN_SPARSE_PAGE] : 0;
        if (a) memcpy(b->sparse + o, (const void *)(uintptr_t)(a + in), n);
        else memset(b->sparse + o, 0, n);
        o += n;
    }
    return b->sparse;
}

uint64_t prof_tex_checks, prof_tex_samples, prof_tex_full, prof_tex_full_bytes, prof_tex_dyn, prof_tex_watch_skips;
// (debug) LARECOMP_BH_PROBE=1: for uploads whose height doesn't fill a power of two GOBs, level 0 decoded with each
// block height up to the one in use and scored - ASTC by its error blocks (only a wrong layout makes them),
// uncompressed by the mean difference between vertically adjacent bytes - to learn the driver's default rule.
// Once per texture address.
static void bh_probe(const uint8_t *src, const LevelLayout *l, uint32_t bpb, uint32_t abw, uint32_t abh, uint32_t fmt) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_BH_PROBE") != NULL;
    if (!on || l->hb <= 8) return;
    uint32_t g = (l->hb + 7) / 8;
    if (!(g & (g - 1)) && l->hb % 8 == 0) return;  // (fills its GOBs exactly: every rule agrees)
    static const uint8_t *seen[4096];
    static int nseen;
    for (int i = 0; i < nseen; i++) if (seen[i] == src) return;
    if (nseen < 4096) seen[nseen++] = src;
    bool astc = abw != 0, bc = !astc && bpb == 16 && fmt >= 0x41 && fmt <= 0x50;
    if (bc || (bpb != 1 && bpb != 2 && bpb != 4 && bpb != 8 && !astc)) return;
    uint32_t row = l->wb * bpb;
    uint8_t *lin = malloc((uint64_t)row * l->hb);
    uint8_t *rgba = astc ? malloc((uint64_t)l->w * l->h * 4) : NULL;
    char line[256];
    int n = snprintf(line, sizeof line, "[bh] %ux%u f%#x rows %u in use %u:", l->w, l->h, fmt, l->hb, l->block_height_log2);
    double best = 1e30;
    int besti = -1;
    for (uint32_t bh = 0; bh <= l->block_height_log2; bh++) {
        tegra_deswizzle(src, lin, l->wb, l->hb, 1, bpb, bh, 0);
        double score = 0;
        if (astc) {
            astc_decode_ldr(lin, l->w, l->h, abw, abh, false, rgba);
            uint64_t bad = 0;
            for (uint64_t i = 0; i < (uint64_t)l->w * l->h; i++)
                bad += rgba[4 * i] == 255 && rgba[4 * i + 1] == 0 && rgba[4 * i + 2] == 255 && rgba[4 * i + 3] == 255;
            score = (double)bad / ((double)l->w * l->h);
        } else {
            uint64_t sum = 0;
            for (uint32_t y = 0; y + 1 < l->hb; y++)
                for (uint32_t x = 0; x < row; x++) sum += (uint64_t)abs((int)lin[y * row + x] - (int)lin[(y + 1) * row + x]);
            score = (double)sum / ((double)row * (l->hb - 1));
        }
        n += snprintf(line + n, sizeof line - n, " %.3f", score);
        if (score < best - 1e-9) best = score, besti = (int)bh;
    }
    LOG("%s -> best %d\n", line, besti);
    free(lin);
    free(rgba);
}

static void upload_texture(NvnTexture *t, TexBackend *b) {
    bool sparse = t->pool && (t->pool->flags & NVN_POOL_VIRTUAL);
    if ((!t->addr && !sparse) || t->target == 10) return;
    const uint8_t *base = (const uint8_t *)(uintptr_t)t->addr;
    uint64_t frame = vk_frame_counter();
    if (b->uploaded && b->checked_frame == frame) return;
    if (b->uploaded && !b->dynamic && ((frame + ((uintptr_t)b >> 6)) & 7)) return;
    const FormatInfo *fi = t->layout.fmt;
    if (!fi || !fi->bpb) return;
    VkFmt mf = fmt_of(t->format);
    uint64_t total = tegra_total_size(&t->layout);
    b->checked_frame = frame;
    prof_tex_checks++;
    if (sparse) {
        int ch = sparse_changed(t, b, total, frame);
        if (!ch && b->uploaded) return;
        // Unknown (no write tracking): the mapped pages are hashed in place (sparse_mapped_hash) at each check for a
        // while after a mapping change - the game fills the pages it just mapped - and now and then as a safety net
        // (every 240 frames per 16 MB mapped); otherwise only when the sample changed. Gathered only when that hash
        // changed.
        extern bool vk_write_tracking(void);
        bool tracked = vk_write_tracking();
        if (ch < 0 && b->uploaded) {
            uint64_t period = 240 * (1 + b->sparse_mapped / (16 << 20));
            if (frame >= b->sparse_hot && frame - b->full_frame < period) {
                prof_tex_samples++;
                if (sparse_sample(t, total) == b->sample_hash) return;
            }
            static int log = -1;  // LARECOMP_LOG_SPARSE: these checks too
            if (log < 0) log = getenv("LARECOMP_LOG_SPARSE") ? 200 : 0;
            uint64_t t0 = log ? SDL_GetTicksNS() : 0, mapped, mh = sparse_mapped_hash(t, total, &mapped);
            if (log) log--, LOG("[sparse] f%llu check %ux%u: %.1f of %.1f MB mapped, %.2f ms%s\n", (unsigned long long)frame,
                                t->width, t->height, mapped / 1048576.0, total / 1048576.0, (SDL_GetTicksNS() - t0) / 1e6,
                                mh == b->sparse_mhash ? "" : ", changed");
            prof_tex_full++; prof_tex_full_bytes += mapped;
            b->full_frame = frame;
            b->sample_hash = sparse_sample(t, total);
            if (mh == b->sparse_mhash) return;
        }
        base = sparse_gather(t, b, total);
        uint64_t sh = hash_full(base, total);
        b->full_frame = frame;
        b->sample_hash = sparse_sample(t, total);
        if (!tracked) b->sparse_mhash = sparse_mapped_hash(t, total, &b->sparse_mapped);
        if (b->uploaded && sh == b->data_hash) return;
        b->data_hash = sh;
        b->uploaded = true;
        goto upload;
    }
    if (t->flags & 0x10) b->dynamic = true;
    if (full_hash_budget_frame != frame) { full_hash_budget_frame = frame; full_hash_budget = 4 << 20; }
    const uint8_t *mem = (const uint8_t *)(uintptr_t)t->addr;
    static int always_full = -1;
    if (always_full < 0) always_full = getenv("LARECOMP_TEX_FULLHASH") ? 1 : 0;
    bool full = b->dynamic || !b->uploaded || always_full;
    bool periodic = frame - b->full_frame >= 120 && full_hash_budget >= total;  // (safety net: a full hash now and then)
    if (!full && periodic) { full = true; full_hash_budget -= total; }
    // The OS's page write tracking (exec.c vk_pages_changed, shared with the vertex cache): texture memory the CPU
    // hasn't written since the last check is not hashed at all (dynamic textures were fully hashed every frame,
    // ~2.4 MB a frame in the overworld). The periodic full hash stays as a safety net. LARECOMP_VK_TEX_WATCH=0: off.
    static int watch = -1;
    if (watch < 0) { const char *e = getenv("LARECOMP_VK_TEX_WATCH"); watch = !e || atoi(e) != 0; }
    if (watch && !always_full) {
        extern int vk_pages_changed(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now);
        uint32_t now = 0;
        int ch = vk_pages_changed(t->addr, total, b->wgen, &now);
        if (ch >= 0) {
            b->wgen = now;
            if (!ch && b->uploaded) {
                if (!periodic) { prof_tex_watch_skips++; return; }
                if (b->dynamic) full_hash_budget -= total;  // (the others took theirs above)
            }
        }
    }
    if (!full) {
        prof_tex_samples++;
        uint64_t sh = fnv_sample(mem, total);
        if (sh == b->sample_hash) return;
    }
    prof_tex_full++; prof_tex_full_bytes += total; if (b->dynamic) prof_tex_dyn++;
    uint64_t h = hash_full(mem, total);
    b->full_frame = frame;
    b->sample_hash = fnv_sample(mem, total);
    if (b->uploaded && h == b->data_hash) return;
    if (b->uploaded && h != b->data_hash) b->dynamic = true;
    b->data_hash = h;
    b->uploaded = true;
upload:
    if (b->format != mf.vf && !b->astc_bw) return;  // unsupported format: nothing sensible to copy
    VkCommandBuffer cb = vk_upload_cmd();
    uint32_t layers = t->layout.layers ? t->layout.layers : 1;
    if (layers > b->layers && b->depth == 1) layers = b->layers;
    uint64_t layer_size = tegra_layer_size(&t->layout);
    bool linear = (t->flags & 0x10) != 0;
    uint32_t levels = t->layout.levels < b->levels ? t->layout.levels : b->levels;
    for (uint32_t layer = 0; layer < layers; layer++) {
        for (uint32_t lv = 0; lv < levels; lv++) {
            LevelLayout l = tegra_level(&t->layout, lv);
            if (lv == 0 && layer == 0 && !linear && l.d == 1) bh_probe(base + l.offset, &l, fi->bpb, b->astc_bw, b->astc_bh, t->format);
            if (b->astc_bw) {  // blocks -> linear block rows -> RGBA8 texels
                const uint8_t *src = base + layer * layer_size + l.offset;
                uint8_t *blocks = malloc((uint64_t)l.wb * l.hb * 16);
                if (linear) for (uint32_t y = 0; y < l.hb; y++) memcpy(blocks + (uint64_t)y * l.wb * 16, src + (uint64_t)y * t->stride, l.wb * 16);
                else tegra_deswizzle(src, blocks, l.wb, l.hb, 1, 16, l.block_height_log2, l.block_depth_log2);
                VkSpan s = vk_transient((uint64_t)l.w * l.h * 4, 16);
                astc_decode_ldr(blocks, l.w, l.h, b->astc_bw, b->astc_bh, b->astc_srgb, s.ptr);
                free(blocks);
                VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = l.w, .bufferImageHeight = l.h,
                                        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, layer, 1},
                                        .imageExtent = {l.w < level_w(b, lv) ? l.w : level_w(b, lv), l.h < level_h(b, lv) ? l.h : level_h(b, lv), 1}};
                vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
                continue;
            }
            uint32_t row = l.wb * fi->bpb;
            uint64_t sz = (uint64_t)row * l.hb * l.d;
            bool expand = mf.expand_rgb && fi->bpb == 3;
            uint32_t out_row = expand ? l.wb * 4 : row;
            VkSpan s = vk_transient((uint64_t)out_row * l.hb * l.d, 16);
            uint8_t *lin = expand ? malloc(sz) : s.ptr;
            const uint8_t *src = base + layer * layer_size + l.offset;
            if (linear) {
                for (uint32_t y = 0; y < l.hb; y++) memcpy(lin + (uint64_t)y * row, src + (uint64_t)y * t->stride, row);
            } else {
                tegra_deswizzle(src, lin, l.wb, l.hb, l.d, fi->bpb, l.block_height_log2, l.block_depth_log2);
            }
            {   // (debug) LARECOMP_DUMP_TEX=<w>x<h>: level 0 of uploads that size, as stored and as decoded
                static int dw = -1, dh;
                if (dw < 0 && !(getenv("LARECOMP_DUMP_TEX") && sscanf(getenv("LARECOMP_DUMP_TEX"), "%dx%d", &dw, &dh) == 2)) dw = 0;
                static int ndump;
                if (dw && lv == 0 && layer == 0 && (int)l.w == dw && (int)l.h == dh && ndump < 64) {
                    ndump++;
                    mkdir("build/texdump", 0755);
                    char path[256];
                    LevelLayout l1 = lv + 1 < t->layout.levels ? tegra_level(&t->layout, lv + 1) : l;
                    uint64_t raw = lv + 1 < t->layout.levels ? l1.offset - l.offset : tegra_layer_size(&t->layout) - l.offset;
                    snprintf(path, sizeof path, "build/texdump/%llx_%ux%u_f%x_bh%u_lin%d_stride%u.raw", (unsigned long long)(uintptr_t)base,
                             l.w, l.h, t->format, l.block_height_log2, linear, (unsigned)t->stride);
                    FILE *f = fopen(path, "wb");
                    if (f) { fwrite(src, 1, raw, f); fclose(f); }
                    LOG("[tex] dumped %s (%llu bytes, flags %#x)\n", path, (unsigned long long)raw, (unsigned)t->flags);
                }
            }
            if (expand) {  // RGB8 -> RGBA8
                uint8_t *o = s.ptr;
                uint64_t n = (uint64_t)l.wb * l.hb * l.d;
                for (uint64_t i = 0; i < n; i++) { o[i * 4] = lin[i * 3]; o[i * 4 + 1] = lin[i * 3 + 1]; o[i * 4 + 2] = lin[i * 3 + 2]; o[i * 4 + 3] = 255; }
                free(lin);
            }
            uint32_t w = l.w, hh = l.h, d = l.d;
            // the copy stays inside the image's level: a texture allocated at the internal resolution (16:9, render
            // target sized) can still be filled from the CPU - the save slots' screenshots on the title screen. Its
            // full-size data overran the scaled image (only the width was clamped): GPU writes past the allocation,
            // a device loss below 1x after a seed switch.
            if (w > level_w(b, lv)) w = level_w(b, lv);
            if (hh > level_h(b, lv)) hh = level_h(b, lv);
            VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = l.wb * fi->bw, .bufferImageHeight = l.hb * fi->bh,
                                    .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect, lv,
                                                         b->depth > 1 ? 0 : layer, 1},
                                    .imageExtent = {w, b->base_type == VK_IMAGE_VIEW_TYPE_1D || b->base_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? 1 : hh,
                                                    b->depth > 1 ? d : 1}};
            if (b->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) continue;  // packed D/S: no CPU layout
            vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
        }
    }
}

TexBackend *vk_tex(NvnTexture *t, bool for_render) {
    TexBackend *b = tex_backend_for(t, for_render);
    if (b->pending_rgba) upload_pending(b);
    if (for_render) b->gpu_owned = true;
    else if (!b->gpu_owned) {
        extern uint64_t prof_up_ns;
        uint64_t t0 = prof_up_ns != ~0ull ? SDL_GetTicksNS() : 0;
        upload_texture(t, b);
        if (prof_up_ns != ~0ull) prof_up_ns += SDL_GetTicksNS() - t0;
    }
    return b;
}

void renderer_mark_gpu_owned(NvnTexture *t) { tex_backend_for(t, true)->gpu_owned = true; }
bool renderer_is_gpu_owned(NvnTexture *t) { return tex_backend(t)->gpu_owned; }
void backend_pool_written(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

// Host-produced texel data (decoded video, any thread): applied on the render thread at the texture's next use.
void renderer_upload_rgba(NvnTexture *t, const void *rgba, uint32_t w, uint32_t h, uint32_t stride) {
    TexBackend *b = t->backend;
    if (!b) return;  // the game binds the texture before decoding into it; first frames may arrive earlier
    void *copy = malloc((size_t)stride * h);
    memcpy(copy, rgba, (size_t)stride * h);
    pthread_mutex_lock(&pending_lock);
    free(b->pending_rgba);
    b->pending_rgba = copy;
    b->pending_w = w; b->pending_h = h; b->pending_stride = stride;
    b->gpu_owned = true;
    pthread_mutex_unlock(&pending_lock);
}

// ================================================================== views
#define VIEW_TAB 16384
typedef struct { TexBackend *b; uint64_t k1, k2; VkImageView view; } ViewEnt;
static ViewEnt view_tab[VIEW_TAB];

// Views may still be referenced by submitted or pending work: destroyed once that has completed.
void vk_views_forget(TexBackend *b) { views_forget(b); }
uint32_t vk_view_gen;  // bumped whenever a cached view may be destroyed (exec.c's per-frame texture memo)
static void views_forget(TexBackend *b) {
    vk_view_gen++;
    for (int i = 0; i < VIEW_TAB; i++)
        if (view_tab[i].b == b) { defer((Dead){.view = view_tab[i].view}); view_tab[i].b = NULL; view_tab[i].view = VK_NULL_HANDLE; }
}

VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv, uint32_t ly0,
                          uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect) {
    uint64_t k1 = (uint64_t)type | (uint64_t)fmt << 8 | (uint64_t)lv0 << 40 | (uint64_t)nlv << 48 | (uint64_t)aspect << 56;
    uint64_t k2 = (uint64_t)ly0 | (uint64_t)nly << 16 | (uint64_t)sw.r << 32 | (uint64_t)sw.g << 40 | (uint64_t)sw.b << 48 | (uint64_t)sw.a << 56;
    uint64_t h = ((uint64_t)(uintptr_t)b * 0x9E3779B97F4A7C15ull) ^ (k1 * 0xC2B2AE3D27D4EB4Full) ^ (k2 * 0x165667B19E3779F9ull);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (VIEW_TAB - 1);
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (VIEW_TAB - 1)) {
        ViewEnt *e = &view_tab[slot];
        if (e->b == b && e->k1 == k1 && e->k2 == k2) return e->view;
        if (!e->b) break;
    }
    extern uint64_t prof_views_created;
    prof_views_created++;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = b->image, .viewType = type,
                                .format = fmt, .components = sw, .subresourceRange = {aspect, lv0, nlv, ly0, nly}};
    VkImageView v;
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &v) != VK_SUCCESS) {
        static int nfail;
        if (nfail++ < 20) LOG("[vk] view failed: type %d fmt %d (image fmt %d) levels %u+%u layers %u+%u\n", type, fmt, b->format, lv0, nlv, ly0, nly);
        return VK_NULL_HANDLE;
    }
    ViewEnt *e = &view_tab[slot];
    if (e->b) { defer((Dead){.view = e->view}); vk_view_gen++; }  // evict (16 probes taken)
    *e = (ViewEnt){b, k1, k2, v};
    return v;
}

static VkImageViewType view_type_for(int textype) {
    switch (textype) {
    case TEXTYPE_1D: return VK_IMAGE_VIEW_TYPE_1D;
    case TEXTYPE_3D: return VK_IMAGE_VIEW_TYPE_3D;
    case TEXTYPE_CUBE: return VK_IMAGE_VIEW_TYPE_CUBE;
    case TEXTYPE_2D_ARRAY: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case TEXTYPE_CUBE_ARRAY: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    case TEXTYPE_1D_ARRAY: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}

static const VkComponentMapping IDENTITY = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

// render: attachment view (one level, one layer). Otherwise a sampled view of type want_textype (TEXTYPE_*),
// with the NVN component swizzle, converting between compatible types.
// Layers a render target binds: all of an array / cube image unless its view picks some (a draw without a layer
// output writes layer 0; with one, the passthrough geometry shader emulation picks the face).
uint32_t vk_render_layers(NvnTexture *t, const NvnTextureView *v, bool has_view) {
    TexBackend *b = t ? t->backend : NULL;
    if (!b || b->base_type == VK_IMAGE_VIEW_TYPE_3D || b->layers <= 1) return 1;
    if (!has_view) v = NULL;
    uint32_t ly0 = v && (v->setmask & 2) ? v->min_layer : 0;
    if (ly0 >= b->layers) return 1;
    uint32_t nly = v && (v->setmask & 2) && v->num_layers ? v->num_layers : b->layers - ly0;
    return ly0 + nly > b->layers ? b->layers - ly0 : nly;
}

VkImageView vk_view(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render, int want_textype) {
    TexBackend *b = vk_tex(t, render);
    if (!has_view) v = NULL;
    VkFormat fmt = b->format;
    if (v && (v->setmask & 4) && !(b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
        VkFormat vf = fmt_of(v->format).vf;
        if (vk_format(t->format) == b->format) fmt = vf;  // (fallback images keep their own format)
    }
    uint32_t lv0 = v && (v->setmask & 1) ? v->base_level : 0;
    uint32_t nlv = v && (v->setmask & 1) && v->num_levels ? v->num_levels : b->levels - (lv0 < b->levels ? lv0 : 0);
    uint32_t ly0 = v && (v->setmask & 2) ? v->min_layer : 0;
    uint32_t nly = v && (v->setmask & 2) && v->num_layers ? v->num_layers : b->layers - (ly0 < b->layers ? ly0 : 0);
    if (lv0 >= b->levels) lv0 = b->levels - 1;
    if (lv0 + nlv > b->levels) nlv = b->levels - lv0;
    if (ly0 >= b->layers) ly0 = b->layers - 1;
    if (ly0 + nly > b->layers) nly = b->layers - ly0;
    if (render) {
        VkImageViewType type = b->base_type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        // 3D: storage images only (exec.c image_view asks with the shader's type) - the volumetric fog volume. It got
        // no view before, so its descriptor was never written and the writer stored through whatever a recycled set
        // held: the fog worked by luck, and in the intro's first shot every froxel stayed zero (a black screen).
        // (3D render targets: not used by the game)
        if (b->base_type == VK_IMAGE_VIEW_TYPE_3D)
            return want_textype == TEXTYPE_3D ? vk_image_view(b, VK_IMAGE_VIEW_TYPE_3D, fmt, lv0, 1, 0, 1, IDENTITY, b->aspect)
                                              : VK_NULL_HANDLE;
        uint32_t rl = vk_render_layers(t, v, v != NULL);
        if (rl > 1) return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D_ARRAY, fmt, lv0, 1, ly0, rl, IDENTITY, b->aspect);
        return vk_image_view(b, type, fmt, lv0, 1, ly0, 1, IDENTITY, b->aspect);
    }
    VkImageAspectFlags aspect = b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect;
    VkComponentMapping sw = IDENTITY;
    if (!(b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) && b->samples == 1) {
        const uint8_t *s = v && (v->setmask & 8) ? v->swizzle : t->swizzle;
        uint8_t c[4];
        for (int i = 0; i < 4; i++) c[i] = s[i] <= 5 ? s[i] : (uint8_t)(2 + i);
        if (c[0] || c[1] || c[2] || c[3]) {  // never set: identity
            if (fmt_of(t->format).alpha_one)
                for (int i = 0; i < 4; i++) if (c[i] == 5) c[i] = 1;
            // NVN swizzle 0 zero, 1 one, 2..5 r,g,b,a == VkComponentSwizzle - 1
            sw = (VkComponentMapping){(VkComponentSwizzle)(c[0] + 1), (VkComponentSwizzle)(c[1] + 1), (VkComponentSwizzle)(c[2] + 1),
                                      (VkComponentSwizzle)(c[3] + 1)};
        } else if (fmt_of(t->format).alpha_one) {
            sw.a = VK_COMPONENT_SWIZZLE_ONE;
        }
    }
    VkImageViewType want = view_type_for(want_textype);
    VkImageViewType have = b->base_type;
    if (b->samples > 1) return vk_dummy_view(want_textype);  // multisampled images can't be sampled as 2D
    if (want == have) {
        if (have == VK_IMAGE_VIEW_TYPE_CUBE) nly = 6;
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, nly, sw, aspect);
    }
    if (want == VK_IMAGE_VIEW_TYPE_2D && (have == VK_IMAGE_VIEW_TYPE_2D_ARRAY || have == VK_IMAGE_VIEW_TYPE_CUBE))
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, 1, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_2D_ARRAY && (have == VK_IMAGE_VIEW_TYPE_2D || have == VK_IMAGE_VIEW_TYPE_CUBE))
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, have == VK_IMAGE_VIEW_TYPE_CUBE ? nly : 1, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_CUBE && have == VK_IMAGE_VIEW_TYPE_2D_ARRAY && b->layers >= 6 && b->width == b->height)
        return vk_image_view(b, want, fmt, lv0, nlv, ly0 + 6 <= b->layers ? ly0 : 0, 6, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_1D_ARRAY && have == VK_IMAGE_VIEW_TYPE_1D)
        return vk_image_view(b, want, fmt, lv0, nlv, 0, 1, sw, aspect);
    static int nlog;
    if (nlog++ < 30) LOG("[tex] type mismatch: want %d have %d (%ux%ux%u target %u fmt %#x layers %u)\n", want, have, t->width,
                         t->height, t->depth, t->target, t->format, b->layers);
    return vk_dummy_view(want_textype);
}

// 1x1 black textures for unbound slots, per type
VkImageView vk_dummy_view(int textype) {
    static TexBackend dummies[10];
    static VkImageView views[10];
    if (textype < 0 || textype >= 10) textype = TEXTYPE_2D;
    if (views[textype]) return views[textype];
    TexBackend *b = &dummies[textype];
    VkImageViewType vt = view_type_for(textype);
    bool cube = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                            .imageType = vt == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D
                                       : vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = cube ? 6 : 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL));
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, b->image, VK_IMAGE_ASPECT_COLOR_BIT);
    VkClearColorValue zero = {{0, 0, 0, 0}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, cube ? 6 : 1};
    vkCmdClearColorImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    b->format = VK_FORMAT_R8G8B8A8_UNORM;
    b->levels = 1;
    b->layers = cube ? 6 : 1;
    views[textype] = vk_image_view(b, vt, b->format, 0, 1, 0, cube ? 6 : 1, IDENTITY, VK_IMAGE_ASPECT_COLOR_BIT);
    return views[textype];
}

// ================================================================== samplers
static VkCompareOp compare_op(uint32_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

// Texture filtering (Video tab, g_settings.aniso; LARECOMP_ANISO overrides): anisotropic filtering forced on the
// game's mipmapped, linearly filtered samplers (depth compare ones aside), so floors and walls seen at an angle keep
// their detail instead of fading into smaller mips. 0 = the game's own. A change applies at once: each sampler is made
// again on its next use (backend_gen). Samplers are shared by their settings - the same few hundred serve every pool
// entry, switching back finds the old ones, and Vulkan's sampler count limit (4000) stays far off.
static int aniso_setting(void) {
    static int env = -2;
    if (env == -2) env = getenv("LARECOMP_ANISO") ? atoi(getenv("LARECOMP_ANISO")) : -1;
    return env >= 0 ? env : g_settings.aniso;
}

typedef struct {
    VkFilter mag, min;
    VkSamplerMipmapMode mip;
    VkSamplerAddressMode u, v, w;
    float min_lod, max_lod, bias, aniso;
    VkBool32 aniso_on, compare_on;
    VkCompareOp compare;
    VkBorderColor border;
    float border_rgba[4];
} SamplerKey;

static VkSampler sampler_shared(const VkSamplerCreateInfo *si, const float *border_rgba) {
    static struct { SamplerKey k; VkSampler s; } *tab;
    static int n, cap;
    SamplerKey k;
    memset(&k, 0, sizeof k);  // (padding: keys compare with memcmp)
    k.mag = si->magFilter, k.min = si->minFilter, k.mip = si->mipmapMode;
    k.u = si->addressModeU, k.v = si->addressModeV, k.w = si->addressModeW;
    k.min_lod = si->minLod, k.max_lod = si->maxLod, k.bias = si->mipLodBias;
    k.aniso_on = si->anisotropyEnable, k.aniso = si->anisotropyEnable ? si->maxAnisotropy : 0;
    k.compare_on = si->compareEnable, k.compare = si->compareEnable ? si->compareOp : 0;
    k.border = si->borderColor;
    if (si->borderColor == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT) memcpy(k.border_rgba, border_rgba, 16);
    for (int i = 0; i < n; i++)
        if (!memcmp(&tab[i].k, &k, sizeof k)) return tab[i].s;
    if (n == cap) tab = realloc(tab, (cap = cap ? cap * 2 : 64) * sizeof *tab);
    VK_CHECK(vkCreateSampler(g_vk.dev, si, NULL, &tab[n].s));
    tab[n].k = k;
    return tab[n++].s;
}

VkSampler vk_sampler(NvnSamplerDesc *d) {
    static int aniso = -1;
    static uint32_t gen = 1;
    int a = aniso_setting();
    if (a != aniso) {
        if (aniso >= 0) LOG("[sampler] texture filtering: %dx anisotropic%s\n", a > 1 ? a : 1, a > 1 ? "" : " (the game's own)");
        if (aniso >= 0) gen++;
        aniso = a;
    }
    if (d->backend && d->backend_gen == gen) return (VkSampler)d->backend;
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = d->mag_filter == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    uint32_t mf = d->min_filter;
    si.minFilter = (mf == 1 || mf == 3 || mf == 5) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = mf == 4 || mf == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerAddressMode modes[3];
    bool border = false;
    for (int i = 0; i < 3; i++) {
        switch (d->wrap[i]) {
        case 0: case 7: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
        case 1: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT; break;
        case 2: case 3: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; break;
        case 4: case 5: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; border = true; break;
        case 6: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
        default: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    }
    si.addressModeU = modes[0]; si.addressModeV = modes[1]; si.addressModeW = modes[2];
    si.minLod = d->lod_min;
    si.maxLod = d->lod_max > 1000 ? VK_LOD_CLAMP_NONE : d->lod_max;
    if (mf <= 1) si.maxLod = si.minLod + 0.25f;  // not mipmapped: stay on the base level
    if (si.maxLod < si.minLod) si.maxLod = si.minLod;
    si.mipLodBias = d->lod_bias;
    if (d->max_aniso > 1) { si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = fminf(d->max_aniso, 16); }
    // forced: a linear minifying filter between mips (3: linear, mip nearest; 5: trilinear), linear magnification
    if (a > 1 && (mf == 3 || mf == 5) && si.magFilter == VK_FILTER_LINEAR && !d->compare_mode && a > si.maxAnisotropy) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = (float)a;
    }
    if (si.anisotropyEnable) {
        float lim = g_vk.props.limits.maxSamplerAnisotropy;
        if (si.maxAnisotropy > lim) si.maxAnisotropy = lim;
        if (si.maxAnisotropy <= 1) si.anisotropyEnable = VK_FALSE;
    }
    if (getenv("LARECOMP_LOG_SAMPLERS"))
        LOG("[sampler] min %u mag %u lod %g..%g bias %g aniso %g -> %g wrap %u %u compare %u\n", mf, d->mag_filter,
            d->lod_min, d->lod_max, d->lod_bias, d->max_aniso, si.anisotropyEnable ? si.maxAnisotropy : 1, d->wrap[0],
            d->wrap[1], d->compare_mode);
    static float lodmin = -1;
    if (lodmin < 0) lodmin = getenv("LARECOMP_LOD_MIN") ? (float)atof(getenv("LARECOMP_LOD_MIN")) : 0;
    if (lodmin > 0 && mf > 1) si.minLod = fmaxf(si.minLod, lodmin);
    static int stest = -1;
    if (stest < 0) stest = getenv("LARECOMP_SAMPLER_TEST") != NULL;
    if (stest && mf > 1) { si.minLod = 0; si.maxLod = VK_LOD_CLAMP_NONE; si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = 16; }
    if (d->compare_mode) {
        si.compareEnable = VK_TRUE;
        si.compareOp = getenv("LARECOMP_NO_SHADOW") ? VK_COMPARE_OP_ALWAYS : compare_op(d->compare_func);
    }
    VkSamplerCustomBorderColorCreateInfoEXT cb = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    if (border) {
        const float *c = d->border;
        if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 0) si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        else if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        else if (c[0] == 1 && c[1] == 1 && c[2] == 1 && c[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else if (g_vk.custom_border) {
            memcpy(cb.customBorderColor.float32, c, 16);
            si.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            si.pNext = &cb;
        } else si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    VkSampler s = sampler_shared(&si, d->border);
    d->backend = (void *)s;
    d->backend_gen = gen;
    return s;
}

VkSampler vk_default_sampler(bool linear) {
    static VkSampler s[2];
    if (!s[linear]) {
        VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                  .magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                  .minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                  .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f};
        VK_CHECK(vkCreateSampler(g_vk.dev, &si, NULL, &s[linear]));
    }
    return s[linear];
}
