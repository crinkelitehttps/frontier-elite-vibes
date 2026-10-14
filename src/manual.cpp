/*
 * manual.cpp - see manual.h. The page rendering follows the F-19 port's
 * native/src/host/manual.cpp, on the shared clipboard.h.
 */
#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#ifdef GLF_HAVE_POPPLER
#include <poppler-document.h>
#include <poppler-page-renderer.h>
#include <poppler-page.h>
#endif

#include "gl/clipboard.h"

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

const clipboard::Font font{8, 16, clipboard_font_8x16};

struct Manual
{
	std::vector<clipboard::TextPage> text;
#ifdef GLF_HAVE_POPPLER
	std::unique_ptr<poppler::document> doc;
#endif
	int pdf_pages = 0; /* the document's */
	/* the clipboard's manual pages: PDF page and half (-1 the whole page;
	 * a two-page spread is split, 0 left, 1 right, so its text is bigger) */
	struct Part
	{
		int page, half;
	};
	std::vector<Part> parts;
	std::mutex mu;
	std::condition_variable cv;
	std::map<int, std::shared_ptr<const ManualImage>> cache;
	int want = -1;
	bool stop = false;
	std::thread worker;

	int pages() const { return int(text.size()) + int(parts.size()); }

	/* The page asked for first, then its neighbours; far ones dropped */
	void run()
	{
		std::unique_lock<std::mutex> lk(mu);
		while (!stop)
		{
			int target = -1;
			for (int d : {0, 1, -1, 2})
			{
				int pg = want + d;
				if (want >= 0 && pg >= 0 && pg < pages() && !cache.count(pg))
				{
					target = pg;
					break;
				}
			}
			if (target < 0)
			{
				cv.wait(lk);
				continue;
			}
			lk.unlock();
			auto img = render(target);
			lk.lock();
			cache[target] = std::move(img);
			for (auto it = cache.begin(); it != cache.end();)
				it = std::abs(it->first - want) > 3 ? cache.erase(it) : std::next(it);
		}
	}

	std::shared_ptr<const ManualImage> render(int page) const
	{
		auto img = std::make_shared<ManualImage>();
		img->page = page;
		const int nt = int(text.size());
		if (page < nt)
		{
			clipboard::draw_board(img->argb, font, "CONTROLS " + std::to_string(page + 1) + "/" + std::to_string(nt));
			clipboard::draw_text_page(img->argb, font, text[page]);
			return img;
		}
		const Part part = parts[size_t(page - nt)];
		clipboard::draw_board(img->argb, font,
							  "MANUAL " + std::to_string(part.page + 1) + (part.half < 0 ? "" : part.half ? "R" : "L") +
								  "/" + std::to_string(pdf_pages));
#ifdef GLF_HAVE_POPPLER
		std::unique_ptr<poppler::page> pg(doc->create_page(part.page));
		if (!pg)
			return img;
		poppler::page_renderer r;
		r.set_render_hints(poppler::page_renderer::antialiasing | poppler::page_renderer::text_antialiasing |
						   poppler::page_renderer::text_hinting);
		r.set_image_format(poppler::image::format_argb32);
		const poppler::rectf box = pg->page_rect();
		if (box.width() <= 0 || box.height() <= 0)
			return img;
		/* the printed area (of the half), from a coarse render, fitted to
		 * the paper */
		const double hw = part.half < 0 ? box.width() : box.width() / 2, hx = part.half > 0 ? hw : 0;
		clipboard::Box area{hx, 0, hx + hw, box.height()};
		poppler::image probe = r.render_page(pg.get(), clipboard::kProbeDpi, clipboard::kProbeDpi);
		if (probe.is_valid() && probe.format() == poppler::image::format_argb32)
		{
			const int px = int(hx * clipboard::kProbeDpi / 72), pw = part.half < 0 ? probe.width() : probe.width() / 2;
			area = clipboard::trim((const uint32_t *)probe.const_data() + px, pw, probe.height(),
								   probe.bytes_per_row() / 4, hw, box.height());
			area.x0 += hx;
			area.x1 += hx;
		}
		const clipboard::Fit f = clipboard::fit(area);
		poppler::image pi = r.render_page(pg.get(), f.dpi, f.dpi, f.x, f.y, f.w, f.h);
		if (pi.is_valid() && pi.format() == poppler::image::format_argb32)
			clipboard::blit(img->argb, (const uint32_t *)pi.const_data(), pi.width(), pi.height(),
							pi.bytes_per_row() / 4);
#endif
		return img;
	}

