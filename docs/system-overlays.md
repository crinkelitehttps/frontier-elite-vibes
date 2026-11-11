# System overlays: file format

A star system in the galaxy can be replaced by a JSON file in `systems/` (built in: this repo's `systems/`, compiled into
the exe; user files: `systems/` in the play folder, same file name replaces a built-in one). `//` and `/* */` comments are
allowed. Parsed with cJSON (`vendor/cjson`); a bad file is skipped whole and the reason is in the log (`custom systems: ...`).
Edits show on the next system entry. `custom_systems=0` in glfrontier.cfg turns them off. See `systems/kerbol.json`.

```json
{
  "format": 1,
  "system": { "replaces": "a91dc641", "name": "Kerbol" },
  "bodies": [ { "name": "Kerbol", "type": "star_g", "radius_km": 2616000, "mass_kg": 1.7565e30 }, ... ]
}
```

- `system.replaces`: the catalog id as hex, or `{"sector": [x, y], "number": n}`. (Names: not yet.)
- `system.name`: optional. `system.galaxy`: reserved (map data, step 5 of the plan), ignored with a log line.
- `system.safe`: optional, `true` = no pirates (the pirate limit is made 0: arrival, event 3 and the weekly event 16 make none). Mission hostiles and police are not touched.
- `bodies`: parents before children, 60 at most, 18 starports. Per body: `name` (21 chars), `type` (list in `src/custom_system.c`:
  asteroid ... star_g ... trading_post, orbital_station, orbital_city, surface_port), `parent`, `mass_kg` or `mass_earth`,
  `orbit_km`, `ecc`, `incl_deg`, `phase_deg`, `tilt_deg`, `temp_k`, `radius_km`, `spin` (0 none, 1 about a day, each step
  twice as fast), `lat_deg`/`lon_deg` (surface ports), and the raw `kind`, `seed`, `model`. `model` may be a number or a
  stock name (moon, mars, venus, earth, io, titan, jupiter, saturn, uranus, neptune, mercury, asteroid).

Plan and status: `system-overlays-plan.md`.

## Frames (what the ship is relative to)

In a system with a file the frame between star, planets and moons is the deepest body whose sphere of influence holds
the ship: orbit distance x (body mass / parent mass)^0.4, at least 3 body radii (2% more to leave than to enter). The
game's own rule (L440fe: 4 (log2 distance - log2 mass) against the parent's, in whole powers of two) uses absolute
masses, so with real-sized moons it gave Laythe and Tylo zones of about a million km (relative to Laythe next to Jool)
and Gilly or Pol none (relative to the planet while landing on them). Hook Lframe_soi at the start of L440fe, hcall
`Call_FrameSoi` (`src/custom_system.c`), one step up or down per call; starports keep the game's rule, other systems
are untouched. `GLF_FRAME_LOG=1` logs the player's switches.

## Appearance: `look`

A body may have a `look`, an object or the name of one in the file's top-level `"looks"` object (shared by several bodies):

```json
"looks": { "eve": { "colours": ["#6b3f8f", "#7d4fa3"], "tint": "#ffd0a0", "atmosphere": "#a070d0" } },
"bodies": [ { "name": "Eve", "type": "thick_atmosphere", "look": "eve", ... } ]
```

- `colours`: up to 8 `"#rrggbb"` (or `null` to keep one), replacing the planet's colour for feature code 0..7 (the model's
  surface colour table, `docs/planet-textures.md`: for a plain model code 0 is the base, 1 the next feature and so on; Earth: 0 sea,
  1 land, 2-3 ice). The value is the sunlit colour; the night side keeps the stock model's dark/lit ratio.
