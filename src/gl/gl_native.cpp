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
#include "gl_cockpit.h"
#include "gl_draw.h"
#include "gl_native.h"
#include "gl_planet.h"
#include "gl_scene.h"
#include "game_state.h"
#include "host.h"
#include "main.h"
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
	/* Model text's strings by text_key (Nu_NativeText): the last the game
	 * formatted for each, kept while it isn't run (off screen) */
	std::unordered_map<uint64_t, std::string> texts;
};
/* shows[0]: the world, shows[1]: the last frame of the game's other
 * screens, which leaves the world as it was (native_acquire) */
Frame rec, pub, pub_world, shows[2];
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
	for (int i = 0; i < rec.n; i++)
		if (rec.obj[i].addr == addr && rec.obj[i].model == model && rec.obj[i].table == table &&
			rec.obj[i].pass != pass)
			return false;
	return rec.n < MAX_OBJECTS;
}

/* The far body Nu_NativeFar last saw (L390f2's frame, its object) */
static uint32_t far_a6, far_addr;
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
	o.addr = addr;
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
	uint32_t a6 = (uint32_t)GetReg(REG_A6);
	uint32_t m = (uint32_t)GetReg(REG_A5);
	if ((int16_t)GetReg(REG_D7) < (int16_t)rdword(m + 20) || (int32_t)rdlong(a6 - 8) < 0x40)
		return;
	uint32_t addr = (uint32_t)rdlong(a6 - 212);
	far_a6 = a6;
	far_addr = addr;
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
		capture_light(a6, f.light, f.tint);
	}
	SetReg(REG_D0, enabled == 2 ? 1 : 0);
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
	if (sky_hook(a6))
		return;
	uint32_t addr, table;
	int model;
	if (!wanted(a6, addr, table, model))
		return;
	capture(rec.obj[rec.n++], a6, addr, table, model);
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

extern "C" int native_capture_dirs(float dirs[][3], int max)
{
	if (enabled != 2)
		return 0;
	static unsigned parity;
	parity++;
	const float ahead[3] = {0, 0, -1};
	int n = 0;
	for (int i = 0; i < pub.n && n < max; i++)
	{
		const Object &o = pub.obj[i];
		if (!planet_model(o))
			continue;
		float c[3], radius;
		bool was_ahead = false;
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
		}
		float dist = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
		if (dist <= 0)
			continue;
		float u[3] = {c[0] / dist, c[1] / dist, c[2] / dist}, g[3];
		/* its horizon, where it is nearest ahead: the planet's routine
		 * makes the planet and its haze when that is in view. Size unknown:
		 * its centre and a right angle off it, a frame each. */
		float a = radius > 0 ? std::asin(std::min(radius / dist, 1.0f)) : parity & 1 ? 1.5708f : 0.0f;
		turn_towards(u, ahead, a, g);
		dirs[n][0] = g[0];
		dirs[n][1] = g[1];
		dirs[n][2] = -g[2]; /* the game's: z ahead */
		n++;
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
}

extern "C" void native_publish(void)
{
	pub = rec;
	pub.texts = texts_seen;
}

extern "C" void native_publish_world(void)
{
	pub_world = rec;
	pub_world.texts = texts_seen;
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
};
constexpr int ZTREE_MAX = 4; /* nested depth trees (L385d0_3dview_thing2) */
struct Node
{
	float key; /* the game's depth key (zkey); the farthest is drawn first */
	/* The keys of the ztree nodes it is inside, outermost first, then its
	 * own: those nodes are sorted among the others by their keys, and what
	 * is inside each among itself */
	std::array<float, ZTREE_MAX + 1> path;
	int depth;
	std::vector<Tri> tris;
	std::vector<Seg> segs;
	std::vector<Dot> dots;
	/* Keyed farther back than it is (a batch pushed back by a depth): an
	 * underlay, ground plates, a starport's ground, cloud shadows. The game
	 * paints these under whatever is nearer, and they need not lie under it
	 * in 3D (a cloud's shadow hangs below the cloud, above the ground) */
	bool under = false;
	bool planet = false; /* the planet command: the sphere goes here */
	float radius = 0;    /* ...the planet's, view units */
};
struct Drawn
{
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
	int shade0;   /* the shade of normal 0, faces without a normal */
	uint16_t t[8]; /* temporaries, -64(a6): t0 the depth of sub-models */
	int voff = 0;  /* mcopy: vertex records shifted by this many */
	/* The matrix msetup and mrotate make (-122(a6)), view space: sub-models
	 * called with flip code 6 get it, mcopy makes it the model's own */
	P3 m[3];
	/* The parent's vertices passed to a sub-model: vertex -1 (its origin)
	 * to -5, view space */
	std::optional<P3> pv[5];
};

