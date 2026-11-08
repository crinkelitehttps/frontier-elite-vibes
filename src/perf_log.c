/*
 * perf_log.c - see perf_log.h.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#if defined(__linux__) || defined(ANDROID)
#include <dirent.h>
#include <unistd.h>
#define PERF_PROC 1
#else
#define PERF_PROC 0
#endif
#ifdef _WIN32
#include <windows.h>
#endif

#include "game_state.h"
#include "main.h"
#include "perf_log.h"
#include "vr.h"

#define BUCKET_MS 0.5
#define BUCKETS   400 /* 0..200 ms; the last one holds everything longer */
#define SLOW_MS   50.0

typedef struct
{
	unsigned n[BUCKETS];
	unsigned count;
	double sum, max;
	unsigned slow; /* over SLOW_MS */
} Hist;

static void hist_add(Hist *h, double ms)
{
	int b = (int)(ms / BUCKET_MS);
	h->n[b < 0 ? 0 : b >= BUCKETS ? BUCKETS - 1 : b]++;
	h->count++;
	h->sum += ms;
	if (ms > h->max)
		h->max = ms;
	if (ms > SLOW_MS)
		h->slow++;
}

static void hist_merge(Hist *to, const Hist *from)
{
	for (int i = 0; i < BUCKETS; i++)
		to->n[i] += from->n[i];
	to->count += from->count;
	to->sum += from->sum;
	to->slow += from->slow;
	if (from->max > to->max)
		to->max = from->max;
}

/* the upper edge of the bucket holding fraction q of the samples */
static double hist_pct(const Hist *h, double q)
{
	if (!h->count)
		return 0;
	unsigned want = (unsigned)ceil(q * h->count), seen = 0;
	for (int i = 0; i < BUCKETS; i++)
	{
		seen += h->n[i];
		if (seen >= want)
			return i == BUCKETS - 1 || (i + 1) * BUCKET_MS > h->max ? h->max : (i + 1) * BUCKET_MS;
	}
	return h->max;
}

/* what one interval gathered */
typedef struct
{
	Hist game;    /* game frame to game frame */
	Hist picture; /* a picture's CPU time on the drawing thread */
	unsigned vr_frames, vr_rendered, vr_missed;
	double vr_wait_ms;
	double gpu_ms[PERF_GPU_SLOTS];
	unsigned gpu_n[PERF_GPU_SLOTS];
} Interval;

static SDL_SpinLock lock;
static Interval cur;
static uint64_t freq, last_game, last_report;
static int64_t last_display;
static int enabled = -1;
static double interval_s = 10;
static int gpu_state = -1; /* -1 unknown, 0 no, 1 yes */
static char gpu_why[96];

static bool on(void)
{
	if (enabled < 0)
	{
		const char *e = SDL_getenv("GLF_PERF_LOG");
		enabled = !(e && *e == '0' && !e[1]);
		if (e && atof(e) >= 1)
			interval_s = atof(e);
		freq = SDL_GetPerformanceFrequency();
	}
	return enabled;
}

static double ms(uint64_t ticks)
{
	return (double)ticks * 1000.0 / (double)freq;
}

void perf_game_frame(void)
{
	if (!on())
		return;
	uint64_t now = SDL_GetPerformanceCounter();
	if (last_game)
	{
		double d = ms(now - last_game);
		SDL_AtomicLock(&lock);
		hist_add(&cur.game, d);
		SDL_AtomicUnlock(&lock);
	}
	last_game = now;
}

void perf_picture(uint64_t t0, uint64_t t1)
{
	if (!on())
		return;
	SDL_AtomicLock(&lock);
	hist_add(&cur.picture, ms(t1 - t0));
	SDL_AtomicUnlock(&lock);
}

void perf_vr_frame(int64_t display_ns, int64_t period_ns, bool render, uint64_t wait)
{
	if (!on())
		return;
	unsigned missed = 0;
	if (last_display && period_ns > 0 && display_ns > last_display)
	{
		/* the display times come one period apart; a bigger step is frames the compositor showed again */
		long long steps = llround((double)(display_ns - last_display) / (double)period_ns);
		if (steps > 1 && steps < 1000)
			missed = (unsigned)(steps - 1);
	}
	last_display = display_ns;
	SDL_AtomicLock(&lock);
	cur.vr_frames++;
	cur.vr_rendered += render;
	cur.vr_missed += missed;
	cur.vr_wait_ms += ms(wait);
	SDL_AtomicUnlock(&lock);
}

