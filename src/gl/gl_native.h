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

/* With the scene (gl_scene.c): a new frame, the frame is done (game
 * thread), the latest frame is taken for drawing (render thread) */
void native_reset(void);
void native_publish(void);
void native_acquire(void);

/* Render thread, after the scene, with the 3D view's viewport and its
 * un-turned projection set */
void native_draw(void);
/* The same, before the scene: the sky and the far bodies
 * (GLF_NATIVE_SCENE=2) */
void native_draw_sky(void);

/* GLF_NATIVE_SCENE is set */
bool native_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* GL_NATIVE_H */
