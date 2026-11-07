/* SwitchRecomp: the renderer interface as plain functions, for runtime/snxvk_glue.c. The Renderer struct changes with
   every ScarletNX update; the runtime calls these (renamed snxv_swr_vk_*) instead of mirroring its layout.
   Our runtime paces the presents itself (snxvk_present), so present_buffer gets no release time, no game period and no
   syncpoint to increment once the frame shows: the display clock (display.c) is left to its defaults. */
#include "nx/gpu/renderer.h"

int swr_vk_start(void) { return vk_renderer.start(); }
void swr_vk_present(u64 cpu_addr, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4],
                    u32 transform) {
    vk_renderer.present_buffer(cpu_addr, w, h, pitch, layout, bhl, fmt, crop, transform, 0, 0, 0);
}
u64 swr_vk_screenshot(const char* path) { return vk_renderer.screenshot ? vk_renderer.screenshot(path) : 0; }
void swr_vk_save(int at_exit) {
    if (vk_renderer.save) vk_renderer.save(at_exit);
}
