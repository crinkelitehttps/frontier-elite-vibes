# System overlays: plan

Status 2026-11-09: plan only. Written at the end of the session that built the first version (custom star systems from
`systems/*.sys`, Kerbol in place of 82 Eridani), so the next session can start from here with a clear context. Everything
under "What exists" is built into `build-modded` and `build-original` but **not committed** and not yet tried in game by the
user.

## 1. Goal

A general way to *overlay* star systems on the procedural galaxy:

- Users write a system in an easily edited file (JSON proposed), drop it in a folder, and the game uses it in place of the
  generated one (and later, maybe, as a new system).
- A file can describe stars, planets, moons, stations, surface ports, orbits, sizes, the system's name and (later) its
  galaxy-map data: star type, population, government, economy, allegiance.
- A body can **reference** a stock planet model (by name or number) or **define** its own look (colours, surface map,
  atmosphere, rings ...), loosely approximated is fine.
- Several overlays at once, one file per system (or a pack of systems per file), with built-in examples (Kerbol).

## 2. What exists (built, uncommitted)

Files: `src/custom_system.c/.h`, `systems/kerbol.sys` (format notes in its header), hooks at the end of `fe2/fe2.s` and
`fe2/fe2_modded.s`, hcalls `$a3-$a9` in `src/hostcall.c` **and** in the second VM's table (`src/galaxy/fe2vm.c`
`atlas_hcalls`; without it `enter`, the system map and the atlas see the generated system), setting `custom_systems` in
`src/settings.c`, CMake step that compiles `systems/*.sys` into the exe (`custom_systems_embedded[]`), docs in
`docs/system-generation.md` sections 2 and 6, test save `../savs/Kerbol` (near Kerbin Station).

Loading: built-in files, then the play folder's `systems/*.sys` (same file name replaces a built-in one); re-read at most once a
second, parsed again only when the text changed (hash), so edits show on the next system entry. `GLF_CSYS_LOG=1` logs keys
asked for and radius changes.

| Hook (label) | Replaces | hcall | Host does |
|---|---|---|---|
| `Lcsys_hook` | `tst.l d0; beq.w l473ec` in `L4655a` | `$a3 Call_CustomSystem` | d6 = system key; returns a1 = records written to `Lcsys_recs` (4096 bytes) or 0; asm then runs the game's own hand-made-system copy loop `l4740c` |
| `Lcsys_body` | `move.l 4(a2),118(a1)` in instantiator `L467e8` | `$a4 Call_CustomBody` | sets the new object's radius 266/270 (and 272/276 scaled) from `radius_km` |
| `Lcsys_prad` | `movem.l 4(a0),d1-3` at `l3cdf0` in `L3cd9c_ProjectPlanet` | `$a5 Call_CustomPlanet` | scales the planet op radius d6 (mantissa $4000-$7fff) / d7 (exponent) by object radius / model radius |
| `Lcsys_cull` | first 4 instrs of `L39646_3DObjClipOffscreen` (jsr+3 nop) | `$a6 Call_CustomCull` | enlarges cull size d3 (never shrinks; only when a5 is the object's own model) |
| `Lcsys_base` | `movem.l d0-5,44(a1)` after `l46a84` (surface ports) and after `l7f2dc` (mission objects) | `$a7 Call_CustomBase` | moves the object from parent's model radius to parent's own radius |
| `Lcsys_hull` | `move.w 90(a1),d6; movem.l a4-5,-(a7)` at `L3fa8e` (jsr+nop; movem redone under the return address via `lcsys_ret`) | `$a8 Call_CustomHull` | scales hull-test point d0-d2 and margin d7 into model units |
| `Lcsys_name` | first 3 instrs of `L84706` | `$a9 Call_CustomName` | writes the system name at a0 (a0 left past NUL), d1 = 0 = handled |

The radius ratio is recomputed from the object every time (`model_radius` = model word 22 * 2^(word10 + word12 - 8) mm, matches
`L448b6` exactly), so it is 1 for every stock body, survives saves and object copies; only planet/star models (61-86, 193, 194)
are touched. Verified by script runs: Sol radii and London unchanged; Kerbol bodies at the file's radii; KSC on the new surface.

Open problems found:
- **Surface push**: a ship that arrives *below* a planet's surface is put back on it (stock: Earth -3 km -> 6379.8). For Kerbin
  (6000 km, model 6100) it went to 6099.8, so that code uses the model's radius. Seen only with the cheat goto on the first entry
  into the planet's frame; Mun/Tylo were not pushed. Not found (not `L3fe56`/`L4011c`, not the collision response, which only
  changes velocities at `L3f8fe`). Normal descent/landing untested.
