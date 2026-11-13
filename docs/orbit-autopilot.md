# Orbit autopilot: measuring step (2026-11-11)

Question: does a circular orbit set from `v = sqrt(mu / r)` stay circular in the game's own physics, at 1x and at time acceleration?
It has to, before an "orbit the target" autopilot mode can rely on it.

## Tools (uncommitted)

`src/orbit_probe.c`, script commands in `src/script.c`:

- `circ`: set the player ship on a circular orbit at its current radius, in the plane of its current motion (x-z plane if at rest), engines off.
- `orbitlog N K`: N `ORBIT` lines, one every K host updates: game time, dt, altitude, pe, ap, semi-major axis, e, speed, radial speed.

mu comes from the parent body: gravity mantissa/exponent at object +122, `mu = mant * 2^(exp-51) * dt * k`, the same constants as the HUD orbit (`orbit_calc`, `gl_hud.c`). The log uses one reference step (dt 994), since pe/ap/e do not depend on the step.

Run from `devtools/run`, for example:

    GLFrontier --nosound --do "new 2; waitgame; wait 3; goto Mars 300; stop; wait 3; circ; \
      poke l a6+10428 0x97b3f0; poke w a6+10432 0; poke l a6+14482 0x97b3f0; poke w a6+14486 0; \
      wait 1.5; circ; orbitlog 40 12; wait 40; quit"

Use `new 2`: loading `Start2` leaves the game with dt = 0.

## Results (orbit_assist=0, exact_step=1, build-modded)

| Case | Result |
|---|---|
| Mars 300 km, 1x, 25 s | pe/ap drift 0.01 km, e < 1e-6 |
| Jupiter 20000 km, 1x, 40 s | circular; ap drift 0.1 km |
| Mars 300 km, 1000x | bounded wobble, e up to 0.001, alt 295-300 km |
| Mars 300 km, 10000x, about 9 orbits | e wobbles 0 to 0.002 (pe 293-300 km), semi-major axis within +-0.4 km of 3700, no drift |
| Mars 100 km, 10000x | pe down to 94 km, e 0.0017 |
| Mars 1000 km, 10000x, about 9 orbits | e about 0.002-0.003, semi-major axis falls 4397 -> 4393 km (slow, 0.15%) |
| Moon 100 km, 10000x | e up to 0.003, pe 92 km, semi-major axis -0.8 km |

Conclusions:

1. Gravity is a true inverse-square pull with the mu above, in the old e=18 zone too (the `Lgrav_norm` fix holds).
2. At 1000x and 10000x the orbit is not exactly circular but bounded: e about 0.001-0.003, so +-0.3% of the radius (up to 8% of the altitude for a 100 km Moon orbit). This is integrator error from the fixed sub-step, not a gravity error. The semi-major axis hardly moves, so there is no real decay at 300 km and only a slow one at 1000 km.
3. An orbit mode therefore works in open loop at 1x. At high acceleration a gentle correction every few AI looks should hold altitude. At low altitude, pick a margin above the atmosphere.

## Gotchas found

- Poking the time rate in directly gives a one-frame gravity kick: velocity changes by g * (one frame's dt). At Jupiter that was 8% of orbital speed. It looks like the step's gravity is applied before the position moves, but I have not checked it with the game's own key path. Re-run `circ` after the rate change to measure the steady state.
- At Jupiter 20000 km the poked 10000x did not hold (dt went back to 1x within the 2 s). The cause is unknown (time guard or a game limit). Accelerated Jupiter orbits are untested.
- Right after `goto` a short wait is needed before `circ` at some bodies, because the game re-chooses the frame in the first frames.

## Next: the orbit autopilot feature (design notes, nothing built)

- Add an AI state next to `$20` ("fly to the destination in `212`"), see `autopilot.md` section 5. Fly there with the existing code until about 2-3 radii out, then switch to the orbit state.
- Orbit state, each AI look: in the target's frame, wanted velocity = `sqrt(mu / r)` perpendicular to the radius, in the plane the ship is already moving in. Feed the difference to the normal thrust request. Also correct altitude towards the chosen orbit radius (needed at high time acceleration, see the table above).
- Default altitude: above the atmosphere (drag cutoff about 20 km, `L61c26`) plus a margin; stars and gas giants need their own minimum.
- Open questions: why accelerated Jupiter orbits did not hold; whether the game's own acceleration keys give the one-frame kick; what the time guard (`time_guard.c`) does around an orbiting ship.
- Test method: `circ` + `orbitlog` above; `new 2`, not `load Start2`.

## Exact gravity (2026-11-13)

The game's gravity (L43ed6) works in 16 bit mantissas (r, r^2, r^3 and the mass division each cut to 16 bits), a relative error of
about 1e-4 that changes with the position. `Lgrav_pre` (end of both .s files, hooked at L43ed6's start) now hands it to
`Call_GravExact` (hostcall.c, hcall $af), which does `dv = -mant * 2^(exp-51) * dt * r / |r|^3` in double on the velocity words.
Switches: `exact_gravity` in glfrontier.cfg (default on), the Debug menu check box, `GLF_EXACT_GRAV=0` to compare.

Mars 300 km circular orbit, 120 ORBIT lines (about 1 orbit at 1000x, 9 at 10000x), original against exact:

| Rate | Gravity | e max / mean | pe..ap spread | semi-major axis |
|---|---|---|---|---|
| 1000x | original | 0.00106 / 0.00072 | 294.4..303.1 | 3698.16..3699.13 |
| 1000x | exact | 0.00012 / 0.00007 | 299.1..300.1 | 3699.37..3699.66 |
| 10000x | original | 0.00194 / 0.00118 | 287.4..301.9 | 3694.18..3694.99 |
| 10000x | exact | 0.00121 / 0.00075 | 293.1..302.1 | 3697.33..3697.64 |
| 1x | exact | < 1e-6 | | 3699.867 |

Leapfrog (`Lgrav_step`, hcall `$b0` `Call_Leapfrog`, `leapfrog_gravity` in the cfg, Debug-menu check box, `GLF_LEAPFROG=0` for
the old order): the sub-steps run as velocity Verlet, half a kick, then drift + kick for each sub-step, the last kick a half, so
the velocity is a whole step's at the end of the frame. Only the player's ship is sub-stepped (other objects take one kick and
one move a frame). Same Mars run, exact gravity on:

| Rate | Order | e max / mean | pe..ap spread | semi-major axis (true 3699.88) |
|---|---|---|---|---|
| 1000x | kick, drift | 0.00012 / 0.00007 | 299.1..300.1 | 3699.37..3699.66 |
| 1000x | leapfrog | 0.00006 / 0.00001 | 299.8..300.1 | 3699.60..3699.89 |
| 10000x | kick, drift | 0.00121 / 0.00075 | 293.1..302.1 | 3697.33..3697.64 |
| 10000x | leapfrog | 0.00059 (one row, e steady about 0.00002) / 0.00004 | 297.6..302.1 | 3699.56..3699.87 |

At 1000x the wobble was mostly rounding (9 times smaller with exact gravity). At 10000x what was left is the step size (a
sub-step is about 1.5 game seconds, the kick-then-move integrator is first order), which the leapfrog above removes. Test harness: `orb.sh`-style `--do` script as above, after `goto Mars 300` wait 6 s
(with 3 s the goto sometimes found no body called Mars).
