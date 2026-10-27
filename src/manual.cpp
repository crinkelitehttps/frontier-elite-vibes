/*
 * manual.cpp - see manual.h. The pages, their rendering, the page being
 * read and the worker thread are vibe-lib's (vibe/cockpit_clipboard.h);
 * this builds the controls pages from the bindings and opens the PDF.
 */
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef GLF_HAVE_POPPLER
#include "vibe/poppler.h"
#endif

extern "C"
{
#include "clipboard_font.h"
#include "main.h"
#include "vr.h"
}
#include "manual.h"
#include "gamepad.h"
#include "vibe/bindings.h"

const char *manual_arg;
int manual_book = 1;

namespace
{

using vibe::row;

vibe::Font make_font()
{
	vibe::Font f;
	f.w = 8;
	f.h = 16;
	f.glyphs.assign(clipboard_font_8x16, clipboard_font_8x16 + sizeof clipboard_font_8x16);
	return f;
}

vibe::Clipboard board;

bool open_pdf(const std::string &path)
{
#ifdef GLF_HAVE_POPPLER
	std::unique_ptr<vibe::PdfDocument> doc = vibe::open_poppler_pdf(path);
	if (!doc)
	{
		log_printf("Manual: can't read %s\n", path.c_str());
		return false;
	}
	const int pages = doc->pages();
	if (manual_book)
	{
		/* an open book: two pages to the board, a scanned spread whole */
		board.manual.set_book(true);
		board.manual.set_pdf(std::move(doc));
		log_printf("Manual: %s (%d pages, as a book: %d boards)\n", path.c_str(), pages,
				   board.manual.pages() - board.manual.text_pages());
		return true;
	}
	/* a scan of two-page spreads: each shown a half at a time */
	int spreads = 0;
	board.manual.set_pdf(vibe::split_spreads(std::move(doc), &spreads));
	log_printf("Manual: %s (%d pages, %d of them two-page spreads, shown a half at a time)\n", path.c_str(), pages,
			   spreads);
	return true;
#else
	log_printf("Manual: this build can't read PDFs (no poppler-cpp): %s left out\n", path.c_str());
	return false;
#endif
}

/* ---- the controls pages ------------------------------------------------- */
/* The bindings as seen on one device (the library's pages), the VR one
 * with a word on the hands */
vibe::Section device_section(bool vr)
{
	const std::string title = vr ? "VR motion controllers" : "Gamepad (Xbox layout)";
	for (const vibe::Section &s : gamepad_bindings().sections())
		if (s.title == title)
		{
			vibe::Section out = s;
			if (vr)
				out.lines.insert(out.lines.begin() + 1,
								 {"Hands: touch the panel with a fingertip; take the clipboard with a grip at it.",
								  "Both grips held: move the hands forward / back to move the panel."});
			return out;
		}
	vibe::Section none;
	none.title = title;
	none.lines = {"No bindings for these controllers."};
	return none;
}

vibe::Section keyboard_section()
{
	vibe::Section s;
	s.title = "Keyboard";
	s.lines = {
		row("F11", "clipboard: up / put away", ""),
		row("PgUp / PgDn", "previous / next page", "with Shift: 10 pages"),
		row("Home / End", "first / last page", ""),
		row("Mouse wheel", "previous / next page", ""),
		"The page keys and the wheel turn pages only while it is out.",
		"",
		row("Ctrl+K", "cockpit view on / off", ""),
		row("Ctrl+C", "recentre head tracking / VR", ""),
		row("Ctrl+F", "menu", ""),
		row("Ctrl+F11", "fullscreen", ""),
		row("Ctrl+M", "mouse grab", ""),
		row("Ctrl+E", "renderer", ""),
		row("Ctrl+Q", "quit", ""),
	};
	/* the game's keys the controller presses, with what they do */
	std::vector<std::string> keys;
	for (const vibe::Bindings::Help &h : gamepad_bindings().help())
	{
		if (!h.key || h.note.empty())
			continue;
		std::string l = row(h.action, h.note, "");
		while (!l.empty() && l.back() == ' ')
			l.pop_back();
		if (std::find(keys.begin(), keys.end(), l) == keys.end())
			keys.push_back(l);
	}
	if (!keys.empty())
	{
		s.lines.push_back("");
		s.lines.push_back("Game keys used by the controller bindings:");
		s.lines.insert(s.lines.end(), keys.begin(), keys.end());
	}
	return s;
}

} /* namespace */

vibe::Clipboard &clipboard()
{
	return board;
}

void manual_init(void)
{
	if (board.started())
		return;
	vibe::Manual &manual = board.manual;
	manual.set_font(make_font());
	/* the device in use first */
	for (int k = 0; k < 2; k++)
		manual.add_section(device_section(vr_enabled ? k == 0 : k == 1));
	manual.add_section(keyboard_section());
	std::string pdf = vibe::find_pdf(manual_arg ? manual_arg : "", {"."});
	if (!pdf.empty())
		open_pdf(pdf);
	else
		log_printf("Manual: no PDF (put one in the game folder, or --manual FILE): controls pages only\n");
#if defined(__EMSCRIPTEN__) /* no threads: drawing renders the page */
	board.render_here = true;
#endif
	board.start();
}

int manual_pages(void)
{
	return board.pages();
}

int manual_page(void)
{
	return board.page();
}

void manual_set_page(int p)
{
	board.set_page(p);
}

void manual_turn(int n)
{
	board.turn(n);
}
