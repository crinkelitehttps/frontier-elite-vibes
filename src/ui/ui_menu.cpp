/*
 * ui_menu.cpp - the emulator menu: settings, cheats, debug info, about.
 *
 * Layout mirrors the old Nuklear menu: a main window pinned to the top left
 * of the game area, and an optional second window (console or more cheats)
 * next to it. Sizes are in 320x240 game units so the menu scales with the
 * window.
 */
#include <SDL.h>
#include <stdio.h>

#include "imgui.h"
#include "ui_menu.h"
#include "ui_style.h"
#include "ui_galaxy.h"

extern "C"
{
#include "main.h"
#include "renderer.h"
#include "keymap.h"
#include "touch_input.h"
#include "cheats.h"
#include "game_state.h"
#include "custom_ships.h"
#include "mods.h"
#include "freecam.h"
#include "gamepad.h"
#include "gl/gl_cockpit.h"
#include "headtrack.h"
#include "vr.h"
}

namespace
{

enum class Page
{
	Home,
	Settings,
	Cheats,
	Mods,
	Debug,
	DebugOptions,
	About,
};

enum class Side
{
	None,
	Console,
	MoreCheats,
};

Page page = Page::Home;
Side side = Side::None;

/* ---- layout helpers ----------------------------------------------------- */
constexpr float GAME_W = 320.0f;
constexpr float GAME_H = 240.0f;

ImVec2 game_pos(float x, float y)
{
	return ImVec2(Screen_GetGameOffsetX() + x * Screen_GetGameWidth() / GAME_W,
				  Screen_GetGameOffsetY() + y * Screen_GetGameHeight() / GAME_H);
}

ImVec2 game_size(float w, float h)
{
	return ImVec2(w * Screen_GetGameWidth() / GAME_W, h * Screen_GetGameHeight() / GAME_H);
}

/* Shown under the home page buttons, e.g. why the free camera didn't start */
const char *home_message = nullptr;

/* Shown under the cheats after one of them fails (e.g. no cargo space) */
const char *cheat_message = nullptr;

void report(const char *err)
{
	cheat_message = err;
}

/* Checkbox whose label wraps at the window edge instead of running off it */
bool wrapped_checkbox(const char *label, bool *value)
{
	/* the box has no visible label, so its ID comes from the label text:
	 * every box in a window needs its own */
	ImGui::PushID(label);
	bool changed = ImGui::Checkbox("##check", value);
	ImGui::SameLine();
	ImGui::TextWrapped("%s", label);
	ImGui::PopID();
	return changed;
}

/* A combo over `count` names from `name(i)`; returns true when changed */
template <typename NameFn> bool name_combo(const char *id, int *current, int count, NameFn name)
{
	bool changed = false;
	ImGui::SetNextItemWidth(-FLT_MIN);
	if (ImGui::BeginCombo(id, name(*current)))
	{
		for (int i = 0; i < count; i++)
		{
			if (ImGui::Selectable(name(i), i == *current))
			{
				*current = i;
				changed = true;
			}
		}
		ImGui::EndCombo();
	}
	return changed;
}

/* Touch friendly cash slider: tall, full width and logarithmic, so both
 * small and huge amounts can be reached with a finger. The -/+ buttons
 * nudge it by a tenth of its size for exact values. */
void cash_slider()
{
	const int max_credits = 0x7fffffff / 10;
	static int credits = 0;
	static bool dragging = false;
	if (!dragging)
		credits = (int)(cheat_cash() / 10);

	const float p = ui_style_px();
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3 * p, 7 * p));
	ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 14 * p);
	ImGui::SetNextItemWidth(-FLT_MIN);
	bool changed = ImGui::SliderInt("##cash", &credits, 0, max_credits, "$%d",
									ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
	dragging = ImGui::IsItemActive();
	ImGui::PopStyleVar(2);
	if (changed)
		cheat_set_cash((long)credits * 10);

	/* step: the power of ten below a tenth of the amount, at least 1 */
	int step = 1;
	while (step <= credits / 100)
		step *= 10;
	char minus[24], plus[24];
	snprintf(minus, sizeof(minus), "-%d##cash", step);
	snprintf(plus, sizeof(plus), "+%d##cash", step);
	float w = ui_row_width(2);
	if (ui_button(minus, w))
		cheat_set_cash((long)(credits > step ? credits - step : 0) * 10);
	ImGui::SameLine();
	if (ui_button(plus, w))
		cheat_set_cash((long)(credits < max_credits - step ? credits + step : max_credits) * 10);
}

