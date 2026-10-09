/*
 * gl_native.cpp - see gl_native.h.
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

extern "C"
{
#include "gl_cockpit.h"
#include "gl_draw.h"
#include "gl_native.h"
#include "gl_scene.h"
#include "game_state.h"
#include "host.h"
#include "main.h"
}

#include "fe2core.h"

/* ---- the game's models, read from its own memory --------------------------- */

/* fe2core's image is the game binary as loaded: at address 0 of emulated RAM.
 * Models don't change while the game runs. */
const fe2::Image &fe2::Image::game()
{
	static Image img;
	static std::once_flag once;
	std::call_once(once, [] {
		img.data.assign((const uint8_t *)m68kram, (const uint8_t *)m68kram + MEM_SIZE);
		img.table = FE2_gamedata1;
		img.gamedata2 = FE2_gamedata2;
		img.core_strings = FE2_L3e894;
		int first = 0x10000;
		for (int i = 0; i * 2 < first; i++)
		{
			int o = img.u16(img.table + i * 2);
			if (o)
				first = std::min(first, o);
		}
		img.num_models = first / 2;
	});
	return img;
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
	bool planet = false; /* drawn by the planet code */
};

P3 to_p3(const fe2::Vec3 &v)
{
	return {(float)v.x, (float)v.y, (float)v.z};
}

std::unordered_map<int, std::unique_ptr<Code>> codes;

/* Model `index`, or null */
const Code *code_for(int index)
{
	auto it = codes.find(index);
	if (it != codes.end())
		return it->second.get();
	std::unique_ptr<Code> &slot = codes[index];
	const fe2::Image &img = fe2::Image::game();
	auto model = img.model(index);
	if (!model)
		return nullptr;
	bool ok = true;
	auto ins = fe2::decode(img.data, model->code_addr(), 0x8000, &ok);
	if (!ok || ins.empty())
		return nullptr;
	slot.reset(new Code);
	Code &c = *slot;
	c.model = *model;
	c.ins = std::move(ins);
	c.faces.resize(c.ins.size());
	c.lines.resize(c.ins.size());
	/* vertex -1, the point the parent called it at, is its origin */
	fe2::Model::ParentFn origin = [](int i) -> std::optional<fe2::Vec3> {
		return i == -1 ? std::optional<fe2::Vec3>(fe2::Vec3()) : std::nullopt;
	};
	for (size_t i = 0; i < c.ins.size(); i++)
	{
		c.at[c.ins[i].off] = i;
		if (c.ins[i].op == 0x1f)
			c.planet = true;
		for (const fe2::Prim &p : fe2::primitives(c.model, {c.ins[i]}, origin))
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
				c.faces[i].push_back(std::move(f));
			}
			else if (p.kind == fe2::Prim::Line)
			{
				for (size_t j = 0; j + 1 < p.pts.size(); j++)
					c.lines[i].push_back({p.colour, to_p3(p.pts[j]), to_p3(p.pts[j + 1])});
			}
		}
	}
	return &c;
}

/* ---- captured objects -------------------------------------------------------- */

constexpr int NUM_VARS = 128;

/* An object as the game's own camera sees it, GL view space (z flipped) */
struct Object
{
	uint32_t addr;
	int model;
	int scale;     /* s: view = pos + R v 2^s */
	float pos[3];  /* view units */
	float cols[3][3]; /* R's columns: the model's axes */
	float light[3];   /* away from the light, unit */
	uint16_t tint[8]; /* the light's colour by how square a face is to it */
	uint16_t vars[NUM_VARS]; /* the object's model variables (words at +114) */
	int detail[2];           /* L60d2_optdetail1, L60d4_optdetail2 */
	/* The temporaries the model starts with (-64(a6)): t0 is cleared, the
	 * others are what is left there, which models like the cities use */
	uint16_t t[8];
};

constexpr int MAX_OBJECTS = 256;
struct Frame
{
	Object obj[MAX_OBJECTS];
	int n = 0;
};
Frame rec, pub, show;

int enabled = -1;

} // namespace

