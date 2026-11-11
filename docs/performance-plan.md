# Performance plan (2026-11-11)

Status: plan only, nothing implemented. Written for a session that starts cold: read this whole file,
then `CLAUDE.md`, `docs/native-model-cache.md` and `../devtools/README.md` before changing anything.

Goal: hold 60 pictures/s with no missed refreshes on the user's laptop (Ryzen 7 PRO 4750U, Radeon iGPU,
Mesa, 1920x1080 at 60 Hz) with the user's settings (`../glfrontier.cfg`: Detail x16 =
`native_detail_boost=4`, all shadows on, `camera_hires=1`, `realtime_clock=1`).

## What was found

### 1. The CPU clocks down between frames (measured, but only in a standalone benchmark)

With vsync the render thread works a few ms and sleeps till the next refresh. The governor here is
`schedutil` on `acpi-cpufreq` (three steps: 1.4 / 1.6 / 1.7 GHz, plus boost to ~4.2 GHz). It sees a mostly
idle thread and keeps the core slow, so the work is done at about 1.4-1.7 GHz, not 4 GHz.

A fixed piece of work (appendix A), timed back to back and then once per 16.67 ms with a sleep between:

| Work at full clock | Once per frame, p50 | Once per frame, p95 | With a utilisation clamp, p50 / p95 |
|---|---|---|---|
| 0.85 ms | 2.2 ms | 2.6 ms | 1.7 / 2.3 ms |
| 2.2 ms | 5.7 ms | 6.4 ms | 3.1 / 3.8 ms |
| 5.5 ms | 6.4-7.0 ms | 13.7 ms (max 15) | 6.2 / 6.9 ms |
| 9.9 ms | 10.7 ms | 11.3 ms | not run |
| 13.2 ms | 13.8 ms | 14.5 ms (max 16.9) | not run |

The 5.5 ms row is the "struggling to hold 60" pattern: the clock flaps between slow and boosted, so a job
that fits easily sometimes takes nearly the whole frame. Heavier jobs keep the clock up by themselves.

This has NOT yet been confirmed inside the game. Step 1 does that.

### 2. The game's own log points the same way

`../glfrontier.log` of 2026-11-11 (user playing, mostly external view): all 282 ten-second `PERF` windows
show 60.0 pictures/s. Picture CPU p95 reached 14 ms of the 16.7 ms budget while the GPU needed 1.2-3 ms
(`gpu ms window`). The render thread averaged 21% of a core (peak 55%). Its core read under 2 GHz in 241
of 286 `PERF cpu` samples; those readings are instantaneous, so suggestive, not proof.

### 3. Earlier profiling was done under conditions the game never runs in

The sessions of 2026-11-06/07 mostly measured with `GLF_NOVSYNC=1`, which keeps the thread busy and the
clock at 4 GHz. That would explain London being slower with vsync (50-55 pictures/s) than uncapped
(58-65), and the user seeing ~30 where tests showed 60+. The optimisations made then were real; their
measured headroom was not.

### 4. Heavy scenes still have real cost at full clock

Not re-profiled on 2026-11-11. From 2026-11-07, London (save Snap27), Detail x16, uncapped, per picture
about 16 ms: `gd_flush` + overlays ~4.2 (untimed remainder, not split), sky/planets/atmosphere
(`native_draw_sky`) ~3.5, model runs 3.5-4.3, shadows 2-3, `native_draw` 2.3, sort + spans 0.5.
The render thread was 100% busy and the swap ~0.2 ms: CPU-bound, the GPU has headroom.

## Rules for whoever implements this

- The user is usually playing: never touch their running game or build into `build-modded` /
  `build-original` while it runs. Use a scratch build dir and `BIN=` with `../devtools/t.sh`, drive test
  instances by PID, start them with `--nosound` from a run dir that has a copy of the user's cfg
  (`../glfrontier.cfg`, NOT `../devtools/run/glfrontier.cfg`, which differs).
- Never `pkill -f` with the binary path or a script name in the command.
- Measure with vsync ON and no `GLF_NOVSYNC`. Uncapped numbers are only for splitting costs.
- The user's game shares the CPU and GPU with tests: A/B with several alternating runs, not one each.
- Host code shared by both variants: build both (`-DGLF_MODDED_FE2=ON` and `OFF`).
- Match the surrounding style, don't modernise, commit only when asked.
- Do the steps in order and stop after each one's check; steps 2-4 depend on what step 1 shows.

