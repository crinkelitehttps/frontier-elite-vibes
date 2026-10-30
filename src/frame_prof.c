/*
 * frame_prof.c - per game frame time breakdown, see frame_prof.h.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#if defined(__linux__) || defined(__APPLE__)
#include <time.h>
#endif

#include "frame_prof.h"
#include "main.h"

#define HC_DRAW_SCREEN 0x6f

volatile bool prof_on;

/* groups of host calls (columns); the numbers are in hostcall.c */
enum
{
	G_HOSTUPD, /* 0x07 */
	G_IDLE,    /* 0x1f: sleeping until the next 20 ms tick */
	G_SOUND,   /* play sample / music */
	G_BLIT,    /* the game's own 2D drawing into the emulated screen */
	G_RECORD,  /* Nu_*: the 3D engine's primitives recorded for the renderer */
	G_NATIVE,  /* native scene hooks */
	G_COCKPIT, /* cockpit passes, sky, screen controls */
	G_HUD,
	G_FILES,
	G_OTHER,
	G_COUNT
};
static const char *const group_name[G_COUNT] = {"hostupd", "idle", "sound_calls", "blit2d", "record3d", "native", "cockpit", "hud",
												"files", "other_calls"};

static int group_of(int n)
{
	switch (n)
	{
	case 0x07:
		return G_HOSTUPD;
	case 0x1f:
		return G_IDLE;
	case 0x12:
	case 0x1d:
	case 0x1e:
	case 0x21:
		return G_SOUND;
	case 0x7a:
	case 0x7d:
	case 0x7e:
	case 0x7f:
	case 0x80:
	case 0x85:
	case 0x8a:
	case 0x8b:
	case 0x8c:
		return G_COCKPIT;
	case 0x81:
	case 0x82:
	case 0x83:
	case 0x84:
		return G_NATIVE;
	case 0x86:
	case 0x87:
	case 0x88:
	case 0x89:
		return G_HUD;
	}
	if (n >= 0x22 && n <= 0x2a)
		return G_FILES;
	if (n >= 0x60 && n <= 0x79 && n != HC_DRAW_SCREEN)
		return G_RECORD;
	if (n <= 0x1c && n != 0x00 && n != 0x10 && n != 0x13 && n != 0x14 && n != 0x18 && n != 0x19)
		return G_BLIT;
	return G_OTHER;
}

/* Nu_DrawScreen's own column is separate from the groups: it holds the parts
 * below it */
#define COLS_PARTS PS_COUNT
static const char *const part_name[PS_COUNT] = {"vr_refresh", "keys", "events", "updates", "netplay", "text", "publish", "pace",
												"pub_lock", "pub_scene", "pub_hud", "pub_rest", "pub_page", "pub_world", "pub_camera", "ctx_handoff", "draw_inline"};

/* game thread */
static uint64_t freq, last_end, start_ticks;
static int depth;
static uint64_t call_ticks[256]; /* this frame, inclusive */
static unsigned call_count[256];
static uint64_t part_ticks[PS_COUNT];
static uint64_t top_ticks[256]; /* this summary interval */
static unsigned top_count[256];
static uint64_t outer_ticks; /* depth 0 host calls this frame */
static double cpu_last;

/* other threads, µs */
static SDL_atomic_t audio_us, render_us, swap_us, acquire_us, pictures;
static SDL_atomic_t vr_wait_us, vr_eyes_us, vr_end_us, vr_gpu_us, vr_gpu_n, vr_n;

/* summary interval */
static unsigned long frame_no, sum_frames, over_slow;
static double sum_frame_ms, max_frame_ms, sum_group[G_COUNT], sum_emu, sum_drawscreen, sum_part[PS_COUNT], sum_audio, sum_render, sum_swap;
static unsigned long sum_pics, sum_vr, sum_vr_gpu_n;
static double sum_vr_wait, sum_vr_eyes, sum_vr_end, sum_vr_gpu;
static uint64_t sum_start;
static double slow_ms = 40.0;
static FILE *csv;

