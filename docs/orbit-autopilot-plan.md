# Orbit autopilot: plan (MAP screen target orbit)

Status 2026-11-12 (updated after the user's answers and the pole check): plan only, no code. Follows the measuring step in `docs/orbit-autopilot.md` and the autopilot
findings in `docs/autopilot.md`. Tags: **[code]** read from the source this session, **[open]** not yet checked.

## 1. Goal (as the user set it)

- On the **SHIP screen** (an ORBIT page, section 3) the player defines a **target orbit** round a body, and it is drawn on the **MAP screen** (system map) round that body.
- Both **apsides** (periapsis and apoapsis altitude) are always set. **Inclination, node, argument of periapsis** are
  optional extras; if left alone they are chosen automatically.
- It can be defined **in flight, straight after arriving in the system**, with no need to be near the body first.
- The prospective orbit is **drawn on the existing system map**, next to the ship's current orbit line.
- The autopilot then flies the ship into that orbit.
- Bound orbits only (e < 1). Unbound / escape orbits are out of scope.

## 2. The orbit: variables

| Variable | Player sets | Default if left alone |
|---|---|---|
| Central body | by selecting it on the map (the current target, or the ship's parent frame) | the nav target if it is a planet, moon or star, else the parent frame |
| Periapsis altitude `hp` | **yes** | none, required |
| Apoapsis altitude `ha` | **yes** | none, required (equal to `hp` makes a circle) |
| Inclination `i`, 0..180 degrees (above 90 = retrograde) | optional | the plane the ship is already moving in round that body (cheapest) |
| Ascending-node longitude | optional; when unset, **whichever node (ascending or descending) is nearest the ship** | nearest node, as the user said |
| Argument of periapsis | optional, only meaningful when `ha > hp` | wherever periapsis falls with the cheapest transfer (see 5.3) |
| Direction | follows from `i`; a "retrograde" toggle is just `i := 180 - i` | prograde (the same sense as the ship's current motion round the body) |
| Position on the orbit (anomaly) | **never** | the ship joins at the transfer's end point |

Derived and shown as read-outs: `a = (hp + ha + 2R)/2`, `e = (ra - rp)/(ra + rp)` with `rp = R + hp`, `ra = R + ha`,
period `2*pi*sqrt(a^3/mu)`, speeds at the apsides, and "delta-v needed from here" (an estimate, 5.4).

**Reference plane** (what inclination and node longitude are measured against). Default **the body's equator**; an
optional toggle selects the body's orbital plane round its parent.

**The pole is known [code + measured 2026-11-12].** The surface is drawn with latitude `acos(p.y)` and longitude
`atan(p.x, p.z)` for `p` the direction in the planet's own axes (`gl_planet.c` sphere shader, `uToLocal`), and the Earth
outline points use `v = (cos lat sin lon, sin lat, cos lat cos lon)` (`gl_planet_tex.c`), so the pole is the planet's
local **+y**. In the frame the ship's position and velocity are given in, the pole is the **"up" column of the planet
object's 3x3 matrix** (words 1, 4, 7 at the object, Q15; `cheat_object_axes(obj, col)` returns it as `col[1]`). The
matrix comes from the body record's tilt (word 62, 65536 = 360 degrees). Measured on Earth (object `0x14bf8`, slot 110, Sol
start): words = (-0.921, 0.390, 0 / 0.390, 0.921, 0 / 0, 0, -1.0), so up = (0.390, 0.921, 0), 22.95 degrees from y,
Earth's 23.0. Also Mars 23.99 (record 24.00), Uranus 98.00 (98.00) and Jupiter 2.99 (3.00).

**The frame is inertial; the planet's matrix turns (measured 2026-11-13).** A ship's position and velocity do not turn
with the body (an orbit fixed in space keeps its plane vectors in the frame). What turns is the body's own matrix, in
steps of 360/4096 degrees (0.088, node readings came in multiples of it) at the body's spin, 24 h / 2^(spin-1) a turn
(Earth and Mars 15 deg/h, Jupiter 30). The pole is the axis of that turn, so it does not move; the "forward" column,
which this plan takes as longitude 0, does. A node measured from it therefore creeps by the spin for an orbit that
is not changing (+0.6 deg in 150 s on Earth, +0.8 deg in 100 s on Jupiter, inclination steady). So the plan is stored
as **vectors in the frame** (orbit normal, periapsis direction): the longitude the player types is taken from longitude 0
as it is when the plan is made, and nothing needs rotating afterwards. Verified: plan and ship 0.000 deg apart
after those waits. The page should show the node as typed; for "auto" it shows the ship's node as of now.

**Checks the page makes** (maths only; there is **no minimum altitude**, the player's risk, decided 2026-11-13):
- Both apsides present; periapsis above the body's centre (altitude above `-R`).
- `ha >= hp` (swapped if not). Apoapsis inside the body's sphere of influence (the game's SOI frames, `b5606ac`), so the
  orbit is bound and stays in the frame.
- `i` in 0..180, node and argument of periapsis in 0..360.

## 3. The interface: an ORBIT page on the SHIP screen

Revised 2026-11-13 after the user's mock-up (`~/Downloads/remove-the-light-gray.jpg`: a numeric keypad on the left,
six labelled fields on the right) and the instruction to put it on the SHIP screen, with no button.

**Where it lives [code].** The cockpit's SHIP screen (`gl_mapscreen.c`) already runs the game's ship-screen pages, cycled
by a tap on open space (or F3), and then **two host-drawn pages** after the game's last: the ship log (`EXTRA_SHIPS`)
and the debug page (`EXTRA_DEBUG`), then back to the game's first page (the cycle at `gl_mapscreen.c:1326-1331`; input
for the log in `ship_log_input`, drawing in `draw_ship_log_page`). The orbit page is a third host page, `EXTRA_ORBIT`,
put in the same cycle (game pages, ship log, **orbit**, debug, back to the first). No new button and no change to the
MAP screen's icons.