/* ---- pages ------------------------------------------------------------ */
void page_home()
{
	if (ui_button("CLOSE"))
		toggle_m68k_menu = false;
#if FE2_USE_MODDED
	if (ui_button("FREE CAMERA"))
	{
		home_message = nullptr;
		if (freecam_toggle())
			toggle_m68k_menu = false;
		else
			home_message = "The free camera works in the flight view.";
	}
#endif
	if (ui_button("GALAXY ATLAS"))
	{
		/* opens over the menu; the menu is there again when it closes */
		ui_galaxy_show(true);
	}
	if (ui_button("SETTINGS"))
		page = Page::Settings;
	if (ui_button("CHEATS"))
		page = Page::Cheats;
	if (ui_button("MODS"))
		page = Page::Mods;
	if (ui_button("DEBUG"))
		page = Page::Debug;
	if (ui_button("ABOUT"))
		page = Page::About;
	side = Side::None;
	if (home_message)
		ImGui::TextWrapped("%s", home_message);
}

void page_settings()
{
	bool keep_aspect = Screen_GetLetterboxMode();

	ui_heading("Game settings");
	if (ui_button("Fullscreen Toggle"))
		Screen_ToggleFullScreen();
	if (ui_button("Cycle Renderer"))
		Screen_ToggleRenderer();
#if FE2_USE_MODDED
	ui_heading("Free camera");
	wrapped_checkbox("Invert look up / down", &freecam_invert_y);
	wrapped_checkbox("Show the help box (H)", &freecam_show_help);
#endif
	ui_heading("Aspect ratio");
	if (ui_button(keep_aspect ? "Keep 4:3: ON" : "Keep 4:3: OFF"))
	{
		Screen_SetLetterboxMode(!keep_aspect);
	}

	ui_heading("Cockpit view (Ctrl-K)");
	wrapped_checkbox("3D cockpit in the front view", &cockpit_enabled);
	ImGui::TextWrapped("Field of view (vertical)");
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::SliderInt("##fov", &cockpit_fov, 30, 120, "%d degrees");

	ui_heading("Head tracking");
	wrapped_checkbox("OpenTrack, UDP over network", &headtrack_enabled);
	ImGui::TextWrapped("Port (default %d)", HEADTRACK_DEFAULT_PORT);
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::InputInt("##port", &headtrack_port, 0, 0);
	if (headtrack_port < 1 || headtrack_port > 65535)
		headtrack_port = HEADTRACK_DEFAULT_PORT;
	ImGui::TextWrapped("%s", !headtrack_enabled ? "Off" : headtrack_receiving() ? "Receiving" : "Waiting for data");
	if (ui_button("Recentre (Ctrl-C)"))
		headtrack_recenter();

#ifdef GLF_VR
	ui_heading("VR headset (OpenXR)");
	wrapped_checkbox("Use the headset", &vr_enabled);
	ImGui::TextWrapped("Resolution (from the next start)");
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::SliderInt("##vrres", &vr_resolution, 50, 150, "%d%%");
	ImGui::TextWrapped("%s", vr_status());
	if (vr_running() && ui_button("Recentre headset (Ctrl-C)"))
		vr_recenter();
#endif

	ui_heading("Gamepad");
	ImGui::TextWrapped("%s", gamepad_connected() ? "Connected" : "None connected");
	ImGui::TextWrapped("Bindings: %s, beside the saves", GAMEPAD_FILE);
}

