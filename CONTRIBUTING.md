# Contributing

Thanks for helping. This is a work in progress, so bug reports with logs are as useful as code.

## Reporting a bug

Use the **Bug report** form on the Issues page. Please attach:

- `Metroid Prime 4 Beyond.log` from the game folder (`~/.local/share/MetroidPrime4Recomp` on Linux) (the crash message names it), and `performance.txt` for stutter or slowdowns;
- your CPU, GPU, RAM, Windows version, and the renderer you use (OpenGL, Vulkan output or Native Vulkan);
- the version (Play screen > Updates, or the first line of `update.txt` in the game folder).

Without the log most crashes can't be found.

## Building

See **Build from source** in the README, or run `build.bat` from the repository root after putting `gcc\` and
`python\` there. The toolchain is not in the repository.

## Before you open a pull request

- Build both renderer variants (`build.bat` does) and make sure the EXE starts, reaches the title screen and loads a
  save. For a runtime change, play a few minutes on OpenGL and on Native Vulkan and check the `.log` for new warnings.
- Say what you tested and on what hardware. For a performance change, say what you measured, before and after:
  frame times (`performance.txt`) beat an FPS counter, and an average hides stutter.
- Keep each pull request to one change.

## Rules

- **No Nintendo files.** No game code, RomFS files, keys, generated C, textures or other assets, not even small
  ones. Edits to the generated code go through `beyond/beyond_patches.json` (`beyond/make_patches.py`), which
  stores only our replacement lines and hashes.
- **Don't change the game's timing.** The 60 Hz tick, physics and the game's own waits and sleeps stay as they are.
  Higher frame rates and faster loading are opt-in settings, never the default.
- **The runtime also builds with TCC:** no `unsigned __int128` or `__atomic_*` without the guard used in
  `runtime/cpu.h`.
- **Keep line endings:** `.gitattributes` stores files as they are; don't convert a file's CRLF/LF.
- **Licences:** your code is under the licence of the file you change (Apache-2.0 for the SwitchRecomp runtime and
  tools, GPL-3.0 under `runtime/snxvk/`). The released EXE as a whole is GPL-3.0.
- **No DLSS SDK files** (headers, libraries or `nvngx_dlss.dll`) in the repository.
- **No AMD FidelityFX DLLs** (`amd_fidelityfx_*.dll`) in the repository or the release: the launcher downloads them.
  Only the SDK's MIT-licensed API headers are vendored, in `runtime/snxvk/deps/FidelityFX-SDK-2.4.0`.

## AI-assisted contributions

This project itself is written with an AI pair programmer, so AI-assisted pull requests are welcome, on the same
terms as any other:

- Say in the pull request that AI helped, and with what.
- You are responsible for the change: you have read it, you understand it, you built it and you tested it as above.
- Pull requests that were not built or tested will be closed, AI-written or not.