struct Runner
{
	const Object &o;
	Drawn &out;
	bool wire;
	int s2; /* -192(a6): sub-models' scales are relative to it */
	uint32_t frame_seed;
	int batch = -1; /* the node a batch is going into (L385e4_3dview_word1) */
	std::vector<float> zpath; /* the open ztree nodes' keys */
	/* The sky: all of it, for any camera. The game draws the half of each
	 * part that faces its camera (normal tests), from inside. */
	bool sky = false;

	Node &new_node(float key, bool under)
	{
		Node n;
		n.key = key;
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
		return new_node(zkey(at), false);
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
			return;
		}
		auto p = vertex(r, fe2::s8(w0 >> 5));
		if (zpath.size() < ZTREE_MAX)
			zpath.push_back(zkey(p ? *p : r.org));
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
		auto dist = [&](int i) {
			auto p = vertex(r, fe2::s8(i));
			return p ? zkey(*p) : zkey(r.org);
		};
		float key;
		if (v == 0x7ff)
			key = zkey(r.org);
		else if (v == 0x7fe)
			key = 1e30f;
		else if (v == 0x7fd)
			key = w.size() > 1 ? (int16_t)w[1] * r.k : 0;
		else if ((v & 0x700) == 0x700)
			key = dist(v) + (w.size() > 1 ? (int16_t)w[1] * r.k : 0);
		else if (v & 0x100)
		{
			bool nearest = v & 0x200;
			key = dist(v);
			for (size_t i = 1; i < w.size(); i++)
			{
				float d = dist(w[i]);
				key = nearest ? std::min(key, d) : std::max(key, d);
			}
		}
		else
			key = dist(v);
		bool under = v == 0x7fe || v == 0x7fd || ((v & 0x700) == 0x700 && v != 0x7ff && w.size() > 1 && (int16_t)w[1] > 0);
		new_node(key, under);
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

