/*
 * gl_native.cpp - see gl_native.h.
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

extern "C"
{
#include "frame_prof.h"
#include <SDL.h>
}

extern "C"
{
#include "gl_cockpit.h"
#include "gl_draw.h"
#include "gl_hud.h"
#include "gl_native.h"
#include "gl_planet.h"
#include "gl_scene.h"
#include "gl_shadow.h"
#include "game_state.h"
#include "host.h"
#include "main.h"
#include "scenery.h"
}

#include "fe2core.h"

/* ---- the game's models, read from its own memory --------------------------- */

/* fe2core's image is the game binary as loaded: at address 0 of emulated RAM.
 * Models don't change while the game runs. */
static void set_table(fe2::Image &img, uint32_t table)
{
	img.table = table;
	int first = 0x10000;
	for (int i = 0; i * 2 < first; i++)
	{
		int o = img.u16(img.table + i * 2);
		if (o)
			first = std::min(first, o);
	}
	img.num_models = first / 2;
}

const fe2::Image &fe2::Image::game()
{
	static Image img;
	static std::once_flag once;
	std::call_once(once, [] {
		img.data.assign((const uint8_t *)m68kram, (const uint8_t *)m68kram + MEM_SIZE);
		img.gamedata2 = FE2_gamedata2;
		img.core_strings = FE2_L3e894;
		set_table(img, FE2_gamedata1);
	});
	return img;
}

/* The game's other model tables (the intro's has the hangar and the launch
 * lift): the same image, another table. Only used on the render thread. */
static const fe2::Image &image_for(uint32_t table)
{
	if (table == FE2_gamedata1)
		return fe2::Image::game();
	static std::unordered_map<uint32_t, std::unique_ptr<fe2::Image>> others;
	auto &slot = others[table];
	if (!slot)
	{
		slot.reset(new fe2::Image(fe2::Image::game()));
		set_table(*slot, table);
	}
	return *slot;
}

