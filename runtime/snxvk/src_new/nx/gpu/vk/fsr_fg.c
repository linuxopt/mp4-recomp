/* SwitchRecomp local addition (2026-11-06, Zorkats), part of the vendored ScarletNX renderer build (GPL-3.0-or-later).
   AMD FSR 3.1 frame generation through the FidelityFX API, loaded at run time from amd_fidelityfx_loader_vk.dll in the
   game folder (the launcher downloads AMD's signed DLLs; none ship with the game). Only the MIT-licensed API headers
   are compiled in (deps/FidelityFX-SDK-2.4.0).

   The swapchain is AMD's: its frame generation swapchain context makes the real one and hands back a stand-in handle,
   which every acquire, present, image query and destroy goes through (vk_sc_*). At each present it shows the frame the
   game drew and one generated between it and the last, from the depth and motion vectors the DLSS code already makes
   (dlss.c, the scene's prepass drawn again with last frame's positions). The HUD is part of the presented image, so it
   is generated with the scene (no HUD-less image here). Generation is off while the overlay menu is open.

   The swapchain's own queues: AMD's sample gives it a present queue and an image-acquire queue of their own, apart
   from the game's; the game's graphics queue is only submitted to inside the present, which holds its lock
   (record.c present_run). With anything failing (no DLL, no spare queues, a context that won't create) the run goes
   on with the plain swapchain and says why in the log. */
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdlib.h>
#include <string.h>

/* the FidelityFX headers are written for C++ where a struct's tag is its type name */
typedef struct FfxApiResource FfxApiResource;
typedef struct FfxApiResourceDescription FfxApiResourceDescription;
typedef struct VkQueueInfoFFXAPI VkQueueInfoFFXAPI;
#include "api/include/ffx_api.h"
#include "api/include/vk/ffx_api_vk.h"
#include "framegeneration/include/ffx_framegeneration.h"
#include "framegeneration/include/vk/ffx_api_framegeneration_vk.h"

extern char g_exedir[1024];
extern int snxvk_fsr_fg; /* the INI's fsr_frame_generation, from runtime/snxvk_glue.c */
extern int ov_is_open(void); /* the runtime's overlay menu (beyond_ui) */
extern volatile unsigned long long g_fg_generated_frames; /* the runtime's FPS counter (main.c) adds these */
int vk_dlss_depth_inverted(void);

static struct {
    int tried, wanted;  /* asked for, the loader loaded, queues to come */
    HMODULE dll;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxConfigure configure;
    PfnFfxQuery query;
    PfnFfxDispatch dispatch;
    VkQueue present_q, acquire_q;
    int failed;         /* a context failed: the plain swapchain for the rest of the run */
    /* the frame generation swapchain */
    ffxContext sc_ctx;
    VkSwapchainKHR sc;
    struct ffxQueryDescSwapchainReplacementFunctionsVK fn;
    /* frame generation itself, made at the first scene */
    ffxContext fg_ctx;
    u32 fg_w, fg_h, fg_dw, fg_dh;
    int fg_inverted;
    int legacy_prepare; /* the provider takes only the first prepare description (FSR 3.1.4 and 3.1.5) */
    u64 frame;          /* the frame id, one more each present */
    int configured;     /* this frame's configure was made (at its scene) */
    u64 last_ns;
    u64 generated;
} g_fg;

/* AMD's format helpers (ffx_api_vk.h) are C++ only: the formats this renderer hands it (swapchains, depth, the motion
   vectors), mapped the same way */
static uint32_t ffx_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R16G16B16A16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R8G8B8A8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB: return FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM: return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
    case VK_FORMAT_R16G16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_X8_D24_UNORM_PACK32: return FFX_API_SURFACE_FORMAT_R32_UINT;
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D16_UNORM_S8_UINT: return FFX_API_SURFACE_FORMAT_R16_UNORM;
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}
static int has_stencil(VkFormat f) {
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
}

