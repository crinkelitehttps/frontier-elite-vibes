# The autopilot in Frontier: Elite II

Findings from reading `fe2/fe2.s` (the original game; `fe2_modded.s` has the same layout). Nothing here was measured
in a running game yet; section 10 lists the tests that would check the open points. Background is in
`docs/npc-ship-ai.md` (called "the AI doc" below), because **the player's autopilot is the NPC ship AI**, run on the
player's own ship.

Every statement is tagged:

- **[code]** read from the disassembly, not run
- **[inferred]** a conclusion from the code; could be wrong

Labels like `L6199c` are labels in `fe2/fe2.s`. `NNN(a6)` is a game global, `NNN(a0)` a field of the player's ship
object (field list in the AI doc, section 2.2). Message numbers like `$98a3` are string ids (`$86xx` are entries of the
table at `L653c8`, `$98xx` of the table at `L79280`).

See also: `docs/orbit-autopilot.md` (circular-orbit measurements, plan for an orbit mode).

## 1. Short version

- The autopilot is the equipment item **Automatic Pilot** (140 credits, 1 t, bit 5 of the ship's equipment flags
  `158(a0)`). It needs a selected navigation target (`708(a6)`). [code]
- Switching it on sets the flight state `704(a6)` to `$04` and puts the player's ship into the NPC AI state `$20`
  ("pick up the destination in `212` and fly to it") with `212(a0)` = the target. From then on the player module
  hands each AI step to the **ship module's AI step** (`100(a6)`, function 28, `L750de`), the same code that flies
  traders, police and pirates. [code]
- The flight itself is the AI's "fly to target" state: point the nose at the target, thrust along the wanted velocity
  change, with a square-root braking curve, an obstacle probe that steers round planets and stations, and a
  frame re-choice every 20 AI looks. Stick and keyboard flight controls are ignored; turrets still work. [code]
- **Starports**: the ship flies to an approach point (6 km out along an orbital station's axis, or 4 km above a ground
  port), asks for clearance every AI look until it gets it, then flies to the bay or pad and docks exactly like an NPC.
  The player gets the usual "Clearance granted" message and docking music. [code]
- **Anything else** (planet, moon, star, ship): there is no docking. The autopilot ends with "Arrived in vicinity of
  target" only when the target has become the ship's **parent frame** and the distance exponent is 23 or less
  (largest component under about 68,700 km). It then sets the set speed to 0. [code]
- **Under time acceleration** the AI's distance routine has a shortcut that **places** a ship at its aim point when it
  would arrive within about one accelerated frame. For a starport the aim point is the approach point and then the bay,
  so an accelerated autopilot docking is two jumps and a snap-dock, not a flight. Planets and ships have no aim offset,
  so they get no shortcut. [code]

## 2. Where it lives

### 2.1 Routines

