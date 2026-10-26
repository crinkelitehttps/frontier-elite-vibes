# NPC ship AI in Frontier: Elite II

Findings from reading `fe2/fe2.s` (the original game; `fe2_modded.s` has the same layout) and from
measurements in a scripted test instance. This continues `docs/time-acceleration-and-ship-spawning.md`
(called "the spawning doc" below), which covers the clock, the spawners and the pirate life cycle in outline.
This one goes into what a ship does once it exists: the state machine, steering, weapons, reactions to
being hit, station traffic, ship and loadout generation.

Every statement is tagged:

- **[code]** read from the disassembly, not run
- **[measured]** seen in a test instance (how: section 13)
- **[inferred]** a conclusion from the above; could be wrong

Labels like `L750de` are labels in `fe2/fe2.s`. `NNN(a5)` is an entry of the common jump table (name given
at first use, full list in section 3). `NNN(a0)` is a field of the ship's object (section 2.2).

## 1. Short version

- An AI ship is an object of module 11 (`94(a0) = $b`). Its **physics** runs every frame; its **decisions**
  run once per game second (batched: at 10000x once per frame, told 200 s passed).
- A ship is a state byte (`211(a0)`) plus a target (`210(a0)`). The states and what moves between them are in
  section 5. Hostiles (pirates) are four states: approach (`$10`), attack (`$4`, `$8`) and close pursuit
  (`$c`). Which one is picked depends only on a **distance exponent**, i.e. the base-2 size of the distance
  (section 4.1, a table in metres and kilometres).
- Every pirate targets the player. Nothing else. Groups are three independent ships that happen to start
  100 m apart; there is no leader, formation or messaging.
- **Lasers fire only in state `$8`, which is only entered at about 0.5 to 2 km** (and, in principle, inside 0.5 km if
  the aim test passes, which by my reading it can never do). Inside 0.5 km a pirate does not shoot; at the end it
  flies straight at the player at full thrust and **rams** (section 6.3). Two of two test pirates died that way.
- Missiles and mines are launched by the same attack code with a 1-in-8 roll per look (when aimed in the
  130 to 520 m band); an empty pylon falls through to an energy bomb check. Pylons are given out by ship quality.
- When the time-acceleration shortcut (spawning doc 2.7) fires, the pirate is **placed** next to the player.
  It does fire, for pirates, at 10000x: I measured an exponent going 27 to 9 inside one sample (section 8.2).
  The earlier doc said it never fired in the test; that was wrong.
- Being hit makes a peaceful ship hostile to whoever hit it; a ship hit by a missile runs; a hostile that is
  hit jinks and closes. Traffic ships in some states never retaliate (section 9).
- Pirate ship types differ by roll: a lone pirate is a class 6 (Krait, Gecko, Viper, Moray, Cobra Mk I,
  Constrictor, Cobra Mk III, Puma), a group is class 7 and is all fighters/light ships of one type (section 12).

## 2. How the AI is called

### 2.1 Entry points

[code] Module 11 (the ship module, table `L74226` area; entries are `bra.w` in a 4-byte-spaced table):

| Function | Label | What |
|---|---|---|
| 4 | `L74522` | events 1, 2, `$13`, `$10` (jump follow, arrival, periodic pirate roll) |
| 24 | `L750c0_mod11_fn24_OtherShipPhysics` | per-frame physics for AI ships and missiles |
| 28 | `L750de` | the AI step, once per game second (spawning doc 2.3) |
| 32 | `L74378` | "give me a ship of type d7" for other modules (mission, police) |
| 48 | `L75178` | a ship was hit / a ship is gone (section 9) |

`L750c0` (physics) first puts the elapsed time in `d2`, runs the common polar physics hook (`Lpol_phys`, police
fix) and dispatches on `211(a0)` through the table at `L750f0`. `L750de` (AI) dispatches on the same byte through
the same table: **two words per state, the first is the AI step, the second the physics step.** State numbers go
up in 4s and are the byte offset of the pair.

[code] Because AI is batched, at high time acceleration a ship makes one decision per frame and then flies
that decision for 200 game seconds. This is why a ship can fly through the player at 10000x without reacting.

### 2.2 Object fields used by the AI

Positions and the table are in the spawning doc 3.1. Additional fields, all [code]:

