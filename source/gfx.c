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

void ring(float cx, float cy, float r, float t, u32 c) { stroke_arc(cx, cy, r, r, 0, 2 * M_PI, t, c); }

// A band between radius (rx, ry) and (rx - t, ry - t), as a strip of triangles.
void stroke_arc(float cx, float cy, float rx, float ry, float from, float to, float t, u32 c) {
	int n = (int)((rx > ry ? rx : ry) * (to - from) / 2.0f) + 3;
	if (n > 64) n = 64;
	float irx = rx - t, iry = ry - t;
	if (irx < 0) irx = 0;
	if (iry < 0) iry = 0;
	float ox = cx + rx * cosf(from), oy = cy + ry * sinf(from);
	float ix = cx + irx * cosf(from), iy = cy + iry * sinf(from);
	for (int i = 1; i <= n; i++) {
		float a = from + (to - from) * i / n;
		float qox = cx + rx * cosf(a), qoy = cy + ry * sinf(a);
		float qix = cx + irx * cosf(a), qiy = cy + iry * sinf(a);
		C2D_DrawTriangle(ox, oy, c, ix, iy, c, qox, qoy, c, 0.5f);
		C2D_DrawTriangle(ix, iy, c, qix, qiy, c, qox, qoy, c, 0.5f);
		ox = qox; oy = qoy; ix = qix; iy = qiy;
	}
}

