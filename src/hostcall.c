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

/* the ship turns itself belly-down to a planet below 40 km (the game's own
   behaviour); off lets the pilot fly a stable orbit */
bool orbit_assist = true;

/* the game moves a ship by velocity * (time >> 7), so the motion is up to 10%
   short at 1x and the orbit changes with the time acceleration step; on moves
   it by the exact time (see Lpos_dt in fe2.s) */
bool exact_step = true;

void Call_ExactStep(void)
{
	SetReg(REG_D0, exact_step);
}

/* A time step longer than this (10x) is cut in 2^s: the game's single
   gravity + move step a frame is too coarse for an orbit at 100x and more */
#define SUBSTEP_MAX 10000
#define SUBSTEP_SHIFT_MAX 7

int substep_shift(unsigned int dt)
{
	int s = 0;
	if (!exact_step)
		return 0;
	while (s < SUBSTEP_SHIFT_MAX && (dt >> s) > SUBSTEP_MAX)
		s++;
	return s;
}

static bool substepped;

void Call_SubSteps(void)
{
	int s = substep_shift((unsigned int)rdlong((uint32_t)GetReg(REG_A6) + 14446));
	substepped = s > 0;
	SetReg(REG_D0, s);
}

void Call_SubSkip(void)
{
	SetReg(REG_D0, substepped);
	substepped = false;
}

void Call_OrbitAssist(void)
{
	SetReg(REG_D0, orbit_assist);
}

/* Flight log, to find what kills the ship's speed near a planet: every
   physics frame of the player goes into a ring; when the speed falls to under
   half in a frame the ring is written to the log (lines "FLT"), then the
   next frames too. Call_KillLog notes each landing zeroing and its caller. */
#define FLT_RING 48
#define FLT_AFTER 12

typedef struct
{
	int n, alt, spd, speed_set, dt, dt2, v[3], rate[3], thrust[3];
	unsigned char status, parent, frame, f14, f15, f20, b673, b598, eng;
	unsigned short gear;
	unsigned int target;
} FltFrame;

static FltFrame flt_ring[FLT_RING];
static int flt_n, flt_after;

static void flt_print(const char *tag, const FltFrame *f)
{
	log_printf("FLT %s n=%d alt=%d spd=%d set=%d dt=%d/%d eng=%02x v=%d,%d,%d thr=%d,%d,%d rate=%d,%d,%d st=%02x par=%d fr=%d "
			   "flags=%02x/%02x/%02x b673=%02x b598=%02x gear=%04x tgt=%x\n",
		tag, f->n, f->alt, f->spd, f->speed_set, f->dt, f->dt2, f->eng, f->v[0], f->v[1], f->v[2], f->thrust[0], f->thrust[1], f->thrust[2],
		f->rate[0], f->rate[1], f->rate[2], f->status, f->parent, f->frame, f->f14, f->f15, f->f20, f->b673,
		f->b598, f->gear, f->target);
}

void Call_FlightLog(void)
{
	uint32_t a6 = (uint32_t)GetReg(REG_A6), a0 = (uint32_t)GetReg(REG_A0);
	substepped = false; /* a frame's own: set again by Call_SubSteps */
	if (a0 != rdlong(a6 + 674))
		return;
	FltFrame f = {.n = flt_n++,
		.alt = (int)rdlong(a6 + 692),
		.spd = (int)rdlong(a6 + 688),
		.speed_set = (int)rdlong(a6 + 848),
		.dt = (int)rdlong(a6 + 14446),
		.dt2 = (int)rdlong(a6 + 696),
		.eng = rdbyte(a6 + 705),
		.status = rdbyte(a6 + 704),
		.parent = rdbyte(a0 + 68),
		.frame = rdbyte(a0 + 69),
		.f14 = rdbyte(a6 + 16014),
		.f15 = rdbyte(a6 + 16015),
		.f20 = rdbyte(a6 + 16020),
		.b673 = rdbyte(a6 + 673),
		.b598 = rdbyte(a6 + 598),
		.gear = rdword(a0 + 116),
		.target = rdlong(a6 + 678)};
	for (int i = 0; i < 3; i++)
	{
		f.v[i] = (int)rdlong(a0 + 98 + 4 * i);
		f.rate[i] = (short)rdword(a0 + 134 + 2 * i);
		f.thrust[i] = (short)rdword(a0 + 140 + 2 * i);
	}
	const FltFrame *p = &flt_ring[(f.n + FLT_RING - 1) % FLT_RING];
	long long now = llabs(f.v[0]) + llabs(f.v[1]) + llabs(f.v[2]);
	long long was = llabs(p->v[0]) + llabs(p->v[1]) + llabs(p->v[2]);
	flt_ring[f.n % FLT_RING] = f;
	if (f.n > 0 && was > 300 && now * 20 < was * 17 || now * 17 > was * 20)
	{
		log_printf("FLT ---- speed jumped %lld -> %lld, the last frames:\n", was, now);
		for (int i = FLT_RING - 1; i >= 0; i--)
			if (f.n - i >= 0)
				flt_print("pre", &flt_ring[(f.n - i) % FLT_RING]);
		flt_after = FLT_AFTER;
	}
	else if (flt_after > 0)
	{
		flt_after--;
		flt_print("post", &f);
	}
	else if (f.n % 50 == 0)
		flt_print("tick", &f);
	else if (f.n > 0 && (f.status != p->status || f.frame != p->frame || f.parent != p->parent))
		flt_print("change", &f);
}

void Call_KillLog(void)
{
	uint32_t a7 = (uint32_t)GetReg(REG_A7);
	log_printf("FLT kill: velocity zeroed (L6281a), called from %x\n", rdlong(a7 + 4));
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
	[0x2a] = Call_GameStart,

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
	[0x8c] = Call_ScreenWorld,
	[0x8d] = Call_OrbitAssist,
	[0x8e] = Call_FlightLog,
	[0x8f] = Call_KillLog,
	[0x90] = Call_ExactStep,
	[0x91] = Call_SubSteps,
	[0x92] = Call_SubSkip,
};
const int hcalls_count = sizeof(hcalls) / sizeof(hcalls[0]);