namespace
{

using P3 = std::array<float, 3>;

/* ---- models: their bytecode, with each instruction's geometry ready ---------- */

/* A face, triangulated, in the model's own units (its vertex bytes) */
struct Face
{
	int colour;
	int nidx;        /* the game's normal number, 0 = none */
	bool has_normal; /* else drawn from both sides */
	P3 n;            /* unit */
	std::vector<P3> tris;    /* 3 per triangle */
	std::vector<P3> outline; /* for the wireframe */
	std::vector<std::vector<P3>> more; /* ...a complex polygon's other loops */
};

struct Line
{
	int colour;
	P3 a, b;
};

struct Code
{
	fe2::Model model;
	std::vector<fe2::Instr> ins;
	std::unordered_map<int, size_t> at; /* byte offset -> instruction */
	std::vector<std::vector<Face>> faces; /* by instruction */
	std::vector<std::vector<Line>> lines;
	/* by instruction: its geometry depends on the object (variables,
	 * temporaries, random offsets, the parent's vertices), so it is worked
	 * out each time it is drawn */
	std::vector<bool> dyn;
	bool planet = false; /* drawn by the planet code */
};

P3 to_p3(const fe2::Vec3 &v)
{
	return {(float)v.x, (float)v.y, (float)v.z};
}

float dot(const P3 &a, const P3 &b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* Loops in the plane with normal n filled as the game's scan fill does a
 * complex polygon's (even-odd: an inner loop is a hole, as in a letter's
 * B or a station's ring): trapezoids between the heights of every vertex
 * and edge crossing, as triangles facing along n */
std::vector<P3> even_odd(const std::vector<std::vector<fe2::Vec3>> &loops, const fe2::Vec3 &n)
{
	using fe2::Vec3;
	Vec3 axis = std::fabs(n.x) < std::fabs(n.y) ? (std::fabs(n.x) < std::fabs(n.z) ? Vec3(1, 0, 0) : Vec3(0, 0, 1))
												: (std::fabs(n.y) < std::fabs(n.z) ? Vec3(0, 1, 0) : Vec3(0, 0, 1));
	Vec3 u = fe2::normalize(fe2::cross(n, axis)), v = fe2::cross(n, u);
	struct Edge
	{
		double x0, y0, x1, y1; /* y0 < y1 */
		double x_at(double y) const { return x0 + (x1 - x0) * (y - y0) / (y1 - y0); }
	};
	std::vector<Edge> edges;
	std::vector<double> ys;
	double h = 0;
	int count = 0;
	for (const auto &lp : loops)
		for (size_t i = 0; i < lp.size(); i++)
		{
			const Vec3 &a = lp[i], &b = lp[(i + 1) % lp.size()];
			h += fe2::dot(a, n);
			count++;
			double ax = fe2::dot(a, u), ay = fe2::dot(a, v), bx = fe2::dot(b, u), by = fe2::dot(b, v);
			ys.push_back(ay);
			if (ay == by)
				continue;
			edges.push_back(ay < by ? Edge{ax, ay, bx, by} : Edge{bx, by, ax, ay});
		}
	if (count == 0)
		return {};
	h /= count;
	/* where edges cross */
	for (size_t i = 0; i < edges.size(); i++)
		for (size_t j = i + 1; j < edges.size(); j++)
		{
			const Edge &a = edges[i], &b = edges[j];
			double lo = std::max(a.y0, b.y0), hi = std::min(a.y1, b.y1);
			if (hi <= lo)
				continue;
			double d0 = a.x_at(lo) - b.x_at(lo), d1 = a.x_at(hi) - b.x_at(hi);
			if ((d0 < 0 && d1 > 0) || (d0 > 0 && d1 < 0))
				ys.push_back(lo + (hi - lo) * d0 / (d0 - d1));
		}
	std::sort(ys.begin(), ys.end());
	auto at = [&](double x, double y) {
		Vec3 p = u * x + v * y + n * h;
		return P3{(float)p.x, (float)p.y, (float)p.z};
	};
	std::vector<P3> tris;
	std::vector<std::pair<double, const Edge *>> across;
	const double eps = 1e-9;
	for (size_t k = 0; k + 1 < ys.size(); k++)
	{
		double y0 = ys[k], y1 = ys[k + 1];
		if (y1 - y0 < eps)
			continue;
		double ym = (y0 + y1) / 2;
		across.clear();
		for (const Edge &e : edges)
			if (e.y0 <= ym && e.y1 >= ym)
				across.push_back({e.x_at(ym), &e});
		std::sort(across.begin(), across.end(),
				  [](const std::pair<double, const Edge *> &a, const std::pair<double, const Edge *> &b) {
					  return a.first < b.first;
				  });
		for (size_t i = 0; i + 1 < across.size(); i += 2)
		{
			const Edge &l = *across[i].second, &r = *across[i + 1].second;
			P3 q[4] = {at(l.x_at(y0), y0), at(r.x_at(y0), y0), at(r.x_at(y1), y1), at(l.x_at(y1), y1)};
			/* u, v, n is right handed: counterclockwise in (x, y) faces n */
			for (int t : {1, 2})
			{
				tris.push_back(q[0]);
				tris.push_back(q[t]);
				tris.push_back(q[t + 1]);
			}
		}
	}
	return tris;
}

/* An instruction's faces and lines, in the model's own units */
void build_prims(const fe2::Model &m, const fe2::Instr &ins, const fe2::Model::ParentFn &parent,
				 const fe2::VertexEnv *env, std::vector<Face> &faces, std::vector<Line> &lines)
{
	auto prims = fe2::primitives(m, {ins}, parent, env);
	/* a complex polygon of several loops: one face, filled even-odd */
	std::vector<std::vector<fe2::Vec3>> loops;
	for (const fe2::Prim &p : prims)
		if (p.kind == fe2::Prim::Face && p.pts.size() >= 3)
			loops.push_back(p.pts);
	if (ins.op == 0x05 && loops.size() > 1)
	{
		const fe2::Prim &p = prims.front();
		Face f;
		f.colour = p.colour;
		f.nidx = p.normal ? p.nidx : 0;
		f.has_normal = p.normal.has_value();
		fe2::Vec3 n = fe2::normalize(p.normal ? *p.normal : fe2::newell(p.pts));
		f.n = to_p3(n);
		f.tris = even_odd(loops, n);
		for (auto &v : loops[0])
			f.outline.push_back(to_p3(v));
		for (size_t i = 1; i < loops.size(); i++)
		{
			f.more.emplace_back();
			for (auto &v : loops[i])
				f.more.back().push_back(to_p3(v));
		}
		faces.push_back(std::move(f));
		return;
	}
	for (const fe2::Prim &p : prims)
	{
		if (p.kind == fe2::Prim::Face && p.pts.size() >= 3)
		{
			Face f;
			f.colour = p.colour;
			f.nidx = p.normal ? p.nidx : 0;
			f.has_normal = p.normal.has_value();
			fe2::Vec3 n = fe2::normalize(p.normal ? *p.normal : fe2::newell(p.pts));
			f.n = to_p3(n);
			for (auto &t : fe2::triangulate(p.pts, n))
				for (int v : t)
					f.tris.push_back(to_p3(p.pts[v]));
			for (auto &v : p.pts)
				f.outline.push_back(to_p3(v));
			faces.push_back(std::move(f));
		}
		else if (p.kind == fe2::Prim::Line)
		{
			for (size_t j = 0; j + 1 < p.pts.size(); j++)
				lines.push_back({p.colour, to_p3(p.pts[j]), to_p3(p.pts[j + 1])});
		}
	}
}

std::unordered_map<uint64_t, std::unique_ptr<Code>> codes;

/* Model `index` of the model table at `table`, or null; with `at`, the
 * code there run with that model's vertices (a font's glyph) */
const Code *code_for(uint32_t table, int index, uint32_t at = 0)
{
	uint64_t key = at ? 1ull << 63 | (uint64_t)at << 16 | (uint32_t)index : (uint64_t)table << 32 | (uint32_t)index;
	auto it = codes.find(key);
	if (it != codes.end())
		return it->second.get();
	std::unique_ptr<Code> &slot = codes[key];
	const fe2::Image &img = image_for(table);
	auto model = img.model(index);
	if (!model)
		return nullptr;
	bool ok = true;
	auto ins = fe2::decode(img.data, at ? at : model->code_addr(), 0x8000, &ok);
	if (!ok || ins.empty())
		return nullptr;
	slot.reset(new Code);
	Code &c = *slot;
	c.model = *model;
	c.ins = std::move(ins);
	c.faces.resize(c.ins.size());
	c.lines.resize(c.ins.size());
	c.dyn.resize(c.ins.size());
	/* vertex -1, the point the parent called it at, is its origin; the
	 * parent's other vertices, variables and random offsets make an
	 * instruction dynamic */
	bool dyn = false;
	fe2::Model::ParentFn origin = [&](int i) -> std::optional<fe2::Vec3> {
		if (i != -1)
			dyn = true;
		return fe2::Vec3();
	};
	fe2::VertexEnv probe;
	probe.param = [&](int) {
		dyn = true;
		return 0x8000;
	};
	probe.jitter = [&](int, int) {
		dyn = true;
		return fe2::Vec3();
	};
	for (size_t i = 0; i < c.ins.size(); i++)
	{
		c.at[c.ins[i].off] = i;
		if (c.ins[i].op == 0x1f)
			c.planet = true;
		dyn = false;
		build_prims(c.model, c.ins[i], origin, &probe, c.faces[i], c.lines[i]);
		c.dyn[i] = dyn;
	}
	return &c;
}

/* ---- captured objects -------------------------------------------------------- */

constexpr int NUM_VARS = 128;
constexpr int SKY_MODEL = 0xb0 / 2;

/* An object as the game's own camera sees it, GL view space (z flipped) */
struct Object
{
	uint32_t addr;
	uint32_t table; /* the model table it was drawn from (L973c_game_data, or
	                   L2af80_gamedata2 through L3941a: galaxy map, tombstone) */
	uint32_t subtable; /* sub-models' table: always L973c_game_data's */
	int pass;       /* the scene pass it was captured in */
	int model;
	int scale;     /* s: view = pos + R v 2^s */
	float pos[3];  /* view units */
	float shrink;  /* native_frame_shrink: true distance = |pos| * shrink */
	float cols[3][3]; /* R's columns: the model's axes */
	float light[3];   /* away from the light, unit */
	uint16_t tint[8]; /* the light's colour by how square a face is to it */
	uint16_t vars[NUM_VARS]; /* the object's model variables (words at +114) */
	int detail[2];           /* L60d2_optdetail1, L60d4_optdetail2 */
	/* The temporaries the model starts with (-64(a6)): t0 is cleared, the
	 * others are what is left there, which models like the cities use */
	uint16_t t[8];
	bool own_added = false; /* the player's ship, put in for the ship shadows only (cockpit_own_ship_added) */
};

/* A body too far away for its model, which L390f2 draws as a disc instead
 * (l39092): a sun, a far planet, a far ship's dot. GL view space. */
struct Far
{
	uint32_t addr;
	float pos[3];
	float radius; /* the model's (word at +14) * 2^(s+1), view units */
	float shrink; /* native_frame_shrink: true distance = |pos| * shrink */
	int colour;   /* the model's word at +18: bit 15 twinkly, then 444 + flags */
	float light[3];
	uint16_t tint[8];
	bool planet; /* a planet's model: the game runs it whole (Nu_NativeFar), the disc only stands in when tiny */
};

/* The orbit in the system map (map_orbit_build): points in the map's view
 * space (GL), drawn over the scene; hid: the planet is between them and the eye */
constexpr int MAP_ORBIT_PTS = 400;
struct MapOrbit
{
	int n = 0;
	float pt[MAP_ORBIT_PTS][3];
	bool hid[MAP_ORBIT_PTS];
};

constexpr int MAX_OBJECTS = 256;
constexpr int MAX_FAR = 64;
struct Frame
{
	Object obj[MAX_OBJECTS];
	int n = 0;
	Far far[MAX_FAR];
	int nfar = 0;
	/* The sky (stars, the Milky Way: L44200_DrawBGStars) */
	Object sky;
	bool has_sky = false;
	/* The system map's camera (the matrix at 1082(a6), 1.15 words) while it
	 * was drawn, and the orbit worked out from it */
	bool cam_ok = false;
	int16_t cam[9];
	/* The ship in the map's own copy of the system (17348(a6)), read while
	 * the map is drawn: outside that the memory is the flight's again */
	uint32_t map_ship = 0;
	int map_par = 0;
	double map_r[3];
	MapOrbit orbit;
	/* Model text's strings by text_key (Nu_NativeText): the last the game
	 * formatted for each, kept while it isn't run (off screen) */
	std::unordered_map<uint64_t, std::string> texts;
	/* Which publish it is (native_publish*): its models run once, not once a
	 * picture, however often it is drawn (prepare) */
	uint32_t serial = 0;
};
uint32_t publishes = 0;
/* shows[0]: the world, shows[1]: the last frame of the game's other
 * screens, which leaves the world as it was (native_acquire) */
Frame rec, pub, pub_world, pub_page, shows[3];
Frame *shown = &shows[0];
bool screen_only = false; /* drawing shows[1]: its own planets, no haze */

int enabled = -1;

/* Model text (op $0a) by object and string id */
uint64_t text_key(uint32_t addr, int sid)
{
	return (uint64_t)addr << 16 | (uint16_t)sid;
}
/* Game thread: every string seen, published with each frame */
std::unordered_map<uint64_t, std::string> texts_seen;

} // namespace

extern "C" bool native_enabled(void)
{
	if (enabled < 0)
	{
		const char *e = getenv("GLF_NATIVE_SCENE");
		enabled = e ? std::max(0, atoi(e)) : 2;
	}
	return enabled != 0;
}

extern "C" int native_mode(void)
{
	return native_enabled() ? enabled : 0;
}

/* The model table of the object at addr: L3941a_Put3DGamedata2Obj2 enters
 * L39426 with gamedata2's, else it is L973c_game_data's. a5 = its model
 * (the table plus the model's offset) at the hooks. */
static uint32_t table_of(uint32_t addr)
{
	uint32_t a5 = (uint32_t)GetReg(REG_A5);
	uint16_t w = (uint16_t)rdword(addr + 90);
	if (a5 == (uint32_t)FE2_gamedata2 + (uint16_t)rdword(FE2_gamedata2 + w))
		return FE2_gamedata2;
	return (uint32_t)rdlong(FE2_game_data);
}

/* The object L390f2 or L39426 is at is to be captured */
static bool wanted(uint32_t a6, uint32_t &addr, uint32_t &table, int &model)
{
	if (!native_enabled() || !scene_active())
		return false;
	addr = (uint32_t)rdlong(a6 - 212);
	table = table_of(addr);
	model = (uint16_t)rdword(addr + 90) / 2;
	if (table == FE2_gamedata1 && model == SKY_MODEL)
		return false; /* the sky, drawn by L39426 too */
	/* Already captured from another pass. Objects made up on the stack
	 * (the hangar's) share addresses, but within one pass. */
	int pass = scene_cur_pass();
	uint32_t eff = scenery_virtual_id() ? 0xf0000000u + scenery_virtual_id() : addr;
	for (int i = 0; i < rec.n; i++)
		if (rec.obj[i].addr == eff && rec.obj[i].model == model && rec.obj[i].table == table &&
			rec.obj[i].pass != pass)
			return false;
	return rec.n < MAX_OBJECTS;
}

/* The far body Nu_NativeFar last saw (L390f2's frame, its object) */
static uint32_t far_a6, far_addr;

int native_detail_boost = 2;
bool native_cloud_shadows = true;
/* Every far body's radius, by object (game thread): a planet's size before
 * its planet has been seen close up (native_capture_dirs) */
static std::unordered_map<uint32_t, float> far_radius;

/* Host call (Lnative_clip in fe2.s): the game culls this object as off
 * screen; d0 = 1 to have its matrix worked out and capture it anyway */
extern "C" void Nu_NativeCulled(void)
{
	scene_mute_object(0);
	uint32_t a6 = (uint32_t)GetReg(REG_A6);
	uint32_t addr, table;
	int model;
	bool want = wanted(a6, addr, table, model) && !(a6 == far_a6 && addr == far_addr);
	SetReg(REG_D0, want ? 1 : 0);
}

/* The light vector (L60e2_lighting_vector or the object's own frame's) and
 * the tint table (L60f6_light_tint_table) as L390f2 set them up */
static void capture_light(uint32_t a6, float light[3], uint16_t tint[8])
{
	float l[3] = {(float)(int16_t)rdword(a6 - 198), (float)(int16_t)rdword(a6 - 196),
				  -(float)(int16_t)rdword(a6 - 194)};
	cockpit_pass_to_view(l, light);
	float len = std::sqrt(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
	for (int j = 0; j < 3; j++)
		light[j] = len > 0 ? light[j] / len : 0;
	for (int j = 0; j < 8; j++)
		tint[j] = (uint16_t)rdword(a6 - 104 + 2 * (uint32_t)j);
}

/* The object at a6 (L390f2's or L39426's frame), as the game's own camera
 * sees it */
static void capture(Object &o, uint32_t a6, uint32_t addr, uint32_t table, int model)
{
	int pass = scene_cur_pass();
	/* the host's scenery copies share one buffer: each has an address of its own */
	o.addr = scenery_virtual_id() ? 0xf0000000u + scenery_virtual_id() : addr;
	o.table = table;
	o.subtable = (uint32_t)rdlong(FE2_game_data);
	o.pass = pass;
	o.model = model;
	o.scale = (int16_t)rdword(a6 - 208);
	/* L39426 sets s from the model itself: never shrunk */
	o.shrink = table == o.subtable ? native_frame_shrink(a6) : 1.0f;
	/* the game's view space -> GL's (z flipped) -> the game's own camera
	 * (from a turned pass) */
	float p[3] = {(float)rdlong(a6 - 16), (float)rdlong(a6 - 12), -(float)rdlong(a6 - 8)};
	cockpit_pass_to_view(p, o.pos);
	for (int c = 0; c < 3; c++)
	{
		uint32_t m = a6 - 36 + 6 * (uint32_t)c;
		float col[3] = {(int16_t)rdword(m) / 32768.0f, (int16_t)rdword(m + 2) / 32768.0f,
						-(int16_t)rdword(m + 4) / 32768.0f};
		cockpit_pass_to_view(col, o.cols[c]);
	}
	capture_light(a6, o.light, o.tint);
	o.own_added = cockpit_own_ship_added() && addr == game_player_ship();
	for (int j = 0; j < NUM_VARS; j++)
		o.vars[j] = (uint16_t)rdword(addr + 114 + 2 * (uint32_t)j);
	for (int j = 0; j < 8; j++)
		o.t[j] = j == 0 ? 0 : (uint16_t)rdword(a6 - 64 + 2 * (uint32_t)j);
	o.detail[0] = (int16_t)rdword(FE2_optdetail1);
	o.detail[1] = (int16_t)rdword(FE2_optdetail2);
}

extern "C" float native_frame_shrink(uint32_t a6)
{
	uint32_t table = (uint32_t)rdlong(FE2_game_data);
	uint32_t addr = (uint32_t)rdlong(a6 - 212);
	uint32_t m = table + (uint16_t)rdword(table + (uint16_t)rdword(addr + 90));
	int e = (int16_t)rdword(m + 10) - (int16_t)rdword(a6 - 208);
	return e > 0 ? std::ldexp(1.0f, e) : 1.0f;
}

/* The object's view position and axes without the game's rounding. L390f2
 * cuts the camera-to-object vector down to 14 bits before turning it
 * (L3901a, then l39140/l39174) and rounds the axes' product to 16 bits
 * (L39290): for a model whose middle is far off (a city 40 km across) the
 * parts near the camera jump by up to a metre as the camera moves or
 * turns. Worked out again here in doubles from the same 64-bit positions
 * and matrices; only where the camera and the object share a frame (the
 * other cases go through the parent chain, L38fb4, and keep the game's). */
struct Exact
{
	uint32_t a6 = 0, obj = 0;
	bool pos_ok = false;
	double pos[3];   /* as -16(a6), unrounded */
	bool cam_axes = false; /* L39290 copies the camera's matrix: nothing rounded */
};
static Exact exact;

static double obj_coord(uint32_t o, int k)
{
	return (double)(int32_t)rdlong(o + 56 + 4 * (uint32_t)k) * 4294967296.0 + (double)(uint32_t)rdlong(o + 44 + 4 * (uint32_t)k);
}

/* At Lnative_clip: a4 the object, a3 the camera, a0 just past the matrix
 * L390f2 turned the vector with, d7 the distance exponent, a5 the model */
static void exact_note(uint32_t a6)
{
	exact = Exact();
	static int on = -1;
	if (on < 0)
		on = !getenv("GLF_EXACT_POSE") || atoi(getenv("GLF_EXACT_POSE")) != 0;
	if (!on)
		return;
	uint32_t a3 = (uint32_t)GetReg(REG_A3), a4 = (uint32_t)GetReg(REG_A4), mat = (uint32_t)GetReg(REG_A0) - 6;
	uint32_t m = (uint32_t)GetReg(REG_A5);
	exact.a6 = a6;
	exact.obj = a4;
	uint8_t cpar = (uint8_t)rdbyte(a3 + 68);
	exact.cam_axes = rdbyte(a3 + 69) && !rdbyte(a4 + 69) && cpar == (uint8_t)rdbyte(a4 + 93);
	double rel[3];
	if (cpar == (uint8_t)rdbyte(a4 + 68) && rdbyte(a4 + 69) == rdbyte(a3 + 69))
		for (int k = 0; k < 3; k++)
			rel[k] = obj_coord(a4, k) - obj_coord(a3, k);
	else if (cpar == (uint8_t)rdbyte(a4 + 93))
		for (int k = 0; k < 3; k++)
			rel[k] = -obj_coord(a3, k);
	else
		return;
	/* pos = M rel 2^(16 - S - d0): S the cut, d7 = S - 88(a4), d0 = 16 - d7 + 12(a5), at least 0 */
	int d7 = (int16_t)GetReg(REG_D7);
	int S = d7 + (int16_t)rdword(a4 + 88);
	int d0 = std::max(0, 16 - d7 + (int16_t)rdword(m + 12));
	double f = std::ldexp(1.0, 16 - S - d0) / 32768.0;
	for (int i = 0; i < 3; i++)
	{
		double s = 0, w = 0;
		for (int k = 0; k < 3; k++)
		{
			int16_t mk = (int16_t)rdword(mat + 2 * (uint32_t)(i + 3 * k));
			s += mk * rel[k];
			w += std::abs(mk);
		}
		exact.pos[i] = s * f;
		/* the game's is out by up to a unit of each cut component (w 2^(1-d0)):
		 * further off, a case not followed, keep the game's */
		double g = (double)(int32_t)rdlong(a6 - 16 + 4 * (uint32_t)i);
		if (std::fabs(exact.pos[i] - g) > 4 + 2 * w * std::ldexp(2.0, -d0))
			return;
	}
	exact.pos_ok = true;
}

/* At Lnative_object (L393de), into the captured object: a3 and a4 are the
 * matrices L39290 multiplied (unless it copied the camera's) */
static void exact_apply(uint32_t a6, Object &o)
{
	if (exact.a6 != a6 || exact.obj != (uint32_t)rdlong(a6 - 212))
		return;
	if (exact.pos_ok)
	{
		float p[3] = {(float)exact.pos[0], (float)exact.pos[1], -(float)exact.pos[2]};
		cockpit_pass_to_view(p, o.pos);
	}
	if (exact.cam_axes)
		return;
	uint32_t A = (uint32_t)GetReg(REG_A3), B = (uint32_t)GetReg(REG_A4);
	float cols[3][3];
	for (int j = 0; j < 3; j++)
		for (int i = 0; i < 3; i++)
		{
			double s = 0;
			for (int k = 0; k < 3; k++)
				s += (double)(int16_t)rdword(A + 2 * (uint32_t)(i + 3 * k)) *
					 (int16_t)rdword(B + 2 * (uint32_t)(j + 3 * k));
			cols[j][i] = (float)(s / 1073741824.0);
			double g = (int16_t)rdword(a6 - 36 + 6 * (uint32_t)j + 2 * (uint32_t)i) / 32768.0;
			if (std::fabs(cols[j][i] - g) > 4.0 / 32768)
				return;
		}
	for (int c = 0; c < 3; c++)
	{
		float col[3] = {cols[c][0], cols[c][1], -cols[c][2]};
		cockpit_pass_to_view(col, o.cols[c]);
	}
}

/* Host call (Lnative_clip, before the cull): is the object too far for its
 * model (d7, its distance's exponent, >= the model's word at +20), so that
 * L390f2 would draw it as a disc (l39092)? Kept as a far body if so; d0 = 1
 * when the native scene draws it instead of the game. */
extern "C" void Nu_NativeFar(void)
{
	scene_mute_object(0);
	SetReg(REG_D0, 0);
	far_a6 = 0;
	if (!native_enabled() || !scene_active())
		return;
	exact_note((uint32_t)GetReg(REG_A6));
	uint32_t a6 = (uint32_t)GetReg(REG_A6);
	uint32_t m = (uint32_t)GetReg(REG_A5);
	if ((int16_t)GetReg(REG_D7) < (int16_t)rdword(m + 20) || (int32_t)rdlong(a6 - 8) < 0x40)
		return;
	uint32_t addr = (uint32_t)rdlong(a6 - 212);
	/* A planet is not turned into a disc: the game runs its model (the
	 * planet command) at any distance, with d7 under the disc limit for
	 * the compare after this hook. A twinkly one (a sun) stays a disc. */
	const Code *pc = code_for(table_of(addr), (uint16_t)rdword(addr + 90) / 2);
	bool planet = pc && pc->planet && !((uint16_t)rdword(m + 18) & 0x8000);
	if (planet)
		SetReg(REG_D7, (uint32_t)(int32_t)((int16_t)rdword(m + 20) - 1));
	far_a6 = planet ? 0 : a6;
	far_addr = planet ? 0 : addr;
	bool have = false;
	for (int i = 0; i < rec.nfar && !have; i++)
		have = rec.far[i].addr == addr;
	far_radius[addr] = std::ldexp((float)(int16_t)rdword(m + 14), (int16_t)rdword(a6 - 208) + 1);
	if (!have && rec.nfar < MAX_FAR)
	{
		Far &f = rec.far[rec.nfar++];
		f.addr = addr;
		float p[3] = {(float)rdlong(a6 - 16), (float)rdlong(a6 - 12), -(float)rdlong(a6 - 8)};
		cockpit_pass_to_view(p, f.pos);
		f.radius = std::ldexp((float)(int16_t)rdword(m + 14), (int16_t)rdword(a6 - 208) + 1);
		f.colour = (uint16_t)rdword(m + 18);
		f.shrink = native_frame_shrink(a6);
		f.planet = planet;
		capture_light(a6, f.light, f.tint);

	}
	SetReg(REG_D0, enabled == 2 && !planet ? 1 : 0);
}

/* The sky model, drawn by L44200_DrawBGStars through L39426 in every pass:
 * kept once. Filled, the native scene draws it (native_draw_sky) and the
 * game doesn't: Lnative_object2 leaves L39426 when Lnative_culled is set.
 * Lnative_object would not, but the sky never comes through L390f2. */
static bool sky_hook(uint32_t a6)
{
	if (!native_enabled() || !scene_active())
		return false;
	uint32_t addr = (uint32_t)rdlong(a6 - 212);
	if (table_of(addr) != FE2_gamedata1 || (uint16_t)rdword(addr + 90) / 2 != SKY_MODEL)
		return false;
	if (!rec.has_sky)
	{
		capture(rec.sky, a6, addr, FE2_gamedata1, SKY_MODEL);
		rec.has_sky = true;
	}
	uint32_t ret = (uint32_t)rdlong((uint32_t)GetReg(REG_A7));
	if (enabled == 2 && ret > FE2_PutGameData3DObj && ret < FE2_Put3DGamedata2Obj)
		wrword(FE2_Lnative_culled, 1);
	return true;
}

extern "C" void Nu_NativeObject(void)
{
	scene_mute_object(0);
	uint32_t a6 = (uint32_t)GetReg(REG_A6);
	if (!rec.cam_ok && game_screen() == SCREEN_SYSTEM_MAP)
	{
		for (int i = 0; i < 9; i++)
			rec.cam[i] = (int16_t)rdword(GAME_A6 + 1082 + 2 * (uint32_t)i);
		rec.cam_ok = true;
		uint32_t ship = game_player_ship();
		if (ship)
		{
			uint32_t ms = (uint32_t)GAME_A6 + 17348 +
						  (uint32_t)(int)(int16_t)rdword(FE2_obj_offsets + 2 * (uint32_t)(uint8_t)rdbyte(ship + 93));
			rec.map_ship = ms;
			rec.map_par = (uint8_t)rdbyte(ms + 68);
			for (int i = 0; i < 3; i++)
				rec.map_r[i] = (double)rdlong(ms + 56 + 4 * (uint32_t)i) +
							   (double)(uint32_t)rdlong(ms + 44 + 4 * (uint32_t)i) / 4294967296.0;
		}
	}
	if (sky_hook(a6))
		return;
	uint32_t addr, table;
	int model;
	if (!wanted(a6, addr, table, model))
		return;
	capture(rec.obj[rec.n++], a6, addr, table, model);
	exact_apply(a6, rec.obj[rec.n - 1]);
	/* filled, the native scene draws it: not the game's primitives too,
	 * which the classic view paints under the native scene (over the
	 * planets and the far bodies there) */
	if (enabled == 2)
		scene_mute_object(a6);
}

/* Host call (Lnative_text in fe2.s): L3c676_ProjectVectorText has
 * formatted a model text's string (into the buffer at -220(a6), its own
 * frame's, just after the instruction at -202(a6)) */
extern "C" void Nu_NativeText(void)
{
	if (!native_enabled() || !scene_active())
		return;
	uint32_t a6 = (uint32_t)GetReg(REG_A6);
	uint32_t addr = (uint32_t)rdlong(a6 - 212);
	int sid = (uint16_t)rdword((uint32_t)rdlong(a6 - 202) - 2);
	uint32_t p = (uint32_t)rdlong(a6 - 220);
	std::string str;
	for (int c; str.size() < 255 && (c = rdbyte(p++)) != 0;)
		str += (char)c;
	if (texts_seen.size() > 1024)
		texts_seen.clear();
	texts_seen[text_key(addr, sid)] = str;
}

/* A planet object (L3cd9c, the planet command, is in these models) */
static bool planet_model(const Object &o)
{
	return o.table == FE2_gamedata1 && ((o.model >= 61 && o.model <= 86) || o.model == 193 || o.model == 194);
}

/* v turned towards w (both unit) by angle a, in their plane */
static void turn_towards(const float v[3], const float w[3], float a, float out[3])
{
	float d = v[0] * w[0] + v[1] * w[1] + v[2] * w[2];
	float p[3] = {w[0] - d * v[0], w[1] - d * v[1], w[2] - d * v[2]};
	float l = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
	if (l < 1e-4f) /* w is v: any way will do, up */
	{
		float up[3] = {0, 1, 0};
		if (std::fabs(v[1]) > 0.9f)
			up[1] = 0, up[2] = 1;
		d = v[0] * up[0] + v[1] * up[1] + v[2] * up[2];
		for (int c = 0; c < 3; c++)
			p[c] = up[c] - d * v[c];
		l = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
	}
	for (int c = 0; c < 3; c++)
		out[c] = v[c] * std::cos(a) + p[c] / l * std::sin(a);
}

/* The planets of the last flight frame published, which the capture passes
 * are chosen from. Not `pub`: a MAP or SHIP page made after the flight
 * frame replaces that with the page's own objects (gl_screen.c). */
struct CaptureSource
{
	uint32_t addr;
	float pos[3];
	float cols[3][3];
};
static std::vector<CaptureSource> capture_sources;

static void note_capture_sources(const Frame &f)
{
	capture_sources.clear();
	for (int i = 0; i < f.n; i++)
	{
		const Object &o = f.obj[i];
		if (!planet_model(o))
			continue;
		CaptureSource s;
		s.addr = o.addr;
		memcpy(s.pos, o.pos, sizeof s.pos);
		memcpy(s.cols, o.cols, sizeof s.cols);
		capture_sources.push_back(s);
	}
}

/* Each capture pass is a run of the game's whole 3D engine, 1 to 3 ms (a
 * planet close by is the dearest thing in it): a frame gets this many. A
 * planet is captured again no sooner than a number of frames after its
 * last pass, whether or not that made it seen: sooner for one never seen,
 * later for one already captured, which keeps its sphere and haze meanwhile,
 * placed by the current frame's pose. Of those due, the one that has waited
 * longest goes first; one never tried at all is due at once. And no pass
 * follows another sooner than CAPTURE_GAP_FRAMES: a system has some 25
 * bodies, and each one's interval alone leaves a pass in nearly every frame. */
#define CAPTURE_PASSES_PER_FRAME 1
#define CAPTURE_GAP_FRAMES 3
#define CAPTURE_RETRY_FRAMES 6    /* never seen */
#define CAPTURE_REFRESH_FRAMES 24 /* seen */

extern "C" int native_capture_dirs(float dirs[][3], int max)
{
	if (enabled != 2)
		return 0;
	static unsigned parity, last_any;
	static std::unordered_map<uint32_t, unsigned> last_pass; /* by object, the frame of its last pass */
	parity++;
	const float ahead[3] = {0, 0, -1};
	if (last_any && parity - last_any < CAPTURE_GAP_FRAMES)
		return 0;
	if (max > CAPTURE_PASSES_PER_FRAME)
		max = CAPTURE_PASSES_PER_FRAME;
	int n = 0;
	while (n < max)
	{
		const CaptureSource *best = nullptr;
		float best_dir[3];
		unsigned best_age = 0;
		for (const CaptureSource &o : capture_sources)
		{
			float c[3], radius;
			bool was_ahead = false, fresh = false;
			if (planet_native_find(o.addr, o.pos, o.cols, c, &radius, &was_ahead))
			{
				if (was_ahead)
					continue;
			}
			else
			{
				/* not seen yet: its centre, and its size if it was a far body */
				memcpy(c, o.pos, sizeof c);
				auto f = far_radius.find(o.addr);
				radius = f != far_radius.end() ? f->second : 0;
				fresh = true;
			}
			auto t = last_pass.find(o.addr);
			unsigned age = t == last_pass.end() ? ~0u : parity - t->second;
			if (age < (fresh ? CAPTURE_RETRY_FRAMES : CAPTURE_REFRESH_FRAMES) || (best && age <= best_age))
				continue;
			float dist = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
			if (dist <= 0)
				continue;
			float u[3] = {c[0] / dist, c[1] / dist, c[2] / dist}, g[3];
			/* its horizon, where it is nearest ahead: the planet's routine
			 * makes the planet and its haze when that is in view. Size unknown:
			 * its centre and a right angle off it, a frame each. */
			float a = radius > 0 ? std::asin(std::min(radius / dist, 1.0f)) : parity & 1 ? 1.5708f : 0.0f;
			turn_towards(u, ahead, a, g);
			best = &o;
			best_age = age;
			best_dir[0] = g[0];
			best_dir[1] = g[1];
			best_dir[2] = -g[2]; /* the game's: z ahead */
		}
		if (!best)
			break;
		memcpy(dirs[n++], best_dir, sizeof best_dir);
		last_pass[best->addr] = last_any = parity;
	}
	return n;
}

extern "C" bool native_object_pos(uint32_t addr, float pos[3], float *shrink)
{
	for (int i = 0; i < rec.n; i++)
		if (rec.obj[i].addr == addr && rec.obj[i].table == rec.obj[i].subtable)
		{
			memcpy(pos, rec.obj[i].pos, sizeof rec.obj[i].pos);
			*shrink = rec.obj[i].shrink;
			return true;
		}
	for (int i = 0; i < rec.nfar; i++)
		if (rec.far[i].addr == addr)
		{
			memcpy(pos, rec.far[i].pos, sizeof rec.far[i].pos);
			*shrink = rec.far[i].shrink;
			return true;
		}
	return false;
}

extern "C" void native_reset(void)
{
	rec.n = 0;
	rec.nfar = 0;
	rec.has_sky = false;
	rec.cam_ok = false;
	rec.map_ship = 0;
	rec.orbit.n = 0;
}

/* The ship's orbit in the system map. The map has its own copy of the
 * system (17348(a6), at the time point); there the ship and its parent are
 * drawn at view positions p_s and p_p, which relate to their frame
 * positions (the ship's r, parent relative) as p_s - p_p = k * A * r, A the
 * map camera's matrix (c_i = sum_j m[3j+i] w_j, as the game's own turning
 * into the ship's axes) or its transpose, k the zoom. A is taken as
 * whichever points the way p_s - p_p does and k from the lengths; the conic
 * round the parent (hud_conic_elements) is then drawn from p_p. */
static bool map_log()
{
	static int on = -1;
	if (on < 0)
		on = getenv("GLF_MAP_LOG") != nullptr;
	return on;
}

static void map_orbit_build(Frame &f, bool page = false)
{
	f.orbit.n = 0;
	ConicElems el;
	/* GLF_MAP_LOG: why there is none, each time that changes while the map is up */
	static int why = -1;
	int now = !f.cam_ok ? 1 : !hud_conic_elements(&el) ? 2 : 0;
	if (map_log() && (page || f.cam_ok) && now != why)
	{
		log_printf("map orbit: %s (orbit display %s)\n",
				   now == 1 ? "no map frame captured" : now == 2 ? "no orbit round the ship's parent" : "orbit ready",
				   orbit_display ? "on" : "off");
		why = now;
	}
	if (now)
		return;
	uint32_t ship = game_player_ship();
	uint32_t space = (uint32_t)GAME_A6 + 17348;
	uint32_t ms = f.map_ship;
	int par = f.map_par;
	static int early = 0;
	if (!ms || !par)
	{
		if (map_log() && ++early % 60 == 1)
			log_printf("map orbit: no parent for the map's ship %x (slot %d, flight ship %x, its parent %d)\n", ms,
					   (uint8_t)rdbyte(ship + 93), ship, (uint8_t)rdbyte(ship + 68));
		return;
	}
	uint32_t mp = space + (uint32_t)(int)(int16_t)rdword(FE2_obj_offsets + 2 * (uint32_t)par);
	float ps[3], pp[3], sh_s, sh_p;
	bool has_s = native_object_pos(ms, ps, &sh_s), has_p = native_object_pos(mp, pp, &sh_p);
	/* GLF_MAP_LOG: the scene as the map has it, about once a second */
	static int dump = 0;
	if (map_log() && ++dump % 60 == 1)
	{
		log_printf("map orbit: ship %x (slot %d) %s, parent %x (slot %d) %s; cam %d %d %d / %d %d %d / %d %d %d\n", ms,
				   (uint8_t)rdbyte(ship + 93), has_s ? "found" : "missing", mp, par, has_p ? "found" : "missing",
				   f.cam[0], f.cam[1], f.cam[2], f.cam[3], f.cam[4], f.cam[5], f.cam[6], f.cam[7], f.cam[8]);
		for (int i = 0; i < f.n; i++)
		{
			const Object &o = f.obj[i];
			log_printf("map orbit:   obj %x model %d table %x s %d shrink %g pos %g %g %g\n", o.addr, o.model, o.table,
					   o.scale, o.shrink, o.pos[0], o.pos[1], o.pos[2]);
		}
		for (int i = 0; i < f.nfar; i++)
			log_printf("map orbit:   far %x shrink %g radius %g pos %g %g %g\n", f.far[i].addr, f.far[i].shrink,
					   f.far[i].radius, f.far[i].pos[0], f.far[i].pos[1], f.far[i].pos[2]);
		log_printf("map orbit:   ship r %g %g %g (position units)\n", f.map_r[0], f.map_r[1], f.map_r[2]);
	}
	if (!has_s || !has_p)
		return;
	/* The game keeps each object's view position at its own scale, so the
	 * positions of two objects can't be compared, but their directions can:
	 * u_p to the planet, u_s to the ship. The matrix A turns the frame
	 * vector r (ship from planet) into the view: A r = ls u_s - lp u_p, the
	 * distances ls, lp in position units (least squares; the residual
	 * checks the matrix). The planet is then at lp u_p and a point P of
	 * the orbit at lp u_p + A P. */
	double r[3] = {f.map_r[0], f.map_r[1], f.map_r[2]}, up[3], us[3], best = 1e30, A[3][3] = {}, lp = 0;
	double pl = std::sqrt((double)pp[0] * pp[0] + (double)pp[1] * pp[1] + (double)pp[2] * pp[2]);
	double sl = std::sqrt((double)ps[0] * ps[0] + (double)ps[1] * ps[1] + (double)ps[2] * ps[2]);
	if (!(pl > 0) || !(sl > 0))
		return;
	for (int i = 0; i < 3; i++)
	{
		up[i] = pp[i] / pl;
		us[i] = ps[i] / sl;
	}
	for (int t = 0; t < 2; t++)
	{
		double a[3][3], c[3];
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				a[i][j] = (t ? f.cam[3 * i + j] : f.cam[3 * j + i]) / 32768.0;
		for (int i = 0; i < 3; i++)
			c[i] = a[i][0] * r[0] + a[i][1] * r[1] + a[i][2] * r[2];
		c[2] = -c[2]; /* GL's view space */
		/* c = ls us - lp up: normal equations in (ls, lp) */
		double uu = us[0] * up[0] + us[1] * up[1] + us[2] * up[2];
		double cs = c[0] * us[0] + c[1] * us[1] + c[2] * us[2], cp = c[0] * up[0] + c[1] * up[1] + c[2] * up[2];
		double det = 1 - uu * uu;
		if (std::fabs(det) < 1e-12)
			continue;
		/* [1 uu; uu 1] [ls; -lp] = [cs; cp] */
		double ls = (cs - uu * cp) / det, lpp = (uu * cs - cp) / det;
		double res = 0, cl = c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
		for (int i = 0; i < 3; i++)
		{
			double e = c[i] - (ls * us[i] - lpp * up[i]);
			res += e * e;
		}
		res = std::sqrt(res / cl);
		if (res < best)
		{
			best = res;
			memcpy(A, a, sizeof A);
			lp = lpp;
		}
	}
	if (map_log() && dump % 60 == 1)
		log_printf("map orbit: planet at %g position units, residual %.4f\n", lp, best);
	if (best > 0.05 || !(lp > 0))
		return;
	double centre[3] = {lp * up[0], lp * up[1], lp * up[2]};
	double k = 1; /* position units */
	double rad_view = el.radius;
	/* the path: a closed orbit all round, an escape out to 20 times the
	 * larger of its periapsis and the ship's distance */
	double nu0 = -3.14159265358979, nu1 = 3.14159265358979;
	if (!el.closed)
	{
		double rmax = 20 * std::max(el.rp, std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]));
		double lim = std::min(el.lim, std::acos(std::max(-1.0, std::min(1.0, (el.p / rmax - 1) / el.e))));
		nu0 = -lim;
		nu1 = lim;
	}
	double pts[MAP_ORBIT_PTS][3], big = 0;
	int n = MAP_ORBIT_PTS - 1;
	for (int j = 0; j <= n; j++)
	{
		double nu = nu0 + (nu1 - nu0) * j / n, rad = el.p / (1 + el.e * std::cos(nu)), w[3], c[3];
		for (int i = 0; i < 3; i++)
			w[i] = rad * (std::cos(nu) * el.pe[i] + std::sin(nu) * el.q[i]);
		for (int i = 0; i < 3; i++)
			c[i] = A[i][0] * w[0] + A[i][1] * w[1] + A[i][2] * w[2];
		c[2] = -c[2];
		for (int i = 0; i < 3; i++)
		{
			pts[j][i] = centre[i] + k * c[i];
			big = std::max(big, std::fabs(pts[j][i]));
		}
		/* behind the planet from the eye (the origin) */
		double a = 0, b = 0, cc = -rad_view * rad_view;
		for (int i = 0; i < 3; i++)
		{
			a += pts[j][i] * pts[j][i];
			b += -2 * pts[j][i] * centre[i];
			cc += centre[i] * centre[i];
		}
		double disc = b * b - 4 * a * cc, t = a > 0 ? (-b - std::sqrt(std::max(disc, 0.0))) / (2 * a) : 2;
		f.orbit.hid[j] = disc > 0 && t > 0 && t < 1;
	}
	/* a uniform scale about the eye changes nothing seen; it takes the
	 * numbers, in position units, well beyond the projection's near plane */
	double s = 1e6;
	if (big * s > 5e9)
		s = 5e9 / big;
	for (int j = 0; j <= n; j++)
		for (int i = 0; i < 3; i++)
			f.orbit.pt[j][i] = (float)(pts[j][i] * s);
	f.orbit.n = n + 1;
}

