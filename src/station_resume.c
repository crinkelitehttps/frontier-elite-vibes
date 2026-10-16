/*
 * station_resume.c - see station_resume.h.
 *
 * The station module (L75b64_mod_station_services in fe2.s) is a menu of
 * items in A6 memory: the page kind (11732: 0 top, 2 an item's details such
 * as an open ad, 4 bulletin board, 6 stockmarket, 8/10/12/14/16 shipyard and
 * police pages), the item list (10602, 24 bytes each), its scroll (11722)
 * and count (11724), the face and message (11736-11790, 11686-11720) and the
 * message queue (11792-11905). Its fn 8 (L75f3c) clears all of that and
 * builds the top page. Kept here: that block as the screen is left, and the
 * clickable rectangles the page set (A6_ICON_POSITIONS). Put back, L75c92
 * rebuilds the page from it, re-reading live data for the lists (an ad that
 * expired meanwhile is gone, as if the player had stayed on the page).
 */
#include <stdbool.h>
#include <stdint.h>
#include "station_resume.h"
#include "game_state.h"
#include "host.h" /* rdbyte, rdword, rdlong, wrbyte, wrword */
#include "m68000.h"
#include "gl/gl_cockpit.h"

#define STATE_FLAGS  114   /* bytes 114, 115: page on ($ff), message bits */
#define STATE_START  10602 /* the item list */
#define STATE_END    11914 /* past the BB scroll (11906); 11914+ is the ship screen's */
#define FINES        11774 /* long: unpaid fines, game state, never put back */
#define STATION_OBJ  678   /* long: the station docked at */
#define ICON_MAX     64    /* A6_ICON_POSITIONS: 10 bytes each, $ffff ends it */

static struct
{
	bool held;
	uint32_t station;
	uint8_t flags[2];
	uint8_t state[STATE_END - STATE_START];
	int icons;
	uint8_t icon[ICON_MAX * 10];
} kept;

static bool resume; /* Call_StationResume said yes; the write comes next */

static bool docked(void)
{
	uint8_t s = (uint8_t)rdbyte(GAME_A6 + A6_FLIGHT_STATE);
	return s == 0x54 || s == 0x60; /* as L75f3c_fn8 tests */
}

void station_resume_forget(void)
{
	kept.held = false;
	resume = false;
}

void station_resume_poll(void)
{
	if (kept.held && (!docked() || game_screen() == SCREEN_TITLE))
		station_resume_forget();
}

void Call_ScreenLeave(void)
{
	uint32_t a6 = GAME_A6;
	if ((uint16_t)GetReg(REG_D1) != OWNER_STATION)
		return;
	if (!docked() || !rdbyte(a6 + STATE_FLAGS))
	{
		station_resume_forget();
		return;
	}
	kept.station = (uint32_t)rdlong(a6 + STATION_OBJ);
	kept.flags[0] = (uint8_t)rdbyte(a6 + STATE_FLAGS);
	kept.flags[1] = (uint8_t)rdbyte(a6 + STATE_FLAGS + 1);
	for (int i = 0; i < STATE_END - STATE_START; i++)
		kept.state[i] = (uint8_t)rdbyte(a6 + STATE_START + i);
	int n = 0;
	while (n < ICON_MAX && !(rdword(a6 + A6_ICON_POSITIONS + n * 10) & 0x8000))
		n++;
	for (int i = 0; i < n * 10; i++)
		kept.icon[i] = (uint8_t)rdbyte(a6 + A6_ICON_POSITIONS + i);
	kept.icons = n;
	kept.held = true;
}

static void put_back(void)
{
	uint32_t a6 = GAME_A6;
	wrbyte(a6 + STATE_FLAGS, kept.flags[0]);
	wrbyte(a6 + STATE_FLAGS + 1, kept.flags[1]);
	for (int i = 0; i < STATE_END - STATE_START; i++)
	{
		uint32_t at = STATE_START + i;
		if (at < FINES || at >= FINES + 4)
			wrbyte(a6 + at, kept.state[i]);
	}
	for (int i = 0; i < kept.icons * 10; i++)
		wrbyte(a6 + A6_ICON_POSITIONS + i, kept.icon[i]);
	wrword(a6 + A6_ICON_POSITIONS + kept.icons * 10, 0xffff);
}

void Call_StationResume(void)
{
	if (GetReg(REG_D0) == 0)
	{
		/* coming back from another screen (F4 on the station screen itself
		   still goes to the top page), to the same station, still docked */
		resume = kept.held && cockpit_screens_on() && game_screen_owner() != OWNER_STATION && docked() &&
				 (uint32_t)rdlong(GAME_A6 + STATION_OBJ) == kept.station;
		SetReg(REG_D0, resume);
		return;
	}
	if (resume)
		put_back();
	resume = false;
}
