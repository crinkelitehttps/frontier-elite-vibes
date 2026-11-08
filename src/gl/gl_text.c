/*
 * gl_text.c - see gl_text.h.
 */
#include <stdlib.h>
#include <string.h>

#include "gl_text.h"

#include "gl_api.h"
#include "gl_draw.h"

#define SCALE   8 /* atlas texels to a font pixel */
#define CELL    (8 * SCALE)
#define PAD     4 /* texels round each glyph, so filtering doesn't take its neighbour's */
#define STRIDE  (CELL + 2 * PAD)
#define COLS    16
#define MAX_SKIP 24

static GLuint atlas, prog, vao, vbo;
static GLint u_atlas;
static int atlas_w, atlas_h;
static float *verts;
static size_t verts_cap;

static const char *VS = "in vec2 aPos;\n"
						"in vec2 aUV;\n"
						"in vec4 aCol;\n"
						"out vec2 vUV;\n"
						"out vec4 vCol;\n"
						"void main(){ gl_Position = vec4(aPos, 0.0, 1.0); vUV = aUV; vCol = aCol; }\n";
static const char *FS = "in vec2 vUV;\n"
						"in vec4 vCol;\n"
						"uniform sampler2D uAtlas;\n"
						"out vec4 fragColor;\n"
						"void main(){\n"
						"  float a = texture(uAtlas, vUV).r;\n"
						"  if (a < 0.01) discard;\n"
						"  fragColor = vec4(vCol.rgb, a);\n"
						"}\n";

/* One Scale2x step: n x n -> 2n x 2n */
static void scale2x(const uint8_t *in, int n, uint8_t *out)
{
	for (int y = 0; y < n; y++)
		for (int x = 0; x < n; x++)
		{
			uint8_t p = in[y * n + x];
			uint8_t a = y > 0 ? in[(y - 1) * n + x] : 0;     /* up */
			uint8_t b = x < n - 1 ? in[y * n + x + 1] : 0; /* right */
			uint8_t c = x > 0 ? in[y * n + x - 1] : 0;     /* left */
			uint8_t d = y < n - 1 ? in[(y + 1) * n + x] : 0; /* down */
			uint8_t *o = out + (2 * y) * (2 * n) + 2 * x;
			o[0] = (c == a && c != d && a != b) ? a : p;
			o[1] = (a == b && a != c && b != d) ? b : p;
			o[2 * n] = (d == c && d != b && c != a) ? c : p;
			o[2 * n + 1] = (b == d && b != a && d != c) ? d : p;
		}
}

static void make_atlas(void)
{
	int count = screen_text_glyph_count();
	int rows = (count + COLS - 1) / COLS;
	atlas_w = COLS * STRIDE;
	atlas_h = rows * STRIDE;
	uint8_t *img = calloc((size_t)atlas_w * (size_t)atlas_h, 1);
	if (!img)
		return;
	for (int i = 0; i < count; i++)
	{
		const unsigned char *g = screen_text_glyph(0x20 + i);
		uint8_t a[CELL * CELL], b[CELL * CELL];
		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++)
				a[y * 8 + x] = (g[y] & (0x80 >> x)) ? 255 : 0;
		scale2x(a, 8, b);
		scale2x(b, 16, a);
		scale2x(a, 32, b);
		int ox = (i % COLS) * STRIDE + PAD, oy = (i / COLS) * STRIDE + PAD;
		for (int y = 0; y < CELL; y++)
			memcpy(img + (size_t)(oy + y) * (size_t)atlas_w + ox, b + y * CELL, CELL);
	}
	glGenTextures(1, &atlas);
	glBindTexture(GL_TEXTURE_2D, atlas);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, atlas_w, atlas_h, 0, GL_RED, GL_UNSIGNED_BYTE, img);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glGenerateMipmap(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, 0);
	free(img);
}

