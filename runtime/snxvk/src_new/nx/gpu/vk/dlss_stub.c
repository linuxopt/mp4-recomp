/* Build without NVIDIA DLSS (SWR_NO_DLSS=1, work-profiler/build_snxvk2.py): the same entry points as dlss.c and
   fsr_fg.c, as no-ops, for a build that leaves DLSS and FSR frame generation out. The public build links both.
   `dlss=` and `fsr_frame_generation=` in the ini have no effect here. */
#include <stddef.h>
#include "nx/gpu/vk/vk.h"

int vk_dlss_wanted(void) { return 0; }
void vk_dlss_instance_exts(const char** ext, u32* next, u32 max) { (void)ext; (void)next; (void)max; }
void vk_dlss_device_exts(void* pd, const char** ext, u32* next, u32 max) { (void)pd; (void)ext; (void)next; (void)max; }
int vk_dlss_init(void) { return 0; }
void vk_dlss_draw(void* rt0, void* zt, int depth_test, int depth_cmp) { (void)rt0; (void)zt; (void)depth_test; (void)depth_cmp; }
void vk_dlss_pass(void** rt, u32 n) { (void)rt; (void)n; }
void vk_dlss_frame(void) {}
int vk_dlss_xfb_wanted(void) { return 0; }
const void* vk_dlss_device_chain(const void* next) { return next; }
void vk_dlss_draw_begin(void) {}
void vk_dlss_rt_read(void) {}
void vk_dlss_jitter(void* zt, int depth_write, int depth_equal, void* vp) { (void)zt; (void)depth_write; (void)depth_equal; (void)vp; }
int vk_dlss_capture_begin(void* cb, void* zt, int depth_write, int plain_tris, u64 key, u32 verts, const void* vp, const void* vpj) {
    (void)cb; (void)zt; (void)depth_write; (void)plain_tris; (void)key; (void)verts; (void)vp; (void)vpj; return 0; }
void vk_dlss_capture_end(void* cb) { (void)cb; }
void vk_dlss_before_read(u64 gva) { (void)gva; }

/* FSR frame generation (fsr_fg.c): never on; the swapchain calls are the plain ones */
u32 vk_fsr_fg_queues(void) { return 0; }
void vk_fsr_fg_device(VkQueue present, VkQueue acquire) { (void)present; (void)acquire; }
int vk_fsr_fg_ready(void) { return 0; }
VkResult vk_fsr_fg_swapchain(const VkSwapchainCreateInfoKHR* ci, VkSwapchainKHR* out) {
    (void)ci; (void)out; return VK_ERROR_FEATURE_NOT_PRESENT; }
void vk_fsr_fg_wait_presents(void) {}
VkResult vk_sc_images(VkSwapchainKHR sc, u32* n, VkImage* img) { return vkGetSwapchainImagesKHR(g_vk.dev, sc, n, img); }
VkResult vk_sc_acquire(VkSwapchainKHR sc, u64 timeout, VkSemaphore sem, u32* idx) {
    return vkAcquireNextImageKHR(g_vk.dev, sc, timeout, sem, VK_NULL_HANDLE, idx); }
VkResult vk_sc_present(VkQueue q, const VkPresentInfoKHR* pi) { return vkQueuePresentKHR(q, pi); }
void vk_sc_destroy(VkSwapchainKHR sc) { vkDestroySwapchainKHR(g_vk.dev, sc, NULL); }