## Step 1 - Measure under real conditions

In `src/perf_log.c` (the always-on `PERF` lines; pictures are reported from `draw_picture` in
`src/gl/gl_screen.c` through `perf_picture`):

- **Missed refreshes** per window: count swap-to-swap gaps over 1.5x the display's refresh period
  (after `SDL_GL_SwapWindow`). This is the number that matches what the user sees; "60.0 pictures/s"
  hides single drops. Not meaningful in VR or with swap interval 0: leave it out there.
- **Clock factor**: once per picture, at the start of the render thread's work, time a tiny fixed
  loop (~20 us at full clock; calibrate its full-speed time at startup as the least of many back-to-back
  runs) and log p50/p95 of measured/full-speed per window. 1.0 = full clock, 2.5-3 = the slow steps.
- Add both to the `PERF summary` table.

Reference scenes, all with the user's cfg, vsync on, at least 60 s each after the scene settles:

1. `savs/Snap27` London flight. It loads paused in rear view: `--do "load Snap27; waitgame; poke b a6+706 0;
   wait 2; click 22 182; wait 60; quit"` (front view + unpause).
2. Mars landed: `--do "new 2; waitgame; wait 60; quit"`.
3. `savs/KSC2`.

Done when: a table of missed refreshes, clock factor, picture CPU p50/p95/p99 and GPU ms for the three
scenes exists. If the clock factor is near 1.0 throughout, finding 1 does not hold in the game: skip
step 2 and say so.

## Step 2 - Keep the clock up while the game runs

- Linux: `sched_setattr` with `SCHED_FLAG_UTIL_CLAMP_MIN` (plus `KEEP_POLICY | KEEP_PARAMS`) on the render
  thread, the game thread and the `native_worker` thread (appendix A has working code; no privileges
  needed, `CONFIG_UCLAMP_TASK=y` on this kernel, system limit `sched_util_clamp_min` = 1024). Each thread
  sets its own (tid 0 = caller). Log success or the errno once; a failure is harmless.
- A cfg option (default on), in `settings.c` so it shows in `PERF settings`, and a cog-menu checkbox;
  it costs battery and heat. Try clamp values 1024 and ~512 and keep the lowest that gives the result.
- Windows and Android: nothing in this step. Run the step 1 log on Windows first (it usually boosts the
  foreground app). Android/Quest equivalent is the performance hint API (ADPF: a hint session with a
  target frame time); note it as a follow-up only, the Quest 3 slowdown since vibes-11 may be related.

Done when: alternating A/B on the three scenes shows the clock factor, picture CPU p95 and missed
refreshes with the option on vs off. Expected from the benchmark: p95 roughly halved in light scenes,
little change where the thread is already busy over ~60%.

## Step 3 - Check pacing between the game and render threads

The game thread is paced by `Call_RtPace` (`src/rt_clock.c`: `Screen_WaitFrameTaken`, then `SDL_Delay`
with 1 ms steps) and the render thread by vsync. Two loops near 60 Hz that are not locked: a picture
can show a game frame twice or skip one while both counters say 60.