void perf_gpu_add(int slot, double gms)
{
	if (slot < 0 || slot >= PERF_GPU_SLOTS)
		return;
	SDL_AtomicLock(&lock);
	cur.gpu_ms[slot] += gms;
	cur.gpu_n[slot]++;
	SDL_AtomicUnlock(&lock);
}

void perf_gpu_available(bool yes, const char *why)
{
	gpu_state = yes;
	snprintf(gpu_why, sizeof gpu_why, "%s", why ? why : "");
	log_printf("PERF GPU timing %s%s%s\n", yes ? "on" : "off", why && *why ? ": " : "", why ? why : "");
}

void perf_setting(const char *line)
{
	log_printf("PERF settings: %s\n", line);
}

/* ---- what the player is doing ---- */

static void situation(char *out, size_t n)
{
	GameScreen s = game_screen();
	if (s == SCREEN_FLIGHT)
		snprintf(out, n, "flight/%s/%s%s", game_status_name(game_flight_status()), game_view_name(game_flight_view()),
				 vr_running() ? " VR" : "");
	else
		snprintf(out, n, "%s%s", game_screen_name(s), vr_running() ? " VR" : "");
}

/* ---- the session table ---- */

#define SITUATIONS 32
typedef struct
{
	char name[64];
	double secs;
	Interval sum;
} Situation;
static Situation table[SITUATIONS];
static int n_table;
static SDL_SpinLock table_lock;

static void table_add(const char *name, double secs, const Interval *iv)
{
	SDL_AtomicLock(&table_lock);
	int i;
	for (i = 0; i < n_table && strcmp(table[i].name, name); i++)
		;
	if (i == n_table)
	{
		if (n_table == SITUATIONS)
			i = SITUATIONS - 1; /* the rest share the last row */
		else
		{
			n_table++;
			memset(&table[i], 0, sizeof table[i]);
			snprintf(table[i].name, sizeof table[i].name, "%s", name);
		}
	}
	Situation *t = &table[i];
	t->secs += secs;
	hist_merge(&t->sum.game, &iv->game);
	hist_merge(&t->sum.picture, &iv->picture);
	t->sum.vr_frames += iv->vr_frames;
	t->sum.vr_rendered += iv->vr_rendered;
	t->sum.vr_missed += iv->vr_missed;
	t->sum.vr_wait_ms += iv->vr_wait_ms;
	for (int g = 0; g < PERF_GPU_SLOTS; g++)
	{
		t->sum.gpu_ms[g] += iv->gpu_ms[g];
		t->sum.gpu_n[g] += iv->gpu_n[g];
	}
	SDL_AtomicUnlock(&table_lock);
}

/* the frame and GPU part of a line, shared by the report and the summary */
static int describe(char *out, size_t n, const Interval *iv, double secs)
{
	int len = snprintf(out, n, "game %.1f/s p50 %.1f p95 %.1f p99 %.1f max %.1f ms, %u over %.0f ms; pictures %.1f/s cpu p50 %.1f p95 %.1f ms",
					   iv->game.count / secs, hist_pct(&iv->game, 0.5), hist_pct(&iv->game, 0.95), hist_pct(&iv->game, 0.99),
					   iv->game.max, iv->game.slow, SLOW_MS, iv->picture.count / secs, hist_pct(&iv->picture, 0.5),
					   hist_pct(&iv->picture, 0.95));
	if (iv->vr_frames && len < (int)n)
		len += snprintf(out + len, n - len, "; headset %.1f/s, %u missed (%.1f%%), %u not drawn, xrWaitFrame %.1f ms", iv->vr_frames / secs,
						iv->vr_missed, 100.0 * iv->vr_missed / (iv->vr_frames + iv->vr_missed), iv->vr_frames - iv->vr_rendered,
						iv->vr_wait_ms / iv->vr_frames);
	static const char *const gname[PERF_GPU_SLOTS] = {"headset", "window", "screens", "shadow"};
	bool any = false;
	for (int g = 0; g < PERF_GPU_SLOTS && len < (int)n; g++)
	{
		if (!iv->gpu_n[g])
			continue;
		if (!any)
			len += snprintf(out + len, n - len, "; gpu ms");
		any = true;
		if (g == PERF_GPU_SHADOW)
			/* per picture, with the passes it took (the eyes and the window each draw their own) */
			len += snprintf(out + len, n - len, " shadow %.2f/picture (%.1f maps/picture)", iv->picture.count ? iv->gpu_ms[g] / iv->picture.count : 0.0,
							iv->picture.count ? (double)iv->gpu_n[g] / iv->picture.count : 0.0);
		else
			len += snprintf(out + len, n - len, " %s %.2f", gname[g], iv->gpu_ms[g] / iv->gpu_n[g]);
	}
	return len;
}

