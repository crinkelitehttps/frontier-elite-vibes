/*
 * fe2core.cpp - see fe2core.h. The bytecode format is from watsonmw's
 * "Frontier: Elite 2 engine study" (https://watsonmw.com/fintro) and
 * fe2-intro, checked against fe2/fe2.s.
 */
#include "fe2core.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

/* The game (src/gl/gl_native.cpp) builds the image from its own memory */
#ifndef FE2CORE_EXTERNAL_IMAGE
#include "fe2_bin.h"
#include "fe2_labels.h"
#endif

namespace fe2
{

double dot(const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(const Vec3 &a, const Vec3 &b)
{
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double length(const Vec3 &a) { return std::sqrt(dot(a, a)); }
Vec3 normalize(const Vec3 &a)
{
	double l = length(a);
	return l > 1e-12 ? a * (1.0 / l) : Vec3();
}
Vec3 newell(const std::vector<Vec3> &pts)
{
	Vec3 n;
	for (size_t i = 0; i < pts.size(); i++)
	{
		const Vec3 &a = pts[i], &b = pts[(i + 1) % pts.size()];
		n.x += (a.y - b.y) * (a.z + b.z);
		n.y += (a.z - b.z) * (a.x + b.x);
		n.z += (a.x - b.x) * (a.y + b.y);
	}
	return n;
}

/* ======================================================================== */
/* Image                                                                    */
/* ======================================================================== */

#ifndef FE2CORE_EXTERNAL_IMAGE
const Image &Image::game()
{
	static Image img;
	static bool ready = false;
	if (!ready)
	{
		img.data.assign(fe2_modded_s_bin, fe2_modded_s_bin + fe2_modded_s_bin_len);
		img.table = FE2_gamedata1;
		img.gamedata2 = FE2_gamedata2;
		img.ship_picks = FE2_ship_picks;
		img.core_strings = FE2_L3e894;
		/* the table runs until the first model's data starts */
		int first = 0x10000;
		for (int i = 0; i * 2 < first; i++)
		{
			int o = img.u16(img.table + i * 2);
			if (o)
				first = std::min(first, o);
		}
		img.num_models = first / 2;
		ready = true;
	}
	return img;
}
#endif

Image Image::with_models(const std::vector<std::pair<int, std::vector<uint8_t>>> &models) const
{
	Image out = *this;
	for (auto &m : models)
	{
		uint32_t addr = (uint32_t)out.data.size();
		out.data.insert(out.data.end(), m.second.begin(), m.second.end());
		out.data.resize(out.data.size() + 16, 0);
		if (out.data.size() & 1)
			out.data.push_back(0);
		out.overrides[m.first] = addr;
		out.num_models = std::max(out.num_models, m.first + 1);
	}
	return out;
}

std::optional<uint32_t> Image::model_addr(int index) const
{
	auto it = overrides.find(index);
	if (it != overrides.end())
		return it->second;
	if (index < 0 || index >= num_models || index >= Image::game().num_models)
		return std::nullopt;
	int off = u16(table + index * 2);
	if (!off)
		return std::nullopt;
	return table + off;
}

std::vector<int> Image::model_numbers() const
{
	std::vector<int> out;
	for (int i = 0; i < num_models; i++)
		if (model_addr(i))
			out.push_back(i);
	return out;
}

std::string Image::core_string(int index) const
{
	if (index < 0 || index >= 112)
		return "";
	uint32_t a = core_strings + u16(core_strings + index * 2);
	std::string s;
	while (a < data.size() && data[a] > 0 && data[a] < 0x80 && s.size() < 64)
		s += (char)data[a++];
	return s;
}

std::optional<Model> Image::model(int index) const
{
	auto a = model_addr(index);
	if (!a)
		return std::nullopt;
	Model m;
	m.image = this;
	m.index = index;
	m.addr = *a;
	for (int i = 0; i < 15; i++)
		m.header[i] = u16(m.addr + i * 2);
	int *h = m.header;
	m.code_off = h[0];
	m.vert_off = h[1];
	m.vert_buf = h[2];
	m.norm_off = h[3];
	m.norm_buf = h[4];
	m.scale = h[5];
	m.scale2 = h[6];
	m.radius = h[7];
	m.x10 = h[8];
	m.colour = h[9];
	m.x14 = h[10];
	m.x16 = h[11];
	m.x18 = h[12];
	m.coll_off = h[13];
	m.ship_off = h[14];
	m.num_verts = m.vert_buf / 64;
	m.num_norms = std::max(0, m.norm_buf / 4 - 1);
	for (int i = 0; i < m.num_verts; i++)
	{
		uint32_t a = m.addr + m.vert_off + i * 4;
		m.verts.push_back({u8(a), s8(u8(a + 1)), s8(u8(a + 2)), s8(u8(a + 3))});
	}
	for (int i = 0; i < m.num_norms; i++)
	{
		uint32_t a = m.addr + s16(m.norm_off) + i * 4;
		m.norms.push_back({s8(u8(a)), s8(u8(a + 1)), s8(u8(a + 2)), s8(u8(a + 3))});
	}
	return m;
}

/* ======================================================================== */
/* Model                                                                    */
/* ======================================================================== */

std::optional<Vec3> Model::vertex_pos(int index, const ParentFn &parent, int depth) const
{
	if (depth > 32)
		return Vec3();
	if (index < 0)
		return parent ? parent(index) : std::nullopt;
	int vi = index >> 1, odd = index & 1;
	if (vi >= (int)verts.size())
		return std::nullopt;
	const Vertex &v = verts[vi];
	int t = v.type;
	auto rec = [&](int i) { return vertex_pos(i, parent, depth + 1); };
	if (t <= 2 || t == 0x15 || t == 0x16 || t > 0x16)
		return Vec3(odd ? -v.a : v.a, v.b, v.c);
	if (t == 9 || t == 0xA)
		return Vec3(odd ? 0 : v.a, v.b, v.c);
	int b = v.b, c = v.c;
	if (odd)
	{
		b ^= 1;
		c ^= 1;
	}
	if (t == 5 || t == 6)
	{
		auto p = rec(c);
		return p ? std::optional<Vec3>(-*p) : std::nullopt;
	}
	if (t == 7 || t == 8)
		return rec(c);
	if (t == 3 || t == 4 || t == 0xB || t == 0xC || t == 0xD || t == 0xE || t == 0x13 || t == 0x14)
	{
		auto p = rec(b), q = rec(c);
		if (!p || !q)
			return p ? p : q;
		return (*p + *q) * 0.5;
	}
	if (t == 0xF || t == 0x10)
	{
		auto p = rec(b), q = rec(c), r = rec(v.a);
		if (!p || !q || !r)
			return p ? p : q;
		return *p + *q - *r;
	}
	if (t == 0x11 || t == 0x12)
	{
		auto p = rec(b), q = rec(c);
		if (!p || !q)
			return p ? p : q;
		return *p + *q;
	}
	return Vec3(v.a, v.b, v.c);
}

std::optional<Vec3> Model::normal_vec(int index) const
{
	if (index <= 1)
		return std::nullopt;
	int ni = (index >> 1) - 1;
	if (ni >= (int)norms.size())
		return std::nullopt;
	const Normal &n = norms[ni];
	return Vec3((index & 1) ? -n.x : n.x, n.y, n.z);
}

std::vector<uint8_t> Model::ship_data(int n) const
{
	std::vector<uint8_t> out;
	if (!ship_off)
		return out;
	uint32_t a = addr + ship_off;
	for (int i = 0; i < n && a + i < image->data.size(); i++)
		out.push_back(image->data[a + i]);
	return out;
}

std::vector<uint8_t> Model::collision_bytes() const
{
	std::vector<uint8_t> out;
	if (!coll_off)
		return out;
	uint32_t a = addr + s16(coll_off);
	const auto &d = image->data;
	while (a < d.size())
	{
		int ctrl = d[a++];
		out.push_back(ctrl);
		if (!ctrl)
			break;
		out.push_back(d[a++]);
		if (ctrl & 0x20)
		{
			out.push_back(d[a]);
			out.push_back(d[a + 1]);
			a += 2;
		}
		if ((ctrl & 0x80) && !(ctrl & 0x40))
		{
			for (int i = 0; i < 4; i++)
				out.push_back(d[a++]);
			continue;
		}
		while (a < d.size())
		{
			int n = d[a++];
			out.push_back(n);
			if (!n)
				break;
		}
	}
	return out;
}

std::string Model::ship_name() const
{
	auto sd = ship_data(16);
	if (sd.size() < 16)
		return "";
	int sid = (sd[14] << 8) | sd[15];
	if ((sid & 0xC000) == 0x4000)
		return image->core_string(sid & 0xFFF);
	return "";
}

/* ======================================================================== */
/* Bytecode                                                                 */
/* ======================================================================== */

static const char *OPNAMES[32] = {
	"done",	  "circle",	  "line",	   "tri",	   "quad",	   "complex", "batch",	  "mtri",
	"mquad",  "teardrop", "vtext",	   "if",	   "ifnot",	   "calc",	  "model",	  "sound",
	"cylinder", "cylinder2", "btext", "ifnotvar", "ifvar",	  "ztree",	  "bline",	  "ifdist",
	"circles", "msetup",  "colour",	   "models",   "mrotate",  "calc",	  "mcopy",	  "planet",
};
const char *op_name(int op) { return OPNAMES[op & 31]; }

std::vector<Instr> decode(const std::vector<uint8_t> &data, uint32_t start, uint32_t limit, bool *ok)
{
	std::vector<Instr> out;
	uint32_t pos = start, furthest = start;
	uint32_t end = std::min<uint32_t>((uint32_t)data.size(), start + limit);
	bool bad = false;
	auto rd = [&](uint32_t p) -> int {
		if (p + 2 > end)
		{
			bad = true;
			return 0;
		}
		return (data[p] << 8) | data[p + 1];
	};
	while (pos < end && !bad)
	{
		Instr ins;
		ins.off = (int)(pos - start);
		int w0 = rd(pos);
		int op = w0 & 0x1f, p12 = w0 >> 4;
		ins.op = op;
		ins.w.push_back((uint16_t)w0);
		uint32_t p = pos + 2;
		auto take = [&](int n) {
			for (int i = 0; i < n; i++)
			{
				ins.w.push_back((uint16_t)rd(p));
				p += 2;
			}
		};
		switch (op)
		{
		case 0x00:
			break;
		case 0x01:
		case 0x03:
		case 0x07:
		case 0x09:
			take(2);
			break;
		case 0x02:
			take(1);
			break;
		case 0x04:
		case 0x08:
		case 0x0A:
		case 0x10:
		case 0x16:
			take(3);
			break;
		case 0x05:
		{
			take(1);
			int skip = (ins.w[1] >> 8) + 4;
			uint32_t q = p;
			for (int guard = 0; guard < 200 && !bad; guard++)
			{
				int s0 = rd(q);
				q += 2;
				int f = (s0 >> 9) & 7;
				ComplexSub cs;
				if (f == 0)
				{
					cs.name = "done";
					ins.sub.push_back(cs);
					break;
				}
				else if (f == 1)
				{
					int a = rd(q), b = rd(q + 2);
					q += 4;
					cs.name = "bezier";
					cs.a[0] = s8(a >> 8);
					cs.a[1] = s8(a);
					cs.a[2] = s8(b >> 8);
					cs.a[3] = s8(b);
				}
				else if (f == 2)
				{
					int a = rd(q);
					q += 2;
					cs.name = "line";
					cs.a[0] = s8(s0);
					cs.a[1] = s8(a);
				}
				else if (f == 3)
				{
					cs.name = "linec";
					cs.a[0] = s8(s0);
				}
				else if (f == 4)
				{
					int a = rd(q);
					q += 2;
					cs.name = "bezierc";
					cs.a[0] = s8(s0);
					cs.a[1] = s8(a >> 8);
					cs.a[2] = s8(a);
				}
				else if (f == 5)
					cs.name = "join";
				else if (f == 6)
				{
					int a = rd(q);
					q += 2;
					cs.name = "circle";
					cs.a[0] = s8(s0);
					cs.a[1] = a >> 8;
					cs.a[2] = a & 0x7f;
				}
				else
				{
					bad = true;
					break;
				}
				ins.sub.push_back(cs);
			}
			while (p < q)
				take(1);
			ins.jump = ins.off + skip;
			if ((int)(p - start) != ins.off + skip)
			{
				p = start + ins.off + skip;
				ins.w.resize(2);
			}
			break;
		}
		case 0x06:
		{
			int ctl = p12 >> 1;
			if (ctl == 0x7FD)
				take(1);
			else if (ctl != 0 && ctl != 0x7FF && ctl != 0x7FE)
			{
				int up = ctl >> 8;
				if (up == 1 || up == 3)
				{
					for (int guard = 0; guard < 64; guard++)
					{
						take(1);
						if (!(ins.w.back() & 0x8000))
							break;
					}
				}
				else if (up == 7)
					take(1);
			}
			break;
		}
		case 0x0B:
		case 0x0C:
		case 0x13:
		case 0x14:
		{
			take(1);
			int skip = p12 & 0xFFE;
			if (skip)
				ins.jump = ins.off + skip + 4;
			break;
		}
		case 0x0D:
		case 0x1D:
		case 0x12:
		case 0x1C:
			take(1);
			break;
		case 0x0E:
			take(1);
			if (ins.w[1] & 0x8000)
				take(2);
			break;
		case 0x0F:
		case 0x15:
		case 0x19:
		case 0x1E:
			break;
		case 0x11:
			take(5);
			break;
		case 0x17:
		{
			take(2);
			int skip = p12 & 0xFFE;
			if (skip)
				ins.jump = ins.off + skip + 6;
			break;
		}
		case 0x18:
			take(1);
			for (int guard = 0; guard < 128; guard++)
			{
				take(1);
				int v1 = s8(ins.w.back() >> 8), v2 = s8(ins.w.back());
				if (v1 == 0x7F || v2 == 0x7F)
					break;
			}
			break;
		case 0x1A:
			take(1);
			if (ins.w[1] >> 8)
				take(7);
			break;
		case 0x1B:
			take(2);
			if (ins.w[1] & 0x8000)
				take(2);
			break;
		case 0x1F:
			take(2);
			p += (p12 & 0xFFE);
			break;
		}
		ins.size = (int)(p - pos);
		if (ins.jump >= 0 && op != 0x05)
			furthest = std::max<uint32_t>(furthest, start + ins.jump);
		out.push_back(ins);
		pos = p;
		if (op == 0x00 && pos > furthest)
			break;
	}
	if (ok)
		*ok = !bad;
	return out;
}

static std::string fmt(const char *f, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, f);
	vsnprintf(buf, sizeof(buf), f, ap);
	va_end(ap);
	return buf;
}

static std::string param8(int p)
{
	int kind = p & 0xC0, v = p & 0x3F;
	if (kind == 0x00)
		return fmt("%d", v);
	if (kind == 0x40)
		return fmt("%d", v << 10);
	if (kind == 0x80)
		return fmt("var[%d]", v);
	return fmt("t%d", v);
}

static const char *CALC_OPS[16] = {"add", "sub", "mul", "div", "shr", "shl", "max", "min",
								   "mul2", "sar", "var", "zgt", "zlt", "sin", "cos", "and"};

std::string describe(const Instr &ins)
{
	const auto &w = ins.w;
	int op = ins.op, c = ins.colour();
	std::string col = fmt("#%03x", c);
	auto jt = [&]() { return ins.jump >= 0 ? fmt("%04x", ins.jump) : std::string("return"); };
	switch (op)
	{
	case 0x00:
		return "done";
	case 0x01:
		return fmt("%s v:%d r:%04x %s", (w[2] & 0x80) ? "highlight" : "circle", s8(w[2] >> 8), w[1], col.c_str());
	case 0x02:
		return fmt("line %d, %d %s", s8(w[1] >> 8), s8(w[1]), col.c_str());
	case 0x03:
	case 0x07:
		return fmt("%s %d, %d, %d %s n:%d", op_name(op), s8(w[1] >> 8), s8(w[1]), s8(w[2] >> 8), col.c_str(),
				   w[2] & 0x7F);
	case 0x04:
	case 0x08:
		/* drawn in the order hi(w1), lo(w2), lo(w1), hi(w2) */
		return fmt("%s %d, %d, %d, %d %s n:%d", op_name(op), s8(w[1] >> 8), s8(w[2]), s8(w[1]), s8(w[2] >> 8),
				   col.c_str(), w[3] & 0x7F);
	case 0x05:
	{
		std::string subs;
		for (auto &s : ins.sub)
		{
			if (!subs.empty())
				subs += "; ";
			subs += s.name;
			int n = s.name == "bezier" ? 4 : s.name == "line" ? 2 : s.name == "linec" ? 1
										 : (s.name == "bezierc" || s.name == "circle") ? 3 : 0;
			for (int i = 0; i < n; i++)
				subs += fmt(" %d", s.a[i]);
		}
		return fmt("complex %s n:%d { %s }", col.c_str(), w.size() > 1 ? (w[1] & 0x7F) : 0, subs.c_str());
	}
	case 0x06:
		return (w[0] >> 5) == 0 ? "batch end" : fmt("batch begin %03x", w[0] >> 5);
	case 0x09:
		return fmt("teardrop %d, %d r:%d %s", s8(w[1]), s8(w[1] >> 8), w[2], col.c_str());
	case 0x0A:
		return fmt("vtext v:%d font:%d scale:%d n:%d str:%04x %s", s8(w[2]), (w[1] >> 12) & 15, (w[1] >> 8) & 15,
				   w[1] & 0x7F, w[3], col.c_str());
	case 0x0B:
	case 0x0C:
	{
		std::string what = (w[1] & 0x8000) ? fmt("normal:%d", w[1] & 0x7F) : fmt("z:%d", w[1]);
		return fmt("if %s %s -> %s", op == 0x0B ? ">" : "<", what.c_str(), jt().c_str());
	}
	case 0x0D:
	case 0x1D:
	{
		int fn = ((w[0] >> 4) & 15) + (op == 0x1D ? 8 : 0);
		return fmt("t%d = %s(%s, %s)", (w[0] >> 8) & 7, CALC_OPS[fn & 15], param8(w[1] & 0xFF).c_str(),
				   param8(w[1] >> 8).c_str());
	}
	case 0x0E:
	case 0x1B:
	{
		std::string s = fmt("model %d v:%d flip:%d%s", (w[0] >> 5) & 0x7FF, s8(w[1]), (w[1] >> 8) & 0x3F,
							(w[1] & 0x4000) ? "" : " light");
		if (op == 0x1B)
			s += fmt(" scale:%s%d #%03x", (w[2] & 0x800) ? "t" : "", w[2] >> 12, (w[2] << 1) & 0xFFF);
		return s;
	}
	case 0x0F:
		return fmt("sound %d", w[0] >> 5);
	case 0x10:
	case 0x11:
		return fmt("%s %s (v:%d n:%d r:%d) (v:%d n:%d r:%d)", op_name(op), col.c_str(), s8(w[1]), w[2] & 0x7F,
				   w[2] >> 8, s8(w[1] >> 8), w[3] & 0x7F, w[3] >> 8);
	case 0x13:
	case 0x14:
	{
		int bit = w[1] >> 8;
		std::string cond = bit ? fmt("bit(%s, %d)", param8(w[1] & 0xFF).c_str(), bit) : param8(w[1] & 0xFF);
		return fmt("if %s%s -> %s", op == 0x13 ? "!" : "", cond.c_str(), jt().c_str());
	}
	case 0x15:
		return (w[0] & 0x8000) ? "ztree pop" : fmt("ztree push v:%d", s8(w[0] >> 5));
	case 0x16:
		return fmt("bline %d, %d, %d, %d %s", s8(w[1] >> 8), s8(w[1]), s8(w[2] >> 8), s8(w[2]), col.c_str());
	case 0x17:
	{
		int lim = w[1];
		std::string cmp = (lim & 0x8000) ? fmt("< %d", lim ^ 0x8000) : fmt("> %d", lim);
		return fmt("if dist(%d, %d) %s -> %s", s8(w[2]), s8(w[2] >> 8), cmp.c_str(), jt().c_str());
	}
	case 0x18:
		return fmt("circles %s size:%04x", col.c_str(), w[1]);
	case 0x1A:
		return fmt("colour #%03x n:%d%s", (w[0] >> 4) & 0xFFE, w[1] & 0x7F, (w[1] >> 8) ? " list" : "");
	default:
		break;
	}
	std::string s = op_name(op);
	for (auto x : w)
		s += fmt(" %04x", x);
	return s;
}

std::string disasm(const std::vector<Instr> &instrs)
{
	std::string out;
	std::vector<int> targets;
	for (auto &i : instrs)
		if (i.jump >= 0 && i.op != 0x05)
			targets.push_back(i.jump);
	for (auto &i : instrs)
	{
		bool t = std::find(targets.begin(), targets.end(), i.off) != targets.end();
		std::string hex;
		for (auto x : i.w)
			hex += fmt("%04x ", x);
		out += fmt("%s%04x: %-58s ; %s\n", t ? ">" : " ", i.off, describe(i).c_str(), hex.c_str());
	}
	return out;
}

/* -- primitives -------------------------------------------------------------- */

static std::vector<Vec3> bezier(const Vec3 &p0, const Vec3 &p1, const Vec3 &p2, const Vec3 &p3, int n = 6)
{
	std::vector<Vec3> pts;
	for (int i = 1; i <= n; i++)
	{
		double t = (double)i / n, u = 1 - t;
		pts.push_back(p0 * (u * u * u) + p1 * (3 * u * u * t) + p2 * (3 * u * t * t) + p3 * (t * t * t));
	}
	return pts;
}

static void basis(const Vec3 &n0, Vec3 &n, Vec3 &u, Vec3 &v)
{
	n = normalize(n0);
	Vec3 ref = std::fabs(n.x) < 0.9 ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
	u = normalize(cross(n, ref));
	v = cross(n, u);
}

static std::vector<Vec3> disc(const Vec3 &c, const Vec3 &nrm, double r, int seg = 12)
{
	Vec3 n, u, v;
	basis(nrm, n, u, v);
	std::vector<Vec3> out;
	for (int i = 0; i < seg; i++)
	{
		double a = 2 * M_PI * i / seg;
		out.push_back(c + u * (r * std::cos(a)) + v * (r * std::sin(a)));
	}
	return out;
}

std::vector<Prim> primitives(const Model &m, const std::vector<Instr> &instrs, const Model::ParentFn &parent)
{
	auto vp = [&](int i) { return m.vertex_pos(i, parent); };
	std::vector<Prim> out;
	struct Region
	{
		int end, id, kind; /* kind 0 none, 1 far, 2 var */
	};
	std::vector<Region> regions;
	auto block_at = [&](int off) {
		while (!regions.empty() && off >= regions.back().end)
			regions.pop_back();
		return regions.empty() ? 0 : regions.back().id;
	};
	auto set_flags = [&](Prim &p) {
		for (auto &r : regions)
		{
			if (r.kind == 1)
				p.far = true;
			if (r.kind == 2)
				p.var = true;
		}
	};
	auto region_kind = [](const Instr &ins) {
		const auto &w = ins.w;
		if (ins.op == 0x0C && !(w[1] & 0x8000))
			return 1; /* if < z: skipped up close = far away copy */
		if (ins.op == 0x17 && !(w[1] & 0x8000))
			return 1; /* if dist > max: skipped when big on screen */
		if ((ins.op == 0x13 || ins.op == 0x14) && (w[1] & 0xC0) == 0x80)
			return 2; /* object state variable */
		return 0;
	};
	auto face = [&](const std::vector<int> &idx, int colour, int nidx, int blk) {
		Prim p;
		p.kind = Prim::Face;
		for (int i : idx)
		{
			auto q = vp(i);
			if (!q)
				return;
			p.pts.push_back(*q);
		}
		p.colour = colour;
		p.normal = m.normal_vec(nidx);
		p.nidx = nidx;
		p.block = blk;
		set_flags(p);
		out.push_back(p);
	};
	for (const Instr &ins : instrs)
	{
		int blk = block_at(ins.off);
		const auto &w = ins.w;
		int op = ins.op;
		if (ins.jump >= 0 && op != 0x05 && ins.jump > ins.off)
			regions.push_back({ins.jump, ins.off, region_kind(ins)});
		if (op == 0x03 || op == 0x07)
		{
			std::vector<int> idx = {s8(w[1] >> 8), s8(w[1]), s8(w[2] >> 8)};
			int n = w[2] & 0x7F;
			face(idx, ins.colour(), n, blk);
			if (op == 0x07)
			{
				for (int &i : idx)
					i ^= 1;
				face(idx, ins.colour(), n ? (n ^ 1) : 0, blk);
			}
		}
		else if (op == 0x04 || op == 0x08)
		{
			/* the renderer goes round hi(w1), lo(w2), lo(w1), hi(w2) */
			std::vector<int> idx = {s8(w[1] >> 8), s8(w[2]), s8(w[1]), s8(w[2] >> 8)};
			int n = w[3] & 0x7F;
			face(idx, ins.colour(), n, blk);
			if (op == 0x08)
			{
				for (int &i : idx)
					i ^= 1;
				face(idx, ins.colour(), n ? (n ^ 1) : 0, blk);
			}
		}
		else if (op == 0x05 && w.size() > 1)
		{
			int n = w[1] & 0x7F;
			std::vector<std::vector<Vec3>> loops;
			std::vector<Vec3> cur;
			int last = -1000;
			for (auto &s : ins.sub)
			{
				if (s.name == "line")
				{
					if (!cur.empty() && last != s.a[0])
					{
						loops.push_back(cur);
						cur.clear();
					}
					auto a = vp(s.a[0]), b = vp(s.a[1]);
					if (!a || !b)
						continue;
					if (cur.empty())
						cur.push_back(*a);
					cur.push_back(*b);
					last = s.a[1];
				}
				else if (s.name == "linec")
				{
					if (auto p = vp(s.a[0]))
						cur.push_back(*p);
					last = s.a[0];
				}
				else if (s.name == "bezier")
				{
					if (!cur.empty() && last != s.a[0])
					{
						loops.push_back(cur);
						cur.clear();
					}
					auto p0 = vp(s.a[0]), p1 = vp(s.a[1]), p2 = vp(s.a[2]), p3 = vp(s.a[3]);
					if (!p0 || !p1 || !p2 || !p3)
						continue;
					if (cur.empty())
						cur.push_back(*p0);
					auto b = bezier(*p0, *p1, *p2, *p3);
					cur.insert(cur.end(), b.begin(), b.end());
					last = s.a[3];
				}
				else if (s.name == "bezierc")
				{
					auto p1 = vp(s.a[0]), p2 = vp(s.a[1]), p3 = vp(s.a[2]);
					if (!p1 || !p2 || !p3 || cur.empty())
						continue;
					auto b = bezier(cur.back(), *p1, *p2, *p3);
					cur.insert(cur.end(), b.begin(), b.end());
					last = s.a[2];
				}
				else if (s.name == "join")
				{
					if (!cur.empty())
						loops.push_back(cur);
					cur.clear();
					last = -1000;
				}
				else if (s.name == "circle")
				{
					auto c = vp(s.a[0]);
					auto nn = m.normal_vec(s.a[2]);
					if (c && nn)
						loops.push_back(disc(*c, *nn, s.a[1]));
					cur.clear();
					last = -1000;
				}
			}
			if (!cur.empty())
				loops.push_back(cur);
			for (auto lp : loops)
			{
				if (lp.size() > 3)
				{
					const Vec3 &a = lp.front(), &b = lp.back();
					if (std::fabs(a.x - b.x) < 1e-6 && std::fabs(a.y - b.y) < 1e-6 && std::fabs(a.z - b.z) < 1e-6)
						lp.pop_back();
				}
				if (lp.size() >= 3)
				{
					Prim p;
					p.kind = Prim::Face;
					p.pts = lp;
					p.colour = ins.colour();
					p.normal = m.normal_vec(n);
					p.nidx = n;
					p.block = blk;
					set_flags(p);
					out.push_back(p);
				}
			}
		}
		else if (op == 0x02)
		{
			auto a = vp(s8(w[1] >> 8)), b = vp(s8(w[1]));
			if (a && b)
			{
				Prim p;
				p.kind = Prim::Line;
				p.pts = {*a, *b};
				p.colour = ins.colour();
				p.block = blk;
				set_flags(p);
				out.push_back(p);
			}
		}
		else if (op == 0x16)
		{
			auto p0 = vp(s8(w[1] >> 8)), p1 = vp(s8(w[1])), p2 = vp(s8(w[2] >> 8)), p3 = vp(s8(w[2]));
			if (p0 && p1 && p2 && p3)
			{
				Prim p;
				p.kind = Prim::Line;
				p.pts = {*p0};
				auto b = bezier(*p0, *p1, *p2, *p3);
				p.pts.insert(p.pts.end(), b.begin(), b.end());
				p.colour = ins.colour();
				p.block = blk;
				set_flags(p);
				out.push_back(p);
			}
		}
		else if (op == 0x10 || op == 0x11)
		{
			auto a = vp(s8(w[1])), b = vp(s8(w[1] >> 8));
			if (a && b)
			{
				double r1 = w[2] >> 8, r2 = w[3] >> 8;
				Vec3 d = *b - *a;
				if (length(d) > 0)
				{
					Vec3 n, u, v;
					basis(d, n, u, v);
					const int seg = 8;
					std::vector<Vec3> ra, rb;
					for (int i = 0; i < seg; i++)
					{
						double t = 2 * M_PI * i / seg;
						ra.push_back(*a + u * (r1 * std::cos(t)) + v * (r1 * std::sin(t)));
						rb.push_back(*b + u * (r2 * std::cos(t)) + v * (r2 * std::sin(t)));
					}
					for (int i = 0; i < seg; i++)
					{
						int j = (i + 1) % seg;
						Prim p;
						p.pts = {ra[i], ra[j], rb[j], rb[i]};
						p.colour = ins.colour();
						p.block = blk;
						set_flags(p);
						out.push_back(p); /* no normal: two sided */
					}
					Prim cap1, cap2;
					cap1.pts = std::vector<Vec3>(ra.rbegin(), ra.rend());
					cap1.normal = -d;
					cap2.pts = rb;
					cap2.normal = d;
					for (Prim *c : {&cap1, &cap2})
					{
						c->colour = ins.colour();
						c->block = blk;
						set_flags(*c);
						out.push_back(*c);
					}
				}
			}
		}
		else if (op == 0x01)
		{
			if (auto p = vp(s8(w[2] >> 8)))
			{
				Prim l;
				l.kind = Prim::Light;
				l.pts = {*p};
				l.colour = ins.colour();
				l.block = blk;
				set_flags(l);
				out.push_back(l);
			}
		}
		else if (op == 0x0E || op == 0x1B)
		{
			Prim s;
			s.kind = Prim::Submodel;
			s.sub_index = (w[0] >> 5) & 0x7FF;
			s.sub_vertex = s8(w[1]);
			s.data1 = w[1];
			s.block = blk;
			s.scale_word = op == 0x1B ? w[2] : -1;
			set_flags(s);
			out.push_back(s);
		}
	}
	return out;
}

/* -- collect --------------------------------------------------------------- */

namespace
{
/* child axis each parent axis comes from, per flip code (& 7) */
const int AXIS_SWAPS[8][3] = {{0, 1, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}, {1, 0, 2}, {0, 2, 1}, {0, 1, 2}, {0, 1, 2}};

struct Xform
{
	Vec3 origin;
	Vec3 rot[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}; /* rot[i] = image of axis i */
	int scale = 0;
	Vec3 apply(const Vec3 &c) const
	{
		double k = std::ldexp(1.0, scale);
		return origin + rot[0] * (c.x * k) + rot[1] * (c.y * k) + rot[2] * (c.z * k);
	}
	Vec3 dir(const Vec3 &v) const { return rot[0] * v.x + rot[1] * v.y + rot[2] * v.z; }
	Vec3 inverse(const Vec3 &p) const
	{
		double k = std::ldexp(1.0, scale);
		Vec3 d = p - origin;
		return {dot(d, rot[0]) / k, dot(d, rot[1]) / k, dot(d, rot[2]) / k};
	}
	Xform child(const Vec3 &o, int data1, int sc) const
	{
		Xform x;
		x.origin = o;
		x.scale = sc;
		int code = (data1 >> 8) & 7;
		if (code == 6)
		{
			for (int i = 0; i < 3; i++)
				x.rot[i] = rot[i];
			return x;
		}
		const int *sw = AXIS_SWAPS[code];
		int bits[3] = {0x2000, 0x1000, 0x0800};
		for (int i = 0; i < 3; i++)
			x.rot[sw[i]] = rot[i] * ((data1 & bits[i]) ? -1.0 : 1.0);
		return x;
	}
};
} // namespace

Collected collect(const Image &image, int index, bool submodels, int max_depth, int max_prims)
{
	Collected out;
	auto root = image.model(index);
	if (!root)
		return out;
	std::function<void(const Model &, const Xform &, const Model::ParentFn &, int, const std::string &,
					   const std::string &)>
		walk = [&](const Model &model, const Xform &xf, const Model::ParentFn &parent_fn, int depth,
				   const std::string &tag, const std::string &inherited) {
			bool ok = true;
			auto instrs = decode(image.data, model.code_addr(), 0x8000, &ok);
			if (!ok)
				return;
			auto prims = primitives(model, instrs, parent_fn);
			for (auto &p : prims)
			{
				std::string grp = p.block == 0 ? tag : tag + fmt("_if%04x", p.block);
				std::string mark = inherited;
				if (p.far && mark.find("~far") == std::string::npos)
					mark += "~far";
				if (p.var && mark.find("~var") == std::string::npos)
					mark += "~var";
				grp += mark;
				if (p.kind == Prim::Face)
				{
					GFace f;
					for (auto &q : p.pts)
						f.pts.push_back(xf.apply(q));
					f.colour = p.colour;
					if (p.normal)
						f.normal = xf.dir(*p.normal);
					f.group = grp;
					out.faces.push_back(f);
				}
				else if (p.kind == Prim::Line)
				{
					GLine l;
					for (auto &q : p.pts)
						l.pts.push_back(xf.apply(q));
					l.colour = p.colour;
					l.group = grp;
					out.lines.push_back(l);
				}
				else if (p.kind == Prim::Light)
					out.lights.push_back({xf.apply(p.pts[0]), p.colour, grp});
				else if (p.kind == Prim::Submodel && submodels && depth < max_depth &&
						 (max_prims < 0 || (int)(out.faces.size() + out.lines.size()) < max_prims))
				{
					int ci = p.sub_index;
					auto child = image.model(ci);
					if (!child || ci == model.index)
						continue;
					auto org_local = model.vertex_pos(p.sub_vertex, parent_fn);
					if (!org_local)
						continue;
					Vec3 org = xf.apply(*org_local);
					int sparam = 0;
					if (p.scale_word >= 0 && !(p.scale_word & 0x800))
						sparam = s8(p.scale_word >> 8) >> 4;
					Xform cxf = xf.child(org, p.data1, sparam + child->scale + child->scale2);
					int pv = p.sub_vertex;
					Model::ParentFn pfn = [&model, parent_fn, xf, cxf, pv](int i) -> std::optional<Vec3> {
						if (-(i + 1) != 0)
							return std::nullopt;
						auto q = model.vertex_pos(pv, parent_fn);
						if (!q)
							return std::nullopt;
						return cxf.inverse(xf.apply(*q));
					};
					walk(*child, cxf, pfn, depth + 1, tag + fmt("_m%d", ci), mark);
				}
			}
		};
	Xform x0;
	x0.scale = root->scale;
	walk(*root, x0, nullptr, 0, fmt("m%d", index), "");
	return out;
}

std::vector<Vec3> orient(const std::vector<Vec3> &pts, const std::optional<Vec3> &normal)
{
	if (!normal || pts.size() < 3)
		return pts;
	if (dot(newell(pts), *normal) < 0)
		return std::vector<Vec3>(pts.rbegin(), pts.rend());
	return pts;
}

Vec3 display_rgb(int colour, int base, bool shaded)
{
	int c = colour & 0xEEE;
	int r = (c >> 8) & 15, g = (c >> 4) & 15, b = c & 15;
	if (colour & 0x10)
	{
		r += (base >> 8) & 15;
		g += (base >> 4) & 15;
		b += base & 15;
	}
	if (shaded && !(colour & 0x100))
	{
		r += 3;
		g += 3;
		b += 3;
	}
	return {std::min(15, r) / 15.0, std::min(15, g) / 15.0, std::min(15, b) / 15.0};
}

/* -- ship data --------------------------------------------------------------- */

const ShipField SHIP_FIELDS[] = {
	{0, true, "thrust_fwd", "Forward thrust", "main engine acceleration"},
	{2, true, "thrust_rev", "Reverse thrust", "negative: retro thrusters"},
	{4, false, "guns", "Gun mountings", "low 4 bits: 1 = front, 2 = front and rear"},
	{6, false, "mass", "Mass fully laden (t)", "also the hull strength: full hull = 4 x this"},
	{8, false, "capacity", "Internal capacity (t)", "space for drive, equipment and cargo"},
	{10, false, "price", "Price (x1000 credits)", "shipyard price of a new hull"},
	{12, false, "x12", "Unknown +12", "left as the ship you based it on"},
	{14, false, "name_id", "Name string id", "set by the game for new ships"},
	{16, false, "crew", "Crew", ""},
	{18, false, "pylons", "Missile pylons", ""},
	{20, false, "drive", "Drive",
	 "low byte: 0 none, 1 interplanetary, 2-8 class 1-7, 10-12 military; +32768: built in"},
	{22, true, "kill_points", "Elite points for a kill", "added to the rating of whoever destroys it"},
};
const int SHIP_FIELD_COUNT = sizeof(SHIP_FIELDS) / sizeof(SHIP_FIELDS[0]);

const char *const CATEGORIES[] = {
	"shipyards: independent systems, small ships",
	"shipyards: independent, bigger ships / general traffic",
	"shipyards: Imperial, small / Imperial navy patrols",
	"shipyards: Imperial, bigger ships",
	"shipyards: Federal, small / Federal navy patrols",
	"shipyards: Federal, bigger ships",
	"shipyards: other systems, small / pirates",
	"pirates, assassins and bounty hunters (fighters)",
	"military patrols (independent)",
	"station shuttles and lifters",
	"local police (Viper)",
	"big ships (bulk carriers, cruisers)",
	"traders, passengers and mission ships",
};
const int CATEGORY_COUNT = sizeof(CATEGORIES) / sizeof(CATEGORIES[0]);

std::map<std::string, int> read_fields(const std::vector<uint8_t> &d)
{
	std::map<std::string, int> out;
	for (int i = 0; i < SHIP_FIELD_COUNT; i++)
	{
		const ShipField &f = SHIP_FIELDS[i];
		int v = f.off + 1 < (int)d.size() ? (d[f.off] << 8) | d[f.off + 1] : 0;
		out[f.key] = f.is_signed ? s16(v) : v;
	}
	return out;
}

void write_fields(std::vector<uint8_t> &d, const std::map<std::string, int> &values)
{
	for (int i = 0; i < SHIP_FIELD_COUNT; i++)
	{
		const ShipField &f = SHIP_FIELDS[i];
		auto it = values.find(f.key);
		if (it == values.end() || f.off + 1 >= (int)d.size())
			continue;
		int v = it->second;
		if (f.is_signed)
			v = std::max(-32768, std::min(32767, v));
		v &= 0xffff;
		d[f.off] = (uint8_t)(v >> 8);
		d[f.off + 1] = (uint8_t)v;
	}
}

std::vector<std::vector<int>> picker_lists(const Image &img)
{
	std::vector<std::vector<int>> out;
	uint32_t base = img.ship_picks;
	int cats = 0, first = 0xffff;
	while (cats * 4 < first && cats < 32)
	{
		first = std::min(first, img.u16(base + cats * 4 + 2));
		cats++;
	}
	for (int c = 0; c < cats; c++)
	{
		int n = img.u16(base + c * 4), off = img.u16(base + c * 4 + 2);
		std::vector<int> l;
		for (int k = 0; k < n; k++)
			l.push_back(img.u16(base + off + 2 * k) / 2);
		out.push_back(l);
	}
	return out;
}

std::vector<std::pair<int, std::string>> ships(const Image &img)
{
	std::vector<std::pair<int, std::string>> out;
	for (int i = 10; i <= 41; i++)
	{
		auto m = img.model(i);
		if (m && m->ship_off)
		{
			std::string n = m->ship_name();
			if (!n.empty())
				out.push_back({i, n});
		}
	}
	return out;
}

} // namespace fe2
