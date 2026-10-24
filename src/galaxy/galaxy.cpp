/*
 * galaxy.cpp - see galaxy.h. Each function sets up registers the way the
 * galaxy map code sets them up and calls the routine that does the work.
 */
#include "galaxy.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "fe2vm.h"
#include "galaxy_labels.h"
#include "screen_font.h" /* font_bmp: the advance widths DrawStr uses */

namespace galaxy
{

/* The game's globals used here (offsets from A6, fe2/fe2_modded.s) */
enum
{
	A6_galmap_posx = 914, /* long, sector in the high word */
	A6_galmap_posy = 918,
	A6_galmap_flags = 934,	   /* bit 7: L8444a draws the stars too */
	A6_galmap_ref_pos = 936,   /* 3 longs: where distances are measured from */
	A6_galmap_sel_pos = 948,   /* 3 longs: the selected star, L849a2's input */
	A6_game_data = 14470,	   /* long: the game's models and tables */
	A6_body_list = 17348,	   /* the system's bodies, 64 bytes each (l84f02, l84cde) */
	A6_main_palette1 = 15876, /* 16 words $0RGB */
	BODY_SIZE = 64,
	BODY_MAX = 0x3c, /* l84cde takes keys $80..$bc */
	BODY_SURFACE_PORT = 0x46,
	A5_Put3DGamedata2Obj2 = 96,
	A6_icon_positions = 14514, /* click areas, 5 words each (L4231a) */
	BODY_PORT_FIRST = 0x40,
};

/* L83b46 (galaxy map module fn 32) modes */
enum
{
	SYS_POSITION = 2, /* d0 distance from A6_galmap_ref_pos, d1-d3 position, d6 type, d7 number */
};

/* L84700 string codes (the galaxy map module's strings, L823dc) */
enum
{
	STR_NAME = 0x3fe,		 /* name of system d1 */
	STR_STAR_TYPE = 0x110,	 /* + (type byte & $7e) */
	STR_MULTIPLE = 0x130,	 /* + 2 * companions */
	STR_DISTANCE = 0x13c,	 /* d1 = hundredths of a light year */
	STR_BODY_KIND = 0xec,	 /* + body kind */
};

static bool call(uint32_t addr, fe2vm_regs &r)
{
	return fe2vm_call(addr, &r) == 0;
}

static std::string read_string(uint32_t addr, int max = 256)
{
	std::string s;
	for (int i = 0; i < max; i++)
	{
		uint8_t c = fe2vm_rd8(addr + i);
		if (!c)
			break;
		s += (char)c;
	}
	return s;
}

/* The game's font puts its own symbols on some codes; plain text for the
 * tool (glyphs in src/screen_font.h) */
static void append_game_char(std::string &out, uint8_t c)
{
	switch (c)
	{
	case '^': out += "\xc2\xb0"; break;	 /* degree sign */
	case '{': out += "/h"; break;		 /* "km{" = km h^-1 */
	case '|': out += "/s"; break;		 /* "km|" = km s^-1 */
	case '&': out += "\xc3\xb6"; break;	 /* o umlaut */
	case '`': out += "\xc3\xa4"; break;	 /* a umlaut */
	case '[': out += "\xc3\xbc"; break;	 /* u umlaut */
	case ']': out += "\xc3\x9f"; break;	 /* sharp s */
	case '<': out += "\xc3\xb1"; break;	 /* n tilde */
	case '>': out += "\xc3\xa8"; break;	 /* e grave */
	case '@': out += "\xc2\xa9"; break;	 /* copyright */
	default:
		if (c >= 0x20 && c < 0x7f)
			out += (char)c;
		else if (c >= 0x7f)
			out += '?';
	}
}

std::string plain_text(const std::string &game)
{
	std::string s;
	for (size_t i = 0; i < game.size(); i++)
	{
		uint8_t c = (uint8_t)game[i];
		if (c == 1 && i + 1 < game.size())
			i++; /* colour change */
		else if (c == 0x1e && i + 1 < game.size())
			i++, s += ' '; /* tab to x */
		else if (c == 0x1f && i + 2 < game.size())
			i += 2, s += ' ';
		else if (c == '\r')
			s += '\n';
		else if (c < 0x20)
			;
		else
			append_game_char(s, c);
	}
	return s;
}

static uint32_t rgb_444(int c)
{
	int r = (c >> 8) & 15, g = (c >> 4) & 15, b = c & 15;
	return (uint32_t)(r * 17) << 16 | (uint32_t)(g * 17) << 8 | (uint32_t)(b * 17);
}

/* -- text capture: src/screen_text.c's DrawStr, keeping the text ----------- */

struct Capture
{
	Screen runs;
	int palette[16];
};

static int glyph_advance(int ch)
{
	int i = ch - 0x20;
	if (i < 0 || i >= (int)(sizeof(font_bmp) / 10))
		return 0;
	return font_bmp[i * 10 + 9];
}

static int capture_text(int xpos, int ypos, int col, const uint8_t *str, void *user)
{
	Capture &cap = *(Capture *)user;
	int x = xpos, y = ypos;
	TextRun run{x, y, (uint32_t)col, ""};
	auto flush = [&]() {
		if (!run.text.empty())
			cap.runs.push_back(run);
		run = TextRun{x, y, (uint32_t)col, ""};
	};
	for (; *str; str++)
	{
		int chr = *str;
		if (chr < 0x1e)
		{
			if (chr == '\r')
			{
				y += 10;
				x = xpos;
				flush();
			}
			else if (chr == 1 && str[1])
			{
				col = *++str;
				flush();
			}
			continue;
		}
		if (chr == 0x1e && str[1])
		{
			x = *++str * 2;
			flush();
			continue;
		}
		if (chr == 0x1f && str[1] && str[2])
		{
			x = *++str * 2;
			y = *++str;
			flush();
			continue;
		}
		append_game_char(run.text, (uint8_t)chr);
		x += glyph_advance(chr);
	}
	flush();
	return x;
}

Screen Galaxy::capture(uint32_t addr, uint32_t d0, const char *what, Details &d)
{
	Capture cap;
	fe2vm_regs r{};
	/* the galaxy map runs with the default palette (L83e26) */
	call(G_SetDefaultPalette, r);
	fe2vm_set_text_hook(capture_text, &cap);
	r = fe2vm_regs{};
	r.d[0] = d0;
	int result = fe2vm_call(addr, &r);
	fe2vm_set_text_hook(nullptr, nullptr);
	uint32_t a6 = fe2vm_a6();
	for (int i = 0; i < 16; i++)
		cap.palette[i] = fe2vm_rd16(a6 + A6_main_palette1 + 2 * i);
	for (auto &run : cap.runs)
		run.colour = rgb_444(run.colour < 16 ? cap.palette[run.colour] : 0xfff);
	if (result)
	{
		d.ok = false;
		d.hangs |= result == FE2VM_TIMEOUT;
		d.crashes |= result != FE2VM_TIMEOUT;
		d.error = std::string(what) + ": " + fe2vm_error();
	}
	return cap.runs;
}

/* -- setup ----------------------------------------------------------------- */

bool Galaxy::init(std::string &err)
{
	if (fe2vm_init())
	{
		err = err_ = std::string("starting the game code failed: ") + fe2vm_error();
		return false;
	}
	/* what the name search's copy of the name hash reads: the syllables
	   (L84830, 32 of 4 bytes), the galaxy bitmap L8ad0e reads (128 x 128,
	   plus the row after it reads for the corners) and the core sectors */
	for (int i = 0; i < 32; i++)
		syllables_.push_back(read_string(G_Syllables + 4 * i, 4));
	for (int i = 0; i < 128 * 128 + 130; i++)
		galaxy_bmp_.push_back(fe2vm_rd8(G_galaxy_bmp + i));
	for (auto &c : core_sectors())
		core_keys_.push_back((uint32_t)c.first << 16 | (uint32_t)c.second);
	return true;
}

/* -- the name hash, a copy of l847f6 (for the search only) ---------------------- */

static inline uint16_t rol16(uint16_t v, int n)
{
	n &= 15;
	return (uint16_t)(n ? (v << n) | (v >> (16 - n)) : v);
}

/* d3 after l847f6's mixing, from which L84820 takes the syllables */
static inline uint16_t name_hash(int x, int y, int n)
{
	uint16_t d3 = (uint16_t)x, d4 = (uint16_t)y;
	d3 += (uint16_t)n;		 /* add.w d5,d3 */
	d4 += d3;				 /* add.w d3,d4 */
	d3 = rol16(d3, 3);		 /* rol.w #3,d3 */
	d3 += d4;				 /* add.w d4,d3 */
	d4 = rol16(d4, 5);		 /* rol.w #5,d4 */
	d4 += d3;				 /* add.w d3,d4 */
	d4 = rol16(d4, 4);		 /* rol.w #4,d4 */
	d3 = rol16(d3, n & 63); /* rol.w d5,d3 (count mod 64, a word turns every 16) */
	d3 += d4;				 /* add.w d4,d3 */
	return d3;
}

static std::string hash_name(const std::vector<std::string> &syl, uint16_t d3)
{
	/* L84820 three times, ror.w #5 between; the first letter made capital */
	std::string s = syl[(d3 & 0x7c) >> 2];
	if (!s.empty())
		s[0] = (char)(s[0] - 0x20);
	d3 = rol16(d3, 11);
	s += syl[(d3 & 0x7c) >> 2];
	d3 = rol16(d3, 11);
	s += syl[(d3 & 0x7c) >> 2];
	return s;
}

std::string Galaxy::hashed_name(int x, int y, int n) const
{
	return hash_name(syllables_, name_hash(x, y, n));
}

/* -- sectors --------------------------------------------------------------- */

uint32_t star_colour(const std::string &d)
{
	struct
	{
		const char *word;
		uint32_t rgb;
	} const colours[] = {
		{"white dwarf", 0xe8f0ff}, {"brown", 0xa0603c}, {"red", 0xff6040}, {"orange", 0xffa040},
		{"yellow", 0xfff080},	   {"white", 0xf8f8ff}, {"blue", 0x80a8ff},
	};
	std::string low = d;
	for (auto &c : low)
		c = (char)tolower((unsigned char)c);
	size_t best = std::string::npos;
	uint32_t rgb = 0xc0c0c0;
	for (auto &c : colours)
	{
		size_t at = low.rfind(c.word); /* the main star comes last */
		if (at != std::string::npos && (best == std::string::npos || at > best))
			best = at, rgb = c.rgb;
	}
	return rgb;
}

Sector Galaxy::make_sector(int x, int y)
{
	Sector s;
	s.x = x;
	s.y = y;
	uint32_t a6 = fe2vm_a6();
	uint32_t scratch = fe2vm_scratch();
	uint32_t buf = scratch + 0x100; /* the 3D object L8444a fills: 506 bytes */
	uint32_t text = scratch + 0x400;

	/* A hand placed sector without stars ends at l846da, which reloads
	   a3-a6 from its caller's saved registers (movem.l 40(a7),a3-6: the
	   galaxy map's draw routine) and draws the sector's special object, if
	   it has one, through A5_Put3DGamedata2Obj2. Give it those registers,
	   with an a5 that sends that call to an rts (L6c_rts), so nothing is
	   drawn. 40(a7) there is 36 bytes past our stack top. */
	fe2vm_wr32(scratch + 36, buf);								  /* a3 */
	fe2vm_wr32(scratch + 40, 0);								  /* a4 */
	fe2vm_wr32(scratch + 44, G_rts - A5_Put3DGamedata2Obj2); /* a5 */
	fe2vm_wr32(scratch + 48, a6);								  /* a6 */

	/* L8444a_StarSystemsViewDrawSystems: the stars of the sector at
	   A6_galmap_posx/posy, without drawing them (as L83b46 calls it) */
	fe2vm_wr16(a6 + A6_galmap_posx, (uint16_t)x);
	fe2vm_wr16(a6 + A6_galmap_posy, (uint16_t)y);
	fe2vm_wr8(a6 + A6_galmap_flags, fe2vm_rd8(a6 + A6_galmap_flags) & 0x7f);
	fe2vm_regs r{};
	r.a[3] = buf;
	if (!call(G_StarSystemsViewDrawSystems, r))
	{
		err_ = fe2vm_error();
		return s;
	}
	s.special = core_special(x, y);
	int count = (int16_t)fe2vm_rd16(buf + 122) - 1;
	for (int n = 0; n < count && n < 64; n++)
	{
		Star st;
		st.id = make_id(x, y, n);
		/* L83d3c: position from the 3 bytes after the object type byte */
		r = fe2vm_regs{};
		r.a[1] = buf + 250 + 5 + 4 * n;
		if (!call(G_StarPosition, r))
			break;
		st.x = (int32_t)r.d[0];
		st.y = (int32_t)r.d[1];
		st.z = (int32_t)r.d[2];
		int word = fe2vm_rd16(buf + 124 + 2 * n);
		st.multiple = (word >> 8) & 7;
		st.type_byte = word & 0xff;
		st.type = (word & 0x7e) >> 1;

		/* the name (L84700 code $3fe) and the star line L849a2 builds */
		r = fe2vm_regs{};
		r.d[0] = STR_NAME;
		r.d[1] = st.id;
		r.a[0] = text;
		if (call(G_MapString, r))
			st.name = plain_text(read_string(text));
		auto known = descriptions_.find(word);
		if (known == descriptions_.end())
		{
			r = fe2vm_regs{};
			r.a[0] = text;
			fe2vm_wr8(text, 0);
			if (st.multiple)
			{
				r.d[0] = STR_MULTIPLE + 2 * st.multiple;
				call(G_MapString, r);
				r.a[0]--; /* append, as L849a2 does */
			}
			r.d[0] = STR_STAR_TYPE + (word & 0x7e);
			call(G_MapString, r);
			known = descriptions_.emplace(word, plain_text(read_string(text))).first;
		}
		st.description = known->second;
		st.colour = star_colour(st.description);

		/* L4beee (module 60(a6) fn 32), mode 0: d0 = byte 5, d7 = byte 10 */
		r = fe2vm_regs{};
		r.d[0] = 0;
		r.d[1] = st.id;
		if (call(G_SystemInfo, r))
		{
			st.explored = (r.d[0] & 0xff) != 0;
			st.info = r.d[7] & 0xff;
			st.allegiance = st.info >> 6;
		}
		s.stars.push_back(st);
	}
	return s;
}

const Sector &Galaxy::sector(int x, int y)
{
	uint32_t key = (uint32_t)y << 16 | (uint32_t)x;
	last_use_[key] = ++use_;
	auto it = sectors_.find(key);
	if (it != sectors_.end())
		return it->second;
	return sectors_[key] = make_sector(x, y);
}

bool Galaxy::cached(int x, int y) const
{
	return sectors_.count((uint32_t)y << 16 | (uint32_t)x) != 0;
}

void Galaxy::trim_cache(size_t max_sectors)
{
	if (sectors_.size() <= max_sectors)
		return;
	std::vector<std::pair<uint64_t, uint32_t>> order;
	for (auto &kv : sectors_)
		order.push_back({last_use_[kv.first], kv.first});
	std::sort(order.begin(), order.end());
	for (size_t i = 0; i + max_sectors < order.size(); i++)
	{
		sectors_.erase(order[i].second);
		last_use_.erase(order[i].second);
	}
}

const Star *Galaxy::star(uint32_t id)
{
	const Sector &s = sector(id_x(id), id_y(id));
	int n = id_n(id);
	return n < (int)s.stars.size() ? &s.stars[n] : nullptr;
}

int Galaxy::density(int x, int y, bool noise)
{
	/* L8ad0e: d2 >= $12 returns the smoothed galaxy bitmap, 0 adds the
	   per-sector noise L8444a uses */
	fe2vm_regs r{};
	r.d[0] = (uint32_t)x;
	r.d[1] = (uint32_t)y;
	r.d[2] = noise ? 0 : 0x12;
	if (!call(G_Density, r))
		return 0;
	return (int)(r.d[0] & 0xffff);
}

int Galaxy::star_count(int x, int y)
{
	int n = density(x, y, true) >> 10; /* L8444a: lsr.w #8 / lsr.w #2, at most 62 */
	return n >= 0x3f ? 62 : n;
}

/* -- distances ------------------------------------------------------------- */

uint32_t Galaxy::distance(const Star &a, const Star &b)
{
	/* L84ae6: d0-d2 from, d3-d5 to (as L849a2 calls it) */
	fe2vm_regs r{};
	r.d[0] = (uint32_t)a.x;
	r.d[1] = (uint32_t)a.y;
	r.d[2] = (uint32_t)a.z;
	r.d[3] = (uint32_t)b.x;
	r.d[4] = (uint32_t)b.y;
	r.d[5] = (uint32_t)b.z;
	if (!call(G_Distance, r))
		return 0;
	return r.d[0];
}

std::string Galaxy::map_string(int code, uint32_t d1, uint32_t d2, uint32_t d3)
{
	uint32_t text = fe2vm_scratch() + 0x400;
	fe2vm_wr8(text, 0);
	fe2vm_regs r{};
	r.d[0] = (uint32_t)code;
	r.d[1] = d1;
	r.d[2] = d2;
	r.d[3] = d3;
	r.a[0] = text;
	if (!call(G_MapString, r))
		return "";
	return plain_text(read_string(text));
}

std::string Galaxy::distance_text(uint32_t centi_ly)
{
	return map_string(STR_DISTANCE, centi_ly);
}

std::string Galaxy::game_string(int id)
{
	uint32_t text = fe2vm_scratch() + 0x400;
	fe2vm_wr8(text, 0);
	fe2vm_regs r{};
	r.d[0] = (uint32_t)id;
	r.a[0] = text;
	if (!call(G_GetFmtStr, r))
		return "";
	return plain_text(read_string(text));
}

/* L8444a looks a sector up in the 47 longs before
   L8460c_core_systems_sector (moveq #46,d1 / cmp.l -(a0),d0 / dbeq d1): the
   k-th long back leaves d1 = 47 - k, its index in L84736 and L84616 */
static const int CORE_SECTORS = 47;

std::vector<std::pair<int, int>> Galaxy::core_sectors()
{
	std::vector<std::pair<int, int>> out;
	for (int k = 1; k <= CORE_SECTORS; k++)
	{
		uint32_t v = fe2vm_rd32(G_core_systems_sector - 4 * k);
		out.push_back({(int)(v >> 16), (int)(v & 0xffff)});
	}
	return out;
}

int Galaxy::core_special(int x, int y)
{
	uint32_t want = (uint32_t)x << 16 | (uint32_t)y;
	for (int k = 1; k <= CORE_SECTORS; k++)
		if (fe2vm_rd32(G_core_systems_sector - 4 * k) == want)
			return (int16_t)fe2vm_rd16(G_core_specials + 2 * (CORE_SECTORS - k)); /* l846da: into 90(a3) */
	return 0;
}

double Galaxy::ly_per_sector()
{
	Star a, b;
	b.x = UNITS_PER_SECTOR;
	return distance(a, b) / 100.0;
}

/* -- the galaxy map's info screens ------------------------------------------ */

bool Galaxy::select(const Star &s)
{
	/* what the galaxy map does when a star is clicked: L849a2 with the
	   star's position in A6_galmap_sel_pos, d5 = number, d6 = type word.
	   Distances are measured from the star itself, so L849a2 skips the fuel
	   check (it would look at the player's ship, which there is none of). */
	uint32_t a6 = fe2vm_a6();
	fe2vm_regs r{};
	r.d[0] = SYS_POSITION;
	r.d[1] = s.id;
	if (!call(G_GetSystem, r) || (int32_t)r.d[0] == -1)
		return false;
	for (int i = 0; i < 3; i++)
	{
		fe2vm_wr32(a6 + A6_galmap_sel_pos + 4 * i, r.d[1 + i]);
		fe2vm_wr32(a6 + A6_galmap_ref_pos + 4 * i, r.d[1 + i]);
	}
	fe2vm_regs q{};
	q.d[5] = r.d[7];
	q.d[6] = r.d[6];
	return call(G_SelectSystem, q);
}

const Details &Galaxy::details(uint32_t id)
{
	auto it = details_.find(id);
	if (it != details_.end())
		return it->second;
	Details &d = details_[id];
	d.id = id;
	const Star *s = star(id);
	if (!s)
	{
		d.error = "no such system";
		return d;
	}
	Star st = *s;
	if (!select(st))
	{
		d.error = std::string("selecting the system failed: ") + fe2vm_error();
		return d;
	}
	d.ok = true;
	d.system = capture(G_ScreenSystem, 0, "system screen (l84c04)", d); /* key $f8 */
	d.trade = capture(G_ScreenTrade, 0, "trade screen (l84b34)", d);	/* key $f7 */
	/* key $f6: runs the system generator (sysgen module fn 32, L4655a) and
	   draws the system's icons */
	fe2vm_clear_screen();
	fe2vm_wr16(fe2vm_a6() + A6_icon_positions, 0xffff);
	d.bodies_screen = capture(G_ScreenBodies, 0, "system generator (l84f02)", d);
	if (d.hangs || d.crashes)
		return d; /* the body list is half made */
	read_picture(d.picture);

	/* the body list l84f02 left at A6_body_list */
	uint32_t list = fe2vm_a6() + A6_body_list;
	for (int n = 0; n <= BODY_MAX; n++)
	{
		uint32_t e = list + BODY_SIZE * n;
		if (fe2vm_rd32(e) == 0)
			break;
		Body b;
		b.index = n;
		b.parent = (int16_t)fe2vm_rd16(e + 8) - 1;
		b.kind = (int16_t)fe2vm_rd16(e + 10);
		b.surface = b.kind == BODY_SURFACE_PORT;
		b.port = is_port(b.kind);
		b.name = plain_text(read_string(e + 34, 30));
		for (int i = 0; i < 64; i++)
			b.raw[i] = fe2vm_rd8(e + i);
		d.bodies.push_back(b);
	}
	for (auto &b : d.bodies)
	{
		b.kind_name = kind_name(b.kind);
		if (b.surface) /* L84fa0 draws no icon for these, so they can't be clicked */
			continue;
		b.details = capture(G_ScreenBody, 0x80 + b.index, "body screen (l84cde)", d); /* keys $80.. */
		read_orbit(b);
		read_stats(b);
	}
	return d;
}

/* The body kinds that are starports: $40-$46, whose names (strings $ec +
   kind) are the game's starport kinds */
bool Galaxy::is_port(int kind)
{
	return kind >= BODY_PORT_FIRST && kind <= BODY_SURFACE_PORT;
}

std::string Galaxy::kind_name(int kind)
{
	auto it = kind_names_.find(kind);
	if (it != kind_names_.end())
		return it->second;
	/* l84e66 prints it after the body's name: L8498a, code $ec + kind. The
	   game never shows one for a surface starport, and that string is some
	   other text ("Binary system. Primary:"), so it gets a name here. */
	if (kind == BODY_SURFACE_PORT)
		return kind_names_[kind] = "Surface starport";
	return kind_names_[kind] = map_string(STR_BODY_KIND + kind);
}

void Galaxy::read_picture(Picture &p)
{
	const uint8_t *scr = fe2vm_screen();
	p.pixels.assign(scr, scr + 320 * 200);
	uint32_t a6 = fe2vm_a6();
	for (int i = 0; i < 16; i++)
		p.palette[i] = rgb_444(fe2vm_rd16(a6 + A6_main_palette1 + 2 * i));
	/* the click areas L4231a added: 5 words each, ended by a negative one */
	p.areas.clear();
	p.top = 200;
	p.bottom = 0;
	for (uint32_t e = a6 + A6_icon_positions; (int16_t)fe2vm_rd16(e) >= 0 && e < a6 + A6_icon_positions + 1200;
		 e += 10)
	{
		HitArea h{(int16_t)fe2vm_rd16(e), (int16_t)fe2vm_rd16(e + 2), (int16_t)fe2vm_rd16(e + 4),
				  (int16_t)fe2vm_rd16(e + 6), fe2vm_rd16(e + 8)};
		if (h.key < 0x80 || h.key > 0x80 + BODY_MAX)
			continue; /* the control panel's icons */
		p.areas.push_back(h);
		p.top = std::min(p.top, h.y0);
		p.bottom = std::max(p.bottom, h.y1);
	}
	if (p.areas.empty())
		p.top = p.bottom = 0;
}

/* the value printed right of a label on the body screen */
static const TextRun *value_of(const Screen &s, const char *label)
{
	for (auto &l : s)
		if (l.text == label)
		{
			const TextRun *best = nullptr;
			for (auto &v : s)
				if (v.y == l.y && v.x > l.x && (!best || v.x < best->x))
					best = &v;
			return best;
		}
	return nullptr;
}

/* Mass and temperature as the panel prints them; the radius the way L448b6
   gives an object one: the model (entry word 12) holds a mantissa (word 22
   of its game data record) and exponents (words 10 and 12), and the object
   gets (mantissa << 48) >> (0x38 - exponents) as a 16.32 fixed point number
   of position units (2^32 mm, 4294.967 km) */
void Galaxy::read_stats(Body &b)
{
	b.model = (uint16_t)((b.raw[12] << 8) | b.raw[13]) / 2;
	if (const TextRun *m = value_of(b.details, "Mass"))
		b.mass_earth = atof(m->text.c_str());
	for (auto &l : b.details)
		if (l.text.compare(0, 19, "Surface temperature") == 0)
		{
			size_t at = l.text.find_first_of("-0123456789", 19);
			if (at != std::string::npos)
				b.temperature_c = atoi(l.text.c_str() + at);
		}
	if (b.port)
		return;
	uint32_t data = fe2vm_rd32(fe2vm_a6() + A6_game_data);
	uint32_t rec = data + fe2vm_rd16(data + 2 * b.model);
	int mantissa = (int16_t)fe2vm_rd16(rec + 22);
	int shift = 0x37 - (int16_t)(fe2vm_rd16(rec + 10) + fe2vm_rd16(rec + 12));
	if (shift < 0 || shift > 62)
		return;
	b.radius_km = mantissa * 65536.0 / (double)(1ULL << (shift + 1)) * 4294.967296;
}

void Galaxy::read_orbit(Body &b)
{
	/* the game's own numbers: "0.387 A.U.", "0.206,6.9°", "87 days" */
	const TextRun *r = value_of(b.details, "Orbital radius");
	const TextRun *e = value_of(b.details, "Orbit Ecc. and Incl.");
	const TextRun *t = value_of(b.details, "Orbital period");
	if (!r || r->text.find("A.U.") == std::string::npos)
		return;
	b.au = atof(r->text.c_str());
	if (e)
	{
		b.eccentricity = atof(e->text.c_str());
		size_t comma = e->text.find(',');
		if (comma != std::string::npos)
			b.inclination = atof(e->text.c_str() + comma + 1);
	}
	if (t)
	{
		double v = atof(t->text.c_str());
		if (t->text.find("hour") != std::string::npos)
			v /= 24;
		else if (t->text.find("year") != std::string::npos)
			v *= 365.25;
		b.period_days = v;
	}
}

/* -- the starport overlay ---------------------------------------------------------- */

int Galaxy::generate_bodies(const Star &s, bool *hangs)
{
	/* exactly what the galaxy map does for its info icon: select the system
	   (L849a2), then l84f02, which runs the system generator (sysgen module
	   fn 32) into the body list and draws the icons */
	*hangs = false;
	uint32_t list = fe2vm_a6() + A6_body_list;
	fe2vm_wr32(list, 0);
	if (!select(s))
		return 0;
	fe2vm_regs r{};
	int result = fe2vm_call(G_ScreenBodies, &r);
	if (result)
	{
		*hangs = result == FE2VM_TIMEOUT;
		return -1;
	}
	int n = 0;
	while (n <= BODY_MAX && fe2vm_rd32(list + BODY_SIZE * n) != 0)
		n++;
	return n;
}

const Summary &Galaxy::summary(uint32_t id)
{
	auto it = summaries_.find(id);
	if (it != summaries_.end())
		return it->second;
	Summary &sum = summaries_[id];
	const Star *s = star(id);
	/* l84f02 shows "Unexplored System" and makes no bodies when byte 5 is 0 */
	if (!s || !s->explored)
		return sum;
	Star st = *s;
	bool hangs;
	if (getenv("FE2GALAXY_TRACE")) fprintf(stderr, "summary %08x %s\n", id, st.name.c_str());
	int n = generate_bodies(st, &hangs);
	sum.hangs = hangs;
	sum.crashes = n < 0 && !hangs;
	if (n < 0)
		return sum;
	sum.bodies = (uint8_t)n;
	uint32_t list = fe2vm_a6() + A6_body_list;
	for (int i = 0; i < n; i++)
	{
		int kind = (int16_t)fe2vm_rd16(list + BODY_SIZE * i + 10);
		if (kind == BODY_SURFACE_PORT)
			sum.surface_ports++;
		else if (is_port(kind))
			sum.orbital_ports++;
	}
	return sum;
}

bool Galaxy::has_sector_summary(int x, int y) const
{
	return sector_sums_.count((uint32_t)y << 16 | (uint32_t)x) != 0;
}

const SectorSummary &Galaxy::sector_summary(int x, int y)
{
	uint32_t key = (uint32_t)y << 16 | (uint32_t)x;
	auto it = sector_sums_.find(key);
	if (it != sector_sums_.end())
		return it->second;
	SectorSummary ss;
	std::vector<uint32_t> ids;
	for (auto &st : sector(x, y).stars)
		ids.push_back(st.id);
	for (uint32_t id : ids)
	{
		const Summary &sum = summary(id);
		if (sum.ports())
		{
			ss.inhabited++;
			ss.ports += sum.ports();
			ss.orbital += sum.orbital_ports;
		}
	}
	return sector_sums_[key] = ss;
}

/* FE2GALX1, the game binary's hash, then sector summaries (key, inhabited,
   ports, orbital) and the systems with starports (id, surface, orbital) */
static const char CACHE_MAGIC[8] = {'F', 'E', '2', 'G', 'A', 'L', 'X', '1'};

bool Galaxy::load_cache(const std::string &path)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	char magic[8];
	uint32_t hash = 0, sectors = 0, systems = 0;
	bool ok = fread(magic, 8, 1, f) == 1 && !memcmp(magic, CACHE_MAGIC, 8) && fread(&hash, 4, 1, f) == 1 &&
			  hash == fe2vm_game_hash() && fread(&sectors, 4, 1, f) == 1;
	for (uint32_t i = 0; ok && i < sectors; i++)
	{
		uint32_t key;
		SectorSummary ss;
		ok = fread(&key, 4, 1, f) == 1 && fread(&ss.inhabited, 2, 1, f) == 1 && fread(&ss.ports, 2, 1, f) == 1 &&
			 fread(&ss.orbital, 2, 1, f) == 1;
		if (ok)
			sector_sums_[key] = ss;
	}
	ok = ok && fread(&systems, 4, 1, f) == 1;
	for (uint32_t i = 0; ok && i < systems; i++)
	{
		uint32_t id;
		Summary s;
		ok = fread(&id, 4, 1, f) == 1 && fread(&s.surface_ports, 1, 1, f) == 1 &&
			 fread(&s.orbital_ports, 1, 1, f) == 1;
		if (ok)
			summaries_[id] = s;
	}
	fclose(f);
	if (!ok) /* another game build, or broken: start again */
	{
		sector_sums_.clear();
		summaries_.clear();
	}
	return ok;
}