static const char* rc_str(ffxReturnCode_t r) {
    switch (r) {
    case FFX_API_RETURN_OK: return "ok";
    case FFX_API_RETURN_ERROR: return "error";
    case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "unknown description type";
    case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "runtime error";
    case FFX_API_RETURN_NO_PROVIDER: return "no provider";
    case FFX_API_RETURN_ERROR_MEMORY: return "out of memory";
    case FFX_API_RETURN_ERROR_PARAMETER: return "bad parameter";
    case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE: return "provider too old for a description";
    default: return "?";
    }
}

/* before the device: fsr_frame_generation=1, the loader loads, and no HDR output (its swapchain isn't handled here) */
int vk_fsr_fg_wanted(void);
int vk_fsr_fg_wanted(void) {
    if (g_fg.tried) return g_fg.wanted;
    g_fg.tried = 1;
    if (!snxvk_fsr_fg) return 0;
    if (vk_hdr_wanted()) {
        LOG("fsr: frame generation is off with hdr=1 (not supported with HDR output yet)\n");
        return 0;
    }
    char path[1100];
    snprintf(path, sizeof path, "%samd_fidelityfx_loader_vk.dll", g_exedir);
    /* the loader finds the frame generation DLL beside itself */
    g_fg.dll = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_fg.dll) {
        DWORD e = GetLastError();
        LOG("fsr: could not load %s (error %lu%s): frame generation off\n", path, e,
            e == ERROR_MOD_NOT_FOUND ? ", the file or the Microsoft Visual C++ runtime it needs is missing" : "");
        return 0;
    }
    g_fg.create = (PfnFfxCreateContext)(void*)GetProcAddress(g_fg.dll, "ffxCreateContext");
    g_fg.destroy = (PfnFfxDestroyContext)(void*)GetProcAddress(g_fg.dll, "ffxDestroyContext");
    g_fg.configure = (PfnFfxConfigure)(void*)GetProcAddress(g_fg.dll, "ffxConfigure");
    g_fg.query = (PfnFfxQuery)(void*)GetProcAddress(g_fg.dll, "ffxQuery");
    g_fg.dispatch = (PfnFfxDispatch)(void*)GetProcAddress(g_fg.dll, "ffxDispatch");
    if (!g_fg.create || !g_fg.destroy || !g_fg.configure || !g_fg.query || !g_fg.dispatch) {
        LOG("fsr: %s lacks the FidelityFX API entry points: frame generation off\n", path);
        return 0;
    }
    LOG("fsr: FidelityFX loader %s\n", path);
    g_fg.wanted = 1;
    return 1;
}

/* device.c: the graphics family's queues it asks for beyond its own, and then what it got */
u32 vk_fsr_fg_queues(void);
u32 vk_fsr_fg_queues(void) { return vk_fsr_fg_wanted() ? 2 : 0; }
void vk_fsr_fg_device(VkQueue present, VkQueue acquire);
void vk_fsr_fg_device(VkQueue present, VkQueue acquire) {
    if (!g_fg.wanted) return;
    if (!present || !acquire) {
        LOG("fsr: the GPU's graphics queue family has no two spare queues for the frame generation swapchain: "
            "frame generation off\n");
        g_fg.wanted = 0;
        return;
    }
    g_fg.present_q = present;
    g_fg.acquire_q = acquire;
}

/* frame generation is running: AMD's swapchain is the one and nothing has failed (dlss.c then makes the depth and
   motion vectors each frame) */
int vk_fsr_fg_active(void);
int vk_fsr_fg_active(void) { return g_fg.sc_ctx != NULL && !g_fg.failed; }
/* the next swapchain is to be AMD's */
int vk_fsr_fg_ready(void);
int vk_fsr_fg_ready(void) { return g_fg.wanted && !g_fg.failed && g_fg.present_q; }

/* the compute queue as the swapchain's async compute queue: the game submits to it from other threads. Unused while
   async workloads are off, as they are here */
static VkResult compute_submit(uint32_t n, const VkSubmitInfo* s, VkFence f) {
    vk_queue_lock(g_vk.compute);
    VkResult r = vkQueueSubmit(g_vk.compute, n, s, f);
    vk_queue_unlock(g_vk.compute);
    return r;
}

