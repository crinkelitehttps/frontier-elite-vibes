# Time acceleration and ship spawning in Frontier: Elite II

Findings from reading `fe2/fe2.s` (the original game, same layout in `fe2_modded.s`) and from
measurements in a scripted test instance. Written while looking at issue 6 ("pirates only appear one at
a time") and the report that, with time acceleration pressed, a pirate can appear a few km away "out of
nowhere" while a cargo canister from the last kill is still drifting next to you.

Every statement is tagged:

- **[code]** read from the disassembly, not run
- **[measured]** seen in a test instance (how: last section)
- **[inferred]** a conclusion from the above; could be wrong

Labels like `L74732` are labels in `fe2/fe2.s`. `NNN(a5)` is an entry of the common jump table at
`L45332_a5_jumptab` (6 bytes per entry; the name of each one used here is given where it first appears).
`NNN(a6)` is a game global (`A6_*` in the file header, `fe2/NOTES.md`).

## 1. Short version

- The game clock runs off a 50 Hz vblank. Time acceleration changes how much game time one vblank adds.
  Steps are 1x, 10x, 100x, 1000x, 10000x; hyperspace uses about 261000x.
- One game day is a day of that clock. The periodic pirate event (event 16) fires when the day counter
  passes a multiple of 7, so **it only fires once per 7 game days**. At 1x that is never in practice; at
  10000x it is once every 60 real seconds. Pirates met mid-flight therefore come from time acceleration,
  plus one big roll when you arrive in a system.
- New pirates are placed on a sphere of radius 1.4 x 10^9 km around the system centre, with no regard
  for where the player is. They fly to the player under normal physics at roughly 2000 to 5000 km/s
  (game time) when far away. Time acceleration makes that trip take seconds.
- When a pirate gets close, the game forces time acceleration back to 1x and prints "WARNING: Ship is
  Under Attack". The pirate is then a couple of km from the player. That is the "out of nowhere".
  It is not a teleport. It is a ship that spent 20 to 30 real seconds crossing the system, in a view
  where nothing is drawn at that range.
- Pirates aim at the player plus a random offset of up to 4.2 km on each axis, which is where "2 to 7 km"
  comes from.
- There is a real teleport in the AI distance routine that only runs when time acceleration is on
  (section 2.7). It does fire for pirates: at 10000x a pirate was seen to go from about 10^6 km to under 4 km
  in one step. So "out of nowhere" is partly a teleport, not only a long flight. See `npc-ship-ai.md` section 8.

## 2. Time acceleration

### 2.1 The steps

| Key | Rate added per vblank | Hex | Factor | Index (`10432(a6)`) |
|---|---|---|---|---|
| Shift+F1 | 994 | `$3e2` | 1x | 0 |
| Shift+F2 | 9942 | `$26d6` | 10x | 3 |
| Shift+F3 | 99420 | `$1845c` | 100x | 7 |
| Shift+F4 | 994200 | `$f2b98` | 1000x | 10 |
| Shift+F5 | 9942000 | `$97b3f0` | 10000x | 12 |
| (hyperspace) | 260046848 | `$f880000` | about 261000x | 18 (`$12`) |

[code] `L72350` (module 10 function 16) handles the keys. The ST keymap gives Shift+F1..F5 the codes
`$c1..$c5`. The panel icons, the touch strip (`src/touch_input.c`) and the gamepad bindings all inject
those keys. Selecting 10x and above also does `subi.l #$c22e,532(a6)` (see 2.3); that is a clock
resync, not a fuel or money cost.

[code] The active rate is `14482(a6)`, its index `14486(a6)`. The selected one is kept in
`10428(a6)` / `10432(a6)` and copied by `L72460`. A rate of 0 means the game is paused (menus and
station screens, `L725d4` area); several effect spawners test `tst.l 14482(a6)` and skip when paused.

[measured] The register values above are what the running game holds at each step.

### 2.2 When the keys do nothing or do something else

[code] in `L72350`:

1. If bit 0 of `598(a6)` is set (modal screen) the key is ignored.
2. If the flight state `704(a6)` is `$8` (hyperspace) any of the five keys sets the hyperspace rate
   (`$f880000`, index 18).
3. If bit 1 of `10476(a6)` is set, Shift+F2..F5 are refused: the game shows message `$98ac`
   ("WARNING: Ship is Under Attack"), plays effect 25 and keeps the current rate. Shift+F1 (back to 1x)
   still works. Bit 1 is set every AI pass by a ship in the attack states `$4`/`$8` (`L756a4`) and cleared
   at the start of each AI pass (`L4480c`), so it means "something hostile is attacking right now".
4. Otherwise the new rate is stored.

### 2.3 How the clock works

[code] Variables:

| Global | Meaning |
|---|---|
| `14482(a6)` | game time added per vblank (the rate) |
| `14450(a6)` | accumulator: the vblank handler (`L41e2a_vblank`) adds the rate to it every vblank |
| `14446(a6)` | the frame's tick: the main loop moves the whole accumulator into it each frame. Physics and AI use this as `dt` |
| `14458/14454(a6)` | second accumulator and tick, always `$3e2` per vblank, unscaled. Used where real time is wanted (UI, animation) |
| `524(a6)`/`528(a6)` | `A6_time_of_day`, a 64-bit counter: the first long is the low part, `528` (`A6_day_since_epoch`) the high part, which is the day number |
| `532(a6)` | the time of the last "once a second" pass |
| `600/604(a6)` | a one-shot time limit; when reached, event 21 fires and the limit is disarmed (`$ffffffff`) |

One game second is `$c22e` = 49710 units. At 1x a vblank adds 994, so 50 vblanks (one real second at 50 Hz)
add 49700: the clock runs 0.02 percent slow. A day is 86400 x 49710 = about 2^32 units, which is why the
high long is the day: it is incremented by the carry of the low long (`add.l d4,d2 / addx.l d5,d3` in
the main loop at `L428c6_GameLoop`).

[measured] At 10000x the day counter advances once every 8.6 real seconds (the day number went
`0x11d5a2 -> 0x11d5a8` in 61 s).

Main loop order [code] (`L428c6_GameLoop`):

1. Take the whole accumulator into `14446`; add it to the clock.
2. Clock limit check (event 21).
3. If the day number changed: if `(old_day & $ffff) mod 7 == 0` send event 16; always send event 15.
4. Second tick from `14458`.
5. `L447bc_DoPhysics`: for every object with flag bit 0, run its module's function 24 (movement).
   `97(a0)`/`96(a0)` let an object skip frames; all the objects I looked at have 0, so they run every frame.
6. `L45876_UIKeyboardPoll`.
7. `L457d8`: each active module's per-frame function (16); then, **if at least one game second has passed
   since `532(a6)`**, add that many seconds to `532` and call each module's once-a-second function (44)
   with the number of elapsed seconds in `d0`, then `L4480c`: for every object with flag bit 1 call its module's
   function 28 with `d1` = elapsed seconds. For ships that function is `L750de`, the AI state machine.
8. Draw.

The consequences, all [code] unless said otherwise:

- **Physics uses the full frame tick.** At 10000x one frame is 9.9 million units = 200 game seconds.
  The host patches `Lpos_dt` and the sub-step hook (see the comments at the end of `fe2.s`) exist
  because the original integrated that badly.
- **AI decisions are taken once per game second, batched.** At 10000x the AI runs once per frame and is told
  that 200 seconds went by. So a ship's velocity changes, which are scaled by that number, can be large,
  and a ship can cross very long distances between two AI looks.
- Anything driven by the day number (events 15 and 16) is calendar time, not flight time. This is why
  event 16 only matters under acceleration.

### 2.4 Events tied to the clock

[code]

| Event | Sent | Who cares |
|---|---|---|
| 21 | clock reached the limit at `600(a6)` | `L8160a` area (timed scripted events, hyperspace arrival) |
| 15 | the day number changed | modules with the "daily" bit |
| 16 | the day number changed and the old number is a multiple of 7 | the ship module (`l74710`): the periodic pirate and traffic roll (section 3.3). Skipped in hyperspace (`704(a6) == 8`) |
| 3 | game start / new game (`L42542`) | ship module: first population of the system |
| 1, 19, 2 | the hyperspace jump sequence (`L814dc`) | ship module: collect followers, clean up, populate the new system (section 3.5) |

[measured] `event 16` sent through the debug command (`src/script.c`) makes the generator run; the
`enter` cheat does not.

### 2.5 What forces the rate back to 1x

[code] `L722b2` (module 10 function 32, called through `92(a6)`) does: if the rate is already 1x, return
Z set; otherwise set 10428/10432 and 14482/14486 to the 1x values, **clear `14446` and `14450`
(pending time is thrown away)**, redraw the icon and return NE. Callers:

- `L76058` (module 12 function 32): **every on-screen message** resets the acceleration first (unless in
  hyperspace). Warnings, "Ship is Under Attack", collision/damage messages, docking and police messages.
- `L75606`, the pirate approach state: when the pirate is close enough (section 3.7) it calls it and, if it
  changed anything, shows message `$98ac` and starts the battle music.
- `L63362` (hyperspace exit), `L63a96`, `L78416_PoliceConfiscateShip`, `L76436` (sound/warning helper).

[measured] At 10000x with one pirate coming in, the rate dropped to `$3e2` on its own after 25.9 s.

### 2.6 What changes with the rate besides speed

[code]

- **Space dust** (`L62400`): spawned relative to the player's velocity at an exponent of `$19 - index`, so
  it appears closer the faster time goes (streaks). Dust objects (state `$60`, `L75a06`) are deleted when
  their accumulated tick passes the current rate, so they vanish sooner at high rates. Dust is not spawned
  while paused, nor when the option is off (`A6_opt_dust_n_clouds`).
- **Asteroids** (`L624f2_GenAsteroids`): 10 percent chance per call, only while the player is relative to object
  `$72` (114), capped by `A6_max_asteroids`; the spawn exponent is `$15 - index`.
- **Sparks** (`L4600a`, 810(a5)): not spawned while paused.
- **Ships in state `$80`** (`L7550e`): when the rate is not 1x they copy the orientation of the ship they
  follow instead of flying.
- **The AI distance routine** (`L4388c`/`L43732`, 714(a5)/720(a5)): see 2.7.
- The panel clock redraws every frame at 100x and above, otherwise once a second (`L6f032`).

### 2.7 The distance routine's time-compression shortcut

[code] `L4388c` computes the vector from an AI ship (`a0`) to its target (`a1`) and a distance exponent in `d7`.
Near its end (`l438f0`) it does this only when `10432(a6)`, the acceleration index, is not 0:

```
d6 = d7 - max(norm,2) - index - index/4        ; norm: how far the vector was normalised
if d6 > 8:  nothing
else:       clear the ship's cooldown 281(a0) and 97(a1)
            if d6 - 2 < 0 and the ship has an aim offset (214(a0) not zero):
                  L440fe (re-choose the ship's frame), set its parent to the target's,
                  position := target position + offset, clear velocity
```

So, by design, a hostile that is "close in exponent terms, after allowing for the speed-up" is placed
next to its target immediately. `norm` is the exponent of the closing speed, so `d7 - norm` is the log of the time
to arrive: the ship is placed when it would arrive within about one accelerated frame.
[measured] (later) at 10000x the exponent went 27 to 9 inside one 0.6 s sample, with the rate dropping to 1x: this is that
path. At 10x it did not fire. Details in `npc-ship-ai.md` 8.

### 2.8 Hyperspace

[code] A jump sets the rate to `$f880000` with index 18 (`L814dc` area and the misjump code near
`L81662`) and writes the arrival time into `722(a6)`. About 261000x means a jump of several days is a
few seconds. Events 1, 19 and 2 are sent around it.

## 3. Ship and object spawning

### 3.1 Object table

[code] 115 slots. The first 115 bytes of big space (`A6_big_space`) are per-slot flag bytes (bit 0: runs
physics, bit 1: runs AI, others: render and bookkeeping); slot 0 is at `$74`, slot n at `$d4 + 286*(n-1)`.
Fields used here:

| Offset | Meaning |
|---|---|
| 44/48/52, 56/60/64 | position, low and high longs, in mm, **relative to the parent frame** |
| 68 | parent object id; 93 own id; 69 frame flag |
| 90 | model (byte offset into the model table; index = /2) |
| 94 | module that owns the object (11 = ships/AI, 14, 17, ...) |
| 98..109 | velocity |
| 126 | time stamp (last physics pass, or the arrival time for waiting ships) |
| 158 | flags (bit 8: may follow the player into hyperspace) |
| 210 | AI target (object id) |
| 211 | **AI state** (section 3.6) |
| 214/216/218 | aim offset words (section 3.7) |
| 236 | class (a pirate's is overwritten to `$fb`) |
| 281 | per-ship timer used by `L75248` |

The player is object id 107 / slot 107 in the saves and runs I looked at; `674(a6)` points to it and
`670(a6)` is its id.

[measured] Positions of every object are relative to its parent; parents form a chain
(planet -> star or barycentre -> system root 114/0). Distances between objects in different frames need the
sum along the chain. `L440fe` (called from `L75248` roughly every 19 AI ticks) re-chooses the parent: this is
why a pirate's parent changed from 0 to 114 to 111 as it flew.

Allocators:

- `A5_AddObject` (462(a5), `L44a78`): a full object from a template in `a0` plus class `d0` and model `d1`.
  Ships, canisters, the player.
- `456(a5)` (`L44a98`): a lighter allocator for effects and things made from an existing object (class `d0`,
  model `d1`).
- Deleting: `474(a5)` (`L44c1c`). Few callers: the expiry states (`L759dc`, `L75a06`, `L74dc4`), `L747e2`,
  `L74378`, `L61e30`, `L4b386`, `L63bdc`. A full table makes the allocator fail and the spawner quietly give up.

There is no cap on the number of pirates apart from the 115 slots, and the generator does not remember
kills. [measured] 41 pirates were alive at once after about 40 forced events in one test.

### 3.2 Placing something: `L459c0` (576(a5))

[code] A polar placement used for planet orbits (`L4b06c_mod1_fn24_UpdateSystemPhysics`) and for every
spawn: random angles in `d0/d1` (`A5_2RandInts`), a mantissa in `d6` and an exponent in `d7`, giving a
point on a **sphere around the parent's origin** (no player term). The caller may add the player position.

[measured] **Radius = `d6 * 2^(d7 - 15)` mm.** Checked three ways:

| Caller | d6 | d7 | Radius | Seen |
|---|---|---|---|---|
| Pirates, arriving traffic (`L74d7a`) | `$5000` | 51 | 1.407 x 10^9 km | every pirate: 1.407 x 10^9 km |
| Orbital class 11 ships (`L749f0`) | `$5000` | 24 | 10.4 km | 10.4 to 10.5 km, all three |
| Followers on arrival (`L746ba`) | `$5000` | 23 | 5.2 km | (computed; formula holds for the others) |
| Player at jump start (`L814dc`, then `L816e8`) | `$4000..$5fff` | 51 | 1.1 to 1.7 x 10^9 km | not measured. The earlier row (`$5600`, 57, 9.7 x 10^10 km, "seen after `enter`") was wrong: see `hyperspace.md` |
| Ships requested by other modules (`L74378`, types 1 to 4) | `$5000` | 49 | 3.5 x 10^8 km | not checked |

The player is placed on a sphere of about the same size as the pirates' (see `hyperspace.md`; an earlier version of this
doc said 64 times further out, which was wrong).

### 3.3 The population routine `L74732`

Called from event 2 (arrival, end of `l74630`), event 3 (`L74726`) and event 16 (`l74710`, which first sets
bit 0 of `10476(a6)`). Event 3 and 2 clear that bit. [code]

It asks the system module (`60(a6)`, function 32, `L4beee`) for per-system numbers and reads two of them:
`d3` (pirate limit) and `d5` (traffic limit). In order:

1. `RandInt(d5)` times `L747e2`: **inbound traffic** (3.4a).
2. Arrival only (bit 0 clear): if the system has stations (`1248(a6)`), `RandInt(min(4 x stations, 25))` times
   `L7483e`: **stock parked at stations** (3.4b).
3. Arrival only: `RandInt(d5)` times `L749f0`: **orbital class 11 ships** (3.4c).
4. `RandInt(d3)` times `L7490c` (arrival: `RandInt(4 x d3)`): **pirates** (3.4d).
5. `RandInt(d3)` times `L7490a`, which is just `rts`. Dead code from a removed spawn type.

`RandInt(n)` is 0 to n-1, so "limit 3" means 0 to 2 rolls. On the periodic event the station and class 11
steps are skipped.

Where `d3`/`d5` come from [code], `L4beee`/`L4bf00`: a table of 48-byte records at `L4c122`, matched by the
system id (`726(a6)`) for about 325 hand-placed systems; byte 6 is `d3`, byte 8 is `d5`. Their `d3`
values: 0 for 19 records, 1 for 75, 2 for 43, 3 for 65, 4 for 50, 5 for 12, 6 for 58, 7 for 1, 9 for 2.
Anything else gets one of seven default records chosen by squared distance from the point (`$1718`, `$1524`)
in the galaxy grid (the system id's x/y and low bits):

| Squared distance | Record | `d3` | `d5` |
|---|---|---|---|
| >= `$1fa4` (90) | `L4c002` | 0 | 0 |
| >= `$e1` (15) | `L4c032` | 1 | 0 |
| >= `$64` (10) | `L4c062` | 5 | 0 |
| >= `$31` (7) | `L4c092` | 4 | 1 |
| >= `$19` (5) | `L4c0c2` | 3 | 4 |
| >= `$9` (3) | `L4c0f2` | 3 | 6 |
| closer (the core) | `L4c122` | 1 | 7 |

Government, allegiance, lawlessness and the player's rating play **no part** in the pirate roll. [code]
(The `L7508a` and police code looks at them for other things.)

Pirates per roll [code] (`L7490c`, as changed for issue 6): `RandInt(7)`: 0 one pirate, 1 or 2 a group of
three, 3 to 6 nothing. Mean 1.0 pirates per roll. So a periodic event in a limit-3 system gives 1.0
pirates on average, in a limit-9 system 4.0.

Result on the clock [inferred]: periodic wave every 7 game days = 60 s of real time at 10000x, 10 min at
1000x, 100 min at 100x. A 10000x flight through a limit-3 system meets about one pirate a minute.

### 3.4 What each spawner makes

a. **Inbound traffic, `L747e2`.** Class 1 ship at the 1.4 x 10^9 km sphere, state `$3c` (waiting for an arrival
   time stored in `126(a1)`, now plus up to about 4 game days), destination station picked by `L74d10`
   (`1254(a6)` + `$1da` x n; id in `212`). When the time comes (`L75360`) it becomes the arrival flash
   (state `$40`, `L753b8` spawns a class 10 effect, model `$ba`, state `$6c`) and then flies in.
   [measured] Many such objects (state `$3c`, parent 0, 1.407 x 10^9 km) sit in the table after arrival.
b. **Station stock, `L7483e`.** Class 1 ship at a station (position of the station object), state `$84`,
   departure time `126(a0)` = now + up to 16 x 2^32 units; `L75262` waits, `L75280` asks the station
   module for clearance, `L752aa` launches it. [measured] 20+ state `$84` ships present on arrival at Earth.
c. **Orbital class 11, `L749f0`.** Picks a body with bit 4 of `125(a2)` set (`L74d36`), makes a class 11
   object at 10.4 km from it, state `$2c`. I did not identify what it represents.
d. **Pirates, `L7490c`.** Lone: class 6 (`L74922`, `moveq #6`), group: three of class 7 (`l7494a`) with the
   followers cloned by `L74a34` and shifted 100 m and 300 m. All via `L74a38` (ship build), placed by
   `L74d7a` with `d6=$5000`, `d7=51`; state `$14`; target `210(a1)` = the player id; `L749aa` sets
   `236 = $fb` and a random rating-flavoured value in `238`. See 3.7.
e. **Followers.** [code] Event 1 (jump start) lists nearby objects that are flagged to follow (class 8,
   model `$ba` flashes, class `$fb` ships with bit 8 in `158`, within exponent `$10`) at `10478(a6)`.
   Event 2 re-places each around the player, at 5.2 km (`L746ba`) or on the far sphere (`L746fe`), in the
   waiting state `$3c`. So ships that were on you when you jumped arrive with you, a few km away.
f. **Ships requested by other modules.** `L74378` (module 11 function 32, called through `100(a6)` with
   a type in `d7`). Types 1 to 4: a hostile of class 8, 4, 2 or 10 on the 3.5 x 10^8 km sphere, state `$24`,
   target the id in `d1`, with its own random aim offset (the mission module calls it, `L7ec9e`). Types 5,
   7, 8: a ship of class 10, 4 or 2 at a position handed in, aimed at the player (police, `L80f5c`; the
   mission module, `L7e024`). Type 6: a named class 12 ship parked at a station (`L7f084`). The caller
   decides when; none of these is rolled by the population routine. [code] I did not run them.
   Separately, `804(a5)` = `L4607e` launches a pylon item (class 75, models 6 to 20); pirates use it with
   about a 1-in-8 chance per AI look while in the close-in sub-state (`L756e4`).
g. **Effects and wrecks.** Explosion pieces (`L44d3e`, class 9 model `$bc`, state `$64`; `L75852`; `L61e30`
   state `$54`), sparks (`L4600a`), hyperspace flash (`L63362`), escape pod (`L6348e`).
h. **Cargo.** Canisters (`L44f1c`, `L7e56a`): class 15, model `$b8`, commodity id in `116`, name from the
   format string `$8e00 + id`, state `$4c`/`$50`. The 1 t of metal alloys a dead pirate sheds is this.
i. **Environment.** Dust and asteroids (2.6). Mining (`L81750`, `L81846`): class 13, model `$78`.
j. **The player.** `L615c0` builds object 107 from the template at `L7425a`.

### 3.5 Hyperspace arrival of the player

[code] The placement is done at the **start** of the jump (`L814dc`): `L816e8` with random angles, `d6 = RandInt($2000) | $4000`,
`d7 = 51`; velocity and thrust are zeroed. The new system is generated afterwards and `L74732` runs through event 2, so the
pirates exist during the tunnel. The `$5600`/57 (and `1044(a6) x $151`/63) placement is only in a misjump branch that
looks unreachable. Everything about it, including what `1044(a6)` is (the jump distance), is in `hyperspace.md`.

### 3.6 AI state table (the ones that matter here)

[code] `L750de` dispatches on `211(a0)`. The table at `L750f0` has two words per state; the first is the AI
look, the second the physics look (`L750c0`, normally `L74fb0`).

| State | Handler | Meaning |
|---|---|---|
| `$14` | `L755ee` | new pirate: pick the aim offset, go to `$10` |
| `$10` | `L75606` | approach |
| `$4`, `$8` | `L756a0` | attack (`$8` is the closer sub-state) |
| `$c` | `L75756` | pursue |
| `$1c`, `$24`, `$28` | `L75322`, `L75a2a`, `L757a8` | leaving a station, mission hostile approach, evading a missile (a missile itself is `$44`) |
| `$3c` | `L75360` | waiting for an arrival time |
| `$40` | `L753b8` | arriving (flash) |
| `$70`, `$78`, `$84` | `L75280`, `L752aa`, `L75262` | station traffic |
| `$54`, `$58`, `$5c`, `$60`, `$64`, `$6c`, `$80` | `L759dc`, `L759cc`, `L75a80`, `L75a06`, `L75a80`, `L75986`, `L7550e` | effects, dust, asteroids, followers |

### 3.7 Pirate life cycle in detail

[code] unless marked.

1. **Spawn** (`L7490c`): state `$14`, on the sphere, target the player. [measured] Radius 1.407 x 10^9 km;
   members of a group are 0.1 km apart.
2. **First AI look** (`L755ee`): three random words `>> 3` (so -4096..4095) into `214(a0)`; state `$10`.
   Those words are the aim offset: `L43626` adds `word x 1024` mm per axis to the target position when
   aiming. That is up to 4.2 km per axis, so the aim point is 0 to 7.3 km from the player.
3. **Approach** (`L75606`): `L75248` (regen, and every ~19 ticks `L440fe` re-picks the frame), find the target,
   then `714(a5)` (`L4388c`) for the distance exponent `d7`. If the target's state is `$84` (docked/landed)
   it just stops. If `d7 >= 10`: steer (`726`, `696`) and keep flying.
4. **Close** (`d7 < 10`): state becomes `$4`. If the target is the player and not in hyperspace, call
   `L722b2` (cancel time acceleration); if that changed something, show `$98ac` ("Ship is Under Attack")
   and start the battle music (if the option is on). Then 214/218 are cleared, so from here the pirate
   aims straight at the player.
5. **Attack** (`L756a0`): each AI look sets bit 1 of `10476(a6)` (this is what blocks Shift+F2..F5) and
   re-measures the exponent with `720(a5)`: above 11 the pirate gives up and goes back to the approach
   (`L755ee`); 9 to 11 stays in `$4`; 7 to 8 is `$8`; 5 to 6 rolls for a missile 1 look in 8 (`864(a5)` is the energy
   bomb, not the laser: lasers are fired by the physics step of state `$8`); 4 or less is `$c`, the close
   pursuit (`L75756`). The exponent is the bit-length of the distance in mm over 8192: exponent 10 is about 8 km.
   Full detail, with the distances, in `npc-ship-ai.md`.

[measured] Test, single pirate from 1.25 x 10^9 km, ship in free space:

| Rate | Result |
|---|---|
| 100x | closed 2.0 x 10^5 km per real second, about 2000 km/s of game time |
| 10000x | rate dropped to 1x after 25.9 s; the first look 0.9 s later showed the pirate **2.0 km** away in state `$8`, then 1.9, 1.6, 1.2, 0.8 km |
| 1x | a pirate left at 100 to 400 km did not close; it moved by a few km/s at most. At 1x the long approach is therefore not done by waiting |

[measured] Before the first run, the pirate state stayed `$10` and nothing happened for minutes while the
player's state byte `211` was `$84`: the `enter` and `goto` cheats leave the player in that state
(from the start-of-game landed state). Real flight saves have 0. Set it to 0 before testing.

[inferred] The approach speed at range is set by the AI (`L43c1a`, 726(a5)); I have not worked out its law. It
looks like it falls as the distance shrinks.

## 4. How this explains the report

A player kills a pirate, scoops nothing, and presses Shift+F4 or F5.

1. The clock now runs 1000 to 10000x. Every 10 minutes (1000x) or minute (10000x) of real time the
   periodic event fires, rolls 0 to d3-1 times and creates 0, 1 or 3 pirates per roll on the 1.4 x 10^9 km
   sphere. They are in state `$14` and invisible at that range.
2. They fly at several thousand km/s of game time; at 10000x that is the whole system in 20 to 30 s.
3. Within exponent 10 the pirate goes to state `$4`, time returns to 1x, a warning appears and the
   pirate is a few km from the ship (aim offset). The cargo canister dropped by the last kill is still
   there because from its point of view only seconds of real flight passed at 1x plus the accelerated
   stretch it spent at its own tiny velocity.
4. Nothing was teleported, but nothing was shown either, and nothing about the arrival respects what the
   player was doing when they pressed the button.

Both the arrival of followers (`L746ba`, 5.2 km) and the pirate aim offset produce distances in the
2 to 7 km range, which is why the two get mixed up.

## 5. Things I did not settle

- Settled later (`npc-ship-ai.md` 4.1, 8): `d7 < 10` is a largest component below 4.2 km (the exponent is the bit-length of
  the distance in mm over 8192), and the 2.7 shortcut does fire for pirates at 10000x. Still open: its threshold at 100x and 1000x.
- What the class 11 object, class 13 mining objects and the per-system bytes `d4`, `d6`, `d7` do.
- The AI's far-range speed law: it is the square-root braking curve of `npc-ship-ai.md` 4.2.
- The route by which a pirate moves from state `$10` to `$8` in one look (I only saw the result).

## 6. Possible changes (not made)

Ordered by how small the patch is. All would be same-size patches in both `.s` files.

1. **Make the arrival visible**: raise the threshold in `L75606` (`cmp.w #$a,d7`) so time acceleration is
   cancelled when the pirate is much further out. Smallest change, no change to counts.
2. **Aim the approach properly**: shorten the aim offset (`asr.w #3` in `L755ee`) so the pirate converges on
   the player rather than a point up to 7 km off, and arrives from a direction.
3. **Throttle the periodic event**: skip event 16 while the rate is above 1x, or scale the rolls by the
   rate, so a long accelerated flight does not meet one wave a minute.
4. **Spawn on arrival, not in a calendar event**: tie the extra pirates to distance flown instead of day
   number.
5. **Make it believable at the end**: before the drop, step the rate down (10000x to 1000x to 100x to 1x)
   as the pirate's distance falls, so the approach is seen. **Done, in the host (`src/time_guard.c`, no change to the
   game code):** see section 8.

## 7. How the measurements were made

- `GLFrontier --nosound --control FIFO --do "new 1; waitgame; enter a935c740; goto Castor A,B 1000000000; stop"`
  (system Castor, id `a935c740`; chosen because it has a limit of 3 pirate rolls; Earth's is 1 and gives
  almost none). Run from `devtools/run`, never the real game.
- `event 16`, `peek`, `poke`, `snap` from the script engine. `poke` takes decimal or `0x` hex; a leading
  zero is octal (`000f2b98` writes 0).
- `snap` writes `savs/SnapNN` up to 99, then keeps overwriting 99; read them with `tools/savetool`
  (`savefile.Save.load`, `fe2save.slot_off`, `obj_pos`) and delete them.
- To set a rate directly: `poke l a6+10428 N`, `poke w a6+10432 I`, `poke l a6+14482 N`, `poke w a6+14486 I`
  with the table in 2.1.
- Several instances sharing one control FIFO give garbled results. `pgrep -x GLFrontier` to see them.
- Player death (a pirate group at 1x) resets the object table; every run after a death is invalid.

## 8. The time guard (host side, `src/time_guard.c`)

Added after the analysis above. Setting `time_guard_seconds` (glfrontier.cfg and Settings > Flight), default 9,
0 = off.

- **When:** once per game frame, in the flight view, not landed, docked or in hyperspace. For every ship (all of
  them, not only pirates) it takes the distance to the player from the object table and how fast that fell since the
  last frame, so how long until the ship is within 7 km (where the game's own warning and 1x take over). Ships that are
  parked, docked, launching, waiting to arrive or orbital (states `$2c $30 $34 $3c $70 $74 $78 $7c $84`) are left out.
- **What:** when that time, in real seconds at the present rate, is at most `time_guard_seconds / 10`, the rate goes down
  one step (10000x, 1000x, 100x, 10x, 1x) by pushing the game's own Shift+F key into its key queue. Each step makes the
  time left ten times longer, so each speed shows the approach for about `time_guard_seconds`. A ship braking along
  the AI's curve needs twice what its present speed gives (the sum of v = k sqrt(d)), which it allows for. It does
  the same when the player speeds up again with a ship closing in. While something attacks, where the game refuses the faster
  keys, it uses Shift+F1.
- **Measured** (a test instance at Castor, 10000x, three pirate waves, player at rest relative to a far star): 12 -> 10
  at 1.6 x 10^6 km, 10 -> 7 at 1.7 x 10^4 km, 7 -> 3 at 181 km, 3 -> 0 at 11.8 km, the stages lasting about 8, 8 and 6 s;
  the game's own cancel and warning at 4.2 km (largest component) did the rest. Without the guard the same run stayed at
  10000x until the game's shortcut put the pirate next to the ship.
- **The shortcut is not removed.** A ship that goes from at least 100 km to under 15 km (and to a quarter of the
  distance) in one frame is logged on the SHIP screen's debug page, `ARRIVE XX-123 3.0km<-8.75e+04km 10000x TP` (`TP`:
  its velocity is exactly zero, which is the shortcut's mark). Logging only runs with the guard on. `GLF_GUARD_LOG=1` also
  prints the steps and arrivals to the log.

**The same idea in the game code (`Ltguard`, `docs/original-game-fixes.md` 10).** A reduced version is in both `.s`
files and in the Atari patch: a hook in `L4388c` where the shortcut is, for ships whose target is the player, that
pushes the game's own Shift+F key one step down when `d6` (the game's rough log of the flight time left) is at or under
a limit per rate. The limits (3, 3, 5, 6 for 10000x, 1000x, 100x, 10x) were measured with the host guard in
`GLF_GUARD_PROBE=1` mode, which only logs the real seconds of flight left at each rate change: 0.80, 1.0, 1.2 and
1.5 s, stages of 7 to 9 s. Both guards can be on at once; they only ever step the rate down.
