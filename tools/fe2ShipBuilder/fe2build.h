/*
 * fe2build.h - compile a polygon mesh into Frontier: Elite 2 model data, and
 * read/write .fe2m files (see src/custom_ships.h for the format).
 *
 * Per model the game allows 64 vertices (128 if mirrored in x), 63 normals
 * and signed byte coordinates (<< scale mm). Bigger meshes are split into
 * parts: extra models (511 down) the main model draws with the MODEL opcode,
 * each with its own origin and finer scale. Replacing a ship keeps the
 * template's ship data and (unless bare) its engine plumes and lights.
 */
#pragma once

#include <array>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "fe2core.h"

namespace fe2
{

struct BuildError : std::runtime_error
{
	using std::runtime_error::runtime_error;
};

struct BuildMesh
{
	std::vector<Vec3> verts; /* game mm, FE2 axes (x right, y up, z forward) */
	struct Face
	{
		std::vector<int> idx; /* counter-clockwise seen from outside, in OBJ axes */
		std::string mat;	  /* fe2_XXX[_2s], or anything with a Kd below */
		std::string group;	  /* collision* groups are collision hulls */
	};
	std::vector<Face> faces;
	std::map<std::string, Vec3> kd; /* diffuse colours of non fe2_ materials */
	std::map<std::string, bool> emissive;
};

struct BuildOptions
{
	int index = 0;
	std::optional<int> scale;
	std::optional<int> colour;
	bool keep_extras = true;
	std::set<int> used_slots;
	bool fix_normals = false;
	std::string collision = "auto"; /* auto box kdop14 kdop26 custom */
	std::map<std::string, int> ship_values;
};

using ModelBlobs = std::vector<std::pair<int, std::vector<uint8_t>>>;

struct BuildResult
{
	ModelBlobs models;
	int faces = 0, verts = 0, norms = 0, scale = 0, parts = 0, bytes = 0;
	std::vector<int> slots;
	std::vector<std::string> warnings;
	bool extras = false;
	std::string collision;
};

/* Model numbers past the game's 240 (up to MDLMOD_MAX - 1 in fe2_modded.s) are shared: new ship
   types take them from 240 up, the extra parts of big ships from 511 down */
constexpr int SHIP_SLOT_FIRST = 240, SHIP_SLOT_LAST = 511;
constexpr int SHIP_SLOT_COUNT = SHIP_SLOT_LAST - SHIP_SLOT_FIRST + 1;
constexpr int EXTRA_SLOT_FIRST = 240, EXTRA_SLOT_LAST = 511;
extern const char *const COLLISION_MODES[];
extern const int COLLISION_MODE_COUNT;

BuildResult build_model(const BuildMesh &mesh, const Model *tmpl, const Image &image, const BuildOptions &opt);

/* colour value of a material name (fe2_XXX, *paint*, *glow*, else Kd) */
int material_colour(const std::string &name, const Vec3 *kd, bool emissive);
bool two_sided(const std::string &name);

/* .fe2m files */
struct Fe2mMeta
{
	std::string name;
	int like = -1;
	std::vector<std::pair<int, int>> picks; /* (category, weight) */
};
std::vector<uint8_t> fe2m_bytes(const ModelBlobs &models, const Fe2mMeta *meta);
void write_fe2m(const std::string &path, const ModelBlobs &models, const Fe2mMeta *meta);
bool read_fe2m(const std::string &path, ModelBlobs &models, Fe2mMeta &meta);
bool parse_fe2m(const std::vector<uint8_t> &data, ModelBlobs &models, Fe2mMeta &meta);

} // namespace fe2