bool Galaxy::save_cache(const std::string &path)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f)
		return false;
	uint32_t hash = fe2vm_game_hash(), n = (uint32_t)sector_sums_.size();
	fwrite(CACHE_MAGIC, 8, 1, f);
	fwrite(&hash, 4, 1, f);
	fwrite(&n, 4, 1, f);
	for (auto &kv : sector_sums_)
	{
		fwrite(&kv.first, 4, 1, f);
		fwrite(&kv.second.inhabited, 2, 1, f);
		fwrite(&kv.second.ports, 2, 1, f);
		fwrite(&kv.second.orbital, 2, 1, f);
	}
	/* only the systems with starports: a surveyed sector's others have none */
	n = 0;
	for (auto &kv : summaries_)
		n += kv.second.ports() > 0;
	fwrite(&n, 4, 1, f);
	for (auto &kv : summaries_)
		if (kv.second.ports() > 0)
		{
			fwrite(&kv.first, 4, 1, f);
			fwrite(&kv.second.surface_ports, 1, 1, f);
			fwrite(&kv.second.orbital_ports, 1, 1, f);
		}
	return fclose(f) == 0;
}

/* -- finding systems by name anywhere ---------------------------------------------- */

std::vector<uint32_t> Galaxy::name_candidates(const std::string &query, double cx, double cy, size_t keep,
											  long long *checked) const
{
	std::string q = query;
	for (auto &c : q)
		c = (char)tolower((unsigned char)c);
	std::vector<uint32_t> out;
	if (q.empty())
		return out;
	/* every value of the hash whose name matches */
	std::vector<uint8_t> match(65536);
	bool any = false;
	for (int v = 0; v < 65536; v++)
	{
		std::string s = hash_name(syllables_, (uint16_t)v);
		for (auto &c : s)
			c = (char)tolower((unsigned char)c);
		match[v] = s.find(q) != std::string::npos;
		any |= match[v] != 0;
	}
	if (!any)
		return out;

	/* Candidates, nearest first. A sector can hold no more stars than
	   L8ad0e's density allows: with d2 = 0 the count is the interpolated
	   bitmap value (at most the largest of the 4 bitmap bytes it reads,
	   times 256) times noise / 65536, over 1024; so a 64 x 64 sector bitmap
	   cell never has more than its largest corner / 4 stars. */
	struct Cand
	{
		double d2;
		uint32_t id;
		bool operator<(const Cand &o) const { return d2 < o.d2; }
	};
#ifdef __EMSCRIPTEN__
	unsigned threads = 1; /* no threads in the web build */
#else
	unsigned threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
#endif
	std::vector<std::vector<Cand>> heaps(threads);
	std::vector<long long> counts(threads);
	std::vector<std::thread> pool;
	auto work = [&](unsigned t) {
			auto &heap = heaps[t];
			long long seen = 0;
			for (int by = (int)t; by < 128; by += (int)threads)
				for (int bx = 0; bx < 128; bx++)
				{
					int i = by * 128 + bx;
					int m = std::max(std::max(galaxy_bmp_[i], galaxy_bmp_[i + 1]),
									 std::max(galaxy_bmp_[i + 128], galaxy_bmp_[i + 129]));
					int bound = std::min(62, m >> 2);
					for (int y = by * 64; y < by * 64 + 64; y++)
						for (int x = bx * 64; x < bx * 64 + 64; x++)
							for (int n = 0; n < bound; n++)
							{
								seen++;
								if (!match[name_hash(x, y, n)])
									continue;
								double dx = x + 0.5 - cx, dy = y + 0.5 - cy, d2 = dx * dx + dy * dy;
								if (heap.size() < keep)
								{
									heap.push_back({d2, make_id(x, y, n)});
									std::push_heap(heap.begin(), heap.end());
								}
								else if (d2 < heap.front().d2)
								{
									std::pop_heap(heap.begin(), heap.end());
									heap.back() = {d2, make_id(x, y, n)};
									std::push_heap(heap.begin(), heap.end());
								}
							}
				}
			counts[t] = seen;
	};
	if (threads == 1)
		work(0);
	else
	{
		for (unsigned t = 0; t < threads; t++)
			pool.emplace_back(work, t);
		for (auto &th : pool)
			th.join();
	}
	std::vector<Cand> all;
	long long seen = 0;
	for (unsigned t = 0; t < threads; t++)
	{
		all.insert(all.end(), heaps[t].begin(), heaps[t].end());
		seen += counts[t];
	}
	if (checked)
		*checked = seen;
	std::sort(all.begin(), all.end());
	for (auto &c : all)
	{
		uint32_t key = (uint32_t)id_x(c.id) << 16 | (uint32_t)id_y(c.id);
		if (std::find(core_keys_.begin(), core_keys_.end(), key) == core_keys_.end())
			out.push_back(c.id); /* core sectors' names come from a table */
	}
	return out;
}

