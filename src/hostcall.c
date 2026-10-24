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
#include <math.h>
#include <SDL.h>
#include <string.h>

#include "audio.h"
#include "freecam.h"
#include "gl/gl_cockpit.h"
#include "gl/gl_hud.h"
#include "gl/gl_native.h"
#include "host.h"
#include "game_state.h"
#include "hostcall.h"
#include "input.h"
#include "keymap.h"
#include "main.h"
#include "mods.h"
#include "netplay.h"
#include "renderer.h"
#include "screen_text.h"
#include "shortcut.h"
#include "script.h"
#include "station_resume.h"
#include "strafe.h"

/* D0.b = exception number, A0 = handler */
static void SetExceptionHandler(void)
{
	exception_handlers[GetReg(REG_D0) & 31] = GetReg(REG_A0);
}

/* Called regularly by the game: shortcuts and SDL events */
/* Watches the player from the host's per-frame call, which keeps running
   after the player physics (Call_FlightLog) stops, e.g. in a death: logs
   "WATCH" when the mode, flags or player object change, and every 50 calls
   while any of them changed in the last 200. */
static void watch_player(void)
{
	static unsigned int calls, hot;
	static int last[8];
	uint32_t a6 = GAME_A6;
	if (!a6)
		return;
	uint32_t a0 = rdlong(a6 + 674);
	int now[8] = {rdbyte(a6 + 704), rdbyte(a6 + 705), rdbyte(a6 + 673), (int)a0, a0 ? rdbyte(a0 + 68) : -1, a0 ? rdbyte(a0 + 69) : -1,
		0, a0 ? (int)rdword(a0 + 116) : -1};
	bool changed = memcmp(now, last, sizeof now) != 0;
	calls++;
	if (changed)
		hot = calls + 200;
	if (changed || (calls < hot && calls % 50 == 0))
	{
		log_printf("WATCH %s mode=%02x/%02x b673=%02x obj=%x par=%d fr=%d b19=%02x gear=%x alt=%d spd=%d dt=%d v=%d,%d,%d phi=%d,%d,%d\n", changed ? "change" : "tick",
			now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7], (int)rdlong(a6 + 692), (int)rdlong(a6 + 688), (int)rdlong(a6 + 14446),
			a0 ? (int)rdlong(a0 + 98) : 0, a0 ? (int)rdlong(a0 + 102) : 0, a0 ? (int)rdlong(a0 + 106) : 0, a0 ? (int)rdlong(a0 + 56) : 0,
			a0 ? (int)rdlong(a0 + 60) : 0, a0 ? (int)rdlong(a0 + 64) : 0);
	}
	memcpy(last, now, sizeof now);
}

static void Call_HostUpdate(void)
{
	watch_player();
	Keymap_DebounceAllKeys();
	ShortCut_CheckKeys();
	Main_EventHandler();
	freecam_update();
	station_resume_poll();
	script_poll();
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
	int n, alt, spd, speed_set, dt, dt2, v[3], rate[3], thrust[3], plo[3], phi[3], e;
	int near_slot, near_model, near_frame;
	double near_d; /* nearest other object with the same parent, position units */
	double acc, k; /* |dv| per unit dt against the last frame, and acc * r^2 (r in position units): constant under 1/r^2 gravity */
	unsigned char status, parent, frame, f14, f15, f20, b673, b598, eng;
	unsigned short gear;
	unsigned int target;
} FltFrame;

static FltFrame flt_ring[FLT_RING];
static int flt_n, flt_after;

/* The position exponent L43ed6 (gravity) normalises by, as it computes it:
   17 + bits of the folded high parts, or the low-part shift if they are all 0 */
