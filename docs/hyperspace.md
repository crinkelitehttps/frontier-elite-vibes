# Hyperspace in Frontier: Elite II

Findings from reading `fe2/fe2.s` (the original game; `fe2_modded.s` has the same layout). This continues
`docs/time-acceleration-and-ship-spawning.md` ("the spawning doc") and `docs/npc-ship-ai.md` ("the AI doc"). It
answers one question first: **what decides where the player is put after a jump**, and collects everything else
the docs and the code say about the jump sequence. It corrects the spawning doc in one place (section 1).

Every statement is tagged:

- **[code]** read from the disassembly, not run
- **[measured]** seen in a test instance
- **[inferred]** a conclusion from the above; could be wrong
- **[unknown]** not worked out

Labels like `L814dc` are labels in `fe2/fe2.s`. `NNN(a6)` is a game global, `NNN(a5)` an entry of the common jump
table, `NNN(a0)` a field of the object `a0` (the player here).

## 1. Short version

- The player's arrival position is chosen **at the start of the jump, not at the end**, by `L814dc`. The player is put
  on a random point of a sphere of radius **1.1 to 1.7 x 10^9 km** (mantissa `$4000..$5fff`, exponent 51) around the
  root of the old system. The new system is generated afterwards and the player is **not moved again** [code].
  The same sphere radius family as the pirates' (1.407 x 10^9 km), not 64 times further out.
- The spawning doc said the player arrives at `$5600`, exponent 57 (9.7 x 10^10 km). That placement exists, but only
  in the **misjump** landing branch, which by my reading of the outcome table is never reached (section 6). The doc's
  "64 times further out" and its measured "9.1 x 10^10 km after `enter`" were wrong: the `enter` cheat does not call
  the placement at all (section 8).
- `1044(a6)` is the **jump distance**: the distance between the galaxy map's reference position (where the player is
  in the galaxy) and the selected star, in the units of the jump range check. It is written by the galaxy map
  module whenever a target is picked, read by the jump check for fuel, range and duration, cleared when the jump
  starts, and set again only by the misjump code. It only changes the arrival **after a misjump**, where it scales the
  landing radius (section 5).
- The direction is random in every case; the old position plays no part (the placement routine has no player term).
- A jump takes at 261000x (rate `$f880000`, index 18) a time stored in `13882(a6)`, which comes out of the same range
  check that uses `1044(a6)`.

## 2. The globals

[code] unless marked.

