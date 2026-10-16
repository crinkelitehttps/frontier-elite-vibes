/*
 * hostcall.c - the table of host functions the translated 68k game code
 * calls (hcalls[n] in fe2_modded.s.c), plus the few core calls.
 *
 * The numbers are fixed by the game code, so entries must never move:
 *   0x00-0x27  system, screen (host_screen.c), audio, input, files
 *              (host_files.c)
 *   0x28-0x29  mods framework (mods.c)
 *   0x60-0x79  renderer hooks (renderer.h)
 */
#include <SDL.h>

#include "audio.h"
#include "freecam.h"
#include "gl/gl_cockpit.h"
#include "gl/gl_hud.h"
#include "gl/gl_native.h"
#include "host.h"
#include "hostcall.h"
#include "input.h"
#include "keymap.h"
#include "main.h"
#include "mods.h"
#include "renderer.h"
#include "screen_text.h"
#include "shortcut.h"
#include "station_resume.h"
#include "strafe.h"

/* D0.b = exception number, A0 = handler */
static void SetExceptionHandler(void)
{
	exception_handlers[GetReg(REG_D0) & 31] = GetReg(REG_A0);
}

/* Called regularly by the game: shortcuts and SDL events */
static void Call_HostUpdate(void)
{
	Keymap_DebounceAllKeys();
	ShortCut_CheckKeys();
	Main_EventHandler();
	freecam_update();
	station_resume_poll();
}

/* The game waiting for its next 20 ms tick: sleep rather than spin (a
 * spinning CPU takes power the GPU of a laptop's APU could use) */
static void Call_Idle(void)
{
	SDL_Delay(1);
}

/* The game used to warp the ST mouse pointer here; the host pointer is
 * left alone */
static void Call_NotifyMousePos(void) {}

void Call_DumpRegs(void)
{
	log_printf("D: $%x $%x $%x $%x $%x $%x $%x $%x\n", GetReg(0), GetReg(1), GetReg(2), GetReg(3), GetReg(4),
			   GetReg(5), GetReg(6), GetReg(7));
	log_printf("A: $%x $%x $%x $%x $%x $%x $%x $%x\n", GetReg(8), GetReg(9), GetReg(10), GetReg(11),
			   GetReg(12), GetReg(13), GetReg(14), GetReg(15));
}

void Call_DumpDebug(void)
{
#ifdef M68K_DEBUG
	log_printf("Debug info. PC @ 68k line %d.\n", line_no);
#endif
	Call_DumpRegs();
	u32 sp = GetReg(REG_A7);
	log_printf("Stack: $%x $%x $%x $%x $%x $%x $%x $%x\n", STMemory_ReadLong(sp + 4),
			   STMemory_ReadLong(sp + 8), STMemory_ReadLong(sp + 12), STMemory_ReadLong(sp + 16),
			   STMemory_ReadLong(sp + 20), STMemory_ReadLong(sp + 24), STMemory_ReadLong(sp + 28),
			   STMemory_ReadLong(sp + 32));
}

HOSTCALL hcalls[] = {
	[0x00] = SetExceptionHandler,
	[0x01] = Call_Memset,
	[0x02] = Call_MemsetBlue,
	[0x03] = Call_BlitCursor,
	[0x04] = Call_RestoreUnderCursor,
	[0x05] = Call_BlitBmp,
	[0x06] = Call_OldHLine,
	[0x07] = Call_HostUpdate,
	[0x08] = Call_Memcpy,
	[0x09] = Call_PutPix,
	[0x0a] = Call_BackHLine,
	[0x0b] = Call_FillLine,
	[0x0c] = Call_SetMainPalette,
	[0x0d] = Call_SetCtrlPalette,
	[0x0e] = Call_SetScreenBase,
	[0x10] = Call_DumpRegs,
	[0x11] = Call_MakeExtPalette,
	[0x12] = Call_PlaySFX,
	[0x13] = Call_GetMouseInput,
	[0x14] = Call_GetKeyboardEvent,
	[0x16] = Call_HLine,
	[0x18] = Call_NotifyMousePos,
	[0x19] = Call_InformScreens,
	[0x1b] = Call_DrawStrShadowed,
	[0x1c] = Call_DrawStr,
	[0x1d] = Call_PlayMusic,
	[0x1e] = Call_StopMusic,
	[0x1f] = Call_Idle,
#ifdef M68K_DEBUG
	[0x20] = Call_DumpDebug,
#endif
	[0x21] = Call_IsMusicPlaying,
	[0x22] = Call_Fread,
	[0x23] = Call_Fwrite,
	[0x24] = Call_Fdelete,
	[0x25] = Call_Fopendir,
	[0x26] = Call_Freaddir,
	[0x27] = Call_Fclosedir,
	[0x28] = Call_ModsSaveBegin, /* mods framework, see mods.c */
	[0x29] = Call_ModsSaveEnd,

	[0x60] = Nu_PutTriangle,
	[0x61] = Nu_PutQuad,
	[0x62] = Nu_PutLine,
	[0x63] = Nu_PutPoint,
	[0x64] = Nu_PutTwinklyCircle,
	[0x65] = Nu_PutColoredPoint,
	[0x66] = Nu_PutBezierLine,
	[0x67] = Nu_ComplexStart,
	[0x68] = Nu_ComplexSNext,
	[0x69] = Nu_ComplexSBegin,
	[0x6a] = Nu_ComplexEnd,
	[0x6b] = Nu_3DViewInit,
	[0x6c] = Nu_InsertZNode,
	[0x6d] = Nu_ComplexStartInner,
	[0x6e] = Nu_ComplexBezier,
	[0x6f] = Nu_DrawScreen,
	[0x70] = Nu_PutTeardrop,
	[0x71] = Nu_PutCircle,
	[0x72] = Nu_PutOval,
	[0x73] = Nu_IsGLRenderer,
	[0x74] = Nu_GLClearArea,
	[0x75] = Nu_QueueDrawStr,
	[0x76] = Nu_PutCylinder,
	[0x77] = Nu_PutBlob,
	[0x78] = Nu_PutPlanet,
	[0x79] = Nu_Put2DLine,
	[0x7a] = Call_CockpitPass,
	[0x7b] = Nu_AtmosBand,
	[0x7c] = Nu_ComplexNearCurve,
	[0x7d] = Call_CockpitBackground,
	[0x7e] = Call_Strafe,
	[0x7f] = Call_CockpitSky,
	[0x80] = Call_CockpitVertexD,
	[0x81] = Nu_NativeObject,
	[0x82] = Nu_NativeCulled,
	[0x83] = Nu_NativeFar,
	[0x84] = Nu_NativeText,
	[0x85] = Call_ScreenControls,
	[0x86] = Call_HudCross,
	[0x87] = Call_HudVelocity,
	[0x88] = Call_HudTarget,
	[0x89] = Call_HudLabels,
	[0x8a] = Call_ScreenLeave,
	[0x8b] = Call_StationResume,
};
const int hcalls_count = sizeof(hcalls) / sizeof(hcalls[0]);