| Offset | Meaning |
|---|---|
| `0..17` | orientation matrix, 3 x 3 words, Q15 (about `$7fff` for 1.0). Columns: `0,6,12` right, `2,8,14` up, `4,10,16` **forward** |
| `98,102,106` | velocity (longs, relative to the parent) |
| `116` | per-state counter: missile age in AI looks; cargo/effect age; `L75020` also drains/fills it |
| `134,136,138` | angular rates (pitch, yaw, roll) |
| `140,142,144` | thrust request (x, y, z; `144` is forward). Set by the steering routines, clamped by `146..157` |
| `146..157` | min/max thrust per axis (words), from the ship's model data |
| `158` | equipment flags (long). Bit 8: has a hyperdrive. Bit 10: energy bomb. Bit 12: shield recharge x2. Bit 15: hull self-repair. Bit 16: cooling x4. Bit 14: always set by generation |
| `162` | flags of lost equipment (moved from `158` by a damaging hit) |
| `166` | equipment grade (0 to 13), indexes the three tables at `L74ac6`, `L74ae2`, `L74af2` |
| `168..171` | laser mounts, one byte each (0 = none). The AI only fills `168` (front) |
| `172..` | missile / mine pylons, one byte each (0 = empty) |
| `180`, `182`, `184` | shield now, shield max, hull now |
| `188` | laser cooldown (ticks), `190` heat (fire is refused above `$f000`) |
| `198,202,206` | wanted velocity change this step (long x 3): the steering result, used as a cap by the physics |
| `204` | saved state (a ship that "arrives" returns to it) |
| `210` | target object id; `211` AI state; `212` destination id (a station), `213` signed look-ahead exponent (see 4.1) |
| `214,216,218` | **aim offset** (words, mm x 1024) added to the target position when steering |
| `220,222,224`, `226` | the last target vector (scaled), `226` its distance exponent |
| `234` | the ship's "budget" while it is generated; afterwards its kill count (`addq #4`/`addq #1` in the fire code) |
| `236` | class (1 traffic, 2/4/10 police and navy kinds, 6 lone pirate, 7 group pirate, 8 mission hostile, `$a` police, `$c` named, then overwritten: `$fb` pirate, `$fa` cargo, `$fd` missile, `$fe` flash, `$ff` player) |
| `237` | a small count set during generation |
| `238` | bounty in Cr (pirates), or a pointer to a name string (mission ships) |
| `242` | laser energy reserve (long), starts at `L74af2[grade] << 24` and refills with time |
| `281` | per-ship timer for the frame re-choice (`L75248`) |
| `284` | countdown for the obstacle probe (`L43732`) |

[measured] A fresh lone Adder had grade 3, laser byte `$a1`, one pylon `$82`, hull 260, no shield; a Krait hull 180,
Gecko 220, Eagle Mk2 120, Cobra Mk III 480 with shield 36.

## 3. The common routines the AI uses

[code] `a5` table entries, by the offset the code calls them with:

| Call | Label | What |
|---|---|---|
| `714(a5)` | `L4388c` | **distance and closing law** to target `a1` with look-ahead `d7`; leaves the wanted thrust in `d0-d2`, the distance exponent in `d7`. No obstacle test |
| `720(a5)` | `L43732` | the same, with a periodic **obstacle probe** (4.3) |
| `774(a5)` | `L4350e` | distance only (no velocity term). Used by the close pursuit and the energy bomb |
| `780(a5)` | `L43596` | the target vector (used by the follower state) |
| `726(a5)` | `L43c1a` | clamp `d0-d2` to the ship's thrust limits `146..157` and store as the thrust request `140..144` |
| `696(a5)` | `L43338` | **turn toward** the vector in `220(a0)`: `228` (pitch) = minus the dot with the up column, `232` (yaw) = dot with the right column, roll 0. If the target is behind (dot with forward < 0) both are forced to the maximum `$4000` |
| `702(a5)` | `L433b4` | the same with the signs reversed, and when the target is **ahead** the rates are forced to maximum (`(x >> 8) xor $3fff`): this turns the ship **away** from the target |
| `678(a5)` | `L43322` | turn the thrust request into a rotation request (`232 = thrust.x / 8`, `228 = -thrust.y / 4`): lets a ship "lean" into its thrust. Used by the launch state |
| `684(a5)` | `L43456` | match the roll/orientation of the target (`d7 = 0`: roll only; `-1`: all three axes). Used by docking approach and the follower state |
| `732(a5)` | `L43d48` | apply the thrust request to the velocity, never past the wanted change in `198..206` |
| `672(a5)` | `L42e06` | integrate the angular rates / apply the rotation |
| `750(a5)` | `L43ed6` | gravity of the parent body, frame handling |
| `756(a5)` | `L440fe` | re-choose the parent frame (4.4) |
| `798(a5)` | `L45dc8` | fire laser slot `d0` (section 7.1) |
| `804(a5)` | `L4607e` | launch pylon item `d0` (section 7.2) |
| `864(a5)` | `L45d40` | **energy bomb** (section 7.3). Not the laser |
| `486(a5)` | `L44c96` | deal `d0` damage to `a1`, attacker `d7` (section 7.4) |
| `786(a5)` | `L3f460` | collision test of this ship against everything |

## 4. Steering

### 4.1 The distance exponent

[code] `L4351e` (inside `L435aa`/`L4350e`) takes the target position minus the ship's, in **mm**, adds the aim offset
(`L43626`), ORs the absolute values of the three components together and shifts the vector right until the OR is
below `$2000`. The shift count is the **exponent** `n`; it is what every threshold in the AI compares. It is the
bit-length of the largest component, so the real distance (the length of the vector) can be up to 1.7 times the
largest component.

| `n` | Largest component below | Where the AI uses it |
|---|---|---|
| 4 | 131 m | `<= 4`: close pursuit (`$c`) |
| 5 | 262 m | |
| 6 | 524 m | `<= 6`: close fight band |
| 7 | 1.05 km | `7, 8`: long fight band, state `$8` |
| 8 | 2.1 km | |
| 9 | 4.2 km | `< 10`: approach ends, hostile (`$4`) |
| 10 | 8.4 km | |
| 11 | 16.8 km | `> 11`: a hostile gives up and re-aims |
| 12 | 33.6 km | |
| 18 | 2.1 x 10^3 km | |

