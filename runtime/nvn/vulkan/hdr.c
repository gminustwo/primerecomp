// HDR output (Video tab "HDR"). The game renders its scene in floating point and tonemaps it into an 8-bit sRGB
// target (the shader patched below), on which its visor effects, anti-aliasing and HUD then draw; that target is what
// it presents. With HDR on:
// - the game's full-size sRGB render targets are RGBA16F instead (textures.c hdr_target): linear either way, so what
//   the game draws and blends there is the same, and values past 1 survive;
// - its tonemapper's shoulder goes on past paper white towards the display's peak (hdr_patch_tonemap);
// - a frame is drawn into a linear FP16 composite, 1.0 = paper white (the game's image, then the overlay menu, drawn
//   as SDR into an image of its own and laid over at paper white), and written to the HDR swapchain at the end:
//   scRGB (FP16, linear, 1.0 = 80 nits) or HDR10 (BT.2020 primaries, PQ). device.c vk_frame_begin / vk_frame_end.
// - SMAA and FSR (exec.c post_process) run on a PQ-encoded copy, in RGBA16F images.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "vk.h"
#include "../../settings.h"

static bool g_frame_hdr;      // the frame being drawn is an HDR one (vk_hdr_frame_begin .. vk_hdr_frame_finish)
static TexBackend g_comp, g_ui;  // the frame's composite (RGBA16F) and the menus' SDR image, at the window's size

