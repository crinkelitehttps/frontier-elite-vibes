# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Frontier Elite Vibes: a fork of GLFrontier Extended (Frontier: Elite II's 68k code, disassembled and translated to C, plus a host in C/C++ with an OpenGL renderer). This fork adds a 3D cockpit view, OpenTrack head tracking, OpenXR VR, motion controllers, gamepads, strafing and a **native scene renderer**. Remotes: `origin` = crinkelitehttps/frontier-elite-vibes, `upstream` = BrettWilsonDev/glfrontier-extended. `gh` defaults to upstream here: always pass `-R crinkelitehttps/frontier-elite-vibes`.

The repo lives inside a "play folder" (`..`, not a git repo) holding saves, `glfrontier.cfg`, `gamepad.cfg`, build/package scripts and `devtools/`.

## Build and run

There is no test suite; changes are checked by building and running the game.

```sh
# Original (unmodded) game code — the variant this fork actually uses and ships
cmake -S . -B build-original -G Ninja -DCMAKE_BUILD_TYPE=Release -DGLF_MODDED_FE2=OFF
cmake --build build-original -j$(nproc)
../glfrontier.sh            # runs build-original/GLFrontier from the play folder (saves/cfg live there)
../build-and-run.sh         # build + run in one go
```

Default `GLF_MODDED_FE2=ON` builds `fe2/fe2_modded.s` (Brett's mods: AFMU, free camera, custom ships). Host code that uses mod labels must be inside `#if FE2_USE_MODDED` with a fallback, or the original build breaks. Build both variants when touching shared host code.

Release packaging (play-folder scripts): `../build-windows.sh` / `../package-windows.sh [vibes-N]` (mingw-w64 cross build into `../build-win64`, zip into `../releases`), `../package-linux.sh [vibes-N]` (builds committed HEAD in docker ubuntu:22.04 for glibc 2.34). Package templates are in `../release-files/`.

### Changing the 68k game code

1. Build as68k once (`make` in `tools/as68k`, or cmake; copy the binary somewhere outside the repo).
2. Edit `fe2/fe2.s` (original) and/or `fe2/fe2_modded.s`.
3. `python3 tools/build_fe2.py --variant all --as68k <path>` (set `TMPDIR` to a dir on the repo's filesystem) regenerates `fe2*.s.c`, `fe2*_bin.h`, `fe2*_labels.h`.

Rules (the header of `fe2/fe2_modded.s` has the full version):
- Nothing in the middle of the file may change size — saves and host code hold addresses. Patch with same-size `jmp`/`jsr` hooks padded with `nop`, put new code/data at the **end** of the file. After assembling, `fe2*_labels.h` must only gain new `L*` lines; also diff local label addresses in `fe2*.s.c` against HEAD.
- `bra.w` can't reach the end of the file; use `jmp`.
- Comments are whole lines starting with `*`.
- Careful with sed on asm: an identical instruction may exist elsewhere.
- Host C finds game addresses only via `FE2_*` from `fe2*_labels.h`.
- `hcall #N` calls `hcalls[N]` in `src/hostcall.c` (C reads 68k regs with `GetReg`/`SetReg`, RAM with `rdbyte` etc.). Used numbers: `$60+` = `Nu_*` draw calls, `$7a-$7d,$7f-$80` cockpit, `$7e` strafing, `$81-$84` native scene, `$85` cockpit screen controls, `$86-$89` own HUD (`gl_hud.c`). Add new ones as an `equ` in the .s, an entry in `hcalls[]`, and the C function.

## Architecture

- **Emulation**: `fe2/fe2*.s.c` is the whole game as one giant C switch over 68k registers/RAM (`host.c`, `m68000.h`). It runs on the game thread; `GLF_RENDER_THREAD` controls the separate render thread. Game globals are off `a6` (`A6_*` equates), common routines through `a5`.
- **Rendering pipeline** (`src/gl/`): the game's 3D engine walks objects and calls `Nu_Put*` host calls; `gl_scene.c` records each primitive into the game's depth-tree nodes as a self-contained payload (emulated RAM moves on before drawing), then replays far-to-near into `gl_draw.c` batches at `Nu_DrawScreen`. Prims are in `gl_prims.c`, planets (decoded from planet bytecode, op `$1f`) in `gl_planet.c`/`gl_planet_tex.c`, haze in `gl_atmos.c`, 2D fills in `gl_fill2d.c`, presentation in `gl_screen.c`. `src/renderer.h` is the emulator↔renderer interface; Ctrl-E toggles the game's software renderer.
- **Cockpit** (`gl_cockpit.c`): originally re-ran the game's 3D engine up to 32 times with turned cameras ("passes") and stitched them with stencils — the source of seam artefacts. With the native scene on, it runs only the ahead pass plus "capture passes" toward planets, and nothing from the passes is shown.
- **Native scene** (`gl_native.cpp`, the current main line of work): asm hooks (`Lnative_object`, `Lnative_object2`, `Lnative_clip*`) capture every object's pose — including ones the game culls — and its model bytecode is run natively by `Runner` using the ship builder's decoder `tools/fe2ShipBuilder/fe2core.cpp` (compiled into the game with `FE2CORE_EXTERNAL_IMAGE`). Sky, far bodies/sun, planets and atmosphere are drawn natively in `native_draw_sky`. Read the header comment of `gl_native.h` first. Env: `GLF_NATIVE_SCENE` unset/2 = native filled (default), 1 = wireframe overlay over the game's picture (for alignment checks), 0 = original renderer. `GLF_NATIVE_PLANETS` only matters when the scene mode isn't 2.
- **Input/devices**: `input.c` (mouse/keys, laser pointer latch), `gamepad.c` (`gamepad.cfg` bindings; VR controllers act as a virtual Xbox pad), `headtrack.c` (OpenTrack UDP 4242), `vr.cpp` (OpenXR; Windows via D3D11 interop), `gl_laser.c`, `strafe.c`, `settings.c` (`glfrontier.cfg`).
- **Other**: ImGui menu, cheats, mods registry (`src/mods.c`), galaxy atlas (`src/galaxy`, `tools/fe2GalaxyViewer`), ship builder (`tools/fe2ShipBuilder`), Android/web builds (`tools/androidBuild`, `tools/build-fe2-web.bat`).

Key RE facts: object loop `L44480`, `L390f2` projects one object (shrinks far objects by 2^e, so raw `|pos|` is not true distance), `L39426` draws game-data objects (hangar, galaxy map via `gamedata2`). Model id at obj+90 is a byte offset (index = /2; sky = model 88). Known game variables: `fe2/NOTES.md`.

## Shared folder (`~/projects/shared`)

Reusable assets and feature specs shared with the user's other game ports and VR projects (F-117A, F-19, Thief2VR, ...); read its `README.md`. `assets/` holds our own models/code (never original game data), `specs/` feature specs to carry between projects. A project uses an asset by copying it in with a note of where it came from, and keeps the copies in step. Here: the features come from `~/projects/vibe-lib` (sibling checkout, `add_subdirectory` in CMakeLists.txt; read its `README.md` and `docs/porting.md`): `gl_hands.cpp` uses `vibe/hand.h` (hand model, `FingerTouch` press rule) and `vibe/surface.h`, implementing `specs/vr-hand-touch-panel.md` (adapted: the whole panel and the virtual screen are touchable, no button table, no beams, two-grip distance gesture). `src/manual.cpp` builds the controls pages and opens the PDF (`vibe::Manual`, `vibe::poppler` when poppler-cpp is found; `SpreadPdf` splits two-page spreads into L/R pages) and `src/gl/gl_clipboard.cpp` converts frames (vibe: cm, +z ahead; here: m, -z ahead), grabs via `vibe::ClipboardHolder` and draws, implementing `specs/cockpit-clipboard.md` (adapted: F11 not Ctrl+F11, the page kept in glfrontier.cfg `manual_page`, the `manual` layer in `gamepad.c` with VR names from the virtual-pad mapping). Not adopted yet: `vibe::PanelButtons` (touches click the console directly).

## Testing

`../devtools/README.md` documents the test harness: `t.sh` drives a **separate** instance by PID (`BIN=` picks the binary — use a scratch build dir, not `build-original`, while the user may be playing), plus `tospace.sh`/`climb.sh`/`space.sh` flight scripts, `ot.py` fake OpenTrack, `vpad.py` virtual pad, Monado qwerty for VR, and model dump tools in `devtools/models`. Before chasing a visual "bug" in native mode, compare against `GLF_NATIVE_SCENE=0` at the same window size.

## Working conventions (from the user)

- Don't modernise the codebase; match surrounding style (`.clang-format`).
- Commit only when asked; each new feature on its own branch.
- The user tests visuals in-game faster than automated screenshots — build into `build-original` and let them look rather than burning effort on self-testing.