(In general the bound is 8192 x 2^n mm.)

[measured] A pirate placed 2 x 10^9 mm (2000 km) away reads 18, as the formula gives. A fresh pirate on the
1.4 x 10^9 km sphere reads 36, two less than the formula: for distances above 2^32 mm the code takes the 64-bit
path (`l43550`), which counts differently. Close in, where the AI thresholds are, the table is exact. It also agrees
with the spawning doc's one data point: a pirate 2.0 km away in state `$8`.

The exponent is stored in `226(a0)` after every steer call and is a handy thing to peek.

### 4.2 The closing law

[code] `L4388c` first reads the target's velocity relative to the ship (`L43676`) and normalises it (exponent `d6`).
Then, if `n + d7 - 8 >= 0` (far), it makes a wanted velocity change

    dv = (v_target - v_self) + k * direction / sqrt(|direction|)

so the **closing speed grows with the square root of the distance**: the usual constant-deceleration braking
curve, which brings the ship to rest relative to the target without overshoot when it can brake hard enough. For
short range there is no square-root term, just the velocity difference plus the direction. The result goes to
`198..206` (cap) and, through the ship's matrix, to the thrust request. The physics (`L43d48`) applies thrust but
**never past that wanted change**, so the ship does not oscillate around the target.

[inferred] The numbers fit what the spawning doc measured: about 2 x 10^3 km/s at 10^8 km range, and a few
km/s at 100 to 400 km, which is why nothing arrives at 1x.

### 4.3 The obstacle probe

[code] `L43732` (calls with `720(a5)`) counts `284(a0)` down by `14486(a6)+1` per look. When it runs out (about every
16 looks, quicker at higher time acceleration) it casts a probe along the ship's own velocity (`L409fc`, the game's
ray/sphere test through the frame's objects) with a length that grows with speed and with the acceleration index. If
the probe would hit something, the aim vector in `220(a0)` is replaced by half the hit surface normal plus a quarter
of the old direction, and `284` is cleared so it re-tests soon. [inferred] This is how ships avoid planets and
stations. Used by: attack (`$4`, `$8`), the default state `0`, fly-away `$1c`, tumble `$24`.
`714(a5)` (approach, missile, follower) has no probe.

### 4.4 The frame

[code] Every 20 AI looks (`L75248`: `281(a0)` counts down by the elapsed seconds, reload `$13`) a ship calls `L440fe`,
which re-chooses its parent object among the bodies in the system by comparing distance exponents against the
bodies' radii. The parent decides which gravity (`L43ed6`) and which coordinates the ship moves in. This is the
"a pirate's parent changed from 0 to 114 to 111" of the spawning doc. `Lframe_cand` is a host patch for stations.

## 5. The states

[code] Table at `L750f0` (AI step / physics step). `L74fb0` is the ordinary ship physics: clear skip counter, gravity
(`750`), thrust (`732`), rotation (`672`), position (`570`), heat/regen (`L75020`), hit flash, collision (`786`).

| State | AI step | Physics | Meaning |
|---|---|---|---|
| `$00` | `L7549a` | ordinary | Fly to target `210` (a ship with a destination). Probe-steers with `720`; while `n > 5` thrust + turn. At `n <= 5` go to `L754c2` (arrival logic, 9.2) |
| `$04` | `L756a0` | ordinary | **Attack, far.** Steer, no shooting. Sets bit 1 of `10476(a6)` every look (blocks Shift+F2..F5) |
| `$08` | `L756a0` | `L74fa6`: ordinary then **fire laser slot 0** | **Attack, firing** |
| `$0c` | `L75756` | ordinary | **Close pursuit** |
| `$10` | `L75606` | ordinary | **Approach** (`714`, aim offset kept) |
| `$14` | `L755ee` | ordinary | New hostile: roll the aim offset, go to `$10` |
| `$18` | `L75938` | ordinary | Idle: housekeeping only (no movement decisions) |
| `$1c` | `L75322` | ordinary | Leaving a station (9.4) |
| `$20` | `L75444` | ordinary | Pick up the destination in `212` as target, then state `0` (9.1) |
| `$24` | `L75a2a` | ordinary | Requested hostile (mission/other module, `L74378` types 1 to 4): approach until `n <= 10`, then `$4`; prints the "under attack" message `$98b9` to the player and stops the music |
| `$28` | `L757a8` | ordinary | **Evading a missile**: full forward thrust, turn away (702), random angular jitter each look (`134`, `136` += random) |
| `$2c`, `$30`, `$34` | rts | none (`L74fae`) | Orbital class 11 ships and similar: nothing |
| `$38` | `L7593e` | none | Leave the system: push 20 km away, then the arrival flash morph (9.5) |
| `$3c` | `L75360` | none | Waiting for an arrival time in `126(a0)` (inbound traffic), then flash `$40` |
| `$40` | `L753b8` | none | Arrival flash; at the end restore state `204` |
| `$44` | `L757e0` | missile physics | **Missile** (7.2) |
| `$48` | `L757de` (rts) | `Lmine_phys` | **Mine** (proximity fuse added by the issue 5 fix) |
| `$4c` | `L759f8` | cargo | Cargo canister: wait `282` ticks, go `$50` |
| `$50` | `L759e8` | cargo | Canister drifting; deleted after its age passes `$d` |
| `$54`, `$58`, `$5c`, `$60`, `$64` | various | effects | Debris, asteroid, smoke, dust, explosion pieces |
| `$68`, `$6c` | `L759a6`, `L75986` | none | Jump-out flash and the arrival effect object |
| `$70` | `L75280` | ordinary | Parked ship asking the station for a launch slot |
| `$74` | `L7557c` | `L74f70` | Docked: tell the station, go `$84` |
| `$78` | `L752aa` | none | Launch cleared: wait for the door, pick the launch point (9.4) |
| `$7c` | `L755d0` | none | Police ship held in its bay (host police fix, `Lpol_release`) |
| `$80` | `L7550e` | ordinary | **Docking approach** (9.3) and **follower** (a ship that flies in formation with a ship it follows) |
| `$84` | `L75262` | none | Parked in a station, waiting for its departure time |

