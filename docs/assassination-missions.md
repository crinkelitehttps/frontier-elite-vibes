# Assassination missions

How the original game makes, hands out, runs and pays assassination contracts, and why a target is often
never seen. From `fe2/fe2.s` (the original game; `fe2_modded.s` has the same layout) and from scripted test
runs. Companion docs: `starport-economy.md` (the bulletin board and the system-wide offers list; section 5
is the 42-byte contract record), `station-screens.md` (the screen state machine), `npc-ship-ai.md`
(ship states), `time-acceleration-and-ship-spawning.md` (object table, clock).

Every statement is tagged:

- **[code]** read from the disassembly, not run
- **[measured]** seen in a test instance (how: section 8)
- **[inferred]** a conclusion that could be wrong

Text ids are offsets into the string table `L79280`: id `$98xx` is entry `id - $9800`.

## 1. Two sources, one contract

There are two generators. Both end up as the same kind of contract in the player's list and are run and
paid by the same code.

| | Civilian | Military |
|---|---|---|
| Where | each station's bulletin board | the Federal / Imperial missions list (`9792(a6)`, screen `$e`) |
| Generator | `L60092` | `L5f704` (one of the `L5f67e` handlers) |
| Ad text | `$981e..$9821` ("REMOVAL REQUIRED", "BIOGRAPHICAL", "WANTED", "RETIREMENT") | `$9856` "Assassinate a ... on ..." |
| Gate | combat rating, when the ad is opened | military rank margin, when the offer is made |
| Contract kind (low nibble of byte 0) | 0 | 1 Imperial, 2 Federal (`1246(a6)`, the system's allegiance) |

Kinds 3/4 are the photograph jobs and 5/6 the destroy-a-base jobs (`L7f334`).

## 2. Civilian ads (`L60092`)

[code] `L60092` is one of the 16 bulletin-board news handlers (`L5fdca`, `starport-economy.md` section 3),
picked 2 times in 32. Each station rolls `4 x 1236(a6) + 1` times on arrival in a system, and the periodic
event `$f` (`L5f8ca`) replaces one random entry per station.

- **Target system.** Up to 4 tries. Each: `RandInt(3)` picks a distance band (0 = the current sector,
  1 = one of the 8 neighbouring sectors (`L83be2`), 2 = two sectors away (`L83bfa`)), then the star-map module
  (`28(a6)` fn 32, `d0 = 1`, `L83b46`) picks a random system in that sector; it fails if that is the current
  system. The system data module (`60(a6)` fn 32, `L4beee`) must return a byte 4 of at least 3. That byte is
  per system in the hand-made table at `L4c122`; for any other system it comes from the default record chosen
  by squared distance from Sol's sector (`time-acceleration-and-ship-spawning.md` 3.3): only records within
  about 5 sectors of Sol have 3 or more. So targets are always in or near the core.
- **Who and how much.** One roll (`rand & $3c`) indexes `L60112` (16 entries of base pay and name kind). The
  pay is the base plus `$1000 x distance band`.

| Name kind | Text | Base pay |
|---|---|---|
| 0 `$999d` | plain name | $3000, 3500, 4000, 4500, 5000 |
| 1 `$999e` | "X the merchant" | $6000, 6500, 7000 |
| 2 `$999f` | "Mr X of Y Corp." | $8000, 8500, 9000 |
| 3 `$99a0` | "Director X of Y Corp." | $10000 |
| 4 `$99a1` | "Mafia boss X" | $13000 |
| 5 `$99a2` | "President X of Y Corp." | $15000 |
| 6 `$99a3` | "Governor X" | $17500 |
| 7 `$99a4` | "Senator X" | $20000 |

- **Date.** `day - $11d5a0 + 7 + (rand & $3f)`, kept as a **word** (days since 1 January 3200; the game
  starts on day `$11d5a0`): 7 to 70 days ahead. `L775b4` adds `$11d5a0` back. The word wraps 65536 days
  (about 179 years) after the start date; nobody will reach that.
- **Departure time.** The ad's random long (`a2`, also the name seed) doubles as the time of day the target
  leaves (the game's time of day is a 32-bit fraction of a day, `524(a6)`).
- **Combat rating gate** (`L775b4`, when you open the ad): `824(a6) >= (seed & $ffff) / 4 + $1000`, else
  text `$99ae` ("...we want someone with a higher Elite Federation combat rating"). `824(a6)` is the combat
  score, 256 per kill point (thresholds `L7e87a`: Mostly Harmless 4, Poor 8, Below Average 16, Average 32,
  Above Average 64, Competent 128, Dangerous 1000, Deadly 3000, Elite 6000 points). The gate is 16 to 80
  points, so Below Average sees some ads and about 80 points sees them all.

## 3. Military contracts (`L5f704`)

[code] The system-wide list `9768(a6)` (25 entries of 24 bytes, `starport-economy.md` section 4) is filled by
`L5f67e`, which picks one of 16 handler slots (`L5f6a0`): 8 basic delivery, 2 delivery tier 2, 2 delivery
tier 3, **2 assassination**, 1 photograph, 1 destroy a base.

- **How many.** On arrival (`L5f546`) `9786(a6) + 1` attempts, where `9786 = 8 + 8 x` the dominant power's
  share nibble (byte 43 of the system record). The periodic event makes up to 3 more: `RandInt(25) <= 9786`
  per try. Entries are dropped once `day + 4` passes their date (`L5f8ca`).
- **Rank gate.** `d7 = 9788 - 9790`, clamped at 0: the player's points with the system's power minus the
  points with the rival (`60(a6)` fn 32 `d0 = 2`, `l4bf50`: Federal points `828(a6)`, Imperial `830(a6)`;
  byte 43's low nibble is the Federal share, high the Imperial; on a tie, allegiance 2 means Federal,
  anything else Imperial, so independent systems use the Imperial points).

| Offer | Needs `d7` | Federal rank at that many points | Imperial title |
|---|---|---|---|
| delivery (`L5f80c`, `L5f814`, `L5f828`) | 0, > 16, > 81 | -, Corporal, Sergeant | -, Serf, Master |
| **assassination** (`L5f704`) | **> 256** | Sergeant-Major | Squire |
| photograph (`L5f75c`) | > 625 | Major | Lord |
| destroy a base (`L5f7b4`) | > 1296 | Colonel | Baron |

  The rank thresholds (`L7e8b0`) are `n^4`: 1, 16, 81, 256, 625, 1296, ...

- **Target system** (`L5f6cc`). Not near the player: near a fixed point in enemy space, picked by the
  current system's allegiance `1246(a6)` from `L5f6c0`:
  - Federal (2): `$a905c680`, sector (2, -4) from Sol's sector, next to the Imperial core;
  - Imperial (1) and independent (0): `$a925c604`, Sol's sector.

  Then the same `RandInt(3)` band and random system as in section 2, with byte 4 >= 3.
- **Who and how much.** `rand & $e` picks both from `L5f74c`: double crossing spy $4000, traitorous merchant
  $4500, spying company official $5000, double agent and company director $5500, Mafia operative $6000,
  disloyal company president $7000, enemy governor $8000, enemy senator $10000 (texts `$99a5..$99ac`). No
  distance bonus.
- **Date.** `day + 14 + (rand & $3f)`: 14 to 77 days ahead. Departure time = the random long, as above.

## 4. Taking the contract

[code] Opening an offer (`L775b4` civilian, `L77db0` military) calls `L77686`:

- the star-map module must know the target system (`28(a6)` fn 32, `d0 = 3`);
- the system generator builds the target system into the spare object space `17348(a6)`; it must have at
  least one starport (`664(a6)`), and `(seed x count) >> 16` picks one (`L776f8` counts objects with bit 4
  of `280`). That station's model number goes to `11788(a6)`;
- `68(a6)` fn 32 (`d0 = 12`, `d1 = 8`, seeded) picks the ship type (`11790(a6)`).

The briefing is `$99ad` ("We want X in the Y system killed, for $Z"); the question buttons are `$9861`
(`$99af`, "They will be leaving <station> ... at <time> on <date>"), `$9862` (`$99b0`, ship type and
registration) and `$9863` (`$99b1`, "The attempt must be made at <time> on <date> **or soon after**...").

Accepting (`L7771c`, `$9860`) stores the 42-byte record at `12926 + 42 x n` (max 6, else `$99b4`): byte 0 =
name kind `<< 4` (+ allegiance for military, `l77e4c`), byte 1 = status 0, `+2` target system, `+6`
departure time, `+10` date, `+14` ship type, `+16` pay, `+18` the station's name, `+38` the system the
contract was taken in.

## 5. In the target system

[code] Event 2 (arrival in a system) reaches the ship-screen module's `l7f172` (`L7ecce`). For each contract
of kind 0 to 2 whose target system is the new system:

- if the departure moment (`+10` date, `+6` time) has **already passed**, nothing happens; the target is
  never made;
- else `L7f314` looks for an object named like `+18` (the station); if found, module 11 fn 32 (`100(a6)`,
  `L74378`) type 6 (`l74436`) makes the target: a class 12 ship (`236 = $0c`) with the contract's seed as its
  name seed (`118`), **parked inside that station** in state `$84`, departure time in `126`, `238 = $80`,
  and **no destination** (`clr.b 212(a0)`).

Module 11 (`100(a6)`, the ships module) runs its own arrival handler before module 31 (`124(a6)`), so traffic,
parked stock and pirates are already in the object table when the target is made.

Then the ship states (the per-second AI table `L750f0`):

| State | Handler | What |
|---|---|---|
| `$84` | `L75262` | wait until the departure time |
| `$70` | `L75280` | ask the station for the bay (`68(a6)` fn 32 `d1 = 5`, `L5f290`) |
| `$78` | `L752aa` | wait for the bay door: station `159` must be 0 or `$fe` |
| `$1c` | `L75322` | fly out of the bay; once clear (`d7 > 6`) release the bay and fall into `L754c2` |
| `$68` | `L759a6` | no destination (`212 = 0`): `l75946` leaves the system, the hyperspace-out cloud |

The station's door (`L5f0da` module, `L4b110` animation): `159 = $ff` means "opening, reserved for the ship
in `160`", counting `116` down by half the frame time step `14446(a6)` per update; at 0 it becomes `$fe`
(open). `L752aa` only tests `159`, not `160`, so when the door opens for one ship any other waiting ship
can go too. [measured] A station far from the player updates its door only every second or two, so it
took minutes to open; with the player 20 km away it opened in about 6 seconds.

[measured] Mars High, player parked 20 km off, 1x: the target left `$84` on time, waited about 5 s for the
door, spent about 10 s in `$1c`, then became the `$68` cloud. **It is outside the station for about 10 game
seconds.** At 10x it was outside for about 1 real second; at 100x it went from `$78` to `$68` between two
samples 0.25 s apart. Time acceleration was not cut at any point.

## 6. Kill credit and payment

[code] When a ship dies, `L75178` calls `L7508a`: if the dead ship is class 12 and the killer is the player
(`670(a6)`), the contract whose seed (`+6`) matches the ship's `118` gets status byte 1 = the ship's `238`:
`$80` normally, `$40` if it had docked somewhere (`L75aea`).

Settled when docked in the system the contract was taken in (`L7f334`, event 5 path `l7eda0`), for kinds 0
to 2:

| Status | When | Result | Civilian (`824`) | Military (`L7ef98` / `L7efbc`) |
|---|---|---|---|---|
| 0 | departure moment not passed yet | kept | | |
| 0 | departure moment passed | failed, `$9c26` | -4 kill points (`-$400`) | -5 |
| `$80` (negative) | you killed it | paid `+16 x 10` tenths, `$9c27` | +2 kill points (`+$200`) | +12 |
| `$40` (positive) | you killed it after it had docked | not paid, `$9c28` | -2 kill points (`-$200`) | -1 |

[code] **Possible original bug**: `L7f496` maps the kind to the rating to change (`$00` combat, `$26`
Imperial, `$36` Federal). On the negative paths (`d5 < 0`) `l7f4b2` skips loading `d1` from that table and
tests the low byte of the message id left in `d1` (`$9c26`, `$9c28`), which is never `$36`, so **every
military penalty goes to the Imperial rating**, also for Federal contracts.

## 7. Why the target often never shows up

Answers to the player report "targets just stopped working and showing up. I'd wait and nothing would
appear or I'd follow and they'd vanish".

1. **The target leaves the system about 10 game seconds after the bay door opens** (section 5). It has no
   destination, so as soon as it is clear of the station it jumps out. [measured] At 10x it is visible for
   about a second, at 100x and up not at all. A player waiting for the date with time acceleration (the
   natural thing to do: dates are 1 to 11 weeks away) sees nothing. Nothing in the game drops time
   acceleration when it launches; our host time guard only reacts to ships closing on the player.
2. **Following it never works.** The target is only ever made in its contract system, on arrival there
   (section 5). Following its hyperspace cloud to the next system finds nothing, and coming back after the
   departure moment does not make it again.
3. **Arriving after the departure moment** (even minutes after, on the right day) means it is never made,
   although the briefing `$99b1` says "or soon after".
4. **Waiting docked at the station, or far from it**, the target can launch and leave while the player is
   inside or out of range. Far from the player the door can take minutes, so it may also go later than
   the briefing says.
5. **A full object table** makes `L74a34` fail quietly (`l74436` just returns). [measured] On a fresh
   arrival the core systems keep room: Sol 13 free slots of the 95 a ship can use, the Imperial capital
   (`$a905c640`) 19, the others 31 to 68. Only a table filled by many pirates (no cap, see the spawning
   doc 3.1) or followers would hit this.

[inferred] Nothing in this code stops working with play time (the only counter that could run out is the
civilian date word, after about 179 game years). "After playing long enough" more likely means "once I
started doing these missions in the core with time acceleration". The vanishing is 1 and 2.

Possible fixes, none done:

- drop time acceleration to 1x when a class 12 ship leaves `$84` in the player's system (as `L75606` does
  for a close pirate);
- give the target a destination so it flies somewhere before leaving (e.g. another station of the system
  via `L74d10`, like a leaving trader), or delay the jump-out;
- still make the target when the player arrives within some hours after the departure moment, matching
  "or soon after".

Related, not assassinations: in `l7f20e` (photograph / destroy-a-base arrival path) a failed `456(a5)` jumps
to `l7f1ee`, which pops two longs after three were pushed (`movem.l a1/d7/d0`). `a1` gets the kind number
and the stack is left one long short, so the loop runs on through low memory and the final `rts` returns to
a wrong address. [code] Only when the object table is full with no temporary slot to steal.

## 8. How it was measured

Test instance from `devtools/run`, `GLFrontier --nosound --script FILE` (the `--do` string is cut at
`8 x 256` characters, so long tests need a file). With `new 1; waitgame`:

- A6 is at `0x15e`: contract count `a6+12924` = `0x33da`, records from `0x33dc`. The object slot table and
  the objects are in big space at `0xd15e` (`a6+14466`); slot `n >= 1` is at
  `0xd15e + 0xd4 + (n - 1) x 0x11e`; `+236` class, `+211` state, `+95` distance exponent.
- Plant a civilian contract: byte `0x33dc` = 0, `+1` = 0, `+2` = `0xa925c600` (Sol), `+6` = `0x90000000`
  (departure), `+10` = `0x0011d5a0` (the start day), `+14` = `0x2e`, `+16` = 100, `+18` = "Mars High", 0,
  `+38` = `0xa925c600`; count = 1. Then `enter a925c600`: the class 12 ship was in slot 51.
- `goto Mars High 20`, `poke l a6+524 0x8fff8000` (0.6 s before departure), then `peek` the target's
  `+211` and the station's `+159` (Mars High is body slot 92) every second.
- Rates: `poke l a6+10428 N`, `poke w a6+10432 I`, `poke l a6+14482 N`, `poke w a6+14486 I` (10x: 9942 / 3,
  100x: 99420 / 7).
- Free slots: count the zero bytes of `0xd15e + 20 .. + 114` after `enter`.
