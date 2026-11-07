/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   In-game menu on the native Vulkan renderer: Copyright 2026 Zorkats, same licence. */
/* The in-game menu (beyond_ui/ov.cpp) under renderer=vulkan_native: each present, the renderer (snxvk gpu/vk/ui.c,
   vk_menu_draw) asks for the menu's picture and draws it over the frame with its overlay pipeline. Plain C, so
   both sides can include it. */
#ifndef OV_VK_H
#define OV_VK_H
#ifdef __cplusplus
extern "C" {
#endif

enum { OV_VK_TEX_CREATE, OV_VK_TEX_UPDATE, OV_VK_TEX_DESTROY };
typedef struct {
    unsigned id;               /* nonzero: what the draws name */
    int op;                    /* OV_VK_TEX_*; an update has the size the texture was created with */
    int w, h;
    const unsigned char* rgba; /* w*h texels, R G B A bytes, alpha straight (create and update) */
} OvVkTex;

typedef struct {
    float clip[4];   /* x0 y0 x1 y1, in the display's units */
    unsigned tex;
    unsigned first;  /* the first index */
    unsigned count;  /* indices */
    int base;        /* added to each index */
} OvVkCmd;

typedef struct {
    float w, h;                               /* the display: the vertices' units, spanning the whole image */
    const void* vtx; unsigned nvtx;           /* 20 bytes each: x y u v as floats, then R G B A bytes */
    const unsigned short* idx; unsigned nidx;
    const OvVkCmd* cmd; unsigned ncmd;
    const OvVkTex* tex; unsigned ntex;        /* done before the draws, in order */
} OvVkDraw;

/* One menu frame: 0 while the menu is closed (nothing to do). What d points to stays valid until the next call.
   Called on the renderer's GPU thread, once per present. */
int ov_vk_frame(OvVkDraw* d);

#ifdef __cplusplus
}
#endif
#endif