void page_cheats()
{
	if (ui_button(side == Side::MoreCheats ? "HIDE MORE CHEATS" : "MORE CHEATS"))
		side = (side == Side::MoreCheats) ? Side::None : Side::MoreCheats;

	ImGui::TextWrapped("Cash: $%ld.%ld", cheat_cash() / 10, cheat_cash() % 10);
	ui_heading("Set cash to");
	static const struct
	{
		const char *label;
		long tenths;
	} amounts[] = {{"0", 0},
				   {"100", 1000},
				   {"1,000", 10000},
				   {"1,000,000", 10000000},
				   {"10,000,000", 100000000},
				   {"MAX AMOUNT POSSIBLE", 0x7fffffffL}};
	for (const auto &a : amounts)
		if (ui_button(a.label))
			cheat_set_cash(a.tenths);

	ui_heading("Custom amount");
	cash_slider();
}

/* Grey detail line under an entry, wrapped to the menu's width */
static void detail_line(const char *text)
{
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
	ImGui::TextWrapped("%s", text);
	ImGui::PopStyleColor();
}

/* The .fe2m files custom_ships.c installed, new ship types first */
static void mods_installed_ships()
{
	int count = custom_ships_info_count();
	char heading[64];
	snprintf(heading, sizeof(heading), "Installed ships (%d)", count);
	ui_heading(heading);
	if (!count)
	{
		ImGui::TextWrapped("None. Make some with FE2ShipBuilder.");
		return;
	}
	for (int pass = 1; pass >= 0; pass--)
		for (int i = 0; i < count; i++)
		{
			const CustomShipInfo *s = custom_ships_info(i);
			if (s->new_ship != pass)
				continue;
			if (s->new_ship)
				ImGui::TextWrapped("%s  (new ship, model %d)", s->name[0] ? s->name : "Unnamed ship", s->model);
			else
				ImGui::TextWrapped("%s  (replaces model %d)", s->name[0] ? s->name : "Model", s->model);
			char more[160];
			int n = snprintf(more, sizeof(more), "   %s, %s", s->file, s->source);
			if (s->models > 1 && n < (int)sizeof(more))
				n += snprintf(more + n, sizeof(more) - n, ", %d parts", s->models);
			if (s->like >= 0 && n < (int)sizeof(more))
				n += snprintf(more + n, sizeof(more) - n, ", flies like model %d", s->like);
			if (s->new_ship && n < (int)sizeof(more))
				snprintf(more + n, sizeof(more) - n, ", in %d ship list%s", s->lists, s->lists == 1 ? "" : "s");
			detail_line(more);
		}
}

/* Equipment the mods add to the game's list */
static void mods_items()
{
	int first = cheat_mod_equipment_first(), count = cheat_equipment_count() - first;
	char heading[64];
	snprintf(heading, sizeof(heading), "Items (%d)", count);
	ui_heading(heading);
	if (count <= 0)
	{
		ImGui::TextWrapped("None.");
		return;
	}
	for (int i = first; i < cheat_equipment_count(); i++)
	{
		ImGui::TextWrapped("%s", cheat_equipment_name(i));
		bool flag = cheat_equipment_kind(i) == EQUIP_FLAG;
		char more[80];
		snprintf(more, sizeof(more), "   %dt%s", cheat_equipment_mass(i),
				 !flag ? "" : cheat_has_flag(i) ? ", fitted to your ship" : ", not fitted");
		detail_line(more);
	}
}

void page_mods()
{
#if !FE2_USE_MODDED
	ImGui::TextWrapped("This is the original, unmodded game (built with GLF_MODDED_FE2 off), "
					   "so there are no mods.");
	return;
#endif
	ui_heading("Saved games");
	wrapped_checkbox("Saves loadable by the unmodded game", &mods_vanilla_saves);
	ImGui::TextWrapped(mods_vanilla_saves ? "Mod state is left out of new saves."
										  : "New saves keep mod state and need this modded game.");

	mods_installed_ships();
	mods_items();

	for (int i = 0; i < mods_count(); i++)
	{
		const GameMod *m = mods_get(i);
		ui_heading(m->name);
		ImGui::TextWrapped("%s", m->about);
		ImGui::TextWrapped("In saves: %s", m->in_saves ? m->in_saves : "nothing");
	}
}

