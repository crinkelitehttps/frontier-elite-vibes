/*
 * vr.h - VR headsets through OpenXR (any runtime: SteamVR, Oculus, WMR,
 * Monado, ...).
 *
 * HOW IT WORKS
 *   With a session running, every frame the game draws (Nu_DrawScreen)
 *   goes to the headset as well as the window:
 *     - in the cockpit, the cockpit is drawn once per eye with the
 *       headset's pose and lens (gl_cockpit.c), into the runtime's
 *       swapchains, and submitted as a projection layer;
 *     - everything else (menus, map, external views, cockpit off) is the
 *       classic 4:3 picture on a virtual screen floating ahead (a quad
 *       layer).
 *   The window keeps showing the ordinary picture, with the cockpit
 *   turned by the headset, so the mouse works on it as before.
 *
 *   Motion controllers are hands that touch the control panel and the
 *   virtual screen (gl_hands.cpp), or laser pointers (gl_laser.c): where
 *   one touches or points is the game's mouse pointer. Their sticks and
 *   buttons are a gamepad (gamepad.c).
 *
 *   Linux: the runtime shares the GLX context (OpenGL swapchains).
 *   Windows: Direct3D 11 swapchains, which every Windows runtime supports;
 *   GL renders into a texture shared with D3D (WGL_NV_DX_interop2).
 *
 * Per frame:
 *   vr_update();                    // starts / stops VR, runtime events
 *   if (vr_running()) {
 *       if (vr_begin_frame()) {     // waits for the runtime's frame timing
 *           each eye: draw into vr_acquire_eye(i), vr_release_eye(i)
 *           or: draw into vr_acquire_screen(), vr_release_screen()
 *               and maybe over it: vr_acquire_overlay_eye(i), ...
 *       }
 *       vr_end_frame();             // after every vr_begin_frame()
 *   }
 */
#ifndef VR_H
#define VR_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* An eye's view this frame, in the play space (metres; x right, y up, z
 * back, centred on the seat's eye point). */
typedef struct
{
	float rot[3][3]; /* the eye's axes in play space: rot * v (row major) */
	float pos[3];    /* metres */
	float tan_left, tan_right, tan_down, tan_up; /* frustum edges at distance 1 (left / down negative) */
} VrView;

/* Settings */
extern bool vr_enabled;        /* use a headset (glfrontier.cfg "vr", --vr) */
extern int vr_resolution;      /* percent of the runtime's recommended eye size */

/* Once per frame with the GL context current: starts or stops VR to match
 * vr_enabled, handles the runtime's events */
void vr_update(void);
/* Ends VR (before the GL context goes) */
void vr_shutdown(void);
/* A session is running: frames go to the headset */
bool vr_running(void);
/* One line on what VR is doing, for the menu */
const char *vr_status(void);

/* Waits for the next headset frame and locates the eyes. False if nothing
 * needs drawing this time (vr_end_frame must still be called). */
bool vr_begin_frame(void);
/* The eyes were located this frame */
bool vr_tracking(void);
void vr_view(int eye, VrView *out); /* 0 left, 1 right */
/* Between the eyes, looking where the head looks */
void vr_centre_view(VrView *out);
void vr_eye_size(int eye, int *w, int *h);
/* Framebuffer (colour + stencil) to draw eye i into */
unsigned vr_acquire_eye(int eye);
void vr_release_eye(int eye);
/* The virtual screen: a framebuffer of VR_SCREEN_W x VR_SCREEN_H */
#define VR_SCREEN_W 1280
#define VR_SCREEN_H 960
/* where it floats in the play space: its centre `dist` ahead and `drop`
 * low, `width` wide (m); nearer with hands (gl_laser.h), the same size to
 * the eye */
void vr_screen_place(float *dist, float *width, float *drop);
unsigned vr_acquire_screen(void);
void vr_release_screen(void);
/* Submits what was drawn this frame (the eyes or the screen) */
void vr_end_frame(void);

/* A motion controller as vr_begin_frame found it, in the play space */
typedef struct
{
	bool connected; /* on, and the session has the input focus: its controls */
	bool active;    /* and tracked: its aim */
	float pos[3];   /* where its aim ray starts, metres */
	float dir[3];   /* the way it points (unit) */
	float up[3];    /* the back of the hand (unit, the aim pose's +y) */
	float trigger;  /* 0..1 */
	bool squeeze;   /* the grip button */
	float stick[2]; /* thumbstick, -1..1, x right +, y up + */
	bool stick_click;
	float pad[2]; /* trackpad (WMR, Vive), as the stick */
	bool pad_click;
	bool menu;
	bool lower, upper; /* face buttons (Touch, Index): A / X, B / Y */
} VrHand;
/* 0 left, 1 right. False (and out->connected false) if it isn't there. */
bool vr_controller(int hand, VrHand *out);
/* Buzzes a controller */
void vr_buzz(int hand, float seconds, float amplitude);
/* Draws over the virtual screen this frame, from each eye (call between
 * vr_acquire_screen and vr_end_frame): a framebuffer cleared to nothing,
 * whose drawn pixels (alpha 1) cover the screen */
unsigned vr_acquire_overlay_eye(int eye);
void vr_release_overlay_eye(int eye);

/* Makes the current head position and heading the seat's */
void vr_recenter(void);

#ifdef __cplusplus
}
#endif

#endif /* VR_H */
