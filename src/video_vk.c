/*
Copyright (c) 2020-2026 Rupert Carmichael
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
Vulkan backend, structurally translated from video_gl.c. Render graph:
  Pass 0 (clip):  source (wmax x hmax) -> offscreen (w x h)
  Pass 1 (post):  offscreen -> swapchain, letterboxed viewport
  OSD pass:       alpha-blended over the swapchain in the same render pass

Shaders are SPIR-V loaded from "<name>.spv"; to switch to runtime GLSL
compilation (libshaderc), only vk_shader_module() needs to change.
Post-processing parameters are a single push-constant block (< 128 bytes).
One frame in flight; a fence serializes CPU uploads and GPU reads.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <math.h>

#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#define NUMPASSES 2
#define VK_SWAP_MAX 8 // Upper bound on swapchain images tracked

// The offscreen (pass 0) target format, referenced by the render pass and image
#define VK_FMT_OFFSCREEN VK_FORMAT_R8G8B8A8_UNORM

#include "jgrf.h"
#include "video.h"
#include "video_vk.h"
#include "settings.h"
#include "osd.h"

static jgrf_gdata_t *gdata = NULL;
static jg_setting_t *settings = NULL;

// SDL Window management
static SDL_Window *window;
static SDL_Cursor *cursor;

// Pointer to the video buffer when allocated by the frontend
static void *videobuf = NULL;

// Pointer to the core's video information
static jg_videoinfo_t *vidinfo = NULL;

// Core Vulkan objects
static VkInstance instance = VK_NULL_HANDLE;
static VkSurfaceKHR surface = VK_NULL_HANDLE;
static VkPhysicalDevice physdev = VK_NULL_HANDLE;
static VkDevice device = VK_NULL_HANDLE;
static uint32_t qf_graphics = 0;
static uint32_t qf_present = 0;
static VkQueue q_graphics = VK_NULL_HANDLE;
static VkQueue q_present = VK_NULL_HANDLE;
static VkPhysicalDeviceMemoryProperties memprops;

// Swapchain
static VkSwapchainKHR swapchain = VK_NULL_HANDLE;
static VkFormat swap_format = VK_FORMAT_B8G8R8A8_UNORM;
static VkExtent2D swap_extent;
static uint32_t swap_count = 0;
static VkImage swap_images[VK_SWAP_MAX];
static VkImageView swap_views[VK_SWAP_MAX];
static VkFramebuffer swap_fbos[VK_SWAP_MAX];
static uint32_t swap_index = 0; // image acquired this frame

// Render passes
static VkRenderPass rp_offscreen = VK_NULL_HANDLE;
static VkRenderPass rp_screen = VK_NULL_HANDLE;

// Command + sync (single frame in flight)
static VkCommandPool cmdpool = VK_NULL_HANDLE;
static VkCommandBuffer cmdbuf = VK_NULL_HANDLE;
static VkSemaphore sem_acquire = VK_NULL_HANDLE;
static VkSemaphore sem_render = VK_NULL_HANDLE;
static VkFence fence_frame = VK_NULL_HANDLE;

// Managed image: image + backing memory + view
typedef struct _vktex {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkFormat format;
    uint32_t w, h;
    VkImageLayout layout;
} vktex_t;

static vktex_t tex_source;     // raw game output (wmax x hmax)
static vktex_t tex_offscreen;  // clipped target sampled by pass 1 (w x h)
static vktex_t tex_osd;        // OSD canvas (JGRF_OSD_W x JGRF_OSD_H)
static VkFramebuffer fbo_offscreen = VK_NULL_HANDLE;

// Samplers: nearest and linear, picked per pass like texfilter[] in GL
static VkSampler smp_nearest = VK_NULL_HANDLE;
static VkSampler smp_linear = VK_NULL_HANDLE;

// Staging buffers for per-frame uploads (source pixels, OSD canvas)
typedef struct _vkbuf {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapped;
    VkDeviceSize size;
} vkbuf_t;

static vkbuf_t stage_source;
static vkbuf_t stage_osd;
static vkbuf_t readback; // for screenshots

// Vertex buffers (per vertex: vec2 position, vec2 texcoord)
static vkbuf_t vbo_pass0; // clip quad, texcoords updated on clip change
static vkbuf_t vbo_pass1; // full output quad, static
static vkbuf_t vbo_osd;   // OSD quad, texcoords updated per frame

// Descriptor set layout, pool, and per-image sets
static VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
static VkDescriptorPool dpool = VK_NULL_HANDLE;
static VkDescriptorSet ds_source = VK_NULL_HANDLE;     // samples tex_source
static VkDescriptorSet ds_offscreen = VK_NULL_HANDLE;  // samples tex_offscreen
static VkDescriptorSet ds_osd = VK_NULL_HANDLE;        // samples tex_osd
static VkPipelineLayout playout = VK_NULL_HANDLE;

// Pipelines: pass0 (clip), pass1 (selected post-process), osd
static VkPipeline pipe_pass0 = VK_NULL_HANDLE;
static VkPipeline pipe_pass1 = VK_NULL_HANDLE;
static VkPipeline pipe_osd = VK_NULL_HANDLE;

// Push-constant block for the post-process fragment shader (std430 layout)
typedef struct _vk_pushconst {
    float sourceSize[4];
    float targetSize[4];
    int32_t masktype;
    float maskstr;
    float scanstr;
    float sharpness;
    float curve;
    float corner;
    float tcurve;
} vk_pushconst_t;

static vk_pushconst_t pushconst;

// Vertex data: triangle strip, per-vertex (x, y, u, v)
// Pass 0 texcoords (clip rectangle) are patched in refresh()
static float verts_pass0[] = {
    -1.0f, -1.0f, 0.0f, 0.0f, // Left Bottom
    -1.0f,  1.0f, 0.0f, 1.0f, // Left Top
     1.0f, -1.0f, 1.0f, 0.0f, // Right Bottom
     1.0f,  1.0f, 1.0f, 1.0f, // Right Top
};

static const float verts_pass1[] = {
    -1.0f, -1.0f, 0.0f, 0.0f,
    -1.0f,  1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 1.0f,
};

// Pixel Format
static unsigned pixfmt_active = JG_PIXFMT_XRGB8888;
static struct _pixfmt {
    VkFormat format;
    size_t size;
} pixfmt;

// Dimensions
static struct _dimensions {
    int ww; int wh;
    float rw; float rh;
    float xo; float yo;
    float dpiscale;
} dimensions;

// Last window pixel size the swapchain was built for (zero = freshly created)
static int win_pw = 0;
static int win_ph = 0;

// Last seen aspect ratio for change detection
static double aspect_prev = 0.0;

// Currently selected post-process shader, to detect rehash changes
static int shader_active = -1;

// Forward declarations for internal helpers
static void jgrf_video_vk_refresh(void);
static void jgrf_video_vk_setup(void);
static void jgrf_video_vk_recreate_swapchain(void);
static void vk_pipelines_create(void);
static void vk_pipelines_destroy(void);

// Log a fatal Vulkan error
static void vk_check(VkResult r, const char *what) {
    if (r != VK_SUCCESS)
        jgrf_log(JG_LOG_ERR, "Vulkan: %s failed (%d)\n", what, (int)r);
}

// Find a memory type index matching a type filter and required properties
static uint32_t vk_memtype(uint32_t filter, VkMemoryPropertyFlags props) {
    for (uint32_t i = 0; i < memprops.memoryTypeCount; ++i) {
        if ((filter & (1u << i)) &&
            (memprops.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    jgrf_log(JG_LOG_ERR, "Vulkan: no suitable memory type\n");
    return 0;
}

// Allocate a buffer with backing memory, mapped if host visible
static void vk_buffer_create(vkbuf_t *b, VkDeviceSize size,
    VkBufferUsageFlags usage, VkMemoryPropertyFlags props) {
    b->size = size;
    b->mapped = NULL;

    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vk_check(vkCreateBuffer(device, &bi, NULL, &b->buffer), "vkCreateBuffer");

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b->buffer, &req);

    VkMemoryAllocateInfo ai =
        { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = vk_memtype(req.memoryTypeBits, props);
    vk_check(vkAllocateMemory(device, &ai, NULL, &b->memory),
        "vkAllocateMemory(buffer)");
    vkBindBufferMemory(device, b->buffer, b->memory, 0);

    if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        vkMapMemory(device, b->memory, 0, size, 0, &b->mapped);
}

static void vk_buffer_destroy(vkbuf_t *b) {
    if (b->mapped) {
        vkUnmapMemory(device, b->memory); b->mapped = NULL;
    }
    if (b->buffer) {
        vkDestroyBuffer(device, b->buffer, NULL);
        b->buffer = VK_NULL_HANDLE;
    }
    if (b->memory) {
        vkFreeMemory(device, b->memory, NULL);
        b->memory = VK_NULL_HANDLE;
    }
}

// Begin a one-shot command buffer for setup/transfer work
static VkCommandBuffer vk_cmd_begin(void) {
    VkCommandBufferAllocateInfo ai =
        { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = cmdpool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(device, &ai, &cb);

    VkCommandBufferBeginInfo bi =
        { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

static void vk_cmd_end(VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(q_graphics, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(q_graphics);
    vkFreeCommandBuffers(device, cmdpool, 1, &cb);
}

// Record an image layout transition
static void vk_transition(VkCommandBuffer cb, vktex_t *t,
    VkImageLayout newlayout, VkPipelineStageFlags src_stage,
    VkPipelineStageFlags dst_stage, VkAccessFlags src_access,
    VkAccessFlags dst_access) {

    VkImageMemoryBarrier b =
        { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = t->layout;
    b.newLayout = newlayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
    t->layout = newlayout;
}

// Create a managed image (sampled, optionally a colour attachment)
static void vk_tex_create(vktex_t *t, uint32_t w, uint32_t h, VkFormat fmt,
    int attachment) {
    t->w = w; t->h = h; t->format = fmt;
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (attachment)
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent.width = w; ii.extent.height = h; ii.extent.depth = 1;
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vk_check(vkCreateImage(device, &ii, NULL, &t->image), "vkCreateImage");

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, t->image, &req);
    VkMemoryAllocateInfo ai =
        { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = vk_memtype(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vk_check(vkAllocateMemory(device, &ai, NULL, &t->memory),
        "vkAllocateMemory(image)");
    vkBindImageMemory(device, t->image, t->memory, 0);

    VkImageViewCreateInfo vi =
        { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = t->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    vk_check(vkCreateImageView(device, &vi, NULL, &t->view),
        "vkCreateImageView");
}

static void vk_tex_destroy(vktex_t *t) {
    if (t->view) {
        vkDestroyImageView(device, t->view, NULL);
        t->view = VK_NULL_HANDLE;
    }
    if (t->image) {
        vkDestroyImage(device, t->image, NULL);
        t->image = VK_NULL_HANDLE;
    }
    if (t->memory) {
        vkFreeMemory(device, t->memory, NULL);
        t->memory = VK_NULL_HANDLE;
    }
}

// Update a descriptor set to sample a given image view with a sampler
static void vk_ds_write(VkDescriptorSet set, VkImageView view,
    VkSampler sampler) {
    VkDescriptorImageInfo info = { 0 };
    info.sampler = sampler;
    info.imageView = view;
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w =
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = set;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(device, 1, &w, 0, NULL);
}

// Load a SPIR-V module from "<name>.spv", searching binpath then DATADIR
static VkShaderModule vk_shader_module(const char *name) {
    char path[256];
    struct stat fbuf;

    snprintf(path, sizeof(path), "%s%cshaders%c%s.spv",
        gdata->binpath, SEP, SEP, name);

    if (stat(path, &fbuf) != 0) {
#ifdef JGRF_STATIC
#if defined(DATADIR)
        snprintf(path, sizeof(path),
            "%s%cjollygood%c%s%cshaders%c%s.spv",
            DATADIR, SEP, SEP, jg_get_coreinfo("")->name, SEP, SEP, name);
#endif
#else
#if defined(DATADIR)
        snprintf(path, sizeof(path),
            "%s%cjollygood%cjgrf%cshaders%c%s.spv",
            DATADIR, SEP, SEP, SEP, SEP, name);
#endif
#endif
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        jgrf_log(JG_LOG_WRN, "Could not open SPIR-V module: %s\n", path);
        return VK_NULL_HANDLE;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);

    // SPIR-V is a stream of 32-bit words; size must be a multiple of four
    if (size <= 0 || (size & 3)) {
        jgrf_log(JG_LOG_WRN, "Bad SPIR-V size: %s\n", path);
        fclose(file);
        return VK_NULL_HANDLE;
    }

    uint32_t *code = (uint32_t*)malloc((size_t)size);
    if (!code || fread(code, 1, (size_t)size, file) != (size_t)size) {
        jgrf_log(JG_LOG_WRN, "Could not read SPIR-V module: %s\n", path);
        free(code);
        fclose(file);
        return VK_NULL_HANDLE;
    }
    fclose(file);

    VkShaderModuleCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = (size_t)size;
    ci.pCode = code;
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, NULL, &mod) != VK_SUCCESS)
        jgrf_log(JG_LOG_WRN, "vkCreateShaderModule failed: %s\n", path);

    free(code);
    return mod;
}

// Map the selected shader index to a fragment SPIR-V name and per-pass filters
static const char *vk_shader_select(int *filter_src, int *filter_dst) {
    *filter_src = 0; // source texture (GL texfilter[0])
    *filter_dst = 0; // offscreen texture (GL texfilter[1])
    switch (settings[VIDEO_SHADER].val) {
        default: case 0: return "default.frag"; // Nearest Neighbour
        case 1: *filter_dst = 1; return "default.frag"; // Linear
        case 2: *filter_src = 1; *filter_dst = 1;
                return "sharp-bilinear.frag";
        case 3: return "aann.frag";
        case 4: return "crt-yee64.frag";
        case 5: return "crtea.frag";
        case 6: return "lcd.frag";
    }
}

// Build a graphics pipeline from a vertex + fragment SPIR-V pair
static VkPipeline vk_pipeline_build(const char *vname, const char *fname,
    VkRenderPass rp, int blend) {
    VkShaderModule vs = vk_shader_module(vname);
    VkShaderModule fs = vk_shader_module(fname);
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        // Fall back to the default fragment so the screen is never black
        if (fs == VK_NULL_HANDLE) {
            jgrf_log(JG_LOG_WRN,
                "Falling back to default.frag for missing %s\n", fname);
            fs = vk_shader_module("default.frag");
        }
        if (vs == VK_NULL_HANDLE)
            vs = vk_shader_module("default.vert");
        if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
            if (vs) vkDestroyShaderModule(device, vs, NULL);
            if (fs) vkDestroyShaderModule(device, fs, NULL);
            return VK_NULL_HANDLE;
        }
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
    };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    // Vertex input: one binding, vec2 position + vec2 texcoord
    VkVertexInputBindingDescription bind = { 0 };
    bind.binding = 0;
    bind.stride = 4 * sizeof(float);
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attr[2] = { { 0 }, { 0 } };
    attr[0].location = 0; attr[0].binding = 0;
    attr[0].format = VK_FORMAT_R32G32_SFLOAT; attr[0].offset = 0;
    attr[1].location = 1; attr[1].binding = 0;
    attr[1].format = VK_FORMAT_R32G32_SFLOAT;
    attr[1].offset = 2 * sizeof(float);

    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = attr;

    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    // Viewport and scissor are dynamic, set per draw
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba = { 0 };
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (blend) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo cb =
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn[2] =
        { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds =
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo pi =
        { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &ds;
    pi.layout = playout;
    pi.renderPass = rp;
    pi.subpass = 0;

    VkPipeline pipe = VK_NULL_HANDLE;
    vk_check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, NULL,
        &pipe), "vkCreateGraphicsPipelines");

    vkDestroyShaderModule(device, vs, NULL);
    vkDestroyShaderModule(device, fs, NULL);
    return pipe;
}

// Set the Vulkan pixel format for the source image
static void jgrf_video_vk_refresh_pixfmt(void) {
    pixfmt_active = vidinfo->pixfmt;
    switch (vidinfo->pixfmt) {
        case JG_PIXFMT_XRGB8888:
            pixfmt.format = VK_FORMAT_B8G8R8A8_UNORM;
            pixfmt.size = sizeof(uint32_t);
            jgrf_log(JG_LOG_DBG, "Pixel format: B8G8R8A8_UNORM\n");
            break;
        case JG_PIXFMT_XBGR8888:
            pixfmt.format = VK_FORMAT_R8G8B8A8_UNORM;
            pixfmt.size = sizeof(uint32_t);
            jgrf_log(JG_LOG_DBG, "Pixel format: R8G8B8A8_UNORM\n");
            break;
        case JG_PIXFMT_RGBX5551:
            pixfmt.format = VK_FORMAT_R5G5B5A1_UNORM_PACK16;
            pixfmt.size = sizeof(uint16_t);
            jgrf_log(JG_LOG_DBG, "Pixel format: R5G5B5A1_UNORM_PACK16\n");
            break;
        case JG_PIXFMT_RGB565:
            pixfmt.format = VK_FORMAT_R5G6B5_UNORM_PACK16;
            pixfmt.size = sizeof(uint16_t);
            jgrf_log(JG_LOG_DBG, "Pixel format: R5G6B5_UNORM_PACK16\n");
            break;
        default:
            jgrf_log(JG_LOG_ERR, "Unknown pixel format, exiting...\n");
            break;
    }
}

// Create the SDL Vulkan window
void jgrf_video_vk_create(void) {
    // Vulkan has no API sub-modes; the setting just selects this backend
    SDL_WindowFlags windowflags = SDL_WINDOW_VULKAN |
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;

    char title[128];
    gdata = jgrf_gdata_ptr();
    snprintf(title, sizeof(title), "%s", gdata->gamename);

    jgrf_log(JG_LOG_DBG, "SDL Video driver: %s\n",
        SDL_GetCurrentVideoDriver());

    if (!strcmp(SDL_GetCurrentVideoDriver(), "kmsdrm")) {
        SDL_DisplayID disp_id = SDL_GetPrimaryDisplay();
        const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(disp_id);
        dimensions.ww = dm->w;
        dimensions.wh = dm->h;
    }
    else {
        dimensions.ww =
            (vidinfo->aspect * vidinfo->h * settings[VIDEO_SCALE].val) + 0.5;
        dimensions.wh = (vidinfo->h * settings[VIDEO_SCALE].val) + 0.5;
    }
    dimensions.rw = dimensions.ww;
    dimensions.rh = dimensions.wh;

    jgrf_log(JG_LOG_DBG, "Creating window with dimensions: %d x %d\n",
        dimensions.ww, dimensions.wh);

    window = SDL_CreateWindow(title, dimensions.ww, dimensions.wh,
        windowflags);
    if (!window)
        jgrf_log(JG_LOG_ERR, "Failed to create window: %s\n", SDL_GetError());

    int x, y;
    SDL_GetWindowSizeInPixels(window, &x, &y);
    dimensions.dpiscale = (float)x / dimensions.ww;

    jgrf_video_icon_load(window);

    // Do post window creation Vulkan setup
    jgrf_video_vk_setup();

    SDL_HideCursor();

    if (settings[VIDEO_FULLSCREEN].val)
        SDL_SetWindowFullscreen(window, true);
}

// Create the Vulkan instance, asking SDL for the required surface extensions
static void vk_instance_create(void) {
    Uint32 next = 0;
    const char* const *exts_sdl =
        SDL_Vulkan_GetInstanceExtensions(&next);

    // Make room for a portability extension on macOS
    const char *exts[16];
    Uint32 n = 0;
    for (Uint32 i = 0; i < next && n < 15; ++i)
        exts[n++] = exts_sdl[i];

    VkInstanceCreateFlags flags = 0;
#ifdef __APPLE__
    exts[n++] = "VK_KHR_portability_enumeration";
    flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "jgrf";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName = "jgrf";
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ci =
        { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.flags = flags;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = n;
    ci.ppEnabledExtensionNames = exts;
    vk_check(vkCreateInstance(&ci, NULL, &instance), "vkCreateInstance");
}

// Pick a physical device and the graphics/present queue families
static void vk_device_create(void) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, NULL);
    if (!count)
        jgrf_log(JG_LOG_ERR, "Vulkan: no physical devices\n");

    VkPhysicalDevice *devs =
        (VkPhysicalDevice*)calloc(count, sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(instance, &count, devs);

    // Prefer a discrete GPU, else take the first that has the queues we need
    int chosen = -1;
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devs[i], &props);
        if (chosen < 0)
            chosen = (int)i;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            chosen = (int)i;
            break;
        }
    }
    physdev = devs[chosen];
    free(devs);

    VkPhysicalDeviceProperties devprops;
    vkGetPhysicalDeviceProperties(physdev, &devprops);
    jgrf_log(JG_LOG_INF, "Video: Vulkan %u.%u.%u - %s\n",
        VK_VERSION_MAJOR(devprops.apiVersion),
        VK_VERSION_MINOR(devprops.apiVersion),
        VK_VERSION_PATCH(devprops.apiVersion),
        devprops.deviceName);

    vkGetPhysicalDeviceMemoryProperties(physdev, &memprops);

    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physdev, &qcount, NULL);
    VkQueueFamilyProperties *qfp =
        (VkQueueFamilyProperties*)calloc(qcount, sizeof(*qfp));
    vkGetPhysicalDeviceQueueFamilyProperties(physdev, &qcount, qfp);

    int have_g = 0, have_p = 0;
    for (uint32_t i = 0; i < qcount; ++i) {
        if (!have_g && (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            qf_graphics = i; have_g = 1;
        }
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physdev, i, surface, &present);
        if (!have_p && present) {
            qf_present = i; have_p = 1;
        }
    }
    free(qfp);
    if (!have_g || !have_p)
        jgrf_log(JG_LOG_ERR, "Vulkan: no graphics/present queue\n");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci[2];
    uint32_t qn = 0;
    VkDeviceQueueCreateInfo base =
        { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    base.queueCount = 1;
    base.pQueuePriorities = &prio;
    base.queueFamilyIndex = qf_graphics;
    qci[qn++] = base;
    if (qf_present != qf_graphics) {
        base.queueFamilyIndex = qf_present;
        qci[qn++] = base;
    }

    const char *dexts[4];
    uint32_t dn = 0;
    dexts[dn++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
#ifdef __APPLE__
    dexts[dn++] = "VK_KHR_portability_subset";
#endif

    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = qn;
    dci.pQueueCreateInfos = qci;
    dci.enabledExtensionCount = dn;
    dci.ppEnabledExtensionNames = dexts;
    vk_check(vkCreateDevice(physdev, &dci, NULL, &device), "vkCreateDevice");

    vkGetDeviceQueue(device, qf_graphics, 0, &q_graphics);
    vkGetDeviceQueue(device, qf_present, 0, &q_present);
}

// Choose a surface format, preferring B8G8R8A8 UNORM
static void vk_choose_surface_format(void) {
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physdev, surface, &n, NULL);
    VkSurfaceFormatKHR *fmts =
        (VkSurfaceFormatKHR*)calloc(n, sizeof(*fmts));
    vkGetPhysicalDeviceSurfaceFormatsKHR(physdev, surface, &n, fmts);

    swap_format = fmts[0].format;
    for (uint32_t i = 0; i < n; ++i) {
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            swap_format = fmts[i].format;
            break;
        }
    }
    free(fmts);
}

// Create or recreate the swapchain and its framebuffers
static void vk_swapchain_create(void) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physdev, surface, &caps);

    int pw, ph;
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    swap_extent = caps.currentExtent;
    if (caps.currentExtent.width == 0xFFFFFFFFu) {
        swap_extent.width = (uint32_t)pw;
        swap_extent.height = (uint32_t)ph;
    }
    if (swap_extent.width < caps.minImageExtent.width)
        swap_extent.width = caps.minImageExtent.width;
    if (swap_extent.height < caps.minImageExtent.height)
        swap_extent.height = caps.minImageExtent.height;
    if (swap_extent.width > caps.maxImageExtent.width)
        swap_extent.width = caps.maxImageExtent.width;
    if (swap_extent.height > caps.maxImageExtent.height)
        swap_extent.height = caps.maxImageExtent.height;

    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount && want > caps.maxImageCount)
        want = caps.maxImageCount;

    // FIFO (vsync on) by default; IMMEDIATE for benchmark mode if available
    VkPresentModeKHR present = VK_PRESENT_MODE_FIFO_KHR;
    if (bmark) {
        uint32_t pn = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physdev, surface, &pn, NULL);
        VkPresentModeKHR *pm =
            (VkPresentModeKHR*)calloc(pn, sizeof(*pm));
        vkGetPhysicalDeviceSurfacePresentModesKHR(physdev, surface, &pn, pm);
        for (uint32_t i = 0; i < pn; ++i)
            if (pm[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
                present = VK_PRESENT_MODE_IMMEDIATE_KHR;
        free(pm);
    }

    VkSwapchainCreateInfoKHR ci =
        { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = surface;
    ci.minImageCount = want;
    ci.imageFormat = swap_format;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = swap_extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    uint32_t qfs[2] = { qf_graphics, qf_present };
    if (qf_graphics != qf_present) {
        ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = 2;
        ci.pQueueFamilyIndices = qfs;
    }
    else {
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = present;
    ci.clipped = VK_TRUE;
    vk_check(vkCreateSwapchainKHR(device, &ci, NULL, &swapchain),
        "vkCreateSwapchainKHR");

    vkGetSwapchainImagesKHR(device, swapchain, &swap_count, NULL);
    if (swap_count > VK_SWAP_MAX)
        swap_count = VK_SWAP_MAX;
    vkGetSwapchainImagesKHR(device, swapchain, &swap_count, swap_images);

    for (uint32_t i = 0; i < swap_count; ++i) {
        VkImageViewCreateInfo vi = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = swap_images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = swap_format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        vk_check(vkCreateImageView(device, &vi, NULL, &swap_views[i]),
            "vkCreateImageView(swap)");

        VkFramebufferCreateInfo fi = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = rp_screen;
        fi.attachmentCount = 1;
        fi.pAttachments = &swap_views[i];
        fi.width = swap_extent.width;
        fi.height = swap_extent.height;
        fi.layers = 1;
        vk_check(vkCreateFramebuffer(device, &fi, NULL, &swap_fbos[i]),
            "vkCreateFramebuffer(swap)");
    }
}

static void vk_swapchain_destroy(void) {
    for (uint32_t i = 0; i < swap_count; ++i) {
        if (swap_fbos[i])
            vkDestroyFramebuffer(device, swap_fbos[i], NULL);
        if (swap_views[i])
            vkDestroyImageView(device, swap_views[i], NULL);
        swap_fbos[i] = VK_NULL_HANDLE;
        swap_views[i] = VK_NULL_HANDLE;
    }
    if (swapchain)
        vkDestroySwapchainKHR(device, swapchain, NULL);
    swapchain = VK_NULL_HANDLE;
    swap_count = 0;
}

// Create the offscreen and onscreen render passes
static void vk_renderpasses_create(void) {
    // Offscreen: clear, render, leave it ready to be sampled by pass 1
    {
        VkAttachmentDescription a = { 0 };
        a.format = VK_FMT_OFFSCREEN;
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        a.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference ref = { 0,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sp = { 0 };
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &ref;

        VkSubpassDependency dep = { 0 };
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo ci = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        ci.attachmentCount = 1;
        ci.pAttachments = &a;
        ci.subpassCount = 1;
        ci.pSubpasses = &sp;
        ci.dependencyCount = 1;
        ci.pDependencies = &dep;
        vk_check(vkCreateRenderPass(device, &ci, NULL, &rp_offscreen),
            "vkCreateRenderPass(offscreen)");
    }

    // Onscreen: clear the swapchain image, draw post-process then OSD, present
    {
        VkAttachmentDescription a = { 0 };
        a.format = swap_format;
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        a.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference ref = { 0,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sp = { 0 };
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &ref;

        VkSubpassDependency dep = { 0 };
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = 0;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo ci = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        ci.attachmentCount = 1;
        ci.pAttachments = &a;
        ci.subpassCount = 1;
        ci.pSubpasses = &sp;
        ci.dependencyCount = 1;
        ci.pDependencies = &dep;
        vk_check(vkCreateRenderPass(device, &ci, NULL, &rp_screen),
            "vkCreateRenderPass(screen)");
    }
}

// Create or recreate the offscreen target and framebuffer at w x h
static void vk_offscreen_create(void) {
    if (fbo_offscreen) {
        vkDestroyFramebuffer(device, fbo_offscreen, NULL);
        fbo_offscreen = VK_NULL_HANDLE;
    }
    if (tex_offscreen.image)
        vk_tex_destroy(&tex_offscreen);

    vk_tex_create(&tex_offscreen, vidinfo->w, vidinfo->h,
        VK_FMT_OFFSCREEN, 1);

    VkFramebufferCreateInfo fi =
        { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fi.renderPass = rp_offscreen;
    fi.attachmentCount = 1;
    fi.pAttachments = &tex_offscreen.view;
    fi.width = vidinfo->w;
    fi.height = vidinfo->h;
    fi.layers = 1;
    vk_check(vkCreateFramebuffer(device, &fi, NULL, &fbo_offscreen),
        "vkCreateFramebuffer(offscreen)");

    if (ds_offscreen) {
        int fs, fd; (void)vk_shader_select(&fs, &fd);
        vk_ds_write(ds_offscreen, tex_offscreen.view,
            fd ? smp_linear : smp_nearest);
    }
}

// Create or recreate the source image at wmax x hmax
static void vk_source_create(void) {
    if (tex_source.image)
        vk_tex_destroy(&tex_source);
    if (stage_source.buffer)
        vk_buffer_destroy(&stage_source);

    vk_tex_create(&tex_source, vidinfo->wmax, vidinfo->hmax,
        pixfmt.format, 0);

    vk_buffer_create(&stage_source,
        (VkDeviceSize)vidinfo->wmax * vidinfo->hmax * pixfmt.size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (ds_source) {
        int fs, fd; (void)vk_shader_select(&fs, &fd);
        vk_ds_write(ds_source, tex_source.view,
            fs ? smp_linear : smp_nearest);
    }

    // Transition to shader-read so the first frame's sampler is valid
    VkCommandBuffer cb = vk_cmd_begin();
    vk_transition(cb, &tex_source,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, VK_ACCESS_SHADER_READ_BIT);
    vk_cmd_end(cb);
}

// Create samplers, descriptor layout/pool/sets, and pipeline layout
static void vk_descriptors_create(void) {
    VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minFilter = si.magFilter = VK_FILTER_NEAREST;
    vk_check(vkCreateSampler(device, &si, NULL, &smp_nearest),
        "vkCreateSampler(nearest)");
    si.minFilter = si.magFilter = VK_FILTER_LINEAR;
    vk_check(vkCreateSampler(device, &si, NULL, &smp_linear),
        "vkCreateSampler(linear)");

    VkDescriptorSetLayoutBinding b = { 0 };
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li =
        { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 1;
    li.pBindings = &b;
    vk_check(vkCreateDescriptorSetLayout(device, &li, NULL, &dsl),
        "vkCreateDescriptorSetLayout");

    VkDescriptorPoolSize ps = {
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 };
    VkDescriptorPoolCreateInfo pci =
        { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = 3;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    vk_check(vkCreateDescriptorPool(device, &pci, NULL, &dpool),
        "vkCreateDescriptorPool");

    VkDescriptorSetLayout layouts[3] = { dsl, dsl, dsl };
    VkDescriptorSet sets[3];
    VkDescriptorSetAllocateInfo dai =
        { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = dpool;
    dai.descriptorSetCount = 3;
    dai.pSetLayouts = layouts;
    vk_check(vkAllocateDescriptorSets(device, &dai, sets),
        "vkAllocateDescriptorSets");
    ds_source = sets[0];
    ds_offscreen = sets[1];
    ds_osd = sets[2];

    VkPushConstantRange pcr = { 0 };
    pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(vk_pushconst_t);
    VkPipelineLayoutCreateInfo pli =
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    vk_check(vkCreatePipelineLayout(device, &pli, NULL, &playout),
        "vkCreatePipelineLayout");
}

// Build the three pipelines (pass0, pass1, OSD)
static void vk_pipelines_create(void) {
    int filter_src, filter_dst;
    const char *frag = vk_shader_select(&filter_src, &filter_dst);
    shader_active = settings[VIDEO_SHADER].val;

    // Update per-pass input filters
    vk_ds_write(ds_source, tex_source.view,
        filter_src ? smp_linear : smp_nearest);
    vk_ds_write(ds_offscreen, tex_offscreen.view,
        filter_dst ? smp_linear : smp_nearest);

    pipe_pass0 = vk_pipeline_build("default.vert", "default.frag",
        rp_offscreen, 0);
    pipe_pass1 = vk_pipeline_build("default.vert", frag, rp_screen, 0);
    pipe_osd = vk_pipeline_build("default.vert", "osd.frag",
        rp_screen, 1);
}

static void vk_pipelines_destroy(void) {
    if (pipe_pass0) {
        vkDestroyPipeline(device, pipe_pass0, NULL);
        pipe_pass0 = VK_NULL_HANDLE;
    }
    if (pipe_pass1) {
        vkDestroyPipeline(device, pipe_pass1, NULL);
        pipe_pass1 = VK_NULL_HANDLE;
    }
    if (pipe_osd) {
        vkDestroyPipeline(device, pipe_osd, NULL);
        pipe_osd = VK_NULL_HANDLE;
    }
}

// Set up Vulkan
static void jgrf_video_vk_setup(void) {
    vk_instance_create();
    if (!SDL_Vulkan_CreateSurface(window, instance, NULL, &surface))
        jgrf_log(JG_LOG_ERR, "SDL_Vulkan_CreateSurface: %s\n",
            SDL_GetError());
    vk_device_create();
    vk_choose_surface_format();

    // Command pool + the single reused frame command buffer
    VkCommandPoolCreateInfo cpi =
        { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = qf_graphics;
    vk_check(vkCreateCommandPool(device, &cpi, NULL, &cmdpool),
        "vkCreateCommandPool");

    VkCommandBufferAllocateInfo cbi =
        { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = cmdpool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &cbi, &cmdbuf);

    VkSemaphoreCreateInfo semi =
        { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    vkCreateSemaphore(device, &semi, NULL, &sem_acquire);
    vkCreateSemaphore(device, &semi, NULL, &sem_render);
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(device, &fi, NULL, &fence_frame);

    vk_descriptors_create();

    // OSD canvas image, staging buffer, and descriptor
    vk_tex_create(&tex_osd, JGRF_OSD_W, JGRF_OSD_H,
        VK_FORMAT_R8G8B8A8_UNORM, 0);
    vk_buffer_create(&stage_osd,
        (VkDeviceSize)JGRF_OSD_W * JGRF_OSD_H * 4,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vk_ds_write(ds_osd, tex_osd.view, smp_nearest);
    {
        VkCommandBuffer cb = vk_cmd_begin();
        vk_transition(cb, &tex_osd,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, VK_ACCESS_SHADER_READ_BIT);
        vk_cmd_end(cb);
    }

    // Vertex buffers (host visible so refresh()/OSD can patch texcoords)
    vk_buffer_create(&vbo_pass0, sizeof(verts_pass0),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    memcpy(vbo_pass0.mapped, verts_pass0, sizeof(verts_pass0));

    vk_buffer_create(&vbo_pass1, sizeof(verts_pass1),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    memcpy(vbo_pass1.mapped, verts_pass1, sizeof(verts_pass1));

    vk_buffer_create(&vbo_osd, 4 * 4 * sizeof(float),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    // Create the source and offscreen images, render passes, and pipelines
    vk_source_create();
    vk_renderpasses_create();
    vk_offscreen_create();
    vk_swapchain_create();
    vk_pipelines_create();

    jgrf_video_vk_resize();
    jgrf_video_vk_refresh();
}

// Initialize video buffer
int jgrf_video_vk_init(void) {
    settings = jgrf_settings_ptr();
    gdata = jgrf_gdata_ptr();

    if (!vidinfo)
        jgrf_log(JG_LOG_ERR, "Vulkan: video info unset\n");

    if (!(gdata->hints & JG_HINT_VIDEO_INTERNAL)) {
        videobuf = (void*)calloc(vidinfo->wmax * vidinfo->hmax, pixfmt.size);
        vidinfo->buf = videobuf;
    }

    if (gdata->hints & JG_HINT_VIDEO_PRESCALED)
        settings[VIDEO_SCALE].val = 1;

    jgrf_osd_init();

    return 1;
}

// Deinitialize Vulkan video
void jgrf_video_vk_deinit(void) {
    if (device) vkDeviceWaitIdle(device);

    jgrf_osd_deinit();

    vk_pipelines_destroy();

    vk_buffer_destroy(&vbo_pass0);
    vk_buffer_destroy(&vbo_pass1);
    vk_buffer_destroy(&vbo_osd);
    vk_buffer_destroy(&stage_source);
    vk_buffer_destroy(&stage_osd);
    if (readback.buffer)
        vk_buffer_destroy(&readback);

    if (fbo_offscreen)
        vkDestroyFramebuffer(device, fbo_offscreen, NULL);
    vk_tex_destroy(&tex_offscreen);
    vk_tex_destroy(&tex_source);
    vk_tex_destroy(&tex_osd);

    vk_swapchain_destroy();

    if (rp_offscreen)
        vkDestroyRenderPass(device, rp_offscreen, NULL);
    if (rp_screen)
        vkDestroyRenderPass(device, rp_screen, NULL);

    if (playout)
        vkDestroyPipelineLayout(device, playout, NULL);
    if (dpool)
        vkDestroyDescriptorPool(device, dpool, NULL);
    if (dsl)
        vkDestroyDescriptorSetLayout(device, dsl, NULL);
    if (smp_nearest)
        vkDestroySampler(device, smp_nearest, NULL);
    if (smp_linear)
        vkDestroySampler(device, smp_linear, NULL);

    if (sem_acquire)
        vkDestroySemaphore(device, sem_acquire, NULL);
    if (sem_render)
        vkDestroySemaphore(device, sem_render, NULL);
    if (fence_frame)
        vkDestroyFence(device, fence_frame, NULL);
    if (cmdpool)
        vkDestroyCommandPool(device, cmdpool, NULL);

    if (device)
        vkDestroyDevice(device, NULL);
    if (surface)
        vkDestroySurfaceKHR(instance, surface, NULL);
    if (instance)
        vkDestroyInstance(instance, NULL);

    if (!(gdata->hints & JG_HINT_VIDEO_INTERNAL)) {
        if (videobuf)
            free(videobuf);
    }

    if (cursor) SDL_DestroyCursor(cursor);
    SDL_DestroyWindow(window);
}

// Toggle between fullscreen and windowed
void jgrf_video_vk_fullscreen(void) {
    settings[VIDEO_FULLSCREEN].val ^= 1;
    SDL_SetWindowFullscreen(window,
        settings[VIDEO_FULLSCREEN].val ? true : false);
    jgrf_video_vk_resize();
}

// Refresh any video settings that may have changed
static void jgrf_video_vk_refresh(void) {
    if (vidinfo->aspect != aspect_prev) {
        aspect_prev = vidinfo->aspect;
        jgrf_video_vk_resize();
    }

    float top = (float)vidinfo->y / vidinfo->hmax;
    float bottom = 1.0f + top -
        ((vidinfo->hmax - (float)vidinfo->h) / vidinfo->hmax);
    float left = (float)vidinfo->x / vidinfo->wmax;
    float right = 1.0f + left -
        ((vidinfo->wmax - (float)vidinfo->w) / vidinfo->wmax);

    int refmt = (pixfmt_active != vidinfo->pixfmt);
    if (refmt)
        jgrf_video_vk_refresh_pixfmt();

    // Rebuild source/offscreen images if the resolution or pixel format changed
    // verts_pass0 layout per vertex: x, y, u, v.  v0=LB v1=LT v2=RB v3=RT.
    int changed = (verts_pass0[3]  != top    || verts_pass0[7]  != bottom ||
                   verts_pass0[2]  != left   || verts_pass0[10] != right);

    int resized = ((uint32_t)vidinfo->wmax != tex_source.w ||
                   (uint32_t)vidinfo->hmax != tex_source.h ||
                   (uint32_t)vidinfo->w    != tex_offscreen.w ||
                   (uint32_t)vidinfo->h    != tex_offscreen.h);

    if (changed || refmt || resized) {
        verts_pass0[2]  = left;  verts_pass0[3]  = top;    // LB
        verts_pass0[6]  = left;  verts_pass0[7]  = bottom; // LT
        verts_pass0[10] = right; verts_pass0[11] = top;    // RB
        verts_pass0[14] = right; verts_pass0[15] = bottom; // RT
        memcpy(vbo_pass0.mapped, verts_pass0, sizeof(verts_pass0));

        if (refmt || resized) {
            vkDeviceWaitIdle(device);
            vk_source_create();
            vk_offscreen_create();
        }
    }

    // Update push-constant sizes used by the post-process pass
    pushconst.sourceSize[0] = (float)vidinfo->w;
    pushconst.sourceSize[1] = (float)vidinfo->h;
    pushconst.sourceSize[2] = 1.0f / (float)vidinfo->w;
    pushconst.sourceSize[3] = 1.0f / (float)vidinfo->h;
    pushconst.targetSize[0] = dimensions.rw;
    pushconst.targetSize[1] = dimensions.rh;
    pushconst.targetSize[2] = 1.0f / dimensions.rw;
    pushconst.targetSize[3] = 1.0f / dimensions.rh;

    // Settings for CRTea
    int masktype = 0, maskstr = 0, scanstr = 0, sharpness = 0,
        curve = settings[VIDEO_CRTEA_CURVE].val,
        corner = settings[VIDEO_CRTEA_CORNER].val,
        tcurve = settings[VIDEO_CRTEA_TCURVE].val;
    switch (settings[VIDEO_CRTEA_MODE].val) {
        default: case 0: masktype = 0; maskstr = 0; scanstr = 6;
            sharpness = 3; break;
        case 1: masktype = 1; maskstr = 5; scanstr = 6; sharpness = 7; break;
        case 2: masktype = 2; maskstr = 5; scanstr = 6; sharpness = 7; break;
        case 3: masktype = 3; maskstr = 4; scanstr = 6; sharpness = 3; break;
        case 4: masktype = settings[VIDEO_CRTEA_MASKTYPE].val;
            maskstr = settings[VIDEO_CRTEA_MASKSTR].val;
            scanstr = settings[VIDEO_CRTEA_SCANSTR].val;
            sharpness = settings[VIDEO_CRTEA_SHARPNESS].val; break;
    }
    pushconst.masktype = masktype;
    pushconst.maskstr = maskstr / 10.0f;
    pushconst.scanstr = scanstr / 10.0f;
    pushconst.sharpness = (float)sharpness;
    pushconst.curve = curve / 100.0f;
    pushconst.corner = corner ? (float)corner : -3.0f;
    pushconst.tcurve = tcurve / 10.0f;
}

// Upload the visible region of the game framebuffer into the source image
static void vk_upload_source(VkCommandBuffer cb) {
    uint32_t rw = (uint32_t)(vidinfo->w + vidinfo->x);
    uint32_t rh = (uint32_t)(vidinfo->h + vidinfo->y);
    size_t dstpitch = (size_t)rw * pixfmt.size;
    size_t srcpitch = (size_t)vidinfo->p * pixfmt.size;

    uint8_t *src = (uint8_t*)vidinfo->buf;
    uint8_t *dst = (uint8_t*)stage_source.mapped;
    for (uint32_t r = 0; r < rh; ++r)
        memcpy(dst + r * dstpitch, src + r * srcpitch, dstpitch);

    vk_transition(cb, &tex_source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

    VkBufferImageCopy region = { 0 };
    region.bufferRowLength = rw; // tightly packed
    region.bufferImageHeight = rh;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = rw;
    region.imageExtent.height = rh;
    region.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb, stage_source.buffer, tex_source.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    vk_transition(cb, &tex_source,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
}

// Compute the OSD destination UVs and scale
static void vk_osd_compute_rects(float *u0, float *v0, float *u1, float *v1,
    int *vis_w_out, int *vis_h_out) {
    float rw = dimensions.rw;
    float rh = dimensions.rh;

    int scale_x = ((int)rw + 128) / 256;
    int scale_y = ((int)rh + 112) / 224;
    int scale = (scale_x < scale_y) ? scale_x : scale_y;
    if (scale < 1)
        scale = 1;

    int vw = ((int)rw + scale - 1) / scale;
    if (vw > JGRF_OSD_W)
        vw = JGRF_OSD_W;

    int vh = ((int)rh + scale - 1) / scale;
    if (vh > JGRF_OSD_H)
        vh = JGRF_OSD_H;

    *u0 = 0.0f;
    *v0 = 0.0f;
    *u1 = rw / ((float)scale * (float)JGRF_OSD_W);
    *v1 = rh / ((float)scale * (float)JGRF_OSD_H);
    *vis_w_out = vw;
    *vis_h_out = vh;
}

// Prepare the OSD for drawing; upload canvas if dirty. Returns 1 if active.
static int vk_osd_prepare(VkCommandBuffer cb) {
    int rw = (int)dimensions.rw;
    int rh = (int)dimensions.rh;
    if (rw < 1 || rh < 1)
        return 0;

    int vw, vh;
    float u0, v0, u1, v1;
    vk_osd_compute_rects(&u0, &v0, &u1, &v1, &vw, &vh);

    jgrf_osd_set_visible(vw, vh);
    jgrf_osd_step();

    int active = 0, dirty = 0;
    const void *pixels = jgrf_osd_composite(&active, &dirty);
    if (!active) return 0;

    if (dirty && pixels) {
        memcpy(stage_osd.mapped, pixels,
            (size_t)JGRF_OSD_W * JGRF_OSD_H * 4);

        vk_transition(cb, &tex_osd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

        VkBufferImageCopy region = { 0 };
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = JGRF_OSD_W;
        region.imageExtent.height = JGRF_OSD_H;
        region.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(cb, stage_osd.buffer, tex_osd.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        vk_transition(cb, &tex_osd,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    }

    // OSD quad: fills letterboxed area, UVs select the canvas (top-origin)
    float verts[] = {
        -1.0f, -1.0f, u0, v0,
        -1.0f,  1.0f, u0, v1,
         1.0f, -1.0f, u1, v0,
         1.0f,  1.0f, u1, v1,
    };
    memcpy(vbo_osd.mapped, verts, sizeof(verts));
    return 1;
}

// Set a dynamic viewport + scissor on the command buffer
static void vk_set_viewport(VkCommandBuffer cb, float x, float y,
    float w, float h) {
    VkViewport vp = { x, y, w, h, 0.0f, 1.0f };
    VkRect2D sc = { { (int32_t)x, (int32_t)y }, { (uint32_t)w, (uint32_t)h } };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
}

// Render the scene
void jgrf_video_vk_render(int render) {
    vkWaitForFences(device, 1, &fence_frame, VK_TRUE, UINT64_MAX);

    VkResult acq = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
        sem_acquire, VK_NULL_HANDLE, &swap_index);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        jgrf_video_vk_recreate_swapchain();
        return;
    }

    vkResetFences(device, 1, &fence_frame);

    jgrf_video_vk_refresh();

    VkCommandBuffer cb = cmdbuf;
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi =
        { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cb, &bi);

    // Per-frame uploads happen outside any render pass
    if (render)
        vk_upload_source(cb);
    int osd_active = vk_osd_prepare(cb);

    VkDeviceSize voff = 0;
    VkClearValue clear = { { { 0.0f, 0.0f, 0.0f, 1.0f } } };

    // Pass 0: source -> offscreen (clip), viewport is the full offscreen area
    VkRenderPassBeginInfo rp0 =
        { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp0.renderPass = rp_offscreen;
    rp0.framebuffer = fbo_offscreen;
    rp0.renderArea.extent.width = vidinfo->w;
    rp0.renderArea.extent.height = vidinfo->h;
    rp0.clearValueCount = 1;
    rp0.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rp0, VK_SUBPASS_CONTENTS_INLINE);
    vk_set_viewport(cb, 0, 0, (float)vidinfo->w, (float)vidinfo->h);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_pass0);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, playout,
        0, 1, &ds_source, 0, NULL);
    vkCmdBindVertexBuffers(cb, 0, 1, &vbo_pass0.buffer, &voff);
    vkCmdDraw(cb, 4, 1, 0, 0);
    vkCmdEndRenderPass(cb);

    // Pass 1 + OSD: offscreen -> swapchain, viewport is the letterboxed area
    VkRenderPassBeginInfo rp1 =
        { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp1.renderPass = rp_screen;
    rp1.framebuffer = swap_fbos[swap_index];
    rp1.renderArea.extent = swap_extent;
    rp1.clearValueCount = 1;
    rp1.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rp1, VK_SUBPASS_CONTENTS_INLINE);

    vk_set_viewport(cb, dimensions.xo, dimensions.yo,
        dimensions.rw, dimensions.rh);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_pass1);
    vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
        sizeof(pushconst), &pushconst);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, playout,
        0, 1, &ds_offscreen, 0, NULL);
    vkCmdBindVertexBuffers(cb, 0, 1, &vbo_pass1.buffer, &voff);
    vkCmdDraw(cb, 4, 1, 0, 0);

    if (osd_active) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_osd);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, playout,
            0, 1, &ds_osd, 0, NULL);
        vkCmdBindVertexBuffers(cb, 0, 1, &vbo_osd.buffer, &voff);
        vkCmdDraw(cb, 4, 1, 0, 0);
    }

    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);

    VkPipelineStageFlags wait =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &sem_acquire;
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &sem_render;
    vk_check(vkQueueSubmit(q_graphics, 1, &si, fence_frame),
        "vkQueueSubmit");
}

// Present the rendered image
void jgrf_video_vk_swapbuffers(void) {
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &sem_render;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &swap_index;
    VkResult r = vkQueuePresentKHR(q_present, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        jgrf_video_vk_recreate_swapchain();
}

// Recreate the swapchain and its framebuffers after a resize / out-of-date
static void jgrf_video_vk_recreate_swapchain(void) {
    vkDeviceWaitIdle(device);
    vk_swapchain_destroy();
    vk_swapchain_create();
    SDL_GetWindowSizeInPixels(window, &win_pw, &win_ph);
    jgrf_video_vk_resize();
}

// Handle viewport resizing
void jgrf_video_vk_resize(void) {
    // Recreate the swapchain when the window pixel size changes
    int pw, ph;
    SDL_GetWindowSizeInPixels(window, &pw, &ph);

    if (swapchain != VK_NULL_HANDLE && win_pw &&
        (pw != win_pw || ph != win_ph)) {
        vkDeviceWaitIdle(device);
        vk_swapchain_destroy();
        vk_swapchain_create(); // refreshes swap_extent and swap_fbos
    }
    win_pw = pw;
    win_ph = ph;

    // Letterbox to the swapchain extent
    dimensions.ww = (int)swap_extent.width;
    dimensions.wh = (int)swap_extent.height;
    dimensions.rw = dimensions.ww;
    dimensions.rh = dimensions.wh;

    if (dimensions.rh * vidinfo->aspect > dimensions.rw)
        dimensions.rh = dimensions.rw / vidinfo->aspect + 0.5;
    else if (dimensions.rw / vidinfo->aspect > dimensions.rh)
        dimensions.rw = dimensions.rh * vidinfo->aspect + 0.5;

    dimensions.xo = (dimensions.ww - dimensions.rw) / 2;
    dimensions.yo = (dimensions.wh - dimensions.rh) / 2;

    pushconst.targetSize[0] = dimensions.rw;
    pushconst.targetSize[1] = dimensions.rh;
    pushconst.targetSize[2] = 1.0f / dimensions.rw;
    pushconst.targetSize[3] = 1.0f / dimensions.rh;

    SDL_DisplayID displayid = SDL_GetDisplayForWindow(window);
    const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(displayid);
    if (dm)
        jgrf_set_screenfps((int)(dm->refresh_rate + 0.5));
}

// Retrieve scale parameters for pointing device input
void jgrf_video_vk_get_scale_params(float *xscale, float *yscale,
    float *xo, float *yo) {
    *xscale = dimensions.rw /
        (vidinfo->aspect * vidinfo->h) / dimensions.dpiscale;
    *yscale = dimensions.rh / vidinfo->h / dimensions.dpiscale;
    *xo = dimensions.xo / dimensions.dpiscale;
    *yo = dimensions.yo / dimensions.dpiscale;
}

// Retrieve video information
jg_videoinfo_t* jgrf_video_vk_get_info(void) {
    return vidinfo;
}

// Set the cursor
void jgrf_video_vk_set_cursor(int ctype) {
    cursor = SDL_CreateSystemCursor(ctype);
    SDL_ShowCursor();
    SDL_SetCursor(cursor);
}

// Pass the core's video info pointer into the frontend
void jgrf_video_vk_set_info(jg_videoinfo_t *ptr) {
    vidinfo = ptr;
    jgrf_video_vk_refresh_pixfmt();
}

// Dump the pixels rendered to the swapchain for a screenshot
void *jgrf_video_vk_get_pixels(int *rw, int *rh) {
    // Clear OSD content before grabbing
    for (int i = 0; i < JGRF_OSD_NUM_TEXT_SLOTS; ++i)
        jgrf_osd_text_clear(i);
    jgrf_osd_shade_set(0);
    jgrf_osd_fx_clear();

    // Render one clean frame and present it
    jgrf_video_vk_render(0);
    jgrf_video_vk_swapbuffers();
    vkWaitForFences(device, 1, &fence_frame, VK_TRUE, UINT64_MAX);

    int w = (int)dimensions.rw;
    int h = (int)dimensions.rh;
    int xo = (int)dimensions.xo;
    int yo = (int)dimensions.yo;

    VkDeviceSize rbsize = (VkDeviceSize)w * h * 4;
    if (readback.buffer && readback.size < rbsize)
        vk_buffer_destroy(&readback);
    if (!readback.buffer)
        vk_buffer_create(&readback, rbsize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VkImage img = swap_images[swap_index];
    VkCommandBuffer cb = vk_cmd_begin();

    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);

    VkBufferImageCopy region = { 0 };
    region.bufferRowLength = w;
    region.bufferImageHeight = h;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageOffset.x = xo;
    region.imageOffset.y = yo;
    region.imageExtent.width = w;
    region.imageExtent.height = h;
    region.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        readback.buffer, 1, &region);

    // Restore present layout
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vk_cmd_end(cb);

    uint8_t *pixels = (uint8_t*)calloc((size_t)w * h, sizeof(uint32_t));
    memcpy(pixels, readback.mapped, rbsize);

    // Swap BGRA to RGBA if needed for the screenshot encoder
    if (swap_format == VK_FORMAT_B8G8R8A8_UNORM ||
        swap_format == VK_FORMAT_B8G8R8A8_SRGB) {
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            uint8_t t = pixels[i * 4 + 0];
            pixels[i * 4 + 0] = pixels[i * 4 + 2];
            pixels[i * 4 + 2] = t;
        }
    }

    *rw = w;
    *rh = h;
    return pixels;
}

// Set a text message to be output for a number of frames
void jgrf_video_vk_text(int index, int frames, const char *msg) {
    int anchor;
    int x = 0, y = 0;
    int f = frames;

    switch (index) {
        case JGRF_OSD_TEXT_FRONTEND:
            anchor = JGRF_OSD_ANCHOR_BL; x = 0; y = 0; break;
        case JGRF_OSD_TEXT_CORE:
            anchor = JGRF_OSD_ANCHOR_BR; x = 0; y = 0; break;
        case JGRF_OSD_TEXT_MENU:
            anchor = JGRF_OSD_ANCHOR_TL; x = 8; y = 8;
            f = (frames > 0) ? -1 : 0;
            break;
        case JGRF_OSD_TEXT_AUX:
        default:
            anchor = JGRF_OSD_ANCHOR_TR; x = 0; y = 0; break;
    }

    if (f == 0 || !msg || msg[0] == '\0') {
        jgrf_osd_text_clear(index);
        return;
    }
    jgrf_osd_text(index, f, anchor, x, y, 0, msg);
}

// Rebuild pipelines and re-letterbox after a settings change
void jgrf_video_vk_rehash(void) {
    vkDeviceWaitIdle(device);

    // Rebuild pipelines in case the post-process shader changed
    vk_pipelines_destroy();
    vk_pipelines_create();

    SDL_SetWindowFullscreen(window, settings[VIDEO_FULLSCREEN].val ?
        true : false);
    jgrf_video_vk_resize();

    jgrf_osd_rehash();
}