- One test instance hung once (killed by PID); not reproduced in 3 reruns.
- Galaxy map / info page still describe the old system (star type, planet count, population ...).
- The atlas name search (`src/galaxy`) hashes names itself, won't find an overlay's name.

## 3. Reverse-engineering facts to carry over

System generation (module `L46262_mod_sysgen`, `docs/system-generation.md`):
- `L4655a` (module fn 32) in: a2 name, a0 output buffer, d1 mass limit, d6 system key, d7 size/economy 0..255, d2 0 = build list /
  1 = instantiate (`l467e0`, before our hook), d0/d1 seed pair (0 = hand-made: `l473ec` looks d6 up in `L47390`, 11 entries: Sol
  a925c600, Alpha Centauri a925c601, Wolf 630 a91dc583, 61 Cygni a92dc5c0, Sirius a925c640, Eta Cassiopeia a935c601, Ross 154
  a925c5c1, a8f5c540/41/43/80).
- Callers (all build then instantiate into a 3908-byte stack buffer = **60 bodies max** + end record): `L8145e` (entering a
  system; also sets asteroid cap from 634(a6)), `L4b31e`, `L77686`, `L77b7a` (BBS/missions), `L84f02` (info screen).
  They get the system's params from `A6_mod_starsystemview` fn 32 (d0/d1 seed, d3 star count, d6 key, d7, a2 name).
- Hand-made **source record** (64 bytes), units from Sol's records: 0 long mass (1e22 kg, Earth 599; 0 ends list); 4 seed; 8 parent
  (1-based); 10 kind; 12 model (byte offset = index*2); 14 long orbit radius (2^25 mm = 33.554 km, Earth 4458427); 18 phase
  (65536 = 360 deg; surface port: west longitude); 28 word eccentricity x1000; 30 inclination (surface port: latitude);
  32 temperature K; 34 name (<= 21 chars + NUL); 58 spin (rate = 126(obj) << (spin-1), 0 none); 59 level (0 star, 1 planet,
  2 moon/orbital port, 3 surface port); 62 axial tilt. Surface port records are copied raw (mass long $fff60000). End record:
  byte 4 body count (< 12 allows asteroids), byte 8 ports, byte 9 orbital ports. Output record 20/22 = period (from orbit^3 /
  parent mass), 14/16 orbit radius. Port cap 18.
- Kinds and stock models (catalog): asteroid 2/122 (100 km), small barren 4/128 (1500), barren 6/126 (2240), thin atmosphere
  8/126, volcanic 10/130 (1820), ammonia 12/132 (6050), methane 14/134 (3025), water 16/142 (6380), small terraformed 18/136
  (3400, Mars), life 20/138 (6100) or 140 (5100) or 144 (Earth 6380), terraformed 22/138, thick atmosphere 24/132 (Venus), gas
  giants 26/148 (30500), 28 and 30/146 (61000), 32/150 (122000), brown dwarf 34/152, stars 36,40/154 (M), 42/158 K, 44/160 G,
  46/162 F, 48/164 A, 50/174 white dwarf, 52/156 red giant; ports 64/84 trading post, 66/86 orbital station, 68/88 orbital city,
  70/90-108 surface port (96 London, 94 Paris, 100 Tokyo, 108 Apollonius). Binary container kind 0 model 32.
- Object fields: 90 model*2; 118 seed; 122/124 mass; 126 time base; 134 orbit; 158 spin; 161 level; 162 tilt; 182 temperature;
  246 name; 266 long + 270 word radius (mm, 270 = units of 2^32 mm); 272/276 second radius from model word 24; 280 bits: 4
  starport, 5 surface; positions 64-bit parent-relative (low longs 44.., high 56..).
