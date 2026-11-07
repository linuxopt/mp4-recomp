/* Builds without the front end (Linux, or SWR_NO_FRONTEND / no runtime/imgui/libimgui.a): the hooks the backend and
   the GL queue call do nothing, so the game starts directly and F1 does nothing. */
#include "ov_vk.h"
int fe_should_show(int argc, char** argv, const char* exedir, const char* exename){ (void)argc; (void)argv; (void)exedir; (void)exename; return 0; }
int fe_run(void* hwnd){ (void)hwnd; return 0; }
int fe_wndproc(void* h, unsigned m, unsigned long long w, long long l, long long* r){ (void)h; (void)m; (void)w; (void)l; (void)r; return 0; }
int ov_wndproc(void* h, unsigned m, unsigned long long w, long long l){ (void)h; (void)m; (void)w; (void)l; return 0; }
int ov_is_open(void){ return 0; }
int ov_pad(unsigned long long phys, int lx, int ly){ (void)phys; (void)lx; (void)ly; return 0; }
void ov_gl_frame(void){}
int ov_vk_frame(OvVkDraw* d){ (void)d; return 0; }   /* the native Vulkan renderer draws no menu */
