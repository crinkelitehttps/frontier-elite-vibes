/*
 * gl_gputime.c - see gl_gputime.h.
 */
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "../frame_prof.h"
#include "../main.h"
#include "../perf_log.h"
#include "gl_api.h"
#include "gl_gputime.h"

#if GL_IS_GLES
#ifndef GL_TIMESTAMP_EXT
#define GL_TIMESTAMP_EXT 0x8E28
#endif
#ifndef GL_QUERY_COUNTER_BITS
#define GL_QUERY_COUNTER_BITS 0x8864
#endif
#ifndef GL_GPU_DISJOINT_EXT
#define GL_GPU_DISJOINT_EXT 0x8FBB
#endif
typedef void(GL_APIENTRYP CounterFn)(GLuint id, GLenum target);
typedef void(GL_APIENTRYP Result64Fn)(GLuint id, GLenum pname, GLuint64 *params);
static CounterFn query_counter;
static Result64Fn result64;
#define TIMESTAMP GL_TIMESTAMP_EXT
#else
#define query_counter glQueryCounter
#define result64      glGetQueryObjectui64v
#define TIMESTAMP     GL_TIMESTAMP
#endif

/* a ring of spans, each two timestamp queries; read in order */
#define SPANS 128
typedef struct
{
	int slot;
	bool open, issued;
} Span;
static Span spans[SPANS];
static GLuint queries[SPANS * 2];
static int head, tail; /* next to hand out, oldest unread */
static int state; /* 0 not tried, 1 on, -1 off */

static bool ready(void)
{
	if (state)
		return state > 0;
	state = -1;
	const char *env = SDL_getenv("GLF_GPU_TIME");
	const char *gl_r = (const char *)glGetString(GL_RENDERER), *gl_v = (const char *)glGetString(GL_VERSION);
	log_printf("PERF GPU %s, %s\n", gl_r ? gl_r : "?", gl_v ? gl_v : "?");
	if (env && *env == '0')
	{
		perf_gpu_available(false, "GLF_GPU_TIME=0");
		return false;
	}
#if GL_IS_GLES
	const char *ext = (const char *)glGetString(GL_EXTENSIONS);
	if (!ext || !strstr(ext, "GL_EXT_disjoint_timer_query"))
	{
		perf_gpu_available(false, "no GL_EXT_disjoint_timer_query");
		return false;
	}
	query_counter = (CounterFn)SDL_GL_GetProcAddress("glQueryCounterEXT");
	result64 = (Result64Fn)SDL_GL_GetProcAddress("glGetQueryObjectui64vEXT");
	if (!query_counter || !result64)
	{
		perf_gpu_available(false, "timer query functions missing");
		return false;
	}
	GLint bits = 0;
	glGetQueryiv(TIMESTAMP, GL_QUERY_COUNTER_BITS, &bits);
	glGetError();
	if (bits <= 0)
	{
		perf_gpu_available(false, "no timestamp counter");
		return false;
	}
	GLint disjoint;
	glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint); /* clears the flag */
#endif
	glGenQueries(SPANS * 2, queries);
	state = 1;
	perf_gpu_available(true, NULL);
	return true;
}

int gputime_begin(int slot)
{
	if (!ready())
		return -1;
	if ((head + 1) % SPANS == tail)
	{
		/* full: the oldest is dropped unread */
		spans[tail].issued = false;
		tail = (tail + 1) % SPANS;
	}
	int h = head;
	head = (head + 1) % SPANS;
	spans[h].slot = slot;
	spans[h].open = true;
	spans[h].issued = false;
	query_counter(queries[h * 2], TIMESTAMP);
	return h;
}

void gputime_end(int h)
{
	if (h < 0 || !spans[h].open)
		return;
	query_counter(queries[h * 2 + 1], TIMESTAMP);
	spans[h].open = false;
	spans[h].issued = true;
}

void gputime_collect(void)
{
	if (state <= 0)
		return;
#if GL_IS_GLES
	GLint disjoint = 0;
	glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
#else
	const GLint disjoint = 0;
#endif
	while (tail != head)
	{
		Span *s = &spans[tail];
		if (s->open)
			break;
		if (s->issued)
		{
			GLuint avail = 0;
			glGetQueryObjectuiv(queries[tail * 2 + 1], GL_QUERY_RESULT_AVAILABLE, &avail);
			if (!avail)
				break;
			GLuint64 t0 = 0, t1 = 0;
			result64(queries[tail * 2], GL_QUERY_RESULT, &t0);
			result64(queries[tail * 2 + 1], GL_QUERY_RESULT, &t1);
			/* (a disjoint GPU — clock change, power state — makes the spans read now worthless) */
			if (!disjoint && t1 >= t0)
			{
				double ms = (double)(t1 - t0) / 1e6;
				perf_gpu_add(s->slot, ms);
				if (s->slot == PERF_GPU_HEADSET && prof_on)
					prof_vr_gpu((unsigned)(t1 - t0));
			}
			s->issued = false;
		}
		tail = (tail + 1) % SPANS;
	}
}