/* ---- CPU: threads, clocks, heat ---- */

#if PERF_PROC
typedef struct
{
	int tid;
	unsigned long long ticks;
} ThreadTicks;
static ThreadTicks prev[128];
static int n_prev;
static unsigned long long prev_proc;

static bool read_thread(int tid, char *name, size_t nn, unsigned long long *ticks, int *core)
{
	char path[64], buf[1024];
	snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	size_t got = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[got] = 0;
	char *open = strchr(buf, '('), *close = strrchr(buf, ')');
	if (!open || !close)
		return false;
	snprintf(name, nn, "%.*s", (int)(close - open - 1), open + 1);
	/* the fields after the name: state is field 3, utime 14, stime 15, processor 39 */
	unsigned long long ut = 0, st = 0;
	int field = 3, cpu = -1;
	char *save = NULL;
	for (char *p = strtok_r(close + 2, " ", &save); p; p = strtok_r(NULL, " ", &save), field++)
	{
		if (field == 14)
			ut = strtoull(p, NULL, 10);
		else if (field == 15)
			st = strtoull(p, NULL, 10);
		else if (field == 39)
		{
			cpu = atoi(p);
			break;
		}
	}
	*ticks = ut + st;
	*core = cpu;
	return true;
}

typedef struct
{
	char name[20];
	double pct;
	int core;
} ThreadUse;

static int cmp_use(const void *a, const void *b)
{
	double d = ((const ThreadUse *)b)->pct - ((const ThreadUse *)a)->pct;
	return d > 0 ? 1 : d < 0 ? -1 : 0;
}

static int cpu_line(char *out, size_t n, double secs)
{
	static long hz;
	if (!hz)
		hz = sysconf(_SC_CLK_TCK);
	DIR *d = opendir("/proc/self/task");
	if (!d || hz <= 0)
	{
		if (d)
			closedir(d);
		return snprintf(out, n, "threads unreadable");
	}
	ThreadTicks now[128];
	ThreadUse use[128];
	int n_now = 0, n_use = 0;
	unsigned long long proc = 0;
	struct dirent *e;
	while ((e = readdir(d)) && n_now < 128)
	{
		int tid = atoi(e->d_name);
		if (tid <= 0)
			continue;
		char name[32];
		unsigned long long t;
		int core;
		if (!read_thread(tid, name, sizeof name, &t, &core))
			continue;
		proc += t;
		now[n_now].tid = tid;
		now[n_now++].ticks = t;
		unsigned long long was = t;
		for (int i = 0; i < n_prev; i++)
			if (prev[i].tid == tid)
			{
				was = prev[i].ticks;
				break;
			}
		if (n_prev && t > was)
		{
			snprintf(use[n_use].name, sizeof use[n_use].name, "%s", name);
			use[n_use].pct = 100.0 * (double)(t - was) / (double)hz / secs;
			use[n_use++].core = core;
		}
	}
	closedir(d);
	int len = 0;
	if (n_prev)
	{
		int cores = (int)sysconf(_SC_NPROCESSORS_CONF);
		len = snprintf(out, n, "cpu %.0f%% of one core (%d cores):", 100.0 * (double)(proc > prev_proc ? proc - prev_proc : 0) / (double)hz / secs,
					   cores);
		qsort(use, (size_t)n_use, sizeof use[0], cmp_use);
		for (int i = 0; i < n_use && i < 8 && use[i].pct >= 1.0 && len < (int)n; i++)
			len += snprintf(out + len, n - len, " %s %.0f%%@%d", use[i].name, use[i].pct, use[i].core);
		/* the clocks, where the system lets us read them (a throttled core shows here) */
		bool first = true;
		for (int c = 0; c < cores && c < 16 && len < (int)n; c++)
		{
			char path[96];
			snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", c);
			FILE *f = fopen(path, "r");
			long khz = 0;
			if (f)
			{
				if (fscanf(f, "%ld", &khz) != 1)
					khz = 0;
				fclose(f);
			}
			if (!f && first)
				continue;
			len += snprintf(out + len, n - len, "%s%ld", first ? "; MHz " : "/", khz / 1000);
			first = false;
		}
	}
	memcpy(prev, now, sizeof now[0] * (size_t)n_now);
	n_prev = n_now;
	prev_proc = proc;
	return len;
}
#elif defined(_WIN32)
static int cpu_line(char *out, size_t n, double secs)
{
	static ULONGLONG prev_t;
	FILETIME c, x, k, u;
	if (!GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u))
		return 0;
	ULONGLONG t = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
	int len = 0;
	if (prev_t)
	{
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		len = snprintf(out, n, "cpu %.0f%% of one core (%lu cores)", (double)(t - prev_t) / 1e5 / secs, (unsigned long)si.dwNumberOfProcessors);
	}
	prev_t = t;
	return len;
}
#else
static int cpu_line(char *out, size_t n, double secs)
{
	(void)out;
	(void)n;
	(void)secs;
	return 0;
}
#endif