static void map_orbit_draw(const Frame &f)
{
	if (f.orbit.n < 2)
		return;
	gd_set_depth(nullptr);
	gd_color3f(0.0f, 1.0f, 0.0f);
	for (int i = 0; i + 1 < f.orbit.n; i++)
		if (!(f.orbit.hid[i] || f.orbit.hid[i + 1]) || i % 2 == 0)
			gd_line(f.orbit.pt[i], f.orbit.pt[i + 1]);
}

/* A far disc (a sun, a far planet's dot) is only put when a pass of the
 * game's engine has it ahead of its camera: the ahead pass for what is in
 * the game's own view, otherwise a capture pass, and those come every few
 * frames. Without this a body the view shows from beside the game's view
 * (the free camera, a turned head) is there for a frame and gone for the
 * next few, a star flashing up. The last sightings are kept for
 * FAR_KEEP_FRAMES published frames and placed where they were seen. */
#define FAR_KEEP_FRAMES 8
struct FarKept
{
	Far far;
	int age;
};
struct FarKeep
{
	std::vector<FarKept> kept;
	void apply(Frame &f)
	{
		std::vector<FarKept> now;
		for (int i = 0; i < f.nfar; i++)
			now.push_back({f.far[i], 0});
		for (const FarKept &k : kept)
		{
			if (k.age >= FAR_KEEP_FRAMES || k.far.planet)
				continue;
			bool seen = false;
			for (int i = 0; i < f.nfar && !seen; i++)
				seen = f.far[i].addr == k.far.addr;
			if (seen)
				continue;
			now.push_back({k.far, k.age + 1});
			if (f.nfar < MAX_FAR)
				f.far[f.nfar++] = k.far;
		}
		kept = now;
	}
};
static FarKeep far_keep_pub, far_keep_world;

extern "C" void native_publish(bool screen)
{
	map_orbit_build(rec);
	if (!screen)
	{
		far_keep_pub.apply(rec);
		note_capture_sources(rec);
	}
	pub = rec;
	pub.texts = texts_seen;
	pub.serial = ++publishes;
}

extern "C" void native_publish_world(void)
{
	map_orbit_build(rec);
	far_keep_world.apply(rec);
	note_capture_sources(rec);
	pub_world = rec;
	pub_world.texts = texts_seen;
	pub_world.serial = ++publishes;
}

extern "C" void native_acquire(bool screen, bool world)
{
	if (!screen)
		shows[0] = pub;
	else
	{
		shows[1] = pub;
		if (world)
			shows[0] = pub_world;
	}
}

extern "C" void native_publish_page(void)
{
	map_orbit_build(rec, true);
	pub_page = rec;
	pub_page.texts = texts_seen;
	pub_page.serial = ++publishes;
}

extern "C" void native_acquire_page(void)
{
	shows[2] = pub_page;
}

extern "C" void native_use_page(bool page)
{
	shown = &shows[page ? 2 : 0];
	screen_only = page;
	planet_use_page(page);
}

extern "C" void native_use_screen(bool screen)
{
	shown = &shows[screen ? 1 : 0];
	screen_only = screen;
	planet_use_screen(screen);
}

/* ---- running a model --------------------------------------------------------
 *
 * The game's model interpreter (L3e0a4_Project3DObj) run natively, for the
 * whole model in 3D: its conditions decide what is drawn (detail by
 * depth, parts by the object's variables), its sub-models are placed and
 * coloured the way the game does it, its primitives go into the game's
 * depth tree nodes. Unlike the game it draws everything, on screen or not,
 * facing the camera or not (back faces are culled when drawn). Detail and
 * drawing order use depth along the game's camera, as the game does, since
 * the models are tuned to it. */

namespace
{

/* What one object's models come to, view space, ready to draw: the nodes
 * of the game's depth tree (L38594_InsertIntoZTree), each a batch of
 * primitives in the model's order, or one primitive, with its depth key */
struct Tri
{
	P3 p[3];
	uint16_t colour; /* rgb444 */
};
struct Seg
{
	P3 a, b;
	uint16_t colour;
};
/* A blob (L3ab42_ProjectBlobs): stars, stardust; or a circle (L3a558) */
struct Dot
{
	P3 p;
	uint16_t colour;
	float px;     /* radius in pixels, else < 0 and... */
	float radius; /* ...in view units */
	bool twinkly = false; /* rays across it, 8 pixels longer (L3a93c) */
	int circ = -1;        /* a cached run's circle (Rec.circles): sized for each view */
};
constexpr int ZTREE_MAX = 4; /* nested depth trees (L385d0_3dview_thing2) */
struct GpuDraw;
struct Node
{
	float key; /* the game's depth key (zkey); the farthest is drawn first */
	/* The keys of the ztree nodes it is inside, outermost first, then its
	 * own: those nodes are sorted among the others by their keys, and what
	 * is inside each among itself */
	std::array<float, ZTREE_MAX + 1> path;
	/* Each level's key as a point (view space) and how far it is pushed
	 * back, so the key can be worked out again for any view (rekey) */
	std::array<P3, ZTREE_MAX + 1> kpt;
	std::array<float, ZTREE_MAX + 1> kpush;
	uint8_t kcam = 0; /* bit i: level i's point is the camera ($7fd, $7fe), not one of the model's */
	std::array<float, ZTREE_MAX + 1> dpath; /* the keys for the view drawn */
	int seq = 0; /* made in this order (the model's), for equal keys */
	int depth;
	std::vector<Tri> tris;
	std::vector<Seg> segs;
	std::vector<Dot> dots;
	/* Keyed farther back than it is (a batch pushed back by a depth): an
	 * underlay, ground plates, a starport's ground, cloud shadows. The game
	 * paints these under whatever is nearer, and they need not lie under it
	 * in 3D (a cloud's shadow hangs below the cloud, above the ground) */
	bool under = false;
	/* The key along the view being drawn (dpath[0], rekey): the game keys
	 * along its own camera, and its cockpit passes keyed again along each
	 * turned camera. The game's keys only order things right ahead of it:
	 * seen sideways in Lave Station's bay, the scaffold outside the
	 * station was "nearer" than the bay (an underlay) and drawn over it */
	float dist = 0;
	bool planet = false; /* the planet command: the sphere goes here */
	float radius = 0;    /* ...the planet's, view units */
	/* Triangles kept on the GPU (a cached run's, GLF_NATIVE_GPU): gcount
	 * vertices from gfirst of g's mesh, drawn before tris; gnode is the
	 * node in the cache entry. gcount 0: none of its faces is shown */
	const GpuDraw *g = nullptr;
	int gfirst = 0, gcount = 0, gnode = -1;
};
struct Drawn
{
	/* the player's own ship for its shadow: not drawn */
	bool hide = false;
	std::vector<Node> nodes;
	std::vector<Seg> wire; /* the wireframe: outlines and lines */
	std::vector<Dot> wire_dots;
	float near = 1e30f, far = 0; /* clip w (depth along the camera) it spans */
	/* The object's: what it is drawn at is shrunk by this (far away, the
	 * game works in a frame shrunk by a power of two, Object.shrink) */
	float shrink = 1;
	bool back = false; /* beyond a planet: drawn with the far bodies */
	/* An object that calls a planet's model (the intro's scene): what it
	 * draws before the planet, drawn after the sky, before the planet's
	 * atmosphere */
	std::vector<Node> before_planet;
	std::shared_ptr<GpuDraw> gpu; /* the pose and face colours its GPU nodes (Node.g) are drawn with */
};

/* A model being run: where it is and its state (the game's A6 frame) */
struct Run
{
	const Code *c;
	P3 org;       /* view */
	P3 ax[3];     /* the model's axes in view space */
	int s;        /* view = org + ax v 2^s */
	float k;      /* 2^s */
	int base;     /* base colour, -154(a6) */
	bool shadow = false; /* a cloud's shadow (model 206): drawn as translucent black */
	int shade0;   /* the shade of normal 0, faces without a normal */
	uint16_t t[8]; /* temporaries, -64(a6): t0 the depth of sub-models */
	int voff = 0;  /* mcopy: vertex records shifted by this many */
	/* The matrix msetup and mrotate make (-122(a6)), view space: sub-models
	 * called with flip code 6 get it, mcopy makes it the model's own */
	P3 m[3];
	/* The parent's vertices passed to a sub-model: vertex -1 (its origin)
	 * to -5, view space */
	std::optional<P3> pv[5];
	bool top = false; /* the object's own model, not a sub-model: t is the object's (Object.t) */
	/* where each temporary's value came from: bit i, the object's t[i] (Object.t)
	 * as the model started, copied down to sub-models; a calc result has none */
	uint8_t tsrc[8] = {};
};

/* ---- cached runs (native_model_cache) ------------------------------------------
 * A model's bytecode is run once in the object's own axes (its origin at 0, its
 * axes the unit ones, the same 2^s), and what it made is placed again in later
 * frames by the object's pose (Place) while what the run decided still holds:
 * every test of the run that hangs on the view (distances, faces turned to the
 * camera, sizes on the screen) is kept with its answer and asked again each
 * frame; any other answer, or other variables, scale or detail, and the model
 * runs again. Which faces face the camera and their shade are worked out each
 * frame (CFace), as are circles' sizes (CCircle). A run that used the frame's
 * randomness, an exhaust or text is not kept (volatile): it runs every frame. */

/* The object's pose: view = pos + ax v, v in the run's frame */
struct Place
{
	P3 pos;
	P3 ax[3];
	P3 at(const P3 &v) const
	{
		P3 p;
		for (int j = 0; j < 3; j++)
			p[j] = pos[j] + ax[0][j] * v[0] + ax[1][j] * v[1] + ax[2][j] * v[2];
		return p;
	}
	P3 dir(const P3 &v) const
	{
		P3 p;
		for (int j = 0; j < 3; j++)
			p[j] = ax[0][j] * v[0] + ax[1][j] * v[1] + ax[2][j] * v[2];
		return p;
	}
	/* a view point in the run's frame (at's inverse) */
	P3 back(const P3 &p) const
	{
		/* columns a, b, c: x = (p - pos) . (b x c, c x a, a x b) / det */
		const P3 &a = ax[0], &b = ax[1], &c = ax[2];
		P3 bc = {b[1] * c[2] - b[2] * c[1], b[2] * c[0] - b[0] * c[2], b[0] * c[1] - b[1] * c[0]};
		P3 ca = {c[1] * a[2] - c[2] * a[1], c[2] * a[0] - c[0] * a[2], c[0] * a[1] - c[1] * a[0]};
		P3 ab = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
		float det = a[0] * bc[0] + a[1] * bc[1] + a[2] * bc[2];
		if (det == 0)
			return p;
		P3 q = {p[0] - pos[0], p[1] - pos[1], p[2] - pos[2]};
		return {dot(q, bc) / det, dot(q, ca) / det, dot(q, ab) / det};
	}
};

/* The shade a normal gives (Runner::shade): fixed (>= 0), or the light's tint
 * for normal n, none when the test is on and n faces away from the camera at p */
struct ShadeSpec
{
	int fixed = -1;
	P3 n = {0, 0, 0}, p = {0, 0, 0};
	bool test = false;
};

/* A face of a cached run: its triangles' colour index is the face's */
struct CFace
{
	P3 n, a;   /* its normal and first vertex, for the facing test */
	bool cull; /* dropped when it faces away */
	bool tint_n; /* shaded by its own normal n, else by sh */
	ShadeSpec sh;
	uint16_t colour;
	int base;
	bool shadow;
};

/* A circle of a cached run (Runner::circle) */
struct CCircle
{
	P3 p;
	float across;
	int c;
	bool lit, twinkly;
};

/* An exhaust of a cached run (Runner::teardrop): put after the rest of its node */
struct CTear
{
	size_t node;
	P3 head, tail;
	float width; /* the third word * 2^s / 32 */
	int c;
};

/* An exhaust from view points head to tail into node nd (L3bc46_ProjectThrust) */
void tear_eval(const P3 &head, const P3 &tail, float width, int c, Node &nd)
{
	if (-head[2] <= 512 || -tail[2] <= 512)
		return;
	int px = (int)(width * 256 / -head[2]);
	if (px == 0)
		return;
	Seg line = {head, tail, (uint16_t)c};
	if (px <= 8)
	{
		nd.segs.push_back(line);
		return;
	}
	const float STRETCH = 1.3333333333f, BROADEN = 0.33f;
	const int STEPS = 24;
	const P3 &b = tail;
	P3 dir = {head[0] - b[0], head[1] - b[1], head[2] - b[2]};
	P3 cp[4] = {b,
				{b[0] + STRETCH * dir[0] - BROADEN * dir[1], b[1] + STRETCH * dir[1] + BROADEN * dir[0], b[2] + dir[2]},
				{b[0] + STRETCH * dir[0] + BROADEN * dir[1], b[1] + STRETCH * dir[1] - BROADEN * dir[0], b[2] + dir[2]},
				b};
	auto at = [&](float t) {
		float u = 1 - t, k0 = u * u * u, k1 = 3 * u * u * t, k2 = 3 * u * t * t, k3 = t * t * t;
		P3 q;
		for (int j = 0; j < 3; j++)
			q[j] = k0 * cp[0][j] + k1 * cp[1][j] + k2 * cp[2][j] + k3 * cp[3][j];
		return q;
	};
	P3 prev = at(0);
	for (int i = 1; i <= STEPS; i++)
	{
		P3 cur = at((float)i / STEPS);
		nd.tris.push_back({{b, prev, cur}, (uint16_t)c});
		prev = cur;
	}
}

/* The light's tint for a face with unit view normal n (L3a0ac): the light
 * vector points away from the light; a face square to it (n . light = -1)
 * gets tint 0, the brightest, one edge on 7, one facing away none. The game
 * takes bits 30-28 of the negative 2 n . light (normals are bytes, at most
 * 127/128 long). */
int tint_of(const Object &o, const P3 &n)
{
	float d = (n[0] * o.light[0] + n[1] * o.light[1] + n[2] * o.light[2]) * (127.0f / 128.0f);
	if (d >= 0)
		return 0;
	return o.tint[std::max(0, std::min(7, (int)std::floor(8 * (1 + d))))];
}

/* pl: the run's frame to the view's (null: it is the view's) */
int shade_eval(const Object &o, const Place *pl, const ShadeSpec &s)
{
	if (s.fixed >= 0)
		return s.fixed;
	P3 n = pl ? pl->dir(s.n) : s.n;
	if (s.test && !(dot(n, pl ? pl->at(s.p) : s.p) < 0))
		return 0;
	return tint_of(o, n);
}

/* A circle at view point p: false when it isn't drawn (too near) */
bool circle_eval(const Object &o, const P3 &p, const CCircle &cc, Dot &d)
{
	float z = -p[2];
	if (z < 64)
		return false;
	bool small = z / 8 > cc.across;
	if (!small && z <= cc.across / 2)
		return false; /* round the eye */
	d.p = p;
	d.twinkly = cc.twinkly;
	d.px = small ? std::floor(cc.across * 256 / z) / 2 : -1.0f;
	d.radius = small ? 0 : cc.across / 2;
	int c = cc.c;
	if (cc.lit && (small ? o.light[2] <= 0.875f : o.light[2] < -0.9766f))
		c = (c + o.tint[0]) & 0xfff;
	d.colour = (uint16_t)c;
	d.circ = -1;
	return true;
}

/* `ifdist`: how far apart two view points are on the game's screen (168
 * pixels high for its 36.5 degree view), roughly */
std::optional<float> apart_view(const P3 &p, const P3 &q)
{
	float dp = std::sqrt(dot(p, p)), dq = std::sqrt(dot(q, q));
	if (dp < 64 || dq < 64)
		return std::nullopt;
	static const float f = 84.0f / std::tan(CLASSIC_FOV * (float)M_PI / 360.0f);
	P3 d = {p[0] - q[0], p[1] - q[1], p[2] - q[2]};
	return f * std::sqrt(dot(d, d)) * 2 / (dp + dq);
}
bool apart_jump(const P3 &p, const P3 &q, int w1)
{
	auto d = apart_view(p, q);
	if (!d)
		return false;
	return (w1 & 0x8000) ? *d > (w1 & 0x7fff) : *d <= w1;
}

/* What a cached run decided on the view, and what it made that is worked out per view */
struct Rec
{
	struct Dist
	{
		P3 at;
		float lim;
		bool far;
	};
	struct Facing
	{
		P3 n, p;
		bool faces;
	};
	struct Apart
	{
		P3 p, q;
		int w1;
		bool jump;
	};
	struct Shade
	{
		ShadeSpec s;
		int v;
	};
	std::vector<Dist> dist;
	std::vector<Facing> facing;
	std::vector<Apart> apart;
	std::vector<Shade> shade;
	std::vector<CFace> faces;
	std::vector<CCircle> circles;
	std::vector<CTear> tears;
	/* bit tests of an object variable ($13, $14): only the answer must stay */
	struct Bit
	{
		int var, bit;
		bool set;
	};
	std::vector<Bit> bits;
	bool light = false; /* placed by the light (msetup, cloud shadows): kept while it stays put */
	std::vector<std::pair<uint64_t, std::string>> texts; /* the strings it drew (Frame.texts) */
	/* Sub-models good for one frame only, left out and run for each frame
	 * (cache_place): the call as it was made, in the object's axes */
	struct Live
	{
		Run c;
		int depth;
		int batch, shadow_node;
		std::vector<P3> zpt;
	};
	std::vector<Live> live;
	struct Sizes
	{
		size_t dist, facing, apart, shade, faces, circles, tears, bits, texts, live;
	};
	Sizes sizes() const
	{
		return {dist.size(),	facing.size(), apart.size(), shade.size(), faces.size(),
				circles.size(), tears.size(),  bits.size(),	 texts.size(), live.size()};
	}
	void cut(const Sizes &z)
	{
		dist.resize(z.dist);
		facing.resize(z.facing);
		apart.resize(z.apart);
		shade.resize(z.shade);
		faces.resize(z.faces);
		circles.resize(z.circles);
		tears.resize(z.tears);
		bits.resize(z.bits);
		texts.resize(z.texts);
		live.resize(z.live);
	}
	/* the object's variables and temporaries (Object.vars, .t) it read */
	uint64_t vread[2] = {0, 0};
	uint8_t tread = 0;
	const char *why = nullptr; /* why it isn't kept (volatile) */
};

struct Runner
{
	const Object &o;
	Drawn &out;
	bool wire;
	int s2; /* -192(a6): sub-models' scales are relative to it */
	uint32_t frame_seed;
	int shadow_node = -1; /* the node holding the cloud shadows */
	int batch = -1; /* the node a batch is going into (L385e4_3dview_word1) */
	std::vector<float> zpath; /* the open ztree nodes' keys */
	std::vector<P3> zpt; /* ...and their points (Node.kpt) */
	/* The sky: all of it, for any camera. The game draws the half of each
	 * part that faces its camera (normal tests), from inside. */
	bool sky = false;
	/* Ship shadows: the whole model for the light's view, not the half facing the camera */
	bool casting = false;
	/* A cached run (Rec): the model run in its own axes, pl places that frame
	 * in the view; rec keeps the tests that hang on the view. vol: it made
	 * something good for this frame and view only. L: the light (away from
	 * it) in the run's frame. */
	const Place *pl = nullptr;
	Rec *rec = nullptr;
	mutable bool vol = false;
	P3 L = {0, 0, 0};

	void volatile_(const char *why) const
	{
		vol = true;
		if (rec && !rec->why)
			rec->why = why;
	}
	/* an object variable, a temporary (noted for the cache: Rec.vread, .tread) */
	int var(int i) const
	{
		if (i < 0 || i >= NUM_VARS)
			return 0;
		if (rec)
			rec->vread[i >> 6] |= 1ull << (i & 63);
		return o.vars[i];
	}
	int tmp(const Run &r, int i) const
	{
		if (i < 0 || i >= 8)
			return 0;
		if (rec)
			rec->tread |= r.tsrc[i];
		return r.t[i];
	}

	P3 V(const P3 &p) const { return pl ? pl->at(p) : p; }
	P3 VD(const P3 &n) const { return pl ? pl->dir(n) : n; }

	Node &new_node(float key, bool under, const P3 &pt, float push = 0, bool cam = false)
	{
		Node n;
		n.key = key;
		for (size_t i = 0; i < zpt.size(); i++)
		{
			n.kpt[i] = zpt[i];
			n.kpush[i] = 0;
		}
		n.kpt[zpt.size()] = pt;
		n.kpush[zpt.size()] = push;
		if (cam)
			n.kcam = (uint8_t)(1u << zpt.size());
		n.seq = (int)out.nodes.size();
		n.depth = 0;
		for (float k : zpath)
			n.path[(size_t)n.depth++] = k;
		n.path[(size_t)n.depth++] = key;
		n.under = under;
		if (n.depth > 1)
			n.key = n.path[0];
		out.nodes.push_back(std::move(n));
		return out.nodes.back();
	}

	/* The node a primitive keyed on point `at` goes into */
	Node &node(const P3 &at)
	{
		if (batch >= 0)
			return out.nodes[(size_t)batch];
		return new_node(zkey(at), false, at);
	}