- Measure first: per `PERF` window, pictures that showed a new game frame vs pictures drawn (the on-screen
  counter's DISTINCT value, `count_fps` in `gl_screen.c`), and game frames never shown.
- Only if repeats/skips are common in steady flight: let the swap release the game thread (the render
  thread signals after `SDL_GL_SwapWindow`, the game thread waits for that instead of a timed sleep), and
  keep the timed pace as the fallback for no-vsync and VR.

Done when: distinct = pictures in steady flight on the three scenes, or the numbers show it already is.

## Step 4 - Cut the remaining real work (only where misses remain after steps 1-3)

Profile again first, under step 1's conditions with step 2 on. Split the untimed part of the picture:
`gd_flush` (`src/gl/gl_draw.c`), overlays/HUD, cockpit screens, the GL context hand-back to the game
thread (`context_to_main`), camera screen. `GLF_FRAME_PROF=1` and `GLF_NATIVE_CACHE=2` already time the
rest. Then take these in order of measured cost, one at a time, each with a before/after table:

1. **`gd_flush` CPU vertex path**: everything not in a GPU mesh is transformed on the CPU into ~70-byte
   `GdVert`s and uploaded again every picture (`glBufferData` + `glBufferSubData`, gl_draw.c ~1098).
   Find which callers still send many vertices at x16 (underlays, cloud-shadow faces, live sub-models,
   planets, dots) and move the big ones to kept buffers or do the transform in the shader.
2. **Sky, planets, atmosphere** (`native_draw_sky` in `gl_native.cpp`): keep per game frame what does not
   change between pictures.
3. **Shadows**: ship and scenery casters from GPU meshes, as ground bases already are
   (`gl_shadow_add_mesh`).
4. **Model runs**: per-sub-model meshes so only the changed part runs again. Judged large on
   2026-11-07; only if 1-3 are not enough.

## Step 5 - Target

Proposed: zero missed refreshes per minute on the three scenes with the user's settings and picture CPU
p99 under 8 ms. Past that, more work only matters for VR at 90 Hz.

## Report back

Per step: the before/after table (three scenes, several alternating runs), what was changed (files,
option names), anything that contradicted this plan, and what is still unexplained. If step 1 refutes
finding 1, stop there and report.

## Risks

- The benchmark is synthetic; the game's work pattern (three threads, GL driver threads, swap wait)
  may behave differently.
- The clamp did not close the whole gap (3.1 ms vs 2.2 ms flat out): some wake-up cost remains.
- No system-side fix: this laptop falls back to `acpi-cpufreq` (kernel default is amd-pstate active,
  the firmware has no CPPC), so the only OS option is the `performance` governor, which players can't
  be asked to set.
- Mesa's own driver thread (glthread / radeonsi) may also run slow; the clamp only covers our threads.

## Appendix A - the benchmark

Build: `gcc -O2 -o duty duty.c -lm`. Run: `./duty UNITS [PERIOD_MS [UCLAMP_MIN]]`, e.g. `./duty 10`,
`./duty 10 16.667 1024`. Units 4 / 10 / 25 / 45 / 60 gave the table above.

```c
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

struct sattr { uint32_t size, policy; uint64_t flags; int32_t nice; uint32_t prio;
               uint64_t rt, dl, per; uint32_t umin, umax; };
/* flags: KEEP_POLICY 0x08 | KEEP_PARAMS 0x10 | UTIL_CLAMP_MIN 0x20; pid 0 = this thread */
static int set_uclamp(int minv)
{
	struct sattr a = {0};
	a.size = sizeof a; a.flags = 0x08 | 0x10 | 0x20; a.umin = minv;
	return syscall(SYS_sched_setattr, 0, &a, 0) ? errno : 0;
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static float buf[1 << 18], out[1 << 18];
static double work(int n)
{
	double s = 0;
	for (int k = 0; k < n; k++) {
		for (int i = 0; i < (1 << 18); i++) { out[i] = buf[i] * 1.0001f + 0.5f; s += out[i]; }
		memcpy(buf, out, sizeof buf);
	}
	return s;
}
static int cmp(const void *a, const void *b) { double x = *(double *)a, y = *(double *)b; return x < y ? -1 : x > y; }

int main(int c, char **v)
{
	int n = c > 1 ? atoi(v[1]) : 8;
	double period = c > 2 ? atof(v[2]) : 16.667;
	for (int i = 0; i < (1 << 18); i++) buf[i] = i;
	if (c > 3) { int e = set_uclamp(atoi(v[3])); printf("uclamp min %s -> %s\n", v[3], e ? strerror(e) : "ok"); }
	volatile double sink = 0; double t[400];
	for (int i = 0; i < 60; i++) sink += work(n);
	for (int i = 0; i < 200; i++) { double a = now(); sink += work(n); t[i] = now() - a; }
	qsort(t, 200, sizeof *t, cmp);
	printf("continuous: p50 %.2f p95 %.2f ms\n", t[100], t[190]);
	struct timespec ts = {0, 300000000}; nanosleep(&ts, 0);
	double next = now();
	for (int i = 0; i < 360; i++) {
		double a = now(); sink += work(n); double d = now() - a;
		if (i >= 60) t[i - 60] = d;
		next += period; double w = next - now();
		if (w > 0) { ts.tv_sec = 0; ts.tv_nsec = w * 1e6; nanosleep(&ts, 0); } else next = now();
	}
	qsort(t, 300, sizeof *t, cmp);
	printf("once per %.1f ms: p50 %.2f p95 %.2f max %.2f ms\n", period, t[150], t[285], t[299]);
	return 0;
}
```