int vk_hdr_choose(const VkSurfaceFormatKHR *f, uint32_t n, VkSurfaceFormatKHR *out) {
    // (test) LARECOMP_HDR_TEST=1: HDR frames on the SDR swapchain - the whole HDR path, the composite shown as SDR at
    // the end (paper white = white, brighter clips): for a display without HDR; window shots as for real HDR
    static int test = -1;
    if (test < 0) test = getenv("LARECOMP_HDR_TEST") ? 1 : 0;
    if (test)
        for (uint32_t i = 0; i < n; i++)
            if (f[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
                (f[i].format == VK_FORMAT_B8G8R8A8_UNORM || f[i].format == VK_FORMAT_R8G8B8A8_UNORM)) {
                *out = f[i];
                return HDR_OUT_TEST;
            }
    // scRGB first: linear like the composite (Windows composes in it). (test) LARECOMP_HDR_FORMAT=hdr10: HDR10 first
    const char *pref = getenv("LARECOMP_HDR_FORMAT");
    bool pq_first = pref && !strcmp(pref, "hdr10");
    for (uint32_t i = 0; i < n && !pq_first; i++)
        if (f[i].format == VK_FORMAT_R16G16B16A16_SFLOAT && f[i].colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) {
            *out = f[i];
            return HDR_OUT_SCRGB;
        }
    for (uint32_t i = 0; i < n; i++)
        if ((f[i].format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f[i].format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) &&
            f[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
            *out = f[i];
            return HDR_OUT_PQ;
        }
    for (uint32_t i = 0; i < n && pq_first; i++)
        if (f[i].format == VK_FORMAT_R16G16B16A16_SFLOAT && f[i].colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) {
            *out = f[i];
            return HDR_OUT_SCRGB;
        }
    return HDR_OUT_NONE;
}

// The Video tab's HDR row: 0 the window's display takes no HDR (Windows: HDR off in its display settings), 1 it does,
// 2 the game's frames go out as HDR now
int vk_hdr_status(void) {
    if (g_vk.hdr_out == HDR_OUT_SCRGB || g_vk.hdr_out == HDR_OUT_PQ) return 2;
    return g_vk.hdr_offered ? 1 : 0;
}

// The game's tonemap shoulder rises to this (peak over paper white) while frames go out as HDR; 0: as the game's.
// exec.c stage_set hands it to every shader (rscale[64].z), the patched tonemapper reads it.
float vk_hdr_shoulder(void) {
    static int alt = -1;  // (test) LARECOMP_HDR_ALT=1: the game's own shoulder every other frame (A/B in one run)
    if (alt < 0) alt = getenv("LARECOMP_HDR_ALT") ? 1 : 0;
    if (alt && (vk_frame_counter() & 1)) return 0.0f;
    return g_vk.hdr_out ? (float)g_settings.hdr_peak / (float)g_settings.hdr_paper : 0.0f;
}

// ------------------------------------------------------------------ fullscreen passes of their own
// One sampler (binding 0); 64 bytes of push constants for both stages (present.vert's 32, then the fragment's)
static VkPipelineLayout g_layout;
static VkDescriptorSetLayout g_set_layout;

VkPipeline vk_hdr_pipeline(const char *vs, const char *fs, VkFormat fmt, bool blend) {
    static struct { const char *vs, *fs; VkFormat f; bool b; VkPipeline p; } cache[12];
    for (int i = 0; i < 12; i++)
        if (cache[i].p && cache[i].vs == vs && cache[i].fs == fs && cache[i].f == fmt && cache[i].b == blend) return cache[i].p;
    if (!g_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &g_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &g_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &g_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader(vs), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fs), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    if (blend)  // (premultiplied: the menus' image)
        cba = (VkPipelineColorBlendAttachmentState){
            .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_ONE, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = g_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 12; i++)
        if (!cache[i].p) { cache[i].vs = vs, cache[i].fs = fs, cache[i].f = fmt, cache[i].b = blend, cache[i].p = p; break; }
    return p;
}

// One draw of a fullscreen triangle with `p` (inside a rendering), sampling src; pc: 16 floats (vertex 0-7,
// fragment 8-15). The viewport is the caller's.
void vk_hdr_draw(VkCommandBuffer cb, VkPipeline p, VkImageView src, const float pc[16]) {
    VkDescriptorSet ds = vk_alloc_set(g_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), src, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &ds, 0, NULL);
    vkCmdPushConstants(cb, g_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

// ------------------------------------------------------------------ the frame
static void frame_image(TexBackend *d, VkFormat f, uint32_t w, uint32_t h) {  // (re)made at the window's size
    if (d->image && (d->width != w || d->height != h || d->format != f)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE, d->mem = VK_NULL_HANDLE;
    }
    if (d->image) return;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
    d->format = f, d->width = w, d->height = h, d->depth = 1, d->layers = 1, d->levels = 1, d->samples = 1;
    d->aspect = VK_IMAGE_ASPECT_COLOR_BIT, d->base_type = VK_IMAGE_VIEW_TYPE_2D;
    vk_image_init_layout(vk_cmd(), d->image, d->aspect);
}

static VkImageView frame_view(TexBackend *d) {
    static const VkComponentMapping id = {0};
    return vk_image_view(d, VK_IMAGE_VIEW_TYPE_2D, d->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

static void begin_in(VkCommandBuffer cb, VkImageView v, VkImageLayout layout, VkExtent2D ext, VkAttachmentLoadOp load,
                     const float clear[4]) {
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v,
                                    .imageLayout = layout, .loadOp = load, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    if (clear) memcpy(ca.clearValue.color.float32, clear, 16);
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, ext}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)ext.width, (float)ext.height, 0, 1};
    VkRect2D sc = {{0, 0}, ext};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
}

static void begin(VkCommandBuffer cb, VkImageView v, VkExtent2D ext, VkAttachmentLoadOp load, const float clear[4]) {
    begin_in(cb, v, VK_IMAGE_LAYOUT_GENERAL, ext, load, clear);  // (the images here stay in GENERAL, as the game's do)
}

bool vk_hdr_frame(void) { return g_frame_hdr; }

// device.c vk_frame_begin, an HDR swapchain: the frame's rendering begins in the composite
void vk_hdr_frame_begin(VkCommandBuffer cb, const float clear[4]) {
    frame_image(&g_comp, VK_FORMAT_R16G16B16A16_SFLOAT, g_vk.sc_extent.width, g_vk.sc_extent.height);
    // (the frame's pipelines now: one made inside a rendering crashed the NVIDIA driver, exec.c post_pass)
    vk_hdr_pipeline("present.vert", "present_hdr.frag", VK_FORMAT_R16G16B16A16_SFLOAT, false);
    vk_hdr_pipeline("post.vert", "hdr_ui.frag", VK_FORMAT_R16G16B16A16_SFLOAT, true);
    vk_hdr_pipeline("post.vert", "hdr_out.frag", g_vk.sc_format, false);
    vk_full_barrier(cb);
    begin(cb, frame_view(&g_comp), g_vk.sc_extent, VK_ATTACHMENT_LOAD_OP_CLEAR, clear);
    g_frame_hdr = true;
}

// vk_frame_suspend / resume: the frame's rendering again, keeping what was drawn
void vk_hdr_frame_resume(VkCommandBuffer cb) { begin(cb, frame_view(&g_comp), g_vk.sc_extent, VK_ATTACHMENT_LOAD_OP_LOAD, NULL); }

// The overlay menu (ImGui: in the SDR swapchain's format) into an image of its own, then over the composite
void vk_hdr_menu_begin(VkCommandBuffer cb) {
    vkCmdEndRendering(cb);
    frame_image(&g_ui, g_vk.sdr_format, g_vk.sc_extent.width, g_vk.sc_extent.height);
    vk_full_barrier(cb);
    static const float none[4] = {0, 0, 0, 0};
    begin(cb, frame_view(&g_ui), g_vk.sc_extent, VK_ATTACHMENT_LOAD_OP_CLEAR, none);
}

void vk_hdr_menu_end(VkCommandBuffer cb) {
    vkCmdEndRendering(cb);
    vk_full_barrier(cb);
    vk_hdr_frame_resume(cb);
    VkPipeline p = vk_hdr_pipeline("post.vert", "hdr_ui.frag", VK_FORMAT_R16G16B16A16_SFLOAT, true);
    float pc[16] = {0};
    vk_hdr_draw(cb, p, frame_view(&g_ui), pc);
}

// device.c vk_frame_end: the composite (its rendering ended) into the swapchain image (in COLOR_ATTACHMENT_OPTIMAL),
// in its colour space
void vk_hdr_frame_finish(VkCommandBuffer cb, VkImageView sc_view) {
    vk_full_barrier(cb);
    begin_in(cb, sc_view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, g_vk.sc_extent, VK_ATTACHMENT_LOAD_OP_DONT_CARE, NULL);
    VkPipeline p = vk_hdr_pipeline("post.vert", "hdr_out.frag", g_vk.sc_format, false);
    float pc[16] = {0};
    pc[8] = (float)g_vk.hdr_out, pc[9] = (float)g_settings.hdr_paper, pc[10] = (float)g_settings.hdr_peak;
    vk_hdr_draw(cb, p, frame_view(&g_comp), pc);
    vkCmdEndRendering(cb);
    g_frame_hdr = false;
}

// A window screenshot of an HDR frame: the composite read back (call before vk_hdr_frame_finish's submission).
// extra: bytes more in the same readback, after the composite's (vk_readback has one buffer: a second call reuses it)
VkSpan vk_hdr_shot_copy(VkCommandBuffer cb, uint64_t extra) {
    uint32_t w = g_comp.width, h = g_comp.height;
    VkSpan out = vk_readback((uint64_t)w * h * 8 + extra);
    vk_full_barrier(cb);
    VkBufferImageCopy rg = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, g_comp.image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &rg);
    return out;
}

static float half_to_float(uint16_t h) {
    uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF, f;
    if (e == 0) { float d = m * 5.9604644775390625e-8f; return s ? -d : d; }
    f = e == 31 ? s | 0x7F800000u | (m << 13) : s | ((e + 112) << 23) | (m << 13);
    float r;
    memcpy(&r, &f, 4);
    return r;
}

// ... then (its submission done) as an 8-bit sRGB picture of the SDR range (paper white = white; brighter clips),
// written to rgba; the brightest pixel in nits is logged (how far the highlights went past paper white)
void vk_hdr_shot_convert(const VkSpan *s, uint8_t *rgba, uint32_t *w, uint32_t *h) {
    *w = g_comp.width, *h = g_comp.height;
    const uint16_t *px = s->ptr;
    float peak = 0;
    uint64_t over = 0, n = (uint64_t)*w * *h;
    for (uint64_t i = 0; i < n; i++) {
        float c[3], l;
        for (int k = 0; k < 3; k++) c[k] = half_to_float(px[i * 4 + k]);
        l = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
        if (l > peak) peak = l;
        if (l > 1.0f) over++;
        for (int k = 0; k < 3; k++) {
            float v = c[k] < 0 ? 0 : c[k] > 1 ? 1 : c[k];
            v = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1 / 2.4f) - 0.055f;
            rgba[i * 4 + k] = (uint8_t)lroundf(v * 255);
        }
        rgba[i * 4 + 3] = 255;
    }
    LOG("[hdr] shot: brightest %.0f nits (paper white %d, peak %d), %.2f%% of the window past paper white\n",
        peak * g_settings.hdr_paper, g_settings.hdr_paper, g_settings.hdr_peak, 100.0 * over / (n ? n : 1));
}