	/* ztree (L3a4b4): a node keyed on a vertex whose contents, up to the
	 * matching pop (bit 15), are a depth tree of their own */
	void ztree(const Run &r, const fe2::Instr &ins)
	{
		int w0 = ins.w[0];
		if (w0 & 0x8000)
		{
			if (!zpath.empty())
				zpath.pop_back();
			if (!zpt.empty())
				zpt.pop_back();
			return;
		}
		auto p = vertex(r, fe2::s8(w0 >> 5));
		if (zpath.size() < ZTREE_MAX)
		{
			zpath.push_back(zkey(p ? *p : r.org));
			zpt.push_back(p ? *p : r.org);
		}
	}

	/* batch begin / end (L3a1c0_ZWTF): what follows, up to the end or the
	 * next batch, is one node. Its key (v = w0 >> 5): $7ff the model's
	 * origin, $7fe drawn before everything, $7fd a depth (the next word, in
	 * model units), $7xx vertex xx pushed back by such a depth, $3xx the
	 * nearest and $1xx the farthest of vertex xx and those in the words
	 * after (while bit 15 is set), else vertex xx */
	void begin_batch(const Run &r, const fe2::Instr &ins)
	{
		const auto &w = ins.w;
		int v = (w[0] >> 5) & 0x7ff;
		batch = -1;
		if (v == 0)
			return;
		/* the key, and its point and push (Node.kpt) */
		float key, push = 0;
		P3 pt = r.org;
		auto at = [&](int i, P3 &q) {
			auto p = vertex(r, fe2::s8(i));
			q = p ? *p : r.org;
			return zkey(q);
		};
		if (v == 0x7ff)
			key = zkey(r.org);
		else if (v == 0x7fe)
		{
			key = push = 1e30f;
			pt = P3{0, 0, 0};
		}
		else if (v == 0x7fd)
		{
			key = push = w.size() > 1 ? (int16_t)w[1] * r.k : 0;
			pt = P3{0, 0, 0};
		}
		else if ((v & 0x700) == 0x700)
		{
			push = w.size() > 1 ? (int16_t)w[1] * r.k : 0;
			key = at(v, pt) + push;
		}
		else if (v & 0x100)
		{
			bool nearest = v & 0x200;
			key = at(v, pt);
			for (size_t i = 1; i < w.size(); i++)
			{
				P3 q;
				float k = at(w[i], q);
				if (nearest ? k < key : k > key)
				{
					key = k;
					pt = q;
				}
			}
		}
		else
			key = at(v, pt);
		bool under = v == 0x7fe || v == 0x7fd || ((v & 0x700) == 0x700 && v != 0x7ff && w.size() > 1 && (int16_t)w[1] > 0);
		new_node(key, under, pt, push, v == 0x7fe || v == 0x7fd);
		batch = (int)out.nodes.size() - 1;
	}

	P3 place(const Run &r, const P3 &v) const
	{
		P3 p;
		for (int j = 0; j < 3; j++)
			p[j] = r.org[j] + (r.ax[0][j] * v[0] + r.ax[1][j] * v[1] + r.ax[2][j] * v[2]) * r.k;
		return p;
	}
	P3 turn(const Run &r, const P3 &v) const
	{
		P3 p;
		for (int j = 0; j < 3; j++)
			p[j] = r.ax[0][j] * v[0] + r.ax[1][j] * v[1] + r.ax[2][j] * v[2];
		return p;
	}
	static float len(const P3 &a) { return std::sqrt(dot(a, a)); }
	/* A depth tree key as the game works it out (L3a202): depth along its
	 * camera plus (|x| + |y|) / 8. The models' drawing order is tuned to it,
	 * so it is kept, in the game camera's view space (GL's, z flipped). */
	static float zkey(const P3 &a) { return -a[2] + (std::fabs(a[0]) + std::fabs(a[1])) / 8; }

	/* View space -> the model's units. The axes are square to each other
	 * but not unit (the game's matrix is 127/128 long): divided by their
	 * length twice */
	fe2::Vec3 unplace(const Run &r, const P3 &p) const
	{
		P3 d = {p[0] - r.org[0], p[1] - r.org[1], p[2] - r.org[2]};
		float c[3];
		for (int a = 0; a < 3; a++)
		{
			float n = dot(r.ax[a], r.ax[a]);
			c[a] = n > 0 ? dot(r.ax[a], d) / (n * r.k) : 0;
		}
		return {c[0], c[1], c[2]};
	}
	fe2::Model::ParentFn parent_fn(const Run &r) const
	{
		return [this, &r](int j) -> std::optional<fe2::Vec3> {
			if (j == -1)
				return fe2::Vec3();
			int k = -1 - j;
			if (k < 0 || k >= 5 || !r.pv[k])
				return std::nullopt;
			return unplace(r, *r.pv[k]);
		};
	}
	/* Type $7/$8 vertices' random offsets (L39a26): three random longs
	 * >> shift, view units; new each frame, the same within one */
	fe2::Vec3 jitter(const Run &r, int vi, int shift) const
	{
		volatile_("random vertices"); /* new each frame */
		uint32_t h = o.addr * 2654435761u ^ (uint32_t)r.c->model.index * 40503u ^ (uint32_t)vi * 2246822519u ^
					 frame_seed * 3266489917u;
		float d[3];
		for (float &x : d)
		{
			h ^= h >> 15;
			h *= 2246822519u;
			h ^= h >> 13;
			h *= 3266489917u;
			h ^= h >> 16;
			x = shift >= 32 ? 0 : std::ldexp((float)(int32_t)h, -shift);
		}
		return {d[0] / r.k, d[1] / r.k, d[2] / r.k};
	}
	fe2::VertexEnv env(const Run &r) const
	{
		fe2::VertexEnv e;
		e.param = [this, &r](int p) { return param(r, p); };
		e.offset = r.voff;
		e.jitter = [this, &r](int vi, int shift) { return jitter(r, vi, shift); };
		return e;
	}

	std::optional<P3> vertex(const Run &r, int i) const
	{
		fe2::VertexEnv e = env(r);
		auto v = r.c->model.vertex_pos(i, parent_fn(r), 0, &e);
		if (!v)
			return std::nullopt;
		return place(r, to_p3(*v));
	}

	/* The light's tint for a face with unit view normal n (tint_of) */
	int tint(const P3 &n) const { return tint_of(o, n); }

	/* Normal `n` of the model, the run's frame, unit; and whether it faces
	 * the camera (the game's $8000 when it doesn't). Normal 0 always does. */
	bool normal(const Run &r, int n, P3 &out) const
	{
		auto nv = r.c->model.normal_vec(n);
		if (!nv)
			return false;
		out = turn(r, to_p3(fe2::normalize(*nv)));
		return true;
	}
	/* normal n and the vertex it is tested at; false: no test, it faces */
	bool facing(const Run &r, int n, P3 &nv, P3 &p) const
	{
		if (!normal(r, n, nv))
			return false;
		int ni = (n >> 1) - 1;
		if (ni < 0 || ni >= (int)r.c->model.norms.size())
			return false;
		auto q = vertex(r, r.c->model.norms[ni].v ^ (n & 1));
		if (!q)
			return false;
		p = *q;
		return true;
	}
	bool normal_faces(const Run &r, int n) const
	{
		P3 nv, p;
		if (!facing(r, n, nv, p))
			return true;
		bool f = dot(VD(nv), V(p)) < 0;
		if (rec)
			rec->facing.push_back({nv, p, f});
		return f;
	}
	/* The shade normal n gives a colour (L3a02a_3DPrimCullNLight) */
	ShadeSpec shade_spec(const Run &r, int n) const
	{
		ShadeSpec s;
		if (n <= 1)
		{
			s.fixed = n == 0 ? r.shade0 : o.tint[4];
			return s;
		}
		if (!normal(r, n, s.n))
		{
			s.fixed = 0;
			return s;
		}
		P3 nv;
		s.test = facing(r, n, nv, s.p);
		return s;
	}
	int shade(const Run &r, int n) const
	{
		ShadeSpec s = shade_spec(r, n);
		int v = shade_eval(o, pl, s);
		if (rec && s.fixed < 0)
			rec->shade.push_back({s, v});
		return v;
	}

	/* A face's colour as the game works it out: its colour value plus the
	 * shade (none for $100, unlit), plus the base colour for $10 */
	static int colour(int c, int shade, int base)
	{
		c &= 0xffe;
		bool add_base = c & 0x10;
		c &= ~0x10;
		if (c & 0x100)
			c &= ~0x100;
		else
			c = (c + shade) & 0xfff;
		if (add_base)
			c += base;
		return c & 0xfff;
	}

	/* An operand byte: a constant, a constant << 10, a variable of the
	 * object or a temporary */
	int param(const Run &r, int p) const
	{
		int v = p & 0x3f;
		switch (p & 0xc0)
		{
		case 0x00:
			return v;
		case 0x40:
			return (v << 10) & 0xffff;
		case 0x80:
			return var(v);
		default:
			return tmp(r, v);
		}
	}

	static int sine(int a)
	{
		const fe2::Image &img = fe2::Image::game();
		int i = (a & 0x7fff) >> 4;
		int s = (int16_t)img.u16(FE2_sine_table + 2 * (uint32_t)i);
		return (a & 0x8000) ? -s : s;
	}

	/* calc (L3cc2c_InterpretByteCode): 16 bit words */
	int calc(int fn, int a, int b) const
	{
		a &= 0xffff;
		b &= 0xffff;
		int n = b & 63;
		switch (fn)
		{
		case 0:
			return a + b;
		case 1:
			return a - b;
		case 2:
		case 8:
			return a * b;
		case 3:
			return b ? a / b : a;
		case 4:
			return n < 16 ? a >> n : 0;
		case 5:
			return n < 16 ? a << n : 0;
		case 6:
			return std::max(a, b);
		case 7:
			return b > a ? a : b - 1;
		case 9:
			return n < 16 ? (int16_t)a >> n : ((int16_t)a < 0 ? -1 : 0);
		case 10:
		{
			int v = (a + b) & 0xffff;
			return var(v);
		}
		case 11:
			return b <= a ? a : 0;
		case 12:
			return b >= a ? a : 0;
		case 13:
			return (int)(((int64_t)(int16_t)a * sine(b) * 2) >> 16);
		case 14:
			return (int)(((int64_t)(int16_t)a * sine(b + 0x4000) * 2) >> 16);
		default:
			return a & b;
		}
	}

	/* `if > z:K` / `if < z:K`: K << (detail + s) against the distance. The
	 * game uses the depth along its camera (-8(a6)), which is the same for
	 * things ahead but changes as the view turns: pitched 20 degrees, Merlin's
	 * cloud field (model 99) dropped its smaller clouds. nullopt when the
	 * shift overflows */
	std::optional<bool> beyond(const Run &r, int k) const
	{
		int sh = o.detail[k & 1] + r.s + native_detail_boost;
		if (sh < 0 || sh > 31 || ((int64_t)k << sh) > 0x7fffffff)
			return std::nullopt;
		float lim = (float)((int64_t)k << sh);
		bool far = len(V(r.org)) > lim;
		if (rec)
			rec->dist.push_back({r.org, lim, far});
		return far;
	}

	/* `ifdist` (apart_jump): whether to jump, by how far apart vertices a
	 * and b are on the game's screen */
	bool apart(const Run &r, int a, int b, int w1) const
	{
		auto p = vertex(r, a), q = vertex(r, b);
		if (!p || !q)
			return false;
		bool jump = apart_jump(V(*p), V(*q), w1);
		if (rec)
			rec->apart.push_back({*p, *q, w1, jump});
		return jump;
	}

	void emit(const Run &r, size_t i)
	{
		const std::vector<Face> *faces = &r.c->faces[i];
		const std::vector<Line> *lines = &r.c->lines[i];
		std::vector<Face> dfaces;
		std::vector<Line> dlines;
		if (r.c->dyn[i] || r.voff)
		{
			fe2::VertexEnv e = env(r);
			build_prims(r.c->model, r.c->ins[i], parent_fn(r), &e, dfaces, dlines);
			faces = &dfaces;
			lines = &dlines;
		}
		for (const Face &f : *faces)
		{
			P3 n = turn(r, f.n);
			P3 a = place(r, f.tris[0]);
			bool cull = f.has_normal && !sky && !casting;
			uint16_t col;
			if (rec)
			{
				/* worked out for each view (face_colour): the triangles' colour
				 * is the face's index */
				CFace cf = {n, a, cull, f.has_normal, f.has_normal ? ShadeSpec() : shade_spec(r, f.nidx),
							(uint16_t)f.colour, r.base, r.shadow};
				if (rec->faces.size() > 0xffff)
					volatile_("faces");
				col = (uint16_t)rec->faces.size();
				rec->faces.push_back(cf);
			}
			else
			{
				if (cull && dot(n, a) >= 0)
					continue; /* faces away */
				int sh = f.has_normal ? tint(n) : shade(r, f.nidx);
				col = (uint16_t)colour(f.colour, sh, r.base);
				if (r.shadow)
					col |= 0x8000;
			}
			if (wire)
			{
				auto loop = [&](const std::vector<P3> &lp) {
					for (size_t j = 0; j < lp.size(); j++)
						out.wire.push_back({place(r, lp[j]), place(r, lp[(j + 1) % lp.size()]), col});
				};
				loop(f.outline);
				for (const auto &lp : f.more)
					loop(lp);
				continue;
			}
			/* keyed on its first vertex, as the game does */
			Node *ndp = &node(place(r, f.outline[0]));
			if (r.shadow)
			{
				/* all of an object's cloud shadows in one underlay node, so
				 * they can be drawn once per pixel (draw_tris) */
				if (shadow_node < 0 && ndp->under)
					shadow_node = (int)(ndp - out.nodes.data());
				if (shadow_node >= 0)
					ndp = &out.nodes[(size_t)shadow_node];
			}
			Node &nd = *ndp;
			for (size_t t = 0; t + 2 < f.tris.size(); t += 3)
			{
				Tri tri;
				for (int j = 0; j < 3; j++)
					tri.p[j] = place(r, f.tris[t + j]);
				tri.colour = col;
				nd.tris.push_back(tri);
			}
		}
		for (const Line &l : *lines)
		{
			Seg s = {place(r, l.a), place(r, l.b), (uint16_t)colour(l.colour, r.shade0, r.base)};
			if (wire)
				out.wire.push_back(s);
			else
				node(s.a).segs.push_back(s);
		}
	}

	/* blobs (L3ab42_ProjectBlobs): a dot at each vertex of a list. The size
	 * word: bit 7 clear a constant, rotated right by 7, else an operand
	 * (bit 6 a temporary, else an object variable); with bit 15 the radius
	 * in pixels, else in model units / 128. Vertex bytes follow up to $7f. */
	void blobs(const Run &r, const fe2::Instr &ins)
	{
		const auto &w = ins.w;
		if (w.size() < 3)
			return;
		int c = (w[0] >> 4) & 0xefe;
		if (c & 0x10)
			c = ((c & ~0x10) + r.base) & 0xfff;
		int sz = w[1], v;
		if (!(sz & 0x80))
			v = ((sz >> 7) | (sz << 9)) & 0xffff;
		else if (sz & 0x40)
			v = tmp(r, sz & 0x3f);
		else
			v = var(sz & 0x3f);
		Dot d;
		d.colour = (uint16_t)c;
		d.px = (v & 0x8000) ? (float)(v & 0x7fff) : -1.0f;
		d.radius = (v & 0x8000) ? 0 : (float)(int16_t)v * r.k / 128;
		for (size_t i = 2; i < w.size(); i++)
			for (int b : {w[i] >> 8, w[i] & 0xff})
			{
				if (b == 0x7f)
					return;
				auto p = vertex(r, fe2::s8(b));
				if (!p)
					continue;
				d.p = *p;
				if (wire)
					out.wire_dots.push_back(d);
				else
					node(d.p).dots.push_back(d);
			}
	}

	/* circle (L3a558_ProjectCircle): a disc at the vertex in the high byte
	 * of the third word, its size word (as a blob's, but no pixel form)
	 * / 2 * 2^(s-7) across: the game halves it for the radius (L3a68c's
	 * clip, l3a5e6 up close). Far away for its size, a whole number of
	 * pixels across. With bit 7 of that word, rays across it. Lit unless the
	 * colour's bit 8: plus the brightest tint when the light comes from
	 * anywhere but well behind it (L3a908 on, a small one; a big one only
	 * square to it) */
	void circle(const Run &r, const fe2::Instr &ins)
	{
		const auto &w = ins.w;
		if (w.size() < 3)
			return;
		auto p = vertex(r, fe2::s8(w[2] >> 8));
		if (!p)
			return;
		int sz = w[1], v;
		if (!(sz & 0x80))
			v = ((sz >> 7) | (sz << 9)) & 0xffff;
		else if (sz & 0x40)
			v = tmp(r, sz & 0x3f);
		else
			v = var(sz & 0x3f);
		int c0 = (w[0] >> 4) & 0xfff;
		int c = c0 & 0xefe;
		if (c & 0x10)
			c = ((c & ~0x10) + r.base) & 0xfff;
		CCircle cc = {*p, (float)((v & 0xffff) >> 1) * r.k / 128, c, !(c0 & 0x100), (w[2] & 0x80) != 0};
		Dot d;
		if (rec)
		{
			/* sized for each view (circle_eval) */
			d.p = *p;
			d.circ = (int)rec->circles.size();
			rec->circles.push_back(cc);
			node(d.p).dots.push_back(d);
			return;
		}
		if (!circle_eval(o, *p, cc, d))
			return;
		if (wire)
			out.wire_dots.push_back(d);
		else
			node(d.p).dots.push_back(d);
	}

	/* teardrop (L3bc46_ProjectThrust): an engine's exhaust, from the
	 * vertex in the high byte of the second word (its tail) towards the
	 * one in the low byte (its head); the third word its width, * 2^(s-5).
	 * Narrower than 9 pixels on the game's screen, a line; as gl_prims.c
	 * draws the game's (draw_teardrop) */
	void teardrop(const Run &r, const fe2::Instr &ins)
	{
		const auto &w = ins.w;
		if (w.size() < 3)
			return;
		auto head = vertex(r, fe2::s8(w[1])), tail = vertex(r, fe2::s8(w[1] >> 8));
		if (!head || !tail)
			return;
		int c = (w[0] >> 4) & 0xefe;
		if (c & 0x10)
			c = ((c & ~0x10) + r.base) & 0xfff;
		float width = (float)(int16_t)w[2] * r.k / 32;
		if (rec)
		{
			/* shaped for each view (tear_eval) */
			Node &nd = node(*head);
			rec->tears.push_back({(size_t)(&nd - out.nodes.data()), *head, *tail, width, c});
			return;
		}
		if (wire)
		{
			if (-(*head)[2] > 512 && -(*tail)[2] > 512 && (int)(width * 256 / -(*head)[2]) != 0)
				out.wire.push_back({*head, *tail, (uint16_t)c});
			return;
		}
		if (-(*head)[2] <= 512 || -(*tail)[2] <= 512 || (int)(width * 256 / -(*head)[2]) == 0)
			return;
		tear_eval(*head, *tail, width, c, node(*head));
	}

	/* Axes swapped by `code` and negated by bits 13, 12, 11 of `w`
	 * (L3c446's table, as L3c3be and mrotate use it) */
	static void permute(const P3 (&from)[3], int w, int code, P3 (&to)[3])
	{
		static const int SWAPS[8][3] = {{0, 1, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0},
										{1, 0, 2}, {0, 2, 1}, {0, 1, 2}, {0, 1, 2}};
		const int bits[3] = {0x2000, 0x1000, 0x0800};
		for (int i = 0; i < 3; i++)
		{
			float sign = (w & bits[i]) ? -1.0f : 1.0f;
			for (int j = 0; j < 3; j++)
				to[SWAPS[code][i]][j] = from[i][j] * sign;
		}
	}

	/* Turns the matrix's axes `pair` (0: y, z; 1: z, x; 2: x, y) by the
	 * angle with this cosine and sine (L3c614): a' = a c + b s,
	 * b' = b c - a s */
	static void rotate(Run &r, int pair, float c, float s)
	{
		static const int AXES[4][2] = {{1, 2}, {2, 0}, {0, 1}, {1, 0}};
		P3 &a = r.m[AXES[pair][0]], &b = r.m[AXES[pair][1]];
		for (int j = 0; j < 3; j++)
		{
			float x = a[j], y = b[j];
			a[j] = x * c + y * s;
			b[j] = y * c - x * s;
		}
	}
	/* An angle word (mrotate's): a constant ror 7, or a variable or
	 * temporary (bit 6) */
	int angle(const Run &r, int w) const
	{
		w &= 0xffff;
		if (!(w & 0x80))
			return ((w >> 7) | (w << 9)) & 0xffff;
		int i = (((w & ~0x80) * 2) & 0xffff) & ~0x80;
		if (!(w & 0x40))
			return var(i >> 1);
		return tmp(r, i >> 1);
	}
	void rotate_by(Run &r, int pair, int a) const
	{
		rotate(r, pair, sine(a + 0x4000) / 32768.0f, sine(a) / 32768.0f);
	}

	/* msetup (L3c556): the matrix the camera's (bit 15), or turned about
	 * one axis towards the light in the model's axes (-42(a6)) */
	void msetup(Run &r, int w0)
	{
		if (w0 & 0x8000)
		{
			if (pl)
				volatile_("camera matrix"); /* the camera's axes: the view's */
			r.m[0] = {1, 0, 0};
			r.m[1] = {0, 1, 0};
			r.m[2] = {0, 0, -1};
			return;
		}
		int pair = (w0 >> 5) & 3;
		/* the light, away from it, in the model's axes; the game view's
		 * z is GL's flipped, as are the axes, so the dots agree */
		const P3 light = L;
		if (rec)
			rec->light = true;
		float l[3] = {dot(r.ax[0], light), dot(r.ax[1], light), dot(r.ax[2], light)};
		static const int COMP[4][2] = {{1, 2}, {2, 0}, {0, 1}, {1, 0}};
		float c = l[COMP[pair][0]], s = l[COMP[pair][1]];
		float n = std::sqrt(c * c + s * s);
		if (n <= 0)
			return;
		/* the game's divide comes to about 0.992 of a unit vector */
		rotate(r, pair, c / n * (16384.0f / 0.50392f / 32768.0f), s / n * (16384.0f / 0.50392f / 32768.0f));
	}