#ifdef ANDROID
#include <jni.h>
/* GLFrontierActivity.perfState(): "thermal S headroom H" */
static int android_line(char *out, size_t n)
{
	static int missing;
	if (missing)
		return 0;
	JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	jobject act = (jobject)SDL_AndroidGetActivity();
	if (!env || !act)
		return 0;
	int len = 0;
	jclass cls = (*env)->GetObjectClass(env, act);
	jmethodID m = (*env)->GetMethodID(env, cls, "perfState", "()Ljava/lang/String;");
	(*env)->DeleteLocalRef(env, cls);
	if ((*env)->ExceptionCheck(env) || !m)
	{
		(*env)->ExceptionClear(env);
		missing = 1;
	}
	else
	{
		jstring s = (jstring)(*env)->CallObjectMethod(env, act, m);
		if ((*env)->ExceptionCheck(env))
		{
			(*env)->ExceptionClear(env);
			s = 0;
		}
		if (s)
		{
			const char *c = (*env)->GetStringUTFChars(env, s, 0);
			len = snprintf(out, n, "; %s", c);
			(*env)->ReleaseStringUTFChars(env, s, c);
			(*env)->DeleteLocalRef(env, s);
		}
	}
	(*env)->DeleteLocalRef(env, act);
	return len;
}
#endif

/* ---- the report ---- */

void perf_tick(void)
{
	if (!on())
		return;
	uint64_t now = SDL_GetPerformanceCounter();
	if (!last_report)
	{
		last_report = now;
		char tmp[16];
		cpu_line(tmp, 0, 1); /* the threads' first counts */
		return;
	}
	double secs = ms(now - last_report) / 1000.0;
	if (secs < interval_s)
		return;
	last_report = now;

	static Interval iv;
	SDL_AtomicLock(&lock);
	iv = cur;
	memset(&cur, 0, sizeof cur);
	SDL_AtomicUnlock(&lock);

	char where[64], line[1024];
	situation(where, sizeof where);
	table_add(where, secs, &iv);
	describe(line, sizeof line, &iv, secs);
	log_printf("PERF %.0fs %s: %s\n", secs, where, line);
	int len = cpu_line(line, sizeof line, secs);
#ifdef ANDROID
	if (len < (int)sizeof line)
		len += android_line(line + len, sizeof line - len);
#endif
	if (len > 0)
		log_printf("PERF %s\n", line);
	if (gpu_state == 0 && gpu_why[0])
	{
		/* once more, so a log cut short still says why there is no GPU time */
		log_printf("PERF no GPU timing: %s\n", gpu_why);
		gpu_why[0] = 0;
	}
}

void perf_summary(const char *why)
{
	if (!on())
		return;
	static Situation copy[SITUATIONS];
	SDL_AtomicLock(&table_lock);
	int n = n_table;
	memcpy(copy, table, sizeof table[0] * (size_t)n);
	SDL_AtomicUnlock(&table_lock);
	log_printf("PERF summary (%s), %d situations, build %s (%s game code):\n", why, n, game_build_tag(),
			   FE2_USE_MODDED ? "modded" : "original");
	for (int i = 0; i < n; i++)
	{
		char line[1024];
		describe(line, sizeof line, &copy[i].sum, copy[i].secs);
		log_printf("PERF summary %s, %.0f s: %s\n", copy[i].name, copy[i].secs, line);
	}
}
