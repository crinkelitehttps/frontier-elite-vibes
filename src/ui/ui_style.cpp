/*
 * ui_style.cpp - Frontier look for Dear ImGui, see ui_style.h.
 */
#include "imgui.h"
#include "imgui_internal.h" /* FindRenderedTextEnd */
#include "ui_style.h"
#include "screen_text.h"

extern "C"
{
#include "main.h" /* toggle_touch_controls */
}

namespace
{

int px_scale = 0; /* screen pixels per game pixel, 0 = not built yet */

/* Colours taken from the game's palettes (ST rgb444 values in comments) */
ImVec4 rgb444(int c, float a = 1.0f)
{
	return ImVec4(((c >> 8) & 0xf) / 15.0f, ((c >> 4) & 0xf) / 15.0f, (c & 0xf) / 15.0f, a);
}

const int COL_SPACE = 0x002;      /* space background */
const int COL_PANEL_DARK = 0x444; /* control panel shades */
const int COL_PANEL = 0x666;
const int COL_PANEL_LIGHT = 0x999;
const int COL_HIGHLIGHT = 0xddd;
const int COL_TEXT = 0xddd;
const int COL_ORANGE = 0xf80; /* HUD labels */
const int COL_RED = 0xe00;    /* scanner / toggles */

ImU32 u32(int c)
{
	return ImGui::GetColorU32(rgb444(c));
}

/* Adds the game's font to the atlas: one custom glyph per character, drawn
 * pixel for pixel at `px` times size. The default font is kept underneath
 * as a fallback for characters the game font lacks. */
void build_font(int px)
{
	ImGuiIO &io = ImGui::GetIO();
	io.Fonts->Clear();

	ImFontConfig cfg;
	cfg.SizePixels = 10.0f * px; /* the game's line height is 10 pixels */
	ImFont *font = io.Fonts->AddFontDefault(&cfg);

	int ids[128];
	for (int ch = 0; ch < 128; ch++)
	{
		ids[ch] = -1;
		const unsigned char *g = screen_text_glyph(ch);
		if (!g || ch > 0x7e)
			continue;
		/* 1 pixel of space above: glyph rows 1..8 of a 10 row line */
		ids[ch] = io.Fonts->AddCustomRectFontGlyph(font, (ImWchar)ch, 8 * px, 8 * px, (float)(g[9] * px),
												   ImVec2(0, (float)px));
	}
	io.Fonts->Build();

	unsigned char *pixels;
	int tex_w, tex_h;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &tex_w, &tex_h);
	unsigned int *tex = (unsigned int *)pixels;
	for (int ch = 0; ch < 128; ch++)
	{
		if (ids[ch] < 0)
			continue;
		const ImFontAtlasCustomRect *r = io.Fonts->GetCustomRectByIndex(ids[ch]);
		const unsigned char *g = screen_text_glyph(ch);
		for (int y = 0; y < r->Height; y++)
		{
			unsigned int *row = tex + (r->Y + y) * tex_w + r->X;
			unsigned char bits = g[y / px];
			for (int x = 0; x < r->Width; x++)
				row[x] = (bits & (0x80 >> (x / px))) ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 0);
		}
	}
}

