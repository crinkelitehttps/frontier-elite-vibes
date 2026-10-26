/*
 * cheats.c - see cheats.h.
 *
 * Offsets are into the game's globals (A6, see game_state.h) or the player's
 * ship object; the fe2/fe2_modded.s code each one comes from is noted next to it.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cheats.h"
#include "game_state.h"
#include "host.h" /* rdbyte, wrbyte, ... */
#include "galaxy/fe2vm.h"

bool cheat_ignore_space;

/* A6 offsets */
#define A6_CASH          734   /* long, tenths of a credit */
#define A6_CARGO_SPACE   738   /* word, tonnes left for cargo after equipment */
#define A6_CARGO_USED    742   /* word, tonnes of cargo carried */
#define A6_CARGO         754   /* word per commodity, Water first */
#define A6_ELITE_RATING  824   /* long, points (L7e87a) */
#define A6_FEDERAL_RANK  828   /* word, points (L7e8b0) */
#define A6_IMPERIAL_RANK 830   /* word, points (L7e8b0) */
#define A6_LEGAL_1       13328 /* longs, one per authority (L7e8ca) */
#define A6_LEGAL_2       13332
#define A6_LEGAL_3       13336

/* Ship object offsets */
#define SHIP_MODEL      90  /* word, index into the game data */
#define SHIP_EQUIPMENT  158 /* long, EQUIP_FLAG bits */
#define SHIP_DAMAGED    162 /* long, EQUIP_FLAG bits that are broken */
#define SHIP_DRIVE      166 /* byte, drive param (L76c26) */
#define SHIP_GUN_MOUNTS 167 /* byte, number of gun mountings */
#define SHIP_LASERS     168 /* byte per mounting, laser param or 0 */
#define SHIP_PYLONS     172 /* byte per pylon, missile param or 0 */
#define SHIP_SHIELD     180 /* word, shield energy */
#define SHIP_SHIELD_MAX 182 /* word, 64 per generator (L76c38) */
#define SHIP_HULL       184 /* word */
#define SHIP_FUEL       242 /* long, drive fuel: $20000000 per tonne */

/* Ship model data (L45c06_GetGameDataObj, then +28 to the ship part) */
#define MODEL_SHIP_DATA 28 /* word, offset to the ship data */
#define SHIPDATA_HULL   6  /* word, hull mass: full hull is this * 4 */
#define SHIPDATA_PYLONS 18 /* word, missile pylons */

/* Tables in the game binary (fe2_labels.h) */
#define EQUIP_TABLE    FE2_equipment_list /* the shipyard's equipment list */
#define EQUIP_SIZE     12
#define FUEL_TANK      FE2_fuel_tank_sizes      /* tank size per drive, top byte */
#define ELITE_TABLE    FE2_elite_rating_points  /* rating points, ELITE first */
#define MILITARY_TABLE FE2_military_rank_points /* rank points, highest first */

#define MAX_PYLONS 8

static u32 a6(int offset)
{
	return GAME_A6 + offset;
}

static u16 rd16(u32 addr)
{
	return (u16)rdword(addr);
}

/* =========================================================================
 * Money and cargo
 * ========================================================================= */
long cheat_cash(void)
{
	return rdlong(a6(A6_CASH));
}

void cheat_set_cash(long tenths)
{
	if (tenths < 0)
		tenths = 0;
	if (tenths > 0x7fffffffL)
		tenths = 0x7fffffffL;
	wrlong(a6(A6_CASH), (int)tenths);
}

static const char *const commodity_names[CHEAT_COMMODITIES] = {
	"Water",          "Liquid Oxygen",   "Grain",          "Fruit and Veg.", "Animal Meat",
	"Synthetic Meat", "Liquor",          "Narcotics",      "Medicines",      "Fertilizer",
	"Animal Skins",   "Live Animals",    "Slaves",         "Luxury Goods",   "Heavy Plastics",
	"Metal Alloys",   "Precious Metals", "Gem Stones",     "Minerals",       "Hydrogen Fuel",
	"Military Fuel",  "Hand Weapons",    "Battle Weapons", "Nerve Gas",      "Industrial Parts",
	"Computers",      "Air Processors",  "Farm Machinery", "Robots",         "Radioactives",
	"Rubbish"};

const char *cheat_commodity_name(int i)
{
	return (i >= 0 && i < CHEAT_COMMODITIES) ? commodity_names[i] : "?";
}