- Radius from model: `L448b6_Copy3DObj_a0_a1` with a model sets 266/270 from header word 22 and 272/276 from word 24, both
  shifted by words 10+12. Model lookup = `data + rd16(data + model*2)`, `data = rdlong(A6 + A6_GAME_DATA 14470)`.
- Model size word 14 is the cull size (`L39646`). The planet op radius is the high word of `move.l (a5)+,d7` at the start of
  `L3cd9c`. Surface ports placed with the parent's model word 22 (`l46a84`), mission objects the same (`l7f2dc`). Altitude
  (`L3f566`) and laser rays (`L41766`) use the object radius. Ground collision = model hull test `L3fa8e`.
- Names: `L84700` code `$3fe` -> `L84706`: core sectors (table `L8460c_core_systems_sector`, 46 entries) give real names, else
  syllables `L84830` via hash `l847f6`. Key split (`l8479a`): number = key & 63; v = key >> 3; d4 = v >> 16; d3 = (v & $ffff) >> 3;
  sector x = d3 - $1718, y = d4 - $1524. 82 Eridani = a91dc641, sector (1,-1), number 1.
- Planet drawing: planet op `$1f` (models 61-86, 193, 194), surface features and colour sets in the model bytecode
  (`docs/planet-textures.md`: outlines + circles with XOR codes into a 16-colour table, flag bit 4 = 7 alternative colour sets);
  our renderer decodes it in `src/gl/gl_planet.c` / `gl_planet_tex.c`, atmosphere bands in `gl_atmos.c`, reads the object at
  `-212(a6)` (seed 118, radius 266/270).
- **Extra models (modded build only)**: `Lmdlmod_lookup` redirects all 20 model-table lookups through `Lmdlmod_ptrs` (one long per
  model number up to MDLMOD_MAX = 512; the stock table has 240), data copied into `Lmdlmod_pool` by `src/custom_ships.c` from
  `.fe2m` files (`tools/fe2ShipBuilder`) before the game starts. Not in saves (objects keep the number). The original build has no
  such hook.
- Second VM (`src/galaxy/fe2vm.c`): any new hcall the system code makes must be added to `atlas_hcalls` too.

## 4. Proposed design

### 4.1 File format: JSON

- No JSON library in the tree. Vendor **cJSON** (MIT, one .c/.h) into `vendor/cjson`, or write a small reader; cJSON is the low-risk
  choice and works for Windows/Android builds too. Keep the `.sys` reader only as long as needed to convert `kerbol.sys`, then drop
  it (or keep both; decide in step 1).