static void fg_destroy(void) {
    if (!g_fg.fg_ctx) return;
    g_fg.destroy(&g_fg.fg_ctx, NULL);
    g_fg.fg_ctx = NULL;
}

/* device.c vk_swapchain_create, sc_lock held, any old swapchain already gone: AMD's swapchain in place of
   vkCreateSwapchainKHR */
VkResult vk_fsr_fg_swapchain(const VkSwapchainCreateInfoKHR* ci, VkSwapchainKHR* out);
VkResult vk_fsr_fg_swapchain(const VkSwapchainCreateInfoKHR* ci, VkSwapchainKHR* out) {
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    struct ffxCreateContextDescFrameGenerationSwapChainVersionVK ver = {
        .header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_VK},
        .version = FFX_FRAMEGENERATION_SWAPCHAIN_VK_VERSION};
    struct ffxCreateContextDescFrameGenerationSwapChainVK d = {
        .header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK},
        .physicalDevice = g_vk.phys,
        .device = g_vk.dev,
        .swapchain = &sc,
        .allocator = NULL,
        .createInfo = *ci,
        .gameQueue = {g_vk.gfx, g_vk.gfx_family, NULL}, /* only submitted to inside the present, under its lock */
        .asyncComputeQueue = {g_vk.compute, g_vk.compute_family, compute_submit},
        .presentQueue = {g_fg.present_q, g_vk.gfx_family, NULL},
        .imageAcquireQueue = {g_fg.acquire_q, g_vk.gfx_family, NULL}};
    d.createInfo.oldSwapchain = VK_NULL_HANDLE;
    d.header.pNext = &ver.header;
    ffxReturnCode_t r = g_fg.create(&g_fg.sc_ctx, &d.header, NULL);
    if (r != FFX_API_RETURN_OK) { /* an older provider may not know the version description */
        d.header.pNext = NULL;
        sc = VK_NULL_HANDLE;
        g_fg.sc_ctx = NULL;
        r = g_fg.create(&g_fg.sc_ctx, &d.header, NULL);
    }
    if (r != FFX_API_RETURN_OK || !sc) {
        LOG("fsr: the frame generation swapchain could not be made (%s, %u): frame generation off\n", rc_str(r),
            (unsigned)r);
        if (g_fg.sc_ctx) g_fg.destroy(&g_fg.sc_ctx, NULL);
        g_fg.sc_ctx = NULL;
        g_fg.failed = 1;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    memset(&g_fg.fn, 0, sizeof g_fg.fn);
    g_fg.fn.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
    r = g_fg.query(&g_fg.sc_ctx, &g_fg.fn.header);
    if (r != FFX_API_RETURN_OK || !g_fg.fn.pOutAcquireNextImageKHR || !g_fg.fn.pOutQueuePresentKHR ||
        !g_fg.fn.pOutGetSwapchainImagesKHR || !g_fg.fn.pOutDestroySwapchainFFXAPI) {
        LOG("fsr: the frame generation swapchain gave no functions (%s): frame generation off\n", rc_str(r));
        g_fg.destroy(&g_fg.sc_ctx, NULL);
        g_fg.sc_ctx = NULL;
        g_fg.failed = 1;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct ffxQueryGetProviderVersion pv = {.header = {.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION}};
    g_fg.query(&g_fg.sc_ctx, &pv.header);
    LOG("fsr: frame generation swapchain %ux%u (provider %s)\n", ci->imageExtent.width, ci->imageExtent.height,
        pv.versionName ? pv.versionName : "?");
    g_fg.sc = sc;
    g_fg.configured = 0;
    *out = sc;
    return VK_SUCCESS;
}

/* the FidelityFX swapchain's presents all made: before the device waits idle or the swapchain goes */
void vk_fsr_fg_wait_presents(void);
void vk_fsr_fg_wait_presents(void) {
    if (!g_fg.sc_ctx) return;
    struct ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK w = {
        .header = {.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK}};
    g_fg.dispatch(&g_fg.sc_ctx, &w.header);
}

/* ---- the swapchain calls, AMD's while its swapchain is the one */
VkResult vk_sc_images(VkSwapchainKHR sc, u32* n, VkImage* img);
VkResult vk_sc_images(VkSwapchainKHR sc, u32* n, VkImage* img) {
    if (g_fg.sc_ctx && sc == g_fg.sc) return g_fg.fn.pOutGetSwapchainImagesKHR(g_vk.dev, sc, n, img);
    return vkGetSwapchainImagesKHR(g_vk.dev, sc, n, img);
}
VkResult vk_sc_acquire(VkSwapchainKHR sc, u64 timeout, VkSemaphore sem, u32* idx);
VkResult vk_sc_acquire(VkSwapchainKHR sc, u64 timeout, VkSemaphore sem, u32* idx) {
    if (g_fg.sc_ctx && sc == g_fg.sc)
        /* its images come back once its present thread is done with them, a part of a frame later: a short wait
           rather than a dropped frame */
        return g_fg.fn.pOutAcquireNextImageKHR(g_vk.dev, sc, timeout > 20000000ull ? timeout : 20000000ull, sem,
                                               VK_NULL_HANDLE, idx);
    return vkAcquireNextImageKHR(g_vk.dev, sc, timeout, sem, VK_NULL_HANDLE, idx);
}
/* the graphics queue's lock held (record.c present_run) */
static void configure(int enabled);
VkResult vk_sc_present(VkQueue q, const VkPresentInfoKHR* pi);
VkResult vk_sc_present(VkQueue q, const VkPresentInfoKHR* pi) {
    if (!(g_fg.sc_ctx && pi->swapchainCount == 1 && pi->pSwapchains[0] == g_fg.sc)) return vkQueuePresentKHR(q, pi);
    if (g_fg.fg_ctx && !g_fg.configured) configure(0); /* no scene this frame: shown as drawn */
    VkResult r = g_fg.fn.pOutQueuePresentKHR(q, pi);
    g_fg.frame++;
    g_fg.configured = 0;
    return r;
}
void vk_sc_destroy(VkSwapchainKHR sc);
void vk_sc_destroy(VkSwapchainKHR sc) {
    if (!(g_fg.sc_ctx && sc == g_fg.sc)) {
        vkDestroySwapchainKHR(g_vk.dev, sc, NULL);
        return;
    }
    vk_fsr_fg_wait_presents();
    fg_destroy(); /* its configuration names the swapchain */
    g_fg.destroy(&g_fg.sc_ctx, NULL);
    g_fg.sc_ctx = NULL;
    /* after the context, as the header says */
    g_fg.fn.pOutDestroySwapchainFFXAPI(g_vk.dev, sc, NULL, NULL);
    g_fg.sc = VK_NULL_HANDLE;
}

/* The swapchain is an sRGB format, so a view of its format decodes the sRGB curve as it reads, and AMD's shaders then
   decode it again from the backbuffer's transfer function: generated frames came out far darker than the real ones, a
   flicker over the whole screen. AMD's own helpers take an sRGB backbuffer to be a UNORM image with sRGB views
   (ffx_api_vk.h, the mutable-format rule), so the backbuffer is described as that, to the context and in each
   dispatch: read raw, decoded once. */
static uint32_t gamma_raw(uint32_t f) {
    if (f == FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB) return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    if (f == FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB) return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    return f;
}

/* ---- each frame */
static ffxReturnCode_t generate(ffxDispatchDescFrameGeneration* params, void* user) {
    g_fg.generated++;
    g_fg_generated_frames++;
    params->presentColor.description.format = gamma_raw(params->presentColor.description.format);
    return g_fg.dispatch((ffxContext*)user, &params->header);
}

static bool only_generated(void) { /* SWR_FSR_FG_ONLY_GENERATED=1: only the generated frames shown, to look at them */
    static int o = -1;
    if (o < 0) o = getenv("SWR_FSR_FG_ONLY_GENERATED") != NULL;
    return o != 0;
}

static void configure(int enabled) {
    struct ffxConfigureDescFrameGeneration c = {
        .header = {.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION},
        .swapChain = g_fg.sc,
        .presentCallback = NULL,
        .presentCallbackUserContext = NULL,
        .frameGenerationCallback = generate,
        .frameGenerationCallbackUserContext = &g_fg.fg_ctx,
        .frameGenerationEnabled = enabled,
        .allowAsyncWorkloads = false,
        .HUDLessColor = {0},
        .flags = 0,
        .onlyPresentGenerated = only_generated(),
        .generationRect = {0, 0, (int)g_fg.fg_dw, (int)g_fg.fg_dh},
        .frameID = g_fg.frame};
    ffxReturnCode_t r = g_fg.configure(&g_fg.fg_ctx, &c.header);
    if (r != FFX_API_RETURN_OK) LOG_LIMIT(8, "fsr: frame generation configure failed (%s)\n", rc_str(r));
    g_fg.configured = 1;
}

static int fg_create(u32 w, u32 h) {
    u32 dw = g_vk.sc_extent.width, dh = g_vk.sc_extent.height;
    int inverted = vk_dlss_depth_inverted();
    if (g_fg.fg_ctx && g_fg.fg_w >= w && g_fg.fg_h >= h && g_fg.fg_dw == dw && g_fg.fg_dh == dh &&
        g_fg.fg_inverted == inverted)
        return 1;
    if (g_fg.fg_ctx) {
        vk_finish();
        vk_fsr_fg_wait_presents();
        fg_destroy();
    }
    struct ffxCreateContextDescFrameGenerationVersion ver = {
        .header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION},
        .version = FFX_FRAMEGENERATION_VERSION};
    struct ffxCreateBackendVKDesc be = {.header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK},
                                        .deviceContext = {g_vk.dev, g_vk.phys, vkGetDeviceProcAddr}};
    struct ffxCreateContextDescFrameGeneration d = {
        .header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION},
        .flags = inverted ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED : 0,
        .displaySize = {dw, dh},
        .maxRenderSize = {w, h},
        .backBufferFormat = gamma_raw(ffx_format(g_vk.sc_format))};
    d.header.pNext = &be.header;
    be.header.pNext = &ver.header;
    ffxReturnCode_t r = g_fg.create(&g_fg.fg_ctx, &d.header, NULL);
    if (r != FFX_API_RETURN_OK) { /* an older provider may not know the version description */
        be.header.pNext = NULL;
        g_fg.fg_ctx = NULL;
        r = g_fg.create(&g_fg.fg_ctx, &d.header, NULL);
    }
    if (r != FFX_API_RETURN_OK) {
        LOG("fsr: the frame generation context could not be made (%s, %u): frame generation off\n", rc_str(r),
            (unsigned)r);
        g_fg.fg_ctx = NULL;
        g_fg.failed = 1;
        return 0;
    }
    struct ffxQueryGetProviderVersion pv = {.header = {.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION}};
    g_fg.query(&g_fg.fg_ctx, &pv.header);
    LOG("fsr: frame generation on: render %ux%u, display %ux%u, depth %s (provider %s)\n", w, h, dw, dh,
        inverted ? "inverted" : "not inverted", pv.versionName ? pv.versionName : "?");
    g_fg.fg_w = w;
    g_fg.fg_h = h;
    g_fg.fg_dw = dw;
    g_fg.fg_dh = dh;
    g_fg.fg_inverted = inverted;
    return 1;
}