| Label | What |
|---|---|
| `l62d48` (in `L62be6`, the flight view's key handler) | **F7**: the engines / autopilot key (also the console icon at x = 256 on the bottom panel) |
| `L4b4ea`..`l4b8bc` (system map, module 5) | **F10** "Click on a target or destination for autopilot", then a click picks the target and re-aims a running autopilot |
| `L6199c` (player module, function 28) | the player's **AI step**. When `704(a6) = $04` it calls the ship module's AI step on the player's ship and then checks for arrival and docking |
| `L61af2_mod3_fn24_PlayerPhysics` | the player's per-frame physics; dispatches on `704(a6)` through the table at `L61b2c` |
| `L61f4e` | the physics for state `$04` (autopilot) |
| `L750de` (ship module, function 28) | the NPC AI step, dispatched on `211(a0)` (AI doc section 5) |
| `L75444` / `L7549a` / `L754c2` | AI states `$20` (take the destination as target, set the aim offset), `$00` (fly to target), arrival logic |
| `L7550e` / `L75006` | AI state `$80` (docking approach) / ask the station to accept the ship (station function 3) |
| `L4388c` / `L43732` | the steering law (`714(a5)` / `720(a5)` with obstacle probe), incl. the time-acceleration shortcut at `l438f0` |
| `L5f2b8` (station module `68(a6)`, function 32, `d1 = 6`) | docking clearance request |
| `L63a96` (player module, function 48) | "object gone": target lost turns the autopilot off |

### 2.2 State

| Where | What |
|---|---|
| `704(a6)` | flight state: `$00` manual, `$04` autopilot, `$08` hyperspace, `$10`/`$14`/`$5c` docking or landing after contact, `$18`..`$44` docking animation, `$48` take-off, `$4c` after take-off, `$54` docked, `$58` dead, `$60` landed, `$68` landed rough |
| `705(a6)` bit 0 | engines off |
| `708(a6)` | navigation target (object id); 0 = none |
| `848(a6)` (`A6_plr_speed_set`) | the manual set speed |
| `688(a6)` | current speed |
| `211(a0)` | AI state of the player's ship while on autopilot (AI doc section 5) |
| `210(a0)` / `212(a0)` | AI target / destination. `212` is filled from `708(a6)` |
| `213(a0)` | steering look-ahead 0, 1 or 2 (section 5.2) |
| `214..218(a0)` | aim offset (x, y, z words, x 1024 mm) added to the target's position |
| `226(a0)` | distance exponent to the aim point, written by every steer call |
| `68(a0)` | the ship's parent frame (object id) |
| `712(a6)` | the bay the player was given |

## 3. Switching it on and off

### 3.1 The F7 key

[code] `l62d48`. What F7 does depends on the flight state:

| State | F7 does |
|---|---|
| Docked or landed (`$5c` and up) | launch (`$48`), after the launch-permission check |
| `$50`..`$5b` | nothing |
| Take-off (`$48`) | `$4c`, AI state cleared |
| **Autopilot** (`$04`) | **off**: set speed := current speed (`688` -> `848`), state `$00` manual |
| Manual, engines on | engines off (`705` bit 0 set) |
| Manual, engines off | engines on, and **autopilot on** if a target is selected (`708 != 0`) and the Automatic Pilot is fitted (`158` bit 5); otherwise plain manual |

So in flight F7 cycles **engines on -> engines off -> autopilot -> engines on**. Without the equipment or a target the
third step is skipped. Engaging from `$4c` first releases the bay still held at the station (`L622c4`, station
function 2). Engaging sets `212(a0) = 708(a6)`, `704 = $04` and `211(a0) = $20`. It does not touch the set speed.

The console icon at x = 256 follows this (`L63e6e`): one picture for engines on, another for engines off, and the
base picture when the next press would engage the autopilot or the autopilot/take-off is running.

### 3.2 Picking or changing the target

- [code] **System map** of the current system: F10 shows "Click on a target or destination for autopilot" (sets bit 0 of
  `1182(a6)`); the next click on a body sets `708(a6)`, plays effect 28, and **if the autopilot is running** sets
  `211(a0) = $20`, `212(a0) =` the body. The ship turns to the new target at once (`l4b8bc`).
- [code] **Space view**: a click (key code `$ff`) selects the object within 6 pixels of the pointer, or clears the
  target if there is none (`L62f02`), and **always switches the autopilot off** (`l62d28`). Unlike F7 this does not
  copy the current speed into the set speed, so the ship then flies to whatever set speed was left from before the
  autopilot was engaged. [inferred from code]

### 3.3 Other ways it ends

| Event | Where | Result |
|---|---|---|
| Arrival at a non-starport target | `L6199c` `l61a40` | state `$00`, message `$8637` "Arrived in vicinity of target", **set speed := 0** (the ship brakes to rest in its frame) |
| Docked / touched down | `L6199c` `l619fa` | state `$10` (orbital station), `$14` (ground port) or `$5c` (pad), the normal docking chain |
| Target destroyed or gone | `L63a96` | target cleared, state `$00`, message `$98ab` "Target Lost." (set speed untouched) |

[code] Being shot at does **not** end it: function 48 with a hit only sets the hit-flash timer. A pirate closing in
cancels **time acceleration** (AI doc 6.1) but the autopilot keeps flying.

## 4. What runs while it is on

### 4.1 Once per AI look (`L6199c`)

[code] The player module's function 28 runs at the same call as an NPC's AI step (AI doc 2.1; [inferred] once per game
second, batched at high time acceleration). When `704 = $04` it does:

1. `movea.l 100(a6),a3` / `jsr 28(a3)`: the ship module's AI step `L750de` on the player's ship. It dispatches on
   `211(a0)` exactly as for an NPC.
2. If the AI state is now `$74` (docked, set by `L75006`): remember the station in `678(a6)`/`672(a6)` and go to the
   docking state (`$5c` if the station has pads, bit 6 of `280`; `$10` for an orbital station, bit 5; `$14` otherwise).
3. Else, if the target is **not a starport** (bit 4 of `280(a1)` clear), the target is the ship's parent frame
   (`210(a0) == 68(a0)`) and `226(a0) <= $17`: arrival (section 3.3).

### 4.2 Every frame (`L61f4e`)

[code] Compared with manual flight (`L61f9c`) the autopilot physics:

- does **not** call `L62606_PlayerControlUpdate` (stick, keys, set speed) or `L627ac`,
- does call `L6275c` (only turret aiming, when the view is a turret),
- runs the collision test `L622de`; contact of the docking kind (bits 5 and 6, not 7) goes to the docking code `l62100`
  shared with manual docking; bit 0 (atmosphere) runs the drag/heat code `L61c26`, otherwise `L62400` (scenery,
  asteroids, dust),
- then gravity (`750(a5)`), the AI's thrust request (`732(a5)`) and rotation (`672(a5)`), the same calls as NPC
  physics.

The "VACATE THE AREA" warning count-down (`L62068`) is paused while the autopilot is on.

## 5. How the AI flies the ship

### 5.1 The states it passes through

[code] AI states in `211(a0)` (the AI doc has the full table):

```
$20  L75444   target := destination; aim offset := approach point (starports) or 0
 |
$00  L7549a   fly to target (720 = steer with obstacle probe; 726 = clamp thrust; 696 = turn nose to the aim vector)
 |            each look, while the distance exponent n > 5 (about 260 m)
 v
     L754c2   n <= 5:  not a starport -> nothing (keeps the last thrust request; see 5.4)
 |                     starport     -> ask for clearance (station fn 6) every look until granted
 v
$80  L7550e   granted: aim offset := bay vector, effect 26; follow the bay point (714, look-ahead -1)
 |            and match the station's orientation on all three axes (684, d7 = -1)
 v
$74           docked (by collision at 1x, by L75006 when accelerated), then 704 := $10/$14/$5c
```

### 5.2 Steering

[code] From the AI doc section 4, as it applies to the player:

- **Closing law** (`L4388c`): the wanted velocity change is the target's relative velocity plus a term toward the target
  that grows with the **square root of the distance**: a constant-deceleration braking curve. The thrust is clamped to
  the ship's own limits (`146..157`, from the model) and `732(a5)` never applies more than the wanted change, so it
  does not oscillate.
- **Look-ahead** `213(a0)`: set when the ship object is made (`L449d2` area) from the model's **retro/side thrust**:
  0 below `$aa5`, 1 below `$1329`, 2 above. It is added to the distance exponent before the square root, so each step
  makes the approach curve about 1.4 times faster. Ships with strong retros come in faster. [code; the effect is
  inferred]
- **Nose to target**: `696(a5)` turns the ship to face the aim vector `220(a0)`. Thrust is requested along the wanted
  velocity change in ship axes, so braking on the way in is done with the **retro thrusters** while facing the target.
  [inferred]
- **Relative velocity** (`L43676`): only counted when the ship and the target share a parent frame. If the target is the
  ship's parent, it counts as at rest; if they are in different frames the target's own motion is **ignored**.
- **Obstacle probe** (`L43732`, every ~16 looks, more often when accelerated): a ray along the ship's velocity; if it
  would hit something, the aim vector becomes half the surface normal plus a quarter of the old direction. This is the
  only thing that keeps the autopilot out of planets and stations.
- **Frame** (`L75248`, every 20 looks): re-choose the parent body (`L440fe`). This matters for the planet arrival test
  in 4.1.

### 5.3 The aim offset

[code] `L75444` sets `214..218(a0)`, added to the target's position (x 1024 mm, in the frame's axes, `L43626`):