**Which body.** The body being orbited is the **current target** if it can be orbited (planet, moon, star), else the
ship's parent frame; the page names it at the top. With nothing orbitable (no target and no frame body, or a station as
target), the page says "Select a planet, moon or star on the system map" and has no fields. The target is picked on the
system map (F10 click) or the ship log as it is now.

**Layout (320x200 page, as the mock-up).** Dark green background of the map screen; text, borders and keys in the
light green of the orbit lines (palette indices to be taken from the map's own colours, as the ship log page does).

```
 ORBIT ROUND MARS                                   (title, body name)
 +---+---+---+          Periapsis (km)     [ 300      ]
 | 7 | 8 | 9 |          Apoapsis (km)      [ 300      ]
 +---+---+---+          Inclination (deg)  [ auto     ]
 | 4 | 5 | 6 |          Node longitude     [ auto     ]
 +---+---+---+          Arg of periapsis   [ auto     ]
 | 1 | 2 | 3 |          Plane              [ equator  ]
 +---+---+---+
 | 0 | <x| CLR|         Period 1h 52m  e 0.000  a 3690 km
 +---+---+---+          Vp 3.4 km/s  Va 3.4 km/s
 [ ENGAGE ]  [ CLEAR ORBIT ]                        Esc / tap outside: leave
```

- **Fields.** Tap a field to select it (it gets a highlight and the keypad edits it). Digits append, the backspace key
  removes one, CLR empties it. For the optional fields an empty field means **auto** (shown as "auto"). The mock-up has
  six fields; ours are Periapsis, Apoapsis, Inclination, Node longitude, Argument of periapsis and **Plane**
  (equator / orbital plane, a two-state toggle rather than typed). Altitudes are in km above the surface.
- **Also accepted**: the physical keyboard (digits, backspace, Tab for the next field, Enter to leave the field),
  so desktop play is not slower than the pad. The on-screen keypad is for touch, pad and VR (hands and laser).
- **Apsides**: if the player types an apoapsis below the periapsis the two are swapped for the orbit (the fields keep
  what was typed, a note says "swapped"). A circle is just equal values.
- **Read-outs** (not editable, shown under the fields): period, eccentricity, semi-major axis, speeds at periapsis and
  apoapsis, and the estimated delta-v (5.4).
- **ENGAGE** starts the autopilot mode (section 5) on this body and this orbit. **CLEAR ORBIT** forgets the orbit
  (and removes it from the map). Both are plain buttons at the bottom.
- **No minimum altitude**: the player may set any periapsis, even one inside the atmosphere or the ground. That is
  their risk (decided 2026-11-13); the only checks are the maths ones (numbers present, periapsis above the body's
  centre, apoapsis not beyond the sphere of influence so the orbit stays bound).
- **Leaving**: the same tap on open space cycles to the next page (the debug page), as for the other host pages; F3 and
  the other screen keys work as ever. Esc also returns to the first page. Because the orbit is kept in host memory, leaving
  the page loses nothing.
- **Input plumbing**: taps arrive as for the ship log (`Pointer` per batch, `ship_log_input`-style hit tests on key and
  field rectangles). The page draws into the same 320x200 palette-index buffer as the other host pages.

**The orbit appears on the MAP screen** as soon as it is defined (not only while this page is open): the planned orbit
is drawn round the planet in the system map, next to the ship's own orbit line (section 4). The page does not need a
diagram of its own.

## 4. The prospective orbit on the system map

`map_orbit_build` already turns a conic (`ConicElems`: p, e, the periapsis direction `pe`, the in-plane direction `q`,
the radius) into 400 line points in the map's view space, using the matrix `A` it solves from the ship and the parent's
view positions, with the parts behind the planet hidden. That is exactly what a second, **planned** orbit needs.

Plan:
1. Add a function turning the wanted elements (`hp`, `ha`, `i`, node, argument of periapsis, plane choice) into a
   `ConicElems` in the **parent frame's axes** (the same axes `hud_conic_elements` uses). Needs the frame's pole (2).
2. Run the existing point loop twice: current orbit green (unchanged), planned orbit in another colour (cyan, dashed by
   skipping every other segment if the lines look alike).
3. Mark the planned **periapsis and apoapsis** with small ticks and the two **nodes**, labelled Pe/Ap like the flight HUD.
4. The map's own zoom has to show the planned orbit. The game's map is zoomed by its own code; if the planned apoapsis
   is off screen the line just runs out of view. **[open]** whether the map's zoom is controllable from the host, else
   accept clipping and show the numbers on the ORBIT page.

Because the line is drawn from the same matrix `A`, it uses the game's own map scale and costs one more pass of the
same loop. No new rendering path.

## 5. The autopilot mode (flying into the orbit)

### 5.1 Where it lives

A new AI state next to `$20` ("fly to the destination in `212`", `L75444`), in both `.s` files (`fe2.s` and
`fe2_modded.s`, per `modded-default-build`). The existing flight state `704 = $04` and the player module's AI step
(`L6199c`) run it unchanged. The wanted orbit lives in host memory (set from the ORBIT page by a hostcall), and the AI state
asks the host each look for the **wanted velocity** at the ship's current position (5.3), so the guidance maths stays in
C, not 68k assembly. This is the same split the time guard and orbit probe use.

### 5.2 Phases

1. **Approach**: use the existing `$20` / fly-to-target code until the ship is inside the body's frame and about 2-3
   radii out (or inside the SOI, whichever is smaller). Time acceleration works as it already does.
2. **Plane and apsis shaping**: a series of burns, each computed by the host (5.3).
3. **Trim**: each AI look, compare the ship's elements with the wanted ones and thrust gently to remove the error.
   Wobble of a stable orbit (e about 0.003 at high acceleration) is accepted: **stable is enough**, so the tolerance is
   loose (about 1% of the radius) and there is no active holding afterwards.
4. **Done**: when the elements are within tolerance for a few looks, switch to manual flight, engines off, message
   "In target orbit". The orbit is then stable for free (measured, `orbit-autopilot.md`).

### 5.3 Guidance (host side)

Each look the host knows the ship's position `r` and velocity `u` in the parent frame (as `orbit_probe.c` /
`orbit_calc` already read them) and the wanted orbit. It returns a **wanted velocity** `u_w` and a flag for "burn now".
The AI feeds `u_w - u` into the thrust request exactly as the fly-to code feeds its own velocity error.

