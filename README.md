# Frontier Elite Vibes

[![Frontier Elite Vibes on YouTube](https://img.youtube.com/vi/EPx4313Y_TY/hqdefault.jpg)](https://www.youtube.com/watch?v=EPx4313Y_TY)

> **Frontier Elite Vibes is a fork of [GLFrontier Extended](https://github.com/BrettWilsonDev/glfrontier-extended) by Brett Wilson**, which descends from Tom Morton's GLFrontier (see Acknowledgments). It adds a 3D cockpit view (the world drawn all round you, planets and haze in 3D), OpenTrack head tracking, VR headsets (OpenXR), gamepad support and sideways/vertical thrust. Everything else, and the description below, is Brett Wilson's work.

A fork of GLFrontier (OpenGL Frontier Elite 2) that modernizes the build system, adds cross-platform support (including WebAssembly and Android) and mods the game itself.

## Game Controls

* Ctrl-F11	- Toggle fullscreen.
* Ctrl-E	- Toggle the OpenGL renderer / the game's original software rendered look.
* Ctrl-M	- Toggle mouse grabbing.
* Ctrl-Q	- Quit.
* Ctrl-F	- Toggle the in-game menu (settings, cheats, mods, debug).
* Ctrl-V	- Toggle the free camera (in the flight view).
* F	        - Toggles fps readout.
* Ctrl-K	- Toggle the 3D cockpit view.
* Ctrl-C	- Recentre head tracking and the VR headset.

### Cockpit view, head tracking, VR and gamepads

In the front flight view the game is shown from a seat in the ship: the world fills the window, the game's HUD is drawn at infinity so it stays on what it marks when you look around, and the control panel and scanner float below the view (they still take mouse clicks). Ctrl-K or the SETTINGS page turns it off and sets the field of view. Other views and screens keep the classic layout.

* **Head tracking:** OpenTrack with the output set to "UDP over network", host 127.0.0.1, port 4242. Rotation turns the view, position moves your head in the cockpit. Recentre with Ctrl-C (or OpenTrack's own centre key).
* **Gamepads:** any controller SDL knows (buttons named by their Xbox positions). The defaults:
  * Left stick - thrust sideways and up / down (strafing).
  * Right stick - pitch and roll; hold RB to yaw instead of roll.
  * LB / LT - speed up / down (or main / retro thrust when held, with the engines off).
  * RT - fire. B - flight view (front, rear, turrets, external). Y - cockpit view on / off.
  * D-pad up / down - zoom in / out. On the galaxy map the sticks scroll it. Start - pause. Right stick click - recentre head tracking / VR.
  * Left stick click - a mouse pointer for the control panel and menus: the right stick moves it (and stops steering meanwhile), click the left stick again to click there and put it away. Works in the cockpit, the classic view and VR. (A `gamepad.cfg` from before this change keeps its old bindings.)
  * Hold Back for the console's F1-F10: F1-F4 on the D-pad (left, up, right, down), F5 on left stick click, F6-F9 on X, Y, B, A, F10 on Start.
  * Hold RB for time acceleration: D-pad down is normal time, left / up / right faster, Y fastest.

  The bindings are in `gamepad.cfg` beside the saves, written on the first run with the format explained at the top; edit it to change them (delete it to get the defaults back).
* **VR headsets:** any headset with an OpenXR runtime (SteamVR, Oculus / Meta, Windows Mixed Reality, Monado on Linux). Start the runtime, then run with `--vr` or tick "Use the headset" on the SETTINGS page (remembered in `glfrontier.cfg`). In the cockpit the world is drawn for each eye with the headset's lens and pose, the control panel floats below in stereo; everything else (menus, maps, other views) shows on a big screen floating ahead. The game's mouse pointer is drawn into the picture, since the window's can't be seen; the window keeps showing the game as usual, so the mouse works on it as before. Ctrl-C recentres. `vr_depth` in `glfrontier.cfg` scales the world's stereo depth (percent, 100 = life size). The resolution slider (percent of what the runtime recommends) applies when VR next starts: lower it if the frame rate drops. On Windows the game draws with OpenGL and hands the frames to the runtime through Direct3D 11 (`WGL_NV_DX_interop2`, supported by NVIDIA, AMD and Intel drivers); on a laptop, set GLFrontier to the high-performance GPU in Windows graphics settings. The headset gets a new frame each time the game draws one (the runtime's reprojection smooths head turning in between).
* **VR motion controllers:** each controller is a laser pointer. Where the beam meets the control panel, the HUD (in the cockpit) or the floating screen (everywhere else) is the game's mouse pointer. Pull the trigger with the beam on the panel or a screen to click there; pointed out into space the triggers are the gamepad's (right fires), and with the grip held a trigger clicks anywhere: to target a ship, point the right controller at it, hold the grip (the beam turns amber) and pull the trigger (the left grip is speed up). The controller you last clicked with points; the other's beam is dimmed. Once a beam moves it has the pointer until the real mouse moves.
  The sticks and buttons work as an Xbox pad through the same `gamepad.cfg` bindings: left hand = the pad's left side (thumbstick = left stick, trigger = LT, grip = LB, trackpad click = d-pad by where you press, menu = Back), right hand = its right side (thumbstick = right stick, trigger = RT, grip = RB, trackpad click = Y / A / X / B at top / bottom / left / right, menu = Start). Touch and Index face buttons are X / Y (left) and A / B (right). The gamepad's pointer button does nothing while motion controllers are on. On the galaxy map the thumbsticks scroll it (as the arrow keys do) instead of flying; the left trackpad's top / bottom zoom in / out.

### Free camera

Pauses the game and lets you fly a camera around, e.g. to look at your ship. Start it with Ctrl-V or the FREE CAMERA button in the emulator menu (the cog, also in the touch dropdown).

* W/S, A/D	- Forward / back, left / right.
* R/F	- Up / down (or Page Up / Page Down).
* Arrow keys or mouse drag	- Look.
* Q/E	- Roll.
* +/- or mouse wheel	- Speed (each step doubles or halves it), Shift for 8x.
* H	- Hide / show the help box.
* Esc or Ctrl-V	- Back to the game.

On touch screens: drag on the left half to move (a thumbstick that starts under your finger), drag on the right half to look, and use the on-screen buttons for up / down, roll, speed, help and exit.

## Inherited from GLFrontier Extended (Brett Wilson)

Everything in this section, including the Android and WebAssembly builds and the FE2 Ship Builder, is Brett Wilson's work in GLFrontier Extended, not this fork's. The Android and WebAssembly builds have not been built or tested since this fork's changes (cockpit, head tracking, VR, render thread), so they may need fixes.

* Added native Android build (APK; no external emulator required)
* Added WebAssembly (Wasm) build via Emscripten for browser-based play at full speed
* Rewritten OpenGL renderer: batched drawing, planets drawn from the game's own data (surface features, atmosphere haze), much faster on weak machines
* Touchscreen support with on-screen controls that follow the game's current screen and view
* In-game menu (Dear ImGui) styled after the game's own interface
* Cheats: cash (presets and a slider), cargo, elite rating, military ranks, clean criminal record, and for your ship: drive, lasers, shields, missiles, equipment, refuel and repair
* Mods to the game code (see below):
  * Auto Field-Maintenance Unit: equipment sold at every station; for a tonne of hydrogen it repairs the hull and all damaged equipment and gives the ship a deluxe service (drive and equipment), anywhere, from the comms panel
  * Free camera
  * Custom ship models: new looks for existing ships, and new ship types with their own name, stats, collision, shipyards and traffic (see FE2 Ship Builder below)
  * Option to leave mod data out of saved games, so they load in the unmodded game
* Migrated build system to CMake
* Supports GCC, Clang, and MSVC

## Dependencies
Automatically managed by CMake or included in the `vendor` directory.

Required: 
* GCC or Clang or MSVC compilers
* CMake

Optional:
* emscripten sdk (only for wasm builds)
* Python 3 (only for android builds, standard library only; the JDK, Android SDK and NDK are downloaded if missing)
* Python 3 (only for changing the game code, see below)

## How to Build

1. Download this repository
2. Create a build directory: `mkdir build`
3. Change into the build directory: `cd build`
4. Run CMake: `cmake -DCMAKE_BUILD_TYPE=Release ..`
5. Build the project: `cmake --build . --config Release`

### Original (unmodded) game

The modded game code is the default. To build the original Frontier: Elite 2 code instead, with none of this fork's mods, turn `GLF_MODDED_FE2` off. Use its own build directory so both can be built and run side by side:

```
cmake -S . -B build-original -DCMAKE_BUILD_TYPE=Release -DGLF_MODDED_FE2=OFF
cmake --build build-original --config Release
```

This build runs `fe2/fe2.s` (via `fe2/fe2.s.c`, `fe2/fe2_orig_bin.h` and `fe2/fe2_orig_labels.h`). Host code that belongs to a mod is compiled out (`#if FE2_USE_MODDED`): no free camera, no AFMU, and the MODS page says so. Cheats, the GL renderer and everything else work the same, and the DEBUG page shows the build tag `ORIGINAL`.

Saves from the modded game can hold mod state the original game doesn't know about. To take a save across, turn on MODS > "Saves loadable by the unmodded game" in the modded build before saving.

### Android

*Inherited from GLFrontier Extended; not built or tested in this fork.*

```
tools\build-android.bat          # -> build-android/GLFrontier.apk
tools\build-android.bat --run    # also install and start it (adb, USB debugging on)
```

On Linux / macOS: `python3 tools/androidBuild/build_android.py`, same options. Python 3 is all it needs (standard library only, see `requirements.txt`).

No Gradle: the script builds the APK with the SDK's own tools (CMake + NDK, javac + d8, aapt2, apksigner). It uses the JDK (17+) and Android SDK it finds (`JAVA_HOME`, `ANDROID_HOME`); anything missing is downloaded once into `%LOCALAPPDATA%\glfrontier-android` (`~/.cache/glfrontier-android` elsewhere, or `GLF_ANDROID_CACHE`), outside the repository. The repository only holds `tools/androidBuild/`: a manifest, an icon, a small activity (forces landscape) and the script.

* `--variant original` builds the unmodded game (`GLFrontier-original.apk`).
* `--abi arm64-v8a x86_64` picks the CPUs to include (default arm64-v8a, every current phone; add x86_64 for the emulator). Anything but the default gets the ABIs in its name, e.g. `GLFrontier-x86_64.apk`, so `GLFrontier.apk` is always the one for phones.
* Game data is compiled into the library, so the APK is all there is; saves go to the app's internal storage.
* The APK is signed with the debug key (`~/.android/debug.keystore`, or one made in the cache folder).

## Icon

`tools/icon/make_icon.py` draws the game's icon (a ringed gas giant, after the Frontier: Elite II logo) and writes every copy the builds use: `src/glfrontier.ico` for the Windows exe and its window (`src/glfrontier.rc`), the Android launcher icons in `tools/androidBuild/res`, and the web favicon, inlined in `vendor/minshell.html`. The results are committed; run it (`pip install -r requirements.txt` first) only to change the icon.

## Modding the Game Code

The game is Frontier: Elite 2's 68k code, disassembled by Tom Morton, in `fe2/fe2_modded.s`. It is assembled by `tools/as68k` into C (`fe2/fe2_modded.s.c`), which is what actually runs.

1. Build as68k once: `tools/as68k/build_as68k.bat` (or `make` in `tools/as68k`)
2. Edit `fe2/fe2_modded.s`
3. Run `python tools/build_fe2.py` to regenerate `fe2/fe2_modded.s.c`, `fe2/fe2_bin.h` and `fe2/fe2_labels.h` (`--variant original` does the same for `fe2/fe2.s`, `--variant all` both)
4. Build the game as above

Host code that uses a mod's labels (`FE2_L...`) must go inside `#if FE2_USE_MODDED`, with a fallback for the original build, or that build will not compile.

The header of `fe2/fe2_modded.s` explains the assembler rules, how the game is organised and how mods hook in. In short:
* Nothing in the middle of the file may change size (saved games and the host code hold addresses): existing code is changed with same-size `jmp` hooks, and new code and data go at the end of the file.
* Host code finds the game's labels through `fe2/fe2_labels.h`, never hard coded addresses.
* Big features are best written in C (`src/`) and called from the game with `hcall`.
* Every mod is listed in `src/mods.c`, with what it stores in saved games and how to leave that out.

The web build links prebuilt copies of the game code (`fe2/web`); rebuild them with `tools/build-fe2-web.bat` after changing it. Known game variables are listed in `fe2/NOTES.md`.

## FE2 Ship Builder (custom ship models)

`tools/fe2ShipBuilder` (by Brett Wilson, from GLFrontier Extended) is a ship editor: build a ship's model, paint it, give it collision, stats, a name and the shipyards and traffic it shows up in, then put it in the game. It is its own CMake project; build it with `tools/fe2ShipBuilder/build_fe2ShipBuilder.bat`. `FE2ShipBuilder.exe` is one static file with the game binary compiled in, so it runs on any machine.

* Ships are saved as `tools/fe2ShipBuilder/custom_ships/NNN.fe2m` (NNN = model number; new ship types count up from 240, big ships' extra parts down from 511). Every file there is compiled into GLFrontier.exe on the next build.
* SAVE THE GAME in the studio packs the ships straight into an existing GLFrontier.exe instead, no rebuild needed.
* A `custom_ships` folder beside the game overrides both, for quick tests.
* OBJ in and out for Blender. Command line: `FE2ShipBuilder --build model.obj BASE INDEX out.fe2m [name]`, `FE2ShipBuilder --save-game GLFrontier.exe ship.fe2m...`.
* The `.fe2m` format is described in `src/custom_ships.h`.

## Galaxy Atlas (galaxy map)

The game's galaxy map without its limits: zoom from the whole galaxy down to one star, drag across thousands of sectors, search every system in the galaxy by name, and see every info screen the galaxy map has for a system at once (system data, trade, the planets and starports with their details, the sector's other systems). Open it in the game from the emulator menu (GALAXY ATLAS; the game pauses while it is open), or run it on its own: build `tools/fe2GalaxyViewer/build_fe2GalaxyViewer.bat`, and `FE2GalaxyViewer.exe` is one static file.

Its data is one for one with the game's because it comes from the game itself: the atlas calls the galaxy map's own routines and captures the text the game's info screens print. In the game it calls into the running game and puts the machine back exactly as it was afterwards. The code is shared: `src/galaxy`, see `tools/fe2GalaxyViewer/README.md`.

* Drag to move, mouse wheel to zoom, click a star to select it, double-click to zoom in on it, right-click to measure distances from it. Ctrl+F finds, Home goes to where you are (Sol in the viewer).
* Find any system in the galaxy by name (nearest first); pick one to fly there.
* H (or View > Where people live) tints the map where systems have starports, cyan with orbital stations, green with surface ports only, at any zoom. The survey behind it is saved in `FE2Galaxy.cache` (next to the viewer, or with the game's saves).
* Planets tab: the game's own system view (click the icons, as in the game), an orbit view (drag, zoom, double-click a planet for its moons and stations) and a list.
* Viewer command line: `FE2GalaxyViewer --dump X Y [N]` prints sector X,Y (Sol = 0,0) and system N's screens, `--selftest` checks it over the whole galaxy (and that a call into a running game changes nothing), `--smoke` runs a scripted tour of the window.

## TODO

* more mods

## Acknowledgments
* Frontier: Elite II (1993) was written by David Braben, with music by David Lowe, and published by GameTek. It is the sequel to Elite (1984) by David Braben and Ian Bell
* Brett Wilson, author of [GLFrontier Extended](https://github.com/BrettWilsonDev/glfrontier-extended), which this cockpit fork is based on
* Tom Morton original author of GLFrontier Wayback machine archive [Tom Morton - GLFrontier](https://web.archive.org/web/20171014043201/http://tom.noflag.org.uk/glfrontier.html)
* This project is forked from: [Pcercuei's Copy of GLFrontier](https://github.com/pcercuei/glfrontier)
* Incorporates additional code from: [GLFrontier-win32](https://github.com/Kochise/GLFrontier-win32.git)
* sdl2 for windowing and input: [SDL2](https://github.com/libsdl-org/SDL/tree/SDL2)
* physFs for file system (loading and saving save game): [physFs](https://github.com/icculus/physfs)
* minivorbis for audio: [MiniVorbis](https://github.com/edubart/minivorbis)
* Dear ImGui for the in-game menu: [Dear ImGui](https://github.com/ocornut/imgui)
* glad for loading opengl functions: [glad](https://github.com/Dav1dde/glad)
* Rip of the GLU tesselator into a standalone static library: [glutess](https://github.com/mlabbe/glutess)

![GLFrontier img](https://brettwilsondev.github.io/glfrontier-extended/imgs/glfe2screenshot1.png "GLFrontier")
![GLFrontier menu img](https://brettwilsondev.github.io/glfrontier-extended/imgs/glfrontier_menu.png "GLFrontier Emulator Menu")
![GLFrontier cheat img](https://brettwilsondev.github.io/glfrontier-extended/imgs/glfrontier_cheats.png "GLFrontier Cheat Menu")
![GLFrontier ship img](https://brettwilsondev.github.io/glfrontier-extended/imgs/glfrontier_ship.png "GLFrontier Ship Outside View")
![GLFrontier touch controls img](https://brettwilsondev.github.io/glfrontier-extended/imgs/glfrontier_touch.png "GLFrontier Touch Controls")