- Folder: `systems/` (built-in, compiled in as now) + play folder `systems/` (user's). Android: assets or app storage, check how
  `custom_ships` finds files there.
- Sketch (units: km, kg or Earth masses, degrees, K; everything optional except what is marked):

```json
{
  "format": 1,
  "system": {
    "replaces": "a91dc641",            // required: id, or {"sector": [1,-1], "number": 1}, or a name ("82 Eridani")
    "name": "Kerbol",
    "galaxy": { "star": "star_g", "population": "...", "government": "...", "economy": "...", "allegiance": "independent" }
  },
  "models": {
    "kerbin_look": { "base": "life", "colours": ["#2a5d8f", "#3f7f3a", ...], "surface": "kerbin.png", "atmosphere": "#7fb2ff" }
  },
  "bodies": [
    { "name": "Kerbol", "type": "star_g", "mass_kg": 1.7565e30, "radius_km": 2616000, "temp_k": 5700 },
    { "name": "Kerbin", "parent": "Kerbol", "type": "life", "model": "kerbin_look",
      "mass_kg": 5.29e24, "radius_km": 6000, "orbit": { "radius_km": 135998403, "ecc": 0, "incl_deg": 0, "phase_deg": 180 },
      "spin": 3, "tilt_deg": 0, "temp_k": 288 },
    { "name": "Kerbin Space Center", "parent": "Kerbin", "type": "surface_port", "model": 96, "lat_deg": -0.1, "lon_deg": -74.6 }
  ]
}
```

- Body `model` may be: omitted (type's stock model), a stock model number, a stock name (`"earth"`, `"mars"`, `"jupiter"`,
  `"moon"`, `"venus"` ...: alias table in code), or a key into `models`.
- Validation messages in the log (file:body:field), never a crash; a bad file is skipped whole.
- Optionally a JSON Schema in `systems/schema.json` for editor help.

### 4.2 Overlay core (host)

Rework `custom_system.c` into: parser (JSON -> `System`/`Body`), registry (by key; later by sector for added systems), and the
existing hcall handlers. Keep record writing (`write_records`) and the radius hooks as they are. Add a tool or script command to
dump a generated system *as JSON* (`--do "dumpsys a91dc641"` or from `FE2GalaxyViewer --catalog` data), so users can start from a
real system and edit it.

### 4.3 Planet models: reference or define

Three levels, cheapest first:

1. **Reference** a stock model (works now via `model`/`kind`; add names). Radius via `radius_km` (done).
2. **Appearance overrides in our renderer** (both builds, no new game models): keyed per object (seed at 118 + name 246, or a host
   table filled in `Call_CustomBody`), applied in `gl_planet.c`/`gl_planet_tex.c`/`gl_atmos.c`: colour table replacement or tint,
   atmosphere colour, optionally an equirectangular surface image instead of the decoded outline texture (the code texture is
   2048x1024 already), rings. The game's own logic keeps the stock model (atmosphere/physics flags come with the chosen base type).
   Only affects the native renderer (default); the classic software renderer keeps stock colours.
3. **New planet models in the game** (modded build only): load planet bytecode into free model numbers through the existing
   `Lmdlmod_ptrs`/pool. Needs research first: (a) every game check of planet model *ranges* (e.g. `is_planet_model`, star models
   77-87, `cmpi.w #$..,90(a1)` tests, our own `planet_model()` 61-86/193/194) — a planet at model >= 240 would fail them; (b) the planet
   op data format (outlines/circles/colour sets) to generate it; (c) how a clone keeps hull/atmosphere data. Probably: clone a stock
   planet model's bytes, patch radius words (22, 24, 14 and the op radius) and the colour sets. Decide after level 2.

### 4.4 Galaxy-map data (star type, population ...)

Not traced yet. Starting points: `A6_mod_starsystemview` fn 32 (returns seed, star count d3, d7 size), the info screen `L84f02`, the
star line `L849a2` (built with code `$3fe` names), `src/galaxy/galaxy.cpp` (atlas reads the same via fe2vm: `STR_NAME`, system
type byte, `multiple`, `allegiance`, `info`). Plan: find where the per-system descriptor (type byte, multiple, allegiance, info) is
made and hook it the same way as the name (host answers for overlay keys).

### 4.5 Adding new systems (not replacing)

Harder: the galaxy's star positions come from the density bitmap (`G_galaxy_bmp`) and per-sector generation. Research how a
sector's system count and positions are produced (same module), then let an overlay add a system at a sector position. Until then,
overlays only replace existing ids. The catalog (`tools/galaxy_catalog`, `FE2GalaxyViewer --catalog OUT x0 y0 x1 y1`) lists
candidates; a "quiet" pick near Sol: single G star, independent, few ports.

## 5. Implementation steps

0. Ask the user whether to commit the current work first (it is the base for all of this): `git status` shows fe2*.s, generated
   fe2*.s.c / bin / labels, src/custom_system.*, hostcall.c, settings.c, fe2vm.c, CMakeLists.txt, systems/kerbol.sys, docs.
1. Vendor cJSON; JSON parser producing the same `System`/`Body`; convert `kerbol.sys` to `systems/kerbol.json`; both builds.
2. Schema + validation messages; stock model names; `replaces` by id / sector+number / name.
3. Dump command: generated system -> JSON (script command and/or galaxy viewer).
4. Appearance overrides (4.3 level 2): colours/tint, atmosphere colour; then surface image.
5. Galaxy-map data overlay (4.4) after tracing.
6. Fix the surface push (section 2) if the user hits it in a normal landing.
7. Research 4.3 level 3 (new planet models) and 4.5 (new systems); decide with the user.

Rules that apply throughout (CLAUDE.md): asm changes in both `.s` files, new code at the end, same-size hooks, check labels only
gain lines and `__D` defines only gain (`git show HEAD:fe2/fe2.s.c | grep '^#define __D'` diff), build both variants, add new hcalls
to `fe2vm.c` too, test instances from `../devtools/run` by PID with `timeout -s KILL`, let the user test visuals.
