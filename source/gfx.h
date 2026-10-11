#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>

#define TOP_W 400
#define BOTTOM_W 320
#define SCREEN_H 240

// iOS dark palette
#define C_BG        C2D_Color32(0x00, 0x00, 0x00, 0xFF)
#define C_CARD      C2D_Color32(0x1C, 0x1C, 0x1E, 0xFF)
#define C_CARD_HI   C2D_Color32(0x3A, 0x3A, 0x3C, 0xFF)
#define C_LINE      C2D_Color32(0x38, 0x38, 0x3A, 0xFF)
#define C_TEXT      C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)
#define C_TEXT2     C2D_Color32(0x98, 0x98, 0x9F, 0xFF)
#define C_TEXT3     C2D_Color32(0x63, 0x63, 0x66, 0xFF)
#define C_BLUE      C2D_Color32(0x0A, 0x84, 0xFF, 0xFF)
#define C_GREEN     C2D_Color32(0x30, 0xD1, 0x58, 0xFF)
#define C_ORANGE    C2D_Color32(0xFF, 0x9F, 0x0A, 0xFF)
#define C_RED       C2D_Color32(0xFF, 0x45, 0x3A, 0xFF)
#define C_PURPLE    C2D_Color32(0xBF, 0x5A, 0xF2, 0xFF)
#define C_YELLOW    C2D_Color32(0xFF, 0xD6, 0x0A, 0xFF)
#define C_TEAL      C2D_Color32(0x64, 0xD2, 0xFF, 0xFF)
#define C_PINK      C2D_Color32(0xFF, 0x37, 0x5F, 0xFF)
#define C_GRAY      C2D_Color32(0x8E, 0x8E, 0x93, 0xFF)

void gfx_init(void);
void gfx_exit(void);
void gfx_frame(void);  // clears the text buffer; call once per frame

void disc(float cx, float cy, float r, u32 color);
void ellipse(float x, float y, float w, float h, u32 color);
void rrect(float x, float y, float w, float h, float r, u32 color);
// Outlines, t px thick, drawn inside the shape.
void ring(float cx, float cy, float r, float t, u32 color);
void stroke_arc(float cx, float cy, float rx, float ry, float from, float to, float t, u32 color);
void stroke_rrect(float x, float y, float w, float h, float r, float t, u32 color);

// Text at (x, y) = top-left, or top-center / top-right with C2D_AlignCenter / C2D_AlignRight.
void text(float x, float y, float scale, u32 color, int align, const char *str);
// Same, but cut short with "…" to fit maxw.
void text_fit(float x, float y, float scale, u32 color, int align, float maxw, const char *str);
float text_width(const char *str, float scale);

// Little drawn icons, centered on (cx, cy), about size px tall. bg is what's behind the icon,
// for the few that have gaps (a keyhole, the front window of a stack).
typedef enum {
	GLYPH_NONE,
	// media
	GLYPH_PLAY, GLYPH_PAUSE, GLYPH_PLAYPAUSE, GLYPH_NEXT, GLYPH_PREV,
	GLYPH_VOL_UP, GLYPH_VOL_DOWN, GLYPH_MUTE,
	// system
	GLYPH_SMILE, GLYPH_SEARCH, GLYPH_CAMERA, GLYPH_CROP, GLYPH_MONITOR, GLYPH_GRID, GLYPH_WINDOWS, GLYPH_LOCK,
	// windows
	GLYPH_MINIMIZE, GLYPH_EXPAND, GLYPH_CLOSE, GLYPH_REFRESH, GLYPH_MINUS_CIRCLE, GLYPH_POWER,
	// browser
	GLYPH_TAB, GLYPH_UNDO, GLYPH_REDO, GLYPH_ARROW_RIGHT, GLYPH_ARROW_LEFT, GLYPH_ADDRESS,
	// editing
	GLYPH_COPY, GLYPH_PASTE, GLYPH_CUT, GLYPH_SELECT_ALL, GLYPH_SAVE,
	// key kinds + UI
	GLYPH_BOLT, GLYPH_GLOBE, GLYPH_KEYS, GLYPH_SHORTCUT, GLYPH_APP,
	GLYPH_PLUS, GLYPH_CHECK, GLYPH_CHEVRON_R, GLYPH_CHEVRON_L, GLYPH_CLOCK, GLYPH_LAPTOP, GLYPH_PENCIL, GLYPH_TRASH,
} Glyph;
void glyph(Glyph g, float cx, float cy, float size, u32 color, u32 bg);

// A texture made from RGBA8 pixels (up to 256x256).
typedef struct {
	bool ok;
	C3D_Tex tex;
	Tex3DS_SubTexture sub;
	C2D_Image img;
} Picture;

bool picture_load(Picture *pic, const u8 *rgba, int w, int h);
void picture_free(Picture *pic);
void picture_draw(const Picture *pic, float x, float y, float w, float h);

// A picture that's updated in place from RGB565 frames, like the camera preview (up to 512x256).
bool picture_init565(Picture *pic, int w, int h);
void picture_update565(Picture *pic, const u16 *rgb565);