The first line of the spawning doc's table has `$24` as "missile" and `$1c` as "fly-away"; that was wrong (a missile is
`$44`; `$24` is the mission hostile approach; `$1c` is the station departure).

## 6. Hostile behaviour

### 6.1 The sequence

[code] A pirate is created in `$14` (spawning doc 3.7), then:

1. **`$14` (`L755ee`)**: three random words `>> 3` go into the aim offset `214/216/218` (each -4096..4095, x 1024 mm = up to
   4.2 km per axis). State `$10`.
2. **`$10` (`L75606`)**: `714` with look-ahead 1 gives `n`.
   If the target (the player) is in state `$84` (docked or landed): clear the thrust and **do nothing**.
   If `n >= 10`: thrust (`726`) and turn (`696`).
   If `n < 10`: state `$4`; if the target is the player and not in hyperspace, cancel time acceleration
   (`92(a6)` = `L722b2`) and, if that changed anything, print `$98ac` (Ship is Under Attack), play effect 25 and start the
   battle music (if the option is on); then zero the aim offset (from here the pirate aims straight at the player).
3. **`$4`/`$8` (`L756a0`)**, every look: set the "under attack" bit; `720` gives `n`; then
   - `n > 11`: give up, back to `$14` (re-roll the aim offset, approach again). Hysteresis of 4.2 to 16.8 km.
   - `n` 9 to 11 (2.1 to 16.8 km): state `$4`, thrust + turn.
   - `n` 7 or 8 (0.5 to 2.1 km): **state `$8`**, thrust + turn. The physics step now fires the laser every frame.
   - `n` 5 or 6 (130 to 520 m, `L756e4`): **thrust cut to zero**, state `$8` tentatively, turn toward (696), then the aim test (6.2): if it
     fails the state is `$4` (no fire); either way the missile roll (7.2) happens.
   - `n <= 4` (under 130 m): state `$c`.
4. **`$c` (`L75756`)**: `774` (distance only) gives `n`.
   - `n <= 4` (under 130 m): **full forward thrust** (`d2 = $7fff` into `726`) and **turn away** (`702`). 
   - `n == 5`: thrust cut, turn away.
   - `n > 5`: state `$4` and back to `L756e4`.

[measured] The state changes in a 10x run, with the exponent read from `226(a0)`:

| Real s | State | Exp | Note |
|---|---|---|---|
| 0 to 45 | `$10` | 18 down to 10 | approach at 10x; time acceleration cancelled at the exponent 10 sample |
| 45.4 | `$4` | 10 | |
| 49 | `$4` | 9 | |
| 53.5 | `$8` | 8 | heat 72: the laser has fired |
| 56.5 | `$8` | 7 | heat 624 |
| 58.5 | `$4` | 6 | heat 880 then cooling: **no more shooting** |
| 59.6 | `$c` | 4 | |
| 60.1 | slot reused | 0 | the pirate had rammed the stationary player (player hull -240) and blew up |

The 10000x run had the same sequence from exponent 9 (section 8.2): `$4` at 9, `$8` at 8 (heat rises, so the laser
fires), `$8` at 7, `$4` at 6, `$c`, ram, the player lost 100 hull and the pirate was gone.

### 6.2 The aim test

[code] In `L756e4` the dot product of `220(a0)` (the target vector, scaled) with the forward column of the matrix must exceed
`$60000000` (after doubling) for the state to stay `$8`. The target vector `220` is scaled to a largest component
between `$1000` and `$2000` (4.1), so its length is at most about 14000, and the forward column is at most `$7fff`.
Even straight ahead the product is below `$60000000` by a factor of about 3.
[inferred] The test can therefore never pass, so in the 130 to 520 m band a pirate is always in state `$4` and does not
fire its laser; the 0.5 to 2 km band is the only place lasers fire. Not proven by a direct test; the measured heat falling at
exponent 6 agrees.

### 6.3 Ramming

[code] At `n <= 4` (130 m) the ship sets full forward thrust and turns away, but the physics clamp (the wanted velocity change
`198..206` is whatever `720` last computed) and the ship's own thrust limit decide how fast it is going when it gets there. With
the player stationary it flies into it.

[measured] 2 of 2 test pirates (an Adder-class ship at 10x, an Eagle Mk2 at 10000x) hit the player and were destroyed. The player took
240 and 100 hull. Whether a moving player would be hit as often I did not test; the player's collision radius is the Cobra's.