void prof_init(void)
{
	const char *e = SDL_getenv("GLF_FRAME_PROF");
	if (!e || !*e || *e == '0')
		return;
	const char *s = SDL_getenv("GLF_FRAME_PROF_SLOW");
	if (s && atof(s) > 0)
		slow_ms = atof(s);
	freq = SDL_GetPerformanceFrequency();
	start_ticks = SDL_GetPerformanceCounter();
	csv = fopen("frameprof.csv", "w");
	if (csv)
	{
		fprintf(csv, "frame,wall_s,mode,frame_ms,emu_ms");
		for (int g = 0; g < G_COUNT; g++)
			fprintf(csv, ",%s_ms", group_name[g]);
		fprintf(csv, ",drawscreen_ms");
		for (int p = 0; p < PS_COUNT; p++)
			fprintf(csv, ",%s_ms", part_name[p]);
		fprintf(csv, ",audio_ms,render_ms,swap_ms,acquire_ms,pictures,cpu_ms,hostupd_calls,idle_calls,vr_pictures,vr_wait_ms,vr_eyes_cpu_ms,vr_end_ms,vr_gpu_ms,vr_gpu_n\n");
	}
	log_printf("Frame profile on: frameprof.csv%s, slow frames over %.0f ms\n", csv ? "" : " (cannot open)", slow_ms);
	prof_on = true;
}

uint64_t prof_now(void)
{
	return SDL_GetPerformanceCounter();
}

/* log_printf stamps every call: a line is built first */
static char line[4096];
static int line_len;

static void put(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	if (line_len < (int)sizeof line)
		line_len += vsnprintf(line + line_len, sizeof line - line_len, fmt, args);
	va_end(args);
}

static void put_end(void)
{
	log_printf("%s\n", line);
	line_len = 0;
}

/* CPU time the game thread has used, ms: tells work from waiting and from
 * being descheduled (0 where there is no per thread clock) */
static double thread_cpu_ms(void)
{
#if defined(CLOCK_THREAD_CPUTIME_ID)
	struct timespec ts;
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
		return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
	return 0;
}

static double ms(uint64_t ticks)
{
	return (double)ticks * 1000.0 / (double)freq;
}

uint64_t prof_call_begin(void)
{
	depth++;
	return SDL_GetPerformanceCounter();
}

bool prof_call_end(int n, uint64_t t0)
{
	uint64_t dt = SDL_GetPerformanceCounter() - t0;
	if (depth > 0)
		depth--;
	call_ticks[n & 255] += dt;
	call_count[n & 255]++;
	if (depth == 0)
		outer_ticks += dt;
	return n == HC_DRAW_SCREEN && depth == 0;
}

void prof_part(int slot, uint64_t t0)
{
	part_ticks[slot] += SDL_GetPerformanceCounter() - t0;
}

void prof_audio_add(uint64_t t0)
{
	SDL_AtomicAdd(&audio_us, (int)((SDL_GetPerformanceCounter() - t0) * 1000000 / freq));
}

void prof_render_acquire(uint64_t t0)
{
	SDL_AtomicAdd(&acquire_us, (int)((SDL_GetPerformanceCounter() - t0) * 1000000 / freq));
}

void prof_vr_add(uint64_t wait, uint64_t eyes, uint64_t end)
{
	SDL_AtomicAdd(&vr_wait_us, (int)(wait * 1000000 / freq));
	SDL_AtomicAdd(&vr_eyes_us, (int)(eyes * 1000000 / freq));
	SDL_AtomicAdd(&vr_end_us, (int)(end * 1000000 / freq));
	SDL_AtomicAdd(&vr_n, 1);
}

void prof_vr_gpu(unsigned ns)
{
	SDL_AtomicAdd(&vr_gpu_us, (int)(ns / 1000));
	SDL_AtomicAdd(&vr_gpu_n, 1);
}

void prof_render_add(uint64_t t0, uint64_t swap0, uint64_t swap1)
{
	SDL_AtomicAdd(&render_us, (int)((SDL_GetPerformanceCounter() - t0) * 1000000 / freq));
	SDL_AtomicAdd(&swap_us, (int)((swap1 - swap0) * 1000000 / freq));
	SDL_AtomicAdd(&pictures, 1);
}

