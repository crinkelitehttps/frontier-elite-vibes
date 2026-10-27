# Station records, markets and the bulletin board

What the routines `L5f4a8` to `L601c0` in `fe2/fe2.s` do. They were listed in `unnamed-functions.md` as "procedural
generation of ships present at a starport"; that guess was wrong. They build each station's **market**, **shop list** and
**bulletin-board entries**. Tags: **[code]** read from the disassembly, not run; **[inferred]** a conclusion that could be
wrong. Nothing here was measured in a running game.

## 1. The station record (474 bytes)

[code] `1248(a6)` is the number of stations in the current system (cap `$12`, 18). The records start at `1254(a6)`, stride
`$1da` (474). `L5f51a` clears the count and walks the objects with the callback `L5f4f4`, which, for each object with bit 4
of `280(a0)` set, stores its model byte `93(a0)` into `124(record)` and counts it.

| Offset | Size | What |
|---|---|---|
| 0 | 31 x 4 | per commodity: word at +0 (quantity, `L5fc52`), word at +2 (price, `L5fbc0`) |
| 118 | long | name seed (`118(a0)` of the station object, copied by `L5f546`; `1250(a6)` keeps the last) |
| 124 | byte | station object index (what `516(a5)` turns into the object) |
| 125 | byte | flags: bit 0/1 import ban notices, bit 2/3 system-state notices (section 3), bit 4 "orbital traffic host" |
| 126 | 18 x 16 | news / bulletin entries (section 3), count in the word at 414 |
| 414 | word | number of news entries (max `$12`) |
| 416 | 14 x 4 | shop items: word item id, word random, count in the word at 472 (max 14) |
| 472 | word | number of shop items |

[inferred] Bit 4 of 125 is set in `L5f546` when bit 5 of `280(a0)` of the station object is clear. `L74d36` (orbital class 11
spawner, spawning doc 2.7c) picks the n-th record with that bit set. Bit 5 of 280 marks a surface base (set for kind `$46` bodies in `system-generation.md` section 4), so bit 4 of 125 is set
for orbital stations.

`L74d10` picks a random record (`RandInt(1248)`) and places its object through `516(a5)`; it is the station a leaving ship
heads for (npc-ship-ai 6.5).

## 2. Market generation

[code] Event handler `L5f4a8` (module entry): event 2 (arrival in a system) runs `L5f51a`, shifts the three-system history
(`10396/10400/10404/10408`), and runs `L5f546`. Event `$f` (periodic) runs `L5f8ca` unless `704(a6) = 8`. Event 1 (`L5f52a`)
clears the history first.

`L5f546` reads from the system module (`60(a6)` fn 0 and 1, and the star-system view module fn 2) a handful of attributes into
`1236..1246(a6)` (`1236` and `1238` are used as the 'class' words that `L5f8ca` scales by, `1240/1242` as the shop-item
counts for item kinds 0 and 1, `1246` is the economy class 0..3). It copies a 31-byte commodity flag block to `1204(a6)`: bit 7
looks like an 'illegal' flag (used in section 3), bits 0 to 2 pick a column of the interpolation table.

For each station and each of the 31 commodities:

- `L5fbc0` computes the price word: base `2(a2)` from the table `L5fcd2` (8 bytes per commodity: two base values for
  quantity, two for price), scaled by the curves `L5fbee` (by a system attribute `d0`), `L5fbde` (by the commodity column
  `d3`) and `L5fc02` (by `d1`), then multiplied by `$3e00 + (rand & $3ff)`, about +-3 %.
- `L5fc52` does the same for the quantity word, with up to +100 % random on top.
- Bit 3 of the commodity flags flips the sign of the first term, bit 7 doubles (price) or divides by eight (quantity).

Then it fills the shop list: `RandInt`-driven calls of `L5fa52` with kind 9 (ships for sale, `1248/2` times), kind 0
(`2 x 1240`) and kind 1 (`2 x 1242`). `L5fa70` picks the item id from `L5fac2` using the economy class and the kind.

