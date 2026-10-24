# Galaxy catalog

`systems.json`: 135 systems picked from about 408,000 scanned ones, so that
between them they have every star class, every planet, moon and starport kind
the game makes, and for each kind its lowest, median and highest mass, radius
and temperature. It is for finding a system to test in ("a gas giant over 2000
Earth masses", "a white dwarf", "a Large gas giant with a station moon") without
looking through the galaxy by hand. Each system lists its bodies with parent,
kind, mass (Earth masses), temperature (°C), radius (km) and orbit, and a `why`
list of what it was picked for.

```sh
python3 catalog.py find gas giant mass\>1000      # name words, then mass/radius/temp/au </> limits
python3 catalog.py find white dwarf
python3 catalog.py find station
python3 catalog.py kinds                          # every body kind and its ranges
```

Quote or escape the `>` in a shell. Sector numbers are the game's, Sol = 0,0.

## Where the numbers come from

Nothing is recomputed: `FE2GalaxyViewer --catalog` (tools/fe2GalaxyViewer)
runs the game's own system generator and body panels through `src/galaxy`.
Mass and temperature are what the body panel prints. The radius is the one the
game gives the body's object (L448b6): it comes from the body's *model*
(entry word 12), so it is the same for every body of a kind (only the
terraformed and life-bearing worlds, kinds 20 and 22, have two sizes). Mass is
what varies. Verified against the objects in saved games (Sol, Lave, Ross 154).

## Rebuilding

```sh
cmake -S tools/fe2GalaxyViewer -B /somewhere/gv -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /somewhere/gv
gv/FE2GalaxyViewer --catalog near.jsonl -150 -150 150 150 1 900        # every sector round Sol
gv/FE2GalaxyViewer --catalog far.jsonl -2000 -2000 2000 2000 10 900    # every 10th sector further out
python3 tools/galaxy_catalog/catalog.py build tools/galaxy_catalog/systems.json near.jsonl far.jsonl
```

The scans are about 1,300 systems a second (the two above took 3 minutes and
500 MB; they are not kept). `build` makes three passes over them (about 2
minutes) and takes a greedy cover, rare things first, ties to the system
nearest Sol.

The scan also counts systems the game's own generator never finishes or jumps
to a bad address in (the atlas marks them): about 0.1% of the systems seen.
