/*
 * quiz_buttons.c - see quiz_buttons.h.
 */
#include "quiz_buttons.h"
#include "game_state.h"
#include "input.h"
#include "screen_text.h"

#define SCR_W 320
#define RIGHT_X 316 /* the bottom right of the station screen, above the panel */
#define BOTTOM_Y 165
#define ROW_PITCH 12
#define TEXT_GAP 3 /* from the square's right edge to its text */

/* The station list's button (L78c0c, 16x11, the game's palette: 0 shadow,
 * 14 face / background, 15 highlight); its square is columns 0..10 */
static const char *const SQUARE[11] = {
	"effffffffeeeeeee", "efeeeeee00eeeeee", "efeeeeee000eeeee", "efeeeeee000eeeee",
	"efeeeeee000eeeee", "efeeeeee000eeeee", "efeeeeee000eeeee", "ef000000000eeeee",
	"ee000000000eeeee", "eee00000000eeeee", "eeeeeeeeeeeeeeee",
};
#define SQUARE_W 11
#define SQUARE_H 11
#define TEXT_COLOUR 12 /* the station palette's white */

static QuizButton buttons[QUIZ_BUTTONS] = {
	{0, 0, 0, 0, "PRESS P", 0x19},
	{0, 0, 0, 0, "PRESS OTHER KEY", 0x1e}, /* A */
};
static bool laid_out;

static int text_width(const char *s)
{
	int w = 0;
	for (; *s; s++)
	{
		const unsigned char *g = screen_text_glyph(*s);
		w += g ? g[9] : 4;
	}
	return w;
}

const QuizButton *quiz_buttons(void)
{
	if (!laid_out)
	{
		/* one column, right aligned on the widest label, the last row at
		 * the bottom */
		int widest = 0;
		for (int i = 0; i < QUIZ_BUTTONS; i++)
			if (text_width(buttons[i].label) > widest)
				widest = text_width(buttons[i].label);
		int x0 = RIGHT_X - (SQUARE_W + TEXT_GAP + widest);
		for (int i = 0; i < QUIZ_BUTTONS; i++)
		{
			buttons[i].x0 = x0;
			buttons[i].x1 = x0 + SQUARE_W + TEXT_GAP + text_width(buttons[i].label);
			buttons[i].y1 = BOTTOM_Y - (QUIZ_BUTTONS - 1 - i) * ROW_PITCH;
			buttons[i].y0 = buttons[i].y1 - (SQUARE_H - 1);
		}
		laid_out = true;
	}
	return buttons;
}

static void put(unsigned int *pixels, int x, int y, unsigned int rgb)
{
	if (x >= 0 && x < SCR_W && y >= 0 && y < 168)
		pixels[y * SCR_W + x] = rgb;
}

void quiz_buttons_draw(unsigned int *pixels, const unsigned int *pal)
{
	const QuizButton *b = quiz_buttons();
	for (int i = 0; i < QUIZ_BUTTONS; i++)
	{
		for (int y = 0; y < SQUARE_H; y++)
			for (int x = 0; x < SQUARE_W; x++)
			{
				char c = SQUARE[y][x];
				put(pixels, b[i].x0 + x, b[i].y0 + y, pal[c == '0' ? 0 : c == 'f' ? 15 : 14]);
			}
		/* shadowed like the game's list text: black one pixel down right */
		for (int pass = 0; pass < 2; pass++)
		{
			int x = b[i].x0 + SQUARE_W + TEXT_GAP + (pass == 0);
			int y0 = b[i].y0 + 1 + (pass == 0); /* (the list's text: a row under its square's top) */
			unsigned int rgb = pal[pass == 0 ? 0 : TEXT_COLOUR];
			for (const char *c = b[i].label; *c; c++)
			{
				const unsigned char *g = screen_text_glyph(*c);
				if (!g)
				{
					x += 4;
					continue;
				}
				for (int row = 0; row < 8; row++)
					for (int bit = 0; bit < 8; bit++)
						if (g[row] & (0x80 >> bit))
							put(pixels, x + bit, y0 + row, rgb);
				x += g[9];
			}
		}
	}
}

bool quiz_buttons_click(int x, int y)
{
	if (!game_quiz_waiting())
		return false;
	const QuizButton *b = quiz_buttons();
	for (int i = 0; i < QUIZ_BUTTONS; i++)
		if (x >= b[i].x0 && x <= b[i].x1 && y >= b[i].y0 && y <= b[i].y1)
		{
			Input_TapSTKey(b[i].scancode);
			return true;
		}
	return false;
}