void build_style(int px)
{
	ImGuiStyle &s = ImGui::GetStyle();
	s = ImGuiStyle();
	const float p = (float)px;

	/* square, pixel sized metrics like the game's panels */
	s.WindowRounding = s.ChildRounding = s.FrameRounding = s.PopupRounding = 0;
	s.ScrollbarRounding = s.GrabRounding = s.TabRounding = 0;
	s.WindowBorderSize = p;
	s.FrameBorderSize = p;
	s.PopupBorderSize = p;
	s.ChildBorderSize = 0;
	s.WindowPadding = ImVec2(4 * p, 4 * p);
	s.FramePadding = ImVec2(3 * p, 2 * p);
	s.ItemSpacing = ImVec2(2 * p, 2 * p);
	s.ItemInnerSpacing = ImVec2(2 * p, 2 * p);
	s.ScrollbarSize = 5 * p; /* ui_style_update widens it for touch */
	s.GrabMinSize = 5 * p;
	s.WindowTitleAlign = ImVec2(0.5f, 0.5f);
	s.WindowMinSize = ImVec2(8 * p, 8 * p);
	s.AntiAliasedLines = s.AntiAliasedFill = false; /* crisp pixels */

	ImVec4 *c = s.Colors;
	c[ImGuiCol_Text] = rgb444(COL_TEXT);
	c[ImGuiCol_TextDisabled] = rgb444(COL_PANEL_LIGHT);
	c[ImGuiCol_WindowBg] = rgb444(COL_SPACE, 0.94f);
	c[ImGuiCol_ChildBg] = rgb444(COL_SPACE, 0.0f);
	c[ImGuiCol_PopupBg] = rgb444(COL_SPACE, 0.98f);
	c[ImGuiCol_Border] = rgb444(COL_PANEL_LIGHT);
	c[ImGuiCol_BorderShadow] = rgb444(0x000, 0.0f);
	c[ImGuiCol_FrameBg] = rgb444(0x113);
	c[ImGuiCol_FrameBgHovered] = rgb444(0x225);
	c[ImGuiCol_FrameBgActive] = rgb444(0x336);
	c[ImGuiCol_TitleBg] = rgb444(COL_PANEL_DARK);
	c[ImGuiCol_TitleBgActive] = rgb444(COL_PANEL_DARK);
	c[ImGuiCol_TitleBgCollapsed] = rgb444(COL_PANEL_DARK);
	c[ImGuiCol_ScrollbarBg] = rgb444(0x113);
	c[ImGuiCol_ScrollbarGrab] = rgb444(COL_PANEL);
	c[ImGuiCol_ScrollbarGrabHovered] = rgb444(COL_PANEL_LIGHT);
	c[ImGuiCol_ScrollbarGrabActive] = rgb444(COL_HIGHLIGHT);
	c[ImGuiCol_CheckMark] = rgb444(COL_RED);
	c[ImGuiCol_SliderGrab] = rgb444(COL_RED);
	c[ImGuiCol_SliderGrabActive] = rgb444(0xf44);
	c[ImGuiCol_Button] = rgb444(COL_PANEL);
	c[ImGuiCol_ButtonHovered] = rgb444(COL_PANEL_LIGHT);
	c[ImGuiCol_ButtonActive] = rgb444(COL_PANEL_DARK);
	c[ImGuiCol_Header] = rgb444(COL_PANEL);
	c[ImGuiCol_HeaderHovered] = rgb444(COL_PANEL_LIGHT);
	c[ImGuiCol_HeaderActive] = rgb444(COL_PANEL_DARK);
	c[ImGuiCol_Separator] = rgb444(COL_ORANGE);
	c[ImGuiCol_SeparatorHovered] = rgb444(COL_ORANGE);
	c[ImGuiCol_SeparatorActive] = rgb444(COL_ORANGE);
	c[ImGuiCol_NavHighlight] = rgb444(0x1f2); /* the cockpit focus box's green */
	/* the rest ImGui would draw in its own blue (tabs, tables, selections:
	 * the galaxy atlas uses them) */
	c[ImGuiCol_Tab] = rgb444(COL_PANEL_DARK);
	c[ImGuiCol_TabHovered] = rgb444(COL_PANEL_LIGHT);
	c[ImGuiCol_TabSelected] = rgb444(COL_PANEL);
	c[ImGuiCol_TabSelectedOverline] = rgb444(COL_ORANGE);
	c[ImGuiCol_TabDimmed] = rgb444(COL_PANEL_DARK);
	c[ImGuiCol_TabDimmedSelected] = rgb444(COL_PANEL);
	c[ImGuiCol_TabDimmedSelectedOverline] = rgb444(COL_ORANGE);
	c[ImGuiCol_TableHeaderBg] = rgb444(COL_PANEL_DARK);
	c[ImGuiCol_TableBorderStrong] = rgb444(COL_PANEL_LIGHT);
	c[ImGuiCol_TableBorderLight] = rgb444(COL_PANEL_DARK);
	c[ImGuiCol_TableRowBg] = rgb444(COL_SPACE, 0.0f);
	c[ImGuiCol_TableRowBgAlt] = rgb444(0x113, 0.6f);
	c[ImGuiCol_TextSelectedBg] = rgb444(0x448);
	c[ImGuiCol_MenuBarBg] = rgb444(COL_PANEL_DARK);
	c[ImGuiCol_ResizeGrip] = rgb444(COL_PANEL);
	c[ImGuiCol_ResizeGripHovered] = rgb444(COL_PANEL_LIGHT);
	c[ImGuiCol_ResizeGripActive] = rgb444(COL_HIGHLIGHT);
	c[ImGuiCol_DragDropTarget] = rgb444(COL_ORANGE);
	c[ImGuiCol_PlotLines] = rgb444(COL_ORANGE);
	c[ImGuiCol_PlotHistogram] = rgb444(COL_ORANGE);
	c[ImGuiCol_ModalWindowDimBg] = rgb444(0x000, 0.6f);
}

} // namespace

