# The station screens module

The routines `L75c92` to `L78396` in `fe2/fe2.s`, labelled "station dialogue pieces / rendering / handlers" in `unnamed-functions.md`, are one UI module: the
**station services screens** (the list-and-face screens you see when docked: services, bulletin board, shop, shipyard, commodity market, messages). Tags:
**[code]** read from the disassembly, not run; **[inferred]** a conclusion that could be wrong. Companion docs: `starport-economy.md` (what fills the lists) and
`system-generation.md`.

## 1. Entry points and state

[code] The module's function table has these entries (names from the source labels): fn 8 `L75f3c_fn8` (enter the station screens), fn 16 `L76180_fn16` (per-frame input),
fn 20 `L762ca_fn20` (per-frame ticker for the message line), fn 32 `L7604c_fn32` (post a message), fn 36 `L75ede_fn36` (leave).

State in `a6`:

| Offset | What |
|---|---|
| `114` | station screen active (`$ff`) |
| `115` | bit 0: in the station screens; bit 1: a message is on screen or queued |
| `704` | current screen/mode byte of the whole game (`8` flight 3D view; `$58`, `$60`, `$54` station-related; other values skip drawing) |
| `10578 + 24 * n` | the **item list**: 24-byte records (word id, then the registers saved by `L76018`: `d1`..`d4`, `a0`), max 45 |
| `11722` | index of the first visible item (scroll position, steps of 4 or 14 by screen) |
| `11724` | number of items |
| `11726` | title text id of the screen |
| `11728` | bit 0: suppress the panel call |
| `11730` | last key pressed in the list |
| `11732` | current **screen number** (even values, section 2) |
| `11734` | a filter word for the commodity screen |
| `11736` / `11740` | face seed and face kind for the face box (`L78998`) |
| `11746..11790` | the entry the player picked (saved registers `d0`..`d5`, `a2`), `11790` a model index |
| `11780` | a countdown (`$8000`, decremented by the frame step) that re-blits the face when it hits 0 |
| `11782` | non-zero: leaving needs `450(a5)` (re-enter the station) |
| `11686/11718` | message-line timers (`$2f54d` / `$54f42`) |
| `11690` | the message on screen (`d0-d3/d7/a0/a2`, 28 bytes) |
| `11792`, `11794..11906` | the queue of waiting messages (up to 4, 28 bytes each) |

## 2. Screens (`11732`)

[code] `L75c92` redraws whichever screen is current (it is the 'refresh' called from the module's fn 8 after a message). The mapping:

| `11732` | Screen | Builder |
|---|---|---|
| 0 | root services menu | `L75eb2` |
| 2 | generic list ('Back' list) | `L780c4` |
| 4 | bulletin board | `L77434` |
| 6 | commodity market | `l78314` (via `L782fa`) |
| 8 | shipyard upgrades | `L76b04` (via `L76a76`) |
| `$a` | shop / ships for sale | `l76e2c` |
| `$c` | shipyard ship viewer (3D ship, `l788f8_ShipyardPut3DObj`) | `L76f1e` |
| `$e` | the system-wide offers list (`9792(a6)`, title `$98f3`/`$98f4`) | `L77b2c` |
| `$10` | the accepted-jobs list (`l772c6`) | |

`L75d10` draws the title and up to `L75cec[screen]` items starting at `11722` (3 or 13 per page), each by `L75d72`, which also draws the up and down scroll arrows
(`L78cbc`, `L78c68`) when the list continues. Drawing uses a coloured pair per screen (`L75e04`).

## 3. Building a screen

[code] A screen is built by clearing the list (`L75f2a`: zeroes `10602`, the counts and the key), then calling `L76018` (the shared 'append item' routine, 85 callers) once for every row
with `d0` = the message id of that row; the other registers go with it. Title and face are set in `11726` / `11736` / `11740`. Then `L75eae` (or `L76130`) draws
a message, `L75eb2` redraws everything with a blue background and the face box, and `L78396` is the same with a fixed title and a mode number.

The root list (`L75f3c_fn8`) adds: `$9803` (always), `$9802` (a ship nearby that can dock), `$9800` or `$9801` for each docking bay of the parent station
(`L75fe8` enumerates the station's children), `$9809` and `$982f` (extras when the ship has the flags). Without a ship (`l75e8a`): `$9818`, `$980a`, `$980b`, `$980c`.

## 4. Messages and input

[code] The message line is the one-line text shown over the face while docked:

- `L76130` is 'show message `d0` now': it moves `a2/a3` into the arguments, sets `d7 = -1` and falls into `L760ac`, which stores the message at `11690` and starts the timer.
- `L7604c_fn32` is the module call for the same from other modules; it uses `L76058`, which first closes the panel (`92(a6)` fn 0) if the game isn't in 3D mode, plays effect 29
  unless `d7 < 0` and, if a message is already on screen (`115` bit 1), queues it at `11794 + 28 * n` (up to 4) instead.
- `L760ba` draws the pending message on the face box (`L789c8`); `L763e8` and `L76412` end the screens (restore the panel, clear scan keys 49 to 53 via `168(a5)`).
- The ticker `L762ca_fn20` counts `11686` down; at 0 it shows the next queued message by shifting the queue.

`L76180_fn16` (input) handles: the copy-protection letter prompt at the start (`609(a6)`, `614(a6)`), the keys `1` to `4` (pick an item on the page) and `5` (next page, `l762ae`), the
cursor keys `$e0`/`$e1` (scroll up/down), and `$80..$9f` (clicking an icon; its low nibble selects the item). `L76436` runs the picked item: plays a sound, loads its record into
`d0-d4/a2` and jumps through the 115-entry word table at `L76476` indexed by `(id - $9800)`; `starport-economy.md` section 6 has the table.

## 5. The helpers named in this block

- `L77686`: can the player reach the job's target? Uses the star-system module's distance (fn 3), the rank score `824(a6)` and a station scan with the callback `L776f8`.
- `L77f88`: remove the picked offer from the system-wide list at `9792(a6)` (the same list as `starport-economy.md` section 4) by shifting the later records down.
- `L782dc`: clear the message id (first word) of the picked board entry `d5` so it cannot be taken again.
- `L78008` (id `$983f`, 'sell one tonne'): decrements the player's hold `754(a6)` for the commodity, adds the price to the credits `734(a6)`, decrements the used cargo `742(a6)`.
- `754(a6)` is the player's cargo hold: one word per commodity (31); `738`, `740`, `742`, `744` are the capacity and used counters.

## 6. Not traced

- The shipyard viewer `L76f1e` after the first screen setup (the stat formatting and the ship rotation).
- Most of the remaining entries of the `L76476` table and the strings behind the text ids. (`$983e`, `L78114`, is the 'Back' action: it returns to the bulletin board.)
