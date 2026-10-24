/*
 * app.cpp - FE2 Galaxy Viewer: the game's galaxy map, taken further, as a
 * program of its own. The atlas itself (map, search, panels) is shared with
 * the game: src/galaxy/atlas.h. This file is the window around it, and the
 * command line:
 *   FE2GalaxyViewer --dump X Y [N]   sector X,Y (Sol = 0,0) and system N's screens
 *   FE2GalaxyViewer --selftest       speed and sanity checks over the whole galaxy
 *   FE2GalaxyViewer --smoke          a scripted tour of the window
 *   FE2GalaxyViewer --catalog OUT X0 Y0 X1 Y1 [STEP [SECONDS]]   every explored system as JSON lines
 */
#include <glad/glad.h>

#include <SDL.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C"
{
#include "host.h" /* the machine, for --selftest's in-game test */
}

#include "atlas.h"
#include "fe2vm.h"
#include "galaxy.h"
#include "galaxy_labels.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"
#include "map_view.h"
#include "style.h"

using namespace galaxy;
using namespace ui;

/* the exe's folder, with a trailing slash ("" if unknown: the current one).
 * SDL_GetBasePath is not built in (SDL_FILESYSTEM is off). */
static std::string exe_dir()
{
#ifdef _WIN32
	char path[MAX_PATH];
	DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
		return "";
	std::string s(path, n);
	size_t slash = s.find_last_of("\\/");
	return slash == std::string::npos ? "" : s.substr(0, slash + 1);
#else
	return "";
#endif
}

static unsigned int gl_texture(const uint32_t *rgba, int w, int h, unsigned int old, bool smooth)
{
	GLuint tex = old;
	if (!tex)
		glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	GLint filter = smooth ? GL_LINEAR : GL_NEAREST;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	return tex;
}

/* -- the window ------------------------------------------------------------------ */

class App : public Atlas
{
  public:
	double frame_ms = 0;

	bool start()
	{
		texture = gl_texture;
		px = PX;
		return init(exe_dir() + "FE2Galaxy.cache");
	}