[inferred] The shape of the ending is a big part of why pirates "come out of nowhere" and then vanish: they fly at the player at 130 m, so
time at the close band is a few seconds.

### 6.4 Targeting

[code] Hostile ships have exactly one target, `210(a0)`:
- Pirates: the player (`L7490c` writes `670(a6)`).
- Mission ships (`L74378` types 1 to 4): the id the mission module hands in.
- Police: the player (`L74378` type 5, `L80f5c`).
- A ship that is hit by anyone retargets to the shooter (section 9), and a ship that evades a missile retargets to the
  missile's launcher when the missile is gone.

There is no "pick the nearest enemy", no leader/wingman logic and no target switching because a better one appeared.
Pirates and police do not shoot each other on purpose; a stray shot that hits the other will make that one retarget.

### 6.5 Leaving

[code] When a ship's target disappears (`L75a9a`, event 48 with `d1 = 0`) in a hostile state (`$4`..`$14`): if the system has a station
(`L74d10`, random one) the ship gets it as destination (`212`) and state `$20`; otherwise the ship is deleted. Police
(class `$a`) also clear the police-alert flags `138(a6)`, `137(a6)` bit 0.

## 7. Weapons and damage

### 7.1 Lasers

[code] `L45dc8` (798(a5)) fires mount `d0`: byte `168+d0`. It refuses if the byte is 0, the cooldown `188(a0)` is not 0, heat
`190(a0)` is above `$f000`, or the energy `242(a0)` is 0. The first shot of the player makes a crime (spawning/police,
section 10). The weapon byte's bits 3 to 6 (`& $78`) select a damage word from `L45f02`:

| Index | Damage | Kind |
|---|---|---|
| 0 | 264 | pulse |
| 1 | 50 | pulse |
| 2 | 250 | pulse |
| 3 | 1500 | pulse |
| 4 to 9 | 1, 4, 20, 100, 300, 600 | continuous beam, damage x time |

Pulse weapons set the cooldown to `$795c` (about 0.6 game s) and add 8 x damage to the heat. Beam weapons scale with the tick
and add less heat (`L45fac` shift). The beam is a ray (`822(a5)`, range `$7fffff` mm, about 8.4 km) through the frame's
objects: it hits whatever it actually crosses, so **the pirate's shots hit only when its nose really points at you**. Nothing
aims for it beyond the steering.

A hit goes to `486(a5)` with the shooter's id. The fire routine adds 4 to the shooter's counter at `234`.

[code] AI mounts come from `L74c76`: a random number 0..50 plus a class bias (`L74c4a`: lone pirates `$b2`... table by class), times
the ship's budget, picks a tier from `L74c62` (costs 0, 1, 3, 5, 20, 75, 200, 500, 900); the weapon byte is then
`L74c57[tier]`: none, none, `$88` (pulse 50), `$91` (pulse 250), `$a1`, `$aa`, `$b3`, `$bc`, `$c6`, `$cf` (beams).
[measured] Lasers seen on test pirates: `$88`, `$91`, `$a1`, `$aa`.

### 7.2 Missiles and mines

[code] In the `$4`/`$8` close band (`n` 5 or 6) `L756e4` rolls once per look: `A5_2RandInts` as a word below `$2000` (1 in 8) picks a slot
`(rand & 7) + 1` and calls `804(a5)`. The slot is `172 + slot - 1`. An empty slot returns "nothing launched", and
then the energy bomb is tried (7.3). So a ship with one pylon fires about once every 64 looks (8 x 8), a ship with many sooner.
[inferred] The roll is only reached in the 5 to 6 band, so in 1x play: missiles are launched at 130 to 520 m.

`L4607e`: `cmpi` item byte (`$c0`, `$c1` mines, `$82`, `$83`, `$84` missile, smart missile, naval missile). It makes a class 75 object with
model `(item & 7) * 2 + 6`, state `$44` (missile) or `$48` (mine, `btst #6`), class `$fd`, target = the launcher's target,
name "Missile" or "Mine", forward thrust `$442`. If the launcher is the player the police module is told about the discharge (a crime, once).

If the target is an AI ship in a state below `$28`, **that ship goes to state `$28` and targets the missile** (evade); a
player target gets message `$98ad` ("missile launched") and effect 25.

The missile (`L757e0`, `$44`): `116(a0)` counts AI looks up; at 60 it explodes. Each look: `714` with look-ahead 3, steer with a `$1108`
forward bias; at `n <= 4` it goes off (`Lmine_boom`, with the issue 5 blast).
Damage on detonation from `L75818`: 10 / 100 (mines), 200 (missile), 300 (smart), 400 (naval), 100 (nuclear, plus a special check for the
mission target). At 1x one look is one game second, so the missile lives 60 s; at 10000x it lives 60 frames, which is a long game time. [inferred]

When the target is gone (`L75ac0`/`L75a84`): a missile's `116` is set to `$3c` so it blows next look; a ship that was evading it
turns on the launcher (`212` of the missile) and goes to `$14`.

Mines are given to the low-quality ships: see 12.3.

### 7.3 Energy bomb

[code] `158(a0)` bit 10. `L45d40` (864(a5)) clears the bit, adds 4 to the kill count, plays effect 31, and calls
`618(a5)` over all objects: anything model `<= $4e` within exponent `$11` (1000 km) of the bomber (`L45d9a`, `774`) takes 258 damage.
Only 1 in 64 pirates (quality `>= $fc00`, budget `>= 4`, not class `$a`) carry one. It is used as the fallback of the missile roll.

