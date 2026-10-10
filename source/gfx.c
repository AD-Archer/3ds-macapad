#include "gfx.h"

#include <math.h>
#include <string.h>

static C2D_TextBuf s_buf;

void gfx_init(void) { s_buf = C2D_TextBufNew(8192); }
void gfx_exit(void) { C2D_TextBufDelete(s_buf); }
void gfx_frame(void) { C2D_TextBufClear(s_buf); }

// Circles are triangle fans: citro2d's own ellipse shader doesn't draw everywhere (Azahar).
static void arc(float cx, float cy, float rx, float ry, float from, float to, u32 c) {
	int n = (int)((rx > ry ? rx : ry) * (to - from) / 2.5f) + 3;
	if (n > 48) n = 48;
	float px = cx + rx * cosf(from), py = cy + ry * sinf(from);
	for (int i = 1; i <= n; i++) {
		float a = from + (to - from) * i / n;
		float qx = cx + rx * cosf(a), qy = cy + ry * sinf(a);
		C2D_DrawTriangle(cx, cy, c, px, py, c, qx, qy, c, 0.5f);
		px = qx;
		py = qy;
	}
}

void disc(float cx, float cy, float r, u32 c) { arc(cx, cy, r, r, 0, 2 * M_PI, c); }

void ellipse(float x, float y, float w, float h, u32 c) { arc(x + w / 2, y + h / 2, w / 2, h / 2, 0, 2 * M_PI, c); }

void rrect(float x, float y, float w, float h, float r, u32 c) {
	if (r * 2 > h) r = h / 2;
	if (r * 2 > w) r = w / 2;
	C2D_DrawRectSolid(x + r, y, 0.5f, w - 2 * r, h, c);
	C2D_DrawRectSolid(x, y + r, 0.5f, r, h - 2 * r, c);
	C2D_DrawRectSolid(x + w - r, y + r, 0.5f, r, h - 2 * r, c);
	arc(x + r, y + r, r, r, M_PI, 1.5f * M_PI, c);
	arc(x + w - r, y + r, r, r, 1.5f * M_PI, 2 * M_PI, c);
	arc(x + w - r, y + h - r, r, r, 0, 0.5f * M_PI, c);
	arc(x + r, y + h - r, r, r, 0.5f * M_PI, M_PI, c);
}

void text(float x, float y, float scale, u32 color, int align, const char *str) {
	if (!str || !*str) return;
	C2D_Text t;
	C2D_TextFontParse(&t, NULL, s_buf, str);
	C2D_TextOptimize(&t);
	C2D_DrawText(&t, C2D_WithColor | align, x, y, 0.5f, scale, scale, color);
}

// Decodes one UTF-8 character; returns its length in bytes.
static int utf8_next(const char *s, u32 *cp) {
	const u8 *u = (const u8 *)s;
	if (u[0] < 0x80) { *cp = u[0]; return 1; }
	if ((u[0] & 0xE0) == 0xC0 && u[1]) { *cp = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F); return 2; }
	if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2]) { *cp = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F); return 3; }
	if ((u[0] & 0xF8) == 0xF0 && u[1] && u[2] && u[3]) {
		*cp = ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) | ((u[2] & 0x3F) << 6) | (u[3] & 0x3F);
		return 4;
	}
	*cp = '?';
	return 1;
}

static float char_width(u32 cp, float scale) {
	charWidthInfo_s *info = C2D_FontGetCharWidthInfo(NULL, C2D_FontGlyphIndexFromCodePoint(NULL, cp));
	return info ? info->charWidth * scale : 0;
}

float text_width(const char *str, float scale) {
	float w = 0;
	for (const char *p = str; p && *p; ) {
		u32 cp;
		p += utf8_next(p, &cp);
		w += char_width(cp, scale);
	}
	return w;
}

void text_fit(float x, float y, float scale, u32 color, int align, float maxw, const char *str) {
	if (!str || !*str) return;
	if (text_width(str, scale) <= maxw) {
		text(x, y, scale, color, align, str);
		return;
	}
	char out[256];
	float ell = char_width(0x2026, scale), w = 0;
	size_t n = 0;
	for (const char *p = str; *p; ) {
		u32 cp;
		int len = utf8_next(p, &cp);
		float cw = char_width(cp, scale);
		if (w + cw + ell > maxw || n + len + 4 >= sizeof out) break;
		memcpy(out + n, p, len);
		n += len;
		w += cw;
		p += len;
	}
	while (n > 0 && out[n - 1] == ' ') n--;
	memcpy(out + n, "\xE2\x80\xA6", 4);  // "…"
	text(x, y, scale, color, align, out);
}