`L5f8ca` (periodic): drops expired entries from the system-wide list `9768(a6)` (section 4), tries `L5f67e` up to three times,
then for every station blends each price and quantity `new = (3 * old + fresh) / 4`, with one random news entry replaced by
`L5fdca` (clearing its bit in 125 if it was a ban or state notice), a random shop item removed and new ones added.

## 3. Station news entries (16 bytes at 126 + 16 * i)

[code] Layout, written by `L60192` (max `$12`, else dropped): words `d0` (message id, `$98xx`), `d2`, `d3`, `d4`, then long `d1` and
long `a2`. `L5fdca` picks one of 16 handlers from the table at `L5fde4` with `rand & $3e`; the weights are 5 (L5ffb0),
1 (L5ffee), 4 (L60058), 1 (L5fe24), 2 (L5fe8e), 11 (L5fea2), 3 (L60020), 1 (L60178), 2 (L60092), 2 (L60166).

| Handler | Message id | What it makes |
|---|---|---|
| `L5ffb0` | `$9828` (d2 = 0) | if bit 0 of 125 is clear, sets it and posts a notice about a commodity with flag bit 7; the station's name seed is the subject |
| `L5ffee` | `$9828` (d2 = -1) | same with bit 1: the opposite notice (ban lifted?) |
| `L60058` | `$9826` / `$9827` | system class 2 sets bit 2, class 1 sets bit 3, once each: system-wide state notices |
| `L60020` | `$9829` | a commodity with a non-zero quantity, d2 = `$8e00 + index`: shortage/surplus notice; the seed is `rng_seed1` |
| `L5fe24` | `$981c` / `$981d` | news about a system from the visit history (`10396..10408`), most recent first |
| `L5fe8e` | `$981c`/`$981d` | the same for a random system |
| `L5fea2` | `$9819` / `$981a` | an advert tied to a neighbouring system (rating >= 3), price scaled by a random tier from `L5ff4e`, `L5ff50`, `L5ff6e`, `L5ff8e` |
| `L60092` | `$981e..$9821` | another neighbour-system advert with 12 price tiers (`L60112`) and text ids `$999d+` |
| `L60166` | `$9822..$9825` | a short notice with a random seed |
| `L60178` | `$982b/$982c` | a short notice with text `$99c0..$99c7` |

[inferred] The ids `$9819..$982c` are the bulletin-board message kinds; their texts are in the game's string table (not looked up).