int cheat_cargo(int i)
{
	return (i >= 0 && i < CHEAT_COMMODITIES) ? rd16(a6(A6_CARGO + 2 * i)) : 0;
}

int cheat_cargo_free(void)
{
	return (s16)rdword(a6(A6_CARGO_SPACE)) - (s16)rdword(a6(A6_CARGO_USED));
}

void cheat_add_cargo(int i, int tonnes)
{
	if (i < 0 || i >= CHEAT_COMMODITIES)
		return;
	int old = cheat_cargo(i), now = old + tonnes;
	if (now < 0)
		now = 0;
	if (now > 0xffff)
		now = 0xffff;
	wrword(a6(A6_CARGO + 2 * i), now);
	/* the game compares used tonnes with the space left, keep them in step */
	wrword(a6(A6_CARGO_USED), rdword(a6(A6_CARGO_USED)) + (now - old));
}

/* =========================================================================
 * Ratings
 * ========================================================================= */
const char *const cheat_elite_rank_names[CHEAT_ELITE_RANKS] = {
	"Harmless",      "Mostly Harmless", "Poor",      "Below Average", "Average",
	"Above Average", "Competent",       "Dangerous", "Deadly",        "ELITE"};
const char *const cheat_federal_rank_names[CHEAT_MILITARY_RANKS] = {
	"None",       "Private",       "Corporal", "Sergeant",  "Sergeant-Major", "Major",  "Colonel",
	"Lieutenant", "Lt. Commander", "Captain",  "Commodore", "Rear Admiral",   "Admiral"};
const char *const cheat_imperial_rank_names[CHEAT_MILITARY_RANKS] = {
	"Outsider", "Serf",  "Master", "Sir",     "Squire", "Lord",  "Baron",
	"Viscount", "Count", "Earl",   "Marquis", "Duke",   "Prince"};

/* Points needed for each rank: the game's tables list them highest first */
static u32 elite_points(int rank)
{
	return rank <= 0 ? 0 : (u32)rdlong(ELITE_TABLE + 4 * (CHEAT_ELITE_RANKS - 1 - rank));
}

static u16 military_points(int rank)
{
	return rank <= 0 ? 0 : rd16(MILITARY_TABLE + 2 * (CHEAT_MILITARY_RANKS - 1 - rank));
}

int cheat_elite_rank(void)
{
	u32 points = (u32)rdlong(a6(A6_ELITE_RATING));
	int rank = 0;
	while (rank + 1 < CHEAT_ELITE_RANKS && points >= elite_points(rank + 1))
		rank++;
	return rank;
}

void cheat_set_elite_rank(int rank)
{
	if (rank >= 0 && rank < CHEAT_ELITE_RANKS)
		wrlong(a6(A6_ELITE_RATING), (int)elite_points(rank));
}

static int military_rank(int offset)
{
	u16 points = rd16(a6(offset));
	int rank = 0;
	while (rank + 1 < CHEAT_MILITARY_RANKS && points >= military_points(rank + 1))
		rank++;
	return rank;
}

static void set_military_rank(int offset, int rank)
{
	if (rank >= 0 && rank < CHEAT_MILITARY_RANKS)
		wrword(a6(offset), military_points(rank));
}

int cheat_federal_rank(void)
{
	return military_rank(A6_FEDERAL_RANK);
}
void cheat_set_federal_rank(int rank)
{
	set_military_rank(A6_FEDERAL_RANK, rank);
}
int cheat_imperial_rank(void)
{
	return military_rank(A6_IMPERIAL_RANK);
}
void cheat_set_imperial_rank(int rank)
{
	set_military_rank(A6_IMPERIAL_RANK, rank);
}

void cheat_clear_criminal_record(void)
{
	wrlong(a6(A6_LEGAL_1), 0);
	wrlong(a6(A6_LEGAL_2), 0);
	wrlong(a6(A6_LEGAL_3), 0);
}

/* =========================================================================
 * Equipment list
 * ========================================================================= */
/* L7655c entries: +0 buy price, +2 sell price, +4 mass, +8 kind, +10 param
 * (flag bit, laser / drive / missile value). Names are the game's strings
 * $9873 onwards, in the same order. */