// ... and the window image itself, as the display gets it (a shot's check: device.c copies it after the final pass):
// decoded from scRGB / HDR10, its brightest pixel in nits is logged next to the composite's
void vk_hdr_shot_check(const VkSpan *s, uint32_t w, uint32_t h) {
    float peak = 0;
    for (uint64_t i = 0, n = (uint64_t)w * h; i < n; i++) {
        float nits = 0;
        if (g_vk.hdr_out == HDR_OUT_SCRGB) {
            const uint16_t *p = (const uint16_t *)s->ptr + i * 4;
            nits = 80 * (0.2126f * half_to_float(p[0]) + 0.7152f * half_to_float(p[1]) + 0.0722f * half_to_float(p[2]));
        } else if (g_vk.hdr_out == HDR_OUT_PQ) {
            uint32_t v = ((const uint32_t *)s->ptr)[i], c[3] = {v & 1023, (v >> 10) & 1023, (v >> 20) & 1023};
            float l[3];
            for (int k = 0; k < 3; k++) {  // (PQ to nits, BT.2020: luminance weights of its own)
                float e = c[k] / 1023.0f, p = powf(e, 1 / 78.84375f);
                l[k] = 10000 * powf(fmaxf(p - 0.8359375f, 0) / (18.8515625f - 18.6875f * p), 1 / 0.1593017578125f);
            }
            if (g_vk.sc_format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) { float t = l[0]; l[0] = l[2]; l[2] = t; }
            nits = 0.2627f * l[0] + 0.6780f * l[1] + 0.0593f * l[2];
        }
        if (nits > peak) peak = nits;
    }
    LOG("[hdr] shot: the window image (%s) - brightest %.0f nits\n", g_vk.hdr_out == HDR_OUT_SCRGB ? "scRGB" : "HDR10", peak);
}

