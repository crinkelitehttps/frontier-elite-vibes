/*
 * gl_gputime.h - GPU time of parts of a picture, for perf_log.c (and the
 * frame profiler's headset column).
 *
 * Timestamp queries (GL 3.3, or GL_EXT_disjoint_timer_query on GLES), so
 * spans may nest: the shadow maps are timed inside the eyes. Results are
 * read a few pictures late, never waited for. Spans are best put at
 * framebuffer changes (tiled mobile GPUs). GLF_GPU_TIME=0 turns it off.
 */
#ifndef GL_GPUTIME_H
#define GL_GPUTIME_H

#ifdef __cplusplus
extern "C"
{
#endif

/* slot: PERF_GPU_* (perf_log.h); begin returns a handle for end, -1 when off */
int gputime_begin(int slot);
void gputime_end(int handle);
void gputime_collect(void); /* after each picture: the finished spans into perf_log */

#ifdef __cplusplus
}
#endif

#endif /* GL_GPUTIME_H */
