/*
 * station_resume.h - the station's services screen (bulletin board,
 * shipyard, stockmarket, police...) kept as it was while other screens are
 * up. The game resets it to its top page whenever it is shown again, so
 * looking something up on a map meant finding the ad again after.
 *
 * Only with the cockpit's screens on (cockpit_screens_on), where the COMS
 * screen is meant to stay where the player left it. Pressing the station
 * key (F4) while the station screen is up still goes to the top page.
 *
 * Asm hooks Lscreen_leave / Lstation_enter (end of fe2/fe2*.s).
 */
#ifndef STATION_RESUME_H
#define STATION_RESUME_H

#ifdef __cplusplus
extern "C" {
#endif

/* hcall Call_ScreenLeave: the screen changes owner (d0 new, d1 old), just
 * before the old owner's fn 36 tears its screen down */
void Call_ScreenLeave(void);
/* hcall Call_StationResume: d0 = 0 asks whether to put the station screen
 * back (d0 = 1 back if so); d0 = 1 then writes the kept state back */
void Call_StationResume(void);
/* Each vblank: forget what was kept once the player is no longer docked */
void station_resume_poll(void);
/* A saved game was loaded (or any file read): forget what was kept */
void station_resume_forget(void);

#ifdef __cplusplus
}
#endif

#endif
