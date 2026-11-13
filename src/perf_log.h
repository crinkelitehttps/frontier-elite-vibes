/*
 * perf_log.h - a short performance report in the log, always on, every build.
 *
 * Every 10 seconds two "PERF" lines: what the player was doing (screen,
 * flight status, VR or not), game frames and pictures per second with their
 * frame-time spread (p50/p95/p99/max), the headset's missed frames, GPU time
 * per picture (headset, window, cockpit screens, shadow maps) where the GPU
 * has timer queries, CPU use per thread with the core it last ran on (Linux,
 * Android), core clocks and, on Android, the thermal state.
 *
 * "PERF settings" lines give the options that cost time, at start and when
 * one changes. A "PERF summary" table, one row per situation, goes into the
 * log at exit and before a diagnostics upload, so a long log can be read
 * from its end.
 *
 * Costs a lock and a few adds per frame; the report itself reads a few
 * /proc files every 10 s. GLF_PERF_LOG=0 turns it off, GLF_PERF_LOG=s sets
 * the interval in seconds.
 */
#ifndef PERF_LOG_H
#define PERF_LOG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* GPU time slots (gl_gputime.c) */
enum
{
	PERF_GPU_HEADSET, /* both eyes, or the virtual screen */
	PERF_GPU_WINDOW,  /* the window's picture */
	PERF_GPU_SCREENS, /* cockpit screens, MAP/SHIP pages, camera picture */
	PERF_GPU_SHADOW,  /* one shadow map pass (inside the others) */
	PERF_GPU_SLOTS
};

void perf_game_frame(void); /* game thread, each Nu_DrawScreen */
void perf_picture(uint64_t t0, uint64_t t1); /* drawing thread: one picture's CPU span, in performance-counter ticks */
/* the headset's frame (xrWaitFrame's answer); wait = ticks spent in xrWaitFrame */
void perf_picture_begin(void); /* drawing thread: first thing in each picture (the clock-speed probe) */
/* drawing thread, right after SDL_GL_SwapWindow; period_ms = the display's refresh period, 0 = not paced by vsync */
void perf_swapped(double period_ms);
void perf_vr_frame(int64_t display_ns, int64_t period_ns, bool render, uint64_t wait);
void perf_gpu_add(int slot, double ms);
void perf_gpu_available(bool yes, const char *why);
void perf_tick(void); /* drawing thread, after each picture: writes the report when due */
void perf_summary(const char *why); /* the session table into the log (any thread) */
void perf_setting(const char *line); /* a "key=value ..." text logged as PERF settings */

#ifdef __cplusplus
}
#endif

#endif /* PERF_LOG_H */