// ---- glyphs ----------------------------------------------------------------------

static void tri_right(float x, float cy, float s, u32 c) {
	C2D_DrawTriangle(x, cy - s / 2, c, x, cy + s / 2, c, x + s * 0.8f, cy, c, 0.5f);
}

static void tri_left(float x, float cy, float s, u32 c) {
	C2D_DrawTriangle(x + s * 0.8f, cy - s / 2, c, x + s * 0.8f, cy + s / 2, c, x, cy, c, 0.5f);
}

static void speaker(float cx, float cy, float s, u32 c) {
	float x = cx - s * 0.55f;
	C2D_DrawRectSolid(x, cy - s * 0.18f, 0.5f, s * 0.22f, s * 0.36f, c);
	C2D_DrawTriangle(x + s * 0.1f, cy, c, x + s * 0.5f, cy - s * 0.45f, c, x + s * 0.5f, cy + s * 0.45f, c, 0.5f);
}

void glyph(Glyph g, float cx, float cy, float s, u32 c) {
	float bar = s * 0.12f;
	switch (g) {
	case GLYPH_PLAY:
		tri_right(cx - s * 0.32f, cy, s, c);
		break;
	case GLYPH_PAUSE:
		rrect(cx - s * 0.32f, cy - s / 2, s * 0.22f, s, 2, c);
		rrect(cx + s * 0.10f, cy - s / 2, s * 0.22f, s, 2, c);
		break;
	case GLYPH_PLAYPAUSE:
		tri_right(cx - s * 0.55f, cy, s * 0.8f, c);
		C2D_DrawRectSolid(cx + s * 0.18f, cy - s * 0.4f, 0.5f, bar * 1.3f, s * 0.8f, c);
		C2D_DrawRectSolid(cx + s * 0.42f, cy - s * 0.4f, 0.5f, bar * 1.3f, s * 0.8f, c);
		break;
	case GLYPH_NEXT:
		tri_right(cx - s * 0.6f, cy, s * 0.75f, c);
		tri_right(cx - s * 0.05f, cy, s * 0.75f, c);
		C2D_DrawRectSolid(cx + s * 0.55f, cy - s * 0.375f, 0.5f, bar, s * 0.75f, c);
		break;
	case GLYPH_PREV:
		C2D_DrawRectSolid(cx - s * 0.55f - bar, cy - s * 0.375f, 0.5f, bar, s * 0.75f, c);
		tri_left(cx - s * 0.55f, cy, s * 0.75f, c);
		tri_left(cx, cy, s * 0.75f, c);
		break;
	case GLYPH_VOL_UP:
		speaker(cx - s * 0.1f, cy, s, c);
		C2D_DrawRectSolid(cx + s * 0.2f, cy - bar / 2, 0.5f, s * 0.4f, bar, c);
		C2D_DrawRectSolid(cx + s * 0.4f - bar / 2, cy - s * 0.2f, 0.5f, bar, s * 0.4f, c);
		break;
	case GLYPH_VOL_DOWN:
		speaker(cx - s * 0.1f, cy, s, c);
		C2D_DrawRectSolid(cx + s * 0.2f, cy - bar / 2, 0.5f, s * 0.4f, bar, c);
		break;
	case GLYPH_MUTE: {
		speaker(cx - s * 0.1f, cy, s, c);
		float x = cx + s * 0.4f, d = s * 0.18f;
		C2D_DrawLine(x - d, cy - d, c, x + d, cy + d, c, bar, 0.5f);
		C2D_DrawLine(x - d, cy + d, c, x + d, cy - d, c, bar, 0.5f);
		break;
	}
	case GLYPH_PLUS:
		C2D_DrawRectSolid(cx - s / 2, cy - bar / 2, 0.5f, s, bar, c);
		C2D_DrawRectSolid(cx - bar / 2, cy - s / 2, 0.5f, bar, s, c);
		break;
	case GLYPH_GLOBE: {
		float r = s / 2, t = bar * 0.7f;
		disc(cx, cy, r, c);
		disc(cx, cy, r - t, C_CARD);
		ellipse(cx - r * 0.45f, cy - r, r * 0.9f, r * 2, c);
		ellipse(cx - r * 0.45f + t, cy - r + t, r * 0.9f - 2 * t, r * 2 - 2 * t, C_CARD);
		C2D_DrawRectSolid(cx - r, cy - t / 2, 0.5f, r * 2, t, c);
		break;
	}
	case GLYPH_KEYS:
		rrect(cx - s * 0.6f, cy - s * 0.38f, s * 1.2f, s * 0.76f, 3, c);
		for (int row = 0; row < 2; row++)
			for (int col = 0; col < 4; col++)
				C2D_DrawRectSolid(cx - s * 0.48f + col * s * 0.26f, cy - s * 0.26f + row * s * 0.22f, 0.5f, s * 0.16f, s * 0.13f, C_CARD);
		C2D_DrawRectSolid(cx - s * 0.3f, cy + s * 0.18f, 0.5f, s * 0.6f, s * 0.1f, C_CARD);
		break;
	case GLYPH_BOLT:
		C2D_DrawTriangle(cx + s * 0.15f, cy - s / 2, c, cx - s * 0.35f, cy + s * 0.08f, c, cx + s * 0.05f, cy + s * 0.08f, c, 0.5f);
		C2D_DrawTriangle(cx - s * 0.15f, cy + s / 2, c, cx + s * 0.35f, cy - s * 0.08f, c, cx - s * 0.05f, cy - s * 0.08f, c, 0.5f);
		break;
	}
}

