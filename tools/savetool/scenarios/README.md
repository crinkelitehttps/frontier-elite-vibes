# Scenarios

A scenario builds a loadable commander file from a base state plus edits:

    PYTHONPATH=tools/savetool python3 tools/savetool/fe2save.py scenario tools/savetool/scenarios/orbit-test.txt

(or run from `tools/savetool`). Load the result with the game's own load screen (title menu key 5,
or the Preferences load icon).

Lines: `base FILE` (first), `out FILE` (paths relative to the scenario file), then `field value`:
name, cash, ship (model word), equipment (hex), hull, fuel, ship_pos x,y,z (km), ship_up km,
`land SLOT` (landed on the pad of the surface port in that object slot; the ship must be in that system),
and for any object slot `vel SLOT,x,y,z` (raw velocity), `objup SLOT,km` and `objhull SLOT,h`,
and raw `a6[OFFSET]`, `ship[OFFSET]`, `ram[ADDRESS]` with hex bytes. See `fe2save.py`.

## Base states (`../bases`)

Each is a full game state; the scenario changes what it names. All are the default Jameson with the
starting Eagle-class ship (Merlin/Mars) or the Lave start ship.

| file | where |
|---|---|
| merlin-landed | Ross 154, landed at Merlin starport |
| merlin-flying | Ross 154, hovering 345 m over the Merlin starport, front view |
| mars-landed | Sol, landed at Mars |
| combat-smoking | Merlin, ~5000 km out, player 300 m from a damaged smoking ship (slot 105) |
| lave-docked | Lave, docked in Lave Station |
| ksc-landed | Kerbol (custom system, needs systems/kerbol.json), landed at Kerbin Space Center (slot 108); made from a Kerbin-frame snapshot with `land 108` |

Make more with Ctrl-S in the game: it writes `savs/SnapNN` at any moment (in flight, docked, mid-fight)
in the same format, loadable by the game and by this tool.

Putting a ship on the ground: use `land`, not `ship_pos`. A flying base keeps its flight state, and an
engines-off ship moved over a port just falls onto it and is destroyed (A6+704 becomes $58).

## What a scenario cannot do yet

- Change system: the stations and planets are generated on arrival (hyperspace), so a base must already
  be in the system. Take a snapshot after jumping there.
- Change the ship type: hull, mountings, pylons and cargo space follow from the ship; take a snapshot
  in the right ship.
