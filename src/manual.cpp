/*
 * manual.cpp - see manual.h. The pages, their rendering and the worker
 * thread are vibe-lib's (vibe/manual.h); this builds the controls pages
 * from the bindings and opens the PDF.
 */
#include <algorithm>
#include <atomic>
#include <cmath>
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
#include "gamepad.h"
#include "main.h"
#include "vr.h"
}
#include "manual.h"

const char *manual_arg;

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

/* The manual is a scan of two-page spreads: each wide page is shown a half
 * at a time (0 left, 1 right) so its text is bigger. Wraps a PDF so that
 * its pages are those halves (and the whole of the narrow ones). */
class SpreadPdf : public vibe::PdfDocument
{
  public:
	explicit SpreadPdf(std::unique_ptr<vibe::PdfDocument> doc) : doc_(std::move(doc))
	{
		for (int i = 0; i < doc_->pages(); i++)
		{
			double w = 0, h = 0;
			if (doc_->page_size(i, &w, &h) && w > h * 1.2)
			{
				parts_.push_back({i, 0});
				parts_.push_back({i, 1});
				spreads_++;
			}
			else
				parts_.push_back({i, -1});
		}
	}
	int pages() const override { return int(parts_.size()); }
	int document_pages() const { return doc_->pages(); }
	int spreads() const { return spreads_; }

	bool page_size(int index, double *w, double *h) override
	{
		const Part p = parts_[size_t(index)];
		if (!doc_->page_size(p.page, w, h))
			return false;
		if (p.half >= 0)
			*w /= 2;
		return true;
	}

	bool render(int index, double dpi, int x, int y, int w, int h, std::vector<uint32_t> *out, int *out_w,
				int *out_h) override
	{
		const Part p = parts_[size_t(index)];
		if (p.half < 0)
			return doc_->render(p.page, dpi, x, y, w, h, out, out_w, out_h);
		double pw = 0, ph = 0;
		if (!doc_->page_size(p.page, &pw, &ph))
			return false;
		const int half_px = int(std::lround(pw * dpi / 72 / 2));
		if (w <= 0)
			x = 0, y = 0, w = half_px, h = int(std::ceil(ph * dpi / 72));
		return doc_->render(p.page, dpi, x + (p.half ? half_px : 0), y, w, h, out, out_w, out_h);
	}

  private:
	struct Part
	{
		int page, half;
	};
	std::unique_ptr<vibe::PdfDocument> doc_;
	std::vector<Part> parts_;
	int spreads_ = 0;
};

vibe::Manual *manual;
std::atomic<int> page{0};

bool open_pdf(const std::string &path)
{
#ifdef GLF_HAVE_POPPLER
	std::unique_ptr<vibe::PdfDocument> doc = vibe::open_poppler_pdf(path);
	if (!doc)
	{
		log_printf("Manual: can't read %s\n", path.c_str());
		return false;
	}
	auto spread = std::make_unique<SpreadPdf>(std::move(doc));
	log_printf("Manual: %s (%d pages, %d of them two-page spreads, shown a half at a time)\n", path.c_str(),
			   spread->document_pages(), spread->spreads());
	manual->set_pdf(std::move(spread));
	return true;
#else
	log_printf("Manual: this build can't read PDFs (no poppler-cpp): %s left out\n", path.c_str());
	return false;
#endif
}

/* ---- the controls pages ------------------------------------------------- */
const char LEGEND[] = "S1 / S2: hold shift 1 / 2   M: while the clipboard is out";

/* The bindings as seen on one device: the pad's names, or the VR
 * controllers' (bindings they can't reach left out) */
vibe::Section device_section(bool vr)
{
	vibe::Section s;
	s.title = vr ? "VR motion controllers" : "Gamepad (Xbox layout)";
	s.lines = {LEGEND};
	if (vr)
	{
		s.lines.push_back("Hands: touch the panel with a fingertip; take the clipboard with a grip at it.");
		s.lines.push_back("Both grips held: move the hands forward / back to move the panel.");
	}
	s.lines.push_back("");
	bool any = false;
	for (int i = 0; i < gamepad_help_count(); i++)
	{
		GamepadHelp h;
		gamepad_help(i, &h);
		const char *control = vr ? h.vr_control : h.control;
		if (!control[0])
			continue;
		if (h.gap && any)
			s.lines.push_back("");
		s.lines.push_back(row(control, h.action, h.note));
		any = true;
	}
	return s;
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
	for (int i = 0; i < gamepad_help_count(); i++)
	{
		GamepadHelp h;
		gamepad_help(i, &h);
		if (!h.key || !h.note[0])
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

void manual_init(void)
{
	if (manual)
		return;
	manual = new vibe::Manual;
	std::atexit(manual_shutdown); /* stops the page thread */
	manual->set_font(make_font());
	/* the device in use first */
	for (int k = 0; k < 2; k++)
		manual->add_section(device_section(vr_enabled ? k == 0 : k == 1));
	manual->add_section(keyboard_section());
	std::string pdf = vibe::find_pdf(manual_arg ? manual_arg : "", {"."});
	if (!pdf.empty())
		open_pdf(pdf);
	else
		log_printf("Manual: no PDF (put one in the game folder, or --manual FILE): controls pages only\n");
	manual_set_page(page);
}

void manual_shutdown(void)
{
	delete manual;
	manual = nullptr;
}

int manual_pages(void)
{
	return manual ? manual->pages() : 0;
}

int manual_page(void)
{
	return page;
}

void manual_set_page(int p)
{
	if (!manual)
	{
		page = std::max(p, 0);
		return;
	}
	p = std::clamp(p, 0, std::max(manual->pages() - 1, 0));
	page = p;
#if !defined(__EMSCRIPTEN__) /* (no threads there: manual_image renders it) */
	manual->request(p);
#endif
}

void manual_turn(int n)
{
	if (manual && n)
		manual_set_page(page + n);
}

std::shared_ptr<const ManualImage> manual_image(int p)
{
	if (!manual)
		return nullptr;
#if defined(__EMSCRIPTEN__)
	static std::shared_ptr<const ManualImage> last;
	if (!last || last->page != p)
		last = manual->render(p);
	return last;
#else
	return manual->get(p);
#endif
}