	/* mrotate (L3c4ac): the matrix the model's axes swapped and negated
	 * and turned by an angle (when the next word isn't 0); with bit 15,
	 * the matrix turned by an angle */
	void mrotate(Run &r, const fe2::Instr &ins)
	{
		const auto &w = ins.w;
		int w0 = w[0];
		int a = w.size() > 1 ? w[1] : 0;
		if (!(w0 & 0x8000))
		{
			permute(r.ax, w0 << 1, (w0 >> 7) & 7, r.m);
			if (!a)
				return;
		}
		rotate_by(r, (w0 >> 5) & 3, angle(r, a));
	}

	/* mcopy (L39ea0): with bit 15 the matrix becomes the model's own axes;
	 * else the vertex records are shifted by an operand */
	void mcopy(Run &r, int w0)
	{
		if (w0 & 0x8000)
		{
			if (w0 == 0x801e)
				return; /* a list of stars drawn from the object (the maps) */
			for (int i = 0; i < 3; i++)
				r.ax[i] = r.m[i];
			return;
		}
		r.voff += (int16_t)(param(r, (w0 >> 5) & 0xff) << 2) >> 2;
	}

	/* Cloud shadows cast by the light (native_cloud_shadows; GLF_CLOUD_SHADOWS=2 logs). The
	 * game puts a cloud field's shadow (model 206, a flat copy of the cloud's
	 * outline) straight below its cloud (model 98): the vertex ten ids before
	 * the shadow's is the cloud's, the same place at the cloud's height.
	 * Here the shadow is moved to where the cloud's ray along the light
	 * vector (o.light, away from the light, view space) meets the field's
	 * ground plane (the field's y axis is up). Without light from above (the
	 * night side, the sun on the horizon) there is no shadow: false. */
	/* the cloud shadow drawn as black with some alpha, so the ground (roads,
	 * its colour) shows through, as the ship shadows do */
	bool cloud_shadow_translucent() const
	{
		static const char *env = getenv("GLF_CLOUD_SHADOWS");
		return env ? atoi(env) != 0 : native_cloud_shadows;
	}

	bool cloud_shadow_org(const Run &r, int model, int vid, P3 &org) const
	{
		static const char *env = getenv("GLF_CLOUD_SHADOWS");
		const int mode = env ? atoi(env) : native_cloud_shadows;
		if (!mode || model != 206 || wire || sky)
			return true;
		auto top = vertex(r, vid - 10);
		auto gp = r.c->model.vertex_pos(vid);
		auto tp = r.c->model.vertex_pos(vid - 10);
		if (!top || !gp || !tp || std::fabs(gp->x - tp->x) > 0.5f || std::fabs(gp->z - tp->z) > 0.5f || tp->y <= gp->y)
			return true;
		/* in view space (a cached run's frame need not be square: the
		 * game's matrix isn't quite), then back */
		P3 up = VD(r.ax[1]), at = V(org), tv = V(*top);
		const P3 light = {o.light[0], o.light[1], o.light[2]};
		float ul = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
		if (ul <= 0)
			return true;
		float lu = 0, du = 0;
		P3 d;
		for (int j = 0; j < 3; j++)
		{
			up[j] /= ul;
			d[j] = tv[j] - at[j];
			lu += light[j] * up[j];
			du += d[j] * up[j];
		}
		/* the sun's height above the horizon: -lu (sin of it) */
		if (mode == 2)
		{
			static int n;
			if (n++ % 600 == 0)
				log_printf("cloud shadow: sun elevation %.1f deg, cloud %.0f above\n",
						   std::asin(std::max(-1.0f, std::min(1.0f, -lu))) * 57.2958f, du);
		}
		if (rec)
			rec->light = true;
		if (-lu < (low_sun_shadows ? 0.06f : 0.12f)) /* below about 3.4 (or 7) degrees: no shadow */
			return false;
		for (int j = 0; j < 3; j++)
			at[j] += d[j] - du / lu * light[j];
		org = pl ? pl->back(at) : at;
		return true;
	}

	/* a sub-model call (L3c928, L3c8d8): op $0e, or $1b with a scale and
	 * a colour */
	void call(const Run &r, const fe2::Instr &ins, int depth)
	{
		const auto &w = ins.w;
		const Code *child = code_for(o.subtable, (w[0] >> 5) & 0x7ff);
		if (!child || child == r.c)
			return;
		auto org = vertex(r, fe2::s8(w[1]));
		if (!org)
			return;
		/* a planet (the intro's): the sphere is drawn with the far bodies,
		 * after what of this model is farther (draw_fars) */
		if (child->planet)
		{
			if (!wire)
				new_node(zkey(*org), false, *org).planet = true;
			return;
		}
		if (!cloud_shadow_org(r, (w[0] >> 5) & 0x7ff, fe2::s8(w[1]), *org))
			return;
		Run c;
		c.shadow = (w[0] >> 5 & 0x7ff) == 206 && !wire && !sky && cloud_shadow_translucent();
		c.c = child;
		c.org = *org;
		c.base = r.base;
		c.pv[0] = *org;
		/* with bit 15, the long after (the scale word for $1b) gives
		 * vertices -2 to -5: bytes 0, 2, 3, 1 (low first), up to a $7f */
		size_t ext = ins.op == 0x1b ? 3 : 2;
		if ((w[1] & 0x8000) && w.size() >= ext + 2)
		{
			uint32_t l = (uint32_t)w[ext] << 16 | w[ext + 1];
			const int byte_of[4] = {0, 2, 3, 1};
			for (int k = 0; k < 4; k++)
			{
				int b = (int)(l >> (8 * byte_of[k])) & 0xff;
				if (k > 0 && b == 0x7f)
					break;
				c.pv[k + 1] = vertex(r, fe2::s8(b));
			}
		}
		int sparam = 0;
		if (ins.op == 0x1b)
		{
			int col = (w[2] * 2) & 0xefe;
			if (col & 0x10)
				col = ((col & ~0x10) + r.base) & 0xfff;
			c.base = col;
			if (w[2] & 0x800)
				sparam = (int16_t)tmp(r, (w[2] >> 12) & 7);
			else
				sparam = (int16_t)w[2] >> 12;
		}
		c.s = sparam + child->model.scale + child->model.scale2 - s2;
		c.k = std::ldexp(1.0f, c.s);
		/* its axes: the parent's, swapped and negated (L3c3be); code 6
		 * the parent's msetup / mrotate matrix */
		int code = (w[1] >> 8) & 7;
		if (code == 6)
			for (int i = 0; i < 3; i++)
				c.ax[i] = r.m[i];
		else
			permute(r.ax, w[1], code, c.ax);
		for (int i = 0; i < 3; i++)
			c.m[i] = c.ax[i];
		c.shade0 = o.tint[3];
		std::memset(c.t, 0, sizeof c.t);
		c.t[0] = (uint16_t)(r.t[0] + 1);
		c.t[1] = r.t[1];
		c.t[2] = r.t[2];
		for (int i = 0; i < 3; i++)
			c.tsrc[i] = r.tsrc[i];
		if (!rec)
		{
			run(c, depth + 1);
			return;
		}
		/* A cached run: a sub-model good for this frame only (random
		 * vertices, the camera's matrix) is taken out again and run for each
		 * frame (Rec.live), not the whole object */
		bool was = vol;
		const char *was_why = rec->why;
		bool was_light = rec->light;
		uint64_t vr[2] = {rec->vread[0], rec->vread[1]};
		Rec::Sizes sz = rec->sizes();
		size_t nn = out.nodes.size(), bt = 0, bs = 0, bd = 0;
		int b0 = batch, sh0 = shadow_node;
		if (batch >= 0)
		{
			const Node &bn = out.nodes[(size_t)batch];
			bt = bn.tris.size();
			bs = bn.segs.size();
			bd = bn.dots.size();
		}
		std::vector<float> zp = zpath;
		std::vector<P3> zq = zpt;
		run(c, depth + 1);
		if (!vol || was)
			return;
		vol = false;
		rec->why = was_why;
		rec->light = was_light;
		rec->vread[0] = vr[0];
		rec->vread[1] = vr[1];
		rec->cut(sz);
		out.nodes.resize(nn);
		if (b0 >= 0)
		{
			Node &bn = out.nodes[(size_t)b0];
			bn.tris.resize(bt);
			bn.segs.resize(bs);
			bn.dots.resize(bd);
		}
		zpath = zp;
		zpt = zq;
		if (batch >= (int)nn)
			batch = -1; /* its own batch, made again with it */
		shadow_node = sh0;
		/* the temporaries it is given stay as they were */
		rec->tread |= (uint8_t)(c.tsrc[0] | c.tsrc[1] | c.tsrc[2]);
		rec->live.push_back({c, depth + 1, b0, sh0, zq});
	}

	/* vtext (L3c676_ProjectVectorText): the string the game formatted for
	 * the id in the fourth word (Nu_NativeText), in font (w1 >> 12) of
	 * gamedata2's, each character a model of its own (a glyph, the
	 * font's code at the offset at +32 + 2 (c - 32)) whose closing word
	 * >> 6 is the vertex the next one starts at. Below a space, a new line:
	 * the start of the line moved to the vertex in the font's word at +30.
	 * At vertex w2's low byte, the axes the parent's swapped and negated
	 * as for a sub-model (w2's high byte) but never mirrored, scale
	 * (w1 >> 8) & 15; skipped when normal w1's low byte faces away, its
	 * shade lighting the colour */
	void text(const Run &r, const fe2::Instr &ins, int depth)
	{
		const auto &w = ins.w;
		if (w.size() < 4)
			return;
		int n = w[1] & 0xff;
		if (!sky && !casting && n > 1 && !normal_faces(r, n))
			return;
		auto it = shown->texts.find(text_key(o.addr, w[3]));
		if (rec)
			rec->texts.push_back({text_key(o.addr, w[3]), it == shown->texts.end() ? std::string("\1") : it->second});
		auto org = vertex(r, fe2::s8(w[2]));
		if (it == shown->texts.end() || !org)
			return;
		const fe2::Image &img = image_for(FE2_gamedata2);
		int font = (w[1] >> 12) & 15;
		auto fa = img.model_addr(font);
		if (!fa)
			return;
		Run g;
		g.org = *org;
		int code = (w[2] >> 8) & 7;
		if (code == 6)
			for (int i = 0; i < 3; i++)
				g.ax[i] = r.m[i];
		else
			permute(r.ax, w[2], code, g.ax);
		/* GL view space has z flipped: an unmirrored frame's determinant
		 * is negative */
		const P3 &a = g.ax[0], &b = g.ax[1], &c = g.ax[2];
		float det = a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) +
					a[2] * (b[0] * c[1] - b[1] * c[0]);
		if (pl)
		{
			/* a cached run's frame: the view's handedness is the pose's times its own */
			const P3 &pa = pl->ax[0], &pb = pl->ax[1], &pc = pl->ax[2];
			det *= pa[0] * (pb[1] * pc[2] - pb[2] * pc[1]) - pa[1] * (pb[0] * pc[2] - pb[2] * pc[0]) +
				   pa[2] * (pb[0] * pc[1] - pb[1] * pc[0]);
		}
		if (det > 0)
			for (float &x : g.ax[0])
				x = -x;
		for (int i = 0; i < 3; i++)
			g.m[i] = g.ax[i];
		g.s = (w[1] >> 8) & 15;
		g.k = std::ldexp(1.0f, g.s);
		g.base = colour((w[0] >> 4) & 0xfff, shade(r, n), r.base);
		g.shade0 = o.tint[3];
		P3 line = *org;
		for (unsigned char ch : it->second)
		{
			std::memset(g.t, 0, sizeof g.t);
			std::memset(g.tsrc, 0, sizeof g.tsrc);
			g.t[0] = (uint16_t)(r.t[0] + 1);
			g.tsrc[0] = r.tsrc[0];
			g.pv[0] = g.org;
			int i = (int)(signed char)ch - 0x20;
			if (i < 0)
			{
				g.c = code_for(FE2_gamedata2, font);
				if (!g.c)
					return;
				g.org = line;
				auto p = vertex(g, fe2::s8(img.u16(*fa + 30)));
				if (!p)
					return;
				g.org = line = *p;
				continue;
			}
			g.c = code_for(FE2_gamedata2, font, *fa + (uint32_t)img.u16(*fa + 32 + 2 * (uint32_t)i));
			if (!g.c || g.c->ins.empty())
				return;
			run(g, depth + 1);
			int next = g.c->ins.back().w[0] >> 6;
			if (next)
			{
				auto p = vertex(g, next);
				if (!p)
					return;
				g.org = *p;
			}
		}
	}

	void run(Run &r, int depth)
	{
		if (depth > 8)
			return;
		const Code &c = *r.c;
		size_t i = 0;
		for (int steps = 0; i < c.ins.size() && steps < 4096; steps++)
		{
			const fe2::Instr &ins = c.ins[i];
			const auto &w = ins.w;
			bool jump = false;
			switch (ins.op)
			{
			case 0x00:
				return;
			case 0x0b: /* if > z, or skip unless normal faces */
			case 0x0c:
				if (w[1] & 0x8000)
				{
					bool faces = normal_faces(r, w[1] & 0x7f);
					jump = !sky && !casting && (ins.op == 0x0b ? !faces : faces);
				}
				else
				{
					auto far = beyond(r, w[1]);
					jump = ins.op == 0x0b ? far.value_or(false) : !far.value_or(false);
				}
				break;
			case 0x13: /* skip unless bit; 0 = false, bit numbers start at 1 */
			case 0x14:
			{
				int p = w[1] & 0xff, bit = w[1] >> 8;
				/* an object variable's bit: the cache keeps the answer, not the value */
				bool objvar = (p & 0xc0) == 0x80 && (p & 0x3f) < NUM_VARS;
				int v = objvar ? o.vars[p & 0x3f] : param(r, p);
				bool set = v != 0 && (bit == 0 || (v >> (bit - 1)) & 1);
				if (rec && objvar)
					rec->bits.push_back({p & 0x3f, bit, set});
				jump = ins.op == 0x13 ? !set : set;
				break;
			}
			case 0x17: /* ifdist */
				jump = apart(r, fe2::s8(w[2]), fe2::s8(w[2] >> 8), w[1]);
				break;
			case 0x0d:
			case 0x1d:
			{
				/* bits 4-7, the lowest being the op's own bit 4: $0d has the
				 * even functions, $1d the odd ones */
				int fn = (w[0] >> 4) & 15;
				int ti = (w[0] >> 8) & 7;
				r.t[ti] = (uint16_t)calc(fn, param(r, w[1] & 0xff), param(r, w[1] >> 8));
				r.tsrc[ti] = 0;
				break;
			}
			case 0x1a: /* colour (L3a0f4): the base colour, or normal 0's */
			{
				int col = (w[0] >> 4) & 0xffe;
				int sel = w[1] >> 8;
				if (sel)
				{
					int v = param(r, sel) & 7;
					if (v && (size_t)(v + 1) < w.size())
						col = w[v + 1];
				}
				col = colour(col, shade(r, w[1] & 0x7f), r.base);
				if (w[1] & 0x80)
					r.shade0 = col;
				else
					r.base = col;
				break;
			}
			case 0x0e:
			case 0x1b:
				call(r, ins, depth);
				break;
			case 0x0a:
				text(r, ins, depth);
				break;
			case 0x06:
				begin_batch(r, ins);
				break;
			case 0x15:
				ztree(r, ins);
				break;
			case 0x19:
				msetup(r, w[0]);
				break;
			case 0x1c:
				mrotate(r, ins);
				break;
			case 0x1e:
				mcopy(r, w[0]);
				break;
			case 0x18:
				blobs(r, ins);
				break;
			case 0x01:
				circle(r, ins);
				break;
			case 0x09:
				teardrop(r, ins);
				break;
			case 0x1f: /* the planet (L3cd9c), at its vertex (the low byte of
						  the third word) */
				if (auto p = vertex(r, fe2::s8(w.size() > 2 ? w[2] : 0)))
				{
					Node &n = new_node(zkey(*p), false, *p);
					n.planet = true;
					/* the radius word * 2^(s-8) (L3cdd8 on) */
					n.radius = (w.size() > 1 ? (float)w[1] : 0) * r.k / 256;
				}
				break;
			default:
				emit(r, i);
				break;
			}
			if (!jump)
			{
				i++;
				continue;
			}
			if (ins.jump < 0)
				return; /* a jump of 0 ends the model */
			auto it = c.at.find(ins.jump);
			if (it == c.at.end())
				return;
			i = it->second;
		}
	}
};

void set_colour(int rgb444)
{
	/* bit 15: a cloud's shadow, translucent black over what is under it */
	static bool blended;
	bool sh = rgb444 & 0x8000;
	if (sh != blended)
		gd_set_blend(blended = sh);
	if (sh)
	{
		gd_color4ub(0, 0, 0, 110);
		return;
	}
	Rgb8 c = rgb444_to_rgb8(rgb444 & 0xfff);
	gd_color3ub(c.r, c.g, c.b);
}

/* Each object's models, run */
std::vector<Drawn> drawn;

uint32_t frame_seed;

/* Runs object o's model into d; the sky round the camera. With pl, in the
 * object's own axes (a cached run, rec), false when what it made is good
 * for this frame only. */
bool run_object(const Object &o, Drawn &d, bool wire, bool sky, bool casting = false, const Place *pl = nullptr,
				Rec *rec = nullptr)
{
	d = Drawn();
	const Code *c = code_for(o.table, o.model);
	if (!c)
		return true;
	Runner rn{o, d, wire, c->model.scale + c->model.scale2 - o.scale, frame_seed};
	rn.sky = sky;
	rn.casting = casting;
	rn.pl = pl;
	rn.rec = rec;
	Run r;
	r.c = c;
	if (pl)
	{
		/* the light in the object's axes: as msetup dots it with them */
		r.org = P3{0, 0, 0};
		for (int a = 0; a < 3; a++)
		{
			r.ax[a] = P3{0, 0, 0};
			r.ax[a][(size_t)a] = 1;
			rn.L[(size_t)a] = o.cols[a][0] * o.light[0] + o.cols[a][1] * o.light[1] + o.cols[a][2] * o.light[2];
		}
	}
	else
	{
		r.org = sky ? P3{0, 0, 0} : P3{o.pos[0], o.pos[1], o.pos[2]};
		for (int a = 0; a < 3; a++)
			r.ax[a] = {o.cols[a][0], o.cols[a][1], o.cols[a][2]};
		rn.L = {o.light[0], o.light[1], o.light[2]};
	}
	r.s = o.scale;
	r.k = std::ldexp(1.0f, o.scale);
	r.base = 0x111; /* L390f2 */
	r.shade0 = o.tint[3];
	std::memcpy(r.t, o.t, sizeof r.t);
	for (int a = 0; a < 3; a++)
		r.m[a] = r.ax[a];
	r.pv[0] = r.org;
	r.top = true;
	for (int i = 0; i < 8; i++)
		r.tsrc[i] = (uint8_t)(1u << i);
	rn.run(r, 0);
	return !rn.vol;
}

/* ---- the cache of runs (Rec, Place) ---- */

/* GLF_NATIVE_CACHE=0: every object's model runs every frame, as before */
bool cache_on()
{
	static const char *env = getenv("GLF_NATIVE_CACHE");
	return !env || atoi(env) != 0;
}

/* What a run starts from besides the pose: the same, the same run */
struct CacheKey
{
	uint32_t table, subtable;
	int model, scale, boost, clouds;
	int detail[2];
	bool casting;
	uint16_t tint[8];
};

struct Cached
{
	CacheKey key;
	bool made = false;
	bool vol = false; /* its runs are good for one frame only: run every frame */
	uint32_t seen = 0;
	P3 light;         /* the light in the object's axes when it ran */
	uint16_t vars[NUM_VARS], t[8]; /* the object's when it ran: those it read (Rec.vread, .tread) must stay */
	Drawn d;          /* in the object's axes; triangle colours are face indexes */
	Rec rec;
	/* d's triangles on the GPU (GLF_NATIVE_GPU), node by node: those of a
	 * node that is no underlay and has no cloud shadows */
	struct GNode
	{
		bool gpu = false;
		int first = 0, count = 0; /* vertices in mesh */
		P3 lo, hi;                /* the box round them */
		std::vector<uint16_t> faces; /* the faces they belong to */
	};
	GdMesh *mesh = nullptr;
	std::vector<GNode> gn;
};
std::unordered_map<uint64_t, Cached> cache;

/* An object's GPU nodes this frame: its cache entry, pose, face colours */
struct GpuDraw
{
	const Cached *e = nullptr;
	Place pl;
	float pose[16];
	int base = 0;        /* its faces' first entry in the face table */
	std::vector<int> fc; /* each face's colour (face_colour), for drawing on the CPU */
	bool bent = false;   /* bent onto a planet (Bend): bend holds gd_mesh_draw's numbers */
	float bend[6][4] = {};
};
/* The face table of this frame's objects (gd_face_colours), RGBA8 */
std::vector<uint8_t> face_rgba;

/* GLF_NATIVE_GPU=0: cached runs placed on the CPU every frame (stage 1) */
bool gpu_on()
{
	static const char *env = getenv("GLF_NATIVE_GPU");
	return cache_on() && (!env || atoi(env) != 0);
}

/* Puts e's triangles on the GPU (e.gn, e.mesh) */
void build_mesh(Cached &e)
{
	static std::vector<float> v;
	v.clear();
	e.gn.assign(e.d.nodes.size(), Cached::GNode());
	for (size_t ni = 0; ni < e.d.nodes.size(); ni++)
	{
		const Node &s = e.d.nodes[ni];
		Cached::GNode &g = e.gn[ni];
		if (s.under || s.planet || s.tris.empty())
			continue;
		bool shadows = false;
		for (const Tri &t : s.tris)
			shadows |= t.colour >= e.rec.faces.size() || e.rec.faces[t.colour].shadow;
		if (shadows)
			continue;
		g.gpu = true;
		g.first = (int)(v.size() / 4);
		g.lo = {1e30f, 1e30f, 1e30f};
		g.hi = {-1e30f, -1e30f, -1e30f};
		for (const Tri &t : s.tris)
		{
			g.faces.push_back(t.colour);
			for (const P3 &p : t.p)
			{
				v.insert(v.end(), {p[0], p[1], p[2], (float)t.colour});
				for (int k = 0; k < 3; k++)
				{
					g.lo[k] = std::min(g.lo[k], p[k]);
					g.hi[k] = std::max(g.hi[k], p[k]);
				}
			}
		}
		std::sort(g.faces.begin(), g.faces.end());
		g.faces.erase(std::unique(g.faces.begin(), g.faces.end()), g.faces.end());
		g.count = (int)(v.size() / 4) - g.first;
	}
	if (v.empty())
	{
		e.gn.clear();
		return;
	}
	if (!e.mesh)
		e.mesh = gd_mesh_new();
	gd_mesh_upload(e.mesh, v.data(), (int)(v.size() / 4));
}

