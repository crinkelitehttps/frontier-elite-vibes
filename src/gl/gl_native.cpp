/*
 * gl_native.cpp - see gl_native.h.
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
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

struct Edge
{
	float a[3], b[3];
	uint8_t r, g, b_;
};

/* A face, triangulated. Normal: unit, model axes; none = drawn from both
 * sides and lit like the game's normal 0 */
struct Face
{
	int colour;
	bool has_normal;
	float n[3];
	std::vector<std::array<float, 3>> tris; /* 3 per triangle */
	int layer = 0; /* decals: 1 + the layer of what they lie on */
};

struct Line
{
	int colour;
	float a[3], b[3];
};

/* A model in its own units (the game's vertex bytes): edges for the
 * wireframe, faces and lines (in the model's order) for the filled scene */
struct Mesh
{
	std::vector<Edge> edges;
	std::vector<Face> faces;
	std::vector<Line> lines;
	float radius = 0; /* of its vertices, about its origin */
	int layers = 0;   /* 1 + the highest face layer */
};

/* The game draws a decal (a stripe, a window, a sign) after the face it
 * lies on, and by painter's algorithm it wins. With the depth buffer the
 * two are at the same depth, so each face lying on an earlier one, in its
 * plane and overlapping it, gets a layer above it, drawn a little nearer.
 * Faces are bucketed by plane so big models stay quick. */
void find_layers(Mesh &m)
{
	float tol = std::max(m.radius * 1e-3f, 1e-4f);
	struct Info
	{
		float lo[3], hi[3];
		const Face *f;
	};
	std::unordered_map<uint64_t, std::vector<Info>> planes;
	for (Face &f : m.faces)
	{
		float n[3] = {f.n[0], f.n[1], f.n[2]};
		/* one way round, for faces seen from both sides */
		int big = 0;
		for (int j = 1; j < 3; j++)
			if (std::fabs(n[j]) > std::fabs(n[big]))
				big = j;
		if (n[big] < 0)
			for (float &x : n)
				x = -x;
		float d = n[0] * f.tris[0][0] + n[1] * f.tris[0][1] + n[2] * f.tris[0][2];
		auto q = [](float x, float s) { return (uint64_t)(int64_t)std::lround(x / s) & 0xffff; };
		uint64_t key = q(n[0], 1 / 32.0f) | q(n[1], 1 / 32.0f) << 16 | q(n[2], 1 / 32.0f) << 32 | q(d, 4 * tol) << 48;
		Info in;
		in.f = &f;
		for (int j = 0; j < 3; j++)
		{
			in.lo[j] = in.hi[j] = f.tris[0][j];
			for (auto &v : f.tris)
			{
				in.lo[j] = std::min(in.lo[j], v[j]);
				in.hi[j] = std::max(in.hi[j], v[j]);
			}
		}
		auto &bucket = planes[key];
		for (const Info &e : bucket)
		{
			bool overlap = true;
			for (int j = 0; j < 3; j++)
				overlap = overlap && in.lo[j] < e.hi[j] - tol && e.lo[j] < in.hi[j] - tol;
			if (overlap)
				f.layer = std::max(f.layer, e.f->layer + 1);
		}
		bucket.push_back(in);
		m.layers = std::max(m.layers, f.layer + 1);
	}
}

std::unordered_map<int, Mesh> meshes;

