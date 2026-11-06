/*
 * rt_clock.h - the game's time by the host's clock.
 *
 * The game's vblank handler (every 20 ms, main.c vbl_callback) added the time step to three
 * accumulators that each game frame takes as its dt: game time 14450(a6) (+ the rate 14482(a6),
 * 994 at 1x), real time 14458(a6) and a timer 666(a6) (+994 each). So the world moved in whole
 * vblanks: a picture showed 0, 1 or 2 steps of motion, choppy turning and flying however fast the
 * pictures came (memory: choppy-motion-vblank).
 *
 * With realtime_clock on, the adds are done here instead, at each take (Lrt_take_* in fe2*.s), by
 * the time since the last take (rate * elapsed / 20 ms, remainders kept), so dt follows the
 * clock. Game frames are paced to the pictures (Lrt_limit): with the render thread drawing, the
 * game waits until the drawing has taken its last frame, so it makes about one frame a picture;
 * drawing inline (vsync) paces itself. A step is capped at 100 ms (a load, a stall).
 * Off: the vblank handler adds as before, and the frame limit waits for a vblank.
 *
 * Setting realtime_clock (glfrontier.cfg, Flight menu); GLF_RTCLOCK=0/1 overrides it.
 * GLF_RTPACE=US (experiment): no wait for the drawing, a game frame every US microseconds at most.
 */
#ifndef RT_CLOCK_H
#define RT_CLOCK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

extern bool realtime_clock; /* a setting */

void Call_RtVbl(void);	  /* hcall: the vblank handler's adds (only when off) */
void Call_RtCredit(void); /* hcall: credit the time since the last take */
void Call_RtPace(void);	  /* hcall: the frame limit (when on) */

#ifdef __cplusplus
}
#endif

#endif
