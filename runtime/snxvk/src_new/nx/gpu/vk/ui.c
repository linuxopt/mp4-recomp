/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The on-screen controls, the game's menu (runtime/platform/ui.h) and the shader loading screen before the game
   (runtime/platform/prepare_screen.h) over a presented frame: the quads they give, blended onto the swapchain image
   in a pass of their own after the frame's blit. Nothing is recorded while none shows */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/vk/ui_spv.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/hud.h"
#include "nx/runtime/platform/overlay.h"
#include "nx/runtime/platform/prepare_screen.h"
#include "nx/runtime/platform/stats.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/vpad.h"
#include "ov_vk.h" /* SWR: the in-game menu, runtime/ov_vk.h */

#define MAX_QUADS 4096
typedef struct {
    float x, y, u, v;
    u32 rgba;
} Vertex;
static SnxUiQuad g_quads[MAX_QUADS];
static VkDescriptorSetLayout g_dsl;
static VkPipelineLayout g_layout;
static VkPipeline g_pipe;
static VkFormat g_pipe_format;
static VkDescriptorPool g_pool;
/* by a quad's sample: the atlas blended (sprites), at its nearest texel (the font, fills), and the picture blended */
static VkDescriptorSet g_sets[3];
static VkSampler g_samplers[2];
static VkImage g_atlas;
static VmaAllocation g_atlas_mem;
static VkImageView g_atlas_view;
static u32 g_atlas_version;
static float g_atlas_w, g_atlas_h;
static VkImage g_image; /* ui_image's picture */
static VmaAllocation g_image_mem;
static VkImageView g_image_view;
static u32 g_image_version;
static VkImage g_sc_img[VK_SC_MAX];
static VkImageView g_sc_view[VK_SC_MAX];

static int layout_make(void) {
    VkDescriptorSetLayoutBinding b = {.binding = 0,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .descriptorCount = 1,
                                      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = 1,
                                      .pSetLayouts = &g_dsl,
                                      .pushConstantRangeCount = 1,
                                      .pPushConstantRanges = &pc};
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
    VkDescriptorPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 3, .poolSizeCount = 1, .pPoolSizes = &size};
    VkDescriptorSetLayout layouts[3];
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorSetCount = 3};
    int i;
    if (g_layout) return 1;
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &dci, NULL, &g_dsl))) return 0;
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &lci, NULL, &g_layout))) return 0;
    if (!VK_OK(vkCreateDescriptorPool(g_vk.dev, &pci, NULL, &g_pool))) return 0;
    layouts[0] = layouts[1] = layouts[2] = g_dsl;
    ai.descriptorPool = g_pool;
    ai.pSetLayouts = layouts;
    if (!VK_OK(vkAllocateDescriptorSets(g_vk.dev, &ai, g_sets))) return 0;
    for (i = 0; i < 2; ++i) {
        VkFilter f = i ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                   .magFilter = f,
                                   .minFilter = f,
                                   .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                   .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                   .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                   .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        if (!VK_OK(vkCreateSampler(g_vk.dev, &sci, NULL, &g_samplers[i]))) return 0;
    }
    return 1;
}

