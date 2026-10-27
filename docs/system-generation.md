# Star system generation (module `L46262_mod_sysgen`)

The block that `unnamed-functions.md` called "NPC/person generation from a seed (face parts, names)" (`L46666` to `L472b0`) is not
about people. It is the **star system generator**, the game module `L46262_mod_sysgen` (`fe2/fe2.s` lines 28805 to 30150). It turns a
system's seed into a list of bodies (stars, planets, moons, stations, ground bases) and then into game objects. Tags: **[code]**
read from the disassembly, not run; **[inferred]** a conclusion that could be wrong.

## 1. Entry points

[code] The module's jump table has its useful entries at the end: function 32 is `L4655a` (a comment in the source calls it the
`_DoStuff` function: if it returns at once, systems are not generated) and function 36 is `L463b0`, a name/string lookup
(`d0 & $3fe` into `L4991a`). `L4655a` takes:

| Register | Stored at | What |
|---|---|---|
| `a2` | `628(a6)` | the system's name string (stem used for body names) |
| `a0` | `650(a6)` | the output buffer of 64-byte body records |
| `d1` | `646(a6)` | a mass limit used to cap the planets' sizes |
| `d6` | `656(a6)` | the system key (the packed system id, e.g. `$a925c600`) |
| `d7` | `660(a6)` | a 0..255 'size/economy' value (drives how many stations and bases appear) |
| `d2` | `654(a6)` | 0 = build the record list; non-zero = instantiate the records (`l467e0`) |
| `d0`, `d1` | `620/624(a6)` | the seed pair; `L4752e` steps it (the 68k game's main PRNG, also used for the galaxy) |

**Authored systems:** if the seed `d0` is 0, `l473ec` looks `d6` up in a ten-entry table at `L47390` of `(key, offset)` pairs and copies the
hand-made body records it points to (keys start `$a925c600`, `$a925c601`, `$a91dc583` ...). The records are the same 64-byte format;
masses and orbits are re-normalised into the 16-bit mantissa/exponent form as they are copied, and the names come from the table.
A key not in the table falls back to offset `$24c0`.

## 2. The body record (64 bytes)

[inferred] from how the generator and the instantiator (`L467e8`) use the fields:

| Offset | What |
|---|---|
| 0, 2 | mass as a word mantissa and a word exponent (exponent bias `$3f`) |
| 4 | long, the body's own seed; becomes `118(obj)` (the name seed for `randnames`) |
| 8 | word, parent record number (0 = none; 1-based, the parent is `64 * n` bytes back) |
| 10 | word, kind: 0 star, `$0c..$18` planet classes, `$32` an extra, `>= $40` a starport; `$44` and `$46` special starports (`$46` = ground base) |
| 12 | word, model / object type (`$20` star, `$84..$8e` planets, `$5a..$6c` bases, `$ae` ...) |
| 14, 16 | radius as mantissa and exponent |
| 18 | word, orbit phase / eccentricity (`622(a6)`; `$7fff` marks the second component of a binary) |
| 20, 22 | orbit radius as mantissa and exponent |
| 24 | long, mass value; replaced by the new object's pointer once instantiated |
| 28, 30 | words derived from the seed (`620(a6)`) |
| 32 | word, a 'temperature / size class' value; the planet generator compares it with `$d7`, `$139` and so on to choose the model |
| 34 | name string, up to about 24 characters |
| 58, 59 | bytes; `59` bit 7 says the body was given a `randnames` name |
| 62 | word |

## 3. Building the list

[code] Counters in `a6`: `632` = records so far, `634/635/636/637/638/639` = name counters (suffix letters and digits), `642` = the current
parent record number, `662` = the record a station or base attaches to, `664` = stations and bases so far (capped at `$12` = 18, the same cap as
the station table in `starport-economy.md`), `665` = how many of them are orbital.

- **L4655a** — start. Mode by the star count `644/645(a6)` (`d3`): a single star goes through `l46640`; two or more get `L46ac8` for the pair, then `L46666`
  for the planets of each.
- **L46ac8** — makes a **double star** (a `$20`-model body with `A`, `B` names, e.g. 'Name A,B'), splits the mass between the two and
  puts the second at phase `$7fff`. Called with the key `d0 < $ffffffef`, so the top keys are reserved for special cases.
- **L46666** — the **planet loop**: draws orbit spacings from the PRNG, gets a mass from the saturating curve `L4754a` (table `L47562`,
  a 256-point curve from 0 to `$ffff`), and calls `L46bd0` for each planet until the orbit radius overflows (`bcs l467dc`). A body above
  `$1036640` (about 17 million in mass units) may get further children (the `l466e6` branch recurses through `L46ac8` and `L46666`).
- **L46bd0** — **create one body record** for mass `d0`: finds the first threshold of table `L46418` (longs) that is not below the mass; the index picks the
  model from `L463ea` and the kind from the word 48 bytes before. Names it: a star gets the system name plus a letter; a planet copies a class
  name from the template at `34(a1)` and appends a counter (digits for the first sequence, lower-case letters for the second, `0..9` for the third);
  a body with bit 7 of `59` gets a random name (`L47370`).
- **L46bc8 / L46cb4** — the same without the class lookup, then fill the orbit (20/22), the derived words (28, 30, 62, 58, 59) and the
  'size class' at 32. For a bigger body (`32 < $3e8`) it adds a heat term summed over the parent chain.
- **L46ea4** is the base of a computed dispatch (`jsr L46ea4(pc,a3.w)`, `a3` set per class in `L46bd0`; 0 = a plain rts). The routines after it (`$46ea5` to `$4715x`) — class post-processing by `32(a0)` (`$d7`, `$139`, `$aa`, `$7b`, `$42`, `$dc` ...): picks the final model
  `12(a0)` (`$84`, `$86`, `$8a`, `$8c`, `$8e`), the kind `10(a0)`, and decides whether the planet gets a station or ground bases:
  `L47164` and `L472b0` below. Thresholds on `660(a6)` (`d2`): `>= $1e` gives a first station, `>= $32` a second, `>= $f2` a third.
- **L47164** — creates up to `d1` (0..3) **ground bases** on the current body: copies the 64-byte template `L47b62`, kind long `$39`, sets the parent to
  `662(a6)`, picks the model from `L47210` (`$5a..$6c`, by the seed) and the orbit from `L47270` (16 entries), calls `L47370` for the name. Stops at 18.
- **L472b0** — creates one **orbital station** of type `d0` (0, 1 or 2 pick the templates `L477e2`, `L47ba2`, `L47a62`), at orbit radius `$7fff`/`$22`,
  with a random phase. Also stops at 18.
- **L47370** — marks bit 7 of `59(a0)` and asks the `randnames` module (`A6_mod_randnames`, fn 32) for a name from the body's seed.

## 4. Instantiating (`l467e0`, `L467e8`)

[code] For every 64-byte record: `A5_AddObject(29, model 12(rec))`, then copies the seed into `118(obj)`, the bytes 58/59/62 into 158/161/162, stores the mass
as an exponent form at 122/124, the radius at 134, the orbit radius into 144/152, and the name into `246(obj)`. Kind `>= $40` marks a **starport**:

- sets bit 4 of `280(obj)` (the flag the station table builder `L5f4f4` looks for), `164(obj) = 3` (or 7 for kind `$44`);
- kind `$46` is a **ground base**: it also sets bit 5 of `280(obj)`; models `$5a..$64` also set bit 6; `164(obj)` is 5 or 1 for particular models;
- orbital stations get their position from the parent's orbit (`564(a5)`), a rotation matrix from phase and tilt, and the object's velocity.

This confirms the guess in `starport-economy.md`: bit 5 of `280` marks a surface base, and an orbital station is a starport with bit 5 clear.

## 5. Not traced

- The exact meaning of every record word and the mass thresholds in `L46418` (the table is partly data tables laid over each other).
- The rest of `L46ea4` onward (the tail around `$4707x` to `$4712x`), which assigns the special-case models and the `$32`/`$ae` extras.
- The ten authored systems in `L47390`: only the first keys were read, and none was checked against the galaxy catalog.
