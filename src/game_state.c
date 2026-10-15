/*
 * game_state.c - see game_state.h.
 */
#include "game_state.h"
#include "host.h" /* rdbyte, rdword, rdlong, MEM_SIZE */

GameScreen game_screen(void)
{
	switch (game_screen_owner())
	{
	case OWNER_NONE:
		return SCREEN_TITLE;
	case OWNER_FLIGHT:
		return SCREEN_FLIGHT;
	case OWNER_GALAXY_MAP:
		return SCREEN_GALAXY_MAP;
	case OWNER_SYSTEM_MAP:
		return SCREEN_SYSTEM_MAP;
	case OWNER_OPTIONS:
		return SCREEN_OPTIONS;
	case OWNER_STATION:
		return SCREEN_STATION;
	case OWNER_SHIP_SCREEN:
		return SCREEN_SHIP;
	default:
		return SCREEN_OTHER;
	}
}

int game_screen_owner(void)
{
	return (u16)rdword(GAME_A6 + A6_SCREEN_OWNER);
}

FlightView game_flight_view(void)
{
	u8 v = (u8)rdbyte(GAME_A6 + A6_3DVIEW_MODE);
	return v < VIEW_COUNT ? (FlightView)v : VIEW_FRONT;
}

/* Records of five words (x0, y0, x1, y1, id) every 10 bytes, a negative
 * x0 ending the list (L422d0_UISetIconPositions) */
int game_icon_rects(int rects[][4], int max)
{
	int n = 0;
	for (u32 p = GAME_A6 + A6_ICON_POSITIONS; n < max && n < 64; p += 10, n++)
	{
		if (rdword(p) < 0)
			break;
		for (int i = 0; i < 4; i++)
			rects[n][i] = rdword(p + 2 * i);
	}
	return n;
}

FlightStatus game_flight_status(void)
{
	switch ((u8)rdbyte(GAME_A6 + A6_FLIGHT_STATE))
	{
	case 0x04:
		return FLIGHT_AUTOPILOT;
	case 0x48:
		return FLIGHT_TAKE_OFF;
	case 0x54:
		return FLIGHT_DOCKED;
	case 0x60:
		return FLIGHT_LANDED;
	case 0x68:
		return FLIGHT_LANDED_ROUGH;
	default:
		return FLIGHT_MANUAL;
	}
}

bool game_engines_off(void)
{
	return rdbyte(GAME_A6 + A6_FLIGHT_FLAGS) & 1;
}

unsigned int game_player_ship(void)
{
	u32 ship = (u32)rdlong(GAME_A6 + A6_PLAYER_SHIP);
	return (ship > 0 && ship + 512 < MEM_SIZE) ? ship : 0;
}

const char *game_screen_name(GameScreen s)
{
	static const char *const names[SCREEN_COUNT] = {"Title",   "Flight",  "Galaxy map", "System map",
													"Options", "Station", "Ship",       "Other"};
	return (s >= 0 && s < SCREEN_COUNT) ? names[s] : "?";
}

const char *game_view_name(FlightView v)
{
	static const char *const names[VIEW_COUNT] = {"Front", "Rear", "Top turret", "Bottom turret", "External"};
	return (v >= 0 && v < VIEW_COUNT) ? names[v] : "?";
}

const char *game_status_name(FlightStatus s)
{
	static const char *const names[FLIGHT_COUNT] = {"Manual control", "Autopilot", "Take-off",
													"Docked",         "Landed",    "Landed (rough)"};
	return (s >= 0 && s < FLIGHT_COUNT) ? names[s] : "?";
}

const char *game_build_tag(void)
{
#if !FE2_USE_MODDED
	return "ORIGINAL";
#else
	static char tag[32];
	unsigned int i;
	for (i = 0; i < sizeof(tag) - 1; i++)
	{
		char c = (char)rdbyte(FE2_Lbuildtag_marker + i);
		if (!c)
			break;
		tag[i] = c;
	}
	tag[i] = '\0';
	return tag;
#endif
}

