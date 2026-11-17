/*
 * headlook.c - see headlook.h.
 */
#include "headlook.h"
#include "input.h"
#include "gl/gl_cockpit.h"
#include "main.h"

#define TAP_MS 300          /* a tap: released within this */
#define DOUBLE_MS 350       /* the second press within this of the tap */
#define MOUSE_DEG 0.15f     /* degrees per mouse pixel */
#define YAW_MAX 170.0f
#define PITCH_MAX 85.0f

static volatile float held_yaw, held_pitch;
static bool alt_held;
static HeadlookTap alt_tap;

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

void headlook_turn(float yaw, float pitch)
{
	held_yaw = clampf(held_yaw + yaw, -YAW_MAX, YAW_MAX);
	held_pitch = clampf(held_pitch + pitch, -PITCH_MAX, PITCH_MAX);
}

void headlook_center(void)
{
	held_yaw = held_pitch = 0.0f;
}

void headlook_get(float *yaw, float *pitch)
{
	*yaw = held_yaw;
	*pitch = held_pitch;
}

bool headlook_tap(HeadlookTap *t, bool down, Uint64 now_ms)
{
	if (down == t->down)
		return false;
	t->down = down;
	if (!down)
	{
		/* a short press is a tap; a long one (a drag) starts over */
		t->tapped = now_ms - t->down_at <= TAP_MS;
		t->tap_at = now_ms;
		return false;
	}
	t->down_at = now_ms;
	bool dbl = t->tapped && now_ms - t->tap_at <= DOUBLE_MS;
	t->tapped = false;
	return dbl;
}

static bool is_alt(const SDL_Event *e)
{
	SDL_Keycode k = e->key.keysym.sym;
	return k == SDLK_RALT || k == SDLK_MODE; /* AltGr on some layouts */
}

/* Right Alt lets go: the mouse is the game's again (captured while its
 * right button steers, input.c) */
static void release_alt(void)
{
	alt_held = false;
	SDL_SetRelativeMouseMode((input.cur_mousebut_state & 0x1) ? SDL_TRUE : SDL_FALSE);
}

bool headlook_event(const SDL_Event *event)
{
	switch (event->type)
	{
	case SDL_KEYDOWN:
		if (!is_alt(event))
			return false;
		if (event->key.repeat)
			return alt_held;
		if (!cockpit_active() || toggle_m68k_menu)
			return false;
		if (headlook_tap(&alt_tap, true, SDL_GetTicks64()))
			headlook_center();
		alt_held = true;
		SDL_SetRelativeMouseMode(SDL_TRUE);
		return true;
	case SDL_KEYUP:
		if (!is_alt(event) || !alt_held)
			return false;
		headlook_tap(&alt_tap, false, SDL_GetTicks64());
		release_alt();
		return true;
	case SDL_MOUSEMOTION:
		if (!alt_held)
			return false;
		headlook_turn(event->motion.xrel * MOUSE_DEG, -event->motion.yrel * MOUSE_DEG);
		return true;
	case SDL_WINDOWEVENT:
		/* the key's release may go to another window */
		if (alt_held && event->window.event == SDL_WINDOWEVENT_FOCUS_LOST)
			release_alt();
		return false;
	}
	return false;
}