static const char *const equip_names[] = {
	"XB13 Dummy Prox. Mine", "XB74 Proximity Mine", "KL760 Homing Missile", "LV111 Smart Missile",
	"NN500 Naval Missile", "Auto Refueller", "Atmospheric Shielding", "Laser Cooling Booster",
	"Cargo Bay Life Support", "Scanner", "1MW Pulse Laser", "Radar Mapper", "E.C.M. System",
	"Automatic Pilot", "Extra Passenger Cabin", "Hyperspace Cloud Analyser", "Shield Generator",
	"5MW Pulse Laser", "Fuel Scoop", "Cargo Scoop Conversion", "Interplanetary Drive", "MB4 Mining Machine",
	"1MW Beam Laser", "Class 1 Hyperdrive", "Energy Booster Unit", "30MW Mining Laser", "4MW Beam Laser",
	"Naval E.C.M. System", "Hull Auto-Repair System", "Energy Bomb", "Escape Capsule", "20MW Beam Laser",
	"Class 2 Hyperdrive", "100MW Beam Laser", "Class 3 Hyperdrive", "Class 1 Military Drive",
	"Class 4 Hyperdrive", "Class 2 Military Drive", "Fighter Launch Device", "Class 5 Hyperdrive",
	"Small Plasma Accelerator", "Class 3 Military Drive", "Class 6 Hyperdrive", "Large Plasma Accelerator",
	"Class 7 Hyperdrive",
#if FE2_USE_MODDED
	/* mods, from the end of fe2_modded.s */
	"Auto Field-Maintenance Unit (Game Mod)",
#endif
};
#define EQUIP_COUNT ((int)(sizeof(equip_names) / sizeof(equip_names[0])))

/* The game's list has 45 items; mods add theirs after it */
#define GAME_EQUIP_COUNT 45

static u32 equip(int item)
{
#if FE2_USE_MODDED
	if (item == GAME_EQUIP_COUNT)
		return FE2_Lafmu_equipment;
#endif
	return EQUIP_TABLE + (u32)item * EQUIP_SIZE;
}

int cheat_equipment_count(void)
{
	return EQUIP_COUNT;
}

int cheat_mod_equipment_first(void)
{
	return GAME_EQUIP_COUNT;
}

const char *cheat_equipment_name(int item)
{
	return (item >= 0 && item < EQUIP_COUNT) ? equip_names[item] : "None";
}

EquipKind cheat_equipment_kind(int item)
{
	if (item < 0 || item >= EQUIP_COUNT)
		return (EquipKind)-1;
	return (EquipKind)(u8)rdbyte(equip(item) + 8);
}

int cheat_equipment_mass(int item)
{
	return (item >= 0 && item < EQUIP_COUNT) ? rd16(equip(item) + 4) : 0;
}

static u8 equipment_param(int item)
{
	return (u8)rdbyte(equip(item) + 10);
}

/* The item of `kind` whose param is `value`, -1 if none */
static int find_item(EquipKind kind, u8 value)
{
	for (int i = 0; i < EQUIP_COUNT; i++)
		if (cheat_equipment_kind(i) == kind && equipment_param(i) == value)
			return i;
	return -1;
}

/* Equipment takes cargo space: `tonnes` more (or less if negative) */
static const char *take_space(int tonnes)
{
	if (tonnes > 0 && !cheat_ignore_space && cheat_cargo_free() < tonnes)
		return "Not enough cargo space";
	wrword(a6(A6_CARGO_SPACE), rdword(a6(A6_CARGO_SPACE)) - tonnes);
	return NULL;
}

/* =========================================================================
 * The player's ship
 * ========================================================================= */
static u32 ship(void)
{
	return game_player_ship();
}

bool cheat_have_ship(void)
{
	return ship() != 0;
}

/* Object position: low longs at +44, high longs at +56 (mm, 2^32 mm per high
 * unit), relative to the frame parent. Velocity: three longs at +98. */
#define OBJ_POS_LO 44
#define OBJ_POS_HI 56
#define OBJ_VEL    98  /* three longs, the whole part */
#define OBJ_VEL_FRAC 192 /* three words, the fraction */
#define OBJ_PARENT 68  /* byte, the frame parent's slot, 0 for none */
#define OBJ_TYPE   94  /* byte, 1 for a body (planet, star, station) */
#define OBJ_NAME   246 /* the body's name */
#define OBJ_RADIUS_LO 266 /* long, fraction of a position unit */
#define OBJ_RADIUS_HI 270 /* word, whole position units */
#define MM_PER_KM  1e6
#define OBJECT_SLOTS 115

typedef long long pos64;

