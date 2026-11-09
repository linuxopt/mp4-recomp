# Changelog

Fixes and changes per release. Found a problem? Please use the [bug report form](../../issues/new?template=bug_report.yml)
and attach `Metroid Prime 4 Beyond.log`: it holds the timings and errors needed to trace most issues, and one report per
problem keeps each one easy to track.

Releases are tagged `vYYYY.MM.DD` and built by [.github/workflows/release.yml](.github/workflows/release.yml) when the
tag is pushed. Each one publishes the player zip, `update-<version>.zip` and `update.txt`, which the Play screen's
updater reads from `releases/latest`.

## v2026.11.09

First Windows release from this fork. The game itself has been played on Linux; the Windows build compiles from the same
code and reaches the setup and Play screens, so please report what you find.

### Windows build

- `build.bat` builds Dear ImGui, the Native Vulkan renderer, the runtime and a release folder in one go, with the pinned
  toolchain that `windows/get_tools.ps1` downloads and checks by SHA-256 (winlibs GCC 14.2.0, Python 3.12, glslang 16.6.0,
  SDL2, zlib).
- Setup accepts `CC` for a build from source, and shows the Windows key folder (`%USERPROFILE%\.switch`).
- The setup screen shows the free space on the install drive beside the 30 GB it needs.

### Keyboard and mouse

- A shared input layer (`runtime/input_kbm.c`) used by the Windows and X11 backends, with PrimeHack-style defaults and
  every button rebindable in the in-game menu's CONTROLS page (Shift, Ctrl or Alt held with a key make a chord).
- **True mouse aim in first person** (`mouse_aim.py`): the game's own look update is overridden at translation time, so
  the mouse turns the camera by an angle instead of pushing the right stick at a speed. The overrides check the build ID
  and each instruction word, and other versions of the game are left alone.
- Tests: `linux/input_kbm_test.sh`.

### Game logic at any frame rate

- The game advanced its logic by one fixed step per presented frame, so above 60 FPS it ran faster than real time and
  every dropped frame slowed it down. The 78 places that read the frame-rate mode flag are rewritten at translation time
  (`logic_rate.py`, 84 instructions, each checked against its instruction word), and `swr_logic_k` carries the step from
  the measured frame time. `fps_limit` 120, 144, 240 and unlimited now keep real-time speed.

### Native Vulkan

- ScarletNX updated to `3f15d931`, with Vulkan-Headers 1.4.367, xxHash 0.8.4 and zstd 1.5.7 as pinned upstream, and the
  glue calling the renderer through plain functions instead of mirroring struct layouts.
- Small guest buffer ranges get their own buffer in one batch at the frame's end, instead of ending the rendering pass for
  a copy and a barrier at each draw.
- The F1 menu works under `renderer=vulkan_native`.
- New `[Graphics]` switches: `vk_fast_writeback`, `vk_async_pipelines`, `vk_memory_traps` (default on), and
  `vk_dynamic_offsets` (default on). The log names any switch that is off its default.
- **AMD FSR 3.1 frame generation** (`fsr_frame_generation`, Windows): one generated frame between each pair the game
  renders, through FidelityFX's replacement swapchain. The AMD DLLs are downloaded by the launcher from AMD's FidelityFX
  SDK release, checked by size and SHA-256, and are not part of the release.
- **NVIDIA DLSS / DLAA**: the DLL is downloaded by the launcher from NVIDIA's DLSS repository, checked the same way.

### Launcher

- Themes as data (beyond, amber, high contrast, or a `theme.ini` next to the EXE) with a live contrast check, and a
  reduce-motion option.
- **Download NVIDIA DLSS** and **Download AMD FSR frame generation** buttons on the Graphics page.
- AUTO-SET picks a Low, Medium, High or Ultra profile from the graphics card, its video memory, the CPU and the RAM, and
  offers to fit the settings again when an install is opened on other hardware. Unlike the base project it is free to pick
  120, 144 or 240 FPS, because game speed no longer follows the frame rate.

### Rendering, ported from upstream linuxopt/mp4-recomp

- Compressed 3D textures: decode and upload every slice, which had made the Ice Belt lake fog about four times too thick.
- Fog pass: support the G24R8 stencil view (format 0x0E).
- A unique trailing comment per shader source, to avoid the Mesa 26.2.6 link crash.
- The invisible Control Beam: the 2D engine's copy of the scene depth is used as the depth target instead of an empty
  render target made for its address.
- Shader translator: Sol Valley's missing, holed and black ground.

### Cheats

- 17 built-in cheats for build `8275FBE1EC5191C2`, toggles on the Gameplay page and in the in-game menu, plus Green
  Energy 10x. Atmosphere-format cheat files still work.

### Depth readbacks, ported from PlagueDrRiess/salsburysteaks

- A depth target a DMA copy reads comes back only for the part the copy needs, instead of the whole target every frame.
- The scan visor's reticle depth is read straight from the scaled depth-stencil. The scaled-down blit before the read took
  600-770 ms a copy at `resolution=2` (1.5 FPS while scanning); the read alone is a few ms.
- `SWR_DEPTH_READBACK_WHOLE=1` restores the whole-target readback.

## Next release

Nothing yet.
