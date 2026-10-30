/*
 * gl_native.h - the native scene: the world drawn from the game's objects
 * and models directly, in true perspective for any camera, instead of from
 * what the game's own 3D engine draws for its one view (the cockpit's
 * passes, gl_cockpit.h).
 *
 * Work in progress. So far:
 *   - every object the game draws is captured with its pose (Nu_NativeObject,
 *     a hook at L393de in L390f2, where the object's view position and its
 *     model -> view matrix are ready), turned into the game's own camera's
 *     view space when it comes from a turned pass and kept once per object;
 *     objects the game culls as off screen are captured too (Lnative_clip,
 *     Nu_NativeCulled), so the scene is whole for any camera;
 *   - its model is run natively, like the game's model interpreter: the
 *     bytecode (decoded by the ship builder's tools/fe2ShipBuilder/fe2core.cpp,
 *     from the game's own memory) with its conditions on detail and on the
 *     object's variables, calc ops, sub-models, colours and the game's
 *     lighting; primitives go into the nodes of the game's depth tree;
 *   - GLF_NATIVE_SCENE=2 draws the objects filled over the game's picture:
 *     within an object in the game's depth-tree order (its models are made
 *     for it), between objects with a depth buffer;
 *   - GLF_NATIVE_SCENE=1 draws them as a wireframe instead, a colour per
 *     object, to check against the game's picture.
 *
 * Not done yet: mrotate/msetup/mcopy (sub-models turned by their own
 * matrix, e.g. turrets), vertices moved by variables (landing gear),
 * ztree sorting within a batch.
 *
 * Placement (the game's L39722 / L3988c): view = pos + R * v * 2^s, v the
 * model's vertex (its signed bytes), R the model -> view matrix (1.15 fixed
 * point words at A6-36), pos the view position (longs at A6-16), s the scale
 * (word at A6-208). The game's view space is x right, y up, z ahead; GL's
 * view space has z flipped.
 */
#ifndef GL_NATIVE_H
#define GL_NATIVE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Host call (Lnative_object in fe2.s): an object is about to be drawn */
void Nu_NativeObject(void);
/* Host call (Lnative_clip in fe2.s): the game culls an object as off
 * screen, d0 = 1 to capture it all the same */
void Nu_NativeCulled(void);
/* Host call (Lnative_clip): d0 = 1 if the object is a body too far for its
 * model (a sun, a far planet) that the native scene draws instead of the
 * game's disc */
void Nu_NativeFar(void);
/* Host call (Lnative_text): a model text's string, formatted */
void Nu_NativeText(void);

/* Game thread, in L390f2's frame a6: how much the game shrank the object
 * (l3924a). A far object's view position and scale s are both cut by 2^e
 * so its position fits, which keeps its look but not its distance: its
 * true distance is this times the one at A6-16. */
float native_frame_shrink(uint32_t a6);

/* With the scene (gl_scene.c): a new frame, the frame is done (game
 * thread), the latest frame is taken for drawing (render thread).
 * `screen`: a frame of the game's other screens shown on the cockpit's
 * screen (gl_cockpit.h), kept apart from the world: the last cockpit
 * frame's, or the one captured under the screen. */
void native_reset(void);
/* (`screen`: not the flight frame, so not what the capture passes look at) */
void native_publish(bool screen);
/* (game thread) the world captured under a screen (scene_publish_world);
 * `world`: one was published since the last acquire */
void native_publish_world(void);
void native_acquire(bool screen, bool world);
/* Render thread: draw that screen frame (true), without the world's
 * planets and haze, or the world (false, the default) */
void native_use_screen(bool screen);
/* The same for the page beside the screen's frame (scene_publish_page) */
void native_publish_page(void);
void native_acquire_page(void);
void native_use_page(bool page);

/* Render thread, after the scene, with the 3D view's viewport and its
 * un-turned projection set */
void native_draw(void);
/* The same, before the scene: the sky and the far bodies
 * (GLF_NATIVE_SCENE=2) */
void native_draw_sky(void);

/* Game thread, the cockpit choosing its passes (GLF_NATIVE_SCENE=2): the
 * directions (game view, z ahead) of planets the game's own view did not
 * draw last frame, at their horizon. The game is to draw a pass that way
 * too (a capture pass, not shown), so the planet's routine gives the
 * planet's sphere and its atmosphere. Returns how many, up to `max` and a
 * few a frame: the ones never seen first, else the planets in turn. They
 * are the planets of the last flight frame published, whatever else (a
 * MAP page) was published since. */
int native_capture_dirs(float dirs[][3], int max);

/* Game thread, after the game's 3D view this frame (its HUD): where the
 * object at `addr` is, GL view space of the game's own camera (x right, y
 * up, -z ahead; view units, the true distance is |pos| * *shrink). False if
 * this frame did not capture it. */
bool native_object_pos(uint32_t addr, float pos[3], float *shrink);

/* The native scene is on (GLF_NATIVE_SCENE unset or non-zero) */
bool native_enabled(void);
/* Its value: 1 wireframe, 2 filled (the default), 0 off */
int native_mode(void);

/* Extra detail on top of the game's Shape detail option, in factors of 2
 * (glfrontier.cfg native_detail_boost, 0 = the game's own distances): a
 * model's `if z` tests drop parts this many times twice as far away
 * (docs/level-of-detail.md section 4) */
extern int native_detail_boost;

#ifdef __cplusplus
}
#endif

#endif /* GL_NATIVE_H */