| Global | Meaning |
|---|---|
| `704(a6)` | flight state. `0` flying, `8` hyperspace, `$48` also lets a jump start (not identified), `$c` and below are the states where the jump-possible icon is drawn |
| `726(a6)` | current system id; `840(a6)`, `836(a6)`, `832(a6)` the last three (shifted by `L8145e`) |
| `844(a6)` | set at jump start from `1060(a6)` (the target id, see below) |
| `936(a6)` | 3 longs: the galaxy map's reference position, i.e. where the player is in the galaxy |
| `948(a6)` | 3 longs: the selected star (the jump target) |
| `1042(a6)` | low byte of a target star's type / number info (set in `L849a2`) |
| **`1044(a6)`** | **jump distance** (long): distance from `936` to `948`, see section 3 |
| `1052(a6)`, `1056(a6)`, `1060(a6)` | derived by `L849a2` from the selection: `1060` is the target system id in the same form as `726` |
| `1064/1066/1069(a6)` | the target's system record bytes (from the system module, `60(a6)` fn 32) |
| `1072(a6)` | the flag word the galaxy screen keeps for the jump-icon |
| `13882(a6)` | **flight time** of the jump (long), result of the range check |
| `13886(a6)` | low word of `1044` kept for the arrival (followers' range check) |
| `722(a6)` | hyperspace countdown, loaded from `13882` |
| `13328(a6)` | table of the player's legal status per system, index `1246(a6)` x 4 (saved before the jump, restored after) |
| `977(a6)`, `692(a6)` | tested at jump start: `977` must be non-zero (a target is selected, inferred), `692` at least `$b71b00` (a timer of the drive, unknown) |

## 3. `1044(a6)`: who writes it

[code] Every writer in `fe2/fe2.s` (the only ones; searched `1044(a6)`):

| Where | What |
|---|---|
| `L83e54` (galaxy module, event 1) | `clr.l 1044(a6)`, `clr.w 1072(a6)`, and copies the selected star `948` over the reference position `936`: "the player is now at the star". Event 1 is broadcast by the jump start, **after** `L814dc` has saved the value |
| `L849a2` (selecting a target star on the map) | `1044 := L84ae6(948 - 936)`: the distance from the reference position to the selected star. Also fills `1052..1066`, and asks the system module for the target's record. If the result is 0, `1072` is cleared (no jump) |
| `L83e70` (galaxy module, **event 20**, `d1 < 4`) | jitter `936` by three random words (`rand`, `rand`, `rand` full size), then `1044 := distance(726's star, new 936)`. Then it searches stars upward from the current system (`L83eb2`, `L83c32` until one is more than `$50` away) and **selects that one** (`948 := it`, `L849a2` called), see section 6 |
| `L83ee6` (same handler, `d1 >= 4`) | the jitter in x, y is `rand >> 8` (small), z full; `1044 := distance(726's star, new 936)`; no new target |

Readers: the map screen text (`L8490e` prints it as the distance), the jump-possible icon (`L63ecc`, range check
mode 1), `L849a2` itself (a zero means no target), and the jump start `L814dc` (range check mode 2).

`L84ae6(d0-d2 minus d3-d5)`: the three differences are shifted right by 8; if they fit in 16 bits the vector length is
taken (`A5_VectorLen`), times `$c8b4`, shifted right by 14 (that is a factor of about 3.14). The label at its start
makes `L83c32` mode 2 return the position and `L84ae6` the distance.
[inferred] The unit is the one the range table `L812ce` uses (the maximum distance a drive of that grade can
jump); the map prints it as light years with a decimal. I have not checked the scale.

[code] The effect on arrival is in section 5.

## 4. The jump start (`L814dc`)

Entered from module 140(a6)'s function (`bra.w L814dc` in the dispatcher at `L81206`), i.e. when the player confirms a
jump. In order:

1. **Checks** (return if any fails): `704(a6)` is 0 or `$48`; `977(a6)` is non-zero; `692(a6) >= $b71b00`;
   then the range / fuel check `L81236` mode 2 (section 7) returns a negative `d0` -> nothing happens.
2. Save the legal status of the old system: `13328[1246(a6) x 4] := 11774(a6)`.
3. `13886(a6) := low word of 1044`, `844(a6) := 1060(a6)`.
4. **Range check** `L81236` (mode 2, `d1` = the jump distance): returns the flight time `d1`, stored in `13882(a6)`.
   It also takes the fuel. Zero thrust, `816(a6) := d5` (fuel left, inferred).
5. Hyperspace clock: `600/604(a6)` (the event 21 limit) := now + `d1` (so event 21 fires at arrival).
6. `704(a6) := 8` (hyperspace). Broadcast event **1** (`450(a5)`): the ship module collects followers (spawning doc 3.4e), the
   galaxy module clears `1044`.
7. `510(a5)`: free the whole object table. Then re-flag the player (`528(a5)`, `d0 = 79`), `68(a0) := 0` (**parent =
   the system root**), `69 := 0`, `708`, `732`, `281(a0)` cleared.
8. **Place the player** (this is the arrival position): `d6 := RandInt($2000) | $4000`, `d7 := 51`, `bsr L816e8`.
   See section 5.
9. Effect 19. Then `A5_2RandInts` and the misjump roll (section 6).
10. Normal case: event **19** (clean up), `L8145e` (generate the new system), event **2** (arrival: the ship module
    runs the population routine, spawning doc 3.3, and re-places the followers around the player), then
    `1196(a6) := $7fff0033`, `1183(a6) := $72`, `722(a6) := 13882(a6)`, rate `14482(a6) := $f880000`, index
    `14486(a6) := $12`, and the legal status of the new system `11774(a6) := 13328[1246 x 4]`.

[code] So the whole of the new system, including its pirates and traffic, is **generated at the start of the
tunnel**, not at the end. The player and the pirates therefore already stand at their positions while the
countdown runs.

## 5. The placement `L816e8` (and `L459c0`, 576(a5))

[code] `L816e8` takes `a0` = the object to place (the player), `d6` the mantissa and `d7` the exponent. It draws
random angles (`A5_2RandInts`), calls the polar placement `L459c0` (576(a5), also used for every spawn, spawning doc 3.2),
clears `19(a0)`, `138(a0)`, `142(a0)`, velocity `98(a0)..` and the velocity cap `198(a0)..`, and `A6_plr_speed_set`.

`L459c0`: the point is **the unit vector from the two random numbers, scaled by `d6`, then shifted by `d7 - $3d`**
(`asr`/`asl` over the 3 x 32 bit position at `44(a1)..64(a1)`). `d6` is used as a **word**: `muls d6,..` (signed) and
`move.w d6,d3`. Position radius is `d6 * 2^(d7-15)` mm (spawning doc 3.2, measured for pirates).

Callers for the player:

| Where | `d6` | `d7` | Radius |
|---|---|---|---|
| Jump start (step 8) | `RandInt($2000) \| $4000`, i.e. `$4000..$5fff` | 51 | **1.13 to 1.69 x 10^9 km** (pirates: `$5000`, 51 = 1.407 x 10^9 km) |
| Misjump landing, `1044 = 0` | `$5600` | 57 | 9.68 x 10^10 km |
| Misjump landing, `1044 != 0` | `(1044 & $ffff) x $151` (a long, used as a word) | 63 | about 9.5 x 10^10 km **per unit of `1044`** |

[inferred] The `1044` case: the landing radius is proportional to the distance of the player's galactic position from the
star. A drop-out far from where the map said you were puts you proportionally further from the star. Because `d6` is
used as a signed word, `1044 x $151 >= $8000` (i.e. `1044 >= 98`) wraps (and flips the sign of the result); the
unit is not known, so I do not know whether that happens in play. If `1044` is 0 the default radius is used. In a
normal jump `1044` is 0 when this code runs (event 1 cleared it, and the normal path never reaches the third row).

The **old** position of the player does not matter in any case. The jump start sets `68(a0) = 0` (parent = system root) and
the placement overwrites the position; velocity is cleared.

Therefore, normal jump: **the player is put on a sphere of 1.1 to 1.7 x 10^9 km** in a random direction, and the generated
pirates sit on a sphere of 1.407 x 10^9 km in another random direction. Their separation is about 2 x 10^9 km on
average [inferred, from two random directions on spheres of that size].

After the countdown, `L63362` (hyperspace exit) only: ends hyperspace (`704 := 0`), drops the rate to 1x (`L722b2`), spawns the arrival
flash object (model `$ba`, state `$6c`, near the player) and starts the music. [code] It does not move the player.

## 6. The misjump roll

[code] After step 9 of section 4:

    d0 := A5_2RandInts
    if d0 > $fe00:            message $98ae, misjump          (about 0.8 percent: 511 of 65536)
    else if 16398(a6) != 0:   message $98af, misjump
    else if 16400(a6) != 0:   message $98af, misjump
    else:                     normal jump (step 10)

`16398(a6)` and `16400(a6)` are only **read** in `fe2.s` (searched): nothing in the code writes them by that name, so
they are set by a table fill or another module. [unknown] What they mean. [inferred] A possibility is that they
are "drive damaged" or "hyperspace disrupted" flags, but there is nothing to support that.

On a misjump, event **20** is broadcast (galaxy module: jitters the reference position and may pick a new
destination, section 3), then a random outcome `d1` from the table at `L8160a`:

    $2 x 11, $3 x 2, $4, $5, $6        (16 entries: 11/16, 2/16, 1/16, 1/16, 1/16)

and a branch on it (the table values are the outcome numbers; "0" and "1" do not appear in the table as written, so the
first two branches below are dead):

| Outcome | Probability | What happens |
|---|---|---|
| 2, 4 | 12/16 | `l81658`: print the message; `722 := 13882`, hyperspace rate; **no new system is generated**: the player comes out in the old system at the sphere position of step 8 |
| 3 | 2/16 | `L816b6` first (the **drive is damaged**: `166(a0) := 1`, the mass of the old drive less 4 is added back to `814/742/738(a6)`, message `$98b0`), then as 2 |
| 5 | 1/16 | as 2 |
| 6 | 1/16 | `jmp 468(a5)` with the player's id (`670(a6)`): **destroys the player object** [inferred: `468(a5)` is a delete / kill; I did not read it] |
| 0 | (not in the table) | `l81686`: print the message, `L8145e` (new system), event 2, **land at the far radius** (`$5600`/57, or the `1044` form), hyperspace timer |
| 1 | (not in the table) | `L816b6` (drive damage), then as 0 |

[inferred] The table contains no 0 or 1, so the `l81686` landing at the far radius is **not reached** with the
shipped table. The `$5600 / 57` placement is therefore dead code in this build, and the radius `L816e8` really
gives the player after any jump is the one from step 8. I read the table bytes as `2 x 11, 3 x 2, 4, 5, 6`
(`dc.b $2,$2,$2,$2,$2,$2,$2,$2,$2,$2,$2,$3,$3,$4,$5,$6`, 16 bytes). **Check this with a test before relying on it.**

[code] Event 20 is sent with `d1` = the outcome number (`moveq #0,d1 / move.b L8160a(pc,d0.w),d1` before
`moveq #20,d0 / jsr 450(a5)`). The galaxy handler (`L83e70`) takes `d1 < 4` (outcomes 2 and 3, 13/16 of misjumps)
through the big jitter plus star search, which **selects another star** on the map and recomputes `1044` for it;
outcomes 4 to 6 take the small jitter (`L83ee6`) and only recompute `1044`. Either way the reference position `936`
has been moved, so the galaxy map afterwards thinks the player is somewhere else, but nothing re-places the
player or generates that star. [inferred] A misjump therefore leaves the player in the old system, at the step 8
sphere position, with a shifted galactic position and possibly a different selected target. I have not tried it.

## 7. The range check `L81236` and the flight time

[code] `L81236` is the module-140 function 32 with modes in `d0`:

| `d0` | Meaning |
|---|---|
| 0 | `L8145e`: generate the system chosen on the map (the cheat `cheat_enter_system` calls this, with `d6` = the id) |
| 1 | check only (the jump icon) |
| 2 | check and **pay** |
| 3 | another table (the L8138a branch, equipment lookup) |
| 4 | `l8145c` |

For modes 1 and 2: `a0` the ship, `d1` the distance. The ship's drive grade `166(a0)` indexes the table at `L812ce` (8 bytes per grade): `d2`
is the maximum distance divided by a mass term from the ship model (`d4 = 6(a3)`), and fails (`d0 = -1`) when `d1` is
larger than `d2` or zero. The fuel is `d4 = d1 x mass / $4e21 + 1`. For the **player** (`93(a0) == 670(a6)`) the fuel
comes out of `792(a6)` (or `794(a6)` for the grade >= `$a` branch), and mode 2 subtracts it; for **any other ship**
the fuel is the byte `237(a0)`, which mode 2 decrements (so `237` in the AI doc is the NPC's fuel). The flight time
returned in `d1` is `swap`ped and divided by `d2`, times 7 (`mulu #7`), so it is proportional to the distance over
the maximum distance of the drive. It is stored in `13882(a6)` and drives `722(a6)`.

[inferred] The followers (spawning doc 3.4e) are put through this same check at arrival (`L74652`: mode 2 with `13886(a6)`):
a follower arrives only if its own drive can make that distance and it has the fuel, otherwise it is deleted.

## 8. What was measured, and what that really showed

- The spawning doc 3.2 gave a row "Player after a jump (`L816e8`), `$5600`, 57, 9.7 x 10^10 km, about 9.1 x 10^10 km seen after
  `enter`". The `enter` cheat (`cheat_enter_system`, `src/cheats.c`) does **not** run the jump start. It frees the
  objects, sets the player's parent to 0 and runs the generation `L8145e`. It never calls `L816e8`, so "after `enter`"
  says nothing about the arrival radius; the cheat comment itself says "the ship is left nowhere in the new
  system". The 9.1 x 10^10 km was the player's position carried over from wherever it already was [inferred; I did not
  find where it came from].
- So nothing in the notes **measures** the real arrival position of a jump. What is claimed in section 5 is from the code only.
- The next measurement to take: make a real jump from a script (`Shift+F? / jump` is not wired to a script command; a
  save made with a target selected would do), peek the player's position (`44/48/52`, `56/60/64`) at the end of the
  countdown, and compare with `[1.13, 1.69] x 10^9 km`. Check also the table bytes at `L8160a` in the running image.

## 9. Consequences

[inferred] unless marked.

- **Arrival is next to the pirates' radius, not outside it.** The player is 1.1 to 1.7 x 10^9 km from the system root, the
  pirates 1.407 x 10^9 km; random directions, so 0 to 2.8 x 10^9 km apart. A pirate has to cross about 2 x 10^9 km on
  average at the sqrt braking law (spawning doc / AI doc 4.2), about 5 game days.
- **The pirates exist during the tunnel.** The arrival population (`RandInt(4 x d3)` rolls, spawning doc 3.3) is created at
  step 10, before the countdown. During the tunnel the clock runs at 261000x (index 18), so the AI shortcut (AI doc 8.1:
  `d6 = n - norm - 18 - 4`) can fire for them. The approach code (`L75606`) will not cancel the rate and print the
  warning while `704(a6)` is 8 (hyperspace), so a pirate could be put next to the player during the tunnel and
  wait there. [unknown] Nobody has run this; it would explain "the pirates are already on me when the tunnel ends".
- **Followers re-placed at 5.2 km** (`L746ba`) are around the player's position, which is on the same 1.1 to 1.7 x 10^9 km sphere.
- **Event 16** (the weekly pirate roll) is skipped in hyperspace (spawning doc 2.4), so only the arrival roll creates
  pirates during the tunnel.
- The cost of a longer jump is flight time and fuel only. The arrival radius does not depend on the distance jumped
  (the `1044` form is unreachable, section 6).
- The legal status is per system: saved into `13328[1246 x 4]` at the start, restored from the new system's slot after.

## 10. Not settled

- That the misjump table really has no 0 or 1 entries (section 6), and so that the `$5600`/57 landing and the `1044` scale
  never happen. Check by reading `L8160a` in a running image.
- What `16398(a6)` and `16400(a6)` are and who sets them.
- What `692(a6)` (the `$b71b00` test) and `704(a6) == $48` are.
- The scale of `1044` and whether `1044 >= 98` can be reached.
- Whether the pirate shortcut fires during the tunnel (section 9), and whether the arrival-position radius really is 1.1 to 1.7 x 10^9 km on a real jump.
- Whether `L8145e` or any module's event 2 / event 19 handler moves the player (I read the ship module's and the cheat
  that calls `L8145e`; I did not read every module's).
- What `468(a5)` does in the outcome-6 branch.
- The mapping of the event numbers: 1 jump start, 2 arrival, 19 clean up, 20 misjump, 21 clock limit (spawning doc 2.4).

## 11. Possible changes (not made)

1. **Arrive further out, or where the player chooses.** `L814dc`'s placement could use a larger exponent so the player starts
   outside the pirates' sphere. A one-byte change: the `moveq #51,d7` before `bsr.w L816e8`.
2. **Do not generate the pirates until the tunnel ends**: move the event 2 broadcast after the countdown, or
   skip the arrival roll in `L74732` while `704 == 8`.
3. **Let the player arrive near a body** (a planet or the main star): replace the sphere with the position of the nearest planet
   plus a distance. Larger change; would also need the parent set properly.