// ------------------------------------------------------------------ the game's tonemapper
// The tonemap pass (one shader: the scene, sampler2D t0, and two colour LUTs, sampler3D t1 / t2): per channel the
// exposed scene colour x goes through a curve - a toe below cb3[2].z, a line, a shoulder from cb3[2].w,
// u = x * cb3[3].y + cb3[3].z, y = u / (u + 1) * cb3[3].x + cb3[3].w, which flattens out at white -, and y (a
// gamma-like encoding: shown as is, it looks washed out) indexes the LUTs (t1 at y * cb3[4].y + cb3[4].x), which give
// the graded linear colour. Patched: the LUT coordinates are kept; at the end, for the brightest channel past the
// shoulder's start, x is recovered from y (the shoulder inverted) and taken on by a shoulder of the same value and
// slope there that rises, in linear light, to the peak over paper white (rscale[64].z) instead of 1 - the graded
// colour scaled by how much brighter that is. Below the shoulder nothing changes; with HDR off (0) neither does
// anything else.
static const char k_tm_funcs[] =
    "float pr_srgb(float y) { return y <= 0.04045f ? y / 12.92f : pow((y + 0.055f) / 1.055f, 2.4f); }\n"
    "float pr_dsrgb(float y) { return y <= 0.04045f ? 1.0f / 12.92f : 2.4f / 1.055f * pow((y + 0.055f) / 1.055f, 1.4f); }\n"
    "float pr_hdr_gain(vec3 yc, float hk) {\n"
    "  vec3 y = (yc - as_f(cb3[4][0])) / as_f(cb3[4][1]);\n"
    "  float m = max(y.x, max(y.y, y.z));\n"
    "  float A = as_f(cb3[3][0]), k = as_f(cb3[3][1]), c = as_f(cb3[3][2]), B = as_f(cb3[3][3]), x0 = as_f(cb3[2][3]);\n"
    "  float u0 = x0 * k + c, s0 = u0 / (u0 + 1.0f) * A + B;\n"
    "  if (!(hk > 1.0f) || !(m > s0) || A <= 0.0f || k <= 0.0f) return 1.0f;\n"
    "  float q = clamp((m - B) / A, 0.0f, 0.999999f), x = (q / (1.0f - q) - c) / k;\n"
    "  float l0 = pr_srgb(s0), d0 = pr_dsrgb(s0) * A * k / ((u0 + 1.0f) * (u0 + 1.0f));\n"
    "  float t = max(x - x0, 0.0f) * d0, room = max(hk - l0, 1e-3f);\n"
    "  float lh = l0 + t / (1.0f + t / room), ls = pr_srgb(min(m, 1.0f));\n"
    "  return ls > 1e-5f ? max(lh / ls, 1.0f) : 1.0f;\n"
    "}\n";