static pos64 obj_pos(u32 obj, int axis)
{
	return ((pos64)rdlong(obj + OBJ_POS_HI + 4 * axis) << 32) | (u32)rdlong(obj + OBJ_POS_LO + 4 * axis);
}

static void obj_set_pos(u32 obj, int axis, pos64 v)
{
	wrlong(obj + OBJ_POS_LO + 4 * axis, (int)(u32)(v & 0xffffffff));
	wrlong(obj + OBJ_POS_HI + 4 * axis, (int)(v >> 32));
}

/* Object slot i: the flag bytes are the first 115 bytes of the big space, the objects follow
 * at the offsets of the table L44b02 */
static u32 slot_object(int i)
{
	return FE2_big_space + (u16)rdword(FE2_obj_offsets + 2 * (u32)i);
}

/* Applies `fn` to the position of the ship and of every object at the same spot */
/* new_parent: the slot the ship (and what moves with it) becomes relative to, -1 to keep the parent */
static void move_ship(const pos64 to[3], int new_parent)
{
	u32 me = ship();
	/* a ship that is landed or docked stays so until told otherwise: hand it back to manual control */
	FlightStatus fs = game_flight_status();
	if (fs == FLIGHT_LANDED || fs == FLIGHT_LANDED_ROUGH || fs == FLIGHT_DOCKED)
		wrbyte(a6(A6_FLIGHT_STATE), 0);
	pos64 old[3] = {obj_pos(me, 0), obj_pos(me, 1), obj_pos(me, 2)};
	for (int i = 0; i < OBJECT_SLOTS; i++)
	{
		u32 o = slot_object(i);
		if (o != me && (!rdbyte(FE2_big_space + i) || obj_pos(o, 0) != old[0] || obj_pos(o, 1) != old[1] ||
						obj_pos(o, 2) != old[2]))
			continue;
		for (int a = 0; a < 3; a++)
			obj_set_pos(o, a, to[a]);
		if (new_parent >= 0)
			wrbyte(o + OBJ_PARENT, new_parent);
	}
}

bool cheat_ship_position(double km[3])
{
	if (!ship())
		return false;
	for (int a = 0; a < 3; a++)
		km[a] = (double)obj_pos(ship(), a) / MM_PER_KM;
	return true;
}

void cheat_ship_set_position(const double km[3])
{
	if (!ship())
		return;
	pos64 to[3];
	for (int a = 0; a < 3; a++)
		to[a] = (pos64)(km[a] * MM_PER_KM);
	move_ship(to, -1);
}

void cheat_ship_move_out(double km)
{
	double p[3];
	if (!cheat_ship_position(p))
		return;
	double r = sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
	if (r < 1.0)
		return;
	double scale = (r + km) / r;
	if (scale < 0.0)
		scale = 0.0;
	for (int a = 0; a < 3; a++)
		p[a] *= scale;
	cheat_ship_set_position(p);
}

void cheat_ship_stop(void)
{
	if (!ship())
		return;
	for (int a = 0; a < 3; a++)
	{
		wrlong(ship() + OBJ_VEL + 4 * a, 0);
		wrword(ship() + OBJ_VEL_FRAC + 2 * a, 0);
	}
}

/* ---- bodies ---- */

/* Position in mm in the root frame: the object's own plus its parents' */
static void abs_pos(int slot, pos64 out[3])
{
	out[0] = out[1] = out[2] = 0;
	for (int depth = 0; slot > 0 && slot < OBJECT_SLOTS && depth < 8; depth++)
	{
		u32 o = slot_object(slot);
		for (int a = 0; a < 3; a++)
			out[a] += obj_pos(o, a);
		slot = (u8)rdbyte(o + OBJ_PARENT);
	}
}

/* Models 61-87 and 193-194 hold the planet command: planets, moons and stars (the catalog,
 * tools/galaxy_catalog, has the model of each body kind: 77-87 are the stars, 87 a white dwarf) */
static bool is_planet_model(u32 obj)
{
	int model = (u16)rdword(obj + 90) / 2;
	return (model >= 61 && model <= 87) || model == 193 || model == 194;
}

static bool is_star_model(u32 obj)
{
	int model = (u16)rdword(obj + 90) / 2;
	return model >= 77 && model <= 87;
}