void page_debug()
{
	if (ui_button("OPTIONS"))
		page = Page::DebugOptions;
	if (ui_button(side == Side::Console ? "HIDE CONSOLE" : "SHOW CONSOLE"))
		side = (side == Side::Console) ? Side::None : Side::Console;

	/* Build tag from the end of fe2_modded.s (Lbuildtag_marker). Changes
	 * whenever that file changes - if this still shows the word you had
	 * before your latest edit, the rebuild didn't pick it up. */
	ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "Build tag: %s", game_build_tag());

	char info[256];
	Screen_GetRendererInfo(info, sizeof(info));
	ImGui::TextWrapped("%s", info);

	ImGui::TextWrapped("Touch Controls: %d", toggle_touch_controls ? 1 : 0);
	if (toggle_touch_controls)
	{
		ImGui::TextWrapped("Thrust Keys: %d", toggle_thrust_keys_touch);
		ImGui::TextWrapped("Arrow Keys: %d", toggle_arrow_keys_touch);
		ImGui::TextWrapped("Dropdown Keys: %d", toggle_dropdown_keys_touch);
	}

	ui_heading("Game state");
	GameScreen screen = game_screen();
	ImGui::TextWrapped("Screen: %s (owner %d)", game_screen_name(screen), game_screen_owner());
	if (screen == SCREEN_FLIGHT)
		ImGui::TextWrapped("View: %s", game_view_name(game_flight_view()));
	ImGui::TextWrapped("Status: %s%s", game_status_name(game_flight_status()),
					   game_engines_off() ? ", engines off" : "");
}

void page_debug_options()
{
	ui_heading("WARNING: CRASH ZONE!");

	if (ui_button("Draw FPS"))
	{
		toggle_fps_draw = !toggle_fps_draw;
		SDL_Keysym key = {};
		key.scancode = SDL_SCANCODE_F;
		key.sym = SDLK_f;
		Keymap_KeyDown(&key);
		Keymap_KeyUp(&key);
	}
	if (ui_button("Debug Draw"))
		toggle_debug_draw = !toggle_debug_draw;
	if (ui_button("Toggle Touch Controls"))
		toggle_touch_controls = !toggle_touch_controls;
#if !defined(__EMSCRIPTEN__) && !defined(ANDROID)
	if (ui_button("Dump M68k Memory"))
		dump_m68k_toggle = 1;
#endif

	ui_heading("Emulator speed (default 20ms)");
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::SliderInt("##speed", &emulation_speed, 1, 100, "%d ms");
}

void page_about()
{
	ui_heading("Original author");
	ImGui::TextWrapped("Tom Morton");
	ui_heading("Extended by");
	ImGui::TextWrapped("Brett Wilson");
	ui_heading("Libraries");
	ImGui::TextWrapped("SDL2, PhysFS, minivorbis, glutess, glad and Dear ImGui");
	ui_heading("Thanks to");
	ImGui::TextWrapped("Frontier Developments, the Hatari emulator, Pcercuei and Kochise");
}

/* ---- side windows ------------------------------------------------------- */
void side_console()
{
	ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
	ImGui::TextUnformatted(clslog ? clslog : "");
	ImGui::EndChild();
}

