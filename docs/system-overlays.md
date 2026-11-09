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
- `bodies`: parents before children, 60 at most, 18 starports. Per body: `name` (21 chars), `type` (list in `src/custom_system.c`:
  asteroid ... star_g ... trading_post, orbital_station, orbital_city, surface_port), `parent`, `mass_kg` or `mass_earth`,
  `orbit_km`, `ecc`, `incl_deg`, `phase_deg`, `tilt_deg`, `temp_k`, `radius_km`, `spin` (0 none, 1 about a day, each step
  twice as fast), `lat_deg`/`lon_deg` (surface ports), and the raw `kind`, `seed`, `model`. `model` may be a number or a
  stock name (moon, mars, venus, earth, io, titan, jupiter, saturn, uranus, neptune, mercury, asteroid).

Plan and status: `system-overlays-plan.md`.

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
- All of it applies to the native renderer (the default) only; the classic software renderer keeps the stock colours. The game's
  palette is 4 bits per channel, so colours are rounded to 16 levels. The body is found by its object seed, so a **saved game**
  made before the file had a look (or with other seeds) keeps its old planets: enter the system fresh (new game, `enter`).
- Not done yet: a surface image instead of the model's outlines, rings, named looks in `models` that define new models.

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