int cheat_bodies(CheatBody *out, int max)
{
	int n = 0;
	for (int i = 1; i < OBJECT_SLOTS && n < max; i++)
	{
		u32 o = slot_object(i);
		if (!rdbyte(FE2_big_space + i) || (u8)rdbyte(o + OBJ_TYPE) != 1 || !rdbyte(o + OBJ_NAME))
			continue;
		CheatBody *b = &out[n++];
		b->slot = i;
		b->planet = is_planet_model(o);
		b->star = is_star_model(o);
		int k = 0;
		for (; k < (int)sizeof(b->name) - 1 && rdbyte(o + OBJ_NAME + k); k++)
			b->name[k] = (char)rdbyte(o + OBJ_NAME + k);
		b->name[k] = 0;
		b->radius_km = b->planet ? ((double)(u16)rdword(o + OBJ_RADIUS_HI) +
									(double)(u32)rdlong(o + OBJ_RADIUS_LO) / 4294967296.0) * 4294.967296
								 : 0.0;
	}
	return n;
}

/* ---- ships ---- */

#define OBJ_MODEL  90  /* word, byte offset into the game data's model table */
#define OBJ_CLASS  236 /* byte, see docs/npc-ship-ai.md section 2.2 */
#define OBJ_STATE  211 /* byte, the AI state */
#define OBJ_REG    246 /* the ship's registration (the body's name for a body) */
#define OBJ_MODULE_SHIPS 11
#define SHIPDATA_NAME    14 /* word, string id of the ship type's name */
#define A6_TARGET        708   /* byte, the target's object id (the flight view's click, the map's pick and L63aa4 write it) */
#define A6_GAME_STRINGS  14474 /* long, the core string table: word offsets, then the texts */

/* Copies a game string, printable characters only */
static void copy_text(char *dst, int size, u32 src)
{
	int k = 0;
	for (; k < size - 1; k++)
	{
		int c = (u8)rdbyte(src + k);
		if (c < 32 || c > 126)
			break;
		dst[k] = (char)c;
	}
	dst[k] = 0;
}

/* The ship data of the object's model, or 0 when the model is not a ship's */
static u32 model_ship_data(u32 obj)
{
	u32 data = (u32)rdlong(a6(A6_GAME_DATA));
	u32 model = data + rd16(data + rd16(obj + OBJ_MODEL));
	u32 off = rd16(model + MODEL_SHIP_DATA);
	return off ? model + off : 0;
}

/* The ship type's name: a core string ($4000 + n), the table at A6_game_strings holding word offsets */
static void ship_variant(u32 ship_data, char *out, int size)
{
	u32 id = rd16(ship_data + SHIPDATA_NAME);
	u32 strings = (u32)rdlong(a6(A6_GAME_STRINGS));
	if ((id & 0xc000) != 0x4000 || !strings)
	{
		snprintf(out, size, "model %u", id);
		return;
	}
	copy_text(out, size, strings + (u32)(int16_t)rd16(strings + 2 * (id & 0x3fff)));
	if (!out[0])
		snprintf(out, size, "string %u", id);
}

static int ship_nearer(const void *a, const void *b)
{
	double x = ((const CheatShip *)a)->distance_km, y = ((const CheatShip *)b)->distance_km;
	return x < y ? -1 : x > y;
}

int cheat_ships(CheatShip *out)
{
	if (!ship())
		return 0;
	pos64 base[3], me[3];
	abs_pos((u8)rdbyte(ship() + OBJ_PARENT), base);
	for (int a = 0; a < 3; a++)
		me[a] = base[a] + obj_pos(ship(), a);
	int n = 0;
	for (int i = 1; i < OBJECT_SLOTS; i++)
	{
		u32 o = slot_object(i);
		if (o == ship() || !rdbyte(FE2_big_space + i) || (u8)rdbyte(o + OBJ_TYPE) != OBJ_MODULE_SHIPS)
			continue;
		int class_ = (u8)rdbyte(o + OBJ_CLASS);
		if (class_ == 0xfa || class_ >= 0xfd) /* cargo, missiles, flashes and the player; $fb is a pirate */
			continue;
		u32 data = model_ship_data(o);
		if (!data)
			continue;
		CheatShip *s = &out[n++];
		s->slot = i;
		copy_text(s->registration, sizeof(s->registration), o + OBJ_REG);
		if (!s->registration[0])
			snprintf(s->registration, sizeof(s->registration), "-");
		ship_variant(data, s->variant, sizeof(s->variant));
		pos64 at[3];
		abs_pos((u8)rdbyte(o + OBJ_PARENT), at);
		double sum = 0.0;
		for (int a = 0; a < 3; a++)
		{
			double d = (double)(at[a] + obj_pos(o, a) - me[a]) / MM_PER_KM;
			sum += d * d;
		}
		s->distance_km = sqrt(sum);
		s->state = (u8)rdbyte(o + OBJ_STATE);
		s->at_rest = !rdlong(o + OBJ_VEL) && !rdlong(o + OBJ_VEL + 4) && !rdlong(o + OBJ_VEL + 8);
	}
	qsort(out, n, sizeof(*out), ship_nearer);
	return n;
}

