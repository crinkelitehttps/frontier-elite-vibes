# Asteroid spawning and why fields are rare

Written 2026-11-02 from two investigations (a forum report, "asteroid fields disappear after about a year", and the question
"why do I almost never meet asteroids"). Addresses are for `fe2.s`. Tools: `astlog` script command, `cheat_asteroids`,
`cheat_ship_set_velocity` (src/script.c, cheats.c), `tools/asteroid_survey`.

## Mechanism

`L624f2_GenAsteroids`, called from `L62400` every frame while the player is more than 20 km from everything:

- 10 percent roll per call.
- The player's parent slot must be 114 (the star / barycentre frame), not a planet's frame.
- `A6_num_asteroids` (`13352(a6)`) must be below `A6_max_asteroids` (`13353(a6)`, the cap).
- The asteroid is placed at player position + (velocity + random/2) * 2^(11 + accel index) mm, motionless, model 62
  (it has a hull block, so it can be destroyed).
- Deleted by `L759cc` when the largest axis of its position reaches 2^28 mm (268 km), tested through byte +95.
- Rendered as the full model within about 33.5 km, a grey `$666` disc beyond.

The time-acceleration index is not 0-4: 1x = 0, 10x = 3, 100x = 7, 1000x = 10, 10000x = 12 (`14486(a6)`). So at 1x an
asteroid appears about 21 s ahead with 33.5 km scatter, at 10x about 169 s ahead with 268 km scatter, and from 100x up none
survive. Velocity unit: 1 raw (+98) = 0.09705 m/s.

Measured survival (save `asteroid`, Barnard's Star, cap 13, `astlog`): at 1x every asteroid survives below about 11 km/s,
73 percent at 12.5 km/s, 22 percent at 14, none at 16 (diagonal flight is more lenient). Lifetime is 21 s + 268 km / v.
About 5 percent of passes come within 10 km. At 10x survival is 91 / 67 / 28 / 0 percent at 0.5 / 1 / 2.4 / 4.5 km/s, only
about 1.6 percent reach model range and none come within 10 km.

Not time-based: a year at 10000x in one system, then 1x, refilled to the cap at once. The forum report is not explained.

## The cap

`L8145e` sets `max = (12 - byte 634) * 8 * (word 620 / 65536)^2` from the PRNG state left over by system generation. The eleven
hand-made systems (`L47390`: Sol, Alpha Centauri, Sirius, Ross 154, 61 Cygni, Wolf 630, Eta Cas, Lave, Diso, Leesti, Zaonce)
do not reseed it, so the cap is 0 on a new game and otherwise depends on history. Generated systems are deterministic.

**Survey, 2026-11-02:** 234 systems drawn at random from the 48,339 within 40 sectors of Sol, cap read after `enter`:

| cap | systems |
|---|---|
| 0 | 125 (53 percent) |
| 1-5 | 29 (12 percent) |
| 6-19 | 39 (17 percent) |
| 20-39 | 25 (11 percent) |
| 40 or more | 16 (7 percent) |

Nonzero caps: median 12, mean 19, maximum 78. So about half of all systems have no asteroids, and a typical field is a dozen
objects spread over tens of kilometres. Barnard's Star (cap 13) is typical; UV Ceti (cap 65, save `uvceti`) is rare.
This matches the experience that asteroids are met almost only in Barnard's Star right after a new game: Ross 154 and every
start system have cap 0, and the first generated system reached is the first place a field can exist.

## Leaks and interactions

- The counter is decremented only by the distance despawn, so asteroids that are shot or rammed leak: the usable cap shrinks
  until the next jump resets it. A misjump skips the reset.
- Our time guard (`time_guard.c`) treats asteroids as ships (class `$b`) and drops 10x to 1x when they approach.

## Open items

- Whether the cap formula is as the original authors intended (the squared fraction makes small caps the norm) is unknown.
- Candidate fixes, none made: decrement the counter when an asteroid is destroyed; do not count asteroids in the time guard.
- The survey ran from the `asteroid` save; generated systems should not depend on that, which was not tested.

## How often a slow arrival shows an asteroid (2026-11-02)

Measured: a real hyperspace jump into UV Ceti (save `uvceti`, cap 65; `select` + click, then `astlog` holding the ship still).
The field fills in about 15 s. All 65 positions lie in a cube of about +-33 km round the ship, uniform per axis (rms 17.9 / 19.1 /
19.2 km against 19.3 for uniform). The nearest was 8.8 km; 1 of 65 within 10 km (0.9 expected), 9 within 20 km (7.2 expected).
(`enter` + `goto` is not a usable arrival for this: the first spawn works and then the field never fills.)

Calculated from that placement and the cap survey (probability that at least one asteroid is within r km of a ship that
arrives slowly at 1x):

| r | cap-12 system | average over all systems |
|---|---|---|
| 2 km | 0.1 % | 0.1 % (1 jump in 1000) |
| 5 km | 2 % | 1.5 % (1 in 67) |
| 10 km | 16 % | 10 % (1 in 10) |
| 20 km | 76 % | 32 % (1 in 3) |
| 33 km (drawn as a model at all) | ~100 % | 45 % |

So most jumps show nothing close, and an asteroid you can actually see as a rock is a one-in-ten event. Not measured: the
encounter rate while cruising on (the estimate is about one close pass per 15 minutes in a cap-12 system, from the
survival figures above).