The board screen is at `L77434`: it lists entries `126(station) .. 126 + 16 * 414` through `L76018` (`d0` = message id, skipped
when 0). The shop screen at `L76e2c` lists `416(station)` through `L76018` with id `$9811`, pricing each item from its game-data
object (`10(a3,d4)` x 1000 minus a trade-in of the player's ship scaled by `$3b6`).

## 4. The system-wide list at 9768(a6)

[code] 25 entries of 24 bytes starting at `9768 + 24`; a slot is free when its first long is 0. Written by `L5f8a6` from the
handlers `L5f704`, `L5f75c`, `L5f7b4` (ids `$9856`, `$9854`, `$9855`; date 14 to 77 days ahead, price from `L5f74c`, `L5f7a4`,
`L5f7fc`) and `L5f80c`/`L5f814`/`L5f828` (id `$9853` with a price of 50 x something; they need a neighbour system of rating
>= 3 and, for the last two, a system 'size' above `$10` or `$51` in `d7`). `L5f8ca` expires entries whose date `8(a2)` is
earlier than `day_since_epoch + 4` and compacts the list. `9786/9788/9790(a6)` hold the offer counts for `L5f67e` (set in
`L5f546` from the system module's fn 2).

## 5. Accepted jobs: 6 slots at 12926(a6)

[code] `L7771c` (the 'accept' action of a board entry) stores a job: count `12924(a6)` (max 6, else message `$99b4`), 42 bytes
at `12926 + 42 * n`:

| Offset | What |
|---|---|
| 0 | byte `(entry word d2) << 4` (job kind) |
| 2 | long `d1` (target system) |
| 6 | long `a2` (seed) |
| 10 | long `d4` (due date) |
| 14 | word `11790(a6)` (the object chosen by `L77434`'s game-data lookup) |
| 16 | word `d3` (reward; half is paid up front, `5 x`, when `d2` has bit 31 set, message `$99b2` instead of `$99b3`) |
| 18 | the contact's name (copied from `246(a0)` of the object) |
| 38 | long `726(a6)` (the system the job was taken in) |

The status screen (`L7f334`: 'return if 12924(a6) is nonzero') shows these. `L77686` checks whether the target is in reach
(distance in `824(a6)`) and finds a station via `L776f8` (bit 4 of 280, a counter in `664(a6)`).

## 6. How a chosen entry reaches its handler

[code] Every list entry of the station screens is a 24-byte record at `10578 + 24 * n` (`L76018` appends one: `d0` = message id,
the other registers saved with it). When the player picks item `n`, `L76436` plays a sound, loads the record
(`movem.l (a1),d0-4/a2`) and at `l76468` jumps through the word-offset table at `L76476`, indexed by `(id - $9800) * 2`
(115 entries). That one table is the whole screen state machine of the station: shop, board, buttons and job screens. Ids
used by the generators in section 3:

| Id | Handler | What |
|---|---|---|
| `$9811` | `L76f16` | shop item (ship for sale, etc.) |
| `$9819` `$981a` | `L78058` | advert: random refusal by reputation `868(a6)` (`RandInt(9)` negated against it, text `$9906`), else the offer buttons `$9832...` |
| `$981c` `$981d` | `L7796c` | news about a visited system: refuses if `868(a6) < 0` (`$992a`), else lists the last three systems visited with their distances (`$9846..$9849`) |
| `$981e..$9821` | `L775b4` | job offer: checks the target is in range (`L77686`, `824(a6)`), shows `$99ad` and the accept buttons `$9860..$9863` |
| `$9822` `$9823` | `L7782c`, `L7784a` | notices that need reputation `868(a6)` >= 0 / > -9; otherwise `$991b`; fall into `L77852` |
| `$9824` `$9825` | `L77852` | job offer with buttons `$9840..$9845` (text `$991a`) |
| `$9826` `$9827` | `L77ae8`, `L77b06` | the system-state notices (screens `$98f1`/`$98f2`, a face from the name seed) |
| `$9828` | `L774b6` | ban notice: goes to `L782fa`, the **commodity list screen** (title `$98c8`): rows `$9831` for each commodity whose flag bit 7 matches `11734(a6)` (the illegal-goods filter) |
| `$9829` | `L77fc6` | shortage notice: commodity id `d2 - $8e00`, quantity from `754(a6)` |
| `$982b` `$982c` | `L774c8` | short notice, amount buttons `$986d` (1, 10, 100, ... 100000) |
| `$9853` | `L77e74` | system-wide offer from section 4 (price in d3) |
| `$9854` `$9855` | `L77b7a` | the system-wide advert, then buttons `$9858..$985f` and the accept at `l77c44` |
| `$9856` `$9857` | `L77db0`, `L77eb0` | the two other system-wide offers |
| `$986d` | `L77560` | the 'pay an amount' choice: subtracts `d3` from the credits `734(a6)` and nudges the reputation `868(a6)` by +1 for amounts from 1000 up to 9999 (cap 5) |
| `$98c3`..`$98c9` | `L77434`, `L782fa` ... | screen titles |
| `$9860` | `L7771c` | the 'accept' button of a job offer (section 5); `$9861..$9863` are its neighbours `L777c4`, `L777f0`, `L777be` (info and name queries) |

`L7822c` (id `$9832`, the first button of the advert screens built by `L78058`) is the 'agree' step of an offer: it checks free cargo space
(`740(a6) - 744(a6)` against `d4 / 4 + 1`, unless bit 13 of `d4` is set, which skips the check), sets bit `n` of the
achievement bitset `13306(a6)` (see section 7) when bit 28 of `d2` is set (the bit number comes from `L78144`, which maps three fixed
system ids to 12..14), pays half the reward up front (`10 x`), calls the ship screen module fn 0 with a date 28 days ahead and
deducts the cargo. `L782dc` then clears the entry so it cannot be taken twice.

`L78222` is a small helper: it sets `d0 = $9916` and runs `L76130` with the reply text id in `a3` ('message from the contact');
the earlier name guess 'event handler that classifies messages' was wrong.

### Job table consumers (12926(a6), 42 bytes each)

[code] Byte 0 = `(sub-kind << 4) | type`; byte 1 is a progress flag (0 = open, non-zero = done/seen). The type is the
low nibble:

- **Creators:** `L7771c` writes type 0 (the delivery-style accept). `l77cba` (accept of the system-wide adverts `$9854`/`$9855`) writes
  type `3 + 2 * (id - $9854) + economy class - 1`, so 3 or 4 for `$9854` and 5 or 6 for `$9855`, with the sub-kind from the
  advert text id (`a2 - $994f`). For `$9854` it also sets bit 18 of `158(a0)` of the player ship (a 'carrying it' flag) and
  clears bit 18 of `162(a0)`; for `$9855` it needs a free equipment slot and writes `$85` into it (`738(a6)` is decremented).
  Both refuse above 6 jobs (`$99b4`).
- **Types 3 and 4 (`L64184`):** each frame the player ship has bit 18 of `158(a0)` set, for each such job whose target
  system `2(a1)` is the current one, the target object (index in `14(a1)`) is looked up (`516(a5)`); if its `95(a0)` (distance
  exponent) is <= 13 the flag `1(a1)` becomes `$ff` and the closest value is kept in `15(a1)`: fly near the target object.
- **Types 5 and 6 (`l75896`, in the ship module):** while the player is in the target system, a ship whose id `210(a0)` equals the job
  object `14(a1)` (or whose parent is the station the job names) is handled by spawning a type 9 ship and a `$ca` model; `1(a2)`
  becomes `$ff`. [inferred] an escort/rendezvous job.
- **Type <= 2 (`L7508a`):** when a ship of class `$c` dies, its name seed `118(a0)` is compared with the seed `6(a3)` of every job and,
  on a match, `1(a3)` gets the ship's bounty byte `238(a0)`: the 'hunt this named ship' completion hook.
- **Status page `L7ea70`** (title `$9c07`) lists the jobs with type <= 2 (text base `$999d` for type 0, `$99a5` otherwise), the contact's name
  and the target system; `L7f17e`/`L7f33c` further down the status code list and delete finished or lapsed jobs (`subq.w #1,12924(a6)` at
  line 58983 of `fe2.s`).
- `clr.w 12924(a6)` at line 57451 (new game / load) empties the table.

## 7. Completion, payment and expiry

[code] Two routines in the status-screen module handle jobs; both are reached from its event handler `L7ecce`.

### 7.1 Arrival at the target system: `l7f172` (event 2)

For each job whose target system `2(a1)` is the system just entered:

- **Types 0 to 2:** only if the job is not yet expired (day `10(a1)` not before the current one). `L7f314` compares the contact name `18(a1)`
  with the name `246(a0)` of every object (`618(a5)` walk); if the wanted ship already exists, module `100(a6)` fn 32 is told (`d7 = 6`)
  with the job's seed and date. [inferred] this is the 'target spotted' notice.
- **Types 3 to 6:** the target object is **created** here. The model comes from `L7f1fe` by `(byte0 >> 3) & $e`, the name is the contact's,
  parent `68(a1)` is the matching station (`12921(a6)` from `L7f314`), the name seed `118(a1)` is the job's seed, and the ship gets flags
  `280` bits 5 and 6, state `94 = $e`. The new object's index is stored in `14(a1)` and `15(a1) = $ff` (closest approach starts at its maximum).

### 7.2 Docking back in the origin system: `L7f334` (called by `l7eda0`)

For each job whose **origin** system `38(a1)` is the current one:

| Case | Message (`d1`) | Pay | Rank score `824(a6)` | Rating track |
|---|---|---|---|---|
| Types 0 to 2, done flag `1(a1)` < 0 (`$80..$ff`) | `$9c27` | `16(a1) * 10` credits added to `734(a6)` | `+$200` | `d5 = +12` |
| Types 0 to 2, flag positive (`$01..$7f`) | `$9c28` | none | `-$200` | `d5 = -1` |
| Types 0 to 2, flag 0 and time of day past `10(a1)` / `6(a1)` | `$9c26` | none | `-$400` | `d5 = -5` |
| Types 0 to 2, flag 0 and not yet due | job stays | | | |
| Types 3 to 6, past the due day `10(a1)`, flag 0 | `$9c26` (as above) | none | `-$400` | `d5 = -5` |
| Types 3 to 6, past the due day, flag set | `$9c29` | none | `-$400` | `d5 = -1` |
| Types 3 and 4, in time, flag set, closest approach `15(a1) > $a` | `$9c2a` | `16(a1) * 10` | none | `d5 = $10` |
| Types 3 and 4, in time, flag set, closest approach `<= $a` | `$9c2b` | `16(a1) * 10` | none | `d5 = $14` |
| Types 5 and 6, in time, flag set | `$9c27` | `16(a1) * 10` | none | `d5 = $12` |

After the case is chosen `L7f430`: decrements `12924(a6)`, shifts the later slots down by 42 bytes, formats the message (`A5_GetFmtStr`,
text from `L7f4a4` by type) with the contact's name, and shows it through module `108(a6)` fn 32. Then:

- type 0 adds `d6` to the rank score `824(a6)`, clamped at 0;
- the other types call `L7efe0` through `L7ef98` (even types 2, 4, 6) or `L7efbc` (odd types 1, 3, 5) with `d5` as the delta (`L7f496`:
  `&` $26 for odd, `6` $36 for even). `d4 = L7f49d[type]` (`$ff,8,2,9,3,b,5`, +1 when `d5 = $14`) is passed on to `L7f084`, which sets that bit in `13306(a6)` and
  prints `$9c6a` / `$9c6b + bit` the first time ('achievement' messages, music 7).

### 7.3 The rating tracks

[code] `828(a6)` (via `L7ef98`, 54-entry list) and `830(a6)` (via `L7efbc`, 38-entry list) are 16-bit scores. `L7efe0` finds the tier in the
13-entry threshold table `L7e8b0`, adds the delta (clamped to 0 .. `$ffff`) and, when it crosses a tier boundary, shows `$9c36` (up) or `$9c37` (down) with the new
tier name (`$9c66` for 828, `$9c65` for 830) and plays music 7 on a rise. The same two scores are drawn on the status screen (`L7e8dc` etc.).
`824(a6)` has its own 9-step table (`L7e87a`, text `$9c3c..`). The offer check `L77686` requires the target's distance code to be within `824(a6)`,
so it is the 'how far will contacts send you' score.

[inferred] 828 and 830 are the combat-style and trade/exploration-style ratings (names by text id `$9c65`/`$9c66` not looked up).

### 7.4 Quirks worth knowing

- The expiry test for types 0 to 2 compares two longs (`A6_time_of_day` against `10(a1)` then `6(a1)`). `6(a1)` is the seed written by the offer generators, so on
  the due day itself the comparison is made against a random value. [inferred] this makes the exact expiry moment random within the day; it is probably not intended.
- The type 0 to 2 done flag is written by `L7508a` from the byte `238(a0)`, which is the **high** byte of the bounty long (L749aa writes a word, so it is 0 for
  pirates). Whether job targets use a different value there was not checked.
- A job is never dropped for being out of reach or because its target system changed; only these two routines remove slots (and `clr.w 12924(a6)` on new game).

## 8. Not traced

- The text strings and the full set of 115 handler entries in `L76476`.
- What module `100(a6)` fn 32 (`d7 = 6`) does exactly; whether the `238(a0)` flag value ever differs from 0 for real job targets.
- The status-page layout code between `L7ea70` and `L7eb9c` (display only).