std::vector<uint32_t> Galaxy::check_names(const std::vector<uint32_t> &candidates, const std::string &query,
										  int max_results)
{
	/* the game has the last word: the system must exist and have that name */
	std::string q = query;
	for (auto &c : q)
		c = (char)tolower((unsigned char)c);
	std::vector<uint32_t> out;
	for (uint32_t id : candidates)
	{
		if ((int)out.size() >= max_results)
			break;
		const Star *s = star(id);
		if (!s)
			continue;
		std::string name = s->name;
		for (auto &ch : name)
			ch = (char)tolower((unsigned char)ch);
		if (name.find(q) != std::string::npos)
			out.push_back(id);
	}
	return out;
}

std::vector<uint32_t> Galaxy::find_by_name(const std::string &query, double cx, double cy, int max_results,
										   long long *checked)
{
	/* some candidates fail the game's check (too few stars in the sector) */
	return check_names(name_candidates(query, cx, cy, (size_t)max_results * 8 + 64, checked), query, max_results);
}

const Summary *Galaxy::known_summary(uint32_t id) const
{
	auto it = summaries_.find(id);
	if (it != summaries_.end())
		return &it->second;
	/* the cache keeps only the systems with starports */
	static const Summary none;
	return has_sector_summary(id_x(id), id_y(id)) ? &none : nullptr;
}

} // namespace galaxy