/* SWR: the overlay pipeline for a target of that format, VK_NULL_HANDLE on failure (the in-game menu makes its own) */
static VkPipeline pipeline_new(VkFormat format) {
    VkShaderModule vs, fs;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                              .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                              .pName = "main"},
                                             {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                              .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                              .pName = "main"}};
    VkVertexInputBindingDescription vb = {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[3] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 8}, {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 16}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = 1,
                                               .pVertexBindingDescriptions = &vb,
                                               .vertexAttributeDescriptionCount = 3,
                                               .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL,
                                                 .cullMode = VK_CULL_MODE_NONE,
                                                 .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    /* alpha premultiplied: the atlas is, and the shader multiplies the colour's in */
    VkPipelineColorBlendAttachmentState ba = {.blendEnable = VK_TRUE,
                                              .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
                                              .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                              .colorBlendOp = VK_BLEND_OP_ADD,
                                              .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                              .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                              .alphaBlendOp = VK_BLEND_OP_ADD,
                                              .colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &format};
    VkGraphicsPipelineCreateInfo gci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                        .pNext = &ri,
                                        .stageCount = 2,
                                        .pStages = st,
                                        .pVertexInputState = &vi,
                                        .pInputAssemblyState = &ia,
                                        .pViewportState = &vp,
                                        .pRasterizationState = &rs,
                                        .pMultisampleState = &ms,
                                        .pColorBlendState = &cb,
                                        .pDynamicState = &ds,
                                        .layout = g_layout};
    vs = vk_module_spv(snx_ui_vs_spv, sizeof snx_ui_vs_spv / 4);
    fs = vk_module_spv(snx_ui_fs_spv, sizeof snx_ui_fs_spv / 4);
    st[0].module = vs;
    st[1].module = fs;
    if (!vs || !fs || !VK_OK(vkCreateGraphicsPipelines(g_vk.dev, vk_pipeline_cache(), 1, &gci, NULL, &pipe)))
        pipe = VK_NULL_HANDLE;
    if (vs) vkDestroyShaderModule(g_vk.dev, vs, NULL);
    if (fs) vkDestroyShaderModule(g_vk.dev, fs, NULL);
    return pipe;
}
static int pipeline_make(VkFormat format) {
    if (g_pipe && g_pipe_format == format) return 1;
    if (g_pipe) vkDestroyPipeline(g_vk.dev, g_pipe, NULL); /* vk_finish ran when the swapchain changed format */
    g_pipe = pipeline_new(format);
    g_pipe_format = format;
    return g_pipe != VK_NULL_HANDLE;
}