The transfer (simple, robust, a few steps; real minimum-fuel planning is not needed):
- If the ship is **not on a bound orbit round the body** yet (arriving on a fast flyby): first **capture** by thrusting
  retrograde at the closest approach until bound, to the wanted apoapsis at least. This is the Hohmann-like step.
- **Plane change** at the nearest node (ascending or descending, as the user asked) on the way, combined with the apsis
  burn there when the node is close to the right place. Cheapest at the highest point of the orbit, so if the current
  apoapsis is large, do the plane change near apoapsis if it falls on a node.
- **Apsis shaping**: a burn at periapsis sets the new apoapsis, a burn at apoapsis sets the new periapsis (the usual
  two-burn transfer). With a free argument of periapsis, take whichever pair of burn points the ship reaches first.
- With a free node and inclination the plane stays as it is and only the apsides are set.
- The **burns are not impulsive**: the ship has finite thrust. The AI's thrust request is acceleration-limited, so the
  host simply keeps `u_w` for the whole burn and the ship reaches it over some seconds, centred on the burn point
  (start early by half the burn time). **Decided: try burns at accelerated time first**, and fall back to slowing to 1x around a burn (the
  time guard's step-down, `time_guard.c`) only if that does not work. Things to watch: the burn's centring (an
  accelerated frame is a long step, so start early by half the burn time measured in game seconds, and cut the
  thrust request off at the target velocity inside the AI look rather than waiting for the next one), and the
  one-frame gravity kick when the rate changes.

### 5.4 The "delta-v needed" read-out
The same guidance code, run once with no ship motion, gives the total speed change of the planned burns. This is only an
estimate and is shown on the ORBIT page, so the player can see an orbit costs nothing the ship cannot
afford. **[open]** whether fuel is limited at all: the autopilot may need to check fuel and refuse, or just run out.

### 5.5 Replaces or extends the current autopilot?
**Extends.** "Fly to vicinity of" stays exactly as it is (F7 and the map click). The orbit mode is a different
target type: the ORBIT page's **ENGAGE** button engages it, and the current autopilot keeps its own meaning for other
targets and for docking. Pressing F7 while the orbit mode runs turns it off as it does today. The new mode is also
reachable as a toggle "end in orbit" on a plain autopilot flight to a body (**[open]**, a later nicety).

## 6. Steps (each is testable on its own)

1. **Elements maths in C** (`orbit_plan.c`): wanted elements to `ConicElems`; elements of the ship to the same form;
   error terms. Unit-test through a script command like `circ`: `plan hp ha i`, logging the numbers. **[open]** the
   body's pole (section 2).
2. **Prospective line on the system map** from fixed numbers set by a script command (no UI yet). Check by eye on a
   saved game, user tests (`user-tests-in-game`).
3. **ORBIT page** on the SHIP screen: keypad, fields, read-outs, `EXTRA_ORBIT` in the page cycle. Orbit set by hostcall into the shared state.
4. **Guidance in C**: wanted velocity from (`r`, `u`, wanted orbit, phase). Test headless first (a script run, in the
   style of the `orbitlog` runs): from a flyby at Mars, from a circular 300 km orbit to an inclined elliptical one,
   at 1x, then 1000x. The logs give `pe/ap/e/i` against the target.
5. **AI state** in both `.s` files: ask the host for `u_w`, feed the thrust request, stop on arrival. Reuse the
   `Lcsys_*`-style hook layout, one hook, one hcall (next free number after `$a9`).
6. **Hold and trim** at high time acceleration; interaction with the time guard and with pirates arriving
   (the autopilot keeps flying when shot at, `autopilot.md` 3.3).
7. **Docs and the fixes list** (`docs/original-game-fixes.md` is for fixes; this is a feature, so add this plan's
   status here and a section in `autopilot.md`).

## 7. Open questions and risks

- **Pole / equator** of a body in the game's frame (2, step 1).
- **Map zoom**: whether the planned orbit can be put in view automatically (4).
- **Accelerated Jupiter orbits did not hold** in the earlier measurement (`orbit-autopilot.md`, cause unknown; may be
  the time guard or a game limit). The trim phase must work around it or the cause must be found first.
- **The one-frame gravity kick** when the time rate changes: harmless for a trim loop, but it matters for burns at accelerated time (5.3).
- **Moons and nested frames** (out of scope for now, user's decision): orbit round a moon while the ship's frame is the planet (frame switches by SOI): the
  mode must follow the frame the ship is in and re-target when the SOI changes. The `Relative-to never switches to
  station` investigation shows frame logic is a weak point; keep the first version to planets, moons and stars.
- **Atmosphere / ground**: a low periapsis hits the drag code or the surface. Not prevented (the player's risk); the
  autopilot should stop with a message if the ship is destroyed or lands, as any autopilot does.
- **Input on all platforms**: the on-screen keypad covers touch, pad and VR; the physical keyboard also works (3).

## 8. Decisions

Decided by the user: equator as the default reference plane (the pole is known, section 2); burns are tried at accelerated
time first; moon frame switching, fuel and thrust limits and wobbly orbits are not a concern for now (2026-11-12). The UI is
an ORBIT page on the SHIP screen, reached by the existing tap-on-open-space cycle, with an on-screen keypad as in the
mock-up and no button; the orbit is shown on the MAP screen; **no minimum periapsis**, the player's risk (2026-11-13).

## 9. Step 1 status (2026-11-13)

Done, uncommitted: `src/orbit_plan.c/.h` (wanted elements to an orbit in the frame's axes, the ship's elements in the
same form, plane/apsis errors, the conic for the map in `ConicElems` form, `orbit_ctx_get`) and the script command
`plan [HP HA [INC NODE ARGPE]]` (`-` leaves a value auto), in `script.c`. Built in `build-orbit` only (not
`build-modded`). Checks: internal consistency (elements recovered from states round the orbit, worst 1e-15); pole
matches the record tilt on Earth, Mars, Uranus, Jupiter; plan against a `circ` orbit stays 0.000 deg off over 100-150 s;
period and speeds agree with `2*pi*r/v` (Mars 300 km: 6755 s, 3.441 km/s).

Not yet: the orbital-plane reference (`OrbitSpec.orbital_plane` is ignored), the delta-v estimate, anything drawn.
Test recipe: `devtools/t.sh start` with `ARGS="--script FILE"` and `BIN=../build-orbit/GLFrontier`, then float and activate
the window (i3-msg, `xdotool windowactivate`), or the game idles at 0 percent CPU and never runs the script.

## 10. Step 2 status (2026-11-13)

Done, uncommitted: the planned orbit is drawn on the system map. In `gl_native.cpp`, `map_orbit_build` now builds two
lines with the same matrix and the same hidden-behind-the-planet test (the point loop is a lambda `build(conic, out)`):
`Frame::orbit` (the ship's, green, only while the ORBIT display is on, as before) and the new `Frame::plan` (cyan),
built from `orbit_plan_current()` through `orbit_plan_conic` whenever a plan is valid and its body is the body the map's
ship is relative to (`OrbitPlan::body_slot`, `radius` added for it). `map_orbit_draw` draws both. `orbit_plan.h` got
`extern "C"` guards for the C++ side.

Checked in a test instance (cockpit MAP quad, system map, Mars): the plan line is built (400 points, those behind the
planet hidden; points 0.9-1.7 position units from the centre for a 300 x 4000 km orbit, as expected) and shows round
Mars. Not checked by eye: exact overlap with the ship's own line when the plan equals the ship's orbit, and Pe/Ap/node
ticks (not drawn yet). Map zoom: the map's own icons 7 and 8 zoom in big steps; the plan is drawn to the same scale as
the ship's line, so it is only as visible as that is.

To try it in game: `mkfifo /tmp/glf.ctl; ../glfrontier.sh --control /tmp/glf.ctl`, then
`echo "plan 300 4000 60" > /tmp/glf.ctl` (periapsis km, apoapsis km, optional inclination, node, argument of periapsis;
`-` = auto), and open the MAP screen's system map (icon 2). `echo "plan 20000 20000 90" > /tmp/glf.ctl` replaces it.

## 11. Step 3 status (2026-11-13): the ORBIT page

Done, uncommitted, built into `build-modded`, `build-original` and `build-orbit`. In `gl_mapscreen.c`: `EXTRA_ORBIT`
joins the ship screen's page cycle (game pages, ship log, **orbit**, debug, first), a tap on open space or F3 turns it as
for the others. `orbit_page_input` takes taps on the keypad, the five fields and the buttons; `draw_orbit_page` draws it
with the map screen's colours (palette entries 1-3 set to its dark green 0x030, orbit-line green 0x0f0 and a dimmer
0x060). Typing makes the plan at once (`orbit_ui_apply` -> `orbit_plan_set`, which resolves it against the body the ship
is relative to and fixes what was left auto as of then), so the cyan line on the system map follows. Keyboard: digits and
keypad digits, backspace, Tab / Enter for the next field, while the page is the one shown (`mapscreen_orbit_key` called from
`Input_PressSTKey`). CLEAR ORBIT empties the fields and forgets the plan. ENGAGE is drawn dim and does nothing yet.
Read-outs when a plan exists: e, semi-major axis, period, speeds at periapsis and apoapsis.

Checked in a test instance: the page cycles in after the ship log and before the debug page; typing 300, Tab, 4000 on the
keyboard gave e 0.3333, a 5550 km, period 3h 26m, Vp 3.97, Va 1.99 km/s (the same as the `plan` command). Not checked:
taps by mouse/hand on the keys (the test window cuts off the quad), the cyan line while typing.

Limits of this step:
- The body is the one the ship is **relative to** (its frame). Defining an orbit round another body from afar (the
  selected target while still in the star's frame) is not done: it needs that body's pole and mass (easy) and its place in
  the map's view (the map matrix is solved from the ship and its frame body only, `map_orbit_build`). The page's
  title says which body it is.
- Only whole numbers are typed (no decimal point or sign); the plane is fixed at the equator (shown, not selectable).
- Typing fixes the auto values (inclination, node, argument of periapsis) as of the keystroke; the page shows them as
  `AUTO n`.

Revised after the first in-game test (2026-11-13): (1) keys are the page's only while a field has been tapped (`orbit_ui.sel`
>= 0), and only digits and backspace: Return (thrust) and Tab were being swallowed, so the ship could not be thrust with the
page up; (2) ENGAGE is lit whenever there is a plan, filled and labelled DISENGAGE while engaged (`orbit_plan_engaged`; it
flies nothing yet); (3) the page starts at a 100 km equatorial orbit (100, 100, inclination 0, rest auto), and is made
again for what is typed when the ship comes to another body's frame.

## 12. Step 4 status (2026-11-13): guidance

Done, uncommitted, in all three builds: `src/orbit_guide.c/.h` (pure maths: `orbit_guide_step`) and a test driver, script commands
`fly [A [K]]` / `flystop` (the wanted velocity applied as a limited change of the ship's velocity, A m/s^2, a GUIDE line every K
updates; engines off), run in the game's own physics.

**The guidance.** A static field, no clock and no burn scheduling: find the point of the planned orbit nearest the ship (180 samples
then a refine, with hysteresis so it does not jump between two), take the orbit's velocity there plus a pull towards the point (gain
n = mean motion, capped by the braking curve `sqrt(2 a d)`). The first version thrust straight at that velocity and dropped a low
orbit into the planet during a big plane change (speed traded for direction, cos(angle/2) dip); now the ship **turns at its own
speed**, at most 0.35 rad a step, and the speed difference is thrust separately. Done = the ship's own orbit has the planned plane
(0.5 deg), periapsis and apoapsis (1 percent of the distances) and, for an ellipse, argument of periapsis (5 deg), wherever the ship is on it.

**Results** (Mars, accel 10 m/s^2, game time):
- circular 300 km, inclination 146 deg, to 200 x 2000 km at 60 deg (a 105 deg plane change) at 1000x: done after about 6700 s,
  pe 190 / ap 1983 km, inclination 59.9, then no thrust and the orbit holds (e 0.200);
- at rest 5000 km up to the default 100 km equatorial circle: 1000x done after about 6000 s (pe 105, ap 135, e 0.004, inclination 0.1),
  10000x the same (pe 101, ap 129); holds afterwards, wobbling only by the game's own integrator error (e 0.002-0.004).
  The left-over apsis error (up to 1 percent of the distance, about 30 km at 100 km) is the tolerance, not a drift.

**Found on the way: the ship's coordinates have two regimes** (ship byte +69: 255 = in the body's frame, else 0). When it changes
(Mars: leaving about 1330 km altitude; a plan in the middle of it saw the ship's orbit turn through 135 deg in one frame, with altitude,
speed, periapsis and apoapsis unchanged) the game turns the ship's position and velocity by the body's matrix: coordinates in the system's
axes = M * coordinates in the body's own axes (M = the matrix columns right, up, forward; checked on the data to within the motion
of a frame). In regime 255 the body's own axes are the frame's, so the pole is y and longitude 0 is z; in regime 0 the matrix says (what
`orbit_ctx_get` did before; it now uses y and z in regime 255). `frame_sync` (called each update from `orbit_fly_poll`, always, not
only while flying) turns the plan with that matrix on a change, so it stays the same orbit in space; also the line on the map and
the ORBIT page's AUTO values follow. The earlier "frame is inertial" statements hold within each regime; the pole check on Earth,
Mars, Uranus and Jupiter was of the matrix, the regime-0 pole. Still not understood: why the node of a ship in an inertial orbit creeps
by the body's spin in the *matrix's* prime direction in both regimes (the plan is fixed in the coordinates and stays matched, so it
does not matter for guidance).