### 7.4 What a hit does

[code] `L44c96` (486(a5)): takes `d0` damage, `a1` victim, `d7` attacker.

1. Tells every module that registered for hits (`48(a3)`): this is the reaction table of section 9.
2. Skip entirely for objects with flag bit 4 (id flag byte) or no data block.
3. Shield `180(a1)` absorbs first (sound 18); then hull `184(a1)` (sound 17).
4. When the hull is gone: `l44d80`: if the killer is the player, add the bounty (`238(a1)` x 10 into cash `734(a6)`, message `$8635`) for a
   pirate and a player with bit 6 of its equipment flags (I did not find what it is); add the ship's score to the combat
   rating counter `824(a6)` and print `$98b8` when the top 16 bits change. Then the explosion, and for models in `$14..$4e` a
   few cargo canisters (spawning doc 3.4h): `RandInt(6) - 2` of commodity 15, and up to `min(hull/2, 4)` of random ones from
   `L44ef6`.
5. If the hit does not kill: a random chance (`cmp.w d0,d3`, higher the more of the hull is gone) that a piece of equipment is
   moved from `158` to `162` (lost), via `L44fd6`; message `$98b2` for the player.

## 8. Time acceleration and the AI

### 8.1 The shortcut works

[code] `L4388c` end (spawning doc 2.7): `d6 = n - max(norm, 2) - idx - idx/4`, where `norm` is the exponent of the **relative
velocity** (4.2), `idx` the acceleration index. If `d6 > 8` nothing. Else the ship's cooldown is cleared; if
`d6 - 2 < 0` and the aim offset is not 0, the ship is **put next to the target**: new parent from the target, position =
target position + offset, velocity zeroed (`l439de`).

`n - norm` is the base-2 log of the time the ship needs to cover the distance at its current closing speed, `idx + idx/4` is
the log of the speed-up. So **the ship teleports when, at the present closing speed, it would arrive within about one
frame of accelerated time.** At 1x that is never. [inferred]

### 8.2 Seen

[measured] A pirate on the far sphere with 10000x set (idx 12), sampled every 0.5 s:

    0.2 s  exp 36        4.9 s  exp 36        9.6 s  exp 35        14.3 s  exp 34
    19.1 s exp 32        21.5 s exp 31       23.8 s  exp 27 (rate 10000x)
    24.4 s exp 9, rate 1x, aim vector saturated, state still $10

From exponent 27 to 9 in under 0.6 s, whereas each of the steps before took about 0.6 s. That is the shortcut: the position
was moved, the exponent fell by 18 (from about 10^6 km to under 4 km), and the first look afterwards cancelled the acceleration and gave the
warning. (With a closing speed of about 10^9 mm/s, `norm` is about 8..10, which puts the threshold at `n < 19 + norm` = 27.)

At 10x (idx 3) the pirate did not teleport; it flew in by the braking curve, taking about 45 s from 2000 km. Not tested: 100x and
1000x.

The earlier doc's arrival "by flying" at 10000x was the same event seen after the fact (the pirate was already 2 km away at the
first look).

### 8.3 Other effects

[code] The once-a-second batching (2.1) means: velocity changes ordered by a look are held for the whole batch; the physics
cap `198..206` is not refreshed between looks; a missile's 60 looks are 60 frames.

## 9. Reactions and traffic

### 9.1 Being hit and ships being gone (module 11, function 48)

[code] `L75178`: `d1 = 0`: "object `d0` is gone" (every ship is asked; `d2` = its id). `d1 != 0`: "ship `a0` was hit by `d7`".
Each state has a pair of words at `L751c0`/`L751c2`:

| State | Gone (target = this object) | Hit by `d7` |
|---|---|---|
| `$00` | none | `L75b3c`: **target := shooter, state `$4`** |
| `$04` to `$14` | `L75a9a`: find a station and go there (`$20`), else vanish | `L75ad8`: random angular kick (`136 += rand >> 6`) and state `$c` |
| `$18` | none | `L75b46` (crime report only) |
| `$1c` (leaving station) | none | `L75b1a`: release the bay, then as `$00` |
| `$20` | none | `L75b3c` |
| `$24` | none | `L75ad8` |
| `$28` (evading) | `L75a84`: target := the missile's launcher, state `$14` | nothing |
| `$2c`..`$34`, `$38`, `$3c`, `$40` | none | `L75b46` (crime report only) |
| `$44` (missile) | `L75ac0`: fuse set to 60 | nothing |
| `$48` (mine) | `L75ac0` | nothing |

`L75b46` reports to the police module (crime 10, once per `673(a6)` bit 3) when the hitter is the player. So **only a few states
retaliate**: a ship in the default state, leaving a station or on its way to a destination. A ship that is already hostile is made to
jink and close. Traffic that is jumping out, parked, arriving, or orbiting does not fight back, and shooting them is a crime.
[code] Nothing makes other ships come to a victim's aid.

### 9.2 Default flight and arrival (state `$00`, `$20`)

[code] `L7549a`: fly to `210`; at `n <= 5` (260 m) go to `L754c2`:

- If there is no destination `212`: leave (`l75946`, 9.5).
- If the destination is not the current target: target := destination, state `$00` via `L75444`; `L75444` also sets the aim
  offset: for an orbital station a point 6000 units x 1024 mm = 6 km out along the station's axis, for a ground starport a
  point 4 km up.
- If the target is the destination: station fn 6 (request clearance). If granted: aim offset := the door vector, play effect 26, state `$80`.
  If refused: stay.

### 9.3 Docking approach (`$80`, `L7550e`)

[code] `714` with look-ahead -1 (follow). While `n > 0` (outside about 8 m) thrust and follow (684 matches roll).
When `n <= 0`:
- if the acceleration is 1x (`14482(a6) == $3e2`): keep following. The ship docks by **collision**: in the physics step
  (`L74fb0`) a hit of the right kind (bits 6 and 5 of the collision result) while in `$80` runs `L75006` (station fn 3) and, if
  accepted, goes to `$74`.
- otherwise (accelerated): do `L75006` directly, copy the station's orientation, and finish.

So under time acceleration traffic docks without having to fly into the slot. [inferred]

### 9.4 Parked and launching

[code] `$84` waits for the time in `126(a0)`; stock made at arrival waits up to 16 days, ships that dock wait 2 to 3 days. Then `$70`
(`L75280`): ask the station (fn 5) for a launch slot; granted: effect 79, `$78` (`L752aa`): wait until the door (`159(a1)`) is
open, then `$1c` and pick a **launch point**: for an orbital station a random point 5 to 15 km out (`d3 = $1388`, `d4 = $3a98` through
the station matrix), for a ground base a point 5 km above plus up to 0.5 km. `$1c` (`L75322`) thrusts toward it (`720`, `678`
leaning) until it is within `n <= 6` (520 m) of it, then releases the bay (fn 2) and goes to `L754c2` (9.2).

### 9.5 Leaving the system

[code] `l75946`/`L7593e`/`L74ca2`: a ship with no destination looks at the neighbouring systems four times (`L74cb6`: the
starsystem module's neighbour list, accept if its distance rating is more than 3 and the galaxy module agrees), and if one is found
becomes the `$68` jump-out flash (the arrival flash model, `L7537c`); if none is found it is deleted. The state `$38` form first pushes the
ship 20 km out (`addi.l #$1312d00,44(a0)`).

### 9.6 Followers

[code] State `$80` is also used for ships flying with a ship they follow (`L7550e` used by event 2 for followers): while the
rate is not 1x they just copy the leader's orientation. Not well explored.

## 10. Police and missions

[code] Module `A6_mod_police` (`L80772_mod_police`), function 32 handles crime `d0`: `L80df8` looks up the crime in the 8-byte
records at `L80da0` (fine, flags, detection difficulty), adds to the player's legal status `11774(a6)` and the per-system
record, and may print the "Traffic Control" style messages (`L8108c`). `L80ebc` is the station's reaction: launch Vipers (`L80f5c`,
class 10 via `L74378` type 5) at `178(a1)` police strength. Their physics and the launch fix are in the original-game-fixes doc,
issue 7. The police ship targets the player.

Mission ships (`L7ec9e`, `L7e024`) use `L74378` types 1 to 4 and 5/7/8 to place hostiles; they run the same states (`$24` for the
far spawn). Not explored further.

## 11. Followers and the hyperspace "ships on you"

See the spawning doc 3.4e. Only pirates (`$fb`) with bit 8 of `158` set (6% of the ships with the right quality, and all of class 8)
follow the player into hyperspace if they were within exponent `$10` (540 km). [code]

## 12. Ship and loadout generation

### 12.1 Which model

[code] `L74a38` asks the station module (`68(a6)`, function 32, `d1 = 8`, `L5fa70`) for a model for class `d0`. The module has 13
header entries (a count and an offset) and a list of model numbers (model = byte offset / 2):

| Class | Models | Used for |
|---|---|---|
| 0, 1 | by the system's `1246(a6)`; the table treats entries `2t + class` | inbound/stock traffic |
| 2 | Eagle Mk2, Osprey x3, Adder, Puma x2 | police-ish (`L74378` type 8) |
| 3 | IXI, Eagle Mk2, Puma x2, Imperial Courier x2 | |
| 4 | IXI, Eagle long range, Falcon, Hawk, Kestrel, Adder, Cobra Mk III | navy (type 7) |
| 5 | Gecko, Adder, Viper, Moray, Constrictor, Cobra Mk III, Lion, Asp, Python, Transporter, Boa | traders |
| **6** | **Krait, Gecko, Viper, Moray, Cobra Mk I, Constrictor, Cobra Mk III, Puma** | **lone pirate** |
| **7** | **Osprey x2, Falcon x2, Hawk, Kestrel, IXI, Eagle LR, Eagle Mk2, Adder, Eagle Mk3 x2, Sidewinder x2, Krait x2, Gecko, Viper** | **pirate group (all three the same model)** |
| 8 | Gecko, Viper, Moray, Cobra Mk I, Cobra Mk III x2, Constrictor | mission hostile (types 1 to 4) |
| 9 | Interplanetary Shuttle, Lifter | |
| 10 | Adder x2 | (police Vipers are given their model elsewhere) |
| 11 | Lynx x2, Long-range Cruiser | |
| 12 | Lion, Asp, Python, Transporter, Boa x2, Tiger, Imperial Trader, Anaconda, Puma, Imperial Courier | named ships at stations |