/* A GPU node's triangles as the CPU would have placed them (the self-check, wireframe) */
void gpu_tris(const Node &n, std::vector<Tri> &out)
{
	if (!n.g || !n.gcount)
		return;
	const GpuDraw &g = *n.g;
	for (const Tri &t : g.e->d.nodes[(size_t)n.gnode].tris)
	{
		int c = g.fc[t.colour];
		if (c >= 0)
			out.push_back({{g.pl.at(t.p[0]), g.pl.at(t.p[1]), g.pl.at(t.p[2])}, (uint16_t)c});
	}
}

/* The corners of a GPU node's box, view space */
template <class F> void gpu_corners(const Node &n, F f)
{
	const Cached::GNode &gn = n.g->e->gn[(size_t)n.gnode];
	for (int i = 0; i < 8; i++)
		f(n.g->pl.at({(i & 1) ? gn.hi[0] : gn.lo[0], (i & 2) ? gn.hi[1] : gn.lo[1], (i & 4) ? gn.hi[2] : gn.lo[2]}));
}
uint32_t cache_tick;
std::unordered_map<std::string, int> cache_why;

void key_of(const Object &o, bool casting, CacheKey &k)
{
	std::memset(&k, 0, sizeof k);
	k.table = o.table;
	k.subtable = o.subtable;
	k.model = o.model;
	k.scale = o.scale;
	k.boost = native_detail_boost;
	static const char *env = getenv("GLF_CLOUD_SHADOWS");
	k.clouds = env ? atoi(env) : native_cloud_shadows;
	k.detail[0] = o.detail[0];
	k.detail[1] = o.detail[1];
	k.casting = casting;
	std::memcpy(k.tint, o.tint, sizeof k.tint);
}

Place place_of(const Object &o)
{
	Place pl;
	pl.pos = {o.pos[0], o.pos[1], o.pos[2]};
	for (int a = 0; a < 3; a++)
		pl.ax[a] = {o.cols[a][0], o.cols[a][1], o.cols[a][2]};
	return pl;
}

P3 light_in(const Object &o)
{
	P3 l;
	for (int a = 0; a < 3; a++)
		l[(size_t)a] = o.cols[a][0] * o.light[0] + o.cols[a][1] * o.light[1] + o.cols[a][2] * o.light[2];
	return l;
}

/* Whether the run's answers on the view are the same for this one */
bool cache_inputs_same(const Object &o, const Cached &e)
{
	for (int i = 0; i < NUM_VARS; i++)
		if ((e.rec.vread[i >> 6] >> (i & 63) & 1) && o.vars[i] != e.vars[i])
			return false;
	for (int i = 0; i < 8; i++)
		if ((e.rec.tread >> i & 1) && o.t[i] != e.t[i])
			return false;
	for (const Rec::Bit &b : e.rec.bits)
	{
		int v = o.vars[b.var];
		if ((v != 0 && (b.bit == 0 || (v >> (b.bit - 1)) & 1)) != b.set)
			return false;
	}
	for (const auto &tx : e.rec.texts)
	{
		auto it = shown->texts.find(tx.first);
		if ((it == shown->texts.end() ? std::string("\1") : it->second) != tx.second)
			return false;
	}
	return true;
}

bool cache_holds(const Object &o, const Place &pl, const Cached &e)
{
	if (e.rec.light)
	{
		/* a cloud's shadow moves with the light: about 0.06 degrees */
		P3 l = light_in(o);
		P3 d = {l[0] - e.light[0], l[1] - e.light[1], l[2] - e.light[2]};
		if (dot(d, d) > 1e-6f)
			return false;
	}
	for (const Rec::Dist &g : e.rec.dist)
	{
		P3 p = pl.at(g.at);
		if ((std::sqrt(dot(p, p)) > g.lim) != g.far)
			return false;
	}
	for (const Rec::Facing &g : e.rec.facing)
		if ((dot(pl.dir(g.n), pl.at(g.p)) < 0) != g.faces)
			return false;
	for (const Rec::Apart &g : e.rec.apart)
		if (apart_jump(pl.at(g.p), pl.at(g.q), g.w1) != g.jump)
			return false;
	for (const Rec::Shade &g : e.rec.shade)
		if (shade_eval(o, &pl, g.s) != g.v)
			return false;
	return true;
}

/* A face's colour in this view, -1 when it faces away */
int face_colour(const Object &o, const Place &pl, const CFace &f)
{
	P3 n = pl.dir(f.n);
	if (f.cull && dot(n, pl.at(f.a)) >= 0)
		return -1;
	int sh = f.tint_n ? tint_of(o, n) : shade_eval(o, &pl, f.sh);
	int col = Runner::colour(f.colour, sh, f.base);
	return f.shadow ? col | 0x8000 : col;
}

/* What the cached run made, placed by this frame's pose: as a run in the
 * view would have made it; with gpu, its GPU nodes (Cached.gn) only point
 * at their triangles there (Node.g) */
void cache_place(const Object &o, const Place &pl, const Cached &e, Drawn &d, bool gpu)
{
	/* d's buffers are kept (mostly the object's last frame): only cleared */
	d.hide = false;
	d.wire.clear();
	d.wire_dots.clear();
	d.near = 1e30f;
	d.far = 0;
	d.shrink = 1;
	d.back = false;
	for (Node &n : d.before_planet)
		d.nodes.push_back(std::move(n));
	d.before_planet.clear();
	d.nodes.resize(e.d.nodes.size());
	static std::vector<int> fc;
	fc.resize(e.rec.faces.size());
	for (size_t i = 0; i < fc.size(); i++)
		fc[i] = face_colour(o, pl, e.rec.faces[i]);
	GpuDraw *g = nullptr;
	if (gpu && !e.gn.empty())
	{
		if (!d.gpu)
			d.gpu = std::make_shared<GpuDraw>();
		g = d.gpu.get();
		g->e = &e;
		g->pl = pl;
		float *m = g->pose;
		for (int a = 0; a < 3; a++)
		{
			for (int k = 0; k < 3; k++)
				m[a * 4 + k] = pl.ax[a][k];
			m[a * 4 + 3] = 0;
		}
		for (int k = 0; k < 3; k++)
			m[12 + k] = pl.pos[k];
		m[15] = 1;
		g->fc = fc;
		g->base = (int)(face_rgba.size() / 4);
		for (int c : fc)
		{
			if (c < 0)
			{
				face_rgba.insert(face_rgba.end(), {0, 0, 0, 0});
				continue;
			}
			Rgb8 k = rgb444_to_rgb8(c & 0xfff);
			face_rgba.insert(face_rgba.end(), {(uint8_t)k.r, (uint8_t)k.g, (uint8_t)k.b, 255});
		}
	}
	for (size_t ni = 0; ni < e.d.nodes.size(); ni++)
	{
		const Node &s = e.d.nodes[ni];
		Node &n = d.nodes[ni];
		n.g = nullptr;
		n.gcount = 0;
		n.gnode = -1;
		n.tris.clear();
		n.segs.clear();
		n.dots.clear();
		n.dist = 0;
		n.kpush = s.kpush;
		n.kcam = s.kcam;
		n.seq = s.seq;
		n.depth = s.depth;
		n.under = s.under;
		n.planet = s.planet;
		n.radius = s.radius;
		/* the keys again from their points (Runner::new_node) */
		for (int i = 0; i < s.depth; i++)
		{
			P3 p = (s.kcam >> i) & 1 ? P3{0, 0, 0} : pl.at(s.kpt[(size_t)i]);
			n.kpt[(size_t)i] = p;
			n.path[(size_t)i] = Runner::zkey(p) + s.kpush[(size_t)i];
		}
		n.key = s.depth > 0 ? n.path[(size_t)(s.depth > 1 ? 0 : s.depth - 1)] : s.key;
		if (g && e.gn[ni].gpu)
		{
			const Cached::GNode &gn = e.gn[ni];
			n.g = g;
			n.gnode = (int)ni;
			n.gfirst = gn.first;
			for (uint16_t f : gn.faces)
				if (fc[f] >= 0)
				{
					n.gcount = gn.count;
					break;
				}
		}
		else
			n.tris.reserve(s.tris.size());
		static const std::vector<Tri> none;
		for (const Tri &t : n.g ? none : s.tris)
		{
			int c = fc[t.colour];
			if (c < 0)
				continue;
			n.tris.push_back({{pl.at(t.p[0]), pl.at(t.p[1]), pl.at(t.p[2])}, (uint16_t)c});
		}
		n.segs.reserve(s.segs.size());
		for (const Seg &g : s.segs)
			n.segs.push_back({pl.at(g.a), pl.at(g.b), g.colour});
		for (const Dot &g : s.dots)
		{
			Dot q = g;
			if (g.circ >= 0)
			{
				if (circle_eval(o, pl.at(g.p), e.rec.circles[(size_t)g.circ], q))
					n.dots.push_back(q);
				continue;
			}
			q.p = pl.at(g.p);
			n.dots.push_back(q);
		}
	}
	for (const CTear &t : e.rec.tears)
		tear_eval(pl.at(t.head), pl.at(t.tail), t.width, t.c, d.nodes[t.node]);
	if (e.rec.live.empty())
		return;
	const Code *top = code_for(o.table, o.model);
	if (!top)
		return;
	for (const Rec::Live &lv : e.rec.live)
	{
		Runner rn{o, d, false, top->model.scale + top->model.scale2 - o.scale, frame_seed};
		rn.casting = e.key.casting;
		rn.L = {o.light[0], o.light[1], o.light[2]};
		rn.batch = lv.batch;
		rn.shadow_node = lv.shadow_node;
		for (const P3 &p : lv.zpt)
		{
			rn.zpt.push_back(pl.at(p));
			rn.zpath.push_back(Runner::zkey(rn.zpt.back()));
		}
		Run c = lv.c;
		c.org = pl.at(c.org);
		for (int i = 0; i < 3; i++)
		{
			c.ax[i] = pl.dir(c.ax[i]);
			c.m[i] = pl.dir(c.m[i]);
		}
		for (auto &v : c.pv)
			if (v)
				v = pl.at(*v);
		rn.run(c, lv.depth);
	}
}

void cache_check(const Object &o, bool casting, const Drawn &got, const char *how);

/* Object o's model (filled, not the sky) into d, from the cache when the
 * run it holds still holds; ran / reused counted */
void cached_run(const Object &o, Drawn &d, bool casting, int &ran, int &reused)
{
	/* the shadow casters' runs stay on the CPU (setup_shadows reads their triangles) */
	const bool gpu = !casting && gpu_on();
	if (!cache_on())
	{
		run_object(o, d, false, false, casting);
		ran++;
		return;
	}
	CacheKey k;
	key_of(o, casting, k);
	uint64_t id = (uint64_t)o.addr << 1 | (casting ? 1u : 0u);
	Cached &e = cache[id];
	e.seen = cache_tick;
	Place pl = place_of(o);
	bool same = e.made && std::memcmp(&e.key, &k, sizeof k) == 0 && cache_inputs_same(o, e);
	static const bool dbg = getenv("GLF_NATIVE_CACHE") && atoi(getenv("GLF_NATIVE_CACHE")) == 2;
	if (dbg)
	{
		/* why each model runs again (GLF_NATIVE_CACHE=2), counted: logged by cache_trim */
		std::string why;
		if (!e.made)
			why = "new";
		else if (std::memcmp(&e.key, &k, sizeof k) != 0)
		{
			why = e.key.scale != k.scale ? "scale" : e.key.detail[0] != k.detail[0] || e.key.detail[1] != k.detail[1] ? "detail" : "key";
		}
		else if (e.vol)
			why = std::string("volatile ") + (e.rec.why ? e.rec.why : "");
		else
		{
			for (int i = 0; i < NUM_VARS && why.empty(); i++)
				if ((e.rec.vread[i >> 6] >> (i & 63) & 1) && o.vars[i] != e.vars[i])
					why = "var " + std::to_string(i);
			for (int i = 0; i < 8 && why.empty(); i++)
				if ((e.rec.tread >> i & 1) && o.t[i] != e.t[i])
					why = "t" + std::to_string(i);
			if (why.empty() && !cache_inputs_same(o, e))
				why = "bit/text";
			if (why.empty() && !cache_holds(o, pl, e))
				why = "test";
		}
		if (!why.empty())
			cache_why[std::to_string(o.model) + " " + why]++;
	}
	if (same && !e.vol && cache_holds(o, pl, e))
	{
		cache_place(o, pl, e, d, gpu);
		reused++;
		cache_check(o, casting, d, "reused");
		return;
	}
	ran++;
	if (same && e.vol)
	{
		run_object(o, d, false, false, casting);
		return;
	}
	e.key = k;
	e.made = true;
	e.rec = Rec();
	e.light = light_in(o);
	std::memcpy(e.vars, o.vars, sizeof e.vars);
	std::memcpy(e.t, o.t, sizeof e.t);
	e.vol = !run_object(o, e.d, false, false, casting, &pl, &e.rec);
	if (e.vol)
	{
		/* kept: what it read, to know when to try again */
		e.d = Drawn();
		e.rec.dist.clear();
		e.rec.facing.clear();
		e.rec.apart.clear();
		e.rec.shade.clear();
		e.rec.faces.clear();
		e.rec.circles.clear();
		e.rec.tears.clear();
		e.rec.live.clear();
		e.gn.clear();
		run_object(o, d, false, false, casting);
		return;
	}
	if (gpu)
		build_mesh(e);
	else
		e.gn.clear();
	cache_place(o, pl, e, d, gpu);
	cache_check(o, casting, d, "made");
}

/* GLF_NATIVE_CACHE=3: each cached object also run as before, and what both
 * made compared (counts, colours, points); the differences are logged */
void cache_check(const Object &o, bool casting, const Drawn &got, const char *how)
{
	static const bool on = getenv("GLF_NATIVE_CACHE") && atoi(getenv("GLF_NATIVE_CACHE")) == 3;
	if (!on)
		return;
	static Drawn want;
	run_object(o, want, false, false, casting);
	/* GPU nodes: their triangles as the CPU would have placed them, before the node's own */
	static Drawn cpu;
	bool any_gpu = false;
	for (const Node &n : got.nodes)
		any_gpu |= n.g != nullptr;
	if (any_gpu)
	{
		cpu = got;
		for (Node &n : cpu.nodes)
			if (n.g)
			{
				std::vector<Tri> t;
				gpu_tris(n, t);
				t.insert(t.end(), n.tris.begin(), n.tris.end());
				n.tris = std::move(t);
			}
	}
	const Drawn &have = any_gpu ? cpu : got;
	struct Sum
	{
		size_t tris = 0, segs = 0, dots = 0, nodes = 0;
		double colour = 0, p = 0, key = 0;
	};
	auto sum = [](const Drawn &d) {
		Sum s;
		for (const Node &n : d.nodes)
		{
			s.nodes++;
			s.key += n.depth > 0 ? n.path[0] : 0;
			for (const Tri &t : n.tris)
			{
				s.tris++;
				s.colour += t.colour;
				for (const P3 &q : t.p)
					s.p += std::fabs(q[0]) + std::fabs(q[1]) + std::fabs(q[2]);
			}
			for (const Seg &g : n.segs)
			{
				s.segs++;
				s.colour += g.colour;
			}
			for (const Dot &g : n.dots)
			{
				s.dots++;
				s.colour += g.colour + g.px;
			}
		}
		return s;
	};
	Sum a = sum(have), b = sum(want);
	static int bad, good;
	bool same = a.tris == b.tris && a.segs == b.segs && a.dots == b.dots && a.colour == b.colour &&
				std::fabs(a.p - b.p) <= 1e-3 * std::max(1.0, b.p);
	if (!same && bad < 3 && have.nodes.size() == want.nodes.size())
		for (size_t i = 0; i < have.nodes.size(); i++)
		{
			const Node &x = have.nodes[i], &y = want.nodes[i];
			bool found = false;
			for (size_t j = 0; j < x.tris.size() && j < y.tris.size() && !found; j++)
				for (int v = 0; v < 3 && !found; v++)
					for (int c = 0; c < 3; c++)
						if (std::fabs(x.tris[j].p[v][c] - y.tris[j].p[v][c]) > 1e-3f * (1 + std::fabs(y.tris[j].p[v][c])))
						{
							log_printf("  node %zu (under %d, colour %x) tri %zu: %.1f %.1f %.1f vs %.1f %.1f %.1f\n", i, (int)y.under,
									   y.tris[j].colour, j, x.tris[j].p[v][0], x.tris[j].p[v][1], x.tris[j].p[v][2],
									   y.tris[j].p[v][0], y.tris[j].p[v][1], y.tris[j].p[v][2]);
							found = true;
							break;
						}
			if (found)
				break;
		}
	if (same)
		good++;
	else if (bad++ < 40)
		log_printf("native cache check: model %d (%s%s) differs: tris %zu/%zu segs %zu/%zu dots %zu/%zu colour %.0f/%.0f points %.6g/%.6g nodes %zu/%zu\n",
				   o.model, how, casting ? ", casting" : "", a.tris, b.tris, a.segs, b.segs, a.dots, b.dots, a.colour, b.colour,
				   a.p, b.p, a.nodes, b.nodes);
	if ((good + bad) % 2000 == 0)
		log_printf("native cache check: %d same, %d different\n", good, bad);
}

/* Entries of objects not seen for a while go */
void cache_trim()
{
	if (!cache_why.empty() && cache_tick % 300 == 0)
	{
		std::vector<std::pair<int, std::string>> v;
		for (auto &w : cache_why)
			v.push_back({w.second, w.first});
		std::sort(v.rbegin(), v.rend());
		std::string line;
		for (size_t i = 0; i < v.size() && i < 14; i++)
			line += " " + v[i].second + "=" + std::to_string(v[i].first);
		log_printf("native cache runs (model why=count over 300):%s\n", line.c_str());
		cache_why.clear();
	}
	if (++cache_tick % 64)
		return;
	for (auto it = cache.begin(); it != cache.end();)
		if (cache_tick - it->second.seen > 600)
		{
			gd_mesh_free(it->second.mesh);
			it = cache.erase(it);
		}
		else
			++it;
}

int prof_ran, prof_reused;

void run_objects(bool wire)
{
	frame_seed++;
	cache_trim();
	face_rgba.clear();
	drawn.resize((size_t)shown->n);
	for (int i = 0; i < shown->n; i++)
	{
		const Object &o = shown->obj[i];
		const Code *c = code_for(o.table, o.model);
		/* filled, planets are drawn with the sky (draw_fars) */
		if (c && c->planet && !wire)
			drawn[(size_t)i] = Drawn();
		else if (wire)
		{
			run_object(o, drawn[(size_t)i], wire, false);
			prof_ran++;
		}
		else
			cached_run(o, drawn[(size_t)i], false, prof_ran, prof_reused);
		drawn[(size_t)i].shrink = o.shrink;
		drawn[(size_t)i].hide = o.own_added;
	}
}

void sort_nodes(std::vector<Node> &nodes);

/* Ground objects bent onto the planet. A city (Mars' model 48) is one flat sheet some 40 km wide, laid flat at
 * its centre; the planet curves away under it, 15 m at 10 km out, 50 m at 20 km on Mars. The game drew the ground
 * at the surface as a flat fill and painted the city over it, so that never showed; over the native sphere the
 * outskirts' roads and buildings float, with sky and their shadows under them. Each point of an object near a
 * planet is lowered by the sphere's fall from the plane it touches at the object's origin: R - sqrt(R^2 - w^2),
 * w the point's distance from the vertical through the origin. A ship or a hangar moves by a fraction of a mm;
 * a cloud field follows the curve too. GLF_GROUND_BEND=0 turns it off. */
#define NATIVE_BEND_PLANETS 8
struct Bend
{
	bool on = false;
	P3 b;         /* the object's origin (view units) */
	P3 u;         /* up there, unit */
	double R = 0; /* the planet's centre to b */
};
std::vector<Bend> bends; /* by drawn index, this game frame */

void bend_point(const Bend &bd, P3 &p)
{
	double d[3] = {(double)p[0] - bd.b[0], (double)p[1] - bd.b[1], (double)p[2] - bd.b[2]};
	double h = d[0] * bd.u[0] + d[1] * bd.u[1] + d[2] * bd.u[2];
	double w2 = std::max(d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - h * h, 0.0);
	double drop = w2 / (bd.R + std::sqrt(std::max(bd.R * bd.R - w2, 0.0)));
	for (int k = 0; k < 3; k++)
		p[k] -= (float)(bd.u[k] * drop);
}

void bend_nodes(const Bend &bd, std::vector<Node> &nodes)
{
	if (!bd.on)
		return;
	for (Node &n : nodes)
	{
		for (Tri &t : n.tris)
			for (P3 &p : t.p)
				bend_point(bd, p);
		for (Seg &s : n.segs)
		{
			bend_point(bd, s.a);
			bend_point(bd, s.b);
		}
		for (Dot &dt : n.dots)
			bend_point(bd, dt.p);
	}
}

/* The GPU meshes' numbers (gd_mesh_draw): the bend in the mesh's own units */
void bend_gpu(const Bend &bd, GpuDraw &g)
{
	g.bent = false;
	if (!bd.on)
		return;
	const Place &pl = g.pl;
	/* A^-1 of the pose's axes (columns ax[0..2]) */
	double a[3][3], inv[3][3];
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			a[r][c] = pl.ax[c][r];
	double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
				 a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
	if (std::fabs(det) < 1e-30)
		return;
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
		{
			int r1 = (c + 1) % 3, r2 = (c + 2) % 3, c1 = (r + 1) % 3, c2 = (r + 2) % 3;
			inv[r][c] = (a[r1][c1] * a[r2][c2] - a[r1][c2] * a[r2][c1]) / det;
		}
	double bp[3] = {(double)bd.b[0] - pl.pos[0], (double)bd.b[1] - pl.pos[1], (double)bd.b[2] - pl.pos[2]};
	for (int k = 0; k < 3; k++)
	{
		for (int j = 0; j < 3; j++)
			g.bend[k][j] = pl.ax[k][j];
		g.bend[k][3] = 0;
		g.bend[3][k] = (float)(inv[k][0] * bp[0] + inv[k][1] * bp[1] + inv[k][2] * bp[2]);
		g.bend[4][k] = bd.u[k];
		g.bend[5][k] = (float)(inv[k][0] * bd.u[0] + inv[k][1] * bd.u[1] + inv[k][2] * bd.u[2]);
	}
	g.bend[3][3] = (float)bd.R;
	g.bend[4][3] = 1;
	g.bend[5][3] = 0;
	g.bent = true;
}