- `tint`: multiplies every colour of the planet by this (scaled so its brightest channel is 1, so it recolours without darkening).
- `atmosphere`: the haze bands and the sky seen from the planet take this colour, each band keeping its own lightness.
- `surface`: an image file name (PNG, JPEG, BMP or TGA, up to the GPU's texture size, e.g. 4096 x 2048) in the play folder's
  `systems/`, drawn on the sphere instead of the model's outlines (no colour table: `colours` and `tint` do not apply to it).
  Equirectangular like a world map: north at the top, the image's middle column facing +z of the planet's own axes (the axes are
  the game's, turned by the planet's tilt and spin like any planet's: a planet with tilt 0 still has its poles wherever the
  model puts them, so check the first picture and shift the image if needed). `night` (0 to 1, default 0.25) is how much of the
  image shows on the night side; the terminator stays the game's hard one. A missing or bad file is logged once and the planet
  keeps its stock look. Images are loaded when first drawn and kept (16 at most).
  The image also decides where the ground scenery goes: no mountains or trees where it is clearly blue (sea), instead
  of the model's outlines under it.
- All of it applies to the native renderer (the default) only; the classic software renderer keeps the stock colours. The game's
  palette is 4 bits per channel, so colours are rounded to 16 levels. The body is found by its object seed, so a **saved game**
  made before the file had a look (or with other seeds) keeps its old planets: enter the system fresh (new game, `enter`).
- `tools/ksp_textures.py` makes such images for the built-in Kerbol system from the user's own Kerbal Space Program install
  (reads the planet maps out of `KSP_Data`, shrinks them, writes `ksp_<body>.png`; `pip install UnityPy Pillow`). Nothing from KSP is
  shipped or committed (`systems/ksp_*.png` is git-ignored); without the files the planets keep their stock look.
- Not done yet: rings, an image embedded in the exe (the built-in Kerbol has none), named looks in `models` that define new models.

## Planet models of the file's own: `models` (prototype, modded build only)

```json
"models": { "kerbin": { "like": "earth", "colours": ["#1c3a8c", "#4a8a2a"] } },
"bodies": [ { "name": "Kerbin", "type": "life", "model": "kerbin", ... } ]
```

- `like`: the stock planet model to copy: a name (as for `model`) or a model number (61-86, 193, 194).
- `colours`: replaces the copy's own colour words (feature code 0 = word 0, code 4 = word 1 ...; up to 4, or 8 if the model has
  eight). Unlike `look.colours` this changes the model itself, so the classic renderer shows it too; the game shades its dark side
  from it. `null` keeps a word. Not yet: new outlines/circles (the continents), a new radius word.
- A body names it in `model`. At system entry the host copies the stock model into the model pool (`Lmdlmod_pool`), points a free
  model number (479 downwards) at it, and writes that number in the body's record; entering again reuses the number and space.
- The renderer, the cheat menu and the radius override treat that number as a planet (`custom_system_planet_model`).
- Ground scenery (mountains, hills, trees, clouds: `src/scenery.c`) uses the `like` model's row of the game's scenery table
  (hcall `Call_ScenModel` in Lscen_pick/Lscen_ray), so a copy of Earth gets Earth's features.
- The original build has no model hook: the log says so and the body keeps its type's stock model.
- A saved game keeps the model number only: loaded without the file, a body of this model would read whatever is at that number.
  (Enter the system once and it is back; the number is the same each run only if the same models are made in the same order.)

### Starports of the file's own: `model`, `pads`, `parking`

```json
"models": { "ksc": { "model": 480, "pads": [[2072, -222], [-2, 1766]],
                     "parking": [[1152, 1378], [3152, 1378], [1552, -862], [2952, -862]] } },
"bodies": [ { "name": "Kerbin Space Center", "type": "surface_port", "model": "ksc", ... } ]
```

- `model`: a model installed from a .fe2m file (`tools/fe2ShipBuilder/custom_ships`), used as it is. The Kerbal Space Center is
  model 480, made by `tools/ksc_model.py` (KSP's buildings as boxes, x4; pad 1 the launch pad, pad 2 the runway).
- The pads are in the model's hull, as in the game's own ground ports (model 48 -> 164 -> 209): a record with flag `$80 + n` and
  bits `$22` per pad, pointing at a hull-only model whose planes face in round the pad. A ship over pad n that touches down gently
  on the ground has landed on it (L620a4: bit 5 from the hull, bit 6 from the ground contact in L3f566).
- `pads` (up to 8) and `parking` (up to 4): metres east and north of the model's origin. The pads' count becomes the port's
  (`164(obj)`, with `280(obj)` bit 6 set, in `Call_CustomBase`: the game gives pads only to stock models $5a-$64). Their places
  replace the game's tables by pad count (hook `Lcsys_pad` in L5f18a, hcall `Call_CustomPad`): `pads` is where the station
  module sends the autopilot, traffic and launches (fn 1), `parking` where parked ships stand (fn 9). List them in the hull's order.
- Saved games keep the port's fields from when the system was made; enter the system again for a new pad count.

## Starting from a real system: `--dump-system`

```sh
cmake -S tools/fe2GalaxyViewer -B /tmp/gv -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build /tmp/gv
/tmp/gv/FE2GalaxyViewer --dump-system a925c601 systems/mine.json      # the catalog id (hex) ...
/tmp/gv/FE2GalaxyViewer --dump-system 0,1,3 systems/mine.json         # ... or sector x,y (Sol = 0,0) and number
```

Writes the system as the game's own generator makes it (the viewer runs the unmodified game code, so overlays never get in the
way) in this file format: every body with type, parent, mass, orbit, eccentricity, inclination, phase, tilt, temperature, spin and
seed; a `model` only where the body's model is not its type's usual one; `level` only where the game's differs from the
rule (binary systems). Mass and orbit are decoded from the 15-bit mantissa / exponent words (checked on Sol: Earth 5.98e24 kg at
149.59 million km). Checked by round trip: Alpha Centauri (a binary, 11 bodies) and a generated system load back to the same
body list in the game. The `binary` type (kind 0, model 32) is the container of a double star.