static void summary(uint64_t now)
{
	double n = (double)sum_frames, secs = ms(now - sum_start) / 1000.0;
	put("PROF %.1f frames/s, frame avg %.2f max %.2f ms, %lu over %.0f ms; emu %.2f", sum_frames / secs, sum_frame_ms / n, max_frame_ms,
			   over_slow, slow_ms, sum_emu / n);
	for (int g = 0; g < G_COUNT; g++)
		if (sum_group[g] > 0)
			put(" %s %.2f", group_name[g], sum_group[g] / n);
	put(" (ms/frame)");
	put_end();
	put("PROF drawscreen %.2f =", sum_drawscreen / n);
	for (int p = PS_NETPLAY; p < PS_COUNT; p++)
		put(" %s %.2f", part_name[p], sum_part[p] / n);
	put("; hostupd:");
	for (int p = PS_VR_REFRESH; p <= PS_UPDATES; p++)
		put(" %s %.2f", part_name[p], sum_part[p] / n);
	put_end();
	log_printf("PROF other threads per second: audio mix %.1f ms, render %.1f ms (swap %.1f) over %.1f pictures/s\n", sum_audio / secs, sum_render / secs,
			   sum_swap / secs, sum_pics / secs);
	if (sum_vr)
		log_printf("PROF headset %.1f pictures/s, per picture: xrWaitFrame %.2f ms, eyes drawn (CPU) %.2f, xrEndFrame %.2f, eyes on the GPU %.2f (%lu measured)\n", sum_vr / secs,
				   sum_vr_wait / sum_vr, sum_vr_eyes / sum_vr, sum_vr_end / sum_vr, sum_vr_gpu_n ? sum_vr_gpu / sum_vr_gpu_n : 0.0, sum_vr_gpu_n);
	/* every host call made: calls per frame, and its ms per frame */
	put("PROF calls/frame:");
	for (int i = 0; i < 256; i++)
		if (top_count[i] && i != 0x07 && i != 0x1f)
			put(" $%02x=%.1f(%.2fms)", i, top_count[i] / n, ms(top_ticks[i]) / n);
	put_end();
	/* the three costliest host calls */
	for (int k = 0; k < 3; k++)
	{
		int best = -1;
		for (int i = 0; i < 256; i++)
			if (top_ticks[i] && i != HC_DRAW_SCREEN && i != 0x07 && i != 0x1f && (best < 0 || top_ticks[i] > top_ticks[best]))
				best = i;
		if (best < 0)
			break;
		log_printf("PROF host call $%02x: %.3f ms/frame, %.1f calls/frame, %.1f us each\n", best, ms(top_ticks[best]) / n, top_count[best] / n,
				   ms(top_ticks[best]) * 1000.0 / top_count[best]);
		top_ticks[best] = 0;
	}
	memset(top_ticks, 0, sizeof top_ticks);
	memset(top_count, 0, sizeof top_count);
	sum_frames = over_slow = sum_pics = sum_vr = sum_vr_gpu_n = 0;
	sum_vr_wait = sum_vr_eyes = sum_vr_end = sum_vr_gpu = 0;
	sum_frame_ms = max_frame_ms = sum_emu = sum_drawscreen = sum_audio = sum_render = sum_swap = 0;
	memset(sum_group, 0, sizeof sum_group);
	memset(sum_part, 0, sizeof sum_part);
	sum_start = now;
}