/* Each object's bend: the nearest planet whose ground it is within 60 km above (or 2 km below) */
void make_bends(bool wire)
{
	bends.assign(drawn.size(), Bend());
	static const bool off = getenv("GLF_GROUND_BEND") && atoi(getenv("GLF_GROUND_BEND")) == 0;
	if (off || wire)
		return;
	double c[NATIVE_BEND_PLANETS][3], r[NATIVE_BEND_PLANETS];
	int np = 0;
	for (int i = 0; i < planet_native_count() && np < NATIVE_BEND_PLANETS; i++)
		if (planet_native_ground(i, c[np], &r[np]))
			np++;
	if (!np)
		return;
	for (size_t i = 0; i < drawn.size() && i < (size_t)shown->n; i++)
	{
		const Object &o = shown->obj[i];
		Drawn &d = drawn[i];
		if (d.shrink != 1.0f || o.table != o.subtable || planet_model(o))
			continue;
		Bend &bd = bends[i];
		double best = 60e6;
		for (int j = 0; j < np; j++)
		{
			double v[3] = {o.pos[0] - c[j][0], o.pos[1] - c[j][1], o.pos[2] - c[j][2]};
			double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
			double h = len - r[j];
			if (len <= 0 || h < -2e6 || h >= best)
				continue;
			best = h;
			bd.on = true;
			bd.b = {o.pos[0], o.pos[1], o.pos[2]};
			for (int k = 0; k < 3; k++)
				bd.u[k] = (float)(v[k] / len);
			bd.R = len;
		}
		bend_nodes(bd, d.nodes);
		bend_nodes(bd, d.before_planet);
		if (d.gpu)
			bend_gpu(bd, *d.gpu);
	}
}

/* Ship shadows (gl_shadow.h): the ships in view cast, the rest of the scene
 * receives (not the ships themselves). Shrunk (far) objects stay out, their
 * space is not the others'. A flyable ship's model is 10-41. */
std::vector<float> shadow_tris;
std::vector<char> shadow_caster;
/* the layer of each cascade's map an object looks up (gl_shadow_layer, gl_shadow_all_layer):
 * every caster for what is no caster, the map without itself for a ship, -1 = none */
struct ShadowLayers
{
	int l[GL_SHADOW_CASCADES];
};
std::vector<ShadowLayers> shadow_layer;
bool shadow_on = false;

void setup_shadows(bool wire)
{
	gl_shadow_clear();
	shadow_on = false;
	planet_set_ground_ship(false, nullptr, 1);
	for (size_t i = 0; i < (size_t)shown->n; i++)
		if (shown->obj[i].addr == game_player_ship() && shown->obj[i].table == shown->obj[i].subtable)
			planet_set_ground_ship(true, shown->obj[i].pos, shown->obj[i].shrink);
	shadow_caster.assign(drawn.size(), 0);
	shadow_layer.assign(drawn.size(), ShadowLayers{{-1, -1, -1}});
	if (wire || !gl_shadow_wanted())
		return;
	shadow_tris.clear();
	/* The map is fitted round the casters, so a ship far off would stretch it
	 * and make every texel metres wide: only the ships near the nearest one
	 * (within a few of its sizes, at least 100 m) cast */
	struct Cand
	{
		size_t i;
		P3 c;
		float r;
		bool station; /* a station or base (models 43-54): always in the big cascade */
		bool scenery; /* a mountain or building (models 101-104): own cascade (2), casts only, near the eye */
		bool base;	  /* a ground base (a station model many km wide): only its triangles near the eye, in cascade 2 */
	};
	std::vector<Cand> cands;
	for (size_t i = 0; i < drawn.size() && i < (size_t)shown->n; i++)
	{
		const Object &o = shown->obj[i];
		const Drawn &d = drawn[i];
		const bool station = o.model >= 43 && o.model <= 54;
		const bool scenery = o.model >= 101 && o.model <= 104;
		if (o.table != o.subtable || (!station && !scenery && (o.model < 10 || o.model > 41)) || d.shrink != 1.0f)
			continue;
		P3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
		bool any = false;
		auto add = [&](const P3 &p) {
			for (int k = 0; k < 3; k++)
			{
				lo[k] = std::min(lo[k], p[k]);
				hi[k] = std::max(hi[k], p[k]);
			}
			any = true;
		};
		for (const Node &n : d.nodes)
			if (!n.under)
			{
				if (n.g && n.gcount)
					gpu_corners(n, add);
				for (const Tri &t : n.tris)
					for (const P3 &p : t.p)
						add(p);
			}
		if (!any)
			continue;
		Cand c = {i, {}, 0, station, scenery, false};
		for (int k = 0; k < 3; k++)
		{
			c.c[k] = 0.5f * (lo[k] + hi[k]);
			c.r += 0.25f * (hi[k] - lo[k]) * (hi[k] - lo[k]);
		}
		c.r = std::sqrt(c.r);
		c.base = station && c.r > 5.0e6f; /* an orbital station is a couple of km, a ground base a whole town */
		if (c.base)
		{
			/* only its parts within 2.5 km of the eye cast (below): none near, it is not run again whole
			 * for nothing (49000 triangles placed and thrown away each frame, flying over Mars).
			 * The nearest of the drawn parts' boxes, 1 km to spare for faces turned away. */
			float nearest = 1e30f;
			auto box_dist = [&](const P3 &blo, const P3 &bhi) {
				float s = 0;
				for (int k = 0; k < 3; k++)
				{
					float v = blo[k] > 0 ? blo[k] : bhi[k] < 0 ? -bhi[k] : 0;
					s += v * v;
				}
				nearest = std::min(nearest, std::sqrt(s));
			};
			for (const Node &n : d.nodes)
				if (!n.under)
				{
					P3 blo = {1e30f, 1e30f, 1e30f}, bhi = {-1e30f, -1e30f, -1e30f};
					auto grow = [&](const P3 &p) {
						for (int k = 0; k < 3; k++)
						{
							blo[k] = std::min(blo[k], p[k]);
							bhi[k] = std::max(bhi[k], p[k]);
						}
					};
					if (n.g && n.gcount)
						gpu_corners(n, grow);
					for (const Tri &t : n.tris)
						for (const P3 &p : t.p)
							grow(p);
					if (blo[0] <= bhi[0])
						box_dist(blo, bhi);
				}
			if (nearest > 3.5e6f)
				continue;
		}
		cands.push_back(c);
	}
	const Object *first = nullptr;
	if (!cands.empty())
	{
		auto dist0 = [](const Cand &c) { return std::sqrt(c.c[0] * c.c[0] + c.c[1] * c.c[1] + c.c[2] * c.c[2]); };
		/* Three cascades: a map round a cruiser and a Cobra many km apart has texels as wide as the Cobra.
		 * Cascade 0 has the ships up to 5 times the smallest in view, cascade 1 the bigger ones and the stations,
		 * cascade 2 the scenery near the eye (a hangar's shadow in a map as wide as the station is mush). */
		float rmin = 1e30f;
		for (const Cand &c : cands)
			if (!c.station && !c.scenery)
				rmin = std::min(rmin, c.r);
		std::vector<std::vector<const Cand *>> chosen(GL_SHADOW_CASCADES);
		std::vector<int> cascade_of(drawn.size(), -1);
		for (int cc = 0; cc < GL_SHADOW_CASCADES; cc++)
		{
			std::vector<const Cand *> cls;
			for (const Cand &c : cands)
				if (cc == 2 ? c.scenery || c.base
					: cc == 0 ? !c.station && !c.scenery && c.r <= 5.0f * rmin
							  : !c.scenery && !c.base && (c.station || c.r > 5.0f * rmin))
					cls.push_back(&c);
			if (cls.empty())
				continue;
			auto nearer = [&](const Cand *a, const Cand *b) { return dist0(*a) - a->r < dist0(*b) - b->r; };
			std::stable_sort(cls.begin(), cls.end(), nearer);
			if (cc == 2)
			{
				/* the 12 nearest within 10 km of the eye (what receives lies near the ground there) */
				for (const Cand *c : cls)
					if ((c->base || dist0(*c) - c->r <= 1.0e7f) && chosen[2].size() < 12)
						chosen[2].push_back(c);
				continue;
			}
			/* the map is fitted round the casters, so a ship far off would stretch it and make every texel
			 * metres wide: only those near the class's nearest (within a few of its sizes, at least 100 m) cast */
			const Cand *near = cls[0];
			const float reach = std::max(8.0f * near->r, 1.0e5f);
			for (const Cand *c : cls)
			{
				float dx = c->c[0] - near->c[0], dy = c->c[1] - near->c[1], dz = c->c[2] - near->c[2];
				if (std::sqrt(dx * dx + dy * dy + dz * dz) + c->r > reach && c != near)
					continue;
				chosen[(size_t)cc].push_back(c);
			}
		}
		const float *light = nullptr;
		float lightv[3];
		bool made[GL_SHADOW_CASCADES] = {};
		std::vector<int> own_layer(drawn.size(), -1);
		for (int cc = 0; cc < GL_SHADOW_CASCADES; cc++)
		{
			std::vector<size_t> ends;
			shadow_tris.clear();
			for (const Cand *cp : chosen[(size_t)cc])
			{
				const Cand &c = *cp;
				shadow_caster[c.i] = 1;
				cascade_of[c.i] = cc;
				own_layer[c.i] = gl_shadow_layer(cc, (int)ends.size());
				{
					static const bool dbg2 = getenv("GLF_SHIP_SHADOWS") && atoi(getenv("GLF_SHIP_SHADOWS")) == 2;
					static int fr;
					if (dbg2 && fr++ % 100 < 3)
						log_printf("shadow caster %zu model %d cascade %d own %d shrink %.2f centre %.0f %.0f %.0f r %.0f\n",
								   c.i, (int)shown->obj[c.i].model, cc, (int)shown->obj[c.i].own_added,
								   drawn[c.i].shrink, c.c[0], c.c[1], c.c[2], c.r);
				}
				if (!first)
				{
					first = &shown->obj[c.i];
					for (int k = 0; k < 3; k++)
						lightv[k] = first->light[k];
					light = lightv;
				}
				/* the whole hull, run again without the facing tests: the drawn
				 * model has only the faces turned to the camera */
				static Drawn whole;
				cached_run(shown->obj[c.i], whole, true, prof_ran, prof_reused);
				if (c.i < bends.size())
					bend_nodes(bends[c.i], whole.nodes);
				for (const Node &n : whole.nodes)
					if (!n.under)
						for (const Tri &t : n.tris)
						{
							/* a ground base is a town: only what lies within 2.5 km of the eye casts, so the map is
							 * fitted to the hangars round the ship, not to the whole base */
							if (c.base)
							{
								float cx = (t.p[0][0] + t.p[1][0] + t.p[2][0]) / 3, cy = (t.p[0][1] + t.p[1][1] + t.p[2][1]) / 3,
									  cz = (t.p[0][2] + t.p[1][2] + t.p[2][2]) / 3;
								if (cx * cx + cy * cy + cz * cz > 2.5e6f * 2.5e6f)
									continue;
							}
							for (const P3 &p : t.p)
								shadow_tris.insert(shadow_tris.end(), p.begin(), p.end());
						}
				ends.push_back(shadow_tris.size() / 9);
			}
			if (!ends.empty())
				made[cc] = gl_shadow_set(cc, light, shadow_tris.data(), shadow_tris.size() / 9, ends.data(), (int)ends.size());
		}
		shadow_on = made[0] || made[1] || made[2];
		/* what each object looks up: each cascade's map with every caster, or for a caster of it the map
		 * without itself (none at all for a caster that has no map of its own) */
		for (size_t i = 0; i < drawn.size(); i++)
			for (int cc = 0; cc < GL_SHADOW_CASCADES; cc++)
			{
				int &l = shadow_layer[i].l[cc];
				l = gl_shadow_all_layer(cc);
				if (cascade_of[i] == cc)
					l = made[cc] ? own_layer[i] : -1;
			}
		for (size_t i = 0; i < drawn.size(); i++)
			if (cascade_of[i] >= 0 && (own_layer[i] < 0 || (cascade_of[i] == 2 && shown->obj[i].model >= 101)))
				shadow_layer[i] = ShadowLayers{{-1, -1, -1}}; /* scenery receives nothing */
	}
	static const bool dbg = getenv("GLF_SHIP_SHADOWS") && atoi(getenv("GLF_SHIP_SHADOWS")) == 2;
	static int frames;
	if (dbg && frames++ % 100 == 0)
	{
		int casters = 0, hidden = 0;
		for (size_t i = 0; i < drawn.size(); i++)
		{
			casters += shadow_caster[i];
			hidden += drawn[i].hide;
		}
		log_printf("ship shadows: %d objects, %d casters (%d own, hidden), %zu triangles, map %d\n", shown->n, casters,
				   hidden, shadow_tris.size() / 9, (int)shadow_on);
		for (size_t i = 0; i < drawn.size() && i < (size_t)shown->n; i++)
		{
			size_t nt = 0;
			for (const Node &n : drawn[i].nodes)
				nt += n.tris.size() + (size_t)n.gcount / 3;
			if (nt)
				log_printf("  object %zu model %d shrink %.3f nodes %zu tris %zu caster %d layers %d %d %d\n", i,
						   (int)shown->obj[i].model, drawn[i].shrink, drawn[i].nodes.size(), nt, (int)shadow_caster[i],
						   shadow_layer[i].l[0], shadow_layer[i].l[1], shadow_layer[i].l[2]);
		}
	}
}

/* This frame's objects run (native_draw_sky does it first, native_draw
 * otherwise), each with the clip w it spans */
bool prepared = false;
/* The models only change with the game's frame, not with the head or the
 * eye: the picture before this one, or the other eye, ran the same, so
 * keep what it made (sorted, cut at the planet) and only measure the
 * spans again, which do depend on the view */
uint32_t prepared_serial = 0;
bool prepared_wire = false;
bool shadow_kept = false; /* setup_shadows ran for prepared_serial */
void prepare(bool wire)
{
	if (prepared)
		return;
	prepared = true;
	uint64_t prof_t0 = PROF_BEGIN();
	prof_ran = prof_reused = 0;
	bool keep = shown->serial != 0 && shown->serial == prepared_serial && wire == prepared_wire &&
				drawn.size() == (size_t)shown->n;
	uint64_t tp0 = PROF_BEGIN(), tp1 = tp0, tp2 = tp0;
	if (!keep)
	{
		run_objects(wire);
		if (gpu_on())
			gd_face_colours(face_rgba.data(), (int)(face_rgba.size() / 4));
		make_bends(wire);
		tp1 = PROF_BEGIN();
		prepared_serial = shown->serial;
		prepared_wire = wire;
		for (Drawn &d : drawn)
		{
			/* in the game's order, what comes before a planet it calls */
			if (wire || planet_native_count() == 0)
				continue;
			sort_nodes(d.nodes);
			for (size_t i = 0; i < d.nodes.size(); i++)
				if (d.nodes[i].planet)
				{
					d.before_planet.assign(std::make_move_iterator(d.nodes.begin()),
										   std::make_move_iterator(d.nodes.begin() + (long)i));
					d.nodes.erase(d.nodes.begin(), d.nodes.begin() + (long)i);
					break;
				}
		}
	}
	tp2 = PROF_BEGIN();
	mat4 mvp = gd_mvp();
	auto clip_w = [&](const P3 &p) { return mvp.m[3] * p[0] + mvp.m[7] * p[1] + mvp.m[11] * p[2] + mvp.m[15]; };
	for (Drawn &d : drawn)
	{
		d.near = 1e30f;
		d.far = 0;
		d.back = false;
		auto span = [&](const P3 &p) {
			float w = clip_w(p);
			d.near = std::min(d.near, w);
			d.far = std::max(d.far, w);
		};
		for (const std::vector<Node> *nodes : {&d.before_planet, &d.nodes})
			for (const Node &n : *nodes)
			{
				if (n.g && n.gcount)
				{
					/* w is linear: its least and most over the node's box */
					const Cached::GNode &gn = n.g->e->gn[(size_t)n.gnode];
					const Place &pl = n.g->pl;
					float lo = clip_w(pl.pos), hi = lo;
					for (int k = 0; k < 3; k++)
					{
						float a = mvp.m[3] * pl.ax[k][0] + mvp.m[7] * pl.ax[k][1] + mvp.m[11] * pl.ax[k][2];
						lo += std::min(a * gn.lo[k], a * gn.hi[k]);
						hi += std::max(a * gn.lo[k], a * gn.hi[k]);
					}
					d.near = std::min(d.near, lo);
					d.far = std::max(d.far, hi);
				}
				for (const Tri &t : n.tris)
					for (const P3 &p : t.p)
						span(p);
				for (const Seg &s : n.segs)
				{
					span(s.a);
					span(s.b);
				}
				for (const Dot &s : n.dots)
					span(s.p);
			}
	}
	uint64_t tp3 = PROF_BEGIN();
	/* the casters only change with the game's frame */
	if (keep && shadow_kept)
		gl_shadow_again();
	else
		setup_shadows(wire);
	shadow_kept = cache_on();
	if (prof_on)
	{
		prof_native_add(prof_t0, prof_ran, prof_reused);
		/* GLF_NATIVE_CACHE=2: where prepare's time goes */
		static const bool dbg = getenv("GLF_NATIVE_CACHE") && atoi(getenv("GLF_NATIVE_CACHE")) == 2;
		static double t_run, t_sort, t_span, t_shadow;
		static int pics, frames;
		static size_t tris;
		uint64_t tp4 = PROF_BEGIN();
		double f = 1000.0 / (double)SDL_GetPerformanceFrequency();
		pics++;
		if (!keep)
		{
			frames++;
			t_run += (double)(tp1 - tp0) * f;
			t_sort += (double)(tp2 - tp1) * f;
			for (const Drawn &d : drawn)
				for (const Node &n : d.nodes)
					tris += n.tris.size() + (size_t)n.gcount / 3;
		}
		t_span += (double)(tp3 - tp2) * f;
		t_shadow += (double)(tp4 - tp3) * f;
		if (dbg && (double)(tp4 - tp0) * f > 15)
			log_printf("native slow prepare %.1f ms: run %.1f sort %.1f spans %.1f shadows %.1f (keep %d, kept %d, %d ran, %d reused, %d objects)\n",
					   (double)(tp4 - tp0) * f, keep ? 0 : (double)(tp1 - tp0) * f, keep ? 0 : (double)(tp2 - tp1) * f,
					   (double)(tp3 - tp2) * f, (double)(tp4 - tp3) * f, (int)keep, (int)(keep && shadow_kept), prof_ran,
					   prof_reused, shown->n);
		if (dbg && pics >= 200)
		{
			log_printf("native prepare per game frame: run %.2f ms, sort %.2f; per picture: spans %.2f, shadows %.2f; %d pictures over %d frames, %zu triangles/frame\n",
					   t_run / std::max(frames, 1), t_sort / std::max(frames, 1), t_span / pics, t_shadow / pics, pics, frames,
					   tris / (size_t)std::max(frames, 1));
			t_run = t_sort = t_span = t_shadow = 0;
			pics = frames = 0;
			tris = 0;
		}
	}
}

void draw_segs(const std::vector<Seg> &segs)
{
	for (const Seg &s : segs)
	{
		set_colour(s.colour);
		gd_line(s.a.data(), s.b.data());
	}
}

/* Rays across p, `len` either way: a cross on the screen, as the game
 * draws it, along the eye's right and up, not fixed in the world (it would
 * turn as the head rolls) */
void draw_rays(const float p[3], float len)
{
	float x[3] = {1, 0, 0}, y[3] = {0, 1, 0};
	if (cockpit_active())
	{
		mat4 h = cockpit_head_rotation();
		for (int k = 0; k < 3; k++)
			x[k] = h.m[4 * k], y[k] = h.m[4 * k + 1];
	}
	float a[3], b[3], e[3], g[3];
	for (int k = 0; k < 3; k++)
	{
		a[k] = p[k] - len * x[k], b[k] = p[k] + len * x[k];
		e[k] = p[k] - len * y[k], g[k] = p[k] + len * y[k];
	}
	gd_line(a, b);
	gd_line(e, g);
}

/* As draw_blob (gl_prims.c) draws the game's: small ones as points; a
 * twinkly one with rays 8 pixels longer than its radius, as
 * draw_twinkly_circle */
void draw_dots(const std::vector<Dot> &dots, bool colour = true)
{
	for (const Dot &d : dots)
	{
		if (colour)
			set_colour(d.colour);
		float r = d.px >= 0 ? -0.002f * d.px * d.p[2] : d.radius;
		if (d.twinkly)
			draw_rays(d.p.data(), std::max(r, 0.0f) - 0.016f * d.p[2]);
		if (d.px >= 0 && d.px < 3)
		{
			gd_point(d.p.data(), (float)((int)d.px / 2 + 1));
			continue;
		}
		if (r <= 0)
			continue;
		gd_push();
		gd_translate(d.p[0], d.p[1], d.p[2]);
		gd_disk(r, 48);
		gd_pop();
	}
}

/* A node's triangles. A cloud's shadows (colour bit 15) are drawn once per
 * pixel, so where two overlap the ground is not darkened twice (the
 * stencil counts the first draw; the second undoes it) */
void draw_shadow_group(const std::vector<const Tri *> &tris)
{
	gd_once_phase(0);
	for (int pass = 0; pass < 2; pass++)
	{
		if (pass)
			gd_once_phase(1);
		for (const Tri *t : tris)
		{
			set_colour(t->colour);
			if (pass)
				gd_color4ub(0, 0, 0, 0);
			gd_tri(t->p[0].data(), t->p[1].data(), t->p[2].data());
		}
	}
	gd_once_phase(2);
}

void draw_tris(const std::vector<Tri> &tris, bool defer_shadows = false)
{
	bool any = false;
	for (const Tri &t : tris)
	{
		if (t.colour & 0x8000)
		{
			any = true;
			continue;
		}
		set_colour(t.colour);
		gd_tri(t.p[0].data(), t.p[1].data(), t.p[2].data());
	}
	if (!any || defer_shadows)
		return;
	if (getenv("GLF_CLOUD_SHADOWS") && atoi(getenv("GLF_CLOUD_SHADOWS")) == 3)
	{
		static int n;
		if (n++ % 120 == 0)
			log_printf("shadow group: %zu tris\n", tris.size());
	}
	for (int pass = 0; pass < 2; pass++)
	{
		gd_once_phase(pass);
		for (const Tri &t : tris)
		{
			if (!(t.colour & 0x8000))
				continue;
			set_colour(t.colour);
			if (pass)
				gd_color4ub(0, 0, 0, 0);
			gd_tri(t.p[0].data(), t.p[1].data(), t.p[2].data());
		}
	}
	gd_once_phase(2);
}

