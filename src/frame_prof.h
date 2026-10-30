/*
 * frame_prof.h - where the time of every game frame goes (GLF_FRAME_PROF=1).
 *
 * A game frame runs from one Nu_DrawScreen to the next. Everything the game
 * thread does in it is timed: each host call by number (hostcall.c's
 * wrappers), the parts of the host update and of Nu_DrawScreen (marked with
 * PROF_BEGIN / PROF_END), and what is left of the frame is the 68k
 * emulation itself. The sound mixing (audio thread) and the picture drawing
 * (render thread) are summed on the side for the same stretch of time.
 *
 * Output: frameprof.csv in the working directory, one row per game frame
 * (milliseconds), and a summary in the log every two seconds with the
 * slowest frame and the costliest host calls. GLF_FRAME_PROF_SLOW=ms (default
 * 40) logs the breakdown of each frame longer than that.
 */
#ifndef FRAME_PROF_H
#define FRAME_PROF_H

#include <stdbool.h>
#include <stdint.h>

/* game thread: parts inside the host update and Nu_DrawScreen */
enum
{
	PS_VR_REFRESH, /* Screen_RefreshVr: the headset's frame when the game doesn't draw */
	PS_KEYS,       /* key debounce, shortcuts */
	PS_EVENTS,     /* SDL events: menu, gamepad, mouse, keyboard */
	PS_UPDATES,    /* freecam, station resume, script, time guard, watch */
	PS_NETPLAY,    /* Nu_DrawScreen: the other player's ghost */
	PS_TEXT,       /* Nu_DrawScreen: queued text into the screen */
	PS_PUBLISH,    /* Nu_DrawScreen: the frame handed to the drawing */
	PS_PACE,       /* the cockpit screen's frame limiter (sleeps) */
	PS_PUB_LOCK,   /*   publish: waiting for the frame lock (the drawing holds it while taking a frame) */
	PS_PUB_SCENE,  /*   publish: the scene copied (scene_publish*) */
	PS_PUB_HUD,    /*   publish: the HUD copied */
	PS_PUB_REST,   /*   publish: raster copy, map page, camera capture, the rest */
	PS_PUB_PAGE,   /*   publish: the MAP/info page made on the side (mapscreen_page) */
	PS_PUB_WORLD,  /*   publish: the world captured under a cockpit screen */
	PS_PUB_CAMERA, /*   publish: the camera screen's picture */
	PS_HANDOFF,    /* the GL context between the threads (may wait for the render thread) */
	PS_INLINE,     /* the picture drawn on the game thread (menu open, no render thread) */
	PS_COUNT
};

extern volatile bool prof_on;

void prof_init(void);
uint64_t prof_now(void);

/* hostcall.c's wrappers */
uint64_t prof_call_begin(void);
bool prof_call_end(int n, uint64_t t0); /* true: the frame ended (Nu_DrawScreen) */
void prof_frame_end(int mode);

/* game thread parts */
void prof_part(int slot, uint64_t t0);

/* other threads: summed until the next row */
void prof_audio_add(uint64_t t0);
void prof_render_add(uint64_t t0, uint64_t swap0, uint64_t swap1);
void prof_render_acquire(uint64_t t0); /* the drawing taking a frame, lock wait included */
/* a headset's picture: xrWaitFrame (paces to the display), the eyes drawn
 * (CPU time to issue it), xrEndFrame, all in ticks */
void prof_vr_add(uint64_t wait, uint64_t eyes, uint64_t end);
void prof_vr_gpu(unsigned ns); /* what the GPU took for the eyes of an earlier picture */

#define PROF_BEGIN() (prof_on ? prof_now() : 0)
#define PROF_END(slot, t0) \
	do \
	{ \
		if (prof_on) \
			prof_part((slot), (t0)); \
	} while (0)

#endif /* FRAME_PROF_H */
