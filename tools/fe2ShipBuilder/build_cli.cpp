/*
 * build_cli.cpp - FE2ShipBuilder's "--build" without the window (no SDL, ImGui
 * or GL), for scripts such as tools/ksc_model.py:
 *
 *   fe2build_cli [--build] model.obj TEMPLATE INDEX out.fe2m [bare]
 *
 * TEMPLATE is the game model whose header (and, unless "bare", engine plumes
 * and lights) the new model takes; INDEX is the new model's number. Writes a
 * version 1 .fe2m (no name or "like" records). Build it with:
 *
 *   g++ -std=c++17 -O2 -I tools/fe2ShipBuilder -I . -I fe2 -I src \
 *       tools/fe2ShipBuilder/build_cli.cpp tools/fe2ShipBuilder/fe2core.cpp \
 *       tools/fe2ShipBuilder/fe2build.cpp tools/fe2ShipBuilder/mesh.cpp -o fe2build_cli
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mesh.h"

int main(int argc, char **argv)
{
	int a = (argc > 1 && strcmp(argv[1], "--build") == 0) ? 2 : 1;
	if (argc - a < 4)
	{
		fprintf(stderr, "usage: fe2build_cli [--build] model.obj TEMPLATE INDEX out.fe2m [bare]\n");
		return 2;
	}
	studio::Mesh m;
	std::string err;
	if (!studio::Mesh::from_obj(argv[a], m, err))
	{
		fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	const fe2::Image &img = fe2::Image::game();
	int base = atoi(argv[a + 1]), index = atoi(argv[a + 2]);
	auto tmpl = img.model(base);
	if (!tmpl)
	{
		fprintf(stderr, "no game model %d\n", base);
		return 1;
	}
	fe2::BuildOptions opt;
	opt.index = index;
	if (argc - a > 4 && strcmp(argv[a + 4], "bare") == 0)
		opt.keep_extras = false;
	try
	{
		auto res = fe2::build_model(m.to_build(), &*tmpl, img, opt);
		fe2::write_fe2m(argv[a + 3], res.models, nullptr);
		printf("%d faces %d verts %d norms scale %d parts %d bytes %d\n", res.faces, res.verts, res.norms, res.scale,
			   res.parts, res.bytes);
		for (auto &w : res.warnings)
			printf("warning: %s\n", w.c_str());
	}
	catch (std::exception &e)
	{
		fprintf(stderr, "build failed: %s\n", e.what());
		return 1;
	}
	return 0;
}
