# The Moon River launcher

One window for setting up, building and playing **Moon River** (Bayonetta 2): it checks your dump, runs the build with
live progress, lists mods, edits `config/graphics.ini` and `config/controllers.ini`, and starts the game in the same
window. C++17, SDL3 and [RmlUi](https://github.com/mikke89/RmlUi) on SDL's GPU API. Linux and Windows; on macOS it
builds (Homebrew's `sdl3` and `freetype`), draws its screens with Metal, starts the game as `build/moon-river-game`, as
on Windows, and is also put in a bundle, `build/Moon River.app` (`macos/make_app.sh`).

## Identity & art

`src/platform.h` sets `APP_NAME` (`Moon River`), `APP_ID` (`moon-river`), `UPDATE_REPO` (`Zexyen/Moon-River`), and
`LAUNCHER_NAME` (`moon-river-launcher`, also the CMake target in `CMakeLists.txt`). The art in `ui/img` (`logo.png`
and `icon.png`) is Moon River's own wordmark and crescent-over-river app icon; the night sky, live astronomical moon
phase, river reflection, progress disc and outlined button glyphs are generated procedurally in `src/ui_render.cpp`.

## Build

Needs CMake 3.20+, a C++17 compiler, SDL3 3.4 or newer (with its CMake config) and FreeType. RmlUi 6.3 is fetched by
CMake, or pass `-DRMLUI_SOURCE_DIR=<checkout>` to build without network access.

```
cmake -S launcher -B build/launcher -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/launcher
./build/moon-river-launcher
```

On Windows it is cross-built with llvm-mingw (`-DCMAKE_TOOLCHAIN_FILE=<mingw toolchain>`, as the release workflow
does), statically linked but for `SDL3.dll`, and also makes the top-level `moon-river-launcher.exe`, a small program
that starts `build\moon-river-launcher.exe` (a zip cannot hold a link).

The program is written to `build/moon-river-launcher` and finds the repository from its own folder (or from `MR_REPO`).
The screens are in `launcher/ui` (RML and RCSS, data model `mr`) and are read at start, so they can be changed without
rebuilding.

## Releases

`.github/workflows/release.yml` builds the launcher on Ubuntu 22.04 with SDL3 (`release-3.4.18`). Pushing a tag such as
`v0.1.0` publishes a release with `MoonRiver-<tag>-linux-x86_64.tar.gz`: the repository source plus
`build/moon-river-launcher`, the `libSDL3.so.0` it uses, `build/mrbuild` (the build tool, `tools/mrbuild`), the runtime
built in CI (`prebuilt/`, with FFmpeg) and clang + lld (`deps/llvm`). Unpack it and run `./moon-river-launcher`.
Building the game then needs only a Vulkan driver and OpenGL from the system. Running the workflow by hand only builds
the archive and keeps it as a workflow artifact.

The same tag also publishes `MoonRiver-<tag>-windows-x86_64.zip`, cross-built on Ubuntu with llvm-mingw and then tested
on a Windows runner (`mrbuild smoke` against the runtime it ships): `moon-river-launcher.exe`,
`build\moon-river-launcher.exe`, `SDL3.dll`, `build\mrbuild.exe`, the runtime (`prebuilt\`, with FFmpeg) and LLVM's
Windows clang + lld (`deps\llvm`). It needs only a Vulkan driver.

## How it works

- **Build.** Runs `mrbuild setup` (`build/mrbuild` in a release, `tools/mrbuild/target/release/mrbuild` in a checkout,
  built with cargo first; the launcher says so when it is missing) and reads its step lines and `build/logs/compile.log`
  for progress. Stop sends SIGTERM (on Windows it ends the build's job object); the next build resumes.
- **Button hints.** The footer shows outlined glowing prompts (`gen:glyph` in `src/ui_render.cpp`): those of the device
  used last, or of `button_prompts` in `config/controllers.ini` when that names one (read at start).
- **Mods.** Listed with `mrbuild mods-tree`; switches are written to `config/mods.ini`. Code mods need a rebuild from
  the mods step, RomFS mods apply at the next start.
- **Play in the same window.** The runtime link step also makes `build/libmoonriver.so`. The launcher loads it, hands
  its SDL window over (SDL global property `moonriver.window`, which `runtime/display.c` adopts) and calls
  `mr_host_main`. When the game exits through `ExitProcess`, `plat_exit_hook` starts the launcher again
  (`moon-river-launcher --returned <code>`), unless the player closed the window or it was started with `--play`. If
  `libmoonriver.so` is missing or fails to load, the launcher hides its window and runs `build/moon-river-game` instead.
  On Windows the game always runs that way, as `build\moon-river-game.exe` with its own window. Either way the game's
  output goes to `build/run/moon-river-game.log`, and the four runs before stay beside it as `moon-river-game.1.log` to
  `.4.log`. With a data folder (`--data <folder>`, `MR_DATA`, or RecompLauncher's `RECOMPLAUNCHER_DATA`) the logs are
  in its `logs/`.
- **Updates.** A release build (`MR_VERSION` `vX.Y.Z`) asks GitHub for the latest release at start and, if it is newer,
  shows a `vX.Y.Z is out` pill and offers `Update to vX.Y.Z` on the title menu. That downloads this platform's archive
  with the system's `curl`, checks it against the SHA-256 digest GitHub lists for it (`build/mrbuild sha256`), unpacks
  it with `tar`, moves its files over the installed ones (keeping the player's `config/`) and starts the new launcher.
  The replaced files wait in `build/update` until the next start. A release launcher will not start a game that
  `build/built_version` says another release built; it opens the build screen.
- **Straight to the game.** `moon-river-launcher --play` (on Windows `moon-river-launcher.exe --play`) starts the game
  at once and ends when it does, for a Steam shortcut (Steam Deck game mode), Playnite, LaunchBox or a desktop entry.
  Settings → Display → Desktop shortcut writes a shortcut that does this (`create_shortcut` in `src/platform.cpp`):
  `Moon River.lnk` on the Windows desktop, `moon-river.desktop` in the Linux application menu and on the desktop. Its
  icon (`win32/launcher.ico`, `ui/img/icon.png`) is Moon River's crescent-over-river icon.
- **Input.** Mouse, keyboard (arrows, Enter, Escape) and controllers (d-pad or stick, A/Cross select, B/Circle back;
  left and right change a setting, LB/RB switch settings tabs).
- **Test aids.** Environment variables: `MR_REPO`, `MR_SCREEN` (`title`, `setup`, `build`, `mods`, `settings`),
  `MR_TAB` (`0..2`), `MR_BUILD_DEMO`, `MR_MOON_PHASE` (`0..1`), `MR_UPDATE_DEMO`, `MR_SLOWMO`, `MR_SHOT=<file.png>`.

Fonts in `ui/fonts` (Marcellus, Source Sans 3) are under the SIL Open Font License (the `OFL-*.txt` files next to
them). The pictures in `ui/img` (`logo.png`, `icon.png`) are Moon River's own art, and the procedural textures (sky,
live moon phase, river shimmer, progress ring and glyphs) are generated in `src/ui_render.cpp`.