/* the atlas to the GPU, once each time the frontend gives one (under ui_lock) */
static void atlas_upload(VkCommandBuffer cb) {
    int w, h, i;
    u32 version;
    const u32* px = ui_atlas(&w, &h, &version);
    VkBuffer buf;
    VmaAllocation mem;
    VmaAllocationInfo info;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = (VkDeviceSize)w * (VkDeviceSize)h * 4,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo bai = {.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                            VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                   .usage = VMA_MEMORY_USAGE_AUTO};
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {(u32)w, (u32)h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo iai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {(u32)w, (u32)h, 1}};
    if (!px || version == g_atlas_version) return;
    if (!VK_OK(vk_buffer_create(g_vk.vma, &bci, &bai, &buf, &mem, &info))) return;
    vk_mem_track(mem);
    memcpy(info.pMappedData, px, (size_t)w * (size_t)h * 4);
    vmaFlushAllocation(g_vk.vma, mem, 0, VK_WHOLE_SIZE);
    vk_defer_buffer(buf, mem);
    if (g_atlas) vk_defer_image(g_atlas, g_atlas_mem, g_atlas_view, VK_NULL_HANDLE);
    g_atlas = VK_NULL_HANDLE;
    if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &iai, &g_atlas, &g_atlas_mem, NULL))) {
        g_atlas = VK_NULL_HANDLE;
        return;
    }
    vk_mem_track(g_atlas_mem);
    vci.image = g_atlas;
    if (!VK_OK(vkCreateImageView(g_vk.dev, &vci, NULL, &g_atlas_view))) {
        vk_mem_untrack(g_atlas_mem);
        vmaDestroyImage(g_vk.vma, g_atlas, g_atlas_mem);
        g_atlas = VK_NULL_HANDLE;
        return;
    }
    vk_barrier(cb, g_atlas, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    rec_CmdCopyBufferToImage(cb, buf, g_atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    vk_barrier(cb, g_atlas, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    for (i = 0; i < 2; ++i) {
        VkDescriptorImageInfo di = {g_samplers[i], g_atlas_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstSet = g_sets[i],
                                   .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                   .pImageInfo = &di};
        vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
    }
    g_atlas_version = version;
    g_atlas_w = (float)w;
    g_atlas_h = (float)h;
    ui_atlas_done();
    LOG("vulkan: the on-screen controls' atlas is %dx%d\n", w, h);
}

/* ui_image's picture to the GPU when it changed (under ui_lock): one image made the first time, written over after */
static void image_upload(VkCommandBuffer cb) {
    u32 version;
    const u8* px = ui_image_get(&version);
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {SNX_UI_IMAGE_W, SNX_UI_IMAGE_H, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo iai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {SNX_UI_IMAGE_W, SNX_UI_IMAGE_H, 1}};
    const u64 size = (u64)SNX_UI_IMAGE_W * SNX_UI_IMAGE_H * 4;
    VkBuffer buf;
    u64 off;
    u8* to;
    if (!px || version == g_image_version) return;
    if (!g_image) {
        VkDescriptorImageInfo di = {g_samplers[0], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstSet = g_sets[UI_IMAGE],
                                   .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                   .pImageInfo = &di};
        if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &iai, &g_image, &g_image_mem, NULL))) {
            g_image = VK_NULL_HANDLE;
            return;
        }
        vci.image = g_image;
        if (!VK_OK(vkCreateImageView(g_vk.dev, &vci, NULL, &g_image_view))) {
            vmaDestroyImage(g_vk.vma, g_image, g_image_mem);
            g_image = VK_NULL_HANDLE;
            return;
        }
        di.imageView = g_image_view;
        vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
    }
    to = (u8*)vk_stage(size, 4, &buf, &off);
    if (!to) return;
    memcpy(to, px, (size_t)size);
    copy.bufferOffset = off;
    /* the last frame's draws sampled it: they finish before the copy writes over it */
    vk_barrier(cb, g_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    rec_CmdCopyBufferToImage(cb, buf, g_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    vk_barrier(cb, g_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    g_image_version = version;
}

/* the swapchain image's view, made once per image */
static VkImageView sc_view(VkImage sc) {
    u32 i;
    for (i = 0; i < VK_SC_MAX; ++i)
        if (g_sc_img[i] == sc) return g_sc_view[i];
    for (i = 0; i < VK_SC_MAX; ++i)
        if (!g_sc_img[i]) {
            VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                         .image = sc,
                                         .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                         .format = g_vk.sc_format,
                                         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            if (!VK_OK(vkCreateImageView(g_vk.dev, &vci, NULL, &g_sc_view[i]))) return VK_NULL_HANDLE;
            g_sc_img[i] = sc;
            return g_sc_view[i];
        }
    return VK_NULL_HANDLE;
}
void vk_ui_swapchain_gone(void) {
    u32 i;
    for (i = 0; i < VK_SC_MAX; ++i) {
        if (g_sc_view[i]) vkDestroyImageView(g_vk.dev, g_sc_view[i], NULL);
        g_sc_view[i] = VK_NULL_HANDLE;
        g_sc_img[i] = VK_NULL_HANDLE;
    }
}

static void vertex(Vertex* v, float x, float y, float u, float t, u32 rgba) {
    v->x = x;
    v->y = y;
    v->u = u;
    v->v = t;
    v->rgba = rgba;
    snx_le32_array(v, sizeof *v / 4); /* the device reads the words little-endian: the colour's low byte is R */
}
void vk_ui_draw(VkCommandBuffer cb, VkImage sc, const int safe[4]) {
    SnxUiList list = {g_quads, 0, MAX_QUADS};
    int sw = (int)g_vk.sc_extent.width, sh = (int)g_vk.sc_extent.height, n, i, run;
    VkImageView view;
    Vertex* v;
    VkBuffer buf;
    u64 off;
    struct {
        float w, h;
        u32 srgb, pad;
    } pc = {(float)sw, (float)sh, (u32)g_vk.sc_srgb, 0};
    if (!layout_make()) return;
    ui_lock();
    atlas_upload(cb);
    image_upload(cb);
    /* the screen before the game starts, under the menu, hides the on-screen controls: they aren't drawn under it,
       where they would take quads for nothing (nor do they take fingers then, sdl3.c's press_event) */
    if (prepare_screen_shown())
        prepare_screen_draw(&list, sw, sh, safe, overlay_display_percent());
    else {
        int px, py, scale;
        vpad_draw(&list, safe);
        vpad_showcase_draw(&list, safe, vpad_showcase_fps(g_vk_present, plat_ns()));
        overlay_place(sw, sh, &px, &py, &scale); /* the menu's letters */
        stats_draw(&list, safe, scale, plat_ns());
    }
    {
        int px, py, scale;
        overlay_place(sw, sh, &px, &py, &scale); /* the menu's letters */
        hud_draw(&list, safe, scale);
    }
    overlay_draw(&list, sw, sh, plat_ns());
    ui_unlock();
    n = list.n;
    if (!n || !g_atlas || !pipeline_make(g_vk.sc_format) || !(view = sc_view(sc))) return;
    v = (Vertex*)vk_stage((u64)n * 6 * sizeof(Vertex), 4, &buf, &off);
    if (!v) return;
    for (i = 0; i < n; ++i) {
        const SnxUiQuad* q = &g_quads[i];
        /* the texels' corners, clockwise from the top left, turned: a quarter turn shows the bottom left one there */
        float uu[4] = {q->u, q->u + q->uw, q->u + q->uw, q->u}, vv[4] = {q->v, q->v, q->v + q->vh, q->v + q->vh};
        float px[4] = {q->x, q->x + q->w, q->x + q->w, q->x}, py[4] = {q->y, q->y, q->y + q->h, q->y + q->h};
        float cu[4], cv[4];
        int k;
        float tw = q->sample == UI_IMAGE ? (float)SNX_UI_IMAGE_W : g_atlas_w,
              th = q->sample == UI_IMAGE ? (float)SNX_UI_IMAGE_H : g_atlas_h;
        for (k = 0; k < 4; ++k) {
            cu[k] = uu[(k + 4 - q->turn) % 4] / tw;
            cv[k] = vv[(k + 4 - q->turn) % 4] / th;
        }
        vertex(&v[i * 6 + 0], px[0], py[0], cu[0], cv[0], q->rgba);
        vertex(&v[i * 6 + 1], px[1], py[1], cu[1], cv[1], q->rgba);
        vertex(&v[i * 6 + 2], px[2], py[2], cu[2], cv[2], q->rgba);
        vertex(&v[i * 6 + 3], px[0], py[0], cu[0], cv[0], q->rgba);
        vertex(&v[i * 6 + 4], px[2], py[2], cu[2], cv[2], q->rgba);
        vertex(&v[i * 6 + 5], px[3], py[3], cu[3], cv[3], q->rgba);
    }
    vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    {
        VkRenderingAttachmentInfo at = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                        .imageView = view,
                                        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                        .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                              .renderArea = {{0, 0}, g_vk.sc_extent},
                              .layerCount = 1,
                              .colorAttachmentCount = 1,
                              .pColorAttachments = &at};
        VkViewport vp = {0, 0, (float)sw, (float)sh, 0, 1};
        VkRect2D sci = {{0, 0}, g_vk.sc_extent};
        VkDeviceSize vo = off;
        rec_CmdBeginRendering(cb, &ri);
        vk_render_forget_state(); /* another pipeline and layout: a draw after it sets everything again */
        rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipe);
        rec_CmdSetViewport(cb, 0, 1, &vp);
        rec_CmdSetScissor(cb, 0, 1, &sci);
        rec_CmdPushConstants(cb, g_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        rec_CmdBindVertexBuffers(cb, 0, 1, &buf, &vo);
        /* runs of quads that sample the same way, in their order */
        for (i = 0; i < n; i = run) {
            for (run = i + 1; run < n && g_quads[run].sample == g_quads[i].sample; ++run) continue;
            if (g_quads[i].sample == UI_IMAGE && !g_image) continue; /* its upload failed */
            rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &g_sets[g_quads[i].sample],
                                    0, NULL);
            rec_CmdDraw(cb, (u32)(run - i) * 6, 1, (u32)i * 6, 0);
        }
        rec_CmdEndRendering(cb);
    }
    vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
}