bool ui_style_update(int px)
{
	if (px < 1)
		px = 1;
	bool changed = px != px_scale;
	if (changed)
	{
		px_scale = px;
		build_font(px);
		build_style(px);
	}
	/* touch mode can start at any time: a finger needs a wider scroll bar */
	ImGui::GetStyle().ScrollbarSize = (float)((toggle_touch_controls ? 9 : 5) * px);
	return changed;
}

float ui_style_px(void)
{
	return (float)(px_scale > 0 ? px_scale : 1);
}

float ui_row_width(int count)
{
	float spacing = ImGui::GetStyle().ItemSpacing.x;
	return (ImGui::GetContentRegionAvail().x - spacing * (count - 1)) / count;
}

bool ui_button(const char *label, float width)
{
	const float p = ui_style_px();
	ImVec2 size(width > 0.0f ? width : ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight());
	ImVec2 a = ImGui::GetCursorScreenPos();
	ImVec2 b(a.x + size.x, a.y + size.y);

	/* EnableNav: an invisible button is left out of keyboard / pad navigation otherwise */
	bool pressed = ImGui::InvisibleButton(label, size, ImGuiButtonFlags_EnableNav);
	bool held = ImGui::IsItemActive();
	bool hover = ImGui::IsItemHovered();
	const bool nav = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;

	/* grey face with a one pixel bevel: light top/left, dark bottom/right,
	 * swapped while pressed, like the game's control panel buttons */
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(a, b, u32(hover && !held ? COL_PANEL_LIGHT : COL_PANEL));
	ImU32 light = u32(COL_HIGHLIGHT), dark = u32(0x222);
	ImU32 tl = held ? dark : light, br = held ? light : dark;
	dl->AddRectFilled(a, ImVec2(b.x, a.y + p), tl);
	dl->AddRectFilled(a, ImVec2(a.x + p, b.y), tl);
	dl->AddRectFilled(ImVec2(a.x, b.y - p), b, br);
	dl->AddRectFilled(ImVec2(b.x - p, a.y), b, br);

	/* label with the game's drop shadow, nudged down when pressed */
	const char *end = ImGui::FindRenderedTextEnd(label);
	ImVec2 ts = ImGui::CalcTextSize(label, end);
	float off = held ? p : 0.0f;
	ImVec2 tp((float)(int)(a.x + (size.x - ts.x) * 0.5f + off),
			  (float)(int)(a.y + (size.y - ts.y) * 0.5f + off));
	dl->AddText(ImVec2(tp.x + p, tp.y + p), IM_COL32(0, 0, 0, 255), label, end);
	dl->AddText(tp, IM_COL32_WHITE, label, end);
	if (nav) /* the keyboard / pad focus: a green box, two pixels thick, over the edge */
	{
		const ImU32 green = ImGui::GetColorU32(ImGuiCol_NavHighlight);
		const ImVec2 a2(a.x - p, a.y - p), b2(b.x + p, b.y + p);
		dl->AddRect(a2, b2, green, 0.0f, 0, 2.0f * p);
	}
	return pressed;
}

void ui_heading(const char *text)
{
	ImGui::PushStyleColor(ImGuiCol_Text, rgb444(COL_ORANGE));
	ImGui::TextWrapped("%s", text);
	ImGui::PopStyleColor();
}

bool ui_begin_window(const char *title)
{
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
								   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
	ImGui::PushStyleColor(ImGuiCol_Text, rgb444(COL_ORANGE)); /* title text */
	bool open = ImGui::Begin(title, nullptr, flags);
	ImGui::PopStyleColor();
	return open;
}
