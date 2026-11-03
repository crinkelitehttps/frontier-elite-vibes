# Camera screen slowdown (found 2026-11-03)

Status: FIXED (2026-11-03, uncommitted). Cause: the host scenery's loop `Lvirt_a` in
the object loop runs until hcall `Call_VirtNext` ($9e) answers d0=0; the camera's second
VM set every unknown hcall to a no-op, so d0 stayed non-zero and the loop never ended
(the "object" was the host's Lvirt_obj buffer, type 11 scenery). Fix: gl_camera.c
batch_hcalls answers 0 for $9e and $9f. Confirmed by turning scenery off
(scenery_radius_km=0): picture stable. The temporary diagnostics and limits tried on the way (an object cap, a lower jump
cap, a back-off after a failure, a detailed failure message) were removed again once the
cause was found; a picture takes ~18000 jumps. Lesson: any new hcall whose answer a
loop depends on needs an entry in batch_hcalls.

Original diagnosis follows (the closing-the-screen symptom still holds):

Originally: diagnosed, not fixed. Confirmed by the user: closing the CAMERA screen
returns the game to normal speed.

## Symptom

New game on Earth, `f` shows the fps display. After launch the ship rises
smoothly at normal speed. Yawing about 35 degrees left over the pad makes the
game stand still while the fps display still reads about 140 fps. `top` shows
one core at about 100%. Closing the CAMERA screen cures it.

## Evidence (glfrontier.log, 2026-11-03 20:33-20:45)

- `HEART` lines (every 5 s, counters: game updates / idles / render frames).
  Before the problem: about 50 game updates/s. After 20:34:50: about 2.5
  updates/s, and it never recovered. Render frames stayed at about 60/s, and
  `idles` stayed fixed. So the render thread is fine and keeps redrawing a stale
  scene (that is why fps looks fine); the game thread is spending about 0.4 s of
  emulated 68k time per update.
- At the same moment the log says
  `Camera screen: no picture (game code never returned: looping at $32912)`,
  repeated about every second (9 times in the session, first at 20:34:51).
  `$32912` is `FE2_Project3DObj` (`L3e0a4_Project3DObj`, fe2_labels.h).
- The message comes from `src/galaxy/fe2vm.c` (`fe2vm_check`): the camera screen
  (`src/gl/gl_camera.c`) runs the game's own code in a second VM on the game
  thread to draw its picture; the run is cut off after `MAX_TICKS` = 3,000,000
  jumps and reported as "never returned". Each failed attempt therefore costs
  millions of emulated jumps, and it is retried every second.

## Cause (most likely)

The camera screen's second-VM picture run does not finish for some views (the
yaw that points the camera at something `Project3DObj` loops on), and the
abort is expensive. At 3,000,000 jumps per attempt, even a few attempts a second
starve the main game thread. Not yet shown: what makes `Project3DObj` loop for
that view, and why a yaw of about 35 degrees over the Earth pad triggers it.

## Other candidates that were considered (not ruled out, but the CAMERA test
points at the first)

- Many scenery copies projected per frame (uncommitted deterministic-scenery
  work, `src/scenery.c`, up to 900 copies); would not explain the cure by
  closing the camera screen.
- Cockpit multi-pass rendering needing more passes at that angle.

## Suggested next steps

1. Reproduce with the camera screen open; check `GLF_FRAME_PROF=1`
   (`frameprof.csv`) to see the time per frame go to the camera picture.
2. Find why `Project3DObj` does not return for that view (log the object and
   camera pose when `fe2vm_check` trips; the tool in `fe2vm.c` already records
   the last 256 jump targets).
3. Fix the cause, and also make a failed run cheap: lower the abort limit, or
   skip the camera picture for some seconds after a failure, or only run it
   when the camera screen is visible, so a failure can never stall the game.