void stroke_rrect(float x, float y, float w, float h, float r, float t, u32 c) {
	if (r * 2 > h) r = h / 2;
	if (r * 2 > w) r = w / 2;
	C2D_DrawRectSolid(x + r, y, 0.5f, w - 2 * r, t, c);
	C2D_DrawRectSolid(x + r, y + h - t, 0.5f, w - 2 * r, t, c);
	C2D_DrawRectSolid(x, y + r, 0.5f, t, h - 2 * r, c);
	C2D_DrawRectSolid(x + w - t, y + r, 0.5f, t, h - 2 * r, c);
	stroke_arc(x + r, y + r, r, r, M_PI, 1.5f * M_PI, t, c);
	stroke_arc(x + w - r, y + r, r, r, 1.5f * M_PI, 2 * M_PI, t, c);
	stroke_arc(x + w - r, y + h - r, r, r, 0, 0.5f * M_PI, t, c);
	stroke_arc(x + r, y + h - r, r, r, 0.5f * M_PI, M_PI, t, c);
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

static void line(float x1, float y1, float x2, float y2, float t, u32 c) {
	C2D_DrawLine(x1, y1, c, x2, y2, c, t, 0.5f);
}

static void quad(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, u32 c) {
	C2D_DrawTriangle(x1, y1, c, x2, y2, c, x3, y3, c, 0.5f);
	C2D_DrawTriangle(x1, y1, c, x3, y3, c, x4, y4, c, 0.5f);
}

// An arrowhead with its tip at (x, y), pointing along (dx, dy) (a unit vector), s px long.
static void arrow_head(float x, float y, float dx, float dy, float s, u32 c) {
	float bx = x - dx * s, by = y - dy * s;  // middle of the base
	float nx = -dy * s * 0.6f, ny = dx * s * 0.6f;
	C2D_DrawTriangle(x, y, c, bx + nx, by + ny, c, bx - nx, by - ny, c, 0.5f);
}

// A right-pointing arrow with a shaft, cy across the middle, from x0 to x1 (x1 is the tip).
static void arrow(float x0, float x1, float cy, float s, float t, u32 c) {
	float dir = x1 > x0 ? 1 : -1;
	line(x0, cy, x1 - dir * s * 0.3f, cy, t, c);
	arrow_head(x1, cy, dir, 0, s * 0.42f, c);
}

// A window: a rounded rect with a title bar, drawn solid in c with a 1.5px bg border so it reads on a stack.
static void window(float x, float y, float w, float h, u32 c, u32 bg, bool border) {
	if (border) rrect(x - 1.5f, y - 1.5f, w + 3, h + 3, 3.5f, bg);
	rrect(x, y, w, h, 2.5f, c);
	C2D_DrawRectSolid(x + 1.5f, y + h * 0.3f, 0.5f, w - 3, 1.2f, bg);
}

// A screen, s px tall-ish, with the part a window takes filled in (fx, fy, fw, fh: fractions of it).
static void screen_part(float cx, float cy, float s, float t, float fx, float fy, float fw, float fh, u32 c) {
	float w = s * 1.1f, h = s * 0.8f, x = cx - w / 2, y = cy - h / 2, b = t * 0.75f, pad = b + s * 0.07f;
	stroke_rrect(x, y, w, h, s * 0.1f, b, c);
	float iw = w - 2 * pad, ih = h - 2 * pad;
	rrect(x + pad + iw * fx, y + pad + ih * fy, iw * fw, ih * fh, 1, c);
}

void glyph(Glyph g, float cx, float cy, float s, u32 c, u32 bg) {
	float bar = s * 0.12f, t = s * 0.14f;
	switch (g) {
	case GLYPH_NONE:
		break;
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
		line(x - d, cy - d, x + d, cy + d, bar, c);
		line(x - d, cy + d, x + d, cy - d, bar, c);
		break;
	}

	// ---- system ----
	case GLYPH_SMILE:
		ring(cx, cy, s / 2, t, c);
		disc(cx - s * 0.17f, cy - s * 0.12f, s * 0.07f, c);
		disc(cx + s * 0.17f, cy - s * 0.12f, s * 0.07f, c);
		stroke_arc(cx, cy + s * 0.02f, s * 0.28f, s * 0.26f, 0.2f * M_PI, 0.8f * M_PI, t * 0.8f, c);
		break;
	case GLYPH_SEARCH:
		ring(cx - s * 0.1f, cy - s * 0.1f, s * 0.32f, t, c);
		line(cx + s * 0.14f, cy + s * 0.14f, cx + s * 0.44f, cy + s * 0.44f, t * 1.2f, c);
		break;
	case GLYPH_CAMERA:
		C2D_DrawRectSolid(cx - s * 0.18f, cy - s * 0.42f, 0.5f, s * 0.36f, s * 0.16f, c);
		stroke_rrect(cx - s * 0.5f, cy - s * 0.3f, s, s * 0.7f, s * 0.12f, t * 0.8f, c);
		ring(cx, cy + s * 0.05f, s * 0.18f, t * 0.8f, c);
		break;
	case GLYPH_CROP: {
		float a = s * 0.45f, l = s * 0.26f, w = t * 0.85f;
		for (int i = 0; i < 4; i++) {
			float sx = i & 1 ? 1 : -1, sy = i & 2 ? 1 : -1;
			float x = cx + sx * a, y = cy + sy * a;
			C2D_DrawRectSolid(sx > 0 ? x - l : x, y - (sy > 0 ? w : 0), 0.5f, l, w, c);
			C2D_DrawRectSolid(x - (sx > 0 ? w : 0), sy > 0 ? y - l : y, 0.5f, w, l, c);
		}
		disc(cx, cy, s * 0.1f, c);
		break;
	}
	case GLYPH_MONITOR:
		stroke_rrect(cx - s * 0.5f, cy - s * 0.42f, s, s * 0.66f, s * 0.08f, t * 0.8f, c);
		C2D_DrawRectSolid(cx - t / 2, cy + s * 0.24f, 0.5f, t, s * 0.14f, c);
		rrect(cx - s * 0.24f, cy + s * 0.36f, s * 0.48f, t * 0.8f, 1.5f, c);
		break;
	case GLYPH_GRID: {
		float b = s * 0.42f, gap = s * 0.14f, x0 = cx - b - gap / 2, y0 = cy - b - gap / 2;
		for (int i = 0; i < 4; i++) rrect(x0 + (i & 1) * (b + gap), y0 + (i >> 1) * (b + gap), b, b, s * 0.08f, c);
		break;
	}
	case GLYPH_WINDOWS:
		window(cx - s * 0.5f, cy - s * 0.45f, s * 0.78f, s * 0.62f, c, bg, false);
		window(cx - s * 0.26f, cy - s * 0.15f, s * 0.78f, s * 0.62f, c, bg, true);
		break;
	case GLYPH_LOCK:
		stroke_arc(cx, cy - s * 0.14f, s * 0.24f, s * 0.26f, M_PI, 2 * M_PI, t * 0.85f, c);
		C2D_DrawRectSolid(cx - s * 0.24f, cy - s * 0.14f, 0.5f, t * 0.85f, s * 0.1f, c);
		C2D_DrawRectSolid(cx + s * 0.24f - t * 0.85f, cy - s * 0.14f, 0.5f, t * 0.85f, s * 0.1f, c);
		rrect(cx - s * 0.4f, cy - s * 0.06f, s * 0.8f, s * 0.56f, s * 0.1f, c);
		disc(cx, cy + s * 0.2f, s * 0.07f, bg);
		break;

	// ---- windows ----
	case GLYPH_MINIMIZE:
		line(cx, cy - s * 0.48f, cx, cy + s * 0.02f, t, c);
		arrow_head(cx, cy + s * 0.2f, 0, 1, s * 0.4f, c);
		rrect(cx - s * 0.42f, cy + s * 0.36f, s * 0.84f, t, 1.5f, c);
		break;
	case GLYPH_EXPAND: {
		float a = s * 0.42f, l = s * 0.3f, w = t * 0.9f;
		// top-right and bottom-left corners, with a diagonal between
		C2D_DrawRectSolid(cx + a - l, cy - a, 0.5f, l, w, c);
		C2D_DrawRectSolid(cx + a - w, cy - a, 0.5f, w, l, c);
		C2D_DrawRectSolid(cx - a, cy + a - w, 0.5f, l, w, c);
		C2D_DrawRectSolid(cx - a, cy + a - l, 0.5f, w, l, c);
		line(cx - a + w, cy + a - w, cx + a - w, cy - a + w, w, c);
		break;
	}
	case GLYPH_CLOSE: {
		ring(cx, cy, s / 2, t, c);
		float d = s * 0.17f;
		line(cx - d, cy - d, cx + d, cy + d, t * 0.85f, c);
		line(cx - d, cy + d, cx + d, cy - d, t * 0.85f, c);
		break;
	}
	case GLYPH_REFRESH: {
		float r = s * 0.4f, to = 4.3f;
		stroke_arc(cx, cy, r, r, -0.9f, to, t, c);
		float ex = cx + (r - t / 2) * cosf(to), ey = cy + (r - t / 2) * sinf(to);
		arrow_head(ex - sinf(to) * s * 0.1f, ey + cosf(to) * s * 0.1f, -sinf(to), cosf(to), s * 0.36f, c);
		break;
	}
	case GLYPH_MINUS_CIRCLE:
		ring(cx, cy, s / 2, t, c);
		rrect(cx - s * 0.24f, cy - t * 0.42f, s * 0.48f, t * 0.85f, 1, c);
		break;
	case GLYPH_POWER:
		stroke_arc(cx, cy + s * 0.04f, s * 0.42f, s * 0.42f, -M_PI / 2 + 0.7f, 1.5f * M_PI - 0.7f, t, c);
		rrect(cx - t / 2, cy - s * 0.5f, t, s * 0.48f, t / 2, c);
		break;

	// ---- browser ----
	case GLYPH_TAB:
		rrect(cx - s * 0.5f, cy - s * 0.46f, s * 0.44f, s * 0.3f, s * 0.08f, c);
		rrect(cx - s * 0.5f, cy - s * 0.28f, s, s * 0.74f, s * 0.1f, c);
		C2D_DrawRectSolid(cx - s * 0.2f, cy + s * 0.08f - bar * 0.4f, 0.5f, s * 0.4f, bar * 0.8f, bg);
		C2D_DrawRectSolid(cx - bar * 0.4f, cy + s * 0.08f - s * 0.2f, 0.5f, bar * 0.8f, s * 0.4f, bg);
		break;
	case GLYPH_UNDO:
	case GLYPH_REDO: {
		float m = g == GLYPH_UNDO ? 1 : -1, r = s * 0.3f;
		float ax = cx + m * s * 0.1f, ay = cy + s * 0.08f;
		// the curve on the far side, then a straight run back toward the arrow
		if (m > 0) stroke_arc(ax, ay, r, r, -M_PI / 2, M_PI / 2, t, c);
		else stroke_arc(ax, ay, r, r, M_PI / 2, 1.5f * M_PI, t, c);
		line(ax, ay - r + t / 2, ax - m * s * 0.3f, ay - r + t / 2, t, c);
		line(ax, ay + r - t / 2, ax - m * s * 0.22f, ay + r - t / 2, t, c);
		arrow_head(ax - m * s * 0.5f, ay - r + t / 2, -m, 0, s * 0.36f, c);
		break;
	}
	case GLYPH_ARROW_RIGHT:
		arrow(cx - s * 0.45f, cx + s * 0.48f, cy, s, t, c);
		break;
	case GLYPH_ARROW_LEFT:
		arrow(cx + s * 0.45f, cx - s * 0.48f, cy, s, t, c);
		break;
	case GLYPH_ADDRESS:
		stroke_rrect(cx - s * 0.62f, cy - s * 0.26f, s * 1.24f, s * 0.52f, s * 0.26f, t * 0.75f, c);
		disc(cx - s * 0.34f, cy, s * 0.07f, c);
		rrect(cx - s * 0.18f, cy - t * 0.3f, s * 0.5f, t * 0.6f, 1, c);
		break;

	// ---- editing ----
	case GLYPH_COPY:
		stroke_rrect(cx - s * 0.46f, cy - s * 0.5f, s * 0.64f, s * 0.74f, s * 0.08f, t * 0.75f, c);
		rrect(cx - s * 0.2f - 1.5f, cy - s * 0.26f - 1.5f, s * 0.64f + 3, s * 0.74f + 3, s * 0.1f, bg);
		rrect(cx - s * 0.2f, cy - s * 0.26f, s * 0.64f, s * 0.74f, s * 0.08f, c);
		break;
	case GLYPH_PASTE:
		rrect(cx - s * 0.4f, cy - s * 0.38f, s * 0.8f, s * 0.88f, s * 0.08f, c);
		rrect(cx - s * 0.2f, cy - s * 0.5f, s * 0.4f, s * 0.22f, s * 0.05f, c);
		C2D_DrawRectSolid(cx - s * 0.24f, cy - s * 0.02f, 0.5f, s * 0.48f, t * 0.55f, bg);
		C2D_DrawRectSolid(cx - s * 0.24f, cy + s * 0.18f, 0.5f, s * 0.32f, t * 0.55f, bg);
		break;
	case GLYPH_CUT:
		ring(cx - s * 0.26f, cy + s * 0.3f, s * 0.17f, t * 0.8f, c);
		ring(cx + s * 0.26f, cy + s * 0.3f, s * 0.17f, t * 0.8f, c);
		line(cx - s * 0.14f, cy + s * 0.16f, cx + s * 0.32f, cy - s * 0.5f, t * 0.85f, c);
		line(cx + s * 0.14f, cy + s * 0.16f, cx - s * 0.32f, cy - s * 0.5f, t * 0.85f, c);
		break;
	case GLYPH_SELECT_ALL: {
		float a = s * 0.44f, d = s * 0.22f, w = t * 0.7f;
		for (int i = 0; i < 3; i++) {
			float o = -a + i * (2 * a - d) / 2;
			C2D_DrawRectSolid(cx + o, cy - a, 0.5f, d, w, c);
			C2D_DrawRectSolid(cx + o, cy + a - w, 0.5f, d, w, c);
			C2D_DrawRectSolid(cx - a, cy + o, 0.5f, w, d, c);
			C2D_DrawRectSolid(cx + a - w, cy + o, 0.5f, w, d, c);
		}
		rrect(cx - s * 0.18f, cy - s * 0.18f, s * 0.36f, s * 0.36f, 1.5f, c);
		break;
	}
	case GLYPH_SAVE:
		rrect(cx - s * 0.44f, cy - s * 0.44f, s * 0.88f, s * 0.88f, s * 0.1f, c);
		C2D_DrawRectSolid(cx - s * 0.24f, cy - s * 0.44f, 0.5f, s * 0.48f, s * 0.26f, bg);
		C2D_DrawRectSolid(cx + s * 0.06f, cy - s * 0.4f, 0.5f, s * 0.1f, s * 0.16f, c);
		rrect(cx - s * 0.26f, cy + s * 0.1f, s * 0.52f, s * 0.26f, s * 0.04f, bg);
		break;

	// ---- key kinds + UI ----
	case GLYPH_BOLT:
		quad(cx + s * 0.2f, cy - s * 0.5f, cx - s * 0.3f, cy + s * 0.05f, cx, cy + s * 0.05f, cx + s * 0.1f, cy - s * 0.08f, c);
		quad(cx, cy + s * 0.05f, cx - s * 0.15f, cy + s * 0.5f, cx + s * 0.3f, cy - s * 0.08f, cx + s * 0.1f, cy - s * 0.08f, c);
		break;
	case GLYPH_GLOBE: {
		float r = s / 2, w = t * 0.7f;
		ring(cx, cy, r, w, c);
		stroke_arc(cx, cy, r * 0.45f, r, 0, 2 * M_PI, w, c);
		C2D_DrawRectSolid(cx - r, cy - w / 2, 0.5f, r * 2, w, c);
		break;
	}
	case GLYPH_KEYS:
		stroke_rrect(cx - s * 0.6f, cy - s * 0.38f, s * 1.2f, s * 0.76f, 3, t * 0.7f, c);
		for (int row = 0; row < 2; row++)
			for (int col = 0; col < 4; col++)
				C2D_DrawRectSolid(cx - s * 0.44f + col * s * 0.24f, cy - s * 0.22f + row * s * 0.18f, 0.5f, s * 0.14f, s * 0.11f, c);
		C2D_DrawRectSolid(cx - s * 0.3f, cy + s * 0.14f, 0.5f, s * 0.6f, s * 0.1f, c);
		break;
	case GLYPH_SHORTCUT:
		rrect(cx - s * 0.5f, cy - s * 0.5f, s * 0.74f, s * 0.74f, s * 0.18f, c);
		rrect(cx - s * 0.24f - 1.5f, cy - s * 0.24f - 1.5f, s * 0.74f + 3, s * 0.74f + 3, s * 0.2f, bg);
		rrect(cx - s * 0.24f, cy - s * 0.24f, s * 0.74f, s * 0.74f, s * 0.18f, c);
		break;
	case GLYPH_APP:
		rrect(cx - s * 0.5f, cy - s * 0.5f, s, s, s * 0.22f, c);
		disc(cx - s * 0.2f, cy - s * 0.2f, s * 0.1f, bg);
		disc(cx + s * 0.2f, cy - s * 0.2f, s * 0.1f, bg);
		disc(cx - s * 0.2f, cy + s * 0.2f, s * 0.1f, bg);
		disc(cx + s * 0.2f, cy + s * 0.2f, s * 0.1f, bg);
		break;
	case GLYPH_PLUS:
		rrect(cx - s / 2, cy - bar / 2, s, bar, bar / 2, c);
		rrect(cx - bar / 2, cy - s / 2, bar, s, bar / 2, c);
		break;
	case GLYPH_CHECK:
		line(cx - s * 0.42f, cy + s * 0.02f, cx - s * 0.14f, cy + s * 0.3f, t * 1.1f, c);
		line(cx - s * 0.14f, cy + s * 0.3f, cx + s * 0.44f, cy - s * 0.3f, t * 1.1f, c);
		break;
	case GLYPH_CHEVRON_R:
	case GLYPH_CHEVRON_L: {
		float m = g == GLYPH_CHEVRON_R ? 1 : -1;
		line(cx - m * s * 0.14f, cy - s * 0.36f, cx + m * s * 0.16f, cy, t * 0.9f, c);
		line(cx + m * s * 0.16f, cy, cx - m * s * 0.14f, cy + s * 0.36f, t * 0.9f, c);
		break;
	}
	case GLYPH_CLOCK:
		ring(cx, cy, s / 2, t, c);
		rrect(cx - t * 0.4f, cy - s * 0.3f, t * 0.8f, s * 0.3f + t * 0.4f, 1, c);
		rrect(cx - t * 0.4f, cy - t * 0.4f, s * 0.24f, t * 0.8f, 1, c);
		break;
	case GLYPH_LAPTOP:
		stroke_rrect(cx - s * 0.4f, cy - s * 0.42f, s * 0.8f, s * 0.56f, s * 0.06f, t * 0.8f, c);
		rrect(cx - s * 0.58f, cy + s * 0.2f, s * 1.16f, t, t / 2, c);
		break;
	case GLYPH_PENCIL:
		line(cx - s * 0.22f, cy + s * 0.22f, cx + s * 0.4f, cy - s * 0.4f, s * 0.24f, c);
		C2D_DrawTriangle(cx - s * 0.46f, cy + s * 0.46f, c, cx - s * 0.38f, cy + s * 0.16f, c, cx - s * 0.16f, cy + s * 0.38f, c, 0.5f);
		break;
	case GLYPH_TRASH:
		rrect(cx - s * 0.46f, cy - s * 0.4f, s * 0.92f, t * 0.8f, 1.5f, c);
		C2D_DrawRectSolid(cx - s * 0.16f, cy - s * 0.5f, 0.5f, s * 0.32f, t * 0.8f, c);
		rrect(cx - s * 0.36f, cy - s * 0.26f, s * 0.72f, s * 0.76f, s * 0.08f, c);
		C2D_DrawRectSolid(cx - s * 0.14f, cy - s * 0.12f, 0.5f, t * 0.5f, s * 0.46f, bg);
		C2D_DrawRectSolid(cx + s * 0.07f, cy - s * 0.12f, 0.5f, t * 0.5f, s * 0.46f, bg);
		break;

	// ---- window sizes ----
	case GLYPH_HALF_LEFT: screen_part(cx, cy, s, t, 0, 0, 0.5f, 1, c); break;
	case GLYPH_HALF_RIGHT: screen_part(cx, cy, s, t, 0.5f, 0, 0.5f, 1, c); break;
	case GLYPH_HALF_TOP: screen_part(cx, cy, s, t, 0, 0, 1, 0.5f, c); break;
	case GLYPH_HALF_BOTTOM: screen_part(cx, cy, s, t, 0, 0.5f, 1, 0.5f, c); break;
	case GLYPH_QUARTER_TL: screen_part(cx, cy, s, t, 0, 0, 0.5f, 0.5f, c); break;
	case GLYPH_QUARTER_TR: screen_part(cx, cy, s, t, 0.5f, 0, 0.5f, 0.5f, c); break;
	case GLYPH_QUARTER_BL: screen_part(cx, cy, s, t, 0, 0.5f, 0.5f, 0.5f, c); break;
	case GLYPH_QUARTER_BR: screen_part(cx, cy, s, t, 0.5f, 0.5f, 0.5f, 0.5f, c); break;
	case GLYPH_FILL_SCREEN: screen_part(cx, cy, s, t, 0, 0, 1, 1, c); break;
	case GLYPH_CENTER: screen_part(cx, cy, s, t, 0.25f, 0.2f, 0.5f, 0.6f, c); break;

	// ---- more system, accessibility and the mouse ----
	case GLYPH_COFFEE: {
		float w = s * 0.62f, x = cx - s * 0.44f, y = cy - s * 0.12f, h = s * 0.5f;
		C2D_DrawRectSolid(x, y, 0.5f, w, s * 0.14f, c);
		rrect(x, y, w, h, s * 0.14f, c);
		stroke_arc(x + w - t * 0.2f, y + h * 0.4f, s * 0.2f, s * 0.17f, -M_PI / 2, M_PI / 2, t * 0.8f, c);
		line(cx - s * 0.26f, cy - s * 0.5f, cx - s * 0.26f, cy - s * 0.26f, t * 0.7f, c);
		line(cx - s * 0.06f, cy - s * 0.5f, cx - s * 0.06f, cy - s * 0.26f, t * 0.7f, c);
		rrect(cx - s * 0.5f, cy + s * 0.42f, s * 0.84f, t * 0.7f, 1, c);
		break;
	}
	case GLYPH_MIC:
		rrect(cx - s * 0.17f, cy - s * 0.5f, s * 0.34f, s * 0.6f, s * 0.17f, c);
		stroke_arc(cx, cy - s * 0.06f, s * 0.32f, s * 0.32f, 0, M_PI, t * 0.8f, c);
		line(cx, cy + s * 0.25f, cx, cy + s * 0.44f, t * 0.8f, c);
		rrect(cx - s * 0.2f, cy + s * 0.4f, s * 0.4f, t * 0.8f, 1, c);
		break;
	case GLYPH_SUN:
	case GLYPH_SUN_LOW: {
		bool low = g == GLYPH_SUN_LOW;
		if (low) ring(cx, cy, s * 0.22f, t * 0.8f, c);
		else disc(cx, cy, s * 0.22f, c);
		float r0 = s * 0.33f, r1 = low ? s * 0.42f : s * 0.5f;
		for (int i = 0; i < 8; i++) {
			float a = i * (float)M_PI / 4, ca = cosf(a), sa = sinf(a);
			line(cx + ca * r0, cy + sa * r0, cx + ca * r1, cy + sa * r1, t * 0.75f, c);
		}
		break;
	}
	case GLYPH_MOON:
		disc(cx, cy, s * 0.42f, c);
		disc(cx + s * 0.2f, cy - s * 0.14f, s * 0.34f, bg);
		break;
	case GLYPH_PERSON:
		disc(cx, cy - s * 0.38f, s * 0.12f, c);
		line(cx - s * 0.44f, cy - s * 0.16f, cx + s * 0.44f, cy - s * 0.16f, t * 0.9f, c);
		line(cx, cy - s * 0.16f, cx, cy + s * 0.12f, t * 1.1f, c);
		line(cx, cy + s * 0.08f, cx - s * 0.22f, cy + s * 0.48f, t * 0.9f, c);
		line(cx, cy + s * 0.08f, cx + s * 0.22f, cy + s * 0.48f, t * 0.9f, c);
		break;
	case GLYPH_SPEAK:
		rrect(cx - s * 0.5f, cy - s * 0.42f, s, s * 0.66f, s * 0.16f, c);
		C2D_DrawTriangle(cx - s * 0.32f, cy + s * 0.2f, c, cx - s * 0.04f, cy + s * 0.2f, c, cx - s * 0.36f, cy + s * 0.48f, c, 0.5f);
		for (int i = -1; i <= 1; i++) {
			float bh = i ? s * 0.18f : s * 0.34f;
			C2D_DrawRectSolid(cx + i * s * 0.2f - t * 0.4f, cy - s * 0.09f - bh / 2, 0.5f, t * 0.8f, bh, bg);
		}
		break;
	case GLYPH_ZOOM_IN:
	case GLYPH_ZOOM_OUT: {
		float rx = cx - s * 0.1f, ry = cy - s * 0.1f, d = s * 0.15f, w = t * 0.7f;
		ring(rx, ry, s * 0.36f, t * 0.9f, c);
		line(cx + s * 0.16f, cy + s * 0.16f, cx + s * 0.46f, cy + s * 0.46f, t * 1.2f, c);
		C2D_DrawRectSolid(rx - d, ry - w / 2, 0.5f, 2 * d, w, c);
		if (g == GLYPH_ZOOM_IN) C2D_DrawRectSolid(rx - w / 2, ry - d, 0.5f, w, 2 * d, c);
		break;
	}
	case GLYPH_INVERT:
		ring(cx, cy, s / 2, t, c);
		arc(cx, cy, s / 2, s / 2, -M_PI / 2, M_PI / 2, c);
		break;
	case GLYPH_MOUSE:
	case GLYPH_CLICK_LEFT:
	case GLYPH_CLICK_RIGHT: {
		float w = s * 0.64f, h = s, x = cx - w / 2, y = cy - h / 2, b = t * 0.8f, split = h * 0.42f;
		stroke_rrect(x, y, w, h, w / 2, b, c);
		C2D_DrawRectSolid(cx - b / 2, y, 0.5f, b, split, c);
		C2D_DrawRectSolid(x, y + split - b / 2, 0.5f, w, b, c);
		if (g != GLYPH_MOUSE) {
			// the button that's clicked, filled in
			bool left = g == GLYPH_CLICK_LEFT;
			arc(cx, y + w / 2, w / 2, w / 2, left ? M_PI : 1.5f * M_PI, left ? 1.5f * M_PI : 2 * M_PI, c);
			C2D_DrawRectSolid(left ? x : cx, y + w / 2, 0.5f, w / 2, split - w / 2, c);
		}
		break;
	}
	case GLYPH_GAMEPAD: {
		float w = s * 1.2f, h = s * 0.68f, d = s * 0.15f, th = t * 0.75f, px = cx - w * 0.25f;
		rrect(cx - w / 2, cy - h / 2, w, h, h * 0.42f, c);
		C2D_DrawRectSolid(px - d, cy - th / 2, 0.5f, 2 * d, th, bg);
		C2D_DrawRectSolid(px - th / 2, cy - d, 0.5f, th, 2 * d, bg);
		disc(cx + w * 0.18f, cy + s * 0.07f, s * 0.07f, bg);
		disc(cx + w * 0.3f, cy - s * 0.07f, s * 0.07f, bg);
		break;
	}
	case GLYPH_ARROW_UP:
	case GLYPH_ARROW_DOWN: {
		float m = g == GLYPH_ARROW_DOWN ? 1 : -1;
		line(cx, cy - m * s * 0.45f, cx, cy + m * s * 0.18f, t, c);
		arrow_head(cx, cy + m * s * 0.48f, 0, m, s * 0.42f, c);
		break;
	}
	case GLYPH_WIFI: {
		// a fan of arcs over a dot, opening upward
		float oy = cy + s * 0.36f, w = t * 0.95f;
		for (int i = 1; i <= 3; i++)
			stroke_arc(cx, oy, s * 0.27f * i, s * 0.27f * i, -0.75f * M_PI, -0.25f * M_PI, w, c);
		disc(cx, oy - w * 0.2f, s * 0.1f, c);
		break;
	}
	case GLYPH_BLUETOOTH: {
		float w = t * 0.85f, x = cx - s * 0.04f, d = s * 0.26f;
		line(x, cy - s * 0.5f, x, cy + s * 0.5f, w, c);
		line(x, cy - s * 0.5f, x + d, cy - s * 0.24f, w, c);
		line(x + d, cy - s * 0.24f, x - d, cy + s * 0.24f, w, c);
		line(x, cy + s * 0.5f, x + d, cy + s * 0.24f, w, c);
		line(x + d, cy + s * 0.24f, x - d, cy - s * 0.24f, w, c);
		break;
	}
	case GLYPH_EJECT:
		C2D_DrawTriangle(cx, cy - s * 0.46f, c, cx - s * 0.46f, cy + s * 0.1f, c, cx + s * 0.46f, cy + s * 0.1f, c, 0.5f);
		rrect(cx - s * 0.46f, cy + s * 0.26f, s * 0.92f, s * 0.18f, 1.5f, c);
		break;
	case GLYPH_EYE:
		stroke_arc(cx, cy, s * 0.56f, s * 0.32f, 0, 2 * M_PI, t * 0.85f, c);
		disc(cx, cy, s * 0.17f, c);
		break;
	case GLYPH_STAGE:
		// the front window, with the others in a strip at its left
		for (int i = 0; i < 3; i++) rrect(cx - s * 0.6f, cy - s * 0.38f + i * s * 0.28f, s * 0.22f, s * 0.2f, 1.5f, c);
		window(cx - s * 0.26f, cy - s * 0.4f, s * 0.86f, s * 0.8f, c, bg, false);
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