	void menu_bar()
	{
		if (!ImGui::BeginMenuBar())
			return;
		if (ImGui::BeginMenu("View"))
		{
			view_options();
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Go"))
		{
			go_options();
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Range"))
		{
			range_options();
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Help"))
		{
			about_text();
			ImGui::EndMenu();
		}
		ImGui::EndMenuBar();
	}

	void ui()
	{
		auto t0 = std::chrono::steady_clock::now();
		ImGuiViewport *vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(vp->WorkPos);
		ImGui::SetNextWindowSize(vp->WorkSize);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		ImGui::Begin("FE2 Galaxy Viewer", nullptr,
					 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_MenuBar |
						 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
		ImGui::PopStyleVar();
		menu_bar();
		if (!init_error.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, rgb444(COL_RED));
			ImGui::TextWrapped("%s", init_error.c_str());
			ImGui::PopStyleColor();
			ImGui::End();
			return;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::GetIO().WantTextInput)
			view.has_selected = false;
		update();

		float status_h = ImGui::GetFrameHeight();
		ImVec2 avail = ImGui::GetContentRegionAvail();
		/* wide enough for the game's 320 pixel screens at 2 pixels a pixel when it can be */
		float side = std::clamp(avail.x * 0.45f, 380.0f, 740.0f);
		ImVec2 map_pos = ImGui::GetCursorScreenPos();
		ImVec2 map_size(avail.x - side, avail.y - status_h);
		map(map_pos, map_size);

		ImGui::SetCursorScreenPos(ImVec2(map_pos.x + map_size.x, map_pos.y));
		ImGui::BeginChild("##side", ImVec2(side, 0), ImGuiChildFlags_Borders);
		search_panel(150);
		ImGui::Separator();
		selected_panel();
		if (ImGui::CollapsingHeader("Systems near Sol (placed by hand in the game)"))
			core_list();
		ImGui::EndChild();

		ImGui::SetCursorScreenPos(ImVec2(map_pos.x + 4, map_pos.y + map_size.y));
		char buf[64];
		snprintf(buf, sizeof(buf), "   view %.0f ly across   %.1f ms", avail.x / view.scale * view.ly_per_sector,
				 frame_ms);
		ImGui::TextUnformatted((status_text() + buf).c_str());
		ImGui::End();
		frame_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	}
};

/* -- --dump ------------------------------------------------------------------------ */

static void print_screen(const char *title, const Screen &s)
{
	printf("  -- %s\n", title);
	for (auto &r : s)
		printf("     (%3d,%3d) #%06x %s\n", r.x, r.y, r.colour, r.text.c_str());
}

/* FE2Galaxy --dump X Y [N]: sector X,Y (as the game numbers them, Sol = 0,0),
 * and the info screens of system N in it */
static int cmd_dump(int argc, char **argv)
{
	Galaxy g;
	std::string err;
	if (!g.init(err))
	{
		fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	int sx = argc > 3 ? atoi(argv[2]) : 0, sy = argc > 3 ? atoi(argv[3]) : 0;
	int x = SOL_X + sx, y = SOL_Y + sy;
	auto t0 = std::chrono::steady_clock::now();
	const Sector &s = g.sector(x, y);
	double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	printf("sector %d,%d ($%x,$%x): %d systems in %.3f ms, density %d smooth %d, %.3f ly per sector\n", sx, sy, x,
		   y, (int)s.stars.size(), ms, g.density(x, y, true), g.density(x, y, false), g.ly_per_sector());
	for (auto &st : s.stars)
		printf("%2d %-20s %08x pos %08x %08x %08x mult %d type %2d ($%02x) info $%02x %s  %s\n", id_n(st.id),
			   st.name.c_str(), st.id, st.x, st.y, st.z, st.multiple, st.type, st.type_byte, st.info,
			   st.explored ? "explored" : "-", st.description.c_str());
	if (!s.stars.empty())
		printf("distance first-last: %s\n", g.distance_text(g.distance(s.stars.front(), s.stars.back())).c_str());
	if (getenv("FE2GALAXY_SUMMARY"))
	{
		std::vector<uint32_t> ids;
		for (auto &st : s.stars)
			ids.push_back(st.id);
		for (uint32_t id : ids)
		{
			fprintf(stderr, "summary of %08x...\n", id);
			const Summary &sum = g.summary(id);
			printf("  #%d: %d bodies, %d surface ports, %d orbital%s\n", id_n(id), sum.bodies, sum.surface_ports,
				   sum.orbital_ports, sum.hangs ? " (the game hangs)" : sum.crashes ? " (the game crashes)" : "");
		}
	}
	if (argc > 4)
	{
		int n = atoi(argv[4]);
		if (n < (int)s.stars.size())
		{
			t0 = std::chrono::steady_clock::now();
			const Details &d = g.details(s.stars[n].id);
			ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			printf("details of %s (%.2f ms): %s %s\n", s.stars[n].name.c_str(), ms, d.ok ? "ok" : "FAILED",
				   d.error.c_str());
			print_screen("system", d.system);
			print_screen("trade", d.trade);
			print_screen("bodies", d.bodies_screen);
			if (d.picture.pixels.size() == 320 * 200)
			{
				int count[16] = {};
				for (uint8_t p : d.picture.pixels)
					count[p & 15]++;
				printf("  -- picture: %d click areas, rows %d-%d\n", (int)d.picture.areas.size(), d.picture.top,
					   d.picture.bottom);
				for (int i = 0; i < 16; i++)
					printf("     colour %2d #%06x: %d pixels\n", i, d.picture.palette[i], count[i]);
				if (const char *bmp = getenv("FE2GALAXY_PICTURE")) /* a .bmp of it, to look at */
					if (FILE *f = fopen(bmp, "wb"))
					{
						const int W = 320, H = 200, size = 54 + W * H * 3;
						uint8_t hdr[54] = {'B', 'M'};
						auto put32 = [&](int at, uint32_t v) {
							for (int i = 0; i < 4; i++)
								hdr[at + i] = (uint8_t)(v >> (8 * i));
						};
						put32(2, size), put32(10, 54), put32(14, 40), put32(18, W), put32(22, (uint32_t)-H);
						hdr[26] = 1, hdr[28] = 24;
						fwrite(hdr, 1, 54, f);
						for (uint8_t p : d.picture.pixels)
						{
							uint32_t c = d.picture.palette[p & 15];
							uint8_t bgr[3] = {(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16)};
							fwrite(bgr, 1, 3, f);
						}
						fclose(f);
					}
			}
			if (getenv("FE2GALAXY_ARRIVE"))
			{
				uint32_t mod = fe2vm_rd32(fe2vm_a6() + 92);
				fe2vm_regs r{};
				int res = fe2vm_call(mod + 32, &r);
				printf("arrival at %08x: result %d %s\n", mod + 32, res, fe2vm_error());
				for (int i = 0; i < 115; i++)
				{
					uint32_t o = FE2_big_space + (uint16_t)fe2vm_rd16(FE2_obj_offsets + 2 * i);
					if (getenv("FE2GALAXY_ARRIVE")[0] == '2' && i < 12)
						printf("  [%d] flag %d off %x type %d\n", i, fe2vm_rd8(FE2_big_space + i), o, fe2vm_rd8(o + 94));
					if (fe2vm_rd8(o + 94) != 1)
						continue;
					printf("  slot %d model %d par %d\n", i, fe2vm_rd16(o + 90) / 2, fe2vm_rd8(o + 68));
				}
			}
			for (auto &b : d.bodies)
			{
				printf("  body %d parent %d kind %d %s\n", b.index, b.parent, b.kind, b.name.c_str());
				printf("     model %d mass %.4f temp %d radius %.1f km\n", b.model, b.mass_earth, b.temperature_c, b.radius_km);
				if (getenv("FE2GALAXY_RAW"))
				{
					printf("     raw");
					for (int i = 0; i < 34; i++)
						printf("%s%02x", i % 2 ? "" : " ", b.raw[i]);
					printf("\n");
				}
				print_screen("body", b.details);
			}
		}
	}
	return 0;
}

/* FE2Galaxy --selftest: runs the game code over the whole galaxy and says
 * how fast it is and whether anything failed. Exit code 0 when all is well. */
static int cmd_selftest()
{
	setvbuf(stdout, nullptr, _IONBF, 0); /* see how far it got if it dies */
	using clock = std::chrono::steady_clock;
	auto ms_since = [](clock::time_point t) {
		return std::chrono::duration<double, std::milli>(clock::now() - t).count();
	};
	Galaxy g;
	std::string err;
	if (!g.init(err))
	{
		printf("FAIL init: %s\n", err.c_str());
		return 1;
	}
	int fails = 0;
	auto t = clock::now();
	long long dsum = 0;
	for (int y = 0; y < 512; y++)
		for (int x = 0; x < 512; x++)
			dsum += g.density(x * 16 + 8, y * 16 + 8, false);
	printf("density map 512x512: %.0f ms (sum %lld)\n", ms_since(t), dsum);

	/* a 64x64 block round Sol, like a zoomed out view */
	t = clock::now();
	long stars = 0;
	for (int y = -32; y < 32; y++)
		for (int x = -32; x < 32; x++)
		{
			stars += (long)g.sector(SOL_X + x, SOL_Y + y).stars.size();
			if (!g.error().empty())
			{
				printf("FAIL sector %d,%d: %s\n", x, y, g.error().c_str());
				g.clear_error();
				fails++;
			}
		}
	printf("4096 sectors round Sol: %.0f ms, %ld stars\n", ms_since(t), stars);

	/* the same sector twice gives the same stars */
	const Sector a = g.sector(SOL_X + 7, SOL_Y - 3);
	g.trim_cache(0);
	const Sector &b = g.sector(SOL_X + 7, SOL_Y - 3);
	bool same = a.stars.size() == b.stars.size();
	for (size_t i = 0; same && i < a.stars.size(); i++)
		same = a.stars[i].name == b.stars[i].name && a.stars[i].x == b.stars[i].x && a.stars[i].z == b.stars[i].z;
	printf("repeatable: %s\n", same ? "yes" : "NO");
	fails += !same;

	/* edges and random places all over the galaxy */
	int corners[][2] = {{0, 0}, {SECTORS - 1, 0}, {0, SECTORS - 1}, {SECTORS - 1, SECTORS - 1}, {4096, 4096}};
	for (auto &c : corners)
		g.sector(c[0], c[1]);
	uint32_t seed = 12345;
	auto rnd = [&]() { return (seed = seed * 1103515245u + 12345u) >> 8; };
	t = clock::now();
	int systems = 0, bodies = 0, unexplored = 0, hangs = 0;
	double worst = 0;
	for (int i = 0; i < 1000; i++)
	{
		/* mostly near Sol, where systems have planets, some anywhere */
		int x = i % 3 ? SOL_X + (int)(rnd() % 200) - 100 : (int)(rnd() % SECTORS);
		int y = i % 3 ? SOL_Y + (int)(rnd() % 200) - 100 : (int)(rnd() % SECTORS);
		const Sector &s = g.sector(x, y);
		if (s.stars.empty())
			continue;
		uint32_t id = s.stars[rnd() % s.stars.size()].id;
		auto t1 = clock::now();
		const Details &d = g.details(id);
		double ms = ms_since(t1);
		systems++;
		bodies += (int)d.bodies.size();
		unexplored += d.bodies.empty();
		if (d.hangs || d.crashes)
		{
			/* the game's own code loops forever or crashes for these: a game bug, not ours */
			const Star *st = g.star(id);
			printf("game %s on %s ($%08x, sector %d,%d): %s\n", d.hangs ? "hangs" : "crashes",
				   st ? st->name.c_str() : "?", id, id_x(id) - SOL_X, id_y(id) - SOL_Y, d.error.c_str());
			hangs++;
			continue;
		}
		worst = std::max(worst, ms);
		if (!d.ok || d.system.empty())
		{
			printf("FAIL details of $%08x: %s\n", id, d.error.c_str());
			fails++;
		}
	}
	printf("info screens of %d systems: %.0f ms (worst %.1f ms), %d bodies, %d without planet data, %d the game "
		   "hangs or crashes on\n",
		   systems, ms_since(t), worst, bodies, unexplored, hangs);
	/* the name search's copy of the hash names every star the way the game does */
	t = clock::now();
	int named = 0, wrong = 0;
	auto cores = g.core_sectors();
	for (int i = 0; i < 3000; i++)
	{
		int x = (int)(rnd() % SECTORS), y = (int)(rnd() % SECTORS);
		if (i % 2)
			x = SOL_X + (int)(rnd() % 400) - 200, y = SOL_Y + (int)(rnd() % 400) - 200;
		if (std::find(cores.begin(), cores.end(), std::make_pair(x, y)) != cores.end())
			continue;
		for (auto &st : g.sector(x, y).stars)
		{
			named++;
			if (g.hashed_name(x, y, id_n(st.id)) != st.name)
			{
				if (wrong++ < 5)
					printf("FAIL name hash at %d,%d #%d: %s, game says %s\n", x, y, id_n(st.id),
						   g.hashed_name(x, y, id_n(st.id)).c_str(), st.name.c_str());
			}
		}
		g.trim_cache(20000);
	}
	printf("name hash: %d names checked against the game, %d different (%.0f ms)\n", named, wrong, ms_since(t));
	fails += wrong > 0;

	for (const char *q : {"Zeaex", "andessho", "lave"})
	{
		t = clock::now();
		long long checked = 0;
		auto found = g.find_by_name(q, SOL_X, SOL_Y, 20, &checked);
		printf("find \"%s\": %d found in %.0f ms (%lld star slots hashed)", q, (int)found.size(), ms_since(t),
			   checked);
		if (!found.empty())
			printf(", nearest %s at %d,%d", g.star(found[0])->name.c_str(), id_x(found[0]) - SOL_X,
				   id_y(found[0]) - SOL_Y);
		printf("\n");
	}

	/* starport overlay: sectors round Sol */
	t = clock::now();
	int inhabited = 0, ports = 0, orbital = 0, sectors = 0;
	for (int y = -6; y < 6; y++)
		for (int x = -6; x < 6; x++)
		{
			const SectorSummary &ss = g.sector_summary(SOL_X + x, SOL_Y + y);
			inhabited += ss.inhabited, ports += ss.ports, orbital += ss.orbital, sectors++;
		}
	for (int y = -6; y < 6; y++)
		for (int x = -6; x < 6; x++)
		{
			std::vector<uint32_t> ids;
			for (auto &st : g.sector(SOL_X + x, SOL_Y + y).stars)
				ids.push_back(st.id);
			for (uint32_t id : ids)
				if (g.summary(id).crashes) /* a game bug, like the hangs */
					printf("game crashes on %s ($%08x, sector %d,%d #%d)\n", g.star(id)->name.c_str(), id, x, y,
						   id_n(id));
		}
	printf("starports in %d sectors round Sol: %d inhabited systems, %d starports (%d orbital) in %.0f ms\n",
		   sectors, inhabited, ports, orbital, ms_since(t));
	const Details &sol = g.details(make_id(SOL_X, SOL_Y, 0));
	printf("Sol: picture %d click areas, rows %d-%d; kinds:", (int)sol.picture.areas.size(), sol.picture.top,
		   sol.picture.bottom);
	std::vector<int> seen_kinds;
	for (auto &b : sol.bodies)
		if (std::find(seen_kinds.begin(), seen_kinds.end(), b.kind) == seen_kinds.end())
		{
			seen_kinds.push_back(b.kind);
			printf(" %d%s=%s", b.kind, b.port ? "(port)" : "", b.kind_name.c_str());
		}
	printf("\n");
	for (auto &b : sol.bodies)
		if (b.name == "Earth" || b.name == "Moon")
			printf("  %s: %.3f AU, e %.3f, i %.1f, %.2f days\n", b.name.c_str(), b.au, b.eccentricity,
				   b.inclination, b.period_days);

	/* The game's way (src/ui/ui_galaxy.cpp): a batch of atlas work inside a
	   running game must leave the machine exactly as it found it. Junk in
	   the registers and flags, a stack pointer where the game's would be,
	   then everything and compare every byte. */
	{
		Galaxy lg;
		lg.init(err); /* a fresh copy of the game, so nothing is cached */
		fe2vm_set_live(1);
		for (int i = 0; i < 16; i++)
			Regs[i]._u32 = 0x12345678u * (i + 1);
		Regs[15]._u32 = 0xeffe0;
		N = 1, nZ = 7, V = 0, C = 1, X = 1, rdest = 0x1234;
		std::vector<uint8_t> before((uint8_t *)m68kram, (uint8_t *)m68kram + MEM_SIZE);
		union Reg regs_before[16];
		memcpy(regs_before, Regs, sizeof(Regs));
		t = clock::now();
		fe2vm_begin();
		for (int i = 0; i < 40; i++)
			lg.sector(SOL_X + i % 8, SOL_Y + i / 8);
		int n = (int)lg.details(make_id(SOL_X, SOL_Y, 0)).bodies.size();
		lg.details(make_id(SOL_X + 3, SOL_Y - 2, 2));	  /* Zeaex */
		lg.details(make_id(SOL_X - 9, SOL_Y + 30, 5));	  /* hangs */
		lg.details(make_id(SOL_X + 4, SOL_Y + 1, 3));	  /* crashes */
		lg.sector_summary(SOL_X + 1, SOL_Y + 1);
		lg.find_by_name("zeaex", SOL_X, SOL_Y, 5);
		fe2vm_end();
		double ms = ms_since(t);
		long diff = 0, first = -1;
		for (long i = 0; i < (long)MEM_SIZE; i++)
			if ((uint8_t)m68kram[i] != before[i])
			{
				if (first < 0)
					first = i;
				diff++;
			}
		bool regs_same = !memcmp(regs_before, Regs, sizeof(Regs)) && N == 1 && nZ == 7 && V == 0 && C == 1 &&
						 X == 1 && rdest == 0x1234;
		printf("in-game batch (%.0f ms, Sol %d bodies): %ld RAM bytes changed%s, registers %s\n", ms, n, diff,
			   first >= 0 ? (" from offset " + std::to_string(first)).c_str() : "", regs_same ? "the same" : "CHANGED");
		fails += diff != 0 || !regs_same || n < 50;
		fe2vm_set_live(0);
	}

	printf("%s\n", fails ? "FAILED" : "all ok");
	return fails ? 1 : 0;
}


/* -- --catalog ---------------------------------------------------------------------- */

static void json_str(FILE *f, const std::string &s)
{
	fputc('"', f);
	for (unsigned char c : s)
	{
		if (c == '"' || c == '\\')
			fprintf(f, "\\%c", c);
		else if (c < 0x20)
			fprintf(f, "\\u%04x", c);
		else
			fputc(c, f); /* UTF-8 passes through */
	}
	fputc('"', f);
}

/* FE2Galaxy --catalog OUT X0 Y0 X1 Y1 [STEP [SECONDS]]: one JSON line per
 * explored system in the sectors X0..X1, Y0..Y1 (Sol = 0,0), every STEP-th
 * sector on each axis, stopping after SECONDS. Each line has the system's
 * star and every body the game's generator makes, with the numbers the body
 * panel shows and the radius of its model (tools/catalog.py reads them) */
static int cmd_catalog(int argc, char **argv)
{
	if (argc < 7)
	{
		fprintf(stderr, "usage: --catalog OUT X0 Y0 X1 Y1 [STEP [SECONDS]]\n");
		return 1;
	}
	Galaxy g;
	std::string err;
	if (!g.init(err))
	{
		fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	FILE *f = fopen(argv[2], "w");
	if (!f)
		return 1;
	int x0 = atoi(argv[3]), y0 = atoi(argv[4]), x1 = atoi(argv[5]), y1 = atoi(argv[6]);
	int step = argc > 7 ? std::max(1, atoi(argv[7])) : 1;
	double budget = argc > 8 ? atof(argv[8]) : 1e9;
	auto t0 = std::chrono::steady_clock::now();
	auto secs = [&]() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
	long sectors = 0, systems = 0, bad = 0;
	for (int sy = y0; sy <= y1 && secs() < budget; sy += step)
		for (int sx = x0; sx <= x1 && secs() < budget; sx += step)
		{
			sectors++;
			std::vector<uint32_t> ids;
			for (auto &st : g.sector(SOL_X + sx, SOL_Y + sy).stars)
				if (st.explored)
					ids.push_back(st.id);
			for (uint32_t id : ids)
			{
				const Star &st = *g.star(id);
				const Details &d = g.details(id);
				if (!d.ok || d.hangs || d.crashes)
				{
					bad++;
					continue;
				}
				systems++;
				fprintf(f, "{\"id\":%u,\"sx\":%d,\"sy\":%d,\"n\":%d,\"name\":", id, sx, sy, id_n(id));
				json_str(f, st.name);
				fprintf(f, ",\"star\":");
				json_str(f, st.description);
				fprintf(f, ",\"type\":%d,\"multiple\":%d,\"allegiance\":%d,\"info\":%d,\"bodies\":[", st.type,
						st.multiple, st.allegiance, st.info);
				for (size_t i = 0; i < d.bodies.size(); i++)
				{
					const Body &b = d.bodies[i];
					fprintf(f, "%s{\"i\":%d,\"p\":%d,\"k\":%d,\"kn\":", i ? "," : "", b.index, b.parent, b.kind);
					json_str(f, b.kind_name);
					fprintf(f, ",\"name\":");
					json_str(f, b.name);
					fprintf(f, ",\"port\":%d,\"surf\":%d", b.port, b.surface);
					if (b.model)
						fprintf(f, ",\"model\":%d", b.model);
					if (b.mass_earth >= 0)
						fprintf(f, ",\"mass\":%.4f", b.mass_earth);
					if (b.temperature_c != -9999)
						fprintf(f, ",\"temp\":%d", b.temperature_c);
					if (b.radius_km >= 0)
						fprintf(f, ",\"radius\":%.1f", b.radius_km);
					if (b.au >= 0)
						fprintf(f, ",\"au\":%.4f,\"period\":%.3f", b.au, b.period_days);
					fputc('}', f);
				}
				fprintf(f, "]}\n");
			}
			if (sectors % 50 == 0)
				fprintf(stderr, "\r%ld sectors, %ld systems, %ld failed, %.0f s", sectors, systems, bad, secs());
		}
	fprintf(stderr, "\n%ld sectors, %ld systems, %ld failed, %.0f s\n", sectors, systems, bad, secs());
	fclose(f);
	return 0;
}

/* -- main -------------------------------------------------------------------------- */

static int imgui_asserts;

void galaxy_imgui_assert(const char *expr, const char *file, int line)
{
	if (imgui_asserts++ < 10)
		fprintf(stderr, "ImGui assert: %s (%s:%d)\n", expr, file, line);
}

/* --smoke: drives the window through a scripted tour for a few seconds, then
 * exits; 0 when nothing went wrong */
static bool smoke_step(App &app, int frame, int &status)
{
	MapView &v = app.view;
	switch (frame)
	{
	case 10: v.go_to(SECTORS / 2.0, SECTORS / 2.0, 0.09); break;							/* whole galaxy */
	case 70: app.opt.counts = true; v.go_to(SOL_X + 0.5, SOL_Y + 0.5, 4); break;			/* star counts */
	case 110: app.opt.counts = false; app.opt.tilt = 50; v.go_to(SOL_X + 0.5, SOL_Y + 0.5, 90); break;
	case 150: app.opt.hue = true; break; /* starport survey and overlay */
	case 170: app.opt.colour = MapOptions::BY_ALLEGIANCE; app.opt.range_ly = 20; break;
	case 200:
		strcpy(app.query, "zeaex"); /* the whole galaxy */
		app.start_search();
		break;
	case 330:
		if (!app.hits.empty())
			app.go_to_star(app.hits.front().id, 120);
		break;
	case 350: v.select(make_id(SOL_X - 9, SOL_Y + 30, 5)); break; /* the one the game hangs on */
	case 370: v.select(make_id(SOL_X + 4, SOL_Y + 1, 3)); break;	 /* the one the game crashes on */
	case 390:
		v.select(make_id(SOL_X, SOL_Y, 0)); /* Sol's planets: the game view */
		app.system_tab = 2;
		app.planets_tab = 0;
		break;
	case 400: app.body_sel = 4; break;
	case 420: app.planets_tab = 1; app.orbit_play = true; break; /* orbits */
	case 450: app.orbit_focus = 4; app.orbit_log = false; break;
	case 480: app.planets_tab = 2; break; /* list */
	case 500: v.go_to(SOL_X + 200.5, SOL_Y - 150.5, 2); app.opt.tilt = 0; break;
	case 560:
		printf("smoke: %d ImGui asserts, %d sectors cached, %d surveyed, %d search hits, last frame %.1f ms\n",
			   imgui_asserts, (int)app.g.cache_size(), (int)app.g.sector_summaries().size(), (int)app.hits.size(),
			   app.frame_ms);
		printf("cache: %s (saved %d)\n", app.cache_path.c_str(), (int)app.saved_sectors);
		status = imgui_asserts || !app.init_error.empty() ? 1 : 0;
		return true;
	}
	return false;
}

int main(int argc, char **argv)
{
	bool dump = argc > 1 && std::string(argv[1]) == "--dump";
	bool selftest = argc > 1 && std::string(argv[1]) == "--selftest";
	bool smoke = argc > 1 && std::string(argv[1]) == "--smoke";
	bool catalog = argc > 1 && std::string(argv[1]) == "--catalog";
	if (dump || selftest || smoke || catalog)
	{
#ifdef _WIN32
		/* a windows program has no console: print to the one it was started
		   from, unless the output already goes somewhere (a pipe or file) */
		HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
		if ((out == NULL || out == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS))
		{
			freopen("CONOUT$", "w", stdout);
			freopen("CONOUT$", "w", stderr);
		}
#endif
		if (!smoke)
			return catalog ? cmd_catalog(argc, argv) : dump ? cmd_dump(argc, argv) : cmd_selftest();
	}
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
		return 1;
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_Window *win = SDL_CreateWindow("FE2 Galaxy Atlas", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1600, 960,
									   SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	if (!win)
		return 1;
	SDL_GLContext ctx = SDL_GL_CreateContext(win);
	SDL_GL_MakeCurrent(win, ctx);
	SDL_GL_SetSwapInterval(1);
	if (!gladLoadGLLoader(SDL_GL_GetProcAddress))
		return 1;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::GetIO().IniFilename = nullptr;
	ImGui_ImplSDL2_InitForOpenGL(win, ctx);
	ImGui_ImplOpenGL3_Init("#version 330 core");
	build_font(PX);
	build_style(PX);

	App app;
	app.start();

	bool quit = false;
	int frame = 0, status = 0;
	while (!quit)
	{
		if (smoke && smoke_step(app, frame, status))
			break;
		frame++;
		SDL_Event e;
		while (SDL_PollEvent(&e))
		{
			ImGui_ImplSDL2_ProcessEvent(&e);
			if (e.type == SDL_QUIT)
				quit = true;
			if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE)
				quit = true;
		}
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplSDL2_NewFrame();
		ImGui::NewFrame();
		app.ui();
		ImGui::Render();
		int w, h;
		SDL_GL_GetDrawableSize(win, &w, &h);
		glViewport(0, 0, w, h);
		glClearColor(0, 0, 34 / 255.0f, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		SDL_GL_SwapWindow(win);
	}
	app.save_cache(true);
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplSDL2_Shutdown();
	ImGui::DestroyContext();
	SDL_GL_DeleteContext(ctx);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return status;
}