int cheat_target(void)
{
	return ship() ? (u8)rdbyte(a6(A6_TARGET)) : 0;
}

const char *cheat_set_target(int slot)
{
	if (!ship())
		return "No ship yet.";
	if (slot != 0 && (slot < 1 || slot >= OBJECT_SLOTS || !rdbyte(FE2_big_space + slot) || slot_object(slot) == ship()))
		return "That object is gone.";
	wrbyte(a6(A6_TARGET), slot);
	return NULL;
}

void cheat_target_name(char *out, int size)
{
	int slot = cheat_target();
	out[0] = 0;
	if (slot > 0 && slot < OBJECT_SLOTS && rdbyte(FE2_big_space + slot))
		copy_text(out, size, slot_object(slot) + OBJ_REG);
}

const char *cheat_goto_body(int slot, double distance_km)
{
	if (!ship())
		return "No ship yet.";
	if (slot < 1 || slot >= OBJECT_SLOTS || !rdbyte(FE2_big_space + slot))
		return "That body is gone.";
	u32 target = slot_object(slot);
	int ship_parent = (u8)rdbyte(ship() + OBJ_PARENT);
	pos64 t[3], p[3], me[3];
	abs_pos(slot, t);
	abs_pos(ship_parent, p);
	for (int a = 0; a < 3; a++)
		me[a] = p[a] + obj_pos(ship(), a);

	bool planet = is_planet_model(target);
	double dir[3];
	if (planet)
		for (int a = 0; a < 3; a++)
			dir[a] = (double)(me[a] - t[a]);
	else
	{
		pos64 tp[3];
		abs_pos((u8)rdbyte(target + OBJ_PARENT), tp);
		for (int a = 0; a < 3; a++)
			dir[a] = (double)(t[a] - tp[a]);
	}
	double len = sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	if (len < 1.0)
	{
		dir[0] = dir[1] = 0.0;
		dir[2] = len = 1.0;
	}
	double reach = distance_km * MM_PER_KM;
	if (planet)
		reach += ((double)(u16)rdword(target + OBJ_RADIUS_HI) + (double)(u32)rdlong(target + OBJ_RADIUS_LO) / 4294967296.0) *
				 4294967296.0;
	pos64 to[3];
	for (int a = 0; a < 3; a++)
		to[a] = (pos64)(dir[a] / len * reach) + (planet ? 0 : t[a] - p[a]);
	/* a planet becomes the ship's frame, so it is at rest relative to it; near a station the ship
	 * stays in whatever frame it is in (the game picks the station's when it gets close) */
	move_ship(to, planet ? slot : -1);
	cheat_ship_stop();
	return NULL;
}

/* ---- changing system ---- */

/* The A5 table entries the game's jump start uses (L814dc, module 140(a6)) */
#define A5_BROADCAST 450 /* L458b4: d0 = event for the modules */
#define A5_CLEAR_OBJECTS 510 /* L44be8: a4 = object table, frees every slot */
#define A5_SET_OBJECT_FLAG 528 /* L4484e: a0 = object, d0 = flag byte */
#define A6_GALMAP_REF_POS 936 /* 3 longs: where the galaxy map measures distances from */
#define A6_GALMAP_SEL_POS 948 /* 3 longs: the selected star */

static int game_call(u32 addr, fe2vm_regs *r)
{
	return fe2vm_call(addr, r);
}

/* What the game does when a hyperspace jump starts (L814dc, after its fuel
 * and range checks) and the destination is generated: free the objects but
 * the player's, tell the modules, make the system (L8145e), tell them again.
 * It is not a jump (no tunnel, no fuel, no time passes); the ship is left
 * where it was, which means nowhere in the new system. */