const Mesh &mesh_for(int model)
{
	auto it = meshes.find(model);
	if (it != meshes.end())
		return it->second;
	Mesh &m = meshes[model];
	const fe2::Image &img = fe2::Image::game();
	auto root = img.model(model);
	if (!root)
		return m;
	/* planets (and gas giants' rings) are drawn by the planet code */
	for (auto &ins : fe2::decode(img.data, root->code_addr()))
		if (ins.op == 0x1f)
			return m;
	fe2::Collected c = fe2::collect(img, model, true, 4, 20000);
	double k = std::ldexp(1.0, -root->scale);
	auto add = [&](const std::vector<fe2::Vec3> &pts, int colour, bool closed) {
		fe2::Vec3 rgb = fe2::display_rgb(colour);
		size_t n = pts.size();
		for (size_t i = 0; i + (closed ? 0 : 1) < n; i++)
		{
			const fe2::Vec3 &p = pts[i], &q = pts[(i + 1) % n];
			Edge e;
			for (int j = 0; j < 3; j++)
			{
				e.a[j] = (float)(p[j] * k);
				e.b[j] = (float)(q[j] * k);
			}
			e.r = (uint8_t)(rgb.x * 255);
			e.g = (uint8_t)(rgb.y * 255);
			e.b_ = (uint8_t)(rgb.z * 255);
			m.edges.push_back(e);
		}
	};
	auto grow = [&](const fe2::Vec3 &p) { m.radius = std::max(m.radius, (float)(fe2::length(p) * k)); };
	for (auto &f : c.faces)
	{
		if (f.group.find("~far") != std::string::npos)
			continue;
		add(f.pts, f.colour, true);
		if (f.pts.size() < 3)
			continue;
		Face face;
		face.colour = f.colour;
		face.has_normal = f.normal.has_value();
		fe2::Vec3 n = fe2::normalize(f.normal ? *f.normal : fe2::newell(f.pts));
		for (int j = 0; j < 3; j++)
			face.n[j] = (float)n[j];
		for (auto &t : fe2::triangulate(f.pts, n))
			for (int v : t)
			{
				grow(f.pts[v]);
				face.tris.push_back({(float)(f.pts[v].x * k), (float)(f.pts[v].y * k), (float)(f.pts[v].z * k)});
			}
		m.faces.push_back(std::move(face));
	}
	for (auto &l : c.lines)
	{
		if (l.group.find("~far") != std::string::npos)
			continue;
		add(l.pts, l.colour, false);
		for (size_t i = 0; i + 1 < l.pts.size(); i++)
		{
			Line line;
			line.colour = l.colour;
			for (int j = 0; j < 3; j++)
			{
				line.a[j] = (float)(l.pts[i][j] * k);
				line.b[j] = (float)(l.pts[i + 1][j] * k);
			}
			grow(l.pts[i]);
			grow(l.pts[i + 1]);
			m.lines.push_back(line);
		}
	}
	find_layers(m);
	return m;
}

/* ---- captured objects -------------------------------------------------------- */

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

namespace
{

/* A face's colour as the game works it out (L3a0ac, l3a15a): its colour
 * value plus the light's tint for the face's normal (none for $100,
 * unlit), plus the object's base colour for $10 */
int face_colour(int colour, int tint)
{
	/* the base colour the game starts every object with; models can set
	 * their own (to do, with the object's state) */
	const int base = 0x111;
	int c = colour & 0xffe;
	bool add_base = c & 0x10;
	c &= ~0x10;
	if (c & 0x100)
		c &= ~0x100;
	else
		c = (c + tint) & 0xfff;
	if (add_base)
		c += base;
	return c;
}

/* The tint for a unit normal in view axes (L3a0ac). The light vector
 * points away from the light: a face square to it (n . light = -1) gets
 * tint 0, the brightest, one edge on gets 7, one facing away none. The game
 * takes bits 30-28 of the negative 2 n . light (normals are bytes, at most
 * 127/128 long). */
int face_tint(const Object &o, const float n[3])
{
	float d = (n[0] * o.light[0] + n[1] * o.light[1] + n[2] * o.light[2]) * (127.0f / 128.0f);
	if (d >= 0)
		return 0;
	return o.tint[std::max(0, std::min(7, (int)std::floor(8 * (1 + d))))];
}

void set_colour(int rgb444)
{
	Rgb8 c = rgb444_to_rgb8(rgb444);
	gd_color3ub(c.r, c.g, c.b);
}

struct Placed
{
	const Object *o;
	const Mesh *m;
	float near, far; /* view distances its model spans */
};

/* One object, filled and lit, in its depth slice; up to `headroom` of NDC
 * is left in front of the slice for biasing faces */
void draw_filled(const Placed &p, GdDepth depth, float headroom)
{
	const Object &o = *p.o;
	const Mesh &m = *p.m;
	float k = std::ldexp(1.0f, o.scale);
	auto place = [&](const float v[3], float out[3]) {
		for (int j = 0; j < 3; j++)
			out[j] = o.pos[j] + (o.cols[0][j] * v[0] + o.cols[1][j] * v[1] + o.cols[2][j] * v[2]) * k;
	};
	/* Decals a little nearer than what they lie on (find_layers): 16 steps
	 * of a 24 bit depth buffer a layer, lines above all the faces */
	const float step = 1.0f / (1 << 19);
	for (const Face &f : m.faces)
	{
		float n[3];
		for (int j = 0; j < 3; j++)
			n[j] = o.cols[0][j] * f.n[0] + o.cols[1][j] * f.n[1] + o.cols[2][j] * f.n[2];
		float a[3], b[3], c[3];
		place(f.tris[0].data(), a);
		if (f.has_normal && n[0] * a[0] + n[1] * a[1] + n[2] * a[2] >= 0)
			continue; /* faces away */
		/* no normal: lit like the game's normal 0, half way */
		set_colour(face_colour(f.colour, f.has_normal ? face_tint(o, n) : o.tint[3]));
		depth.bias = std::min(step * (float)f.layer, headroom);
		gd_set_depth(&depth);
		for (size_t t = 0; t + 2 < f.tris.size(); t += 3)
		{
			place(f.tris[t].data(), a);
			place(f.tris[t + 1].data(), b);
			place(f.tris[t + 2].data(), c);
			gd_tri(a, b, c);
		}
	}
	depth.bias = std::min(step * (float)m.layers, headroom);
	gd_set_depth(&depth);
	for (const Line &l : m.lines)
	{
		float a[3], b[3];
		place(l.a, a);
		place(l.b, b);
		set_colour(face_colour(l.colour, o.tint[3]));
		gd_line(a, b);
	}
}

void draw_wire(const Object &o, const Mesh &m)
{
	float k = std::ldexp(1.0f, o.scale);
	auto place = [&](const float v[3], float out[3]) {
		for (int j = 0; j < 3; j++)
			out[j] = o.pos[j] + (o.cols[0][j] * v[0] + o.cols[1][j] * v[1] + o.cols[2][j] * v[2]) * k;
	};
	for (const Edge &e : m.edges)
	{
		float a[3], b[3];
		place(e.a, a);
		place(e.b, b);
		gd_color3ub(e.r, e.g, e.b_);
		gd_line(a, b);
	}
}

} // namespace