/* Game thread, when Nu_DrawScreen returns: the frame is over */
void prof_frame_end(int mode)
{
	uint64_t now = SDL_GetPerformanceCounter();
	if (!last_end)
	{
		/* startup took the time until now */
		last_end = sum_start = now;
		goto reset;
	}
	uint64_t frame = now - last_end;
	double group[G_COUNT] = {0}, part[PS_COUNT], frame_ms = ms(frame);
	for (int n = 0; n < 256; n++)
	{
		if (!call_ticks[n])
			continue;
		if (n != HC_DRAW_SCREEN)
			group[group_of(n)] += ms(call_ticks[n]);
		top_ticks[n] += call_ticks[n];
		top_count[n] += call_count[n];
	}
	double drawscreen = ms(call_ticks[HC_DRAW_SCREEN]);
	double emu = frame_ms - ms(outer_ticks);
	for (int p = 0; p < PS_COUNT; p++)
		part[p] = ms(part_ticks[p]);
	double audio = SDL_AtomicSet(&audio_us, 0) / 1000.0, render = SDL_AtomicSet(&render_us, 0) / 1000.0;
	double swap = SDL_AtomicSet(&swap_us, 0) / 1000.0, acq = SDL_AtomicSet(&acquire_us, 0) / 1000.0;
	int pics = SDL_AtomicSet(&pictures, 0);
	int vrn = SDL_AtomicSet(&vr_n, 0), vrgn = SDL_AtomicSet(&vr_gpu_n, 0);
	double vrw = SDL_AtomicSet(&vr_wait_us, 0) / 1000.0, vre = SDL_AtomicSet(&vr_eyes_us, 0) / 1000.0;
	double vrd = SDL_AtomicSet(&vr_end_us, 0) / 1000.0, vrg = SDL_AtomicSet(&vr_gpu_us, 0) / 1000.0;

	frame_no++;
	if (csv)
	{
		fprintf(csv, "%lu,%.3f,%02x,%.3f,%.3f", frame_no, ms(now - start_ticks) / 1000.0, mode, frame_ms, emu);
		for (int g = 0; g < G_COUNT; g++)
			fprintf(csv, ",%.3f", group[g]);
		fprintf(csv, ",%.3f", drawscreen);
		for (int p = 0; p < PS_COUNT; p++)
			fprintf(csv, ",%.3f", part[p]);
		double cpu_now = thread_cpu_ms();
		fprintf(csv, ",%.3f,%.3f,%.3f,%.3f,%d,%.3f,%u,%u,%d,%.3f,%.3f,%.3f,%.3f,%d\n", audio, render, swap, acq, pics, cpu_now - cpu_last,
				call_count[0x07], call_count[0x1f], vrn, vrw, vre, vrd, vrg, vrgn);
		cpu_last = cpu_now;
		if (frame_no % 50 == 0)
			fflush(csv);
	}

	sum_frames++;
	sum_frame_ms += frame_ms;
	if (frame_ms > max_frame_ms)
		max_frame_ms = frame_ms;
	sum_emu += emu;
	sum_drawscreen += drawscreen;
	for (int g = 0; g < G_COUNT; g++)
		sum_group[g] += group[g];
	for (int p = 0; p < PS_COUNT; p++)
		sum_part[p] += part[p];
	sum_audio += audio;
	sum_render += render;
	sum_swap += swap;
	sum_pics += pics;
	sum_vr += vrn;
	sum_vr_gpu_n += vrgn;
	sum_vr_wait += vrw;
	sum_vr_eyes += vre;
	sum_vr_end += vrd;
	sum_vr_gpu += vrg;

	/* a frame past the limit says where it went (idle sleeping is not slowness) */
	if (frame_ms - group[G_IDLE] > slow_ms)
	{
		over_slow++;
		if (over_slow <= 10)
		{
			put("PROF slow frame %lu (mode %02x): %.1f ms = emu %.1f", frame_no, mode, frame_ms, emu);
			for (int g = 0; g < G_COUNT; g++)
				if (group[g] >= 0.5)
					put(" %s %.1f", group_name[g], group[g]);
			put(" drawscreen %.1f", drawscreen);
			for (int p = 0; p < PS_COUNT; p++)
				if (part[p] >= 0.5)
					put(" %s %.1f", part_name[p], part[p]);
			put_end();
		}
	}
	if (ms(now - sum_start) >= 2000.0)
		summary(now);
	last_end = now;
reset:
	memset(call_ticks, 0, sizeof call_ticks);
	memset(call_count, 0, sizeof call_count);
	memset(part_ticks, 0, sizeof part_ticks);
	outer_ticks = 0;
	last_end = SDL_GetPerformanceCounter();
}