const char *cheat_enter_system(unsigned id)
{
	if (!ship())
		return "No ship yet.";
	u32 player = ship();
	fe2vm_regs r;
	fe2vm_begin();
	memset(&r, 0, sizeof(r));
	r.a[4] = (u32)rdlong(a6(14466));
	game_call(FE2_a5_jumptab + A5_CLEAR_OBJECTS, &r);
	wrword(a6(596), 0);
	memset(&r, 0, sizeof(r));
	r.a[0] = player;
	r.d[0] = 79;
	game_call(FE2_a5_jumptab + A5_SET_OBJECT_FLAG, &r);
	wrbyte(player + OBJ_PARENT, 0);
	wrbyte(player + 69, 0);
	wrbyte(player + 281, 0);
	wrbyte(a6(708), 0);
	wrbyte(a6(732), 0);
	wrbyte(a6(A6_FLIGHT_STATE), 0);
	/* select the system as the galaxy map does when it is clicked (galaxy.cpp Galaxy::select):
	 * L8145e takes the new system from the map's selection */
	memset(&r, 0, sizeof(r));
	r.d[0] = 2; /* L83b46 mode 2: the star's position, type and number */
	r.d[1] = id;
	if (game_call(FE2_L83b46, &r) || (int)r.d[0] == -1)
	{
		fe2vm_end();
		return "No such system.";
	}
	for (int i = 0; i < 3; i++)
	{
		wrlong(a6(A6_GALMAP_SEL_POS) + 4 * i, (int)r.d[1 + i]);
		wrlong(a6(A6_GALMAP_REF_POS) + 4 * i, (int)r.d[1 + i]);
	}
	u32 number = r.d[7], type = r.d[6];
	memset(&r, 0, sizeof(r));
	r.d[5] = number;
	r.d[6] = type;
	game_call(FE2_L849a2, &r);
	memset(&r, 0, sizeof(r));
	r.d[0] = 19;
	game_call(FE2_a5_jumptab + A5_BROADCAST, &r);
	memset(&r, 0, sizeof(r));
	r.d[0] = 0;
	r.d[6] = id;
	r.a[0] = player;
	int bad = game_call((u32)rdlong(a6(140)) + 32, &r); /* module fn 32, d0 = 0: L8145e */
	memset(&r, 0, sizeof(r));
	r.d[0] = 2;
	game_call(FE2_a5_jumptab + A5_BROADCAST, &r);
	if (bad)
	{
		fe2vm_end();
		return "The game's system generator failed; nothing changed.";
	}
	fe2vm_end_keep();
	return NULL;
}

void cheat_broadcast_event(int event)
{
	fe2vm_regs r;
	fe2vm_begin();
	memset(&r, 0, sizeof(r));
	r.d[0] = event;
	game_call(FE2_a5_jumptab + A5_BROADCAST, &r);
	fe2vm_end_keep();
}

/* The ship part of the player's ship model */
static u32 ship_data(void)
{
	u32 data = (u32)rdlong(a6(A6_GAME_DATA));
	u32 model = data + rd16(data + rd16(ship() + SHIP_MODEL));
	return model + rd16(model + MODEL_SHIP_DATA);
}

static u32 fuel_tank_size(void)
{
	u8 drive = (u8)rdbyte(ship() + SHIP_DRIVE);
	return drive < 16 ? (u32)(u8)rdbyte(FUEL_TANK + drive) << 24 : 0;
}

void cheat_refuel(void)
{
	if (ship())
		wrlong(ship() + SHIP_FUEL, (int)fuel_tank_size());
}

void cheat_repair(void)
{
	if (!ship())
		return;
	wrword(ship() + SHIP_HULL, rd16(ship_data() + SHIPDATA_HULL) * 4);
	wrword(ship() + SHIP_SHIELD, rdword(ship() + SHIP_SHIELD_MAX));
	wrlong(ship() + SHIP_DAMAGED, 0);
}

/* ---- equipment flags ---- */
bool cheat_has_flag(int item)
{
	if (!ship() || cheat_equipment_kind(item) != EQUIP_FLAG)
		return false;
	return (rdlong(ship() + SHIP_EQUIPMENT) >> equipment_param(item)) & 1;
}

const char *cheat_toggle_flag(int item)
{
	if (!ship() || cheat_equipment_kind(item) != EQUIP_FLAG)
		return "No ship";
	u32 bit = 1u << equipment_param(item);
	u32 fitted = (u32)rdlong(ship() + SHIP_EQUIPMENT);
	bool removing = fitted & bit;

	const char *err = take_space(removing ? -cheat_equipment_mass(item) : cheat_equipment_mass(item));
	if (err)
		return err;
	wrlong(ship() + SHIP_EQUIPMENT, (int)(fitted ^ bit));
	wrlong(ship() + SHIP_DAMAGED, (int)((u32)rdlong(ship() + SHIP_DAMAGED) & ~bit));
	return NULL;
}