// ---- pictures ----------------------------------------------------------------------

static int pow2(int n) {
	int p = 8;
	while (p < n) p <<= 1;
	return p;
}

// Where pixel (x, y) goes in a texture tw wide: the GPU wants 8x8 tiles in Morton order.
static inline u32 tiled(int x, int y, int tw) {
	return (((y >> 3) * (tw >> 3) + (x >> 3)) << 6) +
		((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
}

bool picture_load(Picture *pic, const u8 *rgba, int w, int h) {
	picture_free(pic);
	if (!rgba || w <= 0 || h <= 0 || w > 256 || h > 256) return false;
	int tw = pow2(w), th = pow2(h);
	if (!C3D_TexInit(&pic->tex, tw, th, GPU_RGBA8)) return false;
	C3D_TexSetFilter(&pic->tex, GPU_LINEAR, GPU_LINEAR);
	u32 *dst = pic->tex.data;
	memset(dst, 0, tw * th * 4);
	// the GPU wants 8x8 tiles in Morton order, and ABGR byte order
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const u8 *p = rgba + (y * w + x) * 4;
			dst[tiled(x, y, tw)] = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
		}
	}
	C3D_TexFlush(&pic->tex);
	pic->sub = (Tex3DS_SubTexture){(u16)w, (u16)h, 0.0f, 1.0f, (float)w / tw, 1.0f - (float)h / th};
	pic->img = (C2D_Image){&pic->tex, &pic->sub};
	pic->ok = true;
	return true;
}

bool picture_init565(Picture *pic, int w, int h) {
	picture_free(pic);
	if (w <= 0 || h <= 0 || w > 512 || h > 256) return false;
	int tw = pow2(w), th = pow2(h);
	if (!C3D_TexInit(&pic->tex, tw, th, GPU_RGB565)) return false;
	C3D_TexSetFilter(&pic->tex, GPU_LINEAR, GPU_LINEAR);
	memset(pic->tex.data, 0, tw * th * 2);
	pic->sub = (Tex3DS_SubTexture){(u16)w, (u16)h, 0.0f, 1.0f, (float)w / tw, 1.0f - (float)h / th};
	pic->img = (C2D_Image){&pic->tex, &pic->sub};
	pic->ok = true;
	return true;
}

void picture_update565(Picture *pic, const u16 *rgb565) {
	if (!pic->ok) return;
	u16 *dst = pic->tex.data;
	int w = pic->sub.width, h = pic->sub.height, tw = pic->tex.width;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) dst[tiled(x, y, tw)] = rgb565[y * w + x];
	C3D_TexFlush(&pic->tex);
}

void picture_free(Picture *pic) {
	if (pic->ok) C3D_TexDelete(&pic->tex);
	pic->ok = false;
}

void picture_draw(const Picture *pic, float x, float y, float w, float h) {
	if (!pic->ok) return;
	C2D_DrawImageAt(pic->img, x, y, 0.5f, NULL, w / pic->sub.width, h / pic->sub.height);
}