Not done: the real thrust (step 5: the AI state feeds the wanted velocity to its thrust request); no time-optimal burns (the plane
change is spread over the arc, not at the node); the approach from far away (the ship must start near the body); an unbound start
(a flyby) is not specially treated (it simply thrusts to the field); the maximum acceleration is a parameter, not read from the ship.

## 13. Step 5 status (2026-11-13): ENGAGE flies it (host-driven)

First in-game test (Merlin, target Aster): ENGAGE did nothing (it only set a flag), the plain autopilot flew to the vicinity,
and time acceleration then flew the ship into the planet's centre; and the ORBIT page took digit keys while the intro played.
Changes, uncommitted, in all three builds:

- **Keyboard**: the page is operated by the UI only. No key reaches it (`mapscreen_orbit_key` and its hook in `input.c` are
  gone), so the flight's keys, Return included, are never touched.
- **Remote body**: the plan is round the selected target if it is a body with gravity (`orbit_body_pick`, `orbit_ctx_body`: the
  body on its own, system axes, regime 0), else the ship's frame body; the page names it. The plan carries its `regime` and
  `orbit_plan_sync` turns it when the ship comes into the body's frame in the other regime. (The cyan map line is still drawn
  only when the ship is in the body's frame.)
- **ENGAGE** (`orbit_ap_request`; the host does the flying, `orbit_guide.c` section "the orbit autopilot"): if the ship is not in the
  body's frame it engages the game's own autopilot as F7 does (needs the target to be the plan's body, the Automatic Pilot fitted
  `158(ship) bit 5`, flight state 0; sets `212(ship)` = target, `704(a6)` = 4, `211(ship)` = `$20`, engines on). When the
  game's autopilot ends, which it does on arriving in the body's vicinity with the ship still at about 100 km/s, the guidance
  takes the ship (engines off, its velocity changed by the guidance as the `fly` driver does) and flies it into the orbit,
  braking included, then ends in orbit with the engines as they were. The button shows DISENGAGE while on, fills, and the status
  (APPROACH, GUIDING, IN ORBIT, or why not: TARGET IT, NO AUTOPILOT, NOT IN FLIGHT, CANCELLED, LEFT THE BODY) is beside CLEAR ORBIT.
  Pressing it again, or CLEAR ORBIT, stops it. The guidance counts on 200 m/s^2 (the game's autopilot arrives at 107 km/s from
  51,000 km out: about 110 m/s^2 of braking, so this is more than the ships do); this is a host-side change of velocity, not the
  ship's thrust, so there is no engine effect and no fuel use.
