/*
 * custom_ships.h - custom 3D models (the "Custom ship models" mod, Lmdlmod_ in
 * fe2/fe2_modded.s).
 *
 * At startup, after the game binary is in RAM, every tools/fe2ShipBuilder/custom_ships/NNN.fe2m
 * file (NNN = model number, 000-511) is copied into the game's model pool
 * and the model numbers it holds are pointed at the copies, so the game
 * draws (and flies, and collides with) those instead of its own. The files
 * are compiled into the executable (CMakeLists.txt), so it stays one
 * standalone file; build them with tools/fe2ShipBuilder (FE2ShipBuilder.exe).
 *
 * .fe2m format, all big endian:
 *   "FE2M"  u16 version (1 or 2)  u16 model count
 *   per model: u16 model number, u16 reserved, u32 byte length, then the
 *   model exactly as the game stores it (header, vertices, normals, code,
 *   collision, ship data; offsets relative to its first byte), padded to
 *   an even length.
 *   version 2 then has u16 record count, and per record u16 type, u16
 *   length, data padded to even, all about the first model:
 *     1 name     ASCII ship name (becomes core string $4070 + n)
 *     2 like     u16 original model number the new ship behaves like
 *     3 picks    (u8 category, u8 weight) pairs: L5fac2_ship_picks lists
 *                the ship joins - shipyards, traders, pirates, police...
 *
 * New ship types use model numbers 240-511 (FE2ShipBuilder: ships from 240 up,
 * parts of big ships from 511 down): see "MOD Custom ship models, part
 * 2" in fe2_modded.s.
 *
 * Does nothing in the original game build (GLF_MODDED_FE2 off).
 */
#ifndef CUSTOM_SHIPS_H
#define CUSTOM_SHIPS_H

#ifdef __cplusplus
extern "C"
{
#endif

/* The .fe2m files compiled into the executable (CMakeLists.txt makes them
 * from tools/fe2ShipBuilder/custom_ships; the list ends with an entry whose name is NULL) */
typedef struct
{
	const char *name;
	const unsigned char *data;
	unsigned int len;
} CustomShipFile;
extern const CustomShipFile custom_ships_embedded[];

/* Put the compiled in models (then any custom_ships folder beside the game,
 * for quick testing without a rebuild) into the game's RAM; call once after
 * Init680x0() */
void custom_ships_install(void);

/* How many models were replaced or added (for the mods menu / logs) */
int custom_ships_count(void);

/* How many of those are new ship types (model numbers 240 and up) */
int custom_ships_new_ships(void);

/* Models made while the game runs (planet models from a system file, custom_system.c).
 * pool_alloc: `size` bytes of the model pool (an address in game RAM), 0 if full or
 * not the modded build; free_index: an unused model number, -1 if none;
 * set_model: make model `index` read from `addr` (0 = back to the game's table);
 * model_addr: where model `index` is if redirected, else 0 (use the game's table) */
unsigned int custom_ships_pool_alloc(unsigned int size);
int custom_ships_free_index(void);
void custom_ships_set_model(int index, unsigned int addr);
unsigned int custom_ships_model_addr(int index);

/* One installed .fe2m file, for the mods menu */
typedef struct
{
	char file[40];   /* e.g. 241.fe2m */
	char source[24]; /* "built in", "saved into the exe" or "folder" */
	char name[41];   /* the ship's name, "" if the file has none */
	int model;       /* its first model number */
	int models;      /* models in the file (big ships have parts) */
	int like;        /* original model a new ship type behaves like, -1 = not set */
	int lists;       /* ship lists (shipyards, traders...) it joins */
	int new_ship;    /* 1: a new ship type, 0: replaces one of the game's models */
} CustomShipInfo;

int custom_ships_info_count(void);
const CustomShipInfo *custom_ships_info(int i);

/* Cheat: put every new ship type up for sale in this system's shipyards
 * now (they normally show up at random, by their weights). Returns the
 * number of starports stocked, 0 when there are none or no new ships. */
int custom_ships_stock_shipyards(void);

/* Mods framework strip function: objects of a new ship type become the
 * original ship they're like, for saves the unmodded game can load */
void custom_ships_strip(void);

#ifdef __cplusplus
}
#endif

#endif
