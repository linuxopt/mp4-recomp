# Metroid Prime 4 Recomp

<a href="https://recomplauncher.com/game/mp4-recomp"><picture><source media="(prefers-color-scheme: dark)" srcset="https://recomplauncher.com/assets/press/launcher/badge-light.svg"><img alt="Get it on RecompLauncher" src="https://recomplauncher.com/assets/press/launcher/badge-dark.svg" height="48"></picture></a>

A native Linux build of **your own copy** of *Metroid Prime 4: Beyond*, packaged as a single AppImage. It is built
on the SwitchRecomp runtime by MZ Labs: the game's Switch code is recompiled ahead of time into a native Linux library on your machine, and the
Switch GPU's work is translated to OpenGL 4.6.

This release is the result of a long stretch of play-testing and fixing: the game boots, plays, scans, sounds right and
runs at a steady 60 fps on the test machine, with most of the rendering bugs and stutters found along the way fixed.
Everything that changed is listed below.

> **No Nintendo files are included.** No game code, no RomFS, no keys, no generated C. You need your own legally
> obtained, dumped copy of the game. The AppImage holds the runtime, the recompiler and a bundled GCC and Python; it
> compiles the game from your files during the first-run setup. The only game-related file inside is a manifest of
> RomFS file names, sizes and SHA-1 hashes, used to verify your dump.

---

## Contents