[inferred] The numbers in the class column are the `236` byte, not the `d5` the table is indexed by for classes below 2; I did not
fully untangle the 0/1 entries with the system tech value.

[measured] Seen: Krait (21), Gecko (22), Adder (23), Cobra Mk III (27) as lone or group ships, and an Eagle Mk2 (18) group.

### 12.2 Equipment and budget

[code] `L74a38` continues with a random 16-bit **quality** `q` (class `$a`: from the system; class `$c`: `$ffff`), the model's budget
`234(a1)` and the model data. Rough order:

1. `q < $c000`: low byte of the model's equipment word; `$c000..$efff`: high byte; `>= $f000` and not class 1: high byte plus the
   **hyperdrive** (bit 8, budget -1). Class 8 always gets the hyperdrive and the high byte.
2. Equipment grade `166` from that, with the cost `L74ac6[grade]`, the laser energy from `L74af2` and a count byte from `L74ae2`.
3. The front laser (7.1).
4. Shield: random fraction of the leftover budget (at most 300), times `q`; shield max in `182`.
5. Pylons: `RandInt(pylon_count + 1) - 1` of them filled with the item byte `L74c20[q >> 12]`:
   `$c0, $c1, $c1, $82 x 9, $83 x 3, $84`. **Low quality means mines**, high quality means smart/naval missiles.
6. Bit 2 (a feature in `158`) with chance 1/32 for `q` high, bit 3 or bit 7 for the next ranges, bit 10 energy bomb for
   `q >= $fc00` with budget >= 4 (not class `$a`), bit 14 always.
7. The rest of the budget becomes extra hull points, capped.

### 12.3 Bounty

[code] `L749aa` after the build: class `$fb`, bounty `238` = a word of the table at `L749c6` indexed `d2 + (rand & $1e)`, `d2 = 8` for a lone pirate and
`0` for a group member. Lone pirates: 6 of 16 rolls are 0, otherwise 50, 100, 125, 150, 175, 200, 250, 300, 400 or 500 Cr. Group
members: 10 of 16 are 0, otherwise 50 to 200. [measured] Bounties seen: 50, 100, 125, 175 and many 0.

## 13. How the measurements were made

Same harness as the spawning doc section 7: a test instance from `devtools/run`, `--nosound --control FIFO` and the script engine (`peek`,
`poke`, `event 16`), system Castor `a935c740` at 10^9 km. Extras:

- `event 16` repeated fills the object table with pirates quickly (more than 50 after 40 events); freeze extras by poking their
  state to `$18` and the player's hull (`184` of the player object) to 30000.
- Object `n` is at `big_space + $d4 + 286 * (n - 1)`; `big_space` is the long at `a6+14466`. Player: id 107 (long at `a6+674`).
  Peek `226(obj)` (word, exponent), `211` (state), `190` (heat), `184`/`180` (hull/shield).
- To get a pirate into range: poke its parent `68(obj) = 107`, position longs at `44/48/52` (low) and `56/60/64` (high), velocity
  `98/102/106` to 0, target `210 = 107`, state `$14`. The game re-parents it within a few looks.
- Set a rate: `poke l a6+10428 N; poke w a6+10432 I; poke l a6+14482 N; poke w a6+14486 I` (spawning doc 2.1).
- Player death or a table full of pirates ruins a run; the second run of a session in which 40 events were sent could not find a
  free slot, and one pirate slot was reused mid-run.

## 14. Not settled

- That the aim test in `L756e4` can never pass (6.2); a direct test: put a pirate straight ahead at 300 m and see whether it
  ever reaches `$8`.
- The player-side behaviour of rams: a moving player, a player with shields, the damage a ram does to a pirate.
- The 100x and 1000x teleport exponents (section 8).
- The 0/1 class lists and how `1246(a6)` selects them (12.1).
- What bit 6 of the player's flags at the bounty payout is (7.4).
- How followers (`$80` as a formation ship) are created in practice.
- How mission ships pick targets other than the player.
- Whether the obstacle probe (4.3) really keeps ships off planets and stations; I never saw it trigger.

## 15. Possible changes (not made)

1. **Make the aim test work** (`cmp.l #$60000000,d0` in `L756e4`): normalise `220` first, or lower the constant to about `$10000000`. Then
   pirates in the 130 to 520 m band shoot, which is probably what the game meant.
2. **Do not ram**: at `n <= 4` thrust forward only if the ship is not closing fast, or turn away first. The simplest: use the
   `n == 5` branch (thrust cut) for `n <= 4` too and let the physics brake.
3. **Spread out groups**: give each member its own aim offset (`L755ee` already does) and a start distance; today they start 100 m and
   300 m apart and use the same models, so they arrive together.
4. **Stop the teleport** when the player has not asked for time acceleration: in `L4388c` skip the shortcut for hostile states, so the
   pirate has to arrive by flying (and the warning at exponent 10 has something to react to). Together with spawning doc change 5 (step the
   rate down), this removes "out of nowhere".
5. **Retaliation for more states**: add `L75b3c` to the hit table for `$38`, `$3c`, `$40` and the station states so traffic fights back.
