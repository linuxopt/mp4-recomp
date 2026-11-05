# Metroid Prime 4 Recomp — developer notes (Linux first)

> **Developer notes.** Players: see [README.md](README.md) for installing and playing, and for the list of fixes.
> Metroid Prime 4 Recomp targets Metroid Prime 4: Beyond (Switch 1 build, program `010019A01E2F2000`, main build
> `8275FBE1EC5191C279F3A943AD8524F4BAB087A3`, NintendoSDK 19.3.15). The whole game translates and compiles to a native
> Linux library and plays: boot, menus, gameplay, the scan visor, audio and videos work. The **Status** section below is
> the bring-up log from the first sessions; later rendering, stutter and audio fixes are listed in the README.

One AppImage that sets up, configures and runs a native PC build of **your own** copy of the game. It is the
SwitchRecomp runtime (by MZ Labs) with the front end built in: first-run setup that compiles the game with a bundled
GCC, a Play / settings screen, and an in-game menu (F1 or the Home / Guide button).

**No Nintendo files are in this repository**: no game code, no RomFS, no keys, no generated C.
`beyond/romfs_beyond.manifest` lists RomFS file names, sizes and SHA-1 hashes only, so setup can verify the player's files.

## What this fork adds

| feature | where | state |
|---|---|---|
| Prime 4 setup: title / build ID checks, RomFS layout and manifest | `beyond/beyond_setup.py`, `beyond/romfs_beyond.manifest` | works (all 445 files, 26.9 GB, verified) |
| `fsp-srv` 205 (`OpenDataStorageWithProgramIndex`, newer SDKs) | `runtime/svc_fs.c` | works: the game mounts its RomFS |
| HID: player 1 reports one controller style (Full Key), like a real Pro Controller | `runtime/svc_hid.c` | works: the six-axis abort is gone |
| Audio renderer REV15 (0x188-byte voices with float biquad filters) | `runtime/main.c` `audren_update` | works: the renderer updates, audio runs |
| Audio at real speed for games that update the renderer once a frame: each update mixes as many 5 ms blocks as real time needs; a 20 ms cushion when the output runs empty | `runtime/main.c`, `runtime/audio.c` | works: 200 blocks a second and 0 underruns (was 60 a second, 1949 of 2000 blocks finding the output empty) |
| Play screen for Prime 4: the installed-data check looks for Prime 4's RomFS, titles and texts renamed | `runtime/beyond_ui` | works: READY, and Play starts the game |
| H.264 videos on Linux: the system's OpenH264 2.x (`libopenh264.so`) is loaded at the first video when the runtime is built without the static decoder; one decoder per NVDEC channel; decoder addresses (iova) kept for a buffer's lifetime | `runtime/host1x.c`, `runtime/nvdrv.c`, `switchrecomp.py` | works: the intro video plays (960 of 960 jobs decoded in 30 s) |
| Smooth video: VIC pictures converted on up to 8 threads with the AVX2 converter and 64-byte tiled copies on by default (both checked byte for byte against the plain code) | `runtime/host1x.c`, `runtime/main.c` | works: 9 ms → 0.4 ms per 1080p picture; late video pictures 113–138 → 11–16 of 1200; 58 consecutive moving frames with none repeated or torn |
| Correct video pictures: each decoded H.264 picture is filed under the surface NVDEC wrote it to, so VIC converts the one the game asks for | `runtime/host1x.c` | works: the title logo no longer flashes during the first seconds of the intro |
| FFmpeg H.264 decoding (the system's `libavcodec`, loaded at run time; OpenH264 when it is missing) | `runtime/host1x.c`, `switchrecomp.py` | works: 4.1 ms vs 8.5 ms a picture on the game's videos; title-screen video 5–10 ms instead of 12–19 ms |
| Hardware video decoding (VA-API through FFmpeg, the primary GPU's decoder), software FFmpeg or OpenH264 when unavailable | `runtime/host1x.c` | works: 3–6 ms a picture without the CPU (software: about 20 ms for a keyframe) |
| H.264 pictures filed under their real surface, `PictureLumaOffset[CurrPicIdx]` (bits 2–6 of word 0xb4 of the picture setup), and looked up only by it | `runtime/host1x.c` | works: 1530 of 1530 intro pictures shown once and in order (before: steps of 0, 1, 2, a repeat and a skip every third frame); no title logo, no static |
| Timing logs on by default (present, monitor, video, audio; `SWR_NO_VIDEO_TIMING=1` turns them off); the last five logs are kept | `runtime/*.c`, `runtime/plat.c` | works |
| **Gameplay at 60 FPS**: draws into one vertex buffer share one cached region (keyed on the buffer's end) instead of uploading its whole tail per draw | `runtime/gpu_gl.c` | works: gameplay 34–40 → 60 FPS; vertex uploads 50–60 MB → 4–5 MB a frame |
| Global memory (`LDG`/`STG`) through pointers in any constant buffer, not only NVN's SSBO table | `runtime/shader.c`, `runtime/gpu_gl.c` | translated: 49 shader stages; untranslated shaders 61 → 24 (`VMNMX`, `ATOM`, `SUST` left) |
| VSync on by default on Linux | `runtime/main.c`, menu | works: frames that reach the monitor wrong 59 → 1–2 of 3600 |
| **Shaders built before the game uses them**: Prime 4 unpacks its shaders into GPU memory at boot (they are compressed in `MaterialArchive.arc`); a scanner thread finds them, the GL thread translates a few per frame, the compile threads build them and keep them ready. On by default (`[Graphics] prebuild_shaders`) | `runtime/gpu_gl.c` | works: 601 of 662 programs ready 11 s into the intro on a first run, before the game draws with them; later starts load them from the cache |
| Shader translator: `AL2P` and indexed attribute loads (200 shaders), carry-in adds declaring the flags (a vertex shader that failed to compile, so its object flickered) | `runtime/shader.c` | works: all 599 translated stages compile in glslang; no skipped draws |
| **Faster shader builds on Linux**: several compile threads, each with its own shared OpenGL context (Mesa and NVIDIA compile GLSL on the calling thread); they also run in the default "wait for shaders" mode now, rebuilding the shader pack at start; bigger driver shader caches that are not cleaned up | `runtime/backend_x11.c`, `runtime/gpu_gl.c` | works: 6 threads on a 24-thread CPU; `[Performance] shader_threads` (0 = auto) |
| **Cheats** in the Atmosphere / dmnt format (the `.txt` files Atmosphere, Ryujinx and Yuzu use), on/off per cheat in the in-game menu, reloaded when the file changes | `runtime/cheats.c`, menu: **Cheats** page | built and unit-tested (`bash linux/cheats_test.sh`) |
| **FOV slider** (percent of the game's own FOV, live) | `runtime/cheats.c` FOV hook, `beyond/fov_hooks.txt` | mechanism built and tested; the camera address for Prime 4 still has to be found |
| **AMD FidelityFX FSR 1** upscaling (EASU + RCAS) with a sharpness slider | `runtime/gpu_gl.c`, `third_party/fsr1` (MIT) | works: runs in game on radeonsi; on a fixed test picture EASU + RCAS give smooth edges where bilinear shows steps (mean gradient 10.1 bilinear, 11.3 FSR) |
| Project naming: Metroid Prime 4 Recomp, `~/.local/share/MetroidPrime4Recomp`, `MP4_RECOMP_HOME` (an older `MetroidBeyondRecomp` install is moved on first start) | everywhere | done |

### Cheats

Put a cheat file at `<game folder>/Metroid Prime 4 Beyond_data/cheats/8275FBE1EC5191C2.txt` (the first 16 hex digits of
the build ID, as on Atmosphere). `[Name]` starts a cheat, `{Name}` is a master code that always runs. Turn cheats on in
the in-game menu (F1 → **CHEATS**); the choice is kept in `8275FBE1EC5191C2.enabled` next to the file.
Supported opcodes: 0, 1, 2 (end/else), 3, 4, 5, 6, 7, 8 (keypress), 9, A, C0–C3. Cheats that change game data work.
Cheats that patch ARM instructions do nothing, because the code is recompiled to native code ahead of time.

### FOV

`[Gameplay] fov` (60–160 %) scales the camera's field of view through a pointer chain listed per game build in
`beyond/fov_hooks.txt` (format explained in the file). A test chain can go in `cheats/<BUILD ID>.fov` without
rebuilding. Until the Prime 4 chain is found the slider has no effect, and the menu says so.

### FSR

Graphics → **Upscaling: AMD FSR 1**, with **Internal resolution** below 1 (for example 0.67 or 0.75). Used whenever the
picture is enlarged to the window; FXAA and the CAS sharpening are skipped then (RCAS sharpens). OpenGL renderer.

### Shaders on Linux

- Shader compile threads: `[Performance] shader_threads=0` picks a quarter of your CPU threads (2 to 8). Upstream, the
  default "wait for shaders" mode (`[Launcher] wait_for_shaders=1`) started no compile thread at all, and radeonsi (AMD)
  reports no `GL_KHR_parallel_shader_compile`, so on AMD Linux nothing was precompiled at start and every new shader
  compiled on the GPU thread. Now draws still wait for their new shaders (no missing objects), while the threads
  compile the shader pack (everything seen before without a program binary for this driver) in the background.
- Driver caches: `MESA_SHADER_CACHE_MAX_SIZE=8G`, `__GL_SHADER_DISK_CACHE_SIZE=8 GB`, `__GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1`,
  only when not already set in the environment.
- Unchanged from upstream and still in use: the translated-GLSL cache, the program-binary cache and the shader pack
  that recompiles everything you have seen in the background at start.

## Status (bring-up log)

1. Translation: 153,372 functions, 7.6 M instructions → 253 C files (436 MB) → `Metroid Prime 4 Beyond_code.so`
   (208 MB) in about 2 minutes on 22 threads. No translator changes were needed.
2. Start: modules load, OpenGL 4.6 starts (tested on radeonsi, RX 9070 XT), services connect.
3. Fixed: `fsp-srv` command 205 returned a stub, so the game read through a null storage object while mounting its
   RomFS.
4. Fixed: `nn::hid::GetSixAxisSensorStates` aborted (`UnexpectedDefault`). Player 1 was reported as Full Key **and**
   Joy-Con Dual at once; Prime 4 picked Joy-Con Dual, a style it had requested no six-axis handles
   for, and passed an all-zero handle (style index 0 - 3). Player 1 now reports one style, chosen from the game's
   `SetSupportedNpadStyleSet`.
5. Fixed: the audio renderer rejected the game's updates ("malformed update layout"): REV15 voices are 0x188 bytes.
   They are converted to the 0x170 layout the mixer reads.
6. Fixed: the garbled first screen was the intro video, not a texture (textures decode correctly). Three causes:
   - the Linux build had no H.264 decoder (upstream ships OpenH264 for Windows only), so the video surfaces were never
     written and showed uninitialized memory. The runtime now loads the system's OpenH264 2.x;
   - the decoder's buffer addresses were freed when the game unmapped them after each job, and the game keeps using them
     (on the console an address stays valid for the buffer's lifetime): video stopped after three frames;
   - all NVDEC channels shared one decoder; Prime 4 opens two, and the mixed streams came out gray.
7. Fixed: micro-stutter in the intro video. Each 1080p picture took 5-10 ms to decode plus about 9 ms to convert
   (YUV to RGBA and tiling, one thread, the plain per-pixel code), so about one picture in ten came after its 16.7 ms
   and the previous one stayed on screen. Conversion is now 0.4 ms; the remaining late pictures (about 1 in 100) are
   OpenH264 decode spikes on keyframes. `SWR_VIDEO_TIMING=1` logs decode, convert, picture and present timing.
8. Fixed: the title logo showed during the first seconds of the intro. The game decodes the first title-screen frames
   on a second channel while the intro plays, and H.264 pictures were never filed by surface, so the intro's VIC jobs
   converted the last picture of any channel.
9. Fixed: the remaining stutter in the intro and title screen. OpenH264 needed 12-19 ms a picture on the title
   screen's video (60 FPS allows 16.7): FFmpeg's decoder is used when present. Without vsync the 60.00 FPS limiter
   beat against the 59.95 Hz display (a dropped and a doubled frame about once a second): vsync is now on by default
   on Linux. Measured over 60 s (intro and title screen): late video pictures 131 → 59 of 3360 (keyframes that still
   take about 20 ms to decode), frames reaching the monitor wrong 59 → 1 of 3600. One pause of about 0.2 s remains
   where the intro video hands over to the title screen's.
10. Fixed: choppy sound, which made the whole intro feel like it stuttered. The runtime mixed 5 ms of audio per
    renderer update; Prime 4 updates 60 times a second, so it got 0.3 s of sound per second and the output ran empty
    nearly every block. Each update now mixes what real time needs. `SWR_VIDEO_TIMING=1` also logs the update rate and
    underruns, and the Play screen passes it on to the game.
11. Fixed: the Play screen said "game files are missing" after a successful setup (it checked for RomFS
    folders this game does not have) and showed the wrong title.
12. Fixed: black flashes. One vertex shader read the carry flag without the translator declaring it, so it never
    compiled and the object using it was skipped on most frames (2629 skipped draws in 84 s).
13. Shaders before play: the translator still lacks `LDG`/`STG` (global memory), `VMNMX`, `ATOM` and `SUST`; the 61
    shaders that use them compile at their first draw. `SWR_SHADER_SCAN=5,20` logs what is in GPU memory, and the
    log's "not translated" line counts what is missing.
14. Fixed: the intro's judder, the title logo flashing in it and, after a first attempt, static in its place. Every
    decoded picture was filed under slot 3 of NVDEC's 17-surface table, while the game rotates its pictures through
    four surfaces and decodes one ahead; lookups also matched the display register, which names the previous picture.
    VIC therefore converted the wrong pictures: the same one twice, then one skipped, and early on the other video's.
    The output slot is CurrPicIdx in the picture setup. `SWR_TRACE_NVDEC_REGS=n` writes the surface registers and
    which picture each VIC job converts to nvdec_regs.txt.
15. Fixed: gameplay ran at 30-40 FPS: every draw re-uploaded the tail of the vertex buffer it drew from (50-60 MB a
    frame). Now 60 FPS in the first area (`SWR_TRACE_VB=frame` lists one frame's uploads).
16. Found: the false colours in gameplay (green/orange gradients over the scene, the neon arm cannon) came from an
    88x51x64 RGBA16F froxel volume (volumetric fog and light). The game fills it each frame with a compute shader that
    writes through image stores (`SUST`), which the translator does not have, so the volume had no data and the
    runtime substituted an identity colour table: materials read coordinates as light. A non-cubic empty volume now
    reads as (0,0,0,1), no fog. Ruled out with frame traces (`SWR_TRACE=frame`): ASTC decoding (GPU and CPU decode are
    identical), texture swizzles, the global buffers (cb0[0x510] is the pixel stage's NVN SSBO table entry).
17. Added: image stores (`SUST`/`SULD`) in vertex and pixel shaders, not only compute. The game fills its froxel volume
    from a vertex shader drawing one point per froxel column (88x51 = 4488 points), with formatted 3D image stores;
    the runtime binds the image at the draw (vertex units 0-3, pixel 4-7), marks the texture GPU-written and puts a
    barrier before the next draw. The volume is now computed by the game every frame.
18. Fixed: the arm cannon was one flat colour that changed from run to run. The first draw of every frame is a
    vertex shader that builds the light list in a global buffer (STG); each later draw got a fresh copy of that buffer
    from guest memory, which never sees GPU writes, so every lit material read stale light data. Buffers written by
    vertex/pixel shaders now stay on the GPU between draws (refilled from guest memory only when the CPU changes them),
    and reads inside their range bind the GPU copy. The cannon is lit and shaded, the same in every run.
19. Fixed: broken sound after a menu transition. The game calls RequestUpdate about 20000 times a second for half a
    second there; one 5 ms block per call made about 50 s of sound for a 160 ms output queue (11735 blocks dropped,
    the sound out of step afterwards). Calls less than 5 ms apart now only take the game's changes and report status;
    audio advances with real time only. 0 dropped blocks through the same transition.
20. Partly fixed: large black shapes when looking up near the start. Reproduced by aiming up from the first checkpoint
    (keyboard I/J/K/L, F5 for a frame trace). Most of them were NaN pixels spreading through the blur and downsample
    passes into hard-edged boxes: pixel shader outputs now have NaN replaced by 0 (`SWR_NO_NAN_GUARD=1` turns it off).
    Black share of the view looking up and right: 51% -> 12%. Ruled out: the BC6H lightmap array layout, FMZ multiplies.
21. Fixed: black rock faces and mountains. The rock materials pick their UV set and tangent frame with indexed
    attribute reads (`IPA.IDX`, the register holds the whole address); the translator returned 0 for those, the
    normals became 0 and the lighting NaN. Also fixed on the way: `texelFetch` from render targets ignored the
    resolution scale (the fog pass read the wrong part of the depth buffer: a big black rock in front of the cannon),
    texture lookups now take the render target drawn to last when several share an address (dynamic resolution),
    flow instructions honour their condition-code test and `ISET.CC` sets the flags (the froxel fog writer skipped its
    last slice: black sky), and FMNMX / saturate follow Maxwell's NaN rules. Debug aid: `SWR_DBG_FS=hash@anchor@expr`
    shows a pixel shader's intermediate value on screen.
22. Fixed: 100-300 ms hitches when turning in the first area. The game waits for GPU fences, and the GPU thread was
    held up by ASTC textures streamed in as the camera turned: this GPU has no ASTC, the driver (or our decoder) decoded
    each one synchronously, 20-90 ms apiece. ASTC now decodes on background threads with its mipmaps; a texture shows
    from its small levels at once and gets the large ones a frame or two later (`SWR_ASTC_SYNC=1` / `SWR_ASTC_DRIVER=1`
    for the old behaviour). Small render-target readbacks (the auto-exposure value) are asynchronous too. Measured:
    1316 of 1320 frames clean while looking around, a single repeated frame every few seconds left (the GPU just over one
    refresh). Diagnostics: `SWR_STALL_DUMP=1` (thread states when frames stop), slow GPU methods / GL calls in the log.
19. Now: the loading icon, the intro video, the publisher logo, the title screen and the first area run; the game runs without stopping
   (40 s tested). **Next**: play past the boot sequence (title screen, input, saves, the first area) and fix what
   comes up there, plus game-specific patches (`beyond/beyond_patches.json`). The first one makes Hard selectable from
   the start: the front end's logic graph lights the Hard button from a bridge node (`F_8686740`, `code_055.c`) that
   sends one bool per key, and the patch makes it send true for key `0xF19B0CAF`, the unlock flag. That key feeds only
   the Hard chain in the front end; whether gameplay reads it too is not checked yet.

Test loop used during bring-up: build the runtime without the Play screen (`SWR_NO_LINUX_FRONTEND=1`), copy it next to
the compiled game and read `Metroid Prime 4 Beyond.log`.

## Build and play on Linux

Native Linux (X11, or Wayland through XWayland; OpenGL 4.3). You need your own Metroid Prime 4: Beyond, extracted to an
ExeFS (`main`, `main.npdm`, `rtld`, `sdk`, `subsdk0`) and a RomFS folder (`Worlds/`, `UI/`, `Preload/`, `Kernel.pak`, ...).

```
bash linux/install_deps.sh
bash linux/setup_and_build_runtime.sh
bash linux/setup_game.sh GAME_FOLDER [UPDATE_FOLDER]
bash linux/run.sh
```

### AppImage

`bash linux/make_appimage.sh` builds `linux/dist/Metroid_Prime_4_Recomp-x86_64.AppImage`: the runtime with the Play /
setup screens, the recompiler, a relocatable GCC 14.3 and Python 3.12, so nothing needs to be installed and no game
files are inside. The game is installed in `~/.local/share/MetroidPrime4Recomp` (or `MP4_RECOMP_HOME`).
It needs glibc 2.38 or newer, SDL2 for controllers, an OpenGL 4.3 driver and, for the game's videos, OpenH264 2.x
(`openh264` / `libopenh264` package; without it videos stay black). With FFmpeg installed (the `libavcodec` the
AppImage was built against) the faster FFmpeg decoder is used instead. `bash linux/build_openh264.sh` builds OpenH264 2.4.1
from source into the runtime instead (static, no system library needed); it has not been run in this fork yet.

### Windows

The Windows build (`build.bat`) is renamed for this project, but has not been built or tested in this fork.

## Credits

- **yozander2009-stack** and contributors: the recompiled runtime and launcher this port is built from
- **MZ Labs**: SwitchRecomp (Apache-2.0); the credit "SwitchRecomp by MZ Labs" and NOTICE.txt must be kept
- **ScarletNX**: the native Vulkan renderer (GPL-3.0)
- **AMD**: FidelityFX Super Resolution 1 (MIT, `third_party/fsr1/LICENSE.txt`)
- **Dear ImGui** (MIT), libvpx and OpenH264 (BSD), SDL2 and zlib (zlib licence)
- **Claude** (Anthropic): AI pair programmer for this fork's changes

The program icon is the [Metroid icon created by Smashicons - Flaticon](https://www.flaticon.com/free-icons/metroid)
(Flaticon licence, free with attribution).

## Licences

The released program is distributed under **GPL-3.0** (`LICENSE-GPL-3.0.txt`), because it contains the ScarletNX
renderer. Each source file keeps its own licence; Apache-2.0 and MIT code is GPL-3.0-compatible.

Metroid Prime 4: Beyond is © Nintendo. This project is not affiliated with or endorsed by Nintendo.