- **The crash with time acceleration**: the game's autopilot and the guidance decide once a frame, which at 10000x is 200 game
  seconds, enough for the ship to cross the last 20,000 km at 100 km/s (it was at 3,100 km, at speed, when the autopilot
  noticed it had arrived). Now each frame while engaged is capped to a tenth of the way to the body at the speed it closes at
  plus what a frame of braking adds (`ap_time_cap`, `time_guard_cap`, stepping the rate down by the game's own keys as the time
  guard does for ships, `cheat_body_distance_km`). Pressing time acceleration again just raises it again when it is safe.
- **Tested** (script run, Earth 3,000 km up, target the Moon, plan 100 x 100 km equatorial, rate set to 10000x again every 3 s):
  APPROACH, GUIDING at about 50,000 km, IN ORBIT; the orbit pe 105 / ap 129 km, inclination 0.04 deg; engines restored.
  Script commands for it: `target NAME`, `orbitplan HP HA [INC NODE ARGPE]`, `giveautopilot`, `engage`, `disengage`, `apstatus`.

Not done / known: the plane change is spread over the arc; a body's orbit line on the map while the ship is not in its frame;
only whole-number entry; the plane is fixed at the equator; the original game's own autopilot (F7) still flies into a planet at
high time acceleration when ENGAGE is not used.

