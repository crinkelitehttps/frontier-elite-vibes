# Atmosphere strobing on ascent, and the sky stuck after loading

Reports (user): (1) climbing out of an atmosphere, in the band of altitude between "air" and "space", the atmosphere
strobes on and off, quite jarring. (2) After loading a game the sky can stay stuck on the atmosphere's colour and
fixes itself after a short time.

Method: reading `src/gl/gl_atmos.c`, `gl_screen.c`, `gl_scene.c`, `gl_planet.c`, `gl_native.cpp` and the 68k planet
code. **Nothing was run and no code was changed.** Facts are **[code]**; causes not proven by a run are
**[hypothesis]**, with a test for each at the end. Background on how the stars are gated is in
`docs/background-stars.md`.

## 1. The pieces involved

The game makes the haze only in some frames and only for some views, so the host keeps what it saw and re-publishes
it. Four separate pieces of state, all in `gl_atmos.c` unless noted. **[code]**

| state | what | kept for |
|---|---|---|
| `held` (AtmosBuf) | the last frame that made bands: bands, planets, **its own `sky`/`sky_air`** | `HELD_FRAMES` = 120 publishes, or fewer (see `bare`) |
| `held_age`, `bare` | age of `held`; frames in a row with the planet's horizon in view and no bands | `bare` limit `BARE_FRAMES` = 8 |
| function statics `sky`, `sky_air`, `sky_age` in `atmos_publish` | the sky colour and the "in air" (stars hidden) flag, last frame that set them | `HELD_FRAMES` = 120, **counted only in some paths** |
| `rec.sky` | this frame's, set in `atmos_place_bands` when palette colour 14 differs from its default (`L3da2e` ran) | one frame |

What reaches the screen is `show` (= `pub`) from `atmos_publish`:

- `pub.sky` (>= 0) makes the window clear colour the sky colour, only if `bgcol == 14` or a screen is up
  (`gl_screen.c:893-899`).
- `pub.sky >= 0 && pub.sky_air` hides the stars (`atmos_sky_hides_stars`, `native_draw_sky`).
- `pub.bands` are drawn as shells round the planet (`draw_bands`).

`atmos_publish` runs from `scene_publish` (a normal world frame) and `scene_publish_world`, **not** for a screen's own
frame (`gl_scene.c:350`, `scene_publish(screen=true)` skips it).

## 2. Strobing on ascent

Where it happens: the game stops making bands and stops setting colour 14 gradually, and not the same in every
frame or pass. The game's own conditions **[code]**:

- Bands are built only if the planet routine reaches the band code, and skipped if `118(a3) <= $ff4e` (-178) at
  `fe2.s:17557` (`l3da24`).
- `L3da2e` (sky colour + "no stars" flag) runs only if the planet's projected size `>= $b2` (`fe2.s:16942`), has an
  atmosphere record, `206(a3) >= 5` and the sun term is on the lit side (see the stars doc).
- In the cockpit the game camera's pass makes bands for some directions and not others, and capture passes
  towards the planet come only "every few frames" (comments in `atmos_publish`, `atmos_place_bands`).

So near the limit the 68k side flips frame to frame, and the host's hold-over is what decides whether the player
sees that. Three problems in the hold-over, in order of how sure I am:

### 2.1 Held frames publish a different sky than fresh frames (most likely) [hypothesis, strong reading of code]

`atmos_publish` has two kinds of frame:

1. **Fresh** (`rec.n_bands > 0`): `held = rec`, then `pub = rec`, then the sky is *patched* from the static `sky`
   if `rec.sky < 0` (`atmos_publish`, the `sky_age < HELD_FRAMES` branch).
2. **Held** (`rec.n_bands == 0`, inside the window): `pub = held`, `return`. **The static `sky` is not consulted.**
   `held.sky` is whatever the *fresh frame that filled `held`* had in `rec.sky`, which is -1 when that frame made
   bands but `L3da2e` did not set colour 14 (the `set && (ahead || !rec.sky_ahead)` test in `atmos_place_bands`).