- [Requirements](#requirements)
- [Install and play](#install-and-play)
- [Saves](#saves)
- [Controls and the in-game menu](#controls-and-the-in-game-menu)
- [Cheat support](#cheat-support)
- [Performance on the test machine](#performance-on-the-test-machine)
- [What changed](#what-changed)
  - [Rendering and visual fixes](#rendering-and-visual-fixes)
  - [Stutter and performance](#stutter-and-performance)
  - [Shaders](#shaders)
  - [Gameplay fixes](#gameplay-fixes)
  - [Audio](#audio)
  - [Video playback](#video-playback)
  - [Boot, setup and launcher](#boot-setup-and-launcher)
  - [Diagnostics and testing tools](#diagnostics-and-testing-tools)
- [Known issues](#known-issues)
- [Troubleshooting](#troubleshooting)
- [Advanced switches](#advanced-switches)
- [Building from source](#building-from-source)
- [Credits](#credits)
- [Licences](#licences)

---

## Requirements

- **Linux, any x86-64 CPU** (Steam Deck included), glibc 2.35 or newer. X11, or Wayland through XWayland.
- **GPU:** an OpenGL 4.3+ driver (Mesa or NVIDIA). Developed and tested on an **AMD Radeon RX 9070 XT** with Mesa 26.2
  (radeonsi).
- **SDL2** (controllers and audio) and **zlib**, which most distributions already ship.
- **For the game's videos:** OpenH264 2.x (`openh264` / `libopenh264`), or FFmpeg's `libavcodec` (faster, preferred when
  present). VA-API hardware decoding is used when available. Without any of these, videos stay black.
- **Your game dump:** *Metroid Prime 4: Beyond* for Switch, program `010019A01E2F2000`, with the update whose main has build
  ID `8275FBE1EC5191C279F3A943AD8524F4BAB087A3` (the cartridge alone is build `E860CBED…` and is refused). Either:
  - the **files** from your Switch: the game as `.xci` (cartridge) or `.nsp` (eShop) and the update as `.nsp` (DBI or
    nxdumptool), plus your Switch's **`prod.keys`** (Lockpick_RCM) in `~/.switch/`, or where Ryujinx or yuzu keep it.
    Setup decrypts them with those keys and applies the update to the game; or
  - an **extracted** ExeFS (`main`, `main.npdm`, `rtld`, `sdk`, `subsdk*`) and RomFS (`Worlds/`, `UI/`, `Preload/`,
    `Kernel.pak`, …) with the update applied.

  Setup checks the build ID and verifies all 445 RomFS files (26.9 GB) against the manifest.
- About 30 GB of free disk space for the installed game data, plus space for shader caches (from `.xci` / `.nsp` files the
  same: the unpacked files become the installed ones).

## Install and play

### Windows

1. Download `MetroidPrime4Recomp-PC-vYYYY.MM.DD.HHMM.zip` from [releases](../../releases/latest) and extract it anywhere with
   room, for example `D:\Games\`. You get a `Metroid Prime 4 Beyond` folder.
2. Run **`Metroid Prime 4 Beyond.exe`** in it.
3. **First run:** the setup screen asks for your game (its `.xci` / `.nsp` file, or an extracted folder) and the update
   (its `.nsp`, or an extracted folder). Press **Start Setup**: it verifies the files, copies them into the game folder
   and compiles the game with the bundled GCC (a few minutes on a fast CPU).
4. Press **Play**. The first launch builds shaders in the background; later launches load them from the cache.

Everything stays in the game folder, next to the EXE:

| Path | What |
|---|---|
| `Metroid Prime 4 Beyond.ini` | settings, rewritten with every option explained on the first start |
| `Metroid Prime 4 Beyond_data\` | the installed game data, saves, cheats and mods |
| `shader_cache\` | compiled shaders |
| `Metroid Prime 4 Beyond.log`, `setup.log` | logs for bug reports |

### Linux (AppImage)

1. Download `Metroid_Prime_4_Recomp-x86_64.AppImage` from the release and make it executable:

   ```bash
   chmod +x Metroid_Prime_4_Recomp-x86_64.AppImage
   ```

2. Run it:

   ```bash
   ./Metroid_Prime_4_Recomp-x86_64.AppImage
   ```

3. **First run:** the setup screen asks for your game: its `.xci` / `.nsp` file and the update's `.nsp`, or an extracted
   game folder (it can hold `exefs` and `romfs` folders, or the files directly) and an update folder if you extracted the
   update separately. Setup verifies the files, copies them
   into the install folder and compiles the game with the bundled GCC (a few minutes on a fast CPU).
4. Press **Play**. The first launch builds shaders in the background; later launches load them from the cache.

Everything is installed to `~/.local/share/MetroidPrime4Recomp`. An install from an earlier build in
`~/.local/share/MetroidBeyondRecomp` is moved there on the first start, saves and shader caches included.

**Installing somewhere else (an SD card):** start the AppImage once with `--install` and a folder. The game goes into a
`MetroidPrime4Recomp` folder there, and the choice is remembered, so later starts (from Steam, the app menu, or after
an update) need no option:

```bash
./Metroid_Prime_4_Recomp-x86_64.AppImage --install /run/media/deck/YOUR-SD-CARD
```

On a Steam Deck the SD card is a folder in `/run/media/deck/` (`/run/media/mmcblk0p1` on older SteamOS). You can also
put `--install /run/media/deck/YOUR-SD-CARD` in the game's **Properties > Launch Options** in Steam. The folder must be
on a drive formatted by SteamOS (ext4): setup refuses FAT32 (no files over 4 GB) and drives that can't run programs
(exFAT or NTFS without file permissions). `--install` alone prints where the game is installed, and
`--install default` goes back to `~/.local/share/MetroidPrime4Recomp`. An earlier install isn't moved for you: to keep
it (and its saves), move everything in its folder into the new one before setting up again. `MP4_RECOMP_HOME`, if set,
names the game folder directly and overrides `--install`.
Settings are in `Metroid Prime 4 Beyond.ini` in the game folder, rewritten with every option explained on the first start.

**Updates:** the Play screen checks this project's latest release when it opens (its UPDATES section, further down, also has
a CHECK button and the setting to turn it off). When there is a newer build it offers UPDATE: the new AppImage is downloaded next to the one
you started, checked against its SHA-256 and put in its place, and the launcher starts again; the previous AppImage is
kept as `<name>.previous` (UNDO puts it back). If the update changed the recompiler, the Play screen asks to rebuild
the game code (a few minutes). Downloads use the system's libcurl (the `curl` package). With `skip_launcher=1` the
check happens when you open the Play screen (`--launcher`). Builds before 2026-11-04 have no updater: download that
release once by hand. The Windows build checks the same way: it downloads `update-<version>.zip` and `update.txt` from this
repository's latest release, checks each file's size and SHA-256 and puts the new files in place.

### RecompLauncher

[RecompLauncher](https://recomplauncher.com/game/mp4-recomp) installs and updates the game from these releases, on Windows
and Linux. It asks for your game file, the update and your `prod.keys`, builds the game in its own folder, shows the main
settings on the game's page and starts the game straight away; its own updates replace the Play screen's. What it does is
in `recomplauncher.json`, which every release carries (next to the EXE, and inside the AppImage). The build is the same
setup as the Play screen's, run with no window: `"Metroid Prime 4 Beyond" --prepare --base GAME [--update UPDATE]
[--keys prod.keys]` prints `progress <0-1> <text>` lines and exits with 0 once the game is built. With the game already
installed (a new release whose recompiler changed) it rebuilds from the installed game files.

## Saves

Saves are plain files in:

```
~/.local/share/MetroidPrime4Recomp/Metroid Prime 4 Beyond_data/save/user/
```

**Using your Switch save:** export it with a save manager on a homebrew-enabled Switch (JKSV or Checkpoint), close the
game, back up the existing `save.bin`, and copy the exported files into that folder. A save from a newer game update
than the one this build targets may not load; restore your backup if it doesn't. Nintendo's cloud saves can't be
downloaded to a PC.

## Controls and the in-game menu

- Controllers work through SDL2 (tested with an Xbox Series X controller; **ZL is the left trigger**).
- **Mouse and keyboard** work together with a controller, on Linux and Windows. The defaults follow PrimeHack:

  | Action | Key or button |
  |---|---|
  | Move (left stick) | **W A S D**, or **arrow keys** |
  | Camera (right stick) | **the mouse**, or **I J K L** |
  | Fire, charge, interact (A) | **left click**, or **Enter** |
  | Lock on, grapple, scan (ZL) | **right click**, or **Shift** |
  | Secondary weapon (R) | **middle click**, or **F** |
  | Jump, dodge (B) | **Space** |
  | Morph ball (Y) | **Ctrl** |
  | Visors, spring ball (X) | **E**, or **Alt** |
  | L | **Q** |
  | D-pad right, left, up, down | **1**, **2**, **3**, **4** |
  | + | **R** |
  | - (pause: the game's own menu) | **Esc**, **Tab**, or **`** |

  While you play, the cursor is hidden and kept in the window. Opening the in-game menu (F1) or switching to another
  window frees it. The menu's **CONTROLS** page rebinds every button: select one, then press a key for it (Shift,
  Ctrl or Alt held with it make a chord such as `Shift+1`; pressing a key the button already has takes it off). The
  same page has the mouse buttons and the mouse settings: mouse look on or off, sensitivity, invert up and down, and
  the starting push (how far the stick is pushed by the smallest mouse movement, past the game's dead zone).
  In the `.ini`, `[Input]` has a `key_<button>=` line per button (`key_a=Enter`, `key_dpad_right=1, Shift+F`, `-` for
  none), and `mouse_buttons` sets what the left, right, middle, back and forward buttons press (default
  `a,zl,r,-,-`, `-` for nothing). On Windows with `fast_forward=1`, **Tab** toggles fast forward instead.
  **Esc** doesn't quit: close the window, or use QUIT GAME in the F1 menu.
- **1:1 mouse aim** (the mouse turns the first-person camera by a fixed angle, like a PC shooter) runs inside the game's
  motion-control aiming, so it needs the game's own **Settings > Controls > Camera > Enable Motion Controls** turned on.
  With it off the mouse moves the right stick instead. Mouse movement reaches the game on every frame at any frame
  rate (`SWR_MOUSE_POLL_60=1` brings back the old 60-a-second updates, which made aiming above 60 fps feel like 60).
- **F1** or the Home / Guide button opens the in-game menu (settings, cheats, FOV, upscaling).
- **F3** saves a screenshot to `captures/`, **F4** toggles the 60 fps limit, **F5** writes a frame trace for bug
  reports, **F11** toggles fullscreen.

## Cheat support

Cheats are off by default and need the **Cheat codes** switch left on. Open them on the launcher's **Gameplay** page
(scroll down) or on the **Cheats** page of the in-game menu (F1 or the Home / Guide button). Changes apply while the
game is running.

**Built-in cheats** for game build `8275FBE1EC5191C2`:

| Group | Cheats |
|---|---|
| Ammo | Missiles, Super shots, Large shots and Power Bombs don't decrease; ammo and bike boost don't decrease; infinite elemental ammo; use any elemental shot; more green crystals (hold ZL while depositing) |
| Samus | Infinite health; infinite jumps; speed booster (hold B to go faster); walk through walls (hold L for the other way); freeze the Flare Pool escape timer |
| Combat | One-hit kills, bosses included; two-hit kills, not bosses; allies at full health; allies can't go down |

The **Green Energy 10x** option is on the same page.

Some of these are still being tested. Walk through walls can leave you stuck, so keep a separate save before using it.

**Your own cheat files** use the Atmosphere (dmnt) format, so existing files work as they are. On Windows put them at
`Metroid Prime 4 Beyond_data\cheats\8275FBE1EC5191C2.txt` in the game folder; on Linux at
`~/.local/share/MetroidPrime4Recomp/Metroid Prime 4 Beyond_data/cheats/8275FBE1EC5191C2.txt`. Then turn each cheat on
in the in-game menu. Cheats that change game data work. Cheats that patch ARM instructions have no effect, because the
game's code is recompiled.

## Performance on the test machine

AMD Radeon RX 9070 XT, Mesa 26.2 radeonsi, warm shader caches, measured with MangoHud over real play sessions (frame
times after the save had loaded):

| Session | Play time | Average | Frames over 100 ms |
|---|---|---|---|
| Before this round of fixes | 5 min | 59.5 fps | 6 |
| Mid-way | 57 min | 59.8 fps | 30 (24 of them in one newly visited area) |
| This release | 5.6 min | 59.9 fps, 1% low 43 fps | 2 (both right after the save loaded) |

Loading a save still costs one freeze of about 0.5 s (the area's lighting volumes, 1.2 GB, are uploaded).

---

## What changed

Every fix below was found by playing, reproduced (most of them by replaying the player's recorded controller input
from the same save), traced to a root cause and checked again in the game.

### Rendering and visual fixes

- **Black arm cannon and black enemies after a cutscene.** The game keeps its lighting in six huge 3D "brick" volumes
  (1408×256×1008, 173–346 MB each) and streams new lighting into them as you move. The code that detects those updates
  only worked when a volume sat in one contiguous block of host memory, which these volumes never do, so no update was
  ever picked up. A new area (for example after the Fury Green cutscene) then had no lighting data, and the cannon and
  enemies, which take their ambient light from these volumes, went black. Volumes are now checked and uploaded page by
  page. Cost: about 0.06 ms per frame.
- **Lighting volumes update in slabs.** Each volume is handled as 63 slabs of 16 slices that are checked in rotation, and
  only the slabs that changed are uploaded. Before, every change re-uploaded the whole volume (54–138 ms, in bursts of
  six), and the world showed dark boxes until the next re-upload.
- **Magenta floor and suit at every spawn.** The game streams a texture's largest mip level in after the texture is
  already on screen, and marks that level as missing in the texture descriptor until it arrives. We ignored that field
  and sampled mip 0, whose memory still held leftover data: magenta speckle on the floor and Samus's heel fins for about
  half a second at each spawn. The sampler's minimum level now follows the descriptor's lowest resident mip, and when
  that value drops (the level has just arrived) the texture is re-checked immediately. The rainbow-coloured screen
  reported after opening a door was very likely the same bug.
- **Speckled textures on tree trunks** (a bug introduced and removed in this round): a mapped upload buffer for decoded
  ASTC textures could hand a texture stale data. It also turned out to have no measurable benefit, so it was removed.
- **Black cannon while blending reflections.** Moving between lighting zones makes the game blend two reflection cube
  maps on the GPU, drawing all six faces at once per mip level. The renderer had no path for a cube map built from render
  targets drawn that way and handed reflection shaders a flat image. Cube maps and arrays can now be assembled from such
  targets, including every mip level.
- **Dark box around the morph ball.** The fog effect reads a per-light texture the renderer can't look up yet; it got a
  black texture, which reads as fully shadowed. It now gets white ("no shadow").
- **Cannon clipping at the near plane.** The game enables depth clamping on every draw; it was ignored, so geometry
  right at the near plane (the first-person cannon, shadow-tile resets) was cut away. Depth clamping is now honoured.
- **The sun shining through trees and walls.** The lens flare fades by asking the GPU how many pixels of a test shape at
  the sun's position are visible. The emulator always answered "1000". Occlusion queries are now measured on the GPU
  and returned a frame or two later, without stalling.
- **Reflection and lighting cube maps rendered onto one face.** The game renders them with geometry shaders, which the
  translator didn't have, so all six faces landed on one. Added a geometry stage (per-vertex inputs, emits,
  passthrough), a third pipeline stage, and the `VMAD` and `VSETP` instructions.
- **Black rock faces, mountains and sky.** Rock materials pick UV sets and tangent frames with indexed attribute reads
  (`IPA.IDX`), which returned 0. Also fixed on the way: `texelFetch` from render targets ignored the resolution scale,
  texture lookups now take the render target drawn last when several share an address (dynamic resolution), flow
  instructions honour their condition-code test, `ISET.CC` sets the flags, and `FMNMX` / saturate follow Maxwell's NaN
  rules.
- **Hard-edged black boxes when looking up.** NaN pixels spread through the blur and downsample passes. Pixel shader
  outputs now have NaN replaced by 0, as the hardware effectively does.
- **Neon green/orange gradients and a flat-coloured cannon.** The fog/light froxel volume is filled each frame by a vertex
  shader writing through image stores (`SUST`), which the translator lacked, and the light list is built by a vertex
  shader writing a global buffer that later draws read back from stale memory. Image stores are now supported in vertex
  and pixel shaders, and shader-written buffers stay on the GPU between draws.

### Stutter and performance

- **Gameplay at 60 fps instead of 30–40.** Every draw re-uploaded the tail of the vertex buffer it drew from (50–60 MB a
  frame). Draws into one buffer now share one cached region: 4–5 MB a frame.
- **Hitches when turning (100–300 ms).** ASTC textures streamed in as the camera turned were decoded synchronously on
  the GPU thread (this GPU has no ASTC). They now decode on background threads: a texture shows from its small mip
  levels at once and gets the large ones a frame or two later. Streamed ASTC texture arrays decode in the background too.
- **Lighting-volume uploads about 3.5× faster.** The 173–363 MB volumes upload through a persistently mapped buffer and
  are read from guest memory in parallel.
- **Fewer forced waits on the GL thread.** Removed round trips that stopped the renderer until the GL thread caught up:
  an error check after every texture view, two uniform-location lookups, and `glGenerateMipmap`, which was wrongly
  synchronous.
- **Auto-exposure readback no longer stalls.** It is read asynchronously, and the choice is remembered across sessions,
  instead of running synchronously for 30 frames after every load. Small readbacks the game needs immediately (the scan
  visor's counters) still return the same frame.
- **Driver submits cut from 125 to 15 per frame.** Bloom and exposure mip chains were recopied in full each time one more
  level was drawn (about 128 copies a frame). Only changed levels are copied now.
- **No `glFlush` per occlusion-query report.**
- **A slow frame every 5 seconds.** The background shader scan reread all of the game's GPU memory (about 300 MB) every
  5 s, pushing the game's data out of the CPU cache, and on Linux it ran at full priority. It now rereads only buffers
  that changed or hold shaders (about 70 MB, 24 ms instead of 110 ms on the test machine), with a full pass about once a
  minute or when a draw needs a shader it missed, and background threads run at a lower priority on Linux.
- **Default resolution 1×.** The launcher's auto-settings no longer pick 2× on high-end cards.
- **VSync on by default** on Linux: frames reaching the monitor wrong fell from 59 to 1–2 of 3600.

### Shaders

- **Extract shaders (Linux, Advanced > Shaders > EXTRACT).** Reads your own game files and translates every shader in
  them ahead of time, with a progress dialog. The game keeps most of its shaders packed inside its area files; the
  extraction finds all 68,404 of them and saves the 38,178 translations that don't depend on the game's running state to
  `shader_cache/extracted_v<version>.bin` (131 MB). The game reads only that file's index at start and a translation the
  first time it needs it. It takes about 2 minutes on a Ryzen 9 7900X3D with an NVMe drive (it reads all 27 GB of the
  game's files once; expect longer on a Steam Deck or an SD card) and needs `libzstd` (the `zstd` package). Nothing is
  compiled: the graphics driver still compiles each shader in the background when an area loads it. The gain is small:
  in the Ice Belt area-load test the pre-build's translating time fell from 1.26 s to 0.70 s, and the stutter was the
  same within measurement noise. Extract again after an update that changes the shader translator (the old file is
  removed then).
- **Shaders built before the game draws with them.** The game unpacks its shaders into GPU memory when an area loads; a
  scanner thread finds them, the GL thread translates a few per frame, and compile threads build them so they're ready
  before the first draw.
- **The right variant is pre-built.** Some shaders need a different version depending on what they're drawn with
  (pixel shaders reading integer textures such as the light grid; vertex shaders with integer inputs such as bone
  indices). The pre-build now predicts the variant from how the shader uses each input (right for 46 of 48 textures and
  all 132 vertex inputs in the test area). First visit to an area: 109 → 40 compiles during play (4.3 s → 2.0 s).
  Second visit: **0**.
- **No more freezes from indirect-branch shaders.** Shaders with an indirect branch (`BRX`) take their jump table from
  constant memory, which the pre-build doesn't have, so the first draw compiled them on the spot (50–160 ms each; seven
  in a row froze the game for 850 ms in a new area). The draw now uses the pre-built version, which is just as correct,
  while the exact one compiles in the background and replaces it. On a cold-cache test route: 1.48 s → 0.36 s of
  compiles.
- **A draw joins a compile already in progress** instead of starting a second one, and the translated-shader cache loads
  again instead of being rebuilt every launch.
- **The scanner wakes up early** when a shader is drawn before it was found, so shaders an area loads over the next
  seconds are found before they're drawn.
- **Several compile threads with their own shared OpenGL contexts** (Mesa and NVIDIA compile GLSL on the calling
  thread), and larger driver shader caches.
- **Translator additions:** `AL2P` and indexed attribute loads, `LDG`/`STG` global memory through any constant buffer,
  `VMNMX`, `VMAD`, `VSETP`, image stores in vertex and pixel shaders, up to 11 global buffers per shader (was 8), carry
  flags declared where they're read.

### Gameplay fixes

- **The scan visor works** (it was needed to open the first door). Several bugs stood in the way:
  - the depth-stencil buffer the scan reads was given to the shader as plain depth, so no pixel ever matched a target;
    stencil reads now get a stencil view;
  - the game copies its depth-stencil buffer with the 2D copy engine; the copy fell back to stale CPU memory and
    required an exact size match, which dynamic resolution broke;
  - a decoder bug in the instruction that works out which target a pixel belongs to;
  - the scan counter's write-back copied 7.5 KB of GPU data over live game memory every frame, rolling it back; only the
    words the GPU actually changed are written back now.
- **ZL lock-on** confirmed working (left trigger past about a quarter).
- **Hard difficulty unlocked from the start.** On the Switch, Hard only opens after you finish the game. Setup now
  applies a one-line patch (`beyond/beyond_patches.json`) so the difficulty screen reads that unlock flag as set. If
  you already ran setup, run it again to get the patch.

### Audio

- **Missing sound.** The game sends almost every sound through **splitters**, which the mixer didn't support: most
  sounds went to the final mix at zero gain and skipped the reverb and filter chains. Splitters are implemented, with
  their state kept between updates.
- **Missing cutscene voices.** The game mixes in 5.1, with dialogue in the centre channel, which the stereo output
  dropped. Surround downmix is now on by default, as on a console with stereo output.
- **Sound at real speed.** The runtime mixed 5 ms of audio per renderer update; the game updates 60 times a second, so
  it got 0.3 s of sound per second. Each update now mixes what real time needs.
- **Broken sound after a menu transition** (the game updating 20,000 times a second for half a second): fixed.
- Audio renderer REV15 voice layout supported.

### Video playback

- **Intro video decoded on Linux** (it showed as garbled noise): the system's OpenH264 is loaded; FFmpeg and VA-API are
  used when present. One decoder per NVDEC channel; decoder addresses kept for the buffer's lifetime.
- **Smooth video:** picture conversion on up to 8 threads with AVX2 (9 ms → 0.4 ms per 1080p picture).
- **Correct pictures in order:** decoded pictures are filed under their real surface (`CurrPicIdx`), which fixed the
  judder, the title logo flashing into the intro and the static.

### Boot, setup and launcher

- Prime 4 setup: title and build-ID checks, RomFS layout and manifest verification.
- `fsp-srv` 205 (`OpenDataStorageWithProgramIndex`): the game mounts its RomFS.
- HID: player 1 reports a single controller style, fixing the six-axis abort at boot.
- The Play screen checks for Prime 4's data and shows Prime 4's titles.
- Setup takes the game's `.xci` / `.nsp` and the update's `.nsp` directly, decrypted with your own `prod.keys`, and applies
  the update's RomFS patch (BKTR) itself: no emulator needed to extract them.
- The AppImage runs on any x86-64 CPU: its program is built with the bundled toolchain instead of the build
  machine's, which had made it need AVX-512 and glibc 2.43 ("CPU ISA level is lower than required" on a Steam Deck).
- Cheats in Atmosphere / dmnt format, toggled per cheat in the in-game menu.
- AMD FidelityFX FSR 1 upscaling with a sharpness slider.
- **AMD FSR 3.1 frame generation** (Windows, `renderer=vulkan_native`, `[Graphics] fsr_frame_generation=1`): one
  generated frame between each pair the game renders, on any GPU with Vulkan 1.3. AMD's two signed DLLs are not part
  of the release: the launcher's Graphics page has a **Download AMD FSR frame generation** button that fetches them
  from AMD's FidelityFX SDK on GitHub (v2.4.0, size and SHA-256 checked). Limits for now: the HUD is generated along
  with the scene (no HUD-less image yet), it is off with HDR, and it pauses while the in-game menu is open and on
  screens without a 3D scene.
- **NVIDIA DLSS download**: the launcher's Graphics page has a **Download NVIDIA DLSS** button that fetches
  `nvngx_dlss.dll` from NVIDIA's DLSS repository on GitHub (v310.9.16, size and SHA-256 checked) for DLAA
  (`[Graphics] dlss=1`, RTX cards).
- Launcher themes (Display page, LAUNCHER LOOK): Beyond, Amber visor, High contrast, or your own `theme.ini` next to
  the game (`accent=#RRGGBB` lines; the page shows the text contrast it gives). The in-game menu follows the theme.
  **Reduce motion** stops the background rings and the progress-bar stripes.
- The setup screen shows the free space on the game folder's drive beside the 30 GB it needs, and gives the Windows
  key folder (`%USERPROFILE%\.switch`) on Windows.

### Diagnostics and testing tools

- **Input recording and replay:** every session records controller input to `input_record.csv`; `SWR_TEST_SCRIPT`
  replays it frame by frame, with `SWR_TEST_CAPTURE_FRAMES` / `SWR_TEST_CAPTURE_PREFIX` for frame captures and
  `SWR_TEST_EXIT_FRAME` to stop. This is how most bugs above were reproduced without the player. A replay never takes
  the mouse pointer.
- **Mouse aim per frame** (Linux): `SWR_TEST_MOUSE_HZ=1000` (from guest frame `SWR_TEST_MOUSE_FROM`, where the replay's
  input stops) sends the window that many one-count mouse movements a second and logs, each second, the game's look
  updates and how many found no new movement. `SWR_TEST_ROWS=FROM,TO,PATH` writes the window's middle row of pixels for
  every frame in that range, cheap enough to keep the frame rate, so the sideways shift from frame to frame shows on
  which frames the camera turned.
- **Frame traces** (F5, or `SWR_TRACE=<frame>`): every draw with its shaders, textures (including the raw descriptor
  words), buffers and a render-target snapshot.
- **`SWR_SHADER_OVERRIDE_DIR=dir`** compiles `dir/<hash>_vs.glsl` (or `_fs`, `_gs`) instead of the translation, to test a
  change to one shader; **`SWR_TRACE_RAW=address`** writes a traced texture as floats and its guest memory next to the
  trace; **`SWR_TRACE_DMA_RANGE=address:size`** logs the copy-engine and 2D-engine writes into a range.
- **`SWR_DBG_FS=hash@anchor@expr`** shows a pixel shader's intermediate value on screen.
- **First-draw shader log** says why each shader wasn't pre-built ("not found in GPU memory yet", "found 2.1 s ago, 66
  ahead of it", "translated without constant values", …).
- **`SWR_STALL_STACK=1`** logs the GL thread's stack whenever one call runs past 15 ms. It's how the remaining driver
  hitches were traced.
- Slow-frame, present, video and audio timing logs; the last five logs are kept.

---

## Known issues

- **Driver shader-variant compiles (AMD/radeonsi).** radeonsi compiled a separate version of every vertex shader that
  reads per-instance data, on the drawing thread and without caching it (25–40 ms each, on every launch). Those shaders
  now read that data themselves, which removed most of the area-change hitches (16 slow frames to 2 in the Ice Belt
  test); `SWR_INSTANCE_FETCH=0` turns this off. A first launch after updating still shows some objects a moment late
  while the new versions compile once.
- **First visit to a new area** can still compile a handful of shaders the game loads moments before drawing them.
- **Loading a save** freezes for about 0.5 s while the lighting volumes upload.
- **The FOV slider** has no effect yet (the camera address for this build hasn't been found).
- **Not modelled yet:** a filter effect on two audio submixes and an aux effect, so a few sounds may be clearer than
  intended.
- **Windows:** the release's Windows build is made by GitHub Actions from this repository. The Windows work comes from
  a contributor (pull request #13), who played it on Windows; this fork's maintainer tests on Linux only.
- The rainbow screen after opening a door is believed fixed (see above) but wasn't reproduced directly.

## Troubleshooting

- The log is `~/.local/share/MetroidPrime4Recomp/Metroid Prime 4 Beyond.log` (the last five are kept). Attach it to bug
  reports.
- Screenshots with F3; a frame trace with F5 at the moment something looks wrong helps most.
- Black videos: install OpenH264 or FFmpeg.
- Rendering glitches: delete `~/.local/share/MetroidPrime4Recomp/shader_cache` once to rule out a stale cache (the
  next launch rebuilds it).

## Advanced switches

Environment variables for testing and for turning individual fixes off if one ever misbehaves:

| Variable | Effect |
|---|---|
| `SWR_NO_TIC_MINMIP=1` | ignore the texture descriptor's lowest resident mip |
| `SWR_NO_RMIN_RECHECK=1` | don't re-check a texture when more of its mips become resident |
| `SWR_NO_SLABS=1` | upload lighting volumes whole instead of by slab |
| `SWR_NO_BRX_STANDIN=1` | compile indirect-branch shaders at the draw instead of using the pre-built stand-in |
| `SWR_NO_SCAN_WAKE=1` | scanner keeps its fixed schedule |
| `SWR_NO_MEM_PREBUILD=1` | turn the shader pre-build off |
| `SWR_INSTANCE_FETCH=0` | let the driver fetch per-instance vertex data (radeonsi; the old path) |
| `SWR_MOUSE_POLL_60=1` | mouse aim gets the mouse's movement with the 60 Hz input poll (the old path) |
| `SWR_NO_PREBUILD_INTTEX=1` | pre-build plain variants only |
| `SWR_NO_NAN_GUARD=1` | keep NaN pixel outputs |
| `SWR_ASTC_SYNC=1` | decode ASTC on the GPU thread |
| `SWR_NO_GPU_SSBO=1` | read shader-written buffers from guest memory |
| `SWR_SYNC_SHADERS=1` | draws wait for their shaders (the launcher default, `wait_for_shaders=1`) |
| `SWR_STALL_STACK=1` | log the GL thread's stack on calls over 15 ms |

## Building from source

The source for the runtime, recompiler and AppImage packaging is in this repository (no Nintendo files: no game code,
no RomFS, no keys, no generated C). Developer notes, the bring-up log and more build options are in
[DEVELOPMENT.md](DEVELOPMENT.md).

```bash
bash linux/install_deps.sh
bash linux/setup_and_build_runtime.sh
bash linux/make_appimage.sh
```

`make_appimage.sh` produces `linux/dist/Metroid_Prime_4_Recomp-x86_64.AppImage` and `update-linux.txt` (its version, the
UTC build time, its SHA-256 and the ID of the recompiler it brings); `bash linux/publish_release.sh [TAG]` uploads both
to the latest release (or TAG), which is what the launcher's updater reads. To run from the source tree without
the AppImage instead: `bash linux/setup_game.sh GAME_FOLDER [UPDATE_FOLDER]`, then `bash linux/run.sh`.

### Windows

You need a 64-bit GCC (MinGW-w64), Python 3 and glslang:

- **GCC:** [MSYS2](https://www.msys2.org) with its UCRT64 compiler (`pacman -S mingw-w64-ucrt-x86_64-gcc`, then add
  `C:\msys64\ucrt64\bin` to `PATH`), or [winlibs](https://winlibs.com) GCC 14.2.0 (UCRT, POSIX threads, x64) unpacked
  into `gcc\` in the repository. Releases are built with the winlibs one.
- **Python 3:** any Python 3 on `PATH` or the `py` launcher, or the Python 3.12 embeddable package in `python\`.
- **glslang:** from the [Vulkan SDK](https://vulkan.lunarg.com) (found through `VULKAN_SDK`), on `PATH`, or named with
  `set GLSLANG=C:\path\to\glslang.exe`.

All four can be fetched at their pinned versions instead (winlibs GCC 14.2.0, Python 3.12, glslang 16.6.0, SDL2), each
checked by SHA-256, into the folders `build.bat` looks in:

```powershell
powershell -ExecutionPolicy Bypass -File windows\get_tools.ps1
$env:GLSLANG = "$PWD\third_party\glslang\bin\glslang.exe"
```

Then:

```bat
build.bat
```

This builds `bin\runtime_win64_win32.exe`. To set the game up from the source tree, copy it next to where the game
should live as `Metroid Prime 4 Beyond.exe`, then start it with the recompiler pointed at the repository and a GCC for
the game code:

```bat
set SWR_RECOMPILER=C:\path\to\mp4-recomp
set CC=C:\msys64\ucrt64\bin\gcc.exe
"Metroid Prime 4 Beyond.exe"
```

The setup screen then works as on Linux: choose your `.xci` / `.nsp` and the update `.nsp` (with `prod.keys` in
`%USERPROFILE%\.switch` or where Ryujinx or yuzu keep it), or extracted folders. `SWR_PYTHON` names the Python to use
when `python.exe` on `PATH` isn't the one you want.

### Windows release

`build.bat release X.Y.Z` builds the runtime and packs `release\X.Y.Z\`: the game folder, the zip players download,
`update-X.Y.Z.zip` and `update.txt` for the Play screen's updater.

The Windows build in a release is made by GitHub Actions: **Actions > Windows release > Run workflow** (optionally with
the release's tag; the latest release by default). [.github/workflows/release.yml](.github/workflows/release.yml) fetches
the pinned toolchain with `windows/get_tools.ps1`, runs `build.bat release` with the build's UTC time as the version
(`YYYY.MM.DD.HHMM`, as the AppImage's) and adds the three files to that release, next to the Linux AppImage. `update.txt`
names the release's tag (`SWR_RELEASE_TAG`), so installed copies find `update-<version>.zip` there.

`build.bat release X.Y.Z` also packs a release folder in `release\`. It needs the pinned toolchain in `gcc\` and
`python\` (both go into the release, so players need nothing installed) and `redist\zlib1.dll` and `redist\SDL2.dll`.

## Credits

- **yozander2009-stack** and contributors: the recompiled runtime and launcher this port is built from
- **MZ Labs**: SwitchRecomp (Apache-2.0); "SwitchRecomp by MZ Labs" and NOTICE.txt must be kept
- **ScarletNX**: the native Vulkan renderer (GPL-3.0)
- **AMD**: FidelityFX Super Resolution 1 (MIT); FidelityFX SDK 2.4.0 API headers (MIT) for FSR 3.1 frame
  generation, whose DLLs are downloaded from AMD at the player's request
- **NVIDIA**: DLSS, downloaded from NVIDIA at the player's request (`nvngx_dlss.dll` is not part of the release)
- **melos444**: the [Green Energy 10x](https://gamebanana.com/mods/734159) mod that the Gameplay option of that name is based on
- **Gamerjin**, **Luffysan** and **ZiT866**: the [cheat research on GBAtemp](https://gbatemp.net/threads/metroid-prime-4-beyond-010019a01e2f2000.677778/) that the built-in cheats are based on
- **PlagueDrRiess**: the depth-target readback work in the renderer (read back only the part a DMA copy needs, and the
  scan visor's reticle depth straight from the scaled buffer), ported here from the Windows fork of this project
- **Dear ImGui** (MIT), libvpx and OpenH264 (BSD), SDL2 and zlib (zlib licence)
- **melos444**: the [Green Energy 10x](https://gamebanana.com/mods/734159) mod that the Gameplay option of that name is based on
- **Gamerjin**, **Luffysan** and **ZiT866**: the [cheat research on GBAtemp](https://gbatemp.net/threads/metroid-prime-4-beyond-010019a01e2f2000.677778/) that the built-in cheats are based on
- **Claude** (Anthropic): AI pair programmer for this fork's changes
- Program icon: [Metroid icon created by Smashicons - Flaticon](https://www.flaticon.com/free-icons/metroid)

## Licences

The released program is distributed under **GPL-3.0** (`LICENSE`), because it contains the ScarletNX renderer; its
source is this repository. Each source file keeps its own licence (`LICENSE.txt` and `NOTICE.txt` for SwitchRecomp's
Apache-2.0; Apache-2.0 and MIT code is GPL-3.0-compatible). The licence texts are also inside the AppImage under
`usr/share/metroid-prime-4-recomp/licenses`.

*Metroid Prime 4: Beyond* is © Nintendo. This project is not affiliated with or endorsed by
Nintendo.

[![Listed on PlayPorted](https://playported.com/badge.svg)](https://playported.com/ports/1366-metroid-prime-4-beyond/) [![Listed on Decomp Buddy](https://decompbuddy.com/badge.svg)](https://decompbuddy.com/games/metroid-prime-4-beyond-mp4-recomp-nintendo-switch)
