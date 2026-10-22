# Standalone VR (Meta Quest 2) plan

Goal: run GLFrontier on a Quest 2 (and similar Android OpenXR headsets) with no PC.
Written 2026-10-22 from a skim of the build files; nothing here has been built or measured on Android yet.

## Why it should work

- The game is not emulated at runtime. The 68k code is translated to C ahead of time
  (`fe2/fe2.s.c`, `fe2/fe2_modded.s.c`) and the game binary is compiled in (`fe2/fe2_bin.h`,
  `fe2/fe2_orig_bin.h`). No game files need to be supplied by the user.
- The graphics are 1990s low-poly, so drawing them twice for stereo is cheap for the Quest 2 GPU
  (XR2 Gen 1, target 72 Hz).
- `CMakeLists.txt` already has an Android/GLES3 path (`if(ANDROID)`) and `tools/androidBuild/`
  builds an APK without Gradle (`python3 tools/androidBuild/build_android.py`, `--run` to install
  with adb). It came from upstream GLFrontier Extended.
- Desktop VR already exists: `src/vr.cpp` (OpenXR, loader built in), plus hand tracking, cockpit
  screens and head tracking. The Android work is mainly about getting those running there.

## Known gaps

- The README says the Android build has not been built or tested since our changes (cockpit,
  head tracking, VR, render thread), so expect compile or runtime fixes.
- `CMakeLists.txt` sets `GLF_VR_DEFAULT OFF` on Android and Web. `vr.cpp` is written for desktop
  bindings (D3D11 on Windows, GLX on Linux).
- Unknown: how much CPU the render thread and the software-rendered ship cameras use on the XR2.

## Steps

### 1. Stock Android build (2D, no VR)
- Get `build_android.py` producing an APK again. It downloads the JDK, SDK and NDK into
  `~/.cache/glfrontier-android` if they are missing (override with `GLF_ANDROID_CACHE`).
- Fix whatever has rotted: the render thread, cockpit, head-tracking code and `host_screen.c` / `src/gl`
  may assume desktop GL (glad core profile) in places.
- Run on the Quest as a normal 2D Android app (it opens in a window). Measure:
  frame time, CPU use of the render thread, thermals after 15+ minutes.
- This step shows how much performance headroom there is before any OpenXR work.

### 2. OpenXR on Android (`src/vr.cpp`)
- Instance creation: `XR_KHR_android_create_instance`, with `XrInstanceCreateInfoAndroidKHR`
  (JavaVM and activity). Call `xrInitializeLoaderKHR` first.
- Graphics: `XR_KHR_opengles_enable` (GLES3). Use the EGL display/config/context that the
  app created; the swapchain images are GL textures.
- Loader: link the OpenXR loader for Android (the Khronos loader works; Meta's loader also exists).
  Check whether the statically built loader in `CMakeLists.txt` is usable on Android or whether
  the Meta one is needed.
- Lifecycle: handle pause/resume and `XrSessionState` changes (the Android activity must
  begin/end the session on focus changes). SDL on Android makes this fiddly; a native-activity
  entry point may be simpler.
- Manifest: add the OpenXR intent category and the `com.oculus.supportedDevices` metadata so it
  launches as an immersive app. Add the hand-tracking permission/feature if used.
- Keep the desktop D3D11/GLX path behind `GLF_VR` as it is; add the Android path as a separate
  branch in `vr.cpp`.

### 3. Input
- Controllers: map thumbsticks, triggers, grips and buttons to the existing gamepad/input layer.
- Hand tracking: `XR_EXT_hand_tracking` should feed the existing vr-hands code.
- Check that grabbing and moving the cockpit screens, and touching the cockpit touchscreen, work.

### 4. Performance and comfort
- Render scale, fixed foveation (`XR_FB_foveation`), possibly multiview (`OVR_multiview2`) if
  per-eye draw cost is too high.
- Make sure the render thread, audio and the software ship cameras do not stall the frame loop.
  Consider lowering ship-camera resolution or update rate on Android.
- Keep 72 Hz. Test long sessions for thermal throttling.

## Rough effort

- Step 1: a day or two, depending on how much has rotted.
- Step 2: about a week (most of the risk is here).
- Step 3: a few days.
- Step 4: a few days, open-ended.
- Total: roughly 2-4 weeks of focused work to a playable sideloaded build.

## Risks

- The Android build may need more repair than expected.
- SDL's Android activity may not fit OpenXR's lifecycle well.
- Render thread or software cameras may be too heavy for the XR2's CPU.
- The Meta Store is not realistic (the game is Frontier); distribution would be sideloading
  (APK via adb or SideQuest).

## Cheapest first check

Do step 1 only, then run the 2D APK on the Quest. If it holds a steady frame rate there, the
rest is plumbing.
