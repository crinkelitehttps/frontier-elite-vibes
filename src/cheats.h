/*
 * cheats.h - edits the player's money, cargo, ratings and ship in emulated
 * RAM for the cheat menu.
 *
 * Everything goes through the game's own variables and tables (found in
 * fe2/fe2_modded.s), so cargo space, equipment mass and the like stay consistent
 * with what the game itself would do.
 */
#ifndef CHEATS_H
#define CHEATS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Fitting equipment normally needs free cargo space for its mass; with
 * this set it goes in anyway (free space can then go negative) */
extern bool cheat_ignore_space;

/* ---- money, in tenths of a credit ---- */
long cheat_cash(void);
void cheat_set_cash(long tenths);

/* ---- cargo, in tonnes ---- */
#define CHEAT_COMMODITIES 31
const char *cheat_commodity_name(int i);
int cheat_cargo(int i);
int cheat_cargo_free(void);
void cheat_add_cargo(int i, int tonnes);

/* ---- ratings ---- */
#define CHEAT_ELITE_RANKS    10 /* Harmless .. ELITE */
#define CHEAT_MILITARY_RANKS 13 /* None .. Admiral, Outsider .. Prince */
extern const char *const cheat_elite_rank_names[CHEAT_ELITE_RANKS];
extern const char *const cheat_federal_rank_names[CHEAT_MILITARY_RANKS];
extern const char *const cheat_imperial_rank_names[CHEAT_MILITARY_RANKS];

int cheat_elite_rank(void);
void cheat_set_elite_rank(int rank);
int cheat_federal_rank(void);
void cheat_set_federal_rank(int rank);
int cheat_imperial_rank(void);
void cheat_set_imperial_rank(int rank);
/* Makes the player Clean with every authority */
void cheat_clear_criminal_record(void);

/* ---- the player's ship ---- */
bool cheat_have_ship(void);
void cheat_refuel(void);
/* Full hull, full shields, nothing damaged */
void cheat_repair(void);

/* ---- location, within the current system ----
 * The position is 64 bit and relative to the ship's frame parent (the body the
 * HUD says "Relative to", normally the planet), in km here. Objects that share the
 * ship's exact position (its parts) move with it. The game's own frame handling
 * takes over from there, so a far jump can make it re-pick the frame. A landed or docked ship is
 * set to manual control. */
bool cheat_ship_position(double km[3]);
void cheat_ship_set_position(const double km[3]);
/* Moves the ship `km` further from (or, negative, nearer to) its frame parent */
void cheat_ship_move_out(double km);
/* Zero velocity relative to the frame parent */
void cheat_ship_stop(void);

/* The system's named bodies: planets, moons, stars and stations. `radius_km` is the planet's
 * (0.0 for a station, whose field means something else) */
typedef struct
{
	int slot;
	char name[32];
	bool planet; /* planet, moon or star, else a station or city */
	bool star;   /* a star (also `planet`) */
	double radius_km;
} CheatBody;
int cheat_bodies(CheatBody *out, int max);
/* Puts the ship `distance_km` above a planet's surface, or that far from a station, on the side it
 * is now (planet) or away from the planet the station is on (station); stops it. NULL or a reason. */
const char *cheat_goto_body(int slot, double distance_km);

/* Makes the game generate the system with this id (the game's 32 bit code, galaxy.h make_id) and
 * makes it the current one, as the start of a jump does. No tunnel, fuel or time; the ship keeps its
 * old position, which means nothing in the new system: follow with cheat_goto_body. NULL or a reason. */
const char *cheat_enter_system(unsigned id);

/* Sends event N to the game's modules (the module broadcast); 16 is the hyperspace arrival, which
 * generates the system's traders, pirates and police. For testing. */
void cheat_broadcast_event(int event);

/* Shipyard items, in the order of the game's equipment list */
typedef enum
{
	EQUIP_FLAG = 0,     /* a bit in the ship's equipment flags */
	EQUIP_CABIN = 2,    /* passenger cabin */
	EQUIP_LASER = 4,    /* goes on a gun mounting */
	EQUIP_DRIVE = 6,    /* the ship's drive */
	EQUIP_SHIELD = 8,   /* shield generator */
	EQUIP_MISSILE = 10, /* goes on a missile pylon */
	EQUIP_MINING = 12,  /* mining machine */
} EquipKind;

int cheat_equipment_count(void);
/* items from here to cheat_equipment_count() - 1 are added by mods */
int cheat_mod_equipment_first(void);
const char *cheat_equipment_name(int item);
EquipKind cheat_equipment_kind(int item);
int cheat_equipment_mass(int item);

/* The functions that change the ship return NULL on success or a short
 * reason why not */
bool cheat_has_flag(int item);
const char *cheat_toggle_flag(int item);

int cheat_drive(void); /* item, -1 = none */
const char *cheat_fit_drive(int item);

int cheat_gun_mounts(void);
const char *cheat_gun_mount_name(int mount);
int cheat_laser(int mount);                       /* item, -1 = empty */
const char *cheat_fit_laser(int mount, int item); /* item -1 removes */

int cheat_shields(void);                  /* generators fitted */
const char *cheat_add_shields(int count); /* negative removes */

int cheat_pylons(void);
int cheat_missiles(void);                /* pylons in use */
const char *cheat_fill_pylons(int item); /* item -1 empties them */

#ifdef __cplusplus
}
#endif

#endif /* CHEATS_H */