| Target | Offset | Point |
|---|---|---|
| Orbital station (`280` bits 4 and 5) | the station's matrix x (0, 6000, 0) | about 6 km out along the station's axis |
| Ground starport (bit 4 only) | (0, 0, 4000) | about 4 km on the frame's third axis ("above" in the AI doc) |
| Anything else | 0 | the object's centre |

After clearance (`l754e8`) the offset becomes the **bay vector** from station function 6 shifted down by 10: for an
orbital station (0, 0, -1), i.e. the station's own origin (the docking slot); for ground ports with pads the pad
position (tables `L5f1ce`/`L5f1dc`/`L5f1fa`, or the custom pads through the `Lcsys_pad` hook, `docs/system-overlays.md`).

### 5.4 Arrival by target type

| Target | Ends how | Notes |
|---|---|---|
| Orbital station | docks (`$74` -> `$10` -> animation -> `$54`) | |
| Ground starport | lands on a pad (`$5c` -> `$60`) or ground docking (`$14`) | |
| Planet or moon | "Arrived in vicinity of target" when the body is the parent frame **and** the largest distance component is under about 68,700 km | for small moons the frame switch comes first, for large planets the distance test does |
| Star, gas giant | [inferred] **may never arrive**: the test is measured to the **centre**, so a body whose radius is over about 119,000 km can never pass it, and one over about 68,700 km only on some approach directions. A star is always too big | the autopilot keeps diving at the centre and only the obstacle probe turns it away |
| Ship | [inferred] never ends by itself: at `n <= 5` (within about 260 m) the AI stops updating the thrust request and the ship coasts on the last one, roughly keeping station. Ends when the target leaves (Target Lost) | the "different frame" rule in 5.2 can make it lag a moving target |

## 6. Clearance and docking

### 6.1 The request

[code] At the approach point the AI calls station function 6 (`L5f2b8`) **every AI look** until it is granted.
For the player:

