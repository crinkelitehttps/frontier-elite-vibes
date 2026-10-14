// clipboard.h: the cockpit clipboard from the F-19 port: a board image
// (brown board, white paper, metal clip with a page label) holding text
// pages (e.g. the controls) or pages of a PDF manual, and where to put it in
// the cockpit. Header-only C++17, no dependencies; renderer- and
// PDF-library-agnostic (you render PDF pages, it trims, fits and lays them
// out; you draw the quad).
//
// From ~/projects/f19 (native/src/host/manual.cpp and the clipboard quad in
// GlRenderer::render_view, commit 004431e). See README.md beside this file
// and ../../specs/cockpit-clipboard.md.
//
// Images are 0xAARRGGBB words, top row first (GL_BGRA on little-endian).
//
//   std::vector<uint32_t> img;
//   clipboard::draw_board(img, font, "CONTROLS 1/8");
//   clipboard::draw_text_page(img, font, pages[0]);
//   // upload kWidth x kHeight, mipmapped; draw clipboard::corners(reading_board(), kBoardWidth, q)
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace clipboard {

// ------------------------------------------------------------------ layout
// Image size and areas, pixels.
constexpr int kWidth = 2120, kHeight = 1716;
constexpr int kPaperX = 36, kPaperY = 120, kPaperW = kWidth - 2 * kPaperX, kPaperH = kHeight - kPaperY - 36;
constexpr int kClipW = 640, kClipY0 = 30, kClipY1 = 200;
// Where a page goes: below the clip, inside a 24-pixel margin (2000 x 1446).
constexpr int kTextX = kPaperX + 24, kTextY = kClipY1 + 10, kTextW = kPaperW - 48, kTextH = kPaperY + kPaperH - 24 - kTextY;

constexpr uint32_t kBoard = 0xFF7A5230, kBoardEdge = 0xFF4E321C, kPaper = 0xFFFFFFFF, kShadow = 0xFF5A3C22;
constexpr uint32_t kClip = 0xFFB4B8BC, kClipEdge = 0xFF6E7378, kInk = 0xFF202428;
constexpr uint32_t kTitleInk = 0xFF1C3A6E, kStripe = 0xFFEDF0F4;

// Text pages: an 8-pixel-wide bitmap font at 3x (title 4x, clip label 4x).
// With an 8x16 font: 83 columns by 28 lines.
constexpr int kTextScale = 3, kTitleScale = 4, kLabelScale = 4;

// An 8-pixel-wide bitmap font: 256 glyphs of h rows, one byte per row,
// bit 7 = left (a PSF console font's glyph data; F-19 uses kbd's
// default8x16).
struct Font {
    int w = 8, h = 16;
    const uint8_t* glyphs = nullptr;  // 256 * h bytes
};

inline int text_columns(const Font& f) { return kTextW / (f.w * kTextScale); }
inline int text_rows(const Font& f) { return (kTextH - f.h * kTitleScale - f.h * kTextScale / 2) / (f.h * kTextScale); }

// ------------------------------------------------------------------ drawing
inline void fill(std::vector<uint32_t>& img, int x0, int y0, int x1, int y1, uint32_t c) {
    x0 = std::max(x0, 0), y0 = std::max(y0, 0), x1 = std::min(x1, kWidth), y1 = std::min(y1, kHeight);
    for (int y = y0; y < y1; y++) std::fill(&img[size_t(y) * kWidth + x0], &img[size_t(y) * kWidth + x1], c);
}

// `text` with its top-left at (x, y), `s` image pixels per font pixel.
inline void draw_text(std::vector<uint32_t>& img, const Font& font, const std::string& text, int x, int y, int s, uint32_t c) {
    if (!font.glyphs) return;
    for (unsigned char ch : text) {
        for (int r = 0; r < font.h; r++) {
            const uint8_t bits = font.glyphs[ch * font.h + r];
            for (int col = 0; col < 8; col++)
                if (bits & (0x80 >> col)) fill(img, x + col * s, y + r * s, x + (col + 1) * s, y + (r + 1) * s, c);
        }
        x += font.w * s;
    }
}

// The clipboard without a page: board, paper (with a shadow) and the clip
// with `label` centred on it ("CONTROLS 2/8", "MANUAL 29/196"). Resizes
// `img` to kWidth x kHeight.
inline void draw_board(std::vector<uint32_t>& img, const Font& font, const std::string& label) {
    img.assign(size_t(kWidth) * kHeight, kBoardEdge);
    fill(img, 8, 8, kWidth - 8, kHeight - 8, kBoard);
    fill(img, kPaperX + 10, kPaperY + 10, kPaperX + kPaperW + 10, kPaperY + kPaperH + 10, kShadow);
    fill(img, kPaperX, kPaperY, kPaperX + kPaperW, kPaperY + kPaperH, kPaper);
    const int cx0 = (kWidth - kClipW) / 2;
    fill(img, cx0, kClipY0, cx0 + kClipW, kClipY1, kClipEdge);
    fill(img, cx0 + 6, kClipY0 + 6, cx0 + kClipW - 6, kClipY1 - 6, kClip);
    const int lw = int(label.size()) * font.w * kLabelScale, lh = font.h * kLabelScale;
    draw_text(img, font, label, (kWidth - lw) / 2, (kClipY0 + kClipY1 - lh) / 2, kLabelScale, kInk);
}

// ------------------------------------------------------------------ text pages
struct TextPage {
    std::string title;
    std::vector<std::string> lines;  // "" = a blank line
};

// A section: a title and lines (at most text_columns() wide; longer ones
// are cut). An empty line separates groups; a page break avoids splitting
// a group that fits on one page. Continuation pages get " (cont.)".
struct Section {
    std::string title;
    std::vector<std::string> lines;
};

inline void add_section(std::vector<TextPage>& pages, const Section& s, const Font& font) {
    const int rows = text_rows(font);
    std::vector<std::vector<std::string>> groups(1);
    for (const std::string& l : s.lines) {
        if (l.empty()) {
            if (!groups.back().empty()) groups.emplace_back();
        } else {
            groups.back().push_back(l);
        }
    }
    bool first = true;
    auto new_page = [&] {
        pages.push_back(TextPage{first ? s.title : s.title + " (cont.)", {}});
        first = false;
    };
    new_page();
    for (const auto& g : groups) {
        if (g.empty()) continue;
        auto* lines = &pages.back().lines;
        const int used = int(lines->size()), need = int(g.size()) + (used ? 1 : 0);
        if (used && used + need > rows && int(g.size()) <= rows) {
            new_page();
            lines = &pages.back().lines;
        } else if (used) {
            lines->push_back("");
        }
        for (const std::string& l : g) {
            if (int(lines->size()) >= rows) {
                new_page();
                lines = &pages.back().lines;
            }
            lines->push_back(l);
        }
    }
}

// A controls row: three columns at 0, 20 and 38 characters (always at least
// one space between them), e.g. row("S2 L menu", "F8", "change waypoint").
inline std::string row(std::string a, const std::string& b, const std::string& c) {
    a.resize(std::max<size_t>(a.size() + 1, 20), ' ');
    std::string ab = a + b;
    ab.resize(std::max<size_t>(ab.size() + 1, 38), ' ');
    return ab + c;
}

// A text page on the paper (call after draw_board): title, then the lines
// with every other non-blank line on a light stripe.
inline void draw_text_page(std::vector<uint32_t>& img, const Font& font, const TextPage& page) {
    draw_text(img, font, page.title, kTextX, kTextY, kTitleScale, kTitleInk);
    const int lh = font.h * kTextScale, y0 = kTextY + font.h * kTitleScale + lh / 2;
    for (size_t i = 0; i < page.lines.size(); i++) {
        const int y = y0 + int(i) * lh;
        if (i % 2 && !page.lines[i].empty()) fill(img, kTextX - 8, y, kTextX + kTextW + 8, y + lh, kStripe);
        draw_text(img, font, page.lines[i].substr(0, size_t(text_columns(font))), kTextX, y, kTextScale, kInk);
    }
}

// ------------------------------------------------------------------ PDF pages
// Two renders per page with your PDF library (F-19: poppler-cpp, ARGB32):
//   1. the whole page at kProbeDpi -> trim() -> the printed area in points;
//   2. fit(area) -> render that sub-rectangle at fit.dpi -> blit().
constexpr double kProbeDpi = 36;

struct Box {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // points, top-left origin
};

// The printed area of a page from a coarse render (`probe`, w x h pixels,
// `stride` pixels per row, at kProbeDpi): the bounding box of "ink" (darkest
// channel < 200), ignoring the outermost pixel, padded 8 pt and clamped to
// the page (page_w x page_h points). The whole page if nothing is inked.
inline Box trim(const uint32_t* probe, int w, int h, int stride, double page_w, double page_h) {
    int bx0 = w, by0 = h, bx1 = -1, by1 = -1;
    for (int y = 1; y < h - 1; y++) {
        const uint32_t* r = probe + size_t(y) * stride;
        for (int x = 1; x < w - 1; x++) {
            const uint32_t c = r[x];
            if (std::min({c >> 16 & 0xFF, c >> 8 & 0xFF, c & 0xFF}) < 200) {
                bx0 = std::min(bx0, x), bx1 = std::max(bx1, x);
                by0 = std::min(by0, y), by1 = std::max(by1, y);
            }
        }
    }
    Box b{0, 0, page_w, page_h};
    if (bx1 > bx0 && by1 > by0) {
        const double k = 72.0 / kProbeDpi, pad = 8;
        b.x0 = std::max(0.0, bx0 * k - pad), b.x1 = std::min(page_w, (bx1 + 1) * k + pad);
        b.y0 = std::max(0.0, by0 * k - pad), b.y1 = std::min(page_h, (by1 + 1) * k + pad);
    }
    return b;
}

// How to render `area` so it fills the text area: at `dpi`, the pixel
// sub-rectangle (x, y, w, h) of the page, placed centred at (dst_x, dst_y)
// once rendered (blit() does that from the actual image size).
struct Fit {
    double dpi = 72;
    int x = 0, y = 0, w = 0, h = 0;
};

inline Fit fit(const Box& area) {
    const double s = std::min(kTextW / (area.x1 - area.x0), kTextH / (area.y1 - area.y0));  // pixels per point
    Fit f;
    f.dpi = s * 72;
    f.x = int(area.x0 * s), f.y = int(area.y0 * s);
    f.w = std::min(int((area.x1 - area.x0) * s), kTextW), f.h = std::min(int((area.y1 - area.y0) * s), kTextH);
    return f;
}

// Copy a rendered page (w x h, `stride` pixels per row) onto the paper,
// centred in the text area, opaque. Call after draw_board.
inline void blit(std::vector<uint32_t>& img, const uint32_t* src, int w, int h, int stride) {
    w = std::min(w, kTextW), h = std::min(h, kTextH);
    const int ox = kTextX + (kTextW - w) / 2, oy = kTextY + (kTextH - h) / 2;
    for (int y = 0; y < h; y++) {
        const uint32_t* r = src + size_t(y) * stride;
        uint32_t* d = &img[size_t(oy + y) * kWidth + ox];
        for (int x = 0; x < w; x++) d[x] = r[x] | 0xFF000000u;
    }
}

// ------------------------------------------------------------------ placement
// The board in the cockpit frame: centimetres from the neutral eye point,
// x right, y up, z forward (the aircraft frame of the F-19 / F-117A ports;
// flip z for a -Z-forward frame). Centre `c`, unit axes `r` (along the
// text) and `u` (up the page); the front faces u x r.
struct Board {
    float c[3] = {}, r[3] = {1, 0, 0}, u[3] = {0, 1, 0};
};

constexpr float kBoardWidth = 42.0f;  // cm; height 34 (the image's aspect)
inline float board_height(float width = kBoardWidth) { return width * kHeight / kWidth; }

// Reading position: 41.9 cm away, 9 degrees below straight ahead, facing
// the eye, in front of the instrument panel; covers about 53 x 44 degrees.
inline Board reading_board() {
    const float t = 9 * 3.14159265f / 180;
    Board b;
    b.c[0] = 0, b.c[1] = -6.6f, b.c[2] = 41.4f;
    b.u[0] = 0, b.u[1] = std::cos(t), b.u[2] = std::sin(t);
    return b;
}

// Stowed: upright beside the left of the seat at hip height, along the
// aircraft (text runs forward), face to the seat, top leaning 10 degrees
// out: readable when you look down to the left.
inline Board stowed_board() {
    const float t = 10 * 3.14159265f / 180;
    Board b;
    b.c[0] = -28, b.c[1] = -52, b.c[2] = 8;
    b.r[0] = 0, b.r[1] = 0, b.r[2] = 1;
    b.u[0] = -std::sin(t), b.u[1] = std::cos(t), b.u[2] = 0;
    return b;
}

// Seen from behind (eye on the back side): draw the back plain (e.g. map
// every corner to texel (2, 2), the board's edge colour) instead of the
// mirrored page.
inline bool back_facing(const Board& b, const float eye[3]) {
    const float* r = b.r;
    const float* u = b.u;
    const float n[3] = {u[1] * r[2] - u[2] * r[1], u[2] * r[0] - u[0] * r[2], u[0] * r[1] - u[1] * r[0]};
    return n[0] * (eye[0] - b.c[0]) + n[1] * (eye[1] - b.c[1]) + n[2] * (eye[2] - b.c[2]) < 0;
}

// Corners TL, TR, BR, BL: x, y, z, u, v (v = 0 at the image's top row).
inline void corners(const Board& b, float width, float out[4][5]) {
    const float hw = width / 2, hh = board_height(width) / 2;
    const float k[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 3; i++) out[j][i] = b.c[i] + k[j][0] * hw * b.r[i] + k[j][1] * hh * b.u[i];
        out[j][3] = (k[j][0] + 1) / 2;
        out[j][4] = (1 - k[j][1]) / 2;
    }
}

// Flat fallback (menus, flat views, a VR virtual screen): the image centred
// at 96% of the target, aspect kept. Returns NDC half sizes.
inline void flat_extent(int target_w, int target_h, float* half_w, float* half_h) {
    const float s = 0.96f * std::min(float(target_w) / kWidth, float(target_h) / kHeight);
    *half_w = kWidth * s / target_w, *half_h = kHeight * s / target_h;
}

}  // namespace clipboard