static int flt_exp(const int *lo, const int *hi)
{
	uint32_t d6 = 0, d7 = 0;
	for (int i = 0; i < 3; i++)
	{
		uint32_t l = (uint32_t)lo[i], h = (uint32_t)hi[i];
		if ((int32_t)h < 0)
			h = ~h, l = ~l;
		d6 |= l;
		d7 |= h;
	}
	d7 = (d7 << 1) | (d6 >> 31);
	d6 <<= 1;
	int e = 0;
	if (d7 == 0)
	{
		for (; d6 >= 0x4000; d6 >>= 1)
			e++;
		return e;
	}
	for (; d7; d7 >>= 1)
		e++;
	return 17 + e;
}

static void flt_print(const char *tag, const FltFrame *f)
{
	log_printf("FLT %s n=%d alt=%d spd=%d set=%d dt=%d/%d eng=%02x v=%d,%d,%d thr=%d,%d,%d rate=%d,%d,%d st=%02x par=%d fr=%d "
			   "flags=%02x/%02x/%02x b673=%02x b598=%02x gear=%04x tgt=%x phi=%d,%d,%d plo=%08x,%08x,%08x e=%d acc=%.4g k=%.4g near=%d/m%d/fr%d/d%.4f\n",
		tag, f->n, f->alt, f->spd, f->speed_set, f->dt, f->dt2, f->eng, f->v[0], f->v[1], f->v[2], f->thrust[0], f->thrust[1], f->thrust[2],
		f->rate[0], f->rate[1], f->rate[2], f->status, f->parent, f->frame, f->f14, f->f15, f->f20, f->b673,
		f->b598, f->gear, f->target, f->phi[0], f->phi[1], f->phi[2], (unsigned)f->plo[0], (unsigned)f->plo[1],
		(unsigned)f->plo[2], f->e, f->acc, f->k, f->near_slot, f->near_model, f->near_frame, f->near_d);
}