extern "C" bool native_enabled(void)
{
	if (enabled < 0)
	{
		const char *e = getenv("GLF_NATIVE_SCENE");
		enabled = e ? std::max(0, atoi(e)) : 0;
	}
	return enabled != 0;
}

extern "C" void Nu_NativeObject(void)
{
	if (!native_enabled() || !scene_active())
		return;
	uint32_t a6 = (uint32_t)GetReg(REG_A6);
	uint32_t addr = (uint32_t)rdlong(a6 - 212);
	for (int i = 0; i < rec.n; i++)
		if (rec.obj[i].addr == addr)
			return; /* already captured, from another pass */
	if (rec.n >= MAX_OBJECTS)
		return;
	Object &o = rec.obj[rec.n++];
	o.addr = addr;
	o.model = (uint16_t)rdword(addr + 90) / 2;
	o.scale = (int16_t)rdword(a6 - 208);
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
	/* the light vector (L60e2_lighting_vector or the object's own frame's)
	 * and the tint table (L60f6_light_tint_table) as L390f2 set them up */
	float l[3] = {(float)(int16_t)rdword(a6 - 198), (float)(int16_t)rdword(a6 - 196),
				  -(float)(int16_t)rdword(a6 - 194)};
	cockpit_pass_to_view(l, o.light);
	float len = std::sqrt(o.light[0] * o.light[0] + o.light[1] * o.light[1] + o.light[2] * o.light[2]);
	for (int j = 0; j < 3; j++)
		o.light[j] = len > 0 ? o.light[j] / len : 0;
	for (int j = 0; j < 8; j++)
		o.tint[j] = (uint16_t)rdword(a6 - 104 + 2 * (uint32_t)j);
	for (int j = 0; j < NUM_VARS; j++)
		o.vars[j] = (uint16_t)rdword(addr + 114 + 2 * (uint32_t)j);
	for (int j = 0; j < 8; j++)
		o.t[j] = j == 0 ? 0 : (uint16_t)rdword(a6 - 64 + 2 * (uint32_t)j);
	o.detail[0] = (int16_t)rdword(FE2_optdetail1);
	o.detail[1] = (int16_t)rdword(FE2_optdetail2);
}

extern "C" void native_reset(void)
{
	rec.n = 0;
}

extern "C" void native_publish(void)
{
	pub = rec;
}

