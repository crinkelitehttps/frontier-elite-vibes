/*
 * galaxy.h - the galaxy as the game sees it, read by running the game's own
 * galaxy map code (fe2vm.h). Nothing here recomputes the game's maths: sector
 * contents, positions, names, star types, system data, trade, planets and
 * distances all come back from the game routines named next to each call in
 * galaxy.cpp.
 *
 * Coordinates are the game's: sectors 0..$1fff on x and y, Sol's sector at
 * ($1718,$1524), which the game shows as sector (0,0). Inside a sector a
 * position is 16.16: 65536 units per sector. System ids are the game's
 * 32 bit codes, (y << 19) | (x << 6) | number in the sector.
 */
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace galaxy
{

/* The game shows sector numbers relative to Sol's sector (L83a32 and L84c04
 * subtract these before printing) */
constexpr int SOL_X = 0x1718, SOL_Y = 0x1524;
constexpr int SECTORS = 0x2000; /* L8ad0e: no stars outside 0..$1fff */
constexpr int UNITS_PER_SECTOR = 0x10000;

inline uint32_t make_id(int x, int y, int n)
{
	return ((uint32_t)y << 19) | ((uint32_t)x << 6) | (uint32_t)n;
}
inline int id_x(uint32_t id)
{
	return (id >> 6) & 0x1fff;
}
inline int id_y(uint32_t id)
{
	return (id >> 19) & 0x1fff;
}
inline int id_n(uint32_t id)
{
	return id & 0x3f;
}

struct Star
{
	uint32_t id = 0;
	std::string name;
	int32_t x = 0, y = 0, z = 0; /* 16.16 sector units; z is above/below the galactic plane */
	int multiple = 0;			 /* 0 single, else the companions' string (see description) */
	int type = 0;				 /* star type number, (type byte & $7e) / 2 */
	int type_byte = 0;			 /* low byte of the star type word (bit 7: no planet data) */
	std::string description;	 /* the game's "Type 'G' yellow star" line, with multiple star prefix */
	uint32_t colour = 0;		 /* RGB guessed from the colour word in the description */
	/* from the system info table (L4beee, mode 0) */
	int info = 0;		/* byte 10: allegiance << 6 | government/economy */
	int allegiance = 0; /* info >> 6: 0 independent, 1 Empire, 2 Federation (see the strings) */
	bool explored = false; /* byte 5 != 0: the game has planet data for it */
};

struct Sector
{
	int x = 0, y = 0;
	std::vector<Star> stars;
	int special = 0; /* hand placed sectors: the extra object the galaxy map draws there (L84616), 0 = none */
};

/* A line of text as the game drew it: x, y in 320x200 screen pixels */
struct TextRun
{
	int x, y;
	uint32_t colour; /* RGB */
	std::string text;
};
using Screen = std::vector<TextRun>;

struct Body
{
	int index = 0;	/* entry in the game's body list (17348(a6)), 0 = primary star */
	int parent = -1; /* what it orbits, an index in this list; 8(entry) holds it + 1 */
	int kind = 0;	 /* 10(entry): body kind, the game's picture and description number */
	bool surface = false; /* kind $46, a surface starport: the game's map leaves these out */
	bool port = false;	  /* a starport (surface or orbital): kinds $40-$46 */
	std::string name;
	std::string kind_name; /* the game's words for the kind ("Orbital trading post"...), L84700 $ec + kind */
	Screen details;		   /* the game's body panel (l84cde); none for surface starports */
	uint8_t raw[64] = {};  /* the game's list entry (A6_body_list), for the catalog */
	/* from the panel and the list entry; mass and temperature are not-a-number / -9999 when the panel has none */
	int model = 0;			  /* the model index the game gives its object (entry word 12, / 2) */
	double mass_earth = -1;	  /* "334670.16 Earth masses" */
	int temperature_c = -9999; /* "Surface temperature 5800°C" */
	double radius_km = -1;	  /* from the model, as L448b6 sets the object's radius; -1 for starports */
	/* the orbit, read from the numbers details shows; au < 0 when it has none */
	double au = -1, eccentricity = 0, inclination = 0, period_days = 0;
};

/* A click area the game registers on its screen (L4231a): key $80+n picks body n */
struct HitArea
{
	int x0, y0, x1, y1, key;
};

/* The game's system picture (l84f02): its body icons, as drawn into the screen */
struct Picture
{
	std::vector<uint8_t> pixels; /* 320 x 200, palette indices */
	uint32_t palette[16] = {};	 /* RGB */
	std::vector<HitArea> areas;
	int top = 0, bottom = 0; /* the rows with icons in them */
};

struct Details
{
	uint32_t id = 0;
	bool ok = false;
	bool hangs = false;	  /* the game's own code never finished (see the error): it would hang the game too */
	bool crashes = false; /* the game's own code jumped to a bad address: it would crash the game too */
	std::string error;
	Screen system; /* "Government type..." (l84c04) */
	Screen trade;  /* imports and exports (l84b34) */
	Screen bodies_screen; /* the system's bodies view (l84f02), or "Unexplored System" */
	Picture picture;	  /* its icons */
	std::vector<Body> bodies;
};

/* What a system has, for the map's overlay: from the game's system
 * generator (the one l84f02 runs), without the screens */
struct Summary
{
	uint8_t surface_ports = 0, orbital_ports = 0, bodies = 0;
	bool hangs = false, crashes = false; /* as in Details; not saved in the cache */
	int ports() const { return surface_ports + orbital_ports; }
};

/* the systems of a sector with starports, for the overlay */
struct SectorSummary
{
	uint16_t inhabited = 0; /* systems with a starport */
	uint16_t ports = 0, orbital = 0;
};

class Galaxy
{
  public:
	bool init(std::string &err);
	const std::string &error() const { return err_; }
	void clear_error() { err_.clear(); }

	/* sector contents (L8444a, L83d3c, L84700, L4beee); cached */
	const Sector &sector(int x, int y);
	bool cached(int x, int y) const;
	size_t cache_size() const { return sectors_.size(); }
	void trim_cache(size_t max_sectors);

	/* the game's galaxy density (L8ad0e): smooth when noise is false (the
	   background of the galaxy map), else what sets a sector's star count */
	int density(int x, int y, bool noise);
	/* stars the game puts in a sector: density >> 10, at most 62 (L8444a) */
	int star_count(int x, int y);

	/* distance in hundredths of a light year, as the game works it out (L84ae6) */
	uint32_t distance(const Star &a, const Star &b);
	std::string distance_text(uint32_t centi_ly);

	/* everything the galaxy map's info screens show about a system; cached */
	const Details &details(uint32_t id);

	/* a star by id (generates its sector) */
	const Star *star(uint32_t id);

	/* a string from the galaxy map's string table, by L84700 code */
	std::string map_string(int code, uint32_t d1 = 0, uint32_t d2 = 0, uint32_t d3 = 0);
	/* any game string by number (L3e73a_GetFmtStr), e.g. $84b7 + allegiance */
	std::string game_string(int id);

	/* the sectors the game places by hand (Sol and its neighbours, L84530) */
	std::vector<std::pair<int, int>> core_sectors();
	int core_special(int x, int y); /* Sector::special */

	/* light years per sector along x, from the game's distance routine */
	double ly_per_sector();

	/* -- the starport overlay ------------------------------------------------ */
	const Summary &summary(uint32_t id);
	/* works out one sector's summary (generating it and running the system
	   generator for each explored system); cached, and saved in the cache file */
	const SectorSummary &sector_summary(int x, int y);
	bool has_sector_summary(int x, int y) const;
	const std::unordered_map<uint32_t, SectorSummary> &sector_summaries() const { return sector_sums_; }
	bool load_cache(const std::string &path);
	bool save_cache(const std::string &path);

	/* -- finding systems by name anywhere ------------------------------------- */
	/* Systems whose name contains `query` (any case), nearest (cx, cy) first.
	   The generated names are a hash of the sector and number (l847f6), so a
	   copy of that hash picks candidates from every sector the galaxy bitmap
	   allows stars in; each candidate is then checked with the game's own
	   code before it is returned. Core sectors are not included (see
	   core_sectors). */
	std::vector<uint32_t> find_by_name(const std::string &query, double cx, double cy, int max_results,
									   long long *checked = nullptr);
	/* find_by_name in two steps, so the slow one can run on another thread:
	   name_candidates only reads copies of game data (safe on any thread),
	   check_names runs the game code (the thread that owns the Galaxy) */
	std::vector<uint32_t> name_candidates(const std::string &query, double cx, double cy, size_t keep,
										  long long *checked = nullptr) const;
	std::vector<uint32_t> check_names(const std::vector<uint32_t> &candidates, const std::string &query,
									  int max_results);
	/* a system's summary if it is known (worked out, from the cache, or in a
	   surveyed sector), without working it out; null if not */
	const Summary *known_summary(uint32_t id) const;
	/* the name the copy of the hash gives, to compare with the game's */
	std::string hashed_name(int x, int y, int n) const;

  private:
	std::unordered_map<uint32_t, Summary> summaries_;
	std::unordered_map<uint32_t, SectorSummary> sector_sums_;
	std::vector<std::string> syllables_; /* L84830 */
	std::vector<uint8_t> galaxy_bmp_;	 /* L8adc0_galaxy_bmp, with the bytes L8ad0e may read past its end */
	std::vector<uint32_t> core_keys_;	 /* x << 16 | y of the core sectors */

	std::unordered_map<uint32_t, Sector> sectors_;
	std::map<uint32_t, Details> details_;
	std::string err_;
	uint64_t use_ = 0;
	std::unordered_map<uint32_t, uint64_t> last_use_;
	std::unordered_map<int, std::string> descriptions_; /* by type word: the same for every star */

	Sector make_sector(int x, int y);
	bool select(const Star &s);
	Screen capture(uint32_t addr, uint32_t d0, const char *what, Details &d);
	int generate_bodies(const Star &s, bool *hangs); /* sysgen into A6_body_list; body count */
	static bool is_port(int kind);
	std::string kind_name(int kind);
	void read_picture(Picture &p);
	void read_orbit(Body &b);
	void read_stats(Body &b);
	std::unordered_map<int, std::string> kind_names_;
};

/* the colour of a star from its description ("red", "yellow"...) */
uint32_t star_colour(const std::string &description);

} // namespace galaxy
