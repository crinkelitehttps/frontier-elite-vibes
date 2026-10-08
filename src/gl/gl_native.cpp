/*
 * gl_native.cpp - see gl_native.h.
 */
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

/* A model's edges in its own units (the game's vertex bytes) */
struct Mesh
{
	std::vector<Edge> edges;
};

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
	for (auto &f : c.faces)
		if (f.group.find("~far") == std::string::npos)
			add(f.pts, f.colour, true);
	for (auto &l : c.lines)
		if (l.group.find("~far") == std::string::npos)
			add(l.pts, l.colour, false);
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
		enabled = e && atoi(e) > 0;
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

extern "C" void native_draw(void)
{
	if (!native_enabled())
		return;
	gd_push();
	gd_identity();
	for (int i = 0; i < show.n; i++)
	{
		const Object &o = show.obj[i];
		const Mesh &m = mesh_for(o.model);
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
	gd_pop();
}
