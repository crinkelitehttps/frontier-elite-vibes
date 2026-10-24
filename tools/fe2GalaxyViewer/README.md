# FE2 Galaxy Viewer (the Galaxy Atlas)

The game's galaxy map, zoomable from the whole galaxy down to one star, with
every system's details a click away. The same atlas is in the game (emulator
menu > GALAXY ATLAS); this is it on its own. Build with
`build_fe2GalaxyViewer.bat` (needs CMake and MinGW gcc; MSVC is untested); the
result is `build/FE2GalaxyViewer.exe`.

## Where the data comes from

Nothing is recomputed. `fe2/fe2_modded.s.c` (the game's 68k code translated
to C) is compiled into the tool, and the atlas calls the same routines the
galaxy map calls, with the registers set up the way the map sets them up:

| What | Game routine |
|---|---|
| Stars of a sector (count, star types, positions) | `L8444a_StarSystemsViewDrawSystems` |
| A star's position | `L83d3c` |
| Names, star lines, distance text | `L84700` (galaxy map module strings) |
| Allegiance, government, economy bytes | `L4beee` (system info module, fn 32) |
| Distances | `L84ae6` |
| Galaxy density (the background) | `L8ad0e` |
| Selecting a system | `L849a2` |
| System screen (government, economy, population...) | `l84c04` (map key $f8) |
| Trade screen (imports, exports, illegal goods) | `l84b34` (map key $f7) |
| Planets and starports (runs the system generator), and the system picture | `l84f02` (map key $f6) |
| One body's screen (mass, temperature, orbit...) | `l84cde` (map keys $80+) |

The info screens are captured as the game draws them: the tool catches the
game's DrawStr host calls (text, position and colour), so the System, Trade
and Planets tabs show exactly what the game prints, at the same places.

Because it is the game's code, a change to `fe2/fe2_modded.s` shows up in the
atlas after `python tools/build_fe2.py` and a rebuild of the tool.

## How it works

The code is shared with the game, in `src/galaxy`:

* `fe2vm.c` runs game code. The translated game code has
  `Start680x0_at(entry)` (`tools/as68k/output_c.c` writes it): it starts at
  any 68k address, and while the host is calling in it returns when the code
  returns to a stop address, stops a call that never returns, and returns
  from a bad jump instead of aborting (`host.h`). Here the viewer owns the
  machine: `fe2vm_init` loads the game binary like the game does and runs the
  start of its setup (`L4569c_SetupA6Jumptab`, `L45c22_UseMainGameData`). In
  the game, each frame's calls go between `fe2vm_begin` and `fe2vm_end`,
  which save the registers, flags, interrupt state and all of the 68k RAM
  and put them back, and swap in the atlas's host calls.
* `galaxy_labels.h` names the routines from `fe2/fe2_labels.h` (the unnamed
  ones are exported through `tools/build_fe2.py`'s `EXTRA_LABELS`).
* `galaxy.cpp` wraps the routines above; `map_view.cpp` draws the map;
  `atlas.cpp` is the search, the survey and the panels. Here `app.cpp` is the
  window and the command line; in the game it is `src/ui/ui_galaxy.cpp`.

## Scanning the galaxy

`FE2GalaxyViewer --catalog OUT X0 Y0 X1 Y1 [STEP [SECONDS]]` writes one JSON
line per explored system in those sectors (Sol = 0,0): its star and every body
the game's generator makes, with mass, temperature, orbit and the radius of the
body's model. `tools/galaxy_catalog` picks a small covering set from scans.

## The newer parts

* **Game view** (Planets tab): the screen host calls `l84f02` makes (blits,
  lines, clears) are done as `src/host_screen.c` does them, into the game's
  screen in RAM; the tool shows that picture and the click areas the game
  registered with `L4231a`. Clicking one shows that body's `l84cde` text, as
  the game does.
* **Orbit view**: radius, eccentricity and period are read from the numbers
  the body screens print. Where along its orbit a body is, is not in them
  (the game works it out from its clock in the system), so everything starts
  at its nearest point on day 0. Stations (printed "0.000 A.U.") go on a ring
  round their planet with their real period; surface ports on the rim.
* **Where people live**: per system, the starports `l84f02`'s body list has
  (kinds $40-$44 in orbit, $46 on the surface), surveyed round the view a few
  milliseconds a frame and saved in `FE2Galaxy.cache` (tagged with a hash of
  the game binary, so a changed game starts a fresh survey).
* **Search anywhere**: a system's generated name is a hash of its sector and
  number (`l847f6`, syllables `L84830`). The tool hashes every star slot the
  galaxy bitmap allows (about 1.2 billion, on all cores, under a second),
  keeps the nearest matches and has the game name each one before showing it.
  `--selftest` checks the copy of the hash against the game's names.

## Checks

* `FE2GalaxyViewer --selftest` runs the density map, 4096 sectors round Sol, a
  repeat of a sector, the galaxy's corners, the info screens of 1000 random
  systems, the name hash against the game's names, the search and the
  starport survey, and says how long it took and whether anything failed. It
  also runs a batch of calls the way the game does and checks that every
  byte of RAM and every register is the same afterwards.
* `FE2GalaxyViewer --smoke` opens the window and runs a scripted tour for a few
  seconds (zoom out, star counts, tilt, search, selection); ImGui asserts are
  counted in release builds too.
* `FE2GalaxyViewer --dump X Y [N]` prints a sector and system N's screens.

## Things found in the game

* When a system fills all 60 body slots, the system generator returns with
  its stack out of step (`l466e6` pushes `642(a6)`, `l46748` leaves by
  `bge.w l467dc` without popping it) and jumps to a bad address. Greliain,
  sector 4,1 number 3, does it; the game would most likely crash there.

* Hand-placed sectors round Sol without stars end `L8444a` at `l846da`, which
  reloads registers from its caller's stack (the map's draw routine) to draw
  the sector's special object (`L84616`). The atlas gives it a proper frame.
* Some systems make the system generator loop forever (`L46666`, orbit
  spacing `d7` = 0), for example Andessho, sector -9,30 number 5. The game
  runs the same code for its info icon, the system map and on arrival, so it
  would most likely hang there too. The atlas stops the call and says so.

## In the game

`src/ui/ui_galaxy.cpp` opens the atlas over the game screen from the emulator
menu, pauses the game while it is open (as the free camera does), starts on
the system you are in (`726(a6)`), and lays the atlas out for a small window:
the map with a search box and VIEW / GO / INFO buttons, the selected system
underneath, and DETAILS for its screens a game screen wide. The survey is kept
next to the saves.