/* ------------------------------------------------------------ SWR: the in-game menu (runtime ov_vk.h, beyond_ui/ov.cpp)
   Dear ImGui's draw data through the overlay's pipeline: its vertices have the same layout, and its textures (the font
   atlas) are kept here with their alpha premultiplied, as the shader blends. Drawn over everything else in the picture */
#define MENU_TEX   16
#define MENU_SETS  64 /* a set is never rewritten while a frame may still bind it: each texture made takes the next one */
#define MENU_PIPES 6  /* one per target format: the swapchain's and the present pass's image's */
typedef struct {
    unsigned id; /* 0: free */
    int w, h;
    VkImage img;
    VmaAllocation mem;
    VkImageView view;
    VkDescriptorSet set;
} MenuTex;
static MenuTex g_mtex[MENU_TEX];
static VkDescriptorPool g_mpool;
static VkDescriptorSet g_msets[MENU_SETS];
static u32 g_mset_next;
static struct {
    VkFormat format;
    VkPipeline pipe;
} g_mpipes[MENU_PIPES];

static int menu_sets_make(void) {
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MENU_SETS};
    VkDescriptorPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .maxSets = MENU_SETS,
                                      .poolSizeCount = 1,
                                      .pPoolSizes = &size};
    VkDescriptorSetLayout layouts[MENU_SETS];
    VkDescriptorSetAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorSetCount = MENU_SETS, .pSetLayouts = layouts};
    int i;
    if (g_mpool) return 1;
    if (!layout_make() || !VK_OK(vkCreateDescriptorPool(g_vk.dev, &pci, NULL, &g_mpool))) return 0;
    for (i = 0; i < MENU_SETS; ++i) layouts[i] = g_dsl;
    ai.descriptorPool = g_mpool;
    if (!VK_OK(vkAllocateDescriptorSets(g_vk.dev, &ai, g_msets))) {
        vkDestroyDescriptorPool(g_vk.dev, g_mpool, NULL);
        g_mpool = VK_NULL_HANDLE;
        return 0;
    }
    return 1;
}