	~Manual()
	{
		{
			std::lock_guard<std::mutex> lk(mu);
			stop = true;
		}
		cv.notify_all();
		if (worker.joinable())
			worker.join();
	}
};

Manual *manual;
std::atomic<int> page{0};

/* The PDF: --manual FILE, else the first *.pdf here (by name) */
std::string find_pdf()
{
	if (manual_arg)
		return manual_arg;
	namespace fs = std::filesystem;
	std::vector<fs::path> pdfs;
	std::error_code ec;
	for (const auto &e : fs::directory_iterator(".", ec))
	{
		std::string ext = e.path().extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		if (ext == ".pdf" && e.is_regular_file(ec))
			pdfs.push_back(e.path());
	}
	std::sort(pdfs.begin(), pdfs.end());
	return pdfs.empty() ? std::string() : pdfs.front().string();
}

bool open_pdf(const std::string &path)
{
#ifdef GLF_HAVE_POPPLER
	std::unique_ptr<poppler::document> doc(poppler::document::load_from_file(path));
	if (!doc || doc->is_locked() || doc->pages() <= 0)
	{
		log_printf("Manual: can't read %s\n", path.c_str());
		return false;
	}
	manual->pdf_pages = doc->pages();
	int spreads = 0;
	for (int i = 0; i < manual->pdf_pages; i++)
	{
		std::unique_ptr<poppler::page> pg(doc->create_page(i));
		const poppler::rectf box = pg ? pg->page_rect() : poppler::rectf();
		if (box.width() > box.height() * 1.2)
		{
			manual->parts.push_back({i, 0});
			manual->parts.push_back({i, 1});
			spreads++;
		}
		else
			manual->parts.push_back({i, -1});
	}
	manual->doc = std::move(doc);
	log_printf("Manual: %s (%d pages, %d of them two-page spreads, shown a half at a time)\n", path.c_str(),
			   manual->pdf_pages, spreads);
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
clipboard::Section device_section(bool vr)
{
	clipboard::Section s;
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
		s.lines.push_back(clipboard::row(control, h.action, h.note));
		any = true;
	}
	return s;
}

clipboard::Section keyboard_section()
{
	using clipboard::row;
	clipboard::Section s;
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
	manual = new Manual;
	std::atexit(manual_shutdown); /* stops the page thread */
	/* the device in use first */
	for (int k = 0; k < 2; k++)
		clipboard::add_section(manual->text, device_section(vr_enabled ? k == 0 : k == 1), font);
	clipboard::add_section(manual->text, keyboard_section(), font);
	std::string pdf = find_pdf();
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
	{
		std::lock_guard<std::mutex> lk(manual->mu);
		manual->want = p;
	}
#if defined(__EMSCRIPTEN__)
	/* no threads: render it here */
	if (!manual_image(p))
	{
		auto img = manual->render(p);
		std::lock_guard<std::mutex> lk(manual->mu);
		manual->cache.clear();
		manual->cache[p] = std::move(img);
	}
#else
	if (!manual->worker.joinable())
		manual->worker = std::thread([] { manual->run(); });
	manual->cv.notify_all();
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
	std::lock_guard<std::mutex> lk(manual->mu);
	auto it = manual->cache.find(p);
	return it != manual->cache.end() ? it->second : nullptr;
}