extern "C" void native_acquire(void)
{
	show = pub;
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
struct Node
{
	float key; /* the game's depth key (zkey); the farthest is drawn first */
	std::vector<Tri> tris;
	std::vector<Seg> segs;
	/* Keyed farther back than it is (a batch pushed back by a depth): an
	 * underlay, ground plates, a starport's ground, cloud shadows. The game
	 * paints these under whatever is nearer, and they need not lie under it
	 * in 3D (a cloud's shadow hangs below the cloud, above the ground) */
	bool under = false;
};
struct Drawn
{
	std::vector<Node> nodes;
	std::vector<Seg> wire; /* the wireframe: outlines and lines */
	float near = 1e30f, far = 0; /* clip w (depth along the camera) it spans */
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
};

struct Runner
{
	const Object &o;
	Drawn &out;
	bool wire;
	int s2; /* -192(a6): sub-models' scales are relative to it */
	int batch = -1; /* the node a batch is going into (L385e4_3dview_word1) */

	/* The node a primitive keyed on point `at` goes into */
	Node &node(const P3 &at)
	{
		if (batch >= 0)
			return out.nodes[(size_t)batch];
		out.nodes.push_back({zkey(at), {}, {}, false});
		return out.nodes.back();
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
		out.nodes.push_back({key, {}, {}, under});
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
	static float dot(const P3 &a, const P3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
	static float len(const P3 &a) { return std::sqrt(dot(a, a)); }
	/* A depth tree key as the game works it out (L3a202): depth along its
	 * camera plus (|x| + |y|) / 8. The models' drawing order is tuned to it,
	 * so it is kept, in the game camera's view space (GL's, z flipped). */
	static float zkey(const P3 &a) { return -a[2] + (std::fabs(a[0]) + std::fabs(a[1])) / 8; }

	std::optional<P3> vertex(const Run &r, int i) const
	{
		static const fe2::Model::ParentFn origin = [](int j) -> std::optional<fe2::Vec3> {
			return j == -1 ? std::optional<fe2::Vec3>(fe2::Vec3()) : std::nullopt;
		};
		auto v = r.c->model.vertex_pos(i, origin);
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

	/* `if > z:K` / `if < z:K`: K << (detail + s) against the depth along
	 * the game's camera (-8(a6)), as the game does, to pick the same detail
	 * (behind it, never beyond); nullopt when the shift overflows */
	std::optional<bool> beyond(const Run &r, int k) const
	{
		int sh = o.detail[k & 1] + r.s;
		if (sh < 0 || sh > 31 || ((int64_t)k << sh) > 0x7fffffff)
			return std::nullopt;
		return -r.org[2] > (float)((int64_t)k << sh);
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
		for (const Face &f : r.c->faces[i])
		{
			P3 n = turn(r, f.n);
			P3 a = place(r, f.tris[0]);
			if (f.has_normal && dot(n, a) >= 0)
				continue; /* faces away */
			int sh = f.has_normal ? tint(n) : shade(r, f.nidx);
			uint16_t col = (uint16_t)colour(f.colour, sh, r.base);
			if (wire)
			{
				for (size_t j = 0; j < f.outline.size(); j++)
					out.wire.push_back(
						{place(r, f.outline[j]), place(r, f.outline[(j + 1) % f.outline.size()]), col});
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
		for (const Line &l : r.c->lines[i])
		{
		{
			Seg s = {place(r, l.a), place(r, l.b), (uint16_t)colour(l.colour, r.shade0, r.base)};
			if (wire)
				out.wire.push_back(s);
			else
				node(s.a).segs.push_back(s);
		}
		}
	}

	/* a sub-model call (L3c928, L3c8d8): op $0e, or $1b with a scale and
	 * a colour */
	void call(const Run &r, const fe2::Instr &ins, int depth)
	{
		const auto &w = ins.w;
		const Code *child = code_for((w[0] >> 5) & 0x7ff);
		if (!child || child == r.c || child->planet)
			return;
		auto org = vertex(r, fe2::s8(w[1]));
		if (!org)
			return;
		Run c;
		c.c = child;
		c.org = *org;
		c.base = r.base;
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
		/* its axes: the parent's, swapped and negated (L3c3be); code 6 is
		 * the matrix made by mrotate, not done yet: the parent's */
		static const int SWAPS[8][3] = {{0, 1, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0},
										{1, 0, 2}, {0, 2, 1}, {0, 1, 2}, {0, 1, 2}};
		int code = (w[1] >> 8) & 7;
		const int bits[3] = {0x2000, 0x1000, 0x0800};
		for (int i = 0; i < 3; i++)
		{
			float sign = code != 6 && (w[1] & bits[i]) ? -1.0f : 1.0f;
			int to = code == 6 ? i : SWAPS[code][i];
			for (int j = 0; j < 3; j++)
				c.ax[to][j] = r.ax[i][j] * sign;
		}
		c.shade0 = o.tint[3];
		std::memset(c.t, 0, sizeof c.t);
		c.t[0] = (uint16_t)(r.t[0] + 1);
		c.t[1] = r.t[1];
		c.t[2] = r.t[2];
		run(c, depth + 1);
	}

	void run(Run &r, int depth)
	{
		if (depth > 8 || r.c->planet)
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
					jump = ins.op == 0x0b ? !faces : faces;
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
			case 0x06:
				begin_batch(r, ins);
				break;
			case 0x15: /* ztree: sorting within a batch, not done */
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

void run_objects(bool wire)
{
	drawn.resize((size_t)show.n);
	for (int i = 0; i < show.n; i++)
	{
		const Object &o = show.obj[i];
		Drawn &d = drawn[(size_t)i];
		d = Drawn();
		const Code *c = code_for(o.model);
		if (!c)
			continue;
		Runner rn{o, d, wire, c->model.scale + c->model.scale2 - o.scale};
		Run r;
		r.c = c;
		r.org = {o.pos[0], o.pos[1], o.pos[2]};
		for (int a = 0; a < 3; a++)
			r.ax[a] = {o.cols[a][0], o.cols[a][1], o.cols[a][2]};
		r.s = o.scale;
		r.k = std::ldexp(1.0f, o.scale);
		r.base = 0x111; /* L390f2 */
		r.shade0 = o.tint[3];
		std::memcpy(r.t, o.t, sizeof r.t);
		rn.run(r, 0);
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

} // namespace

extern "C" void native_draw(void)
{
	if (!native_enabled())
		return;
	bool wire = enabled == 1;
	run_objects(wire);
	gd_push();
	gd_identity();
	if (wire)
	{
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
		gd_pop();
		return;
	}

	/* Each object spans near..far in clip w. Objects whose spans overlap
	 * share a depth slice, so they hide each other properly; the slices go
	 * nearest first, so slices hide each other too, and each is precise
	 * however far away it is. */
	mat4 mvp = gd_mvp();
	auto clip_w = [&](const P3 &p) { return mvp.m[3] * p[0] + mvp.m[7] * p[1] + mvp.m[11] * p[2] + mvp.m[15]; };
	std::vector<Drawn *> placed;
	for (Drawn &d : drawn)
	{
		for (const Node &n : d.nodes)
		{
			for (const Tri &t : n.tris)
				for (const P3 &p : t.p)
				{
					float w = clip_w(p);
					d.near = std::min(d.near, w);
					d.far = std::max(d.far, w);
				}
			for (const Seg &s : n.segs)
				for (const P3 &p : {s.a, s.b})
				{
					float w = clip_w(p);
					d.near = std::min(d.near, w);
					d.far = std::max(d.far, w);
				}
		}
		if (d.far > 1.0f)
			placed.push_back(&d);
	}
	std::sort(placed.begin(), placed.end(), [](const Drawn *a, const Drawn *b) { return a->near < b->near; });
	std::vector<std::pair<size_t, size_t>> groups; /* [first, last) in placed */
	float group_far = 0;
	for (size_t i = 0; i < placed.size(); i++)
	{
		if (!groups.empty() && placed[i]->near < group_far)
			groups.back().second = i + 1;
		else
		{
			groups.push_back({i, i + 1});
			group_far = 0;
		}
		group_far = std::max(group_far, placed[i]->far);
	}
	/* Underlays first, all objects', farthest first, under everything */
	std::vector<const Node *> unders;
	for (const Drawn *d : placed)
		for (const Node &n : d->nodes)
			if (n.under)
				unders.push_back(&n);
	std::stable_sort(unders.begin(), unders.end(), [](const Node *a, const Node *b) { return a->key > b->key; });
	for (const Node *n : unders)
	{
		for (const Tri &t : n->tris)
		{
			set_colour(t.colour);
			gd_tri(t.p[0].data(), t.p[1].data(), t.p[2].data());
		}
		draw_segs(n->segs);
	}

	float slice = groups.empty() ? 0 : 2.0f / (float)groups.size();
	for (size_t g = groups.size(); g-- > 0;)
	{
		GdDepth depth = {};
		depth.near = std::max(placed[groups[g].first]->near * 0.99f, 1.0f);
		for (size_t j = groups[g].first; j < groups[g].second; j++)
			depth.far = std::max(depth.far, placed[j]->far * 1.01f + 1.0f);
		depth.lo = -1 + slice * (float)g;
		depth.hi = -1 + slice * (float)(g + 1);
		for (size_t j = groups[g].first; j < groups[g].second; j++)
		{
			Drawn &d = *placed[j];
			/* Within the object, the game's order: its depth tree's nodes
			 * farthest first, each in the model's order, as the models were
			 * made for (a city's buildings stand half in its ground, decals
			 * lie on their faces), hidden only by other objects... */
			std::stable_sort(d.nodes.begin(), d.nodes.end(),
							 [](const Node &a, const Node &b) { return a.key > b.key; });
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
	gd_set_depth(nullptr);
	gd_pop();
}
