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
