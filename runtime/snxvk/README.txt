Native Vulkan renderer (ini renderer=vulkan_native), taken from ScarletNX
(https://github.com/scarletnx/ScarletNX, main branch commit 3f15d931fb4b, fetched 2026-11-05), itself a fork of
SwitchRecomp. The sources are in src_new/: the files the gpu, shader and vk components build from, with the headers
they include and the code generators (src/ in a private checkout is the earlier 2026-10-18 copy, not used).

LICENSE: the code in this folder is GPL-3.0-or-later (LICENSE; MZ Labs' original parts Apache-2.0, LICENSE.apache,
NOTICE). A runtime EXE built with libsnxvk_new.a linked in is therefore a GPL-3.0 work: fine for personal use; anyone
given such an EXE must also be offered this source. A build with SWR_NO_SNXVK=1 leaves it out.

deps/: Vulkan-Headers 1.4.367, volk 1.4.363, VulkanMemoryAllocator 3.4.0, xxHash 0.8.4 and zstd 1.5.7 (its lib/
sources only), the versions and SHA-256 hashes ScarletNX's cmake/dependencies.cmake pins (verified).
src_new/tools/: upstream's shaderdec and settingsgen code generators (same project, same licence).

Left out of the build: the NVN driver path (gpu/nvn, off by default upstream), FFmpeg NVDEC (vk/nvdec.c; our host1x
decodes video) and the SDL window (common/gpu/vk/window.c). vk/uma.c is built but kept off: swr_settings.c sets
vk_memory=1 (guest buffers copied), as its in-place import needs memory hooks our runtime doesn't have.

Local changes to upstream (marked "SWR" in the code):
  gpu/gpu.c             plat_prof_mark_gpu (our profiler samples the renderer's GPU thread); the save state layout
                        check (gpu_state_schema) only under clang, whose __builtin_dump_struct GCC lacks
  gpu/vk/buffer.c       own buffers for small ranges only once their data stayed the same across two presents
                        (own_ready), made at the frame's end in one batch (vk_buf_promote) instead of ending the
                        rendering pass for a copy and a barrier at each draw that found one ready; big ranges with write
                        tracking still take theirs at once. Their counts in the buffer stats (profile=1)
  gpu/vk/cmd.c          vk_buf_promote before a frame's last submission when it ends without a present
  gpu/vk/hdr.c          HDR output, FXAA, sharpening, the FPS counter and the skip prompt in one present pass, recorded
                        through the recording thread's ring; vk_hdr_mid_target (the SDR image the in-game menu draws on
                        with HDR output)
  gpu/vk/renderer.c     the present pass and the in-game menu's draw at each present; a stats copy that read past
                        its source array (memcpy of the longer one's size)
  gpu/vk/device.c       VK_EXT_swapchain_colorspace asked for with hdr=1, the HDR swapchain format (scRGB FP16, else
                        HDR10 PQ), the present pass's views dropped with the swapchain
                        On Windows the QueryPerformanceCounter time domain counts as the host clock, so gpuprof.c's
                        GPU idle report (profile=1) works there too
  gpu/vk/gpuprof.c      that report timed on QueryPerformanceCounter on Windows (plat_ns's clock)
  gpu/vk/writeback.c    vk_fast_writeback: a storage result's 1 KB chunk the CPU left alone since the draw (guest memory
                        still equals the pre-draw copy) lands in one copy instead of the byte-by-byte merge; the merge
                        held the frame's syncpoint back (57 -> 85+ FPS on the benchmark route). Unchanged result
  gpu/vk/surface.c      flush_land: a storage image's result lands over its pre-draw bytes in one pass (land_layout:
  gpu/texture/blocklinear.c  bl_land's base takes the padding and partial GOBs from them) instead of a whole copy of
                        those bytes first and the result over it; the same bytes land (checked against the old landing
                        on 200,000 random shapes). Overlapping rows or layers keep the old copy first
  gpu/vk/surface.c      vk_surf_snap_put/take: a draw storage image's landed bytes kept as its next draw's bytes from
  gpu/vk/image_out.c    before it, where guest memory still holds exactly them (compared after the CPU write tracking
  gpu/vk/render.c       and GPU-side baselines, checked again at the bind), instead of reading guest memory into a new
                        buffer at each bind
  gpu/vk/buffer.c       vk_ww_landed_as: the pages of such a landing noted as landed, so its own writes, which the next
                        epoch's scan reports, don't count as the CPU's while their bytes are unchanged (upstream notes
                        a landing only through the alias pool's mirror, which our runtime lacks)
  memory/own_win.c      GPU-owned guest memory for Windows ([Graphics] vk_memory_traps, default 1), in place of
                        upstream's own.c, which reads and lands through the alias pool's mirror our runtime lacks.
                        Owned pages are PAGE_NOACCESS; a CPU access to one faults into a vectored handler that lands
                        what the GPU holds of it: the GPU copy is read into host buffers first, then every other thread
                        is suspended for the copy into the opened pages (whole pages only, so no other bytes on them can
                        change meanwhile). own_read_open reads a range only when none of its pages is trapped
  memory/own.h          its additions (own_read_open, own_in_fault, own_stop_stats, g_swr_memory_traps)
  gpu/vk/owned.c        the landing's pages noted as landed from guest memory itself (landed_live; upstream reads them
                        through the mirror); vk_own_res_ref and vk_own_cpu_touched from upstream commit 504dc04f
  gpu/vk/image_out.c,   upstream commit 504dc04f (draw storage images kept on the GPU, guest memory owned by the image
  gpu/vk/surface.c,     instead of a copy of its bytes from before each draw), with traps on only; render.c merges it
  gpu/vk/render.c,      with vk_surf_snap_take above
  gpu/vk/vk.h
  gpu/vk/buffer.c       landed_hash: the write tracking's check of a landed page skips pages trapped since
  gpu/vk/pipeline.c     vk_dynamic_offsets: the busiest constant and storage buffer bindings (by measured offset churn,
  gpu/vk/render.c       within the device's dynamic buffer limits) are dynamic in the vertex and fragment set layouts, so
  gpu/vk/vk.h           a draw whose buffers stay the same and only move reuses its cached set and passes the offsets at
                        the bind; compute keeps its own plain layout. Off with descriptor buffers
  gpu/vk/ui.c           vk_menu_draw: our in-game menu (runtime/ov_vk.h, ov_vk_frame) through the overlay pipeline
                        (pipeline_new split out of pipeline_make, one pipeline per target format)
  gpu/vk/vk.h           the declarations of the above
  gpu/vk/swr_glue.c     the renderer interface as plain functions for runtime/snxvk_glue.c, so it mirrors no struct
  gpu/vk/swr_settings.c our .ini's settings (profile=1 also turns on vk_gpu_profile and vk_buffer_stats), and the
                        [Graphics] vk_* switches: vk_fast_writeback (default 1), vk_async_pipelines (default 0),
                        vk_memory_traps (default 1), vk_dynamic_offsets (default 1)
  gpu/vk/dlss.c, dlss_stub.c: DLSS. Its hooks are in render.c, texture.c, device.c, renderer.c and spirv_translate.c.
                        The public build links dlss.c too: DLSS ships, and the front end downloads NVIDIA's
                        nvngx_dlss.dll at a pinned version (the release carries no NVIDIA file). dlss_stub.c is only
                        for a build that leaves DLSS out (SWR_NO_DLSS=1)

Build: set GLSLANG=<glslang.exe 16.6.0> and run python\python.exe work-profiler\build_snxvk2.py
(SWR_NO_DLSS=1: runtime\snxvk\libsnxvk_new_nodlss.a, for a build that leaves DLSS out), then the normal
--build-runtime links it and compiles runtime\snxvk_glue.c (the adapter between this renderer and our runtime).
Every global symbol of the ScarletNX objects is renamed with the prefix snxv_ so its GPU front end can sit next to ours.
