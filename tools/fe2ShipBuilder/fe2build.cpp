/*
 * fe2build.cpp - see fe2build.h.
 */
#include "fe2build.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <tuple>

namespace fe2
{

const char *const COLLISION_MODES[] = {"auto", "box", "kdop14", "kdop26", "custom"};
const int COLLISION_MODE_COUNT = 5;

static const int TWO_SIDED = 0x10000; /* marker on a colour: normal 0, never hidden */
static const int MAX_VERTS = 64, MAX_NORMS = 63;
static const int HEADER_SIZE = 0x1e;
static const int MODEL_FLAG_QUADS_OK = 1;

/* round half to even, like Python's round(), so the studio builds exactly
 * what tools/fe2ShipBuilder built */
static long rne(double x) { return (long)std::nearbyint(x); }

static std::string lower(std::string s)
{
	for (auto &c : s)
		c = (char)tolower((unsigned char)c);
	return s;
}
static bool starts(const std::string &s, const char *p) { return s.rfind(p, 0) == 0; }
static bool has(const std::string &s, const char *p) { return s.find(p) != std::string::npos; }
static bool ends(const std::string &s, const char *p)
{
	size_t n = strlen(p);
	return s.size() >= n && s.compare(s.size() - n, n, p) == 0;
}

/* -- colours ----------------------------------------------------------------- */

int material_colour(const std::string &name, const Vec3 *kd, bool emissive)
{
	std::string n = lower(name);
	if (starts(n, "fe2_") && n.size() >= 7)
	{
		char *end = nullptr;
		std::string hex = n.substr(4, 3);
		long v = strtol(hex.c_str(), &end, 16);
		if (end && *end == 0)
			return (int)v & 0xFFF;
	}
	if (has(n, "paint"))
		return 0x010;
	Vec3 c = kd ? *kd : Vec3(0.6, 0.6, 0.6);
	bool emit = has(n, "glow") || has(n, "emit") || has(n, "light") || emissive;
	auto q = [](double x, double mul, double sub) { return std::max(0, std::min(7, (int)rne((x * 15 - sub) / mul))); };
	if (emit)
		return (q(c.x, 2, 0) << 9) | (q(c.y, 2, 0) << 5) | (q(c.z, 2, 0) << 1) | 0x100;
	/* shaded: the game adds up to ~3 of lighting per channel */
	return (q(c.x, 2, 3) << 9) | (q(c.y, 2, 3) << 5) | (q(c.z, 2, 3) << 1);
}

bool two_sided(const std::string &name)
{
	std::string n = lower(name);
	return ends(n, "_2s") || has(n, "twosided") || has(n, "two_sided");
}

/* Like Blender's Recalculate Outside (see the Python original's notes):
 * returns faces flipped, or with check_only the number of closed pieces that
 * are inside out. Verts are in FE2 axes. */
static int orient_faces(const std::vector<Vec3> &verts, std::vector<BuildMesh::Face> &faces, bool check_only)
{
	std::map<std::pair<int, int>, std::vector<int>> edges;
	for (int fi = 0; fi < (int)faces.size(); fi++)
	{
		auto &idx = faces[fi].idx;
		for (size_t k = 0; k < idx.size(); k++)
		{
			int a = idx[k], b = idx[(k + 1) % idx.size()];
			edges[{std::min(a, b), std::max(a, b)}].push_back(fi);
		}
	}
	std::vector<bool> seen(faces.size(), false);
	int flipped = 0;
	auto has_directed = [&](int fi, int a, int b) {
		auto &idx = faces[fi].idx;
		for (size_t k = 0; k < idx.size(); k++)
			if (idx[k] == a && idx[(k + 1) % idx.size()] == b)
				return true;
		return false;
	};
	for (int start = 0; start < (int)faces.size(); start++)
	{
		if (seen[start])
			continue;
		std::vector<int> comp = {start}, todo = {start};
		seen[start] = true;
		bool consistent = true;
		while (!todo.empty())
		{
			int fi = todo.back();
			todo.pop_back();
			auto idx = faces[fi].idx;
			for (size_t k = 0; k < idx.size(); k++)
			{
				int a = idx[k], b = idx[(k + 1) % idx.size()];
				auto &shared = edges[{std::min(a, b), std::max(a, b)}];
				if (shared.size() != 2)
					continue;
				for (int nb : shared)
				{
					if (seen[nb])
						continue;
					seen[nb] = true;
					if (has_directed(nb, a, b))
					{
						consistent = false;
						if (!check_only)
						{
							std::reverse(faces[nb].idx.begin(), faces[nb].idx.end());
							flipped++;
						}
					}
					comp.push_back(nb);
					todo.push_back(nb);
				}
			}
		}
		bool closed = true;
		for (int fi : comp)
		{
			auto &idx = faces[fi].idx;
			for (size_t k = 0; k < idx.size() && closed; k++)
			{
				int a = idx[k], b = idx[(k + 1) % idx.size()];
				if (edges[{std::min(a, b), std::max(a, b)}].size() != 2)
					closed = false;
			}
		}
		if (!closed || (check_only && !consistent))
			continue;
		double vol = 0;
		for (int fi : comp)
		{
			std::vector<Vec3> pts;
			for (int i : faces[fi].idx)
				pts.push_back(verts[i]);
			vol += dot(pts[0], newell(pts));
		}
		/* outward faces in a right handed OBJ read into mirrored FE2 axes */
		if (vol > 0)
		{
			if (check_only)
			{
				flipped++;
				continue;
			}
			for (int fi : comp)
				std::reverse(faces[fi].idx.begin(), faces[fi].idx.end());
			flipped += (int)comp.size();
		}
	}
	return flipped;
}

struct Poly
{
	std::vector<Vec3> pts;
	Vec3 nrm;
	int colour;
};

static bool good_quad(const std::vector<Vec3> &q)
{
	Vec3 n1 = -newell({q[0], q[1], q[2]}), n2 = -newell({q[0], q[2], q[3]});
	double l1 = length(n1), l2 = length(n2);
	if (l1 < 1e-12 || l2 < 1e-12)
		return false;
	return dot(n1, n2) / (l1 * l2) > 0.995;
}

static bool is_collision(const std::string &name)
{
	std::string n = lower(name);
	return starts(n, "collision") || starts(n, "col_");
}

static void split_polygons(const BuildMesh &mesh, bool fix_normals, std::vector<Poly> &out,
						   std::map<std::string, std::vector<Poly>> &coll, int &flipped, int &inside_out)
{
	std::vector<BuildMesh::Face> faces;
	for (auto &f : mesh.faces)
	{
		if (is_collision(f.group) || is_collision(f.mat))
		{
			std::vector<Vec3> pts;
			for (int i : f.idx)
				pts.push_back(mesh.verts[i]);
			Vec3 n = newell(pts);
			if (length(n) > 1e-12)
				coll[f.group.empty() ? f.mat : f.group].push_back({pts, -n, 0});
		}
		else
			faces.push_back(f);
	}
	flipped = inside_out = 0;
	if (fix_normals)
		flipped = orient_faces(mesh.verts, faces, false);
	else
		inside_out = orient_faces(mesh.verts, faces, true);
	for (auto &f : faces)
	{
		std::vector<Vec3> clean;
		for (int i : f.idx)
		{
			const Vec3 &p = mesh.verts[i];
			if (clean.empty() || length(p - clean.back()) > 1e-9)
				clean.push_back(p);
		}
		if (clean.size() > 1 && length(clean.front() - clean.back()) < 1e-9)
			clean.pop_back();
		if (clean.size() < 3)
			continue;
		Vec3 nrm = newell(clean);
		if (length(nrm) < 1e-12)
			continue;
		nrm = -nrm; /* OBJ right handed, FE2 left handed */
		auto kd = mesh.kd.find(f.mat);
		auto em = mesh.emissive.find(f.mat);
		int colour = material_colour(f.mat, kd != mesh.kd.end() ? &kd->second : nullptr,
									 em != mesh.emissive.end() && em->second) |
					 (two_sided(f.mat) ? TWO_SIDED : 0);
		if (clean.size() == 3 || (clean.size() == 4 && good_quad(clean)))
		{
			out.push_back({clean, nrm, colour});
			continue;
		}
		for (auto &t : triangulate(clean, nrm))
		{
			std::vector<Vec3> tp = {clean[t[0]], clean[t[1]], clean[t[2]]};
			Vec3 tn = -newell(tp);
			if (length(tn) > 1e-12)
				out.push_back({tp, tn, colour});
		}
	}
}

/* -- per model tables ---------------------------------------------------------- */

using IV3 = std::array<int, 3>;

struct Part
{
	std::vector<std::array<int, 4>> verts; /* type, a, b, c */
	std::map<IV3, int> lookup;
	std::vector<std::array<int, 4>> norms; /* vertex, x, y, z */
	struct NL
	{
		IV3 vec;
		double d;
		int index;
	};
	std::vector<NL> nlookup;
	struct F
	{
		std::vector<int> v;
		int ni, col;
	};
	std::vector<F> faces;