void player_cheats()
{
	static int commodity = 0;
	ui_heading("Cargo");
	name_combo("##cargo", &commodity, CHEAT_COMMODITIES, cheat_commodity_name);
	ImGui::TextWrapped("Carrying %dt, %dt free", cheat_cargo(commodity), cheat_cargo_free());
	static const int steps[] = {1, 10, 100, -1, -10, -100};
	float w = ui_row_width(3);
	for (int i = 0; i < 6; i++)
	{
		char label[16];
		snprintf(label, sizeof(label), "%+d##cargo%d", steps[i], i);
		if (i % 3)
			ImGui::SameLine();
		if (ui_button(label, w))
			cheat_add_cargo(commodity, steps[i]);
	}

	ui_heading("Elite rating");
	int elite = cheat_elite_rank();
	if (name_combo("##elite", &elite, CHEAT_ELITE_RANKS, [](int i) { return cheat_elite_rank_names[i]; }))
		cheat_set_elite_rank(elite);

	ui_heading("Federal rank");
	int federal = cheat_federal_rank();
	if (name_combo("##federal", &federal, CHEAT_MILITARY_RANKS,
				   [](int i) { return cheat_federal_rank_names[i]; }))
		cheat_set_federal_rank(federal);

	ui_heading("Imperial title");
	int imperial = cheat_imperial_rank();
	if (name_combo("##imperial", &imperial, CHEAT_MILITARY_RANKS,
				   [](int i) { return cheat_imperial_rank_names[i]; }))
		cheat_set_imperial_rank(imperial);

	ui_heading("Legal status");
	if (ui_button("Clear Criminal Record"))
		cheat_clear_criminal_record();

#if FE2_USE_MODDED
	if (custom_ships_new_ships() > 0)
	{
		ui_heading("Custom ships");
		static const char *stock_result = "";
		if (ui_button("Sell Modded Ships In This System"))
			stock_result =
				custom_ships_stock_shipyards() ? "Done: see the shipyard's new ships." : "No starports here.";
		ImGui::TextWrapped("%s", stock_result);
	}
#endif
}

/* Items of one kind for a combo, with "None" first when `none` is set */
struct ItemList
{
	int items[64];
	int count = 0;

	ItemList(EquipKind kind, bool none)
	{
		if (none)
			items[count++] = -1;
		for (int i = 0; i < cheat_equipment_count() && count < 64; i++)
			if (cheat_equipment_kind(i) == kind)
				items[count++] = i;
	}

	int index_of(int item) const
	{
		for (int i = 0; i < count; i++)
			if (items[i] == item)
				return i;
		return 0;
	}

	/* Combo of the items; returns the picked item or -2 if unchanged */
	int combo(const char *id, int current) const
	{
		int index = index_of(current);
		const int *list = items;
		if (!name_combo(id, &index, count, [list](int i) { return cheat_equipment_name(list[i]); }))
			return -2;
		return items[index];
	}
};

void ship_cheats()
{
	if (!cheat_have_ship())
	{
		ImGui::TextWrapped("No ship yet.");
		return;
	}

	float w = ui_row_width(2);
	if (ui_button("Refuel", w))
		cheat_refuel();
	ImGui::SameLine();
	if (ui_button("Repair", w))
		cheat_repair();
	wrapped_checkbox("Ignore cargo space", &cheat_ignore_space);
	ImGui::TextWrapped("%dt cargo space free", cheat_cargo_free());

	ui_heading("Drive");
	static const ItemList drives(EQUIP_DRIVE, false);
	int drive = drives.combo("##drive", cheat_drive());
	if (drive >= 0)
		report(cheat_fit_drive(drive));

	static const ItemList lasers(EQUIP_LASER, true);
	for (int m = 0; m < cheat_gun_mounts(); m++)
	{
		ui_heading(cheat_gun_mount_name(m));
		char id[24];
		snprintf(id, sizeof(id), "##laser%d", m);
		int laser = lasers.combo(id, cheat_laser(m));
		if (laser != -2)
			report(cheat_fit_laser(m, laser));
	}

	ui_heading("Shield generators");
	ImGui::TextWrapped("%d fitted", cheat_shields());
	if (ui_button("+1##shield", w))
		report(cheat_add_shields(1));
	ImGui::SameLine();
	if (ui_button("-1##shield", w))
		report(cheat_add_shields(-1));

	ui_heading("Missiles");
	ImGui::TextWrapped("%d of %d pylons armed", cheat_missiles(), cheat_pylons());
	static const ItemList missiles(EQUIP_MISSILE, true);
	static int missile = 3; /* first entry is None */
	name_combo("##missile", &missile, missiles.count,
			   [](int i) { return cheat_equipment_name(missiles.items[i]); });
	if (ui_button("Fill All Pylons"))
		report(cheat_fill_pylons(missiles.items[missile]));

	ui_heading("Equipment");
	for (int i = 0; i < cheat_equipment_count(); i++)
	{
		if (cheat_equipment_kind(i) != EQUIP_FLAG)
			continue;
		bool fitted = cheat_has_flag(i);
		ImGui::PushID(i);
		bool toggled = wrapped_checkbox(cheat_equipment_name(i), &fitted);
		ImGui::PopID();
		if (toggled)
			report(cheat_toggle_flag(i));
	}
}