extern "C" void native_draw(void)
{
	if (!native_enabled())
		return;
	gd_push();
	gd_identity();
	if (enabled == 1)
	{
		for (int i = 0; i < show.n; i++)
			draw_wire(show.obj[i], mesh_for(show.obj[i].model));
		gd_pop();
		return;
	}

	/* Each object spans near..far in view distance (GL looks down -z).
	 * Objects whose spans overlap share a depth slice, so they hide each
	 * other properly; the slices go nearest first, so slices hide each other
	 * too, and each is precise however far away it is. */
	std::vector<Placed> placed;
	for (int i = 0; i < show.n; i++)
	{
		const Object &o = show.obj[i];
		const Mesh &m = mesh_for(o.model);
		if (m.faces.empty() && m.lines.empty())
			continue;
		float r = m.radius * std::ldexp(1.0f, o.scale) * 1.01f + 1.0f;
		float d = -o.pos[2];
		if (d + r <= 1.0f)
			continue; /* behind the camera */
		placed.push_back({&o, &m, std::max(d - r, 1.0f), d + r});
	}
	std::sort(placed.begin(), placed.end(), [](const Placed &a, const Placed &b) { return a.near < b.near; });
	std::vector<std::pair<size_t, size_t>> groups; /* [first, last) in placed */
	for (size_t i = 0; i < placed.size(); i++)
	{
		if (!groups.empty())
		{
			float far = 0;
			for (size_t j = groups.back().first; j < i; j++)
				far = std::max(far, placed[j].far);
			if (placed[i].near < far)
			{
				groups.back().second = i + 1;
				continue;
			}
		}
		groups.push_back({i, i + 1});
	}
	float slice = groups.empty() ? 0 : 2.0f / (float)groups.size();
	for (size_t g = groups.size(); g-- > 0;)
	{
		GdDepth depth;
		depth.near = placed[groups[g].first].near;
		depth.far = 0;
		for (size_t j = groups[g].first; j < groups[g].second; j++)
			depth.far = std::max(depth.far, placed[j].far);
		float headroom = slice * 0.05f;
		depth.lo = -1 + slice * (float)g + headroom;
		depth.hi = -1 + slice * (float)(g + 1);
		depth.bias = 0;
		for (size_t j = groups[g].first; j < groups[g].second; j++)
			draw_filled(placed[j], depth, headroom);
	}
	gd_set_depth(nullptr);
	gd_pop();
}