static VkPipeline menu_pipe(VkFormat format) {
    int i;
    for (i = 0; i < MENU_PIPES && g_mpipes[i].pipe; ++i)
        if (g_mpipes[i].format == format) return g_mpipes[i].pipe;
    if (i == MENU_PIPES) return VK_NULL_HANDLE;
    g_mpipes[i].pipe = pipeline_new(format);
    g_mpipes[i].format = format;
    return g_mpipes[i].pipe;
}

static MenuTex* menu_tex_find(unsigned id) {
    int i;
    for (i = 0; i < MENU_TEX; ++i)
        if (g_mtex[i].id == id) return &g_mtex[i];
    return NULL;
}
static void menu_tex_free(MenuTex* t) {
    vk_defer_image(t->img, t->mem, t->view, VK_NULL_HANDLE); /* once the frames that use it are done */
    memset(t, 0, sizeof *t);
}

/* a texture made, changed (every texel, in place: the frames before read it first) or freed */
static void menu_tex(VkCommandBuffer cb, const OvVkTex* q) {
    MenuTex* t = q->id ? menu_tex_find(q->id) : NULL;
    u64 n = (u64)q->w * (u64)q->h * 4, k;
    int fresh;
    VkBuffer buf;
    VmaAllocation mem;
    VmaAllocationInfo info;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = n,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo bai = {.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                            VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                   .usage = VMA_MEMORY_USAGE_AUTO};
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {(u32)q->w, (u32)q->h, 1}};
    if (q->op == OV_VK_TEX_DESTROY) {
        if (t) menu_tex_free(t);
        return;
    }
    if (!q->id || q->w <= 0 || q->h <= 0 || !q->rgba) return;
    if (t && (q->op == OV_VK_TEX_CREATE || t->w != q->w || t->h != q->h)) {
        menu_tex_free(t);
        t = NULL;
    }
    fresh = !t;
    if (fresh) {
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .extent = {(u32)q->w, (u32)q->h, 1},
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo iai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                     .format = VK_FORMAT_R8G8B8A8_UNORM,
                                     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDescriptorImageInfo di = {g_samplers[0], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                   .pImageInfo = &di};
        if (!(t = menu_tex_find(0))) {
            WARN("vulkan: the in-game menu has more than %d textures\n", MENU_TEX);
            return;
        }
        if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &iai, &t->img, &t->mem, NULL))) {
            memset(t, 0, sizeof *t);
            return;
        }
        vci.image = t->img;
        if (!VK_OK(vkCreateImageView(g_vk.dev, &vci, NULL, &t->view))) {
            vmaDestroyImage(g_vk.vma, t->img, t->mem);
            memset(t, 0, sizeof *t);
            return;
        }
        t->set = g_msets[g_mset_next++ % MENU_SETS];
        di.imageView = t->view;
        wr.dstSet = t->set;
        vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
        t->id = q->id;
        t->w = q->w;
        t->h = q->h;
    }
    if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bci, &bai, &buf, &mem, &info))) return;
    {
        u8* d = info.pMappedData;
        const u8* s = q->rgba;
        for (k = 0; k < n; k += 4) {
            u32 a = s[k + 3];
            d[k + 0] = (u8)((s[k + 0] * a + 127) / 255);
            d[k + 1] = (u8)((s[k + 1] * a + 127) / 255);
            d[k + 2] = (u8)((s[k + 2] * a + 127) / 255);
            d[k + 3] = (u8)a;
        }
    }
    vmaFlushAllocation(g_vk.vma, mem, 0, VK_WHOLE_SIZE);
    vk_defer_buffer(buf, mem);
    if (fresh)
        vk_barrier(cb, t->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    else
        vk_barrier(cb, t->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    rec_CmdCopyBufferToImage(cb, buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    vk_barrier(cb, t->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

static int format_srgb(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

/* The menu over img, a swapchain image (view NULL: its own) or the present pass's SDR image, of the swapchain's size.
   presentable: img is in PRESENT_SRC after the present pass drew it; else in TRANSFER_DST after the frame's blit (and
   the overlay). Left as it came. Once per present, as each call is a menu frame */
void vk_menu_draw(VkCommandBuffer cb, VkImage img, VkImageView view, VkFormat format, int presentable) {
    OvVkDraw d;
    VkPipeline pipe;
    VkBuffer vbuf, ibuf;
    u64 voff, ioff;
    u8* p;
    u32 i, sw = g_vk.sc_extent.width, sh = g_vk.sc_extent.height;
    float kx, ky;
    VkDescriptorSet bound = VK_NULL_HANDLE;
    VkImageLayout layout = presentable ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkPipelineStageFlags2 stage =
        presentable ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    VkAccessFlags2 access = presentable ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_TRANSFER_WRITE_BIT;
    struct {
        float w, h;
        u32 srgb, pad;
    } pc;
    if (!ov_vk_frame(&d) || !menu_sets_make()) return;
    for (i = 0; i < d.ntex; ++i) menu_tex(cb, &d.tex[i]);
    if (!d.ncmd || !d.nvtx || !d.nidx || d.w <= 0 || d.h <= 0 || !sw || !sh) return;
    if (!view) view = sc_view(img);
    if (!view || !(pipe = menu_pipe(format))) return;
    if (!(p = vk_stage((u64)d.nvtx * 20, 4, &vbuf, &voff))) return;
    memcpy(p, d.vtx, (size_t)d.nvtx * 20);
    if (!(p = vk_stage((u64)d.nidx * 2, 4, &ibuf, &ioff))) return;
    memcpy(p, d.idx, (size_t)d.nidx * 2);
    pc.w = d.w;
    pc.h = d.h;
    pc.srgb = (u32)format_srgb(format);
    pc.pad = 0;
    kx = (float)sw / d.w;
    ky = (float)sh / d.h;
    vk_barrier(cb, img, VK_IMAGE_ASPECT_COLOR_BIT, stage, access, layout, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    {
        VkRenderingAttachmentInfo at = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                        .imageView = view,
                                        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                        .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                              .renderArea = {{0, 0}, g_vk.sc_extent},
                              .layerCount = 1,
                              .colorAttachmentCount = 1,
                              .pColorAttachments = &at};
        VkViewport vp = {0, 0, (float)sw, (float)sh, 0, 1};
        VkDeviceSize vo = voff;
        rec_CmdBeginRendering(cb, &ri);
        vk_render_forget_state(); /* another pipeline and layout: a draw after it sets everything again */
        rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        rec_CmdSetViewport(cb, 0, 1, &vp);
        rec_CmdPushConstants(cb, g_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        rec_CmdBindVertexBuffers(cb, 0, 1, &vbuf, &vo);
        rec_CmdBindIndexBuffer(cb, ibuf, ioff, VK_INDEX_TYPE_UINT16);
        for (i = 0; i < d.ncmd; ++i) {
            const OvVkCmd* c = &d.cmd[i];
            MenuTex* t = c->tex ? menu_tex_find(c->tex) : NULL;
            float x0 = c->clip[0] * kx, y0 = c->clip[1] * ky, x1 = c->clip[2] * kx, y1 = c->clip[3] * ky;
            VkRect2D r;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > (float)sw) x1 = (float)sw;
            if (y1 > (float)sh) y1 = (float)sh;
            if (!t || x1 <= x0 || y1 <= y0) continue;
            r.offset.x = (i32)x0;
            r.offset.y = (i32)y0;
            r.extent.width = (u32)(x1 - (float)r.offset.x);
            r.extent.height = (u32)(y1 - (float)r.offset.y);
            if (!r.extent.width || !r.extent.height) continue;
            if (t->set != bound) {
                rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &t->set, 0, NULL);
                bound = t->set;
            }
            rec_CmdSetScissor(cb, 0, 1, &r);
            rec_CmdDrawIndexed(cb, c->count, 1, c->first, c->base, 0);
        }
        rec_CmdEndRendering(cb);
    }
    vk_barrier(cb, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               presentable ? VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               presentable ? 0 : VK_ACCESS_2_TRANSFER_WRITE_BIT, layout);
}