/* A node's GPU triangles (Node.g), as draw_tris would draw them */
void draw_gpu(const Node &n)
{
	if (!n.g || !n.gcount)
		return;
	if (gd_wireframe())
	{
		std::vector<Tri> t;
		gpu_tris(n, t);
		draw_tris(t);
		return;
	}
	set_colour(0); /* opaque: no blending */
	gd_mesh_draw(n.g->e->mesh, n.g->pose, n.g->base, n.gfirst, n.gcount, n.g->bent ? &n.g->bend[0][0] : nullptr);
}

void draw_nodes(const std::vector<Node> &nodes)
{
	for (const Node &n : nodes)
	{
		draw_gpu(n);
		draw_tris(n.tris);
		draw_segs(n.segs);
		draw_dots(n.dots);
	}
}

/* View units per pixel of the game's screen at distance z, as gl_prims.c
 * draws the game's circles */
float px_at(const float p[3])
{
	return 0.002f * std::fabs(p[2]);
}

/* A far body (L3a68c and on): a disc of its true size, lit as a sphere
 * with the tints the game gives a face by how square it is to the light,
 * unless its colour says it shines (bit 8); a twinkly one (a sun) has
 * rays across it 8 pixels longer than its radius, and when it is smaller
 * than a pixel the rays alone, shorter (L3aa74). Something smaller than a
 * pixel is a dot. */
void draw_far(const Far &f)
{
	int c = f.colour & 0xfff;
	int base = (c & 0xeee) + ((c & 0x10) ? 0x111 : 0);
	bool lit = !(c & 0x100);
	bool twinkly = f.colour & 0x8000;
	float px = px_at(f.pos);
	float rpx = f.radius / px;
	if (rpx >= 0.5f)
	{
		/* the disc's axes, square to the line of sight */
		float d = std::sqrt(f.pos[0] * f.pos[0] + f.pos[1] * f.pos[1] + f.pos[2] * f.pos[2]);
		P3 w = {-f.pos[0] / d, -f.pos[1] / d, -f.pos[2] / d};
		P3 u = std::fabs(w[1]) < 0.9f ? P3{w[2], 0, -w[0]} : P3{0, -w[2], w[1]};
		float ul = std::sqrt(dot(u, u));
		u = {u[0] / ul, u[1] / ul, u[2] / ul};
		P3 v = {w[1] * u[2] - w[2] * u[1], w[2] * u[0] - w[0] * u[2], w[0] * u[1] - w[1] * u[0]};
		const int RINGS = lit ? 12 : 1, SEGS = 48;
		auto at = [&](float rho, float th, P3 &n) {
			float cu = rho * std::cos(th), cv = rho * std::sin(th);
			n = {cu * u[0] + cv * v[0], cu * u[1] + cv * v[1], cu * u[2] + cv * v[2]};
			P3 p = {f.pos[0] + f.radius * n[0], f.pos[1] + f.radius * n[1], f.pos[2] + f.radius * n[2]};
			float h = std::sqrt(std::max(0.0f, 1 - rho * rho));
			for (int k = 0; k < 3; k++)
				n[k] += h * w[k];
			return p;
		};
		auto colour = [&](const P3 &n) {
			if (!lit)
				return base;
			float dl = dot(n, P3{f.light[0], f.light[1], f.light[2]}) * (127.0f / 128.0f);
			return dl >= 0 ? base : base + f.tint[std::max(0, std::min(7, (int)std::floor(8 * (1 + dl))))];
		};
		for (int i = 0; i < RINGS; i++)
		{
			float r0 = (float)i / RINGS, r1 = (float)(i + 1) / RINGS;
			for (int j = 0; j < SEGS; j++)
			{
				float t0 = 6.2831853f * j / SEGS, t1 = 6.2831853f * (j + 1) / SEGS;
				P3 n00, n01, n10, n11, nm;
				P3 a = at(r0, t0, n00), b = at(r1, t0, n10), e = at(r1, t1, n11), g = at(r0, t1, n01);
				at((r0 + r1) / 2, (t0 + t1) / 2, nm);
				set_colour(colour(nm));
				gd_tri(a.data(), b.data(), e.data());
				if (i > 0)
					gd_tri(a.data(), e.data(), g.data());
			}
		}
	}
	else if (!twinkly)
	{
		set_colour(lit ? base + f.tint[0] : base);
		gd_point(f.pos, 1.0f);
	}
	if (twinkly)
	{
		set_colour(base);
		draw_rays(f.pos, (rpx >= 1 ? rpx + 8 : 1 + 7 * rpx) * px);
	}
}

/* The game's order within an object: its depth tree's nodes farthest
 * first, each in the model's order */
void sort_nodes(std::vector<Node> &nodes)
{
	std::stable_sort(nodes.begin(), nodes.end(), [](const Node &a, const Node &b) {
		for (int i = 0; i < std::min(a.depth, b.depth); i++)
			if (a.path[(size_t)i] != b.path[(size_t)i])
				return a.path[(size_t)i] > b.path[(size_t)i];
		return false;
	});
}

/* The keys again along the view being drawn (the cockpit's turn and the
 * head are in the projection: depth = clip w, x and y back out of clip x
 * and y), as the game's cockpit passes keyed along each turned camera,
 * then the nodes sorted by them; equal keys keep the model's order.
 * Node.dist is the top level's key. */
void sort_nodes_view(std::vector<Node> &nodes)
{
	mat4 mvp = gd_mvp();
	const float *m = mvp.m, *pr = gd_projection()->m;
	for (Node &n : nodes)
	{
		for (int i = 0; i < n.depth; i++)
		{
			const P3 &p = n.kpt[(size_t)i];
			float cx = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
			float cy = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
			float w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
			float x = pr[0] != 0 ? (cx + pr[8] * w - pr[12]) / pr[0] : 0;
			float y = pr[5] != 0 ? (cy + pr[9] * w - pr[13]) / pr[5] : 0;
			float push = n.kpush[(size_t)i];
			n.dpath[(size_t)i] = push >= 1e30f ? push : w + (std::fabs(x) + std::fabs(y)) / 8 + push;
		}
		n.dist = n.dpath[0];
	}
	std::sort(nodes.begin(), nodes.end(), [](const Node &a, const Node &b) {
		for (int i = 0; i < std::min(a.depth, b.depth); i++)
			if (a.dpath[(size_t)i] != b.dpath[(size_t)i])
				return a.dpath[(size_t)i] > b.dpath[(size_t)i];
		return a.seq < b.seq;
	});
}

/* A planet object: its model (rings) round the sphere, what is farther
 * than its centre before it, the rest after (the game sorts the ring's
 * quarters, whose ends are behind the planet and in front). Faces across
 * that distance are cut until their pieces are small next to the planet. */
void draw_planet_object(const Object &o, int sphere)
{
	Drawn d;
	run_object(o, d, false, false);
	const Node *planet = nullptr;
	for (const Node &n : d.nodes)
		if (n.planet && !planet)
			planet = &n;
	double cd = planet_native_dist(sphere);
	float piece = planet && planet->radius > 0 ? planet->radius / 4 : (float)cd / 100;
	auto len = [](const P3 &p) { return std::sqrt(dot(p, p)); };
	bool back = true;
	std::function<void(const P3 &, const P3 &, const P3 &, int)> split_tri = [&](const P3 &a, const P3 &b,
																			   const P3 &c, int level) {
		bool fa = len(a) > cd, fb = len(b) > cd, fc = len(c) > cd;
		auto edge = [&](const P3 &u, const P3 &v) { return len(P3{u[0] - v[0], u[1] - v[1], u[2] - v[2]}); };
		if ((fa != fb || fb != fc) && level < 8 && std::max({edge(a, b), edge(b, c), edge(c, a)}) > piece)
		{
			auto mid = [](const P3 &u, const P3 &v) {
				return P3{(u[0] + v[0]) / 2, (u[1] + v[1]) / 2, (u[2] + v[2]) / 2};
			};
			P3 ab = mid(a, b), bc = mid(b, c), ca = mid(c, a);
			split_tri(a, ab, ca, level + 1);
			split_tri(ab, b, bc, level + 1);
			split_tri(ca, bc, c, level + 1);
			split_tri(ab, bc, ca, level + 1);
			return;
		}
		P3 m = {(a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3, (a[2] + b[2] + c[2]) / 3};
		if ((len(m) > cd) == back)
			gd_tri(a.data(), b.data(), c.data());
	};
	for (int pass = 0; pass < 2; pass++)
	{
		back = pass == 0;
		for (const Node &n : d.nodes)
		{
			for (const Tri &t : n.tris)
			{
				set_colour(t.colour);
				split_tri(t.p[0], t.p[1], t.p[2], 0);
			}
			for (const Seg &sg : n.segs)
			{
				P3 c = {(sg.a[0] + sg.b[0]) / 2, (sg.a[1] + sg.b[1]) / 2, (sg.a[2] + sg.b[2]) / 2};
				if ((len(c) > cd) != back)
					continue;
				set_colour(sg.colour);
				gd_line(sg.a.data(), sg.b.data());
			}
			for (const Dot &dt : n.dots)
				if ((len(dt.p) > cd) == back)
					draw_dots({dt});
		}
		if (back)
			planet_native_draw(sphere);
	}
}

/* The planets' objects this frame: a planet the game didn't put stands in
 * where it was last seen (gl_planet.c). Again is harmless. */
void place_planets()
{
	for (int i = 0; i < shown->n && !screen_only; i++)
	{
		const Object &o = shown->obj[i];
		const Code *c = code_for(o.table, o.model);
		if (c && c->planet)
			planet_native_object(o.addr, o.pos, o.cols);
	}
}

/* Far bodies and planets, farthest first */
void draw_fars()
{
	place_planets();
	/* a far body, or a planet (its sphere and/or its object), by true
	 * distance: each is shrunk by its own power of two */
	struct Item
	{
		double d;
		int far, sphere;
		const Object *o;
		int obj = -1; /* an object beyond a planet (drawn[obj]) */
	};
	std::vector<Item> order;
	auto dist = [](const float *p) { return std::sqrt((double)p[0] * p[0] + (double)p[1] * p[1] + (double)p[2] * p[2]); };
	/* A planet is its model, however far; only when it is a few pixels
	 * wide (or its sphere hasn't been seen) the disc or dot stands in */
	auto tiny = [](const Far &f) { return f.radius / px_at(f.pos) < 3.0f; };
	auto has_sphere = [&](const Far &f) {
		for (int i = 0; i < planet_native_count(); i++)
			if (planet_native_object_addr(i) == f.addr)
				return true;
		return false;
	};
	std::vector<uint32_t> dotted; /* planet objects drawn as a dot */
	for (int i = 0; i < shown->nfar; i++)
	{
		const Far &f = shown->far[i];
		if (f.planet && !tiny(f) && has_sphere(f))
			continue;
		if (f.planet)
			dotted.push_back(f.addr);
		order.push_back({dist(f.pos) * f.shrink, i, -1, nullptr});
	}
	double nearest_planet = 1e300;
	for (int i = 0; i < planet_native_count(); i++)
	{
		if (std::find(dotted.begin(), dotted.end(), planet_native_object_addr(i)) != dotted.end())
			continue;
		order.push_back({planet_native_dist(i) * planet_native_shrink(i), -1, i, nullptr});
		nearest_planet = std::min(nearest_planet, order.back().d);
	}
	/* An object farther than a planet's centre (a nebula in the intro, a
	 * station round the far side) goes before it, as the game sorts them,
	 * not over it with the rest of the scene */
	for (size_t j = 0; j < drawn.size() && (int)j < shown->n; j++)
	{
		double d = dist(shown->obj[j].pos) * shown->obj[j].shrink;
		if (d > nearest_planet && drawn[j].far > 1.0f)
		{
			drawn[j].back = true;
			order.push_back({d, -1, -1, nullptr, (int)j});
		}
	}
	for (int j = 0; j < shown->n; j++)
	{
		const Object &o = shown->obj[j];
		const Code *c = code_for(o.table, o.model);
		if (!c || !c->planet)
			continue;
		/* without its sphere, the game draws it (a disc far away) */
		for (Item &e : order)
			if (e.sphere >= 0 && planet_native_object_addr(e.sphere) == o.addr)
				e.o = &o;
	}
	std::stable_sort(order.begin(), order.end(), [](const Item &a, const Item &b) { return a.d > b.d; });
	for (const Item &e : order)
	{
		if (e.obj >= 0)
		{
			sort_nodes(drawn[(size_t)e.obj].nodes);
			draw_nodes(drawn[(size_t)e.obj].nodes);
		}
		else if (e.far >= 0)
			draw_far(shown->far[e.far]);
		else if (e.o)
			draw_planet_object(*e.o, e.sphere);
		else
			planet_native_draw(e.sphere);
	}
}

} // namespace

/* The sky, filled, under everything (before the scene): its parts in the
 * model's order, not depth tested. Round the camera, so it is the same
 * whichever way that looks. Then the atmosphere bands and, farthest first,
 * the far bodies and the planets on it. */
extern "C" bool native_view_axes(float m[9])
{
	const Object *o = shown->has_sky ? &shown->sky : shown->n > 0 ? &shown->obj[0] : nullptr;
	if (!o)
		return false;
	std::memcpy(m, o->cols, 9 * sizeof(float));
	return true;
}

extern "C" void native_draw_sky(void)
{
	if (!native_enabled() || enabled != 2)
		return;
	gd_push();
	gd_identity();
	prepare(false);
	/* the cockpit's: not in the air, whatever the game camera's view was
	 * when it decided (gl_atmos.c) */
	bool stars = !(cockpit_active() && shown == &shows[0] && atmos_sky_hides_stars());
	if (shown->has_sky && stars)
	{
		/* round the camera: the same whichever way it looks */
		static Drawn sky;
		static uint32_t sky_serial;
		if (!shown->serial || shown->serial != sky_serial)
		{
			run_object(shown->sky, sky, false, true);
			sky_serial = shown->serial;
		}
		draw_nodes(sky.nodes);
	}
	for (const Drawn &d : drawn)
		draw_nodes(d.before_planet);
	/* before the bands: they are drawn round their planet, and a planet
	 * standing in must be there already (frames the game didn't put it, as
	 * while a cockpit screen's page is made: save "flicker2") */
	place_planets();
	if (!screen_only)
		atmos_draw_native();
	draw_fars();
	gd_pop();
}

extern "C" void native_draw(void)
{
	if (!native_enabled())
		return;
	bool wire = enabled == 1;
	gd_push();
	gd_identity();
	prepare(wire);
	prepared = false;
	if (wire)
	{
		/* the sky in blue, over the game's */
		if (shown->has_sky)
		{
			Drawn d;
			run_object(shown->sky, d, true, true);
			gd_color3ub(96, 128, 255);
			for (const Seg &s : d.wire)
				gd_line(s.a.data(), s.b.data());
			draw_dots(d.wire_dots, false);
		}
		/* far bodies as orange rings */
		gd_color3ub(255, 160, 64);
		for (int i = 0; i < shown->nfar; i++)
		{
			const Far &f = shown->far[i];
			float r = std::max(f.radius, 3 * px_at(f.pos));
			float ring[32][3];
			for (int j = 0; j < 32; j++)
			{
				ring[j][0] = f.pos[0] + r * std::cos(6.2831853f * j / 32);
				ring[j][1] = f.pos[1] + r * std::sin(6.2831853f * j / 32);
				ring[j][2] = f.pos[2];
			}
			gd_line_loop(ring, 32);
		}
		/* bright colours, one per object, to tell them from the game's
		 * picture and each other */
		static const uint8_t pal[6][3] = {{255, 255, 0}, {0, 255, 255}, {255, 0, 255},
										  {0, 255, 0},   {255, 128, 0}, {255, 255, 255}};
		for (size_t i = 0; i < drawn.size(); i++)
			for (const Seg &s : drawn[i].wire)
			{
				gd_color3ub(pal[i % 6][0], pal[i % 6][1], pal[i % 6][2]);
				gd_line(s.a.data(), s.b.data());
			}
		for (size_t i = 0; i < drawn.size(); i++)
		{
			gd_color3ub(pal[i % 6][0], pal[i % 6][1], pal[i % 6][2]);
			draw_dots(drawn[i].wire_dots, false);
		}
		gd_pop();
		return;
	}

	/* Each object spans near..far in clip w. Objects whose spans overlap
	 * share a depth slice, so they hide each other properly; the slices go
	 * nearest first, so slices hide each other too, and each is precise
	 * however far away it is. Spans and keys are compared at their true
	 * distances (a far object is drawn shrunk), so a far one never shares a
	 * slice with near ones. */
	std::vector<Drawn *> placed;
	for (Drawn &d : drawn)
		if (d.far > 1.0f && !d.back && !d.hide)
			placed.push_back(&d);
	std::sort(placed.begin(), placed.end(),
			  [](const Drawn *a, const Drawn *b) { return a->near * a->shrink < b->near * b->shrink; });
	std::vector<std::pair<size_t, size_t>> groups; /* [first, last) in placed */
	float group_far = 0;
	for (size_t i = 0; i < placed.size(); i++)
	{
		if (!groups.empty() && placed[i]->near * placed[i]->shrink < group_far)
			groups.back().second = i + 1;
		else
		{
			groups.push_back({i, i + 1});
			group_far = 0;
		}
		group_far = std::max(group_far, placed[i]->far * placed[i]->shrink);
	}
	/* Underlays, all objects', in the game's order: each before the first
	 * part (of any object) nearer than its key (a city's ground plates, far
	 * back, before everything), after the parts beyond it (a ship seen past
	 * a station's bay, or the station's own scaffold outside it, which the
	 * bay then hides). Painted, not depth tested: they need not lie under
	 * what is drawn on them in 3D. Returns whether it drew any. */
	for (Drawn *d : placed)
		sort_nodes_view(d->nodes);
	struct Under
	{
		float key; /* true key */
		const Node *node;
		ShadowLayers layers; /* the shadow maps' layers it looks up */
	};
	const float *shadow_m0 = shadow_on ? gl_shadow_matrix(0) : nullptr;
	const float *shadow_m1 = shadow_on ? gl_shadow_matrix(1) : nullptr;
	const float *shadow_m2 = shadow_on ? gl_shadow_matrix(2) : nullptr;
	/* the map layers an object looks up (-1: none of that cascade), all -1 if it receives none */
	static const ShadowLayers no_layers = {{-1, -1, -1}};
	auto receives = [&](const Drawn *d) {
		return shadow_on && d->shrink == 1.0f ? shadow_layer[(size_t)(d - drawn.data())] : no_layers;
	};
	auto set_shadow = [&](const ShadowLayers &sl) {
		gd_set_shadow(shadow_m0, shadow_m0 ? sl.l[0] : -1, shadow_m1, shadow_m1 ? sl.l[1] : -1, shadow_m2,
					  shadow_m2 ? sl.l[2] : -1);
	};
	std::vector<Under> unders;
	for (const Drawn *d : placed)
		for (const Node &n : d->nodes)
			if (n.under)
				unders.push_back({n.dist * d->shrink, &n, receives(d)});
	std::stable_sort(unders.begin(), unders.end(), [](const Under &a, const Under &b) { return a.key > b.key; });
	/* All the underlays' cloud shadows (of every object) are drawn together,
	 * once per pixel, at the first underlay node that has any */
	std::vector<const Tri *> under_shadows;
	for (const Under &u : unders)
		for (const Tri &t : u.node->tris)
			if (t.colour & 0x8000)
				under_shadows.push_back(&t);
	bool shadows_drawn = false;
	size_t next_under = 0;
	auto draw_unders = [&](float beyond) {
		if (next_under >= unders.size() || unders[next_under].key <= beyond)
			return false;
		gd_set_depth(nullptr);
		for (; next_under < unders.size() && unders[next_under].key > beyond; next_under++)
		{
			const Node *n = unders[next_under].node;
			set_shadow(unders[next_under].layers);
			draw_tris(n->tris, true);
			if (!shadows_drawn && !under_shadows.empty())
				for (const Tri &t : n->tris)
					if (t.colour & 0x8000)
					{
						gd_set_shadow(nullptr, -1, nullptr, -1, nullptr, -1);
						draw_shadow_group(under_shadows);
						set_shadow(unders[next_under].layers);
						shadows_drawn = true;
						break;
					}
			draw_segs(n->segs);
			draw_dots(n->dots);
		}
		gd_set_shadow(nullptr, -1, nullptr, -1, nullptr, -1);
		return true;
	};

	float slice = groups.empty() ? 0 : 2.0f / (float)groups.size();
	for (size_t g = groups.size(); g-- > 0;)
	{
		GdDepth depth = {};
		depth.near = std::max(placed[groups[g].first]->near * 0.99f, 1.0f);
		for (size_t j = groups[g].first; j < groups[g].second; j++)
			depth.far = std::max(depth.far, placed[j]->far * 1.01f + 1.0f);
		depth.lo = -1 + slice * (float)g;
		depth.hi = -1 + slice * (float)(g + 1);
		/* The group's objects farthest first, as the game draws objects: a
		 * ship seen past a station's bay shares the station's slice, and
		 * the bay (an underlay, no depth) must be painted over it */
		std::vector<Drawn *> order(placed.begin() + (long)groups[g].first, placed.begin() + (long)groups[g].second);
		std::stable_sort(order.begin(), order.end(),
						 [](const Drawn *a, const Drawn *b) { return a->near * a->shrink > b->near * b->shrink; });
		for (Drawn *dp : order)
		{
			Drawn &d = *dp;
			/* Within the object, the game's order: its depth tree's nodes
			 * farthest first, each in the model's order, as the models were
			 * made for (a city's buildings stand half in its ground, decals
			 * lie on their faces), the underlays among them, hidden only by
			 * other objects... */
			depth.write = false;
			depth.depth_only = false;
			gd_set_depth(&depth);
			const ShadowLayers recv = receives(&d);
			for (const Node &n : d.nodes)
			{
				if (n.under)
					continue;
				if (draw_unders(n.dist * d.shrink))
					gd_set_depth(&depth);
				set_shadow(recv);
				draw_gpu(n);
				draw_tris(n.tris);
				draw_segs(n.segs);
				draw_dots(n.dots);
			}
			gd_set_shadow(nullptr, -1, nullptr, -1, nullptr, -1);
			/* ...then its depth, to hide what is behind it of the others */
			depth.write = true;
			depth.depth_only = true;
			gd_set_depth(&depth);
			for (const Node &n : d.nodes)
				if (!n.under)
				{
					draw_gpu(n);
					for (const Tri &t : n.tris)
						gd_tri(t.p[0].data(), t.p[1].data(), t.p[2].data());
				}
		}
	}
	draw_unders(-1e30f);
	gd_set_depth(nullptr);
	map_orbit_draw(*shown);
	gd_pop();
}