## 14. Step 6 status (2026-11-13): the approach, from a manual flight (user-tested Merlin -> Aster)

The flyby of step 5 (one burn at the periapsis) failed in the game. The user flew the approach by hand with the path logger
(`GLF_PATH_LOG=1`, `pathlog.csv`, `orbit_probe.c` `pathlog_poll`, on in `glfrontier.sh`: ship position, velocity, nose, thrust
words, flight state, set speed and orbit elements relative to the target body, 10 rows a second). The manual approach to a
gas giant (Aster, radius 30,497 km), read from the log:

1. Leave Merlin with the nose on the velocity (prograde burns, coasting between).
2. Entering Aster's frame at 98,000 km the periapsis was inside the planet: a forward burn (nose 26-30 degrees off the
   velocity) moved it to about 1,100 km altitude, a small one at 73,000 km to about 1,800 km. The plane was already near the
   final one (8 degrees) and was not changed again.
3. The capture is a steady retro burn (reverse thrust, the nose stays prograde) from about 28,000 km altitude down to
   about 1,900 km, shedding speed over the whole inbound leg: the eccentricity fell from 6.5 to 0.25 (apoapsis 22,000 km, 1.6 times
   the radius at the end of the burn), not an instant burn at the periapsis.
4. A second short retro burn just after the periapsis lowered the apoapsis: final 880 x 2,300 km, e 0.02.