static struct FfxApiResource image_res(VkImage img, VkFormat f, u32 w, u32 h, int depth) {
    struct FfxApiResource r = {0};
    r.resource = (void*)img;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = ffx_format(f);
    r.description.width = w;
    r.description.height = h;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    r.description.usage = depth ? FFX_API_RESOURCE_USAGE_DEPTHTARGET : FFX_API_RESOURCE_USAGE_READ_ONLY;
    if (depth && has_stencil(f)) r.description.usage |= FFX_API_RESOURCE_USAGE_STENCILTARGET;
    r.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS; /* VK_IMAGE_LAYOUT_GENERAL, where dlss.c leaves both */
    return r;
}

/* dlss.c, the scene's motion vectors made (in GENERAL, render pixels, current to previous) with its depth (in
   GENERAL): this frame's configure and prepare, recorded into the frame's commands ahead of the present */
void vk_fsr_fg_scene(Surf* z, VkImage mv, u32 w, u32 h, u32 rw, u32 rh, float jx, float jy, int reset);
/* w x h: the depth and motion vector images; rw x rh: their top-left part the scene was drawn to */
void vk_fsr_fg_scene(Surf* z, VkImage mv, u32 w, u32 h, u32 rw, u32 rh, float jx, float jy, int reset) {
    if (!vk_fsr_fg_active() || !fg_create(w, h)) return;
    VkCommandBuffer cb = vk_cmd(); /* after fg_create, whose vk_finish ends the command buffer it had */
    u64 now = plat_ns();
    float dt = g_fg.last_ns ? (float)((double)(now - g_fg.last_ns) / 1e6) : 16.67f;
    g_fg.last_ns = now;
    configure(!ov_is_open());
    static float js = 0;
    if (!js) js = getenv("SWR_FSR_FG_JSIGN") ? (float)atof(getenv("SWR_FSR_FG_JSIGN")) : 1.0f;
    struct FfxApiResource rz = image_res(z->img, z->f.vk, w, h, 1), rm = image_res(mv, VK_FORMAT_R16G16_SFLOAT, w, h, 0);
    ffxReturnCode_t r = FFX_API_RETURN_ERROR;
    if (!g_fg.legacy_prepare) {
        struct ffxDispatchDescFrameGenerationPrepareV2 p = {
            .header = {.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2},
            .frameID = g_fg.frame,
            .flags = 0,
            .commandList = cb,
            .renderSize = {rw, rh},
            .jitterOffset = {js * jx, js * jy},
            .motionVectorScale = {1.0f, 1.0f}, /* render pixels */
            .frameTimeDelta = dt,
            .reset = reset != 0,
            /* the game's camera isn't known here: plausible values */
            .cameraNear = 0.1f,
            .cameraFar = 1000.0f,
            .cameraFovAngleVertical = 1.0f,
            .viewSpaceToMetersFactor = 1.0f,
            .depth = rz,
            .motionVectors = rm,
            .cameraPosition = {0, 0, 0},
            .cameraUp = {0, 1, 0},
            .cameraRight = {1, 0, 0},
            .cameraForward = {0, 0, -1}};
        r = g_fg.dispatch(&g_fg.fg_ctx, &p.header);
        if (r != FFX_API_RETURN_OK) {
            LOG("fsr: the provider takes no PrepareV2 (%s): the older prepare from now on\n", rc_str(r));
            g_fg.legacy_prepare = 1;
        }
    }
    if (g_fg.legacy_prepare) {
        struct ffxDispatchDescFrameGenerationPrepareCameraInfo ci = {
            .header = {.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO},
            .cameraPosition = {0, 0, 0},
            .cameraUp = {0, 1, 0},
            .cameraRight = {1, 0, 0},
            .cameraForward = {0, 0, -1}};
        struct ffxDispatchDescFrameGenerationPrepare p = {
            .header = {.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE, .pNext = &ci.header},
            .frameID = g_fg.frame,
            .flags = 0,
            .commandList = cb,
            .renderSize = {rw, rh},
            .jitterOffset = {js * jx, js * jy},
            .motionVectorScale = {1.0f, 1.0f},
            .frameTimeDelta = dt,
            .unused_reset = reset != 0,
            .cameraNear = 0.1f,
            .cameraFar = 1000.0f,
            .cameraFovAngleVertical = 1.0f,
            .viewSpaceToMetersFactor = 1.0f,
            .depth = rz,
            .motionVectors = rm};
        r = g_fg.dispatch(&g_fg.fg_ctx, &p.header);
        if (r != FFX_API_RETURN_OK) LOG_LIMIT(8, "fsr: frame generation prepare failed (%s)\n", rc_str(r));
    }
    static u64 n;
    if (++n == 1 || n % 600 == 0)
        LOG("fsr: %llu frames prepared, %llu generated\n", (unsigned long long)n, (unsigned long long)g_fg.generated);
}