bool hdr_patch_tonemap(char **src) {
    char *s = *src;
    if (!strstr(s, "uniform sampler2D t0;") || !strstr(s, "uniform sampler3D t1;") || !strstr(s, "uniform sampler3D t2;") ||
        !strstr(s, "uniform RScale") || !strstr(s, "cb3[4][1]") || !strstr(s, "cb3[3][1]") || !strstr(s, "texture(t2, vec3("))
        return false;
    const char *lut = strstr(s, "    { vec4 t = texture(t1, vec3(as_f(r");
    const char *out = strstr(s, "  c0[0] = as_f(r0);");
    const char *mainp = strstr(s, "void main() {\n");
    if (!lut || !out || !mainp || lut > out) return false;
    char ra[8], rb[8], rc[8];
    if (sscanf(lut, "    { vec4 t = texture(t1, vec3(as_f(%7[r0-9]), as_f(%7[r0-9]), as_f(%7[r0-9])));", ra, rb, rc) != 3) return false;
    char keep[96], apply[256];
    snprintf(keep, sizeof keep, "    pr_lut = vec3(as_f(%s), as_f(%s), as_f(%s));\n", ra, rb, rc);
    snprintf(apply, sizeof apply,
             "  { float g = pr_hdr_gain(pr_lut, rscale[64][2]);  // (PrimeRecomp HDR: runtime/nvn/vulkan/hdr.c)\n"
             "    r0 = as_u(as_f(r0) * g); r1 = as_u(as_f(r1) * g); r2 = as_u(as_f(r2) * g); }\n");
    static const char decl[] = "  vec3 pr_lut = vec3(0.0f);\n";
    size_t n = strlen(s), cap = n + sizeof k_tm_funcs + sizeof decl + strlen(keep) + strlen(apply) + 16;
    char *d = malloc(cap), *p = d;
    size_t a = (size_t)(mainp - s), b = (size_t)(lut - s), c = (size_t)(out - s), ml = strlen("void main() {\n");
    memcpy(p, s, a), p += a;                                         // ... up to main
    memcpy(p, k_tm_funcs, sizeof k_tm_funcs - 1), p += sizeof k_tm_funcs - 1;
    memcpy(p, s + a, ml), p += ml;                                   // void main() {
    memcpy(p, decl, sizeof decl - 1), p += sizeof decl - 1;
    memcpy(p, s + a + ml, b - a - ml), p += b - a - ml;              // ... up to the first LUT lookup
    memcpy(p, keep, strlen(keep)), p += strlen(keep);
    memcpy(p, s + b, c - b), p += c - b;                             // ... up to the outputs
    memcpy(p, apply, strlen(apply)), p += strlen(apply);
    memcpy(p, s + c, n - c + 1);                                     // the outputs, the rest
    free(*src);
    *src = d;
    return true;
}