What the autopilot does now (`flyby_poll`, `orbit_guide.c`): the aiming is as before (the velocity turned, at constant speed, so
the periapsis is the planned one); added **continuous braking** along the motion, at the rate that has the speed at the
periapsis the plan's, with 30% margin, capped at 200 m/s^2; the target is **not** the plan's energy but the ellipse with the
planned periapsis and an apoapsis 30% above the ship's radius (tightening as it falls in): a ship with a circular plan's energy
cannot be farther out than the orbit, and it turned round at its apoapsis, which the periapsis test took for the periapsis and
the ship dived (first test). The periapsis burn (only when the ship is within 1.3 planned-periapsis radii) trims the rest. Else the
guidance of step 4 takes over.

**The handover** killed two tests: the ship was in a good orbit (968 km, e 0.001, planned 1000 km circular) and after IN ORBIT it
fell. The speed governor (set speed, `848(a6)`) was 0 after the game's autopilot and the engines-off flag (`705(a6)` bit 0) was
clear, so the governor braked the ship to a stop at full thrust. `ap_stop` now sets the set speed to the present one and, after
reaching the orbit, leaves the engines-off flag set (coasting, as in manual flight); any other stop restores the flag as it was.
Result: user confirmed Merlin -> Aster works and holds. See also `orbit-autopilot.md` ("Exact gravity") for the wobble of the orbit.

