/*
 * renderer.h - interface between the emulator and the active renderer.
 *
 * Implemented by the OpenGL renderer in src/gl (GL 3.3, or GLES 3 on
 * Android and the web). It can also show the game's original software
 * rendered screen (Ctrl-E).
 * Originally based on Hatari's screen.h (GPL v2 or later).
 */
#ifndef RENDERER_H
#define RENDERER_H

#include <stdbool.h>

#include "main.h"
#include "screen_text.h"

/* ---- emulated screen ---------------------------------------------------- */
extern unsigned long VideoBase;    /* screen base address in ST RAM */
extern unsigned char *VideoRaster; /* host pointer to the 320x200 8-bit screen */

/* ST palettes (rgb444 words) and the 32-bit versions built each frame */
extern int len_main_palette; /* grows as dynamic colours are allocated */
extern unsigned short MainPalette[256];
extern unsigned short CtrlPalette[16];
extern unsigned int MainRGBPalette[256];
extern unsigned int CtrlRGBPalette[16];
extern int fe2_bgcol; /* palette index of the 3D view background */

/* Locations in ST RAM holding the screen buffer addresses. Use the macros
 * below to get host pointers. */
extern unsigned long logscreen, logscreen2, physcreen, physcreen2;
#define LOGSCREEN  (STRam + STMemory_ReadLong(logscreen))
#define LOGSCREEN2 (STRam + STMemory_ReadLong(logscreen2))
#define PHYSCREEN  (STRam + STMemory_ReadLong(physcreen))
#define PHYSCREEN2 (STRam + STMemory_ReadLong(physcreen2))

/* ---- window / renderer state ------------------------------------------ */
enum RENDERERS
{
	R_OLD,    /* the game's own software renderer */
	R_GLWIRE, /* GL wireframe */
	R_GL,     /* GL filled */
	R_MAX
};
extern enum RENDERERS use_renderer;

extern BOOL bGrabMouse;
extern BOOL bInFullScreen;
/* drawable size in real pixels. On high DPI screens (browsers with
   devicePixelRatio != 1, Retina) this is bigger than the window size SDL
   gives mouse events in: see Screen_MouseToPixels */
extern int screen_w;
extern int screen_h;
extern int mouse_shown; /* set by the game when the pointer should show */

void Screen_Init(void);
void Screen_UnInit(void);
/* Stops the render thread, if any (see "Frames" in gl_screen.c); before
 * SDL_Quit when quitting in a hurry */
void Screen_StopRenderThread(void);
/* Draw on a thread of its own (default where there are threads) */
extern bool render_thread_enabled;
void Screen_ToggleFullScreen(void);
void Screen_SetLetterboxMode(bool keep_aspect);
bool Screen_GetLetterboxMode(void);
void Screen_ToggleRenderer(void);
/* Human readable description of the renderer, for the debug menu */
void Screen_GetRendererInfo(char *buf, int len);

/* The 4:3 game area inside the window (SDL coordinates, y down) */
void call_update_letterbox(void);
/* re-reads the window's drawable size; relayouts if it changed */
void Screen_SyncSize(void);
/* scales an SDL mouse event's window coordinates to drawable pixels */
union SDL_Event;
void Screen_MouseToPixels(union SDL_Event *event);
int Screen_GetGameOffsetX(void);
int Screen_GetGameOffsetY(void);
int Screen_GetGameWidth(void);
/* Window pixel -> emulated 320x200 screen pixel, through the cockpit when
 * it is on; false if it points at nothing */
bool Screen_WindowToGame(int wx, int wy, int *gx, int *gy);
int Screen_GetGameHeight(void);

/* ---- host calls from the 68k code (see hostcall.c) ---------------------- */
void Nu_PutComplexStart(void);
void Nu_PutTriangle(void);
void Nu_PutQuad(void);
void Nu_PutLine(void);
void Nu_PutPoint(void);
void Nu_PutTwinklyCircle(void);
void Nu_PutCircle(void);
void Nu_PutColoredPoint(void);
void Nu_PutBezierLine(void);
void Nu_ComplexStart(void);
void Nu_ComplexSNext(void);
void Nu_ComplexSBegin(void);
void Nu_ComplexEnd(void);
void Nu_3DViewInit(void);
void Nu_InsertZNode(void);
void Nu_ComplexStartInner(void);
void Nu_ComplexBezier(void);
void Nu_DrawScreen(void);
void Screen_RefreshVr(void); /* a frame for the headset while the game draws nothing */
void Nu_PutTeardrop(void);
void Nu_PutOval(void);
void Nu_IsGLRenderer(void);
void Nu_GLClearArea(void);
void Nu_PutCylinder(void);
void Nu_PutBlob(void);
void Nu_PutPlanet(void);
void Nu_Put2DLine(void);
void Nu_AtmosBand(void);
void Nu_ComplexNearCurve(void);

#endif /* RENDERER_H */
