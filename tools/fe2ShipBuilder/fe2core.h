/*
 * fe2core.h - Frontier: Elite 2 models, read straight out of the assembled
 * game (fe2/fe2_bin.h, compiled into the studio): the model table, model
 * headers, vertices, normals, bytecode, ship data and ship names.
 *
 * Model layout (big endian words, offsets from the model start):
 *   +00 code  +02 vertices  +04 vertex buffer size (64 per vertex)
 *   +06 normals  +08 normal buffer size ((n + 1) * 4)  +0a scale  +0c scale2
 *   +0e radius  +10 ?  +12 base colour  +14..+18 ?  +1a collision  +1c ship data
 * Vertex N is index 2N, 2N+1 is the same one mirrored in x; normals the same
 * from index 2. See tools/fe2ShipBuilder/README.md for the rest.
 */
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fe2
{

struct Vec3
{
	double x = 0, y = 0, z = 0;
	Vec3() = default;
	Vec3(double a, double b, double c) : x(a), y(b), z(c) {}
	double &operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
	double operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
	Vec3 operator+(const Vec3 &o) const { return {x + o.x, y + o.y, z + o.z}; }
	Vec3 operator-(const Vec3 &o) const { return {x - o.x, y - o.y, z - o.z}; }
	Vec3 operator*(double k) const { return {x * k, y * k, z * k}; }
	Vec3 operator-() const { return {-x, -y, -z}; }
	bool operator==(const Vec3 &o) const { return x == o.x && y == o.y && z == o.z; }
};
double dot(const Vec3 &a, const Vec3 &b);
Vec3 cross(const Vec3 &a, const Vec3 &b);
double length(const Vec3 &a);
Vec3 normalize(const Vec3 &a);
Vec3 newell(const std::vector<Vec3> &pts);
/* ear clipping in the polygon's plane: index triples */
std::vector<std::array<int, 3>> triangulate(const std::vector<Vec3> &pts, const Vec3 &normal);

inline int s8(int v)
{
	v &= 0xff;
	return v >= 0x80 ? v - 256 : v;
}
inline int s16(int v)
{
	v &= 0xffff;
	return v >= 0x8000 ? v - 0x10000 : v;
}

struct Vertex
{
	int type, a, b, c;
};
struct Normal
{
	int v, x, y, z;
};

class Image;

class Model
{
  public:
	const Image *image = nullptr;
	int index = 0;
	uint32_t addr = 0;
	int header[15] = {};
	int code_off, vert_off, vert_buf, norm_off, norm_buf, scale, scale2, radius, x10, colour, x14, x16, x18,
		coll_off, ship_off;
	int num_verts = 0, num_norms = 0;
	std::vector<Vertex> verts;
	std::vector<Normal> norms;

	using ParentFn = std::function<std::optional<Vec3>(int)>;
	std::optional<Vec3> vertex_pos(int index, const ParentFn &parent = nullptr, int depth = 0) const;
	std::optional<Vec3> normal_vec(int index) const;
	uint32_t code_addr() const { return addr + code_off; }
	std::vector<uint8_t> ship_data(int n = 32) const;
	std::vector<uint8_t> collision_bytes() const;
	std::string ship_name() const;
};

class Image
{
  public:
	std::vector<uint8_t> data;
	uint32_t table = 0, gamedata2 = 0, ship_picks = 0, core_strings = 0;
	int num_models = 0;
	std::map<int, uint32_t> overrides; /* model number -> address in data */

	static const Image &game(); /* the built in game binary */
	Image with_models(const std::vector<std::pair<int, std::vector<uint8_t>>> &models) const;

	int u8(uint32_t a) const { return a < data.size() ? data[a] : 0; }
	int u16(uint32_t a) const { return a + 1 < data.size() ? (data[a] << 8) | data[a + 1] : 0; }
	std::optional<uint32_t> model_addr(int index) const;
	std::optional<Model> model(int index) const;
	std::vector<int> model_numbers() const;
	std::string core_string(int index) const;
};

/* -- bytecode ------------------------------------------------------------ */

struct ComplexSub
{
	std::string name; /* done bezier line linec bezierc join circle */
	int a[4] = {0, 0, 0, 0};
};

struct Instr
{
	int off = 0, op = 0, size = 0;
	int jump = -1; /* byte offset of the branch target, -1 = none */
	std::vector<uint16_t> w;
	std::vector<ComplexSub> sub;
	int colour() const { return (w[0] >> 4) & 0xfff; }
};

std::vector<Instr> decode(const std::vector<uint8_t> &data, uint32_t start, uint32_t limit = 0x8000,
						  bool *ok = nullptr);
std::string describe(const Instr &ins);
std::string disasm(const std::vector<Instr> &instrs);
const char *op_name(int op);

struct Prim
{
	enum Kind
	{
		Face,
		Line,
		Light,
		Submodel
	} kind = Face;
	std::vector<Vec3> pts;
	int colour = 0;
	std::optional<Vec3> normal;
	int nidx = 0;
	int block = 0;
	bool far = false, var = false;
	/* submodel */
	int sub_index = 0, sub_vertex = 0, data1 = 0, scale_word = -1;
};

std::vector<Prim> primitives(const Model &m, const std::vector<Instr> &instrs,
							 const Model::ParentFn &parent = nullptr);

/* -- whole model in world space (mm, FE2 axes: x right, y up, z forward) - */

struct GFace
{
	std::vector<Vec3> pts;
	int colour;
	std::optional<Vec3> normal;
	std::string group; /* m16, m16_if0104, m16_m2~var, ... ~far = low detail */
};
struct GLine
{
	std::vector<Vec3> pts;
	int colour;
	std::string group;
};
struct GLight
{
	Vec3 pos;
	int colour;
	std::string group;
};
struct Collected
{
	std::vector<GFace> faces;
	std::vector<GLine> lines;
	std::vector<GLight> lights;
};

Collected collect(const Image &image, int index, bool submodels = true, int max_depth = 4, int max_prims = -1);
std::vector<Vec3> orient(const std::vector<Vec3> &pts, const std::optional<Vec3> &normal);
/* approximate on-screen colour of a colour value, 0..1 */
Vec3 display_rgb(int colour, int base = 0x666, bool shaded = true);

/* -- ship data and the ship picker ---------------------------------------- */

struct ShipField
{
	int off;
	bool is_signed;
	const char *key, *label, *help;
};
extern const ShipField SHIP_FIELDS[];
extern const int SHIP_FIELD_COUNT;
extern const char *const CATEGORIES[];
extern const int CATEGORY_COUNT;

std::map<std::string, int> read_fields(const std::vector<uint8_t> &data);
void write_fields(std::vector<uint8_t> &data, const std::map<std::string, int> &values);
std::vector<std::vector<int>> picker_lists(const Image &image);
/* the game's flyable ships: (model number, name) */
std::vector<std::pair<int, std::string>> ships(const Image &image);

} // namespace fe2