| Check | Message |
|---|---|
| Legal status `11774(a6) >= 10000`, **or** the copy-protection value `614(a6)` is 0 (wrong quiz answers) | `$98a2` "CLEARANCE DENIED. You have large outstanding fine(s)." |
| Another ship holds the approach slot (`159`/`160(a1)`) | `$98a0` "CLEARANCE DENIED. Traffic Control busy." |
| No free bay | `$98a1` "CLEARANCE DENIED. All bays full." |
| Granted | `$98a3` "Clearance granted. Proceed to bay N." (starts the docking music if that option is on), or `$98a5` "Clearance already granted" |

[inferred] Because the request repeats every look, a refused autopilot sits at the approach point and repeats the
refusal message about once a game second until something changes. With large fines it waits there forever.

[code] The clearance has a time limit (`174(a1)` = 3600). When it runs out with the player on autopilot, the station
code (`l4b18c`) sets the ship back to AI state `$20` and prints `$98a6` "CLEARANCE WITHDRAWN": the autopilot flies
back to the approach point and asks again.

### 6.2 The final approach

[code] State `$80` follows the bay point with look-ahead -1 and turns the ship to match the station (so it spins with
an orbital station).

- **At 1x** (`14482(a6) = $3e2`) it keeps following until the ship **touches** the slot. The player's collision test
  reports a docking contact and `l62100` docks the ship, as in manual docking.
- **Accelerated**: once the exponent reaches 0 (within about 8 m) `L75006` asks the station to accept the ship
  (function 3) and sets AI state `$74` straight away; the ship gets the station's orientation (orbital) or an upright
  one (ground). If the station refuses (the slot is no longer reserved for it) it asks for clearance again.

## 7. Time acceleration

[code] `L4388c` at `l438f0`, only when the selected acceleration index `10432(a6)` is not 0 (time-acceleration doc 2.7):

```
d6 = n - max(speed exponent, 2) - index - index/4      ; a rough log of the time left to arrive
if d6 < 2 and the aim offset is not zero:
    re-choose the frame (L440fe); parent := the station's parent (orbital) or the starport itself (ground)
    position := target position + aim offset; velocity := the target's; thrust := 0; n := 0
```

For the autopilot this means:

- **Starports**: once close enough in exponent terms the ship is **placed** at the approach point (n = 0, so it asks for
  clearance on the same look), then in state `$80` placed at the bay point and snap-docked by `L75006`. At 10000x a
  long trip ends in a jump of thousands of km. [inferred]
- **Planets, stars, ships**: offset 0, no shortcut. The ship flies the whole way at the accelerated rate, relying on
  the probe; the closing law's speed scales with distance, so in practice this is still quick. [inferred]
- The host time guard (`Ltguard`, `src/time_guard.c`) only reacts to ships whose **target is the player**, so it
  never slows the player's own autopilot.
- The frame re-choice timer `281(a0)` is cleared whenever `d6 <= 8`, so near the end of the trip the frame is
  re-chosen every look.

## 8. Quirks and possible bugs

None of these is fixed. Each one needs a test before deciding anything (section 10).

1. **Set speed after a click in the space view.** F7 copies the current speed into the set speed, a click in the space
   view does not, so the ship can suddenly brake or accelerate to an old set speed. [inferred]
2. **"Arrived" sets speed 0.** At a planet the ship then brakes to rest in the planet's frame on its own. This is
   probably the "autopilot stops at once" report in `docs/frontierverse-bug-list.md`. [inferred]
3. **Stars and large gas giants may never "arrive"** (5.4). [inferred]
4. **Clearance refusal repeats every look** and a fined player waits at the approach point forever (6.1). [inferred]
5. **Targets in another frame**: their velocity is ignored by the closing law (5.2), so a moving target in a
   different frame is chased where it is, not where it is going. [code]

## 9. Host side

- `src/game_state.c`: `game_flight_status()` maps `704 = $04` to `FLIGHT_AUTOPILOT` ("Autopilot"); used by the
  PERF log, the script engine `status`, netplay and the time guard.
- No host code changes the autopilot. The custom starport pads (`Lcsys_pad`) are used by it because it asks the
  station for a bay the same way NPC traffic does.

## 10. Open points and tests to run

- Engage at 1x toward an orbital station, ground port and a pad port: watch `211(a0)`, `226(a0)`, `214..218(a0)` with
  the script engine; confirm the approach points (6 km / 4 km) and the docking contact.
- Same at 100x and 10000x: confirm the two placements and the snap dock; log distance per frame (the SHIP-screen
  `ARRIVE ... TP` line from the time guard only covers ships targeting the player).
- Autopilot to a star and to a gas giant: does it ever print "Arrived in vicinity of target"?
- A click in the space view while on autopilot with a non-zero old set speed (quirk 1).
- With a large fine: confirm the repeated refusal messages.
- Measure the look-ahead `213(a0)` for a few player ships to confirm which thrust word it reads.