In the altitude band where some fresh frames set the sky colour and some do not, `held` and the patched `pub` of the
fresh frame disagree: fresh frames publish the (kept) atmosphere sky colour and stars hidden; the held frames after
a no-sky fresh frame publish `sky = -1`: black space and stars visible. Since in the cockpit most frames are
the held kind (capture passes with bands come only every few frames), the screen alternates between the two on the
rhythm of those passes. That is a full-screen colour strobe, the sort that reads as the atmosphere flashing on and
off, and stars popping with it (they travel with `sky_air`).

The wrong detail: `held` carries a copy of `sky`/`sky_air`, but the intention (comment: "the sky's colour stays
between the frames that make bands") is that the static copy is the truth.

### 2.2 Bands hysteresis: held up to 120 frames, dropped after 8 bare frames, restored on the next fresh frame [hypothesis]

Where the game makes bands in, say, one capture pass in ten, the host shows them for up to 120 publishes after the
last fresh frame, but `bare` counts only frames with a capture pass whose horizon is in view and no bands, and at
8 of them `held_age = HELD_FRAMES` drops the bands at once (`atmos_publish` third branch). The next fresh frame
brings them back. A rim that the game makes on 1 in 10 passes can therefore cycle: on, 8 bare frames, off, one
fresh frame, on again. Because `bare` is reset by every fresh frame but a gap can be longer than 8 bare frames, this
looks like a slower on/off flash (a few Hz). The same hysteresis works for the sky only through 2.1's path.

### 2.3 Held bands need the planet to be drawn this frame [hypothesis, minor]

In `draw_bands`, a band of a *held* frame (`show.held`) is skipped (`continue`) if `planet_native_centre(radius)`
finds no drawn planet of that radius this frame (`gl_atmos.c:353-356`), and the match is on radius within 0.1%.
If the planet is for a frame not in `drawn_planets` (a stand-in has a different radius, or is not in the list) the
bands vanish for that frame only. This only matters if 2.1/2.2 are not the whole story.

### 2.4 Things that are **not** the cause

- The star gate's one frame lag in the 68k code (`docs/background-stars.md` section 2): it is invisible in native mode,
  which uses the published `sky_air`.
- The 2D draw (`draw_soft_node`), which is bypassed whenever the 3D shells are used.

## 3. Sky stuck on the atmosphere colour after a load

Cause **[code, strong]**: all the hold-over state is **static and is never cleared** by anything but time, and the
time only passes in world frames.

- `soft_nodes_reset` (called from every `scene_reset`) clears `rec` only. `held`, `pub`, `show`, `held_age`, `bare`,
  and the statics `sky`, `sky_air`, `sky_age` are not reset by a load, a new game, a system change or a jump
  (`cheat_enter_system`). Nothing in `state_file.c` touches them.
- `atmos_publish` is **not called for screen frames** (`scene_publish(screen=true)`; menus, the load list, the
  cockpit screens). So while the load menu is up, `held_age` and `sky_age` do not advance at all.
- The age counters advance on different paths: `held_age` only in the held branch, `sky_age` only on frames that
  reach the end of the function. So after a load in space the sequence is:
  1. For the remaining `120 - held_age` publishes the held branch runs and publishes `held.sky` and `held.sky_air`
     (the colour and star flag of the atmosphere the player was last in). The bands themselves are skipped by
     `draw_bands` (the old planet is not drawn), but the **sky colour, the clear colour and the hidden stars** are
     published. This is the "stuck on the atmosphere's colour".
  2. Then `held_age = 120`, and for up to another 120 publishes the `sky_age` branch publishes the static `sky`.
  3. Only then does the sky go back to the game's own colour.

  At 50-60 published frames a second that is 2 to 4 seconds of wrong sky in the worst case, and none if the player was
  already in space with `held.sky == -1` before opening the load menu: this matches "may stay stuck" and "rectifies
  itself after a short time". **[hypothesis on the timing, code for the mechanism]**
- The wrong colour also only shows where the clear colour is replaced by the sky colour: cockpit, and
  `bgcol == 14` or a screen (`gl_screen.c:897`). After the load the game's colour 14 in memory is the loaded
  state's, so the condition is usually met.

## 4. Fixes

**Applied (2026-11-09, built in `build-modded`, not tested in game):** fix 1 (one source for the sky: `atmos_publish` now keeps `kept_sky`/`kept_sky_air`/`kept_sky_age` apart from `held`, ages them on every publish and stamps them on every published frame, fresh or held) and fix 2 (`atmos_reset_hold()`, called from `Call_GameStart` in `host_files.c` and after `cheat_enter_system`). Not applied: 3, 4, 5 (the bands' own hysteresis, 2.2, is unchanged; if the bands still blink after this, that is the next thing).

Original list:

Listed so the next step is a decision, not a design.

1. **One source for the sky.** In `atmos_publish`, make every published frame (fresh or held) take `sky` and
   `sky_air` from the static copy, not from `held`; drop `sky`/`sky_air` from what `held` stores. This removes the
   2.1 mismatch and also leaves 3 as the only hold-over problem.
2. **Reset on discontinuities.** A function `atmos_reset_hold()` (clear `held`, `held_age = HELD_FRAMES`, `bare`,
   `sky = -1`, `sky_age = HELD_FRAMES`, and `pub`/`show`), called when the game state is replaced: load, new
   commander, `cheat_enter_system`, and probably any jump/teleport that changes the frame's planet.
3. **Age by something real.** Key the held sky to the planet it came from (radius or object address in
   `held.planets`) and drop it as soon as no planet of that radius is in the scene, rather than counting frames
   that do not pass in menus.
4. **Hysteresis in the colour, not just the hold.** Optionally fade the clear colour and star visibility over a
   few frames so a single odd frame at the threshold cannot flash.
5. **Bands (2.2):** count `bare` in time (or in capture passes with the planet's horizon in view) over a longer
   window, so a rim made on 1 pass in 10 is not dropped after 8 bare ones.

Each fix touches only `gl_atmos.c` (no 68k changes), so both build variants are unaffected.

## 5. How to confirm before changing anything

- **Strobe:** log one line per `atmos_publish`: `fresh|held|dropped`, `rec.n_bands`, `rec.sky`, `rec.sky_air`,
  `pub.sky`, `pub.sky_air`, `held_age`, `bare`, and `horizon_puts`; climb with the scripts in `devtools` over Earth
  from 2 km to 80 km. Hypothesis 2.1 predicts `pub.sky` flipping between a colour and -1 on `fresh`/`held`
  frames; 2.2 predicts `bare` reaching 8 and `dropped` lines alternating with `fresh`. A save made in the band
  (Ctrl-S, then the `savetool` scenario files) gives a repeatable test. Use `--nosound` and a test instance, not the
  user's game.
- **Load:** fly low over a planet, open the load menu, load a space save, log the same line. Hypothesis 3 predicts
  `held` lines for `120 - held_age` frames with `pub.sky >= 0`, then `sky_age` ones.
- A/B: `GLF_NATIVE_SCENE=0` shows whether the game's own picture does it (it should not strobe; its sky colour
  comes from colour 14 each frame).

## 6. Not checked

- How often a capture pass towards the planet comes in practice at the ascent altitudes (I read "every few
  frames" from the comments, not a measurement), which sets how visible 2.1 is.
- Whether the strobe also happens with `cockpit` off (classic view). In that view there is one pass per frame, so
  the held branch is rarer and 2.1 should show much less; if it still strobes, 2.2 or the 68k conditions are the
  cause.
- The meaning of `118(a3)` (`$ff4e` test) and `206(a3)` in the game's code was not worked out.