static void init(void)
{
	make_atlas();
	static const char *const attribs[] = {"aPos", "aUV", "aCol"};
	prog = gd_build_program(VS, FS, attribs, 3);
	u_atlas = glGetUniformLocation(prog, "uAtlas");
	glGenVertexArrays(1, &vao);
	glGenBuffers(1, &vbo);
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void *)(2 * sizeof(float)));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void *)(4 * sizeof(float)));
	glBindVertexArray(0);
}

void gl_text_shutdown(void)
{
	if (atlas)
		glDeleteTextures(1, &atlas);
	if (prog)
		glDeleteProgram(prog);
	if (vbo)
		glDeleteBuffers(1, &vbo);
	if (vao)
		glDeleteVertexArrays(1, &vao);
	atlas = prog = vbo = vao = 0;
	free(verts);
	verts = NULL;
	verts_cap = 0;
}

typedef struct
{
	const TextGlyphs *t;
	const unsigned int *pal;
	int nskip;
	int skip[MAX_SKIP][4];
} TextPass;

static bool touches(const TextPass *p, int x, int y)
{
	for (int i = 0; i < p->nskip; i++)
	{
		const int *r = p->skip[i];
		if (x < r[0] + r[2] && x + 8 > r[0] && y < r[1] + r[3] && y + 8 > r[1])
			return true;
	}
	return false;
}

static void text_pass(const void *data)
{
	const TextPass *p = data;
	if (!prog)
		init();
	if (!atlas || !p->t->n)
		return;
	size_t need = (size_t)p->t->n * 6 * 8;
	if (need > verts_cap)
	{
		float *v = realloc(verts, need * sizeof(float));
		if (!v)
			return;
		verts = v;
		verts_cap = need;
	}
	size_t k = 0;
	for (int i = 0; i < p->t->n; i++)
	{
		const TextGlyph *g = &p->t->g[i];
		if (touches(p, g->x, g->y))
			continue;
		unsigned int c = p->pal[g->col];
		float r = (float)(c & 0xff) / 255.0f, gr = (float)((c >> 8) & 0xff) / 255.0f, b = (float)((c >> 16) & 0xff) / 255.0f;
		/* screen pixels -> clip space, row 0 at the top */
		float x0 = (float)g->x / 160.0f - 1.0f, x1 = (float)(g->x + 8) / 160.0f - 1.0f;
		float y0 = 1.0f - (float)g->y / 100.0f, y1 = 1.0f - (float)(g->y + 8) / 100.0f;
		float u0 = (float)((g->glyph % COLS) * STRIDE + PAD) / (float)atlas_w, u1 = u0 + (float)CELL / (float)atlas_w;
		float v0 = (float)((g->glyph / COLS) * STRIDE + PAD) / (float)atlas_h, v1 = v0 + (float)CELL / (float)atlas_h;
		const float q[6][4] = {{x0, y0, u0, v0}, {x1, y0, u1, v0}, {x0, y1, u0, v1},
							   {x1, y0, u1, v0}, {x1, y1, u1, v1}, {x0, y1, u0, v1}};
		for (int j = 0; j < 6; j++)
		{
			float *o = verts + k;
			o[0] = q[j][0];
			o[1] = q[j][1];
			o[2] = q[j][2];
			o[3] = q[j][3];
			o[4] = r;
			o[5] = gr;
			o[6] = b;
			o[7] = 1.0f;
			k += 8;
		}
	}
	if (!k)
		return;
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glUseProgram(prog);
	glUniform1i(u_atlas, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, atlas);
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(k * sizeof(float)), verts, GL_STREAM_DRAW);
	glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(k / 8));
	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glDisable(GL_BLEND);
}

void gl_text_queue(const TextGlyphs *t, const unsigned int *pal, const int (*skip)[4], int nskip)
{
	if (!t || !t->n)
		return;
	TextPass *p = gd_custom(text_pass, sizeof *p);
	if (!p)
		return;
	p->t = t;
	p->pal = pal;
	p->nskip = nskip < MAX_SKIP ? nskip : MAX_SKIP;
	for (int i = 0; i < p->nskip; i++)
		memcpy(p->skip[i], skip[i], sizeof p->skip[i]);
}