void side_more_cheats()
{
	static bool ships = false;
	if (ui_button(ships ? "Show Player Cheats" : "Show Ship Cheats"))
	{
		ships = !ships;
		cheat_message = nullptr;
	}
	ImGui::Separator();
	if (ships)
		ship_cheats();
	else
		player_cheats();
	if (cheat_message)
	{
		ImGui::Separator();
		ImGui::TextWrapped("%s", cheat_message);
	}
}

} // namespace

void ui_menu_draw(void)
{
	if (Screen_GetGameWidth() <= 0 || Screen_GetGameHeight() <= 0)
		return;

	ImGui::SetNextWindowPos(game_pos(0, 0));
	ImGui::SetNextWindowSize(game_size(120, 180));
	if (ui_begin_window("EMULATOR MENU"))
	{
		if (page != Page::Home && ui_button("BACK"))
		{
			page = (page == Page::DebugOptions) ? Page::Debug : Page::Home;
			if (page == Page::Home)
				side = Side::None;
		}

		switch (page)
		{
		case Page::Home:
			page_home();
			break;
		case Page::Settings:
			page_settings();
			break;
		case Page::Cheats:
			page_cheats();
			break;
		case Page::Mods:
			page_mods();
			break;
		case Page::Debug:
			page_debug();
			break;
		case Page::DebugOptions:
			page_debug_options();
			break;
		case Page::About:
			page_about();
			break;
		}
	}
	ImGui::End();

	if (side == Side::None)
		return;

	ImGui::SetNextWindowPos(game_pos(120, 0));
	ImGui::SetNextWindowSize(game_size(170, 180));
	if (ui_begin_window(side == Side::Console ? "CONSOLE" : "MORE CHEATS"))
	{
		if (side == Side::Console)
			side_console();
		else
			side_more_cheats();
	}
	ImGui::End();
}

void ui_freecam_overlay(void)
{
	if (Screen_GetGameWidth() <= 0 || Screen_GetGameHeight() <= 0)
		return;
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoResize |
								   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
								   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
	/* below the row of touch buttons (freecam.c: 9% of the height + margins) */
	float top = toggle_touch_controls ? 27.0f : 2.0f;
	ImGui::SetNextWindowPos(game_pos(160, top), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
	ImGui::SetNextWindowSize(game_size(200, 0));
	ImGui::SetNextWindowBgAlpha(0.6f);
	if (ImGui::Begin("FREE CAMERA - GAME PAUSED", nullptr, flags))
	{
		int level = freecam_speed_level();
		if (level >= 0)
			ImGui::Text("Speed x%d", 1 << (level > 30 ? 30 : level));
		else
			ImGui::Text("Speed 1/%d", 1 << -level);
		ImGui::TextWrapped("WASD move, R/F up/down, arrows or mouse drag look, Q/E roll, "
						   "+/- or wheel speed, Shift fast, H hide this, Esc or Ctrl-V exit");
	}
	ImGui::End();
}