	/* View space -> the model's units */
	fe2::Vec3 unplace(const Run &r, const P3 &p) const
	{
		P3 d = {p[0] - r.org[0], p[1] - r.org[1], p[2] - r.org[2]};
		return {dot(r.ax[0], d) / r.k, dot(r.ax[1], d) / r.k, dot(r.ax[2], d) / r.k};
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

	/* The light's tint for a face with unit view normal n (L3a0ac): the
	 * light vector points away from the light; a face square to it
	 * (n . light = -1) gets tint 0, the brightest, one edge on 7, one facing
	 * away none. The game takes bits 30-28 of the negative 2 n . light
	 * (normals are bytes, at most 127/128 long). */
	int tint(const P3 &n) const
	{
		float d = (n[0] * o.light[0] + n[1] * o.light[1] + n[2] * o.light[2]) * (127.0f / 128.0f);
		if (d >= 0)
			return 0;
		return o.tint[std::max(0, std::min(7, (int)std::floor(8 * (1 + d))))];
	}

	/* Normal `n` of the model, view space, unit; and whether it faces the
	 * camera (the game's $8000 when it doesn't). Normal 0 always does. */
	bool normal(const Run &r, int n, P3 &out) const
	{
		auto nv = r.c->model.normal_vec(n);
		if (!nv)
			return false;
		out = turn(r, to_p3(fe2::normalize(*nv)));
		return true;
	}
	bool normal_faces(const Run &r, int n) const
	{
		P3 nv;
		if (!normal(r, n, nv))
			return true;
		int ni = (n >> 1) - 1;
		if (ni < 0 || ni >= (int)r.c->model.norms.size())
			return true;
		auto p = vertex(r, r.c->model.norms[ni].v ^ (n & 1));
		return !p || dot(nv, *p) < 0;
	}
	/* The shade normal n gives a colour (L3a02a_3DPrimCullNLight) */
	int shade(const Run &r, int n) const
	{
		if (n <= 1)
			return n == 0 ? r.shade0 : o.tint[4];
		P3 nv;
		if (!normal(r, n, nv) || !normal_faces(r, n))
			return 0;
		return tint(nv);
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
			return o.vars[v];
		default:
			return v < 8 ? r.t[v] : 0;
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
			return v < NUM_VARS ? o.vars[v] : 0;
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
		int sh = o.detail[k & 1] + r.s;
		if (sh < 0 || sh > 31 || ((int64_t)k << sh) > 0x7fffffff)
			return std::nullopt;
		return len(r.org) > (float)((int64_t)k << sh);
	}

	/* `ifdist`: how far apart two vertices are on the game's screen (168
	 * pixels high for its 36.5 degree view), roughly */
	std::optional<float> apart(const Run &r, int a, int b) const
	{
		auto p = vertex(r, a), q = vertex(r, b);
		if (!p || !q)
			return std::nullopt;
		float dp = len(*p), dq = len(*q);
		if (dp < 64 || dq < 64)
			return std::nullopt;
		static const float f = 84.0f / std::tan(CLASSIC_FOV * (float)M_PI / 360.0f);
		P3 d = {(*p)[0] - (*q)[0], (*p)[1] - (*q)[1], (*p)[2] - (*q)[2]};
		return f * len(d) * 2 / (dp + dq);
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
			if (f.has_normal && !sky && dot(n, a) >= 0)
				continue; /* faces away */
			int sh = f.has_normal ? tint(n) : shade(r, f.nidx);
			uint16_t col = (uint16_t)colour(f.colour, sh, r.base);
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
			Node &nd = node(place(r, f.outline[0]));
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
			v = (sz & 0x3f) < 8 ? r.t[sz & 0x3f] : 0;
		else
			v = o.vars[sz & 0x3f];
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
		float z = p ? -(*p)[2] : 0;
		if (!p || z < 64)
			return;
		int sz = w[1], v;
		if (!(sz & 0x80))
			v = ((sz >> 7) | (sz << 9)) & 0xffff;
		else if (sz & 0x40)
			v = (sz & 0x3f) < 8 ? r.t[sz & 0x3f] : 0;
		else
			v = o.vars[sz & 0x3f];
		float across = (float)((v & 0xffff) >> 1) * r.k / 128;
		int c0 = (w[0] >> 4) & 0xfff;
		int c = c0 & 0xefe;
		if (c & 0x10)
			c = ((c & ~0x10) + r.base) & 0xfff;
		Dot d;
		d.p = *p;
		d.twinkly = w[2] & 0x80;
		bool small = z / 8 > across;
		if (!small && z <= across / 2)
			return; /* round the eye */
		d.px = small ? std::floor(across * 256 / z) / 2 : -1.0f;
		d.radius = small ? 0 : across / 2;
		if (!(c0 & 0x100) && (small ? o.light[2] <= 0.875f : o.light[2] < -0.9766f))
			c = (c + o.tint[0]) & 0xfff;
		d.colour = (uint16_t)c;
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
		if (!head || !tail || -(*head)[2] <= 512 || -(*tail)[2] <= 512)
			return;
		int px = (int)((float)(int16_t)w[2] * r.k / 32 * 256 / -(*head)[2]);
		if (px == 0)
			return;
		int c = (w[0] >> 4) & 0xefe;
		if (c & 0x10)
			c = ((c & ~0x10) + r.base) & 0xfff;
		Seg line = {*head, *tail, (uint16_t)c};
		if (wire)
		{
			out.wire.push_back(line);
			return;
		}
		Node &nd = node(*head);
		if (px <= 8)
		{
			nd.segs.push_back(line);
			return;
		}
		const float STRETCH = 1.3333333333f, BROADEN = 0.33f;
		const int STEPS = 24;
		const P3 &b = *tail;
		P3 dir = {(*head)[0] - b[0], (*head)[1] - b[1], (*head)[2] - b[2]};
		P3 cp[4] = {b,
					{b[0] + STRETCH * dir[0] - BROADEN * dir[1], b[1] + STRETCH * dir[1] + BROADEN * dir[0],
					 b[2] + dir[2]},
					{b[0] + STRETCH * dir[0] + BROADEN * dir[1], b[1] + STRETCH * dir[1] - BROADEN * dir[0],
					 b[2] + dir[2]},
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
			return (i >> 1) < NUM_VARS ? o.vars[i >> 1] : 0;
		return (i >> 1) < 8 ? r.t[i >> 1] : 0;
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
			r.m[0] = {1, 0, 0};
			r.m[1] = {0, 1, 0};
			r.m[2] = {0, 0, -1};
			return;
		}
		int pair = (w0 >> 5) & 3;
		/* the light, away from it, in the model's axes; the game view's
		 * z is GL's flipped, as are the axes, so the dots agree */
		const P3 light = {o.light[0], o.light[1], o.light[2]};
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
				new_node(zkey(*org), false).planet = true;
			return;
		}
		Run c;
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
				sparam = (int16_t)r.t[(w[2] >> 12) & 7];
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
		run(c, depth + 1);
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
		if (!sky && n > 1 && !normal_faces(r, n))
			return;
		auto it = shown->texts.find(text_key(o.addr, w[3]));
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
			g.t[0] = (uint16_t)(r.t[0] + 1);
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
					jump = !sky && (ins.op == 0x0b ? !faces : faces);
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
				int v = param(r, w[1] & 0xff), bit = w[1] >> 8;
				bool set = v != 0 && (bit == 0 || (v >> (bit - 1)) & 1);
				jump = ins.op == 0x13 ? !set : set;
				break;
			}
			case 0x17: /* ifdist */
			{
				auto d = apart(r, fe2::s8(w[2]), fe2::s8(w[2] >> 8));
				if (d)
					jump = (w[1] & 0x8000) ? *d > (w[1] & 0x7fff) : *d <= w[1];
				break;
			}
			case 0x0d:
			case 0x1d:
			{
				/* bits 4-7, the lowest being the op's own bit 4: $0d has the
				 * even functions, $1d the odd ones */
				int fn = (w[0] >> 4) & 15;
				r.t[(w[0] >> 8) & 7] = (uint16_t)calc(fn, param(r, w[1] & 0xff), param(r, w[1] >> 8));
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
					Node &n = new_node(zkey(*p), false);
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
	Rgb8 c = rgb444_to_rgb8(rgb444);
	gd_color3ub(c.r, c.g, c.b);
}

/* Each object's models, run */
std::vector<Drawn> drawn;

uint32_t frame_seed;

/* Runs object o's model into d; the sky round the camera */
void run_object(const Object &o, Drawn &d, bool wire, bool sky)
{
	d = Drawn();
	const Code *c = code_for(o.table, o.model);
	if (!c)
		return;
	Runner rn{o, d, wire, c->model.scale + c->model.scale2 - o.scale, frame_seed};
	rn.sky = sky;
	Run r;
	r.c = c;
	r.org = sky ? P3{0, 0, 0} : P3{o.pos[0], o.pos[1], o.pos[2]};
	for (int a = 0; a < 3; a++)
		r.ax[a] = {o.cols[a][0], o.cols[a][1], o.cols[a][2]};
	r.s = o.scale;
	r.k = std::ldexp(1.0f, o.scale);
	r.base = 0x111; /* L390f2 */
	r.shade0 = o.tint[3];
	std::memcpy(r.t, o.t, sizeof r.t);
	for (int a = 0; a < 3; a++)
		r.m[a] = r.ax[a];
	r.pv[0] = r.org;
	rn.run(r, 0);
}

void run_objects(bool wire)
{
	frame_seed++;
	drawn.resize((size_t)shown->n);
	for (int i = 0; i < shown->n; i++)
	{
		const Object &o = shown->obj[i];
		const Code *c = code_for(o.table, o.model);
		/* filled, planets are drawn with the sky (draw_fars) */
		if (c && c->planet && !wire)
			drawn[(size_t)i] = Drawn();
		else
			run_object(o, drawn[(size_t)i], wire, false);
		drawn[(size_t)i].shrink = o.shrink;
	}
}

void sort_nodes(std::vector<Node> &nodes);

/* This frame's objects run (native_draw_sky does it first, native_draw
 * otherwise), each with the clip w it spans */
bool prepared = false;
void prepare(bool wire)
{
	if (prepared)
		return;
	prepared = true;
	run_objects(wire);
	mat4 mvp = gd_mvp();
	auto clip_w = [&](const P3 &p) { return mvp.m[3] * p[0] + mvp.m[7] * p[1] + mvp.m[11] * p[2] + mvp.m[15]; };
	for (Drawn &d : drawn)
	{
		auto span = [&](const P3 &p) {
			float w = clip_w(p);
			d.near = std::min(d.near, w);
			d.far = std::max(d.far, w);
		};
		for (const Node &n : d.nodes)
		{
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

void draw_nodes(const std::vector<Node> &nodes)
{
	for (const Node &n : nodes)
	{
		for (const Tri &t : n.tris)
		{
			set_colour(t.colour);
			gd_tri(t.p[0].data(), t.p[1].data(), t.p[2].data());
		}
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

/* Far bodies and planets, farthest first */
void draw_fars()
{
	for (int i = 0; i < shown->n && !screen_only; i++)
	{
		const Object &o = shown->obj[i];
		const Code *c = code_for(o.table, o.model);
		if (c && c->planet)
			planet_native_object(o.addr, o.pos, o.cols);
	}
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
	for (int i = 0; i < shown->nfar; i++)
		order.push_back({dist(shown->far[i].pos) * shown->far[i].shrink, i, -1, nullptr});
	double nearest_planet = 1e300;
	for (int i = 0; i < planet_native_count(); i++)
	{
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
extern "C" void native_draw_sky(void)
{
	if (!native_enabled() || enabled != 2)
		return;
	gd_push();
	gd_identity();
	prepare(false);
	if (shown->has_sky)
	{
		Drawn d;
		run_object(shown->sky, d, false, true);
		draw_nodes(d.nodes);
	}
	for (const Drawn &d : drawn)
		draw_nodes(d.before_planet);
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
		if (d.far > 1.0f && !d.back)
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
	/* Underlays, all objects', in the game's order: each before any object
	 * with a part nearer than its key (a city's ground plates, far back,
	 * before everything), after objects wholly beyond it (a ship seen past
	 * a station's bay, which the bay then hides). Painted, not depth
	 * tested: they need not lie under what is drawn on them in 3D. */
	std::vector<std::pair<float, const Node *>> unders; /* true key, node */
	for (const Drawn *d : placed)
		for (const Node &n : d->nodes)
			if (n.under)
				unders.push_back({n.key * d->shrink, &n});
	std::stable_sort(unders.begin(), unders.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
	size_t next_under = 0;
	auto draw_unders = [&](float beyond) {
		if (next_under >= unders.size() || unders[next_under].first <= beyond)
			return;
		gd_set_depth(nullptr);
		for (; next_under < unders.size() && unders[next_under].first > beyond; next_under++)
		{
			const Node *n = unders[next_under].second;
			for (const Tri &t : n->tris)
			{
				set_colour(t.colour);
				gd_tri(t.p[0].data(), t.p[1].data(), t.p[2].data());
			}
			draw_segs(n->segs);
			draw_dots(n->dots);
		}
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
		float nearest = 1e30f;
		for (size_t j = groups[g].first; j < groups[g].second; j++)
			for (const Node &n : placed[j]->nodes)
				if (!n.under)
					nearest = std::min(nearest, n.key * placed[j]->shrink);
		draw_unders(nearest);
		for (size_t j = groups[g].first; j < groups[g].second; j++)
		{
			Drawn &d = *placed[j];
			/* Within the object, the game's order: its depth tree's nodes
			 * farthest first, each in the model's order, as the models were
			 * made for (a city's buildings stand half in its ground, decals
			 * lie on their faces), hidden only by other objects... */
			sort_nodes(d.nodes);
			depth.write = false;
			depth.depth_only = false;
			gd_set_depth(&depth);
			for (const Node &n : d.nodes)
			{
				if (n.under)
					continue;
				for (const Tri &t : n.tris)
				{
					set_colour(t.colour);
					gd_tri(t.p[0].data(), t.p[1].data(), t.p[2].data());
				}
				draw_segs(n.segs);
				draw_dots(n.dots);
			}
			/* ...then its depth, to hide what is behind it of the others */
			depth.write = true;
			depth.depth_only = true;
			gd_set_depth(&depth);
			for (const Node &n : d.nodes)
				if (!n.under)
					for (const Tri &t : n.tris)
						gd_tri(t.p[0].data(), t.p[1].data(), t.p[2].data());
		}
	}
	draw_unders(-1e30f);
	gd_set_depth(nullptr);
	gd_pop();
}