void Call_FlightLog(void)
{
	uint32_t a6 = (uint32_t)GetReg(REG_A6), a0 = (uint32_t)GetReg(REG_A0);
	substepped = false; /* a frame's own: set again by Call_SubSteps */
	{
		/* the player's mode and object, seen from any object's call: a death
		   replaces the player object, after which no frame below is logged */
		static int last_mode = -1, last_obj, quiet;
		int mode = rdbyte(a6 + 704);
		uint32_t obj = rdlong(a6 + 674);
		if (last_mode >= 0 && (mode != last_mode || obj != (uint32_t)last_obj))
		{
			log_printf("FLT ---- player mode %02x -> %02x, object %x -> %x (b673=%02x, alt=%d, speed=%d), the last frames:\n", last_mode, mode, last_obj, obj, rdbyte(a6 + 673),
				(int)rdlong(a6 + 692), (int)rdlong(a6 + 688));
			for (int i = 11; i >= 0; i--)
				if (flt_n - 1 - i >= 0)
					flt_print("pre", &flt_ring[(flt_n - 1 - i) % FLT_RING]);
			quiet = 40;
		}
		else if (quiet > 0 && a0 == obj && --quiet >= 0)
			log_printf("FLT after mode=%02x b673=%02x alt=%d speed=%d dt=%d\n", mode, rdbyte(a6 + 673), (int)rdlong(a6 + 692), (int)rdlong(a6 + 688), (int)rdlong(a6 + 14446));
		last_mode = mode;
		last_obj = (int)obj;
	}
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
		f.plo[i] = (int)rdlong(a0 + 44 + 4 * i);
		f.phi[i] = (int)rdlong(a0 + 56 + 4 * i);
	}
	f.e = flt_exp(f.plo, f.phi);
	f.near_slot = -1;
	f.near_d = 1e9;
	for (int i = 0; i < 0x73; i++)
	{
		uint32_t o = a0 - (uint32_t)(int)(int16_t)rdword(FE2_obj_offsets + 2 * (uint32_t)rdbyte(a0 + 93)) + (uint32_t)(int)(int16_t)rdword(FE2_obj_offsets + 2 * (uint32_t)i);
		if (o == a0 || o == (uint32_t)rdlong(a6 + 16024) || rdbyte(o + 68) != f.parent || !rdword(o + 90))
			continue;
		double d2 = 0;
		for (int j = 0; j < 3; j++)
		{
			double c = (double)(int)(rdlong(a0 + 56 + 4 * j) - rdlong(o + 56 + 4 * j)) + ((double)(uint32_t)rdlong(a0 + 44 + 4 * j) - (double)(uint32_t)rdlong(o + 44 + 4 * j)) / 4294967296.0;
			d2 += c * c;
		}
		if (d2 < f.near_d * f.near_d)
			f.near_d = sqrt(d2), f.near_slot = i, f.near_model = rdword(o + 90), f.near_frame = rdbyte(o + 69);
	}
	const FltFrame *p = &flt_ring[(f.n + FLT_RING - 1) % FLT_RING];
	double dv[3] = {f.v[0] - p->v[0], f.v[1] - p->v[1], f.v[2] - p->v[2]};
	if (f.n > 0 && f.dt > 0)
		f.acc = sqrt(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]) / f.dt;
	double r2 = 0;
	for (int i = 0; i < 3; i++)
	{
		double c = f.phi[i] + (double)(uint32_t)f.plo[i] / 4294967296.0;
		r2 += c * c;
	}
	f.k = f.acc * r2;
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
	else if (f.n > 1 && f.frame != p->frame)
	{
		log_printf("FLT ---- frame byte %d -> %d (alt %d -> %d), the last frames:\n", p->frame, f.frame, p->alt, f.alt);
		for (int i = 8; i >= 0; i--)
			if (f.n - i >= 0)
				flt_print("pre", &flt_ring[(f.n - i) % FLT_RING]);
		flt_after = FLT_AFTER;
	}
	else if (f.n > 1 && f.dt >= 100000 && f.dt == p->dt && f.parent == p->parent && f.frame == p->frame && p->k > 0 && f.k > 0
		&& (f.k > p->k * 1.25 || f.k < p->k / 1.25) && !f.thrust[0] && !f.thrust[1] && !f.thrust[2])
	{
		log_printf("FLT ---- gravity step: k %.4g -> %.4g (e %d -> %d), the last frames:\n", p->k, f.k, p->e, f.e);
		for (int i = 8; i >= 0; i--)
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

/* L3f566's squared distance d0 reached 2^30 and is clamped (see Lprox_log):
   the first 30 times for the player are logged */
void Call_ProxLog(void)
{
	uint32_t a6 = GAME_A6, a0 = (uint32_t)GetReg(REG_A0);
	if (a0 != rdlong(a6 + 674))
		return;
	double rr = 0;
	for (int i = 0; i < 3; i++)
	{
		double c = (double)(int)rdlong(a0 + 56 + 4 * i) + (double)(uint32_t)rdlong(a0 + 44 + 4 * i) / 4294967296.0;
		rr += c * c;
	}
	static int logged;
	if (logged++ >= 30)
		return;
	uint32_t a1 = (uint32_t)GetReg(REG_A1);
	log_printf("PROX r=%.5f fr=%d d0=%08x d5=%08x  sp=%08x,%08x,%08x,%08x a1=%x 266=%x 270=%x 272=%x ship266=%x 272=%x\n", sqrt(rr), rdbyte(a0 + 69), (unsigned)GetReg(0),
		(unsigned)GetReg(5), (unsigned)rdlong(GetReg(REG_A7)), (unsigned)rdlong(GetReg(REG_A7) + 4), (unsigned)rdlong(GetReg(REG_A7) + 8),
		(unsigned)rdlong(GetReg(REG_A7) + 12), a1, (unsigned)rdlong(a1 + 266), (unsigned)rdword(a1 + 270), (unsigned)rdlong(a1 + 272),
		(unsigned)rdlong(a0 + 266), (unsigned)rdlong(a0 + 272));
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
	[0x93] = Call_ProxLog,
	[0x94] = Call_NetDamage,
	[0x95] = Call_NetPending,
};
const int hcalls_count = sizeof(hcalls) / sizeof(hcalls[0]);