/* ---- drive ---- */
int cheat_drive(void)
{
	return ship() ? find_item(EQUIP_DRIVE, (u8)rdbyte(ship() + SHIP_DRIVE)) : -1;
}

const char *cheat_fit_drive(int item)
{
	if (!ship())
		return "No ship";
	if (cheat_equipment_kind(item) != EQUIP_DRIVE)
		return "Not a drive";
	const char *err = take_space(cheat_equipment_mass(item) - cheat_equipment_mass(cheat_drive()));
	if (err)
		return err;
	wrbyte(ship() + SHIP_DRIVE, equipment_param(item));
	/* a smaller drive has a smaller tank */
	if ((u32)rdlong(ship() + SHIP_FUEL) > fuel_tank_size())
		cheat_refuel();
	return NULL;
}

/* ---- lasers ---- */
int cheat_gun_mounts(void)
{
	int n = ship() ? (u8)rdbyte(ship() + SHIP_GUN_MOUNTS) : 0;
	return n > 4 ? 4 : n;
}

const char *cheat_gun_mount_name(int mount)
{
	static const char *const names[4] = {"Front", "Rear", "Top turret", "Bottom turret"};
	return (mount >= 0 && mount < 4) ? names[mount] : "?";
}

int cheat_laser(int mount)
{
	if (mount < 0 || mount >= cheat_gun_mounts())
		return -1;
	u8 laser = (u8)rdbyte(ship() + SHIP_LASERS + mount);
	return laser ? find_item(EQUIP_LASER, laser) : -1;
}

const char *cheat_fit_laser(int mount, int item)
{
	if (mount < 0 || mount >= cheat_gun_mounts())
		return "No such mounting";
	if (item >= 0 && cheat_equipment_kind(item) != EQUIP_LASER)
		return "Not a laser";
	int mass = (item >= 0 ? cheat_equipment_mass(item) : 0) - cheat_equipment_mass(cheat_laser(mount));
	const char *err = take_space(mass);
	if (err)
		return err;
	wrbyte(ship() + SHIP_LASERS + mount, item >= 0 ? equipment_param(item) : 0);
	return NULL;
}

/* ---- shields ---- */
static int shield_item(void)
{
	for (int i = 0; i < EQUIP_COUNT; i++)
		if (cheat_equipment_kind(i) == EQUIP_SHIELD)
			return i;
	return -1;
}

int cheat_shields(void)
{
	return ship() ? rd16(ship() + SHIP_SHIELD_MAX) / 64 : 0;
}

const char *cheat_add_shields(int count)
{
	if (!ship())
		return "No ship";
	if (cheat_shields() + count < 0)
		count = -cheat_shields();
	const char *err = take_space(count * cheat_equipment_mass(shield_item()));
	if (err)
		return err;
	int max = rd16(ship() + SHIP_SHIELD_MAX) + count * 64;
	wrword(ship() + SHIP_SHIELD_MAX, max);
	if (rd16(ship() + SHIP_SHIELD) > max)
		wrword(ship() + SHIP_SHIELD, max);
	return NULL;
}

/* ---- missiles ---- */
int cheat_pylons(void)
{
	int n = ship() ? rd16(ship_data() + SHIPDATA_PYLONS) : 0;
	return n > MAX_PYLONS ? MAX_PYLONS : n;
}

int cheat_missiles(void)
{
	int n = 0;
	for (int i = 0; i < cheat_pylons(); i++)
		n += rdbyte(ship() + SHIP_PYLONS + i) != 0;
	return n;
}

const char *cheat_fill_pylons(int item)
{
	if (!ship())
		return "No ship";
	if (item >= 0 && cheat_equipment_kind(item) != EQUIP_MISSILE)
		return "Not a missile";
	int pylons = cheat_pylons();
	if (pylons == 0)
		return "This ship has no pylons";

	/* every missile weighs a tonne */
	int now = item >= 0 ? pylons : 0;
	const char *err = take_space(now - cheat_missiles());
	if (err)
		return err;
	for (int i = 0; i < pylons; i++)
		wrbyte(ship() + SHIP_PYLONS + i, item >= 0 ? equipment_param(item) : 0);
	return NULL;
}
