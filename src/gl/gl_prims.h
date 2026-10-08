/*
 * gl_prims.h - helpers shared by primitive implementations.
 */
#ifndef GL_PRIMS_H
#define GL_PRIMS_H

#include <stdbool.h>
#include <stdint.h>

/* Frontier's lighting model: colour = ambient(object) + N.L * light colour */
typedef struct
{
	bool lit;         /* false: flat, self-coloured */
	float ambient[3]; /* object colour, 0..1 */
	float diffuse[3]; /* light colour, 0..1 */
	float light[3];   /* normalised light direction, eye space */
} GlMaterial;

/* Build a material from the emulator's rgb444 colour words. */
void gl_material_init(GlMaterial *m, const float light_dir[3], int light_col, int extra_col, int obj_col);
/* Shade an eye-space unit normal. */
void gl_material_shade(const GlMaterial *m, const float n_eye[3], float out_rgb[3]);

/* ST rgb444 colour word -> 0..1 floats (nibble / 16) */
void gl_rgb444_to_f(int rgb444, float out_rgb[3]);

/* Read a light source vector from emulated RAM (eye space). */
void gl_read_light_dir(uint32_t addr, float out[3]);

void gl_prims_init(void);
void gl_prims_shutdown(void);
/* Closes any complex polygon left open at the end of the scene. */
void gl_prims_finish(void);
/* A new scene is being recorded: forgets the tessellations kept from the
 * last one (gl_scene.c, scene_reset) */
void gl_prims_new_scene(void);

#endif /* GL_PRIMS_H */