	int add_vertex(IV3 p, int type = 1)
	{
		auto it = lookup.find(p);
		if (it != lookup.end())
			return it->second;
		IV3 m = {-p[0], p[1], p[2]};
		it = lookup.find(m);
		if (it != lookup.end())
			return it->second ^ 1;
		if ((int)verts.size() >= MAX_VERTS)
			throw BuildError("too many vertices");
		int i = (int)verts.size() * 2;
		verts.push_back({type, p[0], p[1], p[2]});
		lookup[p] = i;
		if (m != p)
			lookup[m] = i | 1;
		return i;
	}
	IV3 point(int index) const
	{
		auto &v = verts[index >> 1];
		return {(index & 1) ? -v[1] : v[1], v[2], v[3]};
	}
	int find_normal(IV3 vec, double d) const
	{
		for (auto &n : nlookup)
			if (n.vec == vec && std::fabs(n.d - d) <= 127 * 1.5)
				return n.index;
		return -1;
	}
	static double dotv(IV3 a, IV3 b) { return (double)a[0] * b[0] + (double)a[1] * b[1] + (double)a[2] * b[2]; }
	int add_normal(IV3 vec, int vindex)
	{
		IV3 p = point(vindex);
		double d = dotv(vec, p);
		int i = find_normal(vec, d);
		if (i >= 0)
			return i;
		if ((int)norms.size() >= MAX_NORMS)
			throw BuildError("too many normals");
		i = ((int)norms.size() + 1) * 2;
		norms.push_back({vindex, vec[0], vec[1], vec[2]});
		nlookup.push_back({vec, d, i});
		IV3 m = {-vec[0], vec[1], vec[2]};
		if (m != vec)
			nlookup.push_back({m, dotv(m, {-p[0], p[1], p[2]}), i | 1});
		return i;
	}
	int radius() const
	{
		double r = 0;
		for (auto &v : verts)
			if (v[0] <= 2 || v[0] == 9 || v[0] == 10)
				r = std::max(r, std::sqrt((double)v[1] * v[1] + (double)v[2] * v[2] + (double)v[3] * v[3]));
		return (int)std::ceil(r) + 1;
	}
};

static IV3 qnormal(const Vec3 &n)
{
	double l = length(n);
	IV3 q;
	for (int i = 0; i < 3; i++)
		q[i] = std::max(-127, std::min(127, (int)rne(n[i] * 127 / l)));
	return q;
}

static int fit_scale(double extent)
{
	int s = 0;
	while (extent / std::ldexp(1.0, s) > 126.5)
		s++;
	return s;
}

static bool quantize_face(const Poly &p, int scale, const Vec3 &centre, std::vector<IV3> &qp, IV3 &qn)
{
	double k = std::ldexp(1.0, scale);
	qp.clear();
	for (auto &pt : p.pts)
	{
		IV3 q;
		for (int i = 0; i < 3; i++)
			q[i] = std::max(-127, std::min(127, (int)rne((pt[i] - centre[i]) / k)));
		if (std::find(qp.begin(), qp.end(), q) == qp.end())
			qp.push_back(q);
	}
	if (qp.size() < 3)
		return false;
	std::vector<Vec3> f;
	for (auto &q : qp)
		f.push_back(Vec3(q[0], q[1], q[2]));
	if (length(newell(f)) < 0.5)
		return false;
	qn = qnormal(p.nrm);
	return true;
}

static std::vector<Poly> fill(Part &part, const std::vector<Poly> &polys, int scale, const Vec3 &centre)
{
	std::vector<Poly> dropped;
	for (auto &p : polys)
	{
		std::vector<IV3> qp;
		IV3 qn;
		if (!quantize_face(p, scale, centre, qp, qn))
		{
			dropped.push_back(p);
			continue;
		}
		std::vector<int> vis;
		for (auto &q : qp)
			vis.push_back(part.add_vertex(q));
		int ni = (p.colour & TWO_SIDED) ? 0 : part.add_normal(qn, vis[0]);
		part.faces.push_back({vis, ni, p.colour & 0xFFF});
	}
	return dropped;
}

static Vec3 centroid(const Poly &p)
{
	Vec3 c;
	for (auto &q : p.pts)
		c = c + q;
	double n = (double)p.pts.size();
	return Vec3(c.x / n, c.y / n, c.z / n); /* divide, like the Python original */
}

static void halves(std::vector<Poly> group, std::vector<Poly> &a, std::vector<Poly> &b)
{
	Vec3 lo(1e30, 1e30, 1e30), hi(-1e30, -1e30, -1e30);
	for (auto &g : group)
	{
		Vec3 c = centroid(g);
		for (int i = 0; i < 3; i++)
		{
			lo[i] = std::min(lo[i], c[i]);
			hi[i] = std::max(hi[i], c[i]);
		}
	}
	int ax = 0;
	for (int i = 1; i < 3; i++)
		if (hi[i] - lo[i] > hi[ax] - lo[ax])
			ax = i;
	std::stable_sort(group.begin(), group.end(), [ax](const Poly &x, const Poly &y) { return centroid(x)[ax] < centroid(y)[ax]; });
	size_t h = group.size() / 2;
	a.assign(group.begin(), group.begin() + h);
	b.assign(group.begin() + h, group.end());
}

struct PartOut
{
	std::vector<Poly> group;
	Part part;
	int scale;
	Vec3 centre;
};

static void split(const std::vector<Poly> &polys, int depth, std::vector<PartOut> &out, std::vector<Poly> &lost)
{
	std::vector<std::pair<std::vector<Poly>, int>> todo = {{polys, depth}};
	while (!todo.empty())
	{
		auto [group, level] = todo.back();
		todo.pop_back();
		double ylo = 1e30, yhi = -1e30, zlo = 1e30, zhi = -1e30;
		for (auto &p : group)
			for (auto &pt : p.pts)
			{
				ylo = std::min(ylo, pt.y);
				yhi = std::max(yhi, pt.y);
				zlo = std::min(zlo, pt.z);
				zhi = std::max(zhi, pt.z);
			}
		/* x stays 0 so mirrored vertices still mirror correctly */
		Vec3 centre(0, (ylo + yhi) / 2, (zlo + zhi) / 2);
		double ext = 0;
		for (auto &p : group)
			for (auto &pt : p.pts)
				for (int i = 0; i < 3; i++)
					ext = std::max(ext, std::fabs(pt[i] - centre[i]));
		int ps = fit_scale(ext);
		Part part;
		std::vector<Poly> small;
		try
		{
			small = fill(part, group, ps, centre);
		}
		catch (BuildError &)
		{
			if (group.size() < 2)
				throw BuildError("one face needs more vertices than a model can hold");
			std::vector<Poly> a, b;
			halves(group, a, b);
			todo.push_back({b, level});
			todo.push_back({a, level});
			continue;
		}
		bool any = !part.faces.empty();
		if (any)
			out.push_back({group, part, ps, centre});
		if (!small.empty())
		{
			if (level >= 3 || (small.size() == 1 && !any))
				lost.insert(lost.end(), small.begin(), small.end());
			else if (!any)
			{
				std::vector<Poly> a, b;
				halves(small, a, b);
				todo.push_back({b, level + 1});
				todo.push_back({a, level + 1});
			}
			else
				todo.push_back({small, level + 1});
		}
	}
	std::stable_sort(out.begin(), out.end(), [](const PartOut &x, const PartOut &y) {
		Vec3 a = centroid(x.group[0]), b = centroid(y.group[0]);
		return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
	});
}

/* -- template extras (engine plumes, lights and the code around them) ----------- */

static const std::set<int> KEEP_OPS = {0x00, 0x01, 0x09, 0x0B, 0x0C, 0x0D, 0x0F, 0x13, 0x14, 0x17, 0x1D};
static const std::set<int> DRAW_EXTRAS = {0x01, 0x09, 0x0F};
static const std::set<int> JUMPS = {0x0B, 0x0C, 0x13, 0x14, 0x17};

static std::vector<Instr> prune(std::vector<Instr> kept)
{
	bool changed = true;
	while (changed)
	{
		changed = false;
		for (size_t n = 0; n < kept.size(); n++)
		{
			const Instr &ins = kept[n];
			if (!JUMPS.count(ins.op) || ins.jump < 0)
				continue;
			std::vector<size_t> region;
			for (size_t j = n + 1; j < kept.size() && kept[j].off < ins.jump; j++)
				region.push_back(j);
			bool empty = true;
			for (size_t j : region)
				if (!JUMPS.count(kept[j].op) && kept[j].op != 0x0D && kept[j].op != 0x1D)
					empty = false;
			if (empty)
			{
				kept.erase(kept.begin() + n, kept.begin() + n + 1 + region.size());
				changed = true;
				break;
			}
		}
	}
	int last = -1;
	for (size_t n = 0; n < kept.size(); n++)
		if (DRAW_EXTRAS.count(kept[n].op))
			last = (int)n;
	kept.resize(last + 1);
	return kept;
}

static std::vector<int> vertex_deps(const Vertex &v)
{
	std::vector<int> out;
	int t = v.type;
	auto add = [&](int x) {
		if (x >= 0)
			out.push_back(x);
	};
	if (t == 3 || t == 4 || t == 0xB || t == 0xC || t == 0xD || t == 0xE || t == 0x11 || t == 0x12 || t == 0x13 ||
		t == 0x14)
	{
		add(v.b);
		add(v.c);
	}
	else if (t >= 5 && t <= 8)
		add(v.c);
	else if (t == 0xF || t == 0x10)
	{
		add(v.a);
		add(v.b);
		add(v.c);
	}
	return out;
}

struct Extras
{
	std::vector<Instr> kept;
	std::vector<int> entries;
	std::vector<int> nrefs;
};

static std::optional<Extras> template_extras(const Model &t, const Image &image)
{
	bool ok = true;
	auto instrs = decode(image.data, t.code_addr(), 0x8000, &ok);
	if (!ok)
		return std::nullopt;
	std::vector<Instr> kept;
	for (auto &i : instrs)
		if (KEEP_OPS.count(i.op))
			kept.push_back(i);
	kept = prune(kept);
	bool draws = false;
	for (auto &i : kept)
		if (DRAW_EXTRAS.count(i.op))
			draws = true;
	if (!draws)
		return std::nullopt;
	std::set<int> vrefs, nrefs;
	for (auto &i : kept)
	{
		const auto &w = i.w;
		if (i.op == 0x01)
			vrefs.insert(s8(w[2] >> 8));
		else if (i.op == 0x09)
		{
			vrefs.insert(s8(w[1]));
			vrefs.insert(s8(w[1] >> 8));
		}
		else if (i.op == 0x17)
		{
			vrefs.insert(s8(w[2]));
			vrefs.insert(s8(w[2] >> 8));
		}
		else if ((i.op == 0x0B || i.op == 0x0C) && (w[1] & 0x8000) && (w[1] & 0x7F))
			nrefs.insert(w[1] & 0x7F);
	}
	for (int n : nrefs)
	{
		int k = (n >> 1) - 1;
		if (k >= 0 && k < (int)t.norms.size())
			vrefs.insert(t.norms[k].v);
	}
	std::set<int> entries;
	std::vector<int> todo;
	for (int v : vrefs)
		if (v >= 0)
			todo.push_back(v);
	while (!todo.empty())
	{
		int v = todo.back();
		todo.pop_back();
		int e = v >> 1;
		if (entries.count(e) || e >= (int)t.verts.size())
			continue;
		entries.insert(e);
		for (int dep : vertex_deps(t.verts[e]))
		{
			todo.push_back(dep);
			todo.push_back(dep ^ 1);
		}
	}
	return Extras{kept, std::vector<int>(entries.begin(), entries.end()), std::vector<int>(nrefs.begin(), nrefs.end())};
}

static std::vector<uint16_t> relocated_extras(const std::vector<Instr> &kept, const std::map<int, int> &vmap,
											  const std::map<int, int> &nmap)
{
	auto rv = [&](int i) {
		if (i < 0)
			return i & 0xFF;
		auto it = vmap.find(i);
		return (it != vmap.end() ? it->second : i) & 0xFF;
	};
	std::vector<Instr> nw;
	for (auto ins : kept)
	{
		auto &w = ins.w;
		if (ins.op == 0x01)
			w[2] = (uint16_t)((rv(s8(w[2] >> 8)) << 8) | (w[2] & 0xFF));
		else if (ins.op == 0x09)
			w[1] = (uint16_t)((rv(s8(w[1] >> 8)) << 8) | rv(s8(w[1])));
		else if (ins.op == 0x17)
			w[2] = (uint16_t)((rv(s8(w[2] >> 8)) << 8) | rv(s8(w[2])));
		else if ((ins.op == 0x0B || ins.op == 0x0C) && (w[1] & 0x8000) && (w[1] & 0x7F))
		{
			auto it = nmap.find(w[1] & 0x7F);
			w[1] = it == nmap.end() ? 0x8000 : (uint16_t)((w[1] & 0xFF80) | it->second);
		}
		nw.push_back(ins);
	}
	std::vector<int> offs;
	int pos = 0;
	for (auto &c : nw)
	{
		offs.push_back(pos);
		pos += (int)c.w.size() * 2;
	}
	int end = pos;
	auto new_target = [&](int old) {
		for (size_t k = 0; k < nw.size(); k++)
			if (nw[k].off >= old)
				return offs[k];
		return end;
	};
	for (size_t k = 0; k < nw.size(); k++)
	{
		auto &c = nw[k];
		if (c.jump >= 0 && JUMPS.count(c.op))
		{
			int base = c.op == 0x17 ? 6 : 4;
			int skip = new_target(c.jump) - offs[k] - base;
			if (skip < 0 || skip > 0xFFE || (skip & 1))
				throw BuildError("jump out of range while copying the base ship's code");
			c.w[0] = (uint16_t)((c.w[0] & 0x1F) | (skip << 4));
		}
	}
	std::vector<uint16_t> out;
	for (auto &c : nw)
		out.insert(out.end(), c.w.begin(), c.w.end());
	return out;
}

static std::vector<uint8_t> template_tail(const Model &t, const Image &image)
{
	if (!t.ship_off)
		return {};
	uint32_t start = t.addr + t.ship_off;
	uint32_t end = image.gamedata2;
	for (int n : image.model_numbers())
	{
		auto a = image.model_addr(n);
		if (a && *a > t.addr && *a < end)
			end = *a;
	}
	uint32_t size = end - start;
	if (size < 32 || size > 64)
		size = 40;
	return std::vector<uint8_t>(image.data.begin() + start, image.data.begin() + start + size);
}

/* -- collision ------------------------------------------------------------------ */

struct Plane
{
	Vec3 n, p;
};

static std::vector<Vec3> kdop_dirs(const std::string &kind)
{
	std::vector<Vec3> d = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	if (kind == "kdop14" || kind == "kdop26")
		for (int x : {1, -1})
			for (int y : {1, -1})
				for (int z : {1, -1})
					d.push_back(Vec3(x, y, z));
	if (kind == "kdop26")
		for (auto ab : std::vector<std::pair<int, int>>{{0, 1}, {0, 2}, {1, 2}})
			for (int sa : {1, -1})
				for (int sb : {1, -1})
				{
					Vec3 v;
					v[ab.first] = sa;
					v[ab.second] = sb;
					d.push_back(v);
				}
	return d;
}

static std::vector<std::vector<Plane>> collision_planes(const std::vector<Poly> &polys,
														 const std::map<std::string, std::vector<Poly>> &coll,
														 std::string mode, std::string &desc)
{
	if (mode == "auto")
		mode = coll.empty() ? "kdop14" : "custom";
	std::vector<std::vector<Plane>> groups;
	if (mode == "custom")
	{
		if (coll.empty())
			throw BuildError("collision 'custom': there are no collision hulls (objects named collision...)");
		for (auto &kv : coll)
		{
			Vec3 c;
			int n = 0;
			for (auto &p : kv.second)
				for (auto &q : p.pts)
				{
					c = c + q;
					n++;
				}
			c = Vec3(c.x / n, c.y / n, c.z / n);
			std::vector<Plane> planes;
			for (auto &p : kv.second)
			{
				Vec3 nrm = p.nrm;
				/* hulls are convex: planes face away from the middle */
				if (dot(nrm, p.pts[0] - c) < 0)
					nrm = -nrm;
				planes.push_back({nrm, p.pts[0]});
			}
			groups.push_back(planes);
		}
		desc = "custom: " + std::to_string(groups.size()) + " hull(s)";
		return groups;
	}
	std::vector<Plane> planes;
	for (auto &d : kdop_dirs(mode))
	{
		const Vec3 *best = nullptr;
		double bd = -1e300;
		for (auto &p : polys)
			for (auto &q : p.pts)
				if (dot(q, d) > bd)
				{
					bd = dot(q, d);
					best = &q;
				}
		if (best)
			planes.push_back({d, *best});
	}
	groups.push_back(planes);
	desc = mode + " around the mesh";
	return groups;
}

static std::vector<std::vector<int>> collision_into_root(Part &root, const std::vector<std::vector<Plane>> &groups,
														 int scale, std::vector<std::string> &warnings)
{
	double k = std::ldexp(1.0, scale);
	std::vector<std::vector<int>> out;
	for (auto &planes : groups)
	{
		std::vector<int> idxs;
		for (auto &pl : planes)
		{
			IV3 q;
			for (int i = 0; i < 3; i++)
				q[i] = std::max(-127, std::min(127, (int)rne(pl.p[i] / k)));
			try
			{
				int vi = root.add_vertex(q);
				int ni = root.add_normal(qnormal(pl.n), vi);
				if (std::find(idxs.begin(), idxs.end(), ni) == idxs.end())
					idxs.push_back(ni);
			}
			catch (BuildError &)
			{
				warnings.push_back("collision: ran out of room for planes, some left out");
				break;
			}
		}
		if (!idxs.empty())
			out.push_back(idxs);
	}
	return out;
}

/* -- packing ------------------------------------------------------------------------ */

static std::vector<uint16_t> face_code(const std::vector<Part::F> &faces)
{
	std::vector<uint16_t> code;
	for (auto &f : faces)
	{
		int c = (f.col & 0xFFE) << 4;
		std::vector<int> v;
		for (int x : f.v)
			v.push_back(x & 0xFF);
		if (v.size() == 3)
		{
			code.push_back((uint16_t)(c | 0x03));
			code.push_back((uint16_t)((v[0] << 8) | v[1]));
			code.push_back((uint16_t)((v[2] << 8) | (f.ni & 0x7F)));
		}
		else
		{
			/* the game goes round hi(w1), lo(w2), lo(w1), hi(w2) */
			code.push_back((uint16_t)(c | 0x04));
			code.push_back((uint16_t)((v[0] << 8) | v[2]));
			code.push_back((uint16_t)((v[3] << 8) | v[1]));
			code.push_back((uint16_t)(f.ni & 0x7F));
		}
	}
	return code;
}

struct Hdr
{
	int scale2, x10, x14, x16, x18;
};

static void put16(std::vector<uint8_t> &b, int v)
{
	b.push_back((uint8_t)(v >> 8));
	b.push_back((uint8_t)v);
}

static std::vector<uint8_t> pack(const Part &part, const std::vector<uint16_t> &code, int scale, const Hdr &hdr,
								 int colour, std::vector<uint8_t> coll, const std::vector<uint8_t> &tail, int radius,
								 bool is_part)
{
	std::vector<uint8_t> verts, norms, code_b;
	for (auto &v : part.verts)
		for (int i = 0; i < 4; i++)
			verts.push_back((uint8_t)(v[i] & 0xFF));
	for (auto &n : part.norms)
		for (int i = 0; i < 4; i++)
			norms.push_back((uint8_t)(n[i] & 0xFF));
	for (auto w : code)
		put16(code_b, w);
	int vert_off = HEADER_SIZE;
	int norm_off = vert_off + (int)verts.size();
	int code_off = norm_off + (int)norms.size();
	int coll_off = code_off + (int)code_b.size();
	int ship_off = coll_off + (int)coll.size();
	if (ship_off & 1)
	{
		coll.push_back(0);
		ship_off++;
	}
	int ship_field = is_part ? 0 : (tail.empty() ? 0 : ship_off);
	std::vector<uint8_t> b;
	int h[15] = {code_off,
				 vert_off,
				 (int)part.verts.size() * 64,
				 norm_off,
				 ((int)part.norms.size() + 1) * 4,
				 scale,
				 (is_part ? 0 : hdr.scale2) & 0xFFFF,
				 std::max(1, std::min(radius, 0xFFFF)),
				 hdr.x10,
				 colour & 0xFFF,
				 hdr.x14,
				 hdr.x16,
				 hdr.x18,
				 coll_off,
				 ship_field};
	for (int v : h)
		put16(b, v);
	b.insert(b.end(), verts.begin(), verts.end());
	b.insert(b.end(), norms.begin(), norms.end());
	b.insert(b.end(), code_b.begin(), code_b.end());
	b.insert(b.end(), coll.begin(), coll.end());
	b.insert(b.end(), tail.begin(), tail.end());
	if (b.size() & 1)
		b.push_back(0);
	if (b.size() > 0x7FFF)
		throw BuildError("model too big (" + std::to_string(b.size()) + " bytes)");
	return b;
}

/* -- the build ----------------------------------------------------------------------- */

BuildResult build_model(const BuildMesh &mesh, const Model *tmpl, const Image &image, const BuildOptions &opt)
{
	BuildResult res;
	auto &warnings = res.warnings;
	std::vector<Poly> polys;
	std::map<std::string, std::vector<Poly>> coll;
	int flipped = 0, inside_out = 0;
	split_polygons(mesh, opt.fix_normals, polys, coll, flipped, inside_out);
	if (polys.empty())
		throw BuildError("there are no faces to build");
	if (flipped)
		warnings.push_back(std::to_string(flipped) + " faces pointed inwards and were flipped");
	else if (inside_out)
		warnings.push_back(std::to_string(inside_out) +
						   " closed part(s) look inside out (normals pointing in): the game will hide the wrong "
						   "faces. Use Recalc outside on the Shape step.");
	auto planes = collision_planes(polys, coll, opt.collision, res.collision);

	std::optional<Extras> extras;
	if (tmpl && opt.keep_extras)
		extras = template_extras(*tmpl, image);

	double mx = 0;
	for (auto &p : polys)
		for (auto &pt : p.pts)
			for (int i = 0; i < 3; i++)
				mx = std::max(mx, std::fabs(pt[i]));
	if (extras)
		for (int e : extras->entries)
		{
			auto &v = tmpl->verts[e];
			if (v.type <= 2 || v.type == 9 || v.type == 10)
				mx = std::max(mx, std::max({std::abs(v.a), std::abs(v.b), std::abs(v.c)}) * std::ldexp(1.0, tmpl->scale));
		}
	int scale = opt.scale ? *opt.scale : fit_scale(mx);
	double k = std::ldexp(1.0, scale);

	std::map<int, int> vmap, nmap;
	auto make_root = [&](Part &root) {
		vmap.clear();
		nmap.clear();
		if (!extras)
			return;
		int shift = tmpl->scale - scale;
		int base = (int)root.verts.size();
		std::map<int, int> emap;
		for (size_t i = 0; i < extras->entries.size(); i++)
			emap[extras->entries[i]] = base + (int)i;
		auto rv = [&](int i) {
			if (i >= 0 && emap.count(i >> 1))
				return (emap[i >> 1] * 2) | (i & 1);
			return i;
		};
		for (int e : extras->entries)
		{
			auto &v = tmpl->verts[e];
			std::array<int, 4> ent;
			if (v.type <= 2 || v.type == 9 || v.type == 10)
			{
				auto sc = [&](int x) { return std::max(-127, std::min(127, (int)rne(x * std::ldexp(1.0, shift)))); };
				ent = {v.type, sc(v.a), sc(v.b), sc(v.c)};
			}
			else if (v.type == 0xF || v.type == 0x10)
				ent = {v.type, rv(v.a), rv(v.b), rv(v.c)};
			else if (v.type >= 5 && v.type <= 8)
				ent = {v.type, v.a, v.b, rv(v.c)};
			else
				ent = {v.type, v.a, rv(v.b), rv(v.c)};
			if ((int)root.verts.size() >= MAX_VERTS)
				throw BuildError("the base ship's engine glow needs too many vertices");
			root.verts.push_back(ent);
			vmap[e * 2] = emap[e] * 2;
			vmap[e * 2 + 1] = emap[e] * 2 + 1;
		}
		for (int n : extras->nrefs)
		{
			auto &t = tmpl->norms[(n >> 1) - 1];
			int vi = vmap.count(t.v) ? vmap[t.v] : 0;
			int x = (n & 1) ? -t.x : t.x;
			root.norms.push_back({(n & 1) ? (vi ^ 1) : vi, x, t.y, t.z});
			nmap[n] = (int)root.norms.size() * 2;
		}
	};

	Part root;
	make_root(root);
	auto coll_groups = collision_into_root(root, planes, scale, warnings);
	std::vector<PartOut> part_groups;
	std::vector<Poly> lost;
	try
	{
		auto small = fill(root, polys, scale, Vec3());
		if (!small.empty())
			split(small, 1, part_groups, lost);
	}
	catch (BuildError &)
	{
		root = Part();
		make_root(root);
		std::vector<std::string> ignore;
		coll_groups = collision_into_root(root, planes, scale, ignore);
		part_groups.clear();
		lost.clear();
		split(polys, 0, part_groups, lost);
	}
	struct PartUse
	{
		Part *part;
		int scale, origin;
	};
	std::vector<PartUse> parts;
	for (auto &pg : part_groups)
	{
		try
		{
			IV3 o = {(int)rne(pg.centre.x / k), (int)rne(pg.centre.y / k),
					 (int)rne(pg.centre.z / k)};
			int origin = root.add_vertex(o);
			parts.push_back({&pg.part, pg.scale, origin});
		}
		catch (BuildError &)
		{
			lost.insert(lost.end(), pg.group.begin(), pg.group.end());
		}
	}
	if (!lost.empty())
		warnings.push_back(std::to_string(lost.size()) + " faces smaller than the model's precision were dropped");
	if (root.faces.empty() && parts.empty())
		throw BuildError("every face is smaller than the model's precision; scale the mesh up");

	/* parts from the top down, so they stay out of the way of new ship numbers (240 up) */
	int s = EXTRA_SLOT_LAST;
	for (size_t i = 0; i < parts.size(); i++)
	{
		while (s >= EXTRA_SLOT_FIRST && (opt.used_slots.count(s) || s == opt.index))
			s--;
		if (s < EXTRA_SLOT_FIRST)
			throw BuildError("out of model numbers for parts (MDLMOD_MAX in fe2_modded.s)");
		res.slots.push_back(s--);
	}

	int base_colour = opt.colour ? *opt.colour : (tmpl ? tmpl->colour : 0x666);
	std::vector<uint16_t> root_code;
	for (size_t i = 0; i < parts.size(); i++)
	{
		root_code.push_back((uint16_t)((res.slots[i] << 5) | 0x0E));
		root_code.push_back((uint16_t)(parts[i].origin & 0xFF));
	}
	auto fc = face_code(root.faces);
	root_code.insert(root_code.end(), fc.begin(), fc.end());
	if (extras)
	{
		auto ex = relocated_extras(extras->kept, vmap, nmap);
		root_code.insert(root_code.end(), ex.begin(), ex.end());
	}
	root_code.push_back(0);

	std::vector<uint8_t> tail = tmpl ? template_tail(*tmpl, image) : std::vector<uint8_t>();
	if (!opt.ship_values.empty() && tail.size() >= 32)
		write_fields(tail, opt.ship_values);
	std::vector<uint8_t> collb;
	for (auto &g : coll_groups)
	{
		collb.push_back(0x10);
		collb.push_back(0x80);
		for (int n : g)
			collb.push_back((uint8_t)n);
		collb.push_back(0);
	}
	collb.push_back(0);
	Hdr hdr = tmpl ? Hdr{tmpl->scale2, tmpl->x10, tmpl->x14, tmpl->x16, tmpl->x18} : Hdr{0, 0x14, 0x0B, 0x2548, 0x09AB};
	double rmax = 0;
	for (auto &p : polys)
		for (auto &pt : p.pts)
			rmax = std::max(rmax, length(pt));
	int radius = (int)std::ceil(rmax / k) + 1;
	res.models.push_back({opt.index, pack(root, root_code, scale, hdr, base_colour, collb, tail, radius, false)});
	for (size_t i = 0; i < parts.size(); i++)
	{
		auto code = face_code(parts[i].part->faces);
		code.push_back(0);
		res.models.push_back({res.slots[i], pack(*parts[i].part, code, parts[i].scale, hdr, base_colour, {0, 0}, {},
												 parts[i].part->radius(), true)});
	}
	res.faces = (int)root.faces.size();
	res.verts = (int)root.verts.size();
	res.norms = (int)root.norms.size();
	for (auto &p : parts)
	{
		res.faces += (int)p.part->faces.size();
		res.verts += (int)p.part->verts.size();
		res.norms += (int)p.part->norms.size();
	}
	res.scale = scale;
	res.parts = 1 + (int)parts.size();
	res.extras = extras.has_value();
	for (auto &m : res.models)
		res.bytes += (int)m.second.size();
	return res;
}

/* -- .fe2m ----------------------------------------------------------------------------- */

enum
{
	REC_NAME = 1,
	REC_LIKE = 2,
	REC_PICKS = 3
};

std::vector<uint8_t> fe2m_bytes(const ModelBlobs &models, const Fe2mMeta *meta)
{
	std::vector<std::pair<int, std::vector<uint8_t>>> records;
	if (meta)
	{
		if (!meta->name.empty())
			records.push_back({REC_NAME, std::vector<uint8_t>(meta->name.begin(), meta->name.begin() +
																	std::min<size_t>(40, meta->name.size()))});
		if (meta->like >= 0)
			records.push_back({REC_LIKE, {(uint8_t)(meta->like >> 8), (uint8_t)meta->like}});
		if (!meta->picks.empty())
		{
			std::vector<uint8_t> p;
			for (auto &cw : meta->picks)
			{
				p.push_back((uint8_t)cw.first);
				p.push_back((uint8_t)cw.second);
			}
			records.push_back({REC_PICKS, p});
		}
	}
	std::vector<uint8_t> out = {'F', 'E', '2', 'M'};
	put16(out, records.empty() ? 1 : 2);
	put16(out, (int)models.size());
	for (auto &m : models)
	{
		put16(out, m.first);
		put16(out, MODEL_FLAG_QUADS_OK);
		uint32_t n = (uint32_t)m.second.size();
		out.push_back((uint8_t)(n >> 24));
		out.push_back((uint8_t)(n >> 16));
		out.push_back((uint8_t)(n >> 8));
		out.push_back((uint8_t)n);
		out.insert(out.end(), m.second.begin(), m.second.end());
		if (n & 1)
			out.push_back(0);
	}
	if (!records.empty())
	{
		put16(out, (int)records.size());
		for (auto &r : records)
		{
			put16(out, r.first);
			put16(out, (int)r.second.size());
			out.insert(out.end(), r.second.begin(), r.second.end());
			if (r.second.size() & 1)
				out.push_back(0);
		}
	}
	return out;
}

void write_fe2m(const std::string &path, const ModelBlobs &models, const Fe2mMeta *meta)
{
	auto b = fe2m_bytes(models, meta);
	std::ofstream f(path, std::ios::binary);
	if (!f)
		throw BuildError("can't write " + path);
	f.write((const char *)b.data(), (std::streamsize)b.size());
}

bool parse_fe2m(const std::vector<uint8_t> &d, ModelBlobs &models, Fe2mMeta &meta)
{
	models.clear();
	meta = Fe2mMeta();
	if (d.size() < 8 || memcmp(d.data(), "FE2M", 4) != 0)
		return false;
	auto r16 = [&](size_t p) { return p + 1 < d.size() ? (d[p] << 8) | d[p + 1] : 0; };
	int ver = r16(4), count = r16(6);
	size_t pos = 8;
	for (int i = 0; i < count; i++)
	{
		if (pos + 8 > d.size())
			return false;
		int idx = r16(pos);
		uint32_t size = ((uint32_t)d[pos + 4] << 24) | (d[pos + 5] << 16) | (d[pos + 6] << 8) | d[pos + 7];
		pos += 8;
		if (pos + size > d.size())
			return false;
		models.push_back({idx, std::vector<uint8_t>(d.begin() + pos, d.begin() + pos + size)});
		pos += size + (size & 1);
	}
	if (ver >= 2 && pos + 2 <= d.size())
	{
		int n = r16(pos);
		pos += 2;
		for (int i = 0; i < n && pos + 4 <= d.size(); i++)
		{
			int t = r16(pos), size = r16(pos + 2);
			size_t at = pos + 4;
			pos += 4 + size + (size & 1);
			if (at + size > d.size())
				break;
			if (t == REC_NAME)
				meta.name.assign(d.begin() + at, d.begin() + at + size);
			else if (t == REC_LIKE && size >= 2)
				meta.like = r16(at);
			else if (t == REC_PICKS)
				for (int k = 0; k + 1 < size; k += 2)
					meta.picks.push_back({d[at + k], d[at + k + 1]});
		}
	}
	return true;
}

bool read_fe2m(const std::string &path, ModelBlobs &models, Fe2mMeta &meta)
{
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return false;
	std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	return parse_fe2m(d, models, meta);
}

} // namespace fe2
