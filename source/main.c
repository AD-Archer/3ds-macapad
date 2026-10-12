// Macapad for 3DS: the top screen shows the time, what's playing and how the Mac is doing;
// the bottom screen is a pad of keys that run things on the Mac. It can also be a controller
// (buttons, sticks and a trackpad, live) or a keyboard for the Mac.
#include <3ds.h>
#include <arpa/inet.h>
#include <citro2d.h>
#include <ctype.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gfx.h"
#include "http.h"
#include "input.h"
#include "macapad.h"
#include "qrscan.h"

#define SOC_BUFFERSIZE 0x100000
#define LONG_PRESS_MS 500

// lists: inset cards of rows under a title bar
#define LIST_TOP 34
#define ROW_H 34
#define ROW2_H 42       // a row with a second line
#define HEAD_H 26       // a section title above a card
#define BREAK_H 8       // a gap between cards
#define CARD_X 8
#define CARD_W (BOTTOM_W - 2 * CARD_X)
#define TILE 24         // the icon square at the left of a row

// the pad: 4x3 keys
#define GRID_X 6
#define GRID_Y 34
#define GAP 6
#define KEY_W ((BOTTOM_W - 2 * GRID_X - (COLS - 1) * GAP) / (float)COLS)
#define KEY_H ((SCREEN_H - GRID_Y - GRID_X - (ROWS - 1) * GAP) / (float)ROWS)
#define LOOK_PREVIEW_H 76  // the key shown above the "How it looks" rows

// swiping between pages
#define SWIPE_COMMIT 70  // px: farther than this and the page changes
#define FLICK_PX 22      // or a quick flick of at least this far
#define FLICK_MS 260

typedef enum { SCR_FIND, SCR_QR, SCR_PAIR, SCR_MAIN, SCR_LIST, SCR_KEYS, SCR_PAD, SCR_TYPE } Screen;

// What the list on screen is for, so a pick knows where to go.
typedef enum {
	LIST_KIND,       // "What should this key do?"
	LIST_ACTIONS,
	LIST_APPS,
	LIST_SHORTCUTS,
	LIST_KEYNAME,    // the key in a keyboard shortcut
	LIST_MENU,       // START menu
	LIST_SEARCH,     // actions, apps and Shortcuts that match what was typed
	LIST_LOOK,       // "How it looks": a key's name, icon and color, after picking what it does
	LIST_GLYPH,      // the icon for it
	LIST_COLOR,      // the color for it
	LIST_PAD,        // "Use as a controller": start, the preset, changes
	LIST_PADBUTTONS, // "Change a button": every button and stick
	LIST_PADBIND,    // what one button does
	LIST_PADSTICK,   // what one stick does
} ListPurpose;

typedef enum {
	ROW_PLAIN,
	ROW_HEADER,   // a section title (not pickable); an empty one is just a gap
	ROW_TOGGLE,   // a switch at the right
	ROW_CHEVRON,  // leads somewhere: "›" at the right, with detail text before it
	ROW_CHECK,    // one of several: a check at the right when it's the one
	ROW_SEARCH,   // the search field: a pill with what was typed, or a prompt
} RowStyle;

typedef struct {
	RowStyle style;
	char label[64];
	char sub[48];       // a second line under the label
	char detail[32];    // small text at the right
	char value[192];    // what picking it means
	int id;
	Glyph icon;         // GLYPH_NONE: no tile (unless tag is set)
	char tag[4];        // short text on the tile instead of a glyph: "1", "Aa"
	u32 color;          // the tile
	bool on;            // the switch is on / the check shows
	bool danger;        // red label
	bool swatch;        // a plain tile of color, with nothing on it
	char category[16];  // actions: for the key
	char iconName[24];  // actions: for the key
	float y, h;         // from list_layout
	bool first, last;   // ends of its card
} Item;

typedef struct {
	ListPurpose purpose;
	char title[48];
	Item *items;
	int count;
	int sel;
	float scroll, height;
	bool loading;
	char empty[96];
	char filter[48];  // catalogs: only rows whose name contains this
	int results;      // catalogs: rows that matched
	int refreshRow;   // the "Refresh from your Mac" row, whose text follows the cache; -1 = none
	float top;        // where the rows start on screen (lower when there's a preview above them)
} List;

// ---- state -------------------------------------------------------------------

static Screen s_screen;
static Scan s_scan;
static Pairing s_pair;
static Catalog s_cats[3];  // one per CatalogKind; the ones a list asked for stay until it closes
static int s_wantCats;      // bit per CatalogKind
static List s_list;
static Status s_st;

static int s_page;
static float s_slide;      // px the current page is shifted while swiping or sliding into place
static bool s_swiping;     // this touch is a sideways drag
static bool s_swipeAxis;   // this touch has picked an axis
static bool s_editing;
static int s_editSlot = -1;
static int s_flashSlot = -1;
static u64 s_flashUntil;
static char s_findMessage[128];
static bool s_quit;

// keyboard shortcut builder
static bool s_mods[4];  // cmd, shift, opt, ctrl
static char s_keyName[24], s_keyLabel[24];

// touch tracking
static bool s_touching, s_dragging, s_longFired;
static touchPosition s_touchStart, s_touchNow;
static u64 s_touchAt;
static float s_scrollStart;

// artwork + app and website icons
static Picture s_art;
#define ICON_SLOTS (MAX_PAGES * SLOTS + 8)  // every key on every page, plus a few
typedef struct {
	char id[128];
	int state;     // 1 asked, 2 ready, 3 none, 4 the Mac couldn't be reached: asked again after retryAt
	u64 retryAt;
	Picture pic;
} Icon;
static Icon s_icons[ICON_SLOTS];
static bool s_wasOnline;

// ---- helpers -------------------------------------------------------------------

static bool in(touchPosition t, float x, float y, float w, float h) {
	return t.px >= x && t.px < x + w && t.py >= y && t.py < y + h;
}

static void copy(char *dst, size_t size, const char *src) { snprintf(dst, size, "%s", src ? src : ""); }

// ASCII upper case, for section titles.
static void upper(char *dst, size_t size, const char *src) {
	size_t i = 0;
	for (; src[i] && i + 1 < size; i++) dst[i] = (char)toupper((unsigned char)src[i]);
	dst[i] = 0;
}

static u32 category_color(const char *cat) {
	if (!strcmp(cat, "media")) return C_GREEN;
	if (!strcmp(cat, "system")) return C_TEAL;
	if (!strcmp(cat, "windows")) return C_YELLOW;
	if (!strcmp(cat, "browser")) return C_BLUE;
	if (!strcmp(cat, "editing")) return C_ORANGE;
	if (!strcmp(cat, "accessibility")) return C_PINK;
	if (!strcmp(cat, "mouse")) return C_GRAY;
	return C_PURPLE;
}

// The slot for an icon id, or a free one for it (NULL when they're all taken).
static Icon *icon_slot(const char *id) {
	Icon *free_slot = NULL;
	for (int i = 0; i < ICON_SLOTS; i++) {
		if (s_icons[i].state && !strcmp(s_icons[i].id, id)) return &s_icons[i];
		if (!s_icons[i].state && !free_slot) free_slot = &s_icons[i];
	}
	if (free_slot) copy(free_slot->id, sizeof free_slot->id, id);
	return free_slot;
}

// The icon for a key, asked for the first time it's wanted.
static Icon *icon_for(KeyKind kind, const char *ref) {
	char id[128];
	icon_id(kind, ref, id, sizeof id);
	if (!id[0]) return NULL;
	Icon *icon = icon_slot(id);
	if (icon && (!icon->state || (icon->state == 4 && osGetTime() >= icon->retryAt))) {
		icon->state = 1;
		icon_request(id, false);
	}
	return icon;
}

static void icons_collect(void) {
	char id[128];
	u8 *px;
	int w, h;
	bool retry;
	while (icon_take(id, sizeof id, &px, &w, &h, &retry)) {
		Icon *icon = icon_slot(id);
		if (icon) {
			if (px) {
				if (icon->state == 2) picture_free(&icon->pic);
				icon->state = picture_load(&icon->pic, px, w, h) ? 2 : 3;
			} else if (icon->state != 2) {  // (a newer one that didn't come keeps the old one showing)
				icon->state = retry ? 4 : 3;
				icon->retryAt = osGetTime() + 15000;
			}
		}
		free(px);
	}
}

// Asks for every app and website icon on the pad, so they're there before a page is shown. The
// current page's go last: the icon thread serves the newest request first. Cheap once they're in.
// fresh fetches them all again from the Mac (the menu's Refresh).
static void icons_prefetch(bool fresh) {
	for (int n = g_pads.count - 1; n >= 0; n--) {
		PadPage *p = &g_pads.pages[(s_page + n) % g_pads.count];
		for (int slot = 0; slot < SLOTS; slot++) {
			PadKey *k = &p->keys[slot];
			if (k->kind != KEY_APP && k->kind != KEY_WEB) continue;
			Icon *icon = icon_for(k->kind, k->ref);
			if (icon && fresh && icon->state != 1) icon_request(icon->id, true);
		}
	}
}

// Icons that couldn't be fetched get another go once the Mac is back.
static void icons_retry(void) {
	for (int i = 0; i < ICON_SLOTS; i++) if (s_icons[i].state >= 3) s_icons[i].state = 0;
	icons_prefetch(false);
}

static bool ask_text_opt(const char *hint, const char *initial, SwkbdType type, bool required, char *out, size_t size) {
	SwkbdState kb;
	swkbdInit(&kb, type, 2, size - 1);
	swkbdSetHintText(&kb, hint);
	if (initial) swkbdSetInitialText(&kb, initial);
	if (required) swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
	if (type == SWKBD_TYPE_NUMPAD) swkbdSetNumpadKeys(&kb, '.', ':');
	return swkbdInputText(&kb, out, size) == SWKBD_BUTTON_CONFIRM;
}

static bool ask_text(const char *hint, const char *initial, SwkbdType type, char *out, size_t size) {
	return ask_text_opt(hint, initial, type, true, out, size);
}

// Whether hay contains needle, ignoring case.
static bool contains_ci(const char *hay, const char *needle) {
	size_t n = strlen(needle);
	if (!n) return true;
	for (; *hay; hay++) {
		size_t i = 0;
		while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
		if (i == n) return true;
	}
	return false;
}

static PadKey *edit_key(void) { return &g_pads.pages[s_page].keys[s_editSlot]; }

// ---- which icon an action gets -----------------------------------------------------

// The built-in actions, by id.
static const struct { const char *id; Glyph g; } s_actionGlyphs[] = {
	{"next-track", GLYPH_NEXT}, {"previous-track", GLYPH_PREV},
	{"volume-up", GLYPH_VOL_UP}, {"volume-down", GLYPH_VOL_DOWN}, {"mute", GLYPH_MUTE},
	{"emoji", GLYPH_SMILE}, {"spotlight", GLYPH_SEARCH}, {"screenshot-ui", GLYPH_CAMERA},
	{"screenshot-area", GLYPH_CROP}, {"screenshot-screen", GLYPH_MONITOR}, {"mission-control", GLYPH_GRID},
	{"app-windows", GLYPH_WINDOWS}, {"lock-screen", GLYPH_LOCK},
	{"minimize", GLYPH_MINIMIZE}, {"fullscreen", GLYPH_EXPAND}, {"close-window", GLYPH_CLOSE},
	{"next-window", GLYPH_WINDOWS}, {"hide-app", GLYPH_MINUS_CIRCLE}, {"quit-app", GLYPH_POWER},
	{"reload", GLYPH_REFRESH}, {"hard-reload", GLYPH_REFRESH}, {"new-tab", GLYPH_TAB}, {"close-tab", GLYPH_CLOSE},
	{"reopen-tab", GLYPH_UNDO}, {"next-tab", GLYPH_ARROW_RIGHT}, {"previous-tab", GLYPH_ARROW_LEFT},
	{"address-bar", GLYPH_ADDRESS},
	{"copy", GLYPH_COPY}, {"paste", GLYPH_PASTE}, {"cut", GLYPH_CUT}, {"undo", GLYPH_UNDO}, {"redo", GLYPH_REDO},
	{"select-all", GLYPH_SELECT_ALL}, {"find", GLYPH_SEARCH}, {"save", GLYPH_SAVE},
	{"window-left", GLYPH_HALF_LEFT}, {"window-right", GLYPH_HALF_RIGHT}, {"window-top", GLYPH_HALF_TOP},
	{"window-bottom", GLYPH_HALF_BOTTOM}, {"window-top-left", GLYPH_QUARTER_TL}, {"window-top-right", GLYPH_QUARTER_TR},
	{"window-bottom-left", GLYPH_QUARTER_BL}, {"window-bottom-right", GLYPH_QUARTER_BR},
	{"window-maximize", GLYPH_FILL_SCREEN}, {"window-center", GLYPH_CENTER}, {"window-next-display", GLYPH_MONITOR},
	{"window-restore", GLYPH_UNDO},
	{"keep-awake", GLYPH_COFFEE}, {"jiggler", GLYPH_MOUSE}, {"siri", GLYPH_MIC},
	{"brightness-up", GLYPH_SUN}, {"brightness-down", GLYPH_SUN_LOW}, {"dark-mode", GLYPH_MOON},
	{"sleep-display", GLYPH_MONITOR}, {"sleep", GLYPH_MOON}, {"screen-saver", GLYPH_MONITOR},
	{"show-desktop", GLYPH_MONITOR}, {"force-quit", GLYPH_CLOSE},
	{"accessibility-shortcuts", GLYPH_PERSON}, {"voiceover", GLYPH_SPEAK}, {"zoom", GLYPH_ZOOM_IN},
	{"zoom-in", GLYPH_ZOOM_IN}, {"zoom-out", GLYPH_ZOOM_OUT}, {"invert-colors", GLYPH_INVERT},
	{"speak-selection", GLYPH_SPEAK}, {"left-click", GLYPH_CLICK_LEFT}, {"right-click", GLYPH_CLICK_RIGHT},
	{"wifi", GLYPH_WIFI}, {"bluetooth", GLYPH_BLUETOOTH}, {"stage-manager", GLYPH_STAGE}, {"hidden-files", GLYPH_EYE},
	{"empty-trash", GLYPH_TRASH}, {"eject-disks", GLYPH_EJECT}, {"restart", GLYPH_REFRESH}, {"shut-down", GLYPH_POWER},
};

// Material Symbols names the Mac suggests, for actions made there.
static const struct { const char *name; Glyph g; } s_iconGlyphs[] = {
	{"play_pause", GLYPH_PLAYPAUSE}, {"play_arrow", GLYPH_PLAY}, {"pause", GLYPH_PAUSE},
	{"skip_next", GLYPH_NEXT}, {"skip_previous", GLYPH_PREV},
	{"volume_up", GLYPH_VOL_UP}, {"volume_down", GLYPH_VOL_DOWN}, {"volume_off", GLYPH_MUTE}, {"volume_mute", GLYPH_MUTE},
	{"mood", GLYPH_SMILE}, {"sentiment_satisfied", GLYPH_SMILE}, {"search", GLYPH_SEARCH},
	{"photo_camera", GLYPH_CAMERA}, {"camera", GLYPH_CAMERA}, {"screenshot", GLYPH_CROP}, {"crop", GLYPH_CROP},
	{"screenshot_monitor", GLYPH_MONITOR}, {"desktop_mac", GLYPH_MONITOR}, {"monitor", GLYPH_MONITOR},
	{"grid_view", GLYPH_GRID}, {"apps", GLYPH_GRID}, {"dashboard", GLYPH_GRID}, {"web_asset", GLYPH_WINDOWS},
	{"lock", GLYPH_LOCK}, {"lock_open", GLYPH_LOCK},
	{"arrow_downward", GLYPH_MINIMIZE}, {"open_in_new", GLYPH_EXPAND}, {"fullscreen", GLYPH_EXPAND},
	{"close", GLYPH_CLOSE}, {"cancel", GLYPH_CLOSE}, {"refresh", GLYPH_REFRESH}, {"sync", GLYPH_REFRESH},
	{"remove_circle_outline", GLYPH_MINUS_CIRCLE}, {"remove_circle", GLYPH_MINUS_CIRCLE}, {"visibility_off", GLYPH_MINUS_CIRCLE},
	{"power_settings_new", GLYPH_POWER}, {"power", GLYPH_POWER}, {"logout", GLYPH_POWER},
	{"tab", GLYPH_TAB}, {"add_box", GLYPH_TAB}, {"folder", GLYPH_TAB},
	{"undo", GLYPH_UNDO}, {"redo", GLYPH_REDO}, {"arrow_forward", GLYPH_ARROW_RIGHT}, {"arrow_back", GLYPH_ARROW_LEFT},
	{"link", GLYPH_ADDRESS}, {"language", GLYPH_GLOBE}, {"public", GLYPH_GLOBE},
	{"content_copy", GLYPH_COPY}, {"content_paste", GLYPH_PASTE}, {"content_cut", GLYPH_CUT},
	{"select_all", GLYPH_SELECT_ALL}, {"save_alt", GLYPH_SAVE}, {"save", GLYPH_SAVE},
	{"keyboard", GLYPH_KEYS}, {"schedule", GLYPH_CLOCK}, {"laptop_mac", GLYPH_LAPTOP}, {"edit", GLYPH_PENCIL},
	{"delete", GLYPH_TRASH}, {"add", GLYPH_PLUS}, {"check", GLYPH_CHECK}, {"bolt", GLYPH_BOLT}, {"flash_on", GLYPH_BOLT},
	{"terminal", GLYPH_KEYS}, {"shortcut", GLYPH_SHORTCUT}, {"app", GLYPH_APP},
	{"splitscreen_left", GLYPH_HALF_LEFT}, {"splitscreen_right", GLYPH_HALF_RIGHT},
	{"splitscreen_top", GLYPH_HALF_TOP}, {"splitscreen_bottom", GLYPH_HALF_BOTTOM},
	{"north_west", GLYPH_QUARTER_TL}, {"north_east", GLYPH_QUARTER_TR}, {"south_west", GLYPH_QUARTER_BL},
	{"south_east", GLYPH_QUARTER_BR}, {"fit_screen", GLYPH_FILL_SCREEN}, {"filter_center_focus", GLYPH_CENTER},
	{"restore", GLYPH_UNDO}, {"coffee", GLYPH_COFFEE}, {"mouse", GLYPH_MOUSE}, {"mic", GLYPH_MIC},
	{"brightness_high", GLYPH_SUN}, {"brightness_low", GLYPH_SUN_LOW}, {"light_mode", GLYPH_SUN},
	{"dark_mode", GLYPH_MOON}, {"bedtime", GLYPH_MOON}, {"desktop_access_disabled", GLYPH_MONITOR},
	{"wallpaper", GLYPH_MONITOR}, {"desktop_windows", GLYPH_MONITOR},
	{"accessibility_new", GLYPH_PERSON}, {"accessibility", GLYPH_PERSON}, {"record_voice_over", GLYPH_SPEAK},
	{"text_to_speech", GLYPH_SPEAK}, {"zoom_in", GLYPH_ZOOM_IN}, {"zoom_out", GLYPH_ZOOM_OUT},
	{"invert_colors", GLYPH_INVERT}, {"left_click", GLYPH_CLICK_LEFT}, {"right_click", GLYPH_CLICK_RIGHT},
	{"sports_esports", GLYPH_GAMEPAD}, {"gamepad", GLYPH_GAMEPAD},
	{"wifi", GLYPH_WIFI}, {"bluetooth", GLYPH_BLUETOOTH}, {"eject", GLYPH_EJECT}, {"visibility", GLYPH_EYE},
	{"restart_alt", GLYPH_REFRESH}, {"view_sidebar", GLYPH_STAGE},
};

static Glyph glyph_for_icon(const char *name) {
	for (size_t i = 0; i < sizeof s_iconGlyphs / sizeof *s_iconGlyphs; i++)
		if (!strcmp(name, s_iconGlyphs[i].name)) return s_iconGlyphs[i].g;
	return GLYPH_NONE;
}

static Glyph action_glyph(const char *id, const char *icon) {
	if (!strcmp(id, "play-pause")) return s_st.hasTrack && s_st.playing ? GLYPH_PAUSE : GLYPH_PLAY;
	for (size_t i = 0; i < sizeof s_actionGlyphs / sizeof *s_actionGlyphs; i++)
		if (!strcmp(id, s_actionGlyphs[i].id)) return s_actionGlyphs[i].g;
	Glyph g = icon ? glyph_for_icon(icon) : GLYPH_NONE;
	return g != GLYPH_NONE ? g : GLYPH_BOLT;
}

// ---- how a key looks ---------------------------------------------------------------

// The icons a key can be given in "How it looks": the name saved in pads.json, the list's label, the group.
typedef struct { const char *id, *label; Glyph g; int group; } GlyphChoice;
static const GlyphChoice s_glyphChoices[] = {
	{"play_arrow", "Play", GLYPH_PLAY, 0}, {"pause", "Pause", GLYPH_PAUSE, 0}, {"play_pause", "Play / Pause", GLYPH_PLAYPAUSE, 0},
	{"skip_next", "Next", GLYPH_NEXT, 0}, {"skip_previous", "Previous", GLYPH_PREV, 0},
	{"volume_up", "Volume up", GLYPH_VOL_UP, 0}, {"volume_down", "Volume down", GLYPH_VOL_DOWN, 0}, {"volume_off", "Mute", GLYPH_MUTE, 0},
	{"mood", "Smile", GLYPH_SMILE, 1}, {"search", "Search", GLYPH_SEARCH, 1}, {"photo_camera", "Camera", GLYPH_CAMERA, 1},
	{"crop", "Crop", GLYPH_CROP, 1}, {"monitor", "Screen", GLYPH_MONITOR, 1}, {"grid_view", "Grid", GLYPH_GRID, 1},
	{"web_asset", "Windows", GLYPH_WINDOWS, 1}, {"lock", "Lock", GLYPH_LOCK, 1}, {"power", "Power", GLYPH_POWER, 1},
	{"laptop_mac", "Laptop", GLYPH_LAPTOP, 1}, {"schedule", "Clock", GLYPH_CLOCK, 1}, {"keyboard", "Keyboard", GLYPH_KEYS, 1},
	{"app", "App", GLYPH_APP, 1}, {"coffee", "Coffee", GLYPH_COFFEE, 1}, {"mic", "Microphone", GLYPH_MIC, 1},
	{"brightness_high", "Brightness", GLYPH_SUN, 1}, {"dark_mode", "Moon", GLYPH_MOON, 1},
	{"accessibility_new", "Accessibility", GLYPH_PERSON, 1}, {"record_voice_over", "Speech", GLYPH_SPEAK, 1},
	{"zoom_in", "Zoom in", GLYPH_ZOOM_IN, 1}, {"zoom_out", "Zoom out", GLYPH_ZOOM_OUT, 1},
	{"mouse", "Mouse", GLYPH_MOUSE, 1}, {"sports_esports", "Controller", GLYPH_GAMEPAD, 1},
	{"wifi", "Wi-Fi", GLYPH_WIFI, 1}, {"bluetooth", "Bluetooth", GLYPH_BLUETOOTH, 1}, {"eject", "Eject", GLYPH_EJECT, 1},
	{"visibility", "Eye", GLYPH_EYE, 1},
	{"arrow_downward", "Minimize", GLYPH_MINIMIZE, 2}, {"fullscreen", "Expand", GLYPH_EXPAND, 2}, {"close", "Close", GLYPH_CLOSE, 2},
	{"refresh", "Refresh", GLYPH_REFRESH, 2}, {"remove_circle", "Minus", GLYPH_MINUS_CIRCLE, 2}, {"tab", "Tab", GLYPH_TAB, 2},
	{"arrow_back", "Arrow left", GLYPH_ARROW_LEFT, 2}, {"arrow_forward", "Arrow right", GLYPH_ARROW_RIGHT, 2},
	{"link", "Address bar", GLYPH_ADDRESS, 2}, {"language", "Globe", GLYPH_GLOBE, 2},
	{"splitscreen_left", "Left half", GLYPH_HALF_LEFT, 2}, {"splitscreen_right", "Right half", GLYPH_HALF_RIGHT, 2},
	{"fit_screen", "Fill screen", GLYPH_FILL_SCREEN, 2},
	{"content_copy", "Copy", GLYPH_COPY, 3}, {"content_paste", "Paste", GLYPH_PASTE, 3}, {"content_cut", "Cut", GLYPH_CUT, 3},
	{"select_all", "Select all", GLYPH_SELECT_ALL, 3}, {"undo", "Undo", GLYPH_UNDO, 3}, {"redo", "Redo", GLYPH_REDO, 3},
	{"save", "Save", GLYPH_SAVE, 3}, {"edit", "Pencil", GLYPH_PENCIL, 3}, {"delete", "Trash", GLYPH_TRASH, 3},
	{"add", "Plus", GLYPH_PLUS, 3}, {"check", "Check", GLYPH_CHECK, 3}, {"bolt", "Bolt", GLYPH_BOLT, 3},
	{"shortcut", "Shortcut", GLYPH_SHORTCUT, 3},
};
static const char *s_glyphGroups[] = {"Media", "System", "Windows and browser", "Editing and more"};

static const struct { const char *id, *label; } s_colorChoices[] = {
	{"blue", "Blue"}, {"green", "Green"}, {"orange", "Orange"}, {"red", "Red"}, {"purple", "Purple"},
	{"yellow", "Yellow"}, {"teal", "Teal"}, {"pink", "Pink"}, {"gray", "Gray"},
};

static const GlyphChoice *glyph_choice(const char *id) {
	if (!id[0]) return NULL;
	for (size_t i = 0; i < sizeof s_glyphChoices / sizeof *s_glyphChoices; i++)
		if (!strcmp(id, s_glyphChoices[i].id)) return &s_glyphChoices[i];
	return NULL;
}

static const char *glyph_choice_label(const char *id) {
	const GlyphChoice *c = glyph_choice(id);
	return c ? c->label : "Automatic";
}

// 0 for a name that isn't one of the choices.
static u32 color_from(const char *id) {
	if (!strcmp(id, "blue")) return C_BLUE;
	if (!strcmp(id, "green")) return C_GREEN;
	if (!strcmp(id, "orange")) return C_ORANGE;
	if (!strcmp(id, "red")) return C_RED;
	if (!strcmp(id, "purple")) return C_PURPLE;
	if (!strcmp(id, "yellow")) return C_YELLOW;
	if (!strcmp(id, "teal")) return C_TEAL;
	if (!strcmp(id, "pink")) return C_PINK;
	if (!strcmp(id, "gray")) return C_GRAY;
	return 0;
}

static const char *color_choice_label(const char *id) {
	for (size_t i = 0; i < sizeof s_colorChoices / sizeof *s_colorChoices; i++)
		if (!strcmp(id, s_colorChoices[i].id)) return s_colorChoices[i].label;
	return "Automatic";
}

// The key's color: the one picked, else one for what it does.
static u32 key_accent(const PadKey *k) {
	u32 c = color_from(k->color);
	if (c) return c;
	switch (k->kind) {
	case KEY_ACTION: return category_color(k->category);
	case KEY_APP: return C_BLUE;
	case KEY_WEB: return C_TEAL;
	case KEY_KEYS: return C_ORANGE;
	case KEY_SHORTCUT: return C_PURPLE;
	default: return C_GRAY;
	}
}

// The glyph a key shows on its own (apps and websites show their own icon instead when they have one).
static Glyph kind_glyph(const PadKey *k) {
	switch (k->kind) {
	case KEY_ACTION: return action_glyph(k->ref, k->icon);
	case KEY_APP: return GLYPH_APP;
	case KEY_WEB: return GLYPH_GLOBE;
	case KEY_KEYS: return GLYPH_KEYS;
	case KEY_SHORTCUT: return GLYPH_SHORTCUT;
	default: return GLYPH_NONE;
	}
}

static const char *s_kindTitles[] = {"", "Mac action", "App", "Website", "Keyboard shortcut", "Shortcut"};

static void key_tile(const PadKey *k, float x, float y, bool flash);

// ---- lists -----------------------------------------------------------------------

static void list_open(ListPurpose purpose, const char *title) {
	free(s_list.items);
	memset(&s_list, 0, sizeof s_list);
	s_list.purpose = purpose;
	s_list.refreshRow = -1;
	s_list.top = LIST_TOP;
	copy(s_list.title, sizeof s_list.title, title);
	s_screen = SCR_LIST;
}

static Item *list_add(RowStyle style, const char *label, const char *value, int id) {
	s_list.items = realloc(s_list.items, (s_list.count + 1) * sizeof(Item));
	Item *it = &s_list.items[s_list.count++];
	memset(it, 0, sizeof *it);
	it->style = style;
	copy(it->label, sizeof it->label, label);
	copy(it->value, sizeof it->value, value);
	it->id = id;
	return it;
}

// A section title, in capitals. An empty one is a gap between cards.
static void list_header(const char *title) {
	char up[64];
	upper(up, sizeof up, title);
	list_add(ROW_HEADER, up, "", 0);
}

static bool pickable(int i) { return i >= 0 && i < s_list.count && s_list.items[i].style != ROW_HEADER; }

static float list_view(void) { return SCREEN_H - s_list.top; }

static float list_max_scroll(void) {
	float h = s_list.height - list_view();
	return h > 0 ? h : 0;
}

static void list_clamp(void) {
	if (s_list.scroll < 0) s_list.scroll = 0;
	if (s_list.scroll > list_max_scroll()) s_list.scroll = list_max_scroll();
}

// Works out where every row sits, and settles the selection on a row that can be picked.
static void list_layout(void) {
	float y = 6;
	bool inCard = false;
	for (int i = 0; i < s_list.count; i++) {
		Item *it = &s_list.items[i];
		if (it->style == ROW_HEADER) {
			it->y = y;
			it->h = it->label[0] ? HEAD_H + (inCard ? 4 : 0) : BREAK_H;
			y += it->h;
			inCard = false;
			continue;
		}
		if (it->style == ROW_SEARCH) {  // a pill of its own
			it->first = it->last = true;
			it->y = y;
			it->h = ROW_H;
			y += it->h;
			inCard = false;
			continue;
		}
		it->first = !inCard;
		it->last = true;
		if (inCard) s_list.items[i - 1].last = false;
		it->y = y;
		it->h = it->sub[0] ? ROW2_H : ROW_H;
		y += it->h;
		inCard = true;
	}
	s_list.height = y + 8;
	list_clamp();
	if (!pickable(s_list.sel)) {
		// the next row that can be picked, else the first
		int i = s_list.sel < 0 ? 0 : s_list.sel;
		while (i < s_list.count && !pickable(i)) i++;
		if (i >= s_list.count) for (i = 0; i < s_list.count && !pickable(i); i++) {}
		s_list.sel = i;
	}
}

static void list_show_sel(void) {
	if (!pickable(s_list.sel)) return;
	Item *it = &s_list.items[s_list.sel];
	float top = it->y - (it->first ? HEAD_H : 0), bottom = it->y + it->h + 6;
	if (top < s_list.scroll) s_list.scroll = top;
	if (bottom > s_list.scroll + list_view()) s_list.scroll = bottom - list_view();
	list_clamp();
}

// Moves the selection by step rows that can be picked.
static void list_move(int step) {
	int dir = step < 0 ? -1 : 1, i = s_list.sel;
	for (int n = abs(step); n > 0; n--) {
		int j = i + dir;
		while (j >= 0 && j < s_list.count && !pickable(j)) j += dir;
		if (j < 0 || j >= s_list.count) break;
		i = j;
	}
	s_list.sel = i;
	list_show_sel();
}

static void menu_build(void);
static void list_search(void);

static bool is_catalog(void) {
	return s_list.purpose == LIST_ACTIONS || s_list.purpose == LIST_APPS || s_list.purpose == LIST_SHORTCUTS;
}

// Lists with a search field and the magnifier in the title bar.
static bool searchable(void) { return is_catalog() || s_list.purpose == LIST_KIND || s_list.purpose == LIST_SEARCH; }

static void catalogs_finish(void) {
	for (int k = 0; k < 3; k++) catalog_finish(&s_cats[k]);
	s_wantCats = 0;
}

// The field at the top of a list. Shows the query when there is one, else what it searches.
static void add_search_row(const char *prompt) {
	Item *it = list_add(ROW_SEARCH, s_list.filter[0] ? s_list.filter : prompt, "search", -1);
	it->on = s_list.filter[0] != 0;
	list_header("");
}

// The editor's first question.
static void open_kind_menu(void) {
	PadKey *k = edit_key();
	list_open(LIST_KIND, k->kind ? k->label : "New key");
	add_search_row("Search actions, apps, Shortcuts");
	Item *it;
	it = list_add(ROW_CHEVRON, "Mac action", "", KEY_ACTION);
	copy(it->sub, sizeof it->sub, "Media, windows, Siri and more");
	it->icon = GLYPH_BOLT;
	it->color = C_GREEN;
	it = list_add(ROW_CHEVRON, "Open an app", "", KEY_APP);
	copy(it->sub, sizeof it->sub, "Any app on your Mac");
	it->icon = GLYPH_APP;
	it->color = C_BLUE;
	it = list_add(ROW_CHEVRON, "Open a website", "", KEY_WEB);
	copy(it->sub, sizeof it->sub, "In your Mac's browser");
	it->icon = GLYPH_GLOBE;
	it->color = C_TEAL;
	it = list_add(ROW_CHEVRON, "Keyboard shortcut", "", KEY_KEYS);
	copy(it->sub, sizeof it->sub, "Like Cmd + Shift + T");
	it->icon = GLYPH_KEYS;
	it->color = C_ORANGE;
	it = list_add(ROW_CHEVRON, "Run a Shortcut", "", KEY_SHORTCUT);
	copy(it->sub, sizeof it->sub, "From the Shortcuts app");
	it->icon = GLYPH_SHORTCUT;
	it->color = C_PURPLE;
	if (k->kind) {
		list_header("This key");
		it = list_add(ROW_CHEVRON, "How it looks", "", 100);
		copy(it->detail, sizeof it->detail, k->label);
		it->icon = GLYPH_PENCIL;
		it->color = C_GRAY;
		it = list_add(ROW_PLAIN, "Remove this key", "", 101);
		it->icon = GLYPH_TRASH;
		it->color = C_RED;
		it->danger = true;
	}
	list_layout();
}

static void list_fill_catalog(void);

// "How it looks": the key as it will show, then its name, icon and color. A key lands here right
// after what it does is picked, and from "This key" in the long-press menu.
static void open_look(void) {
	PadKey *k = edit_key();
	list_open(LIST_LOOK, "How it looks");
	s_list.top = LIST_TOP + LOOK_PREVIEW_H;
	Item *it;
	it = list_add(ROW_CHEVRON, "Name", "name", 0);
	copy(it->detail, sizeof it->detail, k->label);
	it->icon = GLYPH_PENCIL;
	it->color = C_GRAY;
	it = list_add(ROW_CHEVRON, "Icon", "icon", 0);
	copy(it->detail, sizeof it->detail, glyph_choice_label(k->glyph));
	const GlyphChoice *chosen = glyph_choice(k->glyph);
	it->icon = chosen ? chosen->g : kind_glyph(k);
	it->color = key_accent(k);
	it = list_add(ROW_CHEVRON, "Color", "color", 0);
	copy(it->detail, sizeof it->detail, color_choice_label(k->color));
	it->swatch = true;
	it->color = key_accent(k);
	list_header("");
	it = list_add(ROW_CHEVRON, "What it does", "kind", 0);
	copy(it->detail, sizeof it->detail, s_kindTitles[k->kind]);
	it->icon = GLYPH_BOLT;
	it->color = C_GREEN;
	it = list_add(ROW_PLAIN, "Remove this key", "remove", 0);
	it->icon = GLYPH_TRASH;
	it->color = C_RED;
	it->danger = true;
	list_header("");
	it = list_add(ROW_PLAIN, "Done", "done", 0);
	it->icon = GLYPH_CHECK;
	it->color = C_BLUE;
	list_layout();
}

// Starts on the row that's checked.
static void list_select_checked(void) {
	for (int i = 0; i < s_list.count; i++) if (s_list.items[i].style == ROW_CHECK && s_list.items[i].on) { s_list.sel = i; break; }
	list_show_sel();
}

static void open_glyph_list(void) {
	PadKey *k = edit_key();
	u32 accent = key_accent(k);
	list_open(LIST_GLYPH, "Icon");
	Item *it = list_add(ROW_CHECK, "Automatic", "", 0);
	copy(it->sub, sizeof it->sub, "The app's or site's own, or one for the action");
	it->icon = kind_glyph(k);
	it->color = accent;
	it->on = !k->glyph[0];
	for (int g = 0; g < 4; g++) {
		list_header(s_glyphGroups[g]);
		for (size_t i = 0; i < sizeof s_glyphChoices / sizeof *s_glyphChoices; i++) {
			const GlyphChoice *c = &s_glyphChoices[i];
			if (c->group != g) continue;
			it = list_add(ROW_CHECK, c->label, c->id, 0);
			it->icon = c->g;
			it->color = accent;
			it->on = !strcmp(k->glyph, c->id);
		}
	}
	list_layout();
	list_select_checked();
}

static void open_color_list(void) {
	PadKey *k = edit_key();
	PadKey plain = *k;
	plain.color[0] = 0;
	list_open(LIST_COLOR, "Color");
	Item *it = list_add(ROW_CHECK, "Automatic", "", 0);
	copy(it->sub, sizeof it->sub, "One for what the key does");
	it->swatch = true;
	it->color = key_accent(&plain);
	it->on = !k->color[0];
	list_header("");
	for (size_t i = 0; i < sizeof s_colorChoices / sizeof *s_colorChoices; i++) {
		it = list_add(ROW_CHECK, s_colorChoices[i].label, s_colorChoices[i].id, 0);
		it->swatch = true;
		it->color = color_from(s_colorChoices[i].id);
		it->on = !strcmp(k->color, s_colorChoices[i].id);
	}
	list_layout();
	list_select_checked();
}

// Which catalog a list shows, or -1 for all three.
static int list_catalog(void) {
	return s_list.purpose == LIST_ACTIONS ? CATALOG_ACTIONS : s_list.purpose == LIST_APPS ? CATALOG_APPS
	     : s_list.purpose == LIST_SHORTCUTS ? CATALOG_SHORTCUTS : -1;
}

// The rows once every catalog the list wants is in; until then, "Asking your Mac…".
static void list_catalogs_ready(void) {
	bool done = true;
	for (int k = 0; k < 3; k++) if ((s_wantCats >> k & 1) && !s_cats[k].done) done = false;
	s_list.loading = !done;
	if (done) list_fill_catalog();
}

// Usually from the cache, so the rows are there at once.
static void open_catalog(ListPurpose purpose, CatalogKind kind, const char *title) {
	list_open(purpose, title);
	s_wantCats = 1 << kind;
	catalog_start(&s_cats[kind], kind);
	list_catalogs_ready();
}

// Everything that matches query, from all three catalogs.
static void open_search(const char *query) {
	list_open(LIST_SEARCH, "Search");
	copy(s_list.filter, sizeof s_list.filter, query);
	s_wantCats = 7;
	for (int k = 0; k < 3; k++) catalog_start(&s_cats[k], k);
	list_catalogs_ready();
}

// "Updated 3 min ago", "Refreshing…", or what went wrong, for one catalog or all three (kind -1).
static void refresh_detail(int kind, char *out, size_t size) {
	bool fetching = false, failed = false;
	s64 age = -1;
	char error[96] = "";
	for (int k = 0; k < 3; k++) {
		if (kind >= 0 && k != kind) continue;
		CatalogInfo info;
		catalog_info(k, &info);
		fetching |= info.fetching;
		if (info.failed && !failed) {
			failed = true;
			copy(error, sizeof error, info.error);
		}
		if (info.ageSec > age) age = info.ageSec;  // the oldest of them
	}
	if (fetching) copy(out, size, "Refreshing\xE2\x80\xA6");
	else if (failed) copy(out, size, error);
	else if (age < 0) copy(out, size, "Not yet");
	else if (age < 60) copy(out, size, "Updated just now");
	else if (age < 3600) snprintf(out, size, "Updated %d min ago", (int)(age / 60));
	else if (age < 86400) snprintf(out, size, "Updated %d h ago", (int)(age / 3600));
	else snprintf(out, size, "Updated %d days ago", (int)(age / 86400));
}

// Keeps the Refresh row's text up to date while a list is open.
static void refresh_row_update(void) {
	if (s_list.refreshRow < 0 || s_list.refreshRow >= s_list.count) return;
	Item *it = &s_list.items[s_list.refreshRow];
	if (s_list.purpose == LIST_MENU) {
		char detail[32];
		refresh_detail(-1, detail, sizeof detail);
		snprintf(it->sub, sizeof it->sub, "Lists and icons \xC2\xB7 %s", detail);
	} else {
		refresh_detail(list_catalog(), it->detail, sizeof it->detail);
	}
}

// Asks the Mac for this list's catalogs again (and, from the menu, the pad's icons).
static void refresh_picked(void) {
	catalog_refresh(list_catalog());
	if (s_list.purpose == LIST_MENU) icons_prefetch(true);
	refresh_row_update();
}

static void open_menu(void) {
	list_open(LIST_MENU, "Menu");
	menu_build();
}

// ---- the Mac's switches: Keep Mac awake and Mouse jiggler ----------------------------------

static const char *s_switchSteps[2] = {"keepAwake", "jiggler"};
// One just flipped shows its new state until the Mac's status agrees, or a few seconds pass.
static int s_switchWant[2] = {-1, -1};
static u64 s_switchAt[2];

static bool switch_on(int i) {
	bool now = i == 0 ? s_st.keepAwake : s_st.jiggler;
	if (s_switchWant[i] >= 0 && now != s_switchWant[i] && osGetTime() - s_switchAt[i] < 5000) return s_switchWant[i];
	s_switchWant[i] = -1;
	return now;
}

static void switch_flip(int i) {
	if (s_st.live && !s_st.hasSwitches) {
		toast_set("Update Macapad on your Mac to use this.", true);
		return;
	}
	bool on = !switch_on(i);
	press_switch(s_switchSteps[i], on);
	s_switchWant[i] = on;
	s_switchAt[i] = osGetTime();
}

// The line under a switch: what it does, or until when it's on.
static void switch_sub(int i, char *out, size_t size) {
	s64 until = i == 0 ? s_st.keepAwakeUntil : s_st.jigglerUntil;
	if (s_st.live && !s_st.hasSwitches) {
		copy(out, size, "Update Macapad on your Mac");
	} else if (switch_on(i) && until && s_switchWant[i] < 0) {
		// in the Mac's time, like the clock
		time_t t = until / 1000 + s_st.utcOffset;
		struct tm *tm = gmtime(&t);
		int hour = tm->tm_hour;
		if (g_config.clock24) snprintf(out, size, "On until %02d:%02d", hour, tm->tm_min);
		else snprintf(out, size, "On until %d:%02d %s", hour % 12 ? hour % 12 : 12, tm->tm_min, hour < 12 ? "AM" : "PM");
	} else {
		copy(out, size, i == 0 ? "Stops the screen from sleeping" : "So you never show as away");
	}
}

// Keeps the menu's switches current while it's open.
static void switches_update(void) {
	for (int n = 0; n < s_list.count; n++) {
		Item *it = &s_list.items[n];
		int i = !strcmp(it->value, "awake") ? 0 : !strcmp(it->value, "jiggler") ? 1 : -1;
		if (i < 0) continue;
		it->on = switch_on(i);
		switch_sub(i, it->sub, sizeof it->sub);
	}
}

// The START menu. Built again in place when something in it changes, so the scroll stays put.
static void menu_build(void) {
	free(s_list.items);
	s_list.items = NULL;
	s_list.count = 0;
	Item *it;

	list_header("Control your Mac");
	it = list_add(ROW_CHEVRON, "Use as a controller", "controller", 0);
	copy(it->detail, sizeof it->detail, preset_name(g_controller.preset));
	it->icon = GLYPH_GAMEPAD;
	it->color = C_GREEN;
	it = list_add(ROW_CHEVRON, "Type on your Mac", "type", 0);
	it->icon = GLYPH_KEYS;
	it->color = C_ORANGE;
	it = list_add(ROW_TOGGLE, "Keep Mac awake", "awake", 0);
	it->icon = GLYPH_COFFEE;
	it->color = C_TEAL;
	it = list_add(ROW_TOGGLE, "Mouse jiggler", "jiggler", 0);
	it->icon = GLYPH_MOUSE;
	it->color = C_TEAL;
	switches_update();

	list_header("Pages");
	for (int i = 0; i < g_pads.count; i++) {
		it = list_add(ROW_CHECK, g_pads.pages[i].name, "page", i);
		snprintf(it->tag, sizeof it->tag, "%d", i + 1);
		it->color = i == s_page ? C_BLUE : C_GRAY;
		it->on = i == s_page;
	}
	if (g_pads.count < MAX_PAGES) {
		it = list_add(ROW_PLAIN, "Add a page", "addpage", 0);
		it->icon = GLYPH_PLUS;
		it->color = C_GREEN;
	}

	list_header(g_pads.pages[s_page].name);
	it = list_add(ROW_TOGGLE, "Edit keys", "edit", 0);
	it->icon = GLYPH_PENCIL;
	it->color = C_ORANGE;
	it->on = s_editing;
	it = list_add(ROW_CHEVRON, "Rename page", "renamepage", 0);
	copy(it->detail, sizeof it->detail, g_pads.pages[s_page].name);
	copy(it->tag, sizeof it->tag, "Aa");
	it->color = C_PURPLE;
	if (g_pads.count > 1) {
		it = list_add(ROW_PLAIN, "Remove page", "removepage", 0);
		it->icon = GLYPH_TRASH;
		it->color = C_RED;
		it->danger = true;
	}

	list_header("Clock");
	it = list_add(ROW_TOGGLE, "24-hour time", "clock", 0);
	it->icon = GLYPH_CLOCK;
	it->color = C_BLUE;
	it->on = g_config.clock24;

	list_header("Your Mac");
	it = list_add(ROW_PLAIN, "Forget this Mac", "forget", 0);
	{
		char host[64], sub[96];
		int port;
		if (url_split(g_config.url, host, sizeof host, &port)) snprintf(sub, sizeof sub, "%s \xC2\xB7 %s", g_config.macName, host);
		else copy(sub, sizeof sub, g_config.macName);
		copy(it->sub, sizeof it->sub, sub);
	}
	it->icon = GLYPH_LAPTOP;
	it->color = C_RED;
	it->danger = true;
	it = list_add(ROW_PLAIN, "Refresh from your Mac", "refresh", -2);
	it->icon = GLYPH_REFRESH;
	it->color = C_GRAY;
	s_list.refreshRow = s_list.count - 1;
	refresh_row_update();

	list_header("");
	it = list_add(ROW_PLAIN, "Quit Macapad", "quit", 0);
	it->icon = GLYPH_POWER;
	it->color = C_GRAY;

	list_layout();
}

static const char *s_keyNames[][2] = {
	{"return", "Return"}, {"space", "Space"}, {"tab", "Tab"}, {"delete", "Delete"}, {"escape", "Escape"},
	{"left", "Left arrow"}, {"right", "Right arrow"}, {"up", "Up arrow"}, {"down", "Down arrow"},
	{"home", "Home"}, {"end", "End"}, {"pageup", "Page Up"}, {"pagedown", "Page Down"},
	{"forwarddelete", "Forward Delete"},
	{"-", "-"}, {"=", "="}, {"[", "["}, {"]", "]"}, {"\\", "\\"}, {";", ";"}, {"'", "'"}, {",", ","},
	{".", "."}, {"/", "/"}, {"`", "`"},
};

static bool s_keyListForPad;  // picking a key for a controller button itself (not for the shortcut builder)

static void open_keyname_list(bool forPad) {
	s_keyListForPad = forPad;
	list_open(LIST_KEYNAME, "Which key?");
	char v[4], l[4];
	list_header("Letters");
	for (char c = 'a'; c <= 'z'; c++) {
		snprintf(v, sizeof v, "%c", c);
		snprintf(l, sizeof l, "%c", c - 32);
		list_add(ROW_PLAIN, l, v, 0);
	}
	list_header("Numbers");
	for (char c = '0'; c <= '9'; c++) {
		snprintf(v, sizeof v, "%c", c);
		list_add(ROW_PLAIN, v, v, 0);
	}
	list_header("Other keys");
	for (size_t i = 0; i < sizeof s_keyNames / sizeof *s_keyNames; i++) list_add(ROW_PLAIN, s_keyNames[i][1], s_keyNames[i][0], 0);
	if (forPad) {
		// held on their own, like Shift for SELECT in a game
		list_header("Shift and the like");
		static const char *mods[][2] = {{"shift", "Shift"}, {"rshift", "Right Shift"}, {"cmd", "Command"}, {"opt", "Option"}, {"ctrl", "Control"}, {"fn", "fn"}};
		for (size_t i = 0; i < sizeof mods / sizeof *mods; i++) list_add(ROW_PLAIN, mods[i][1], mods[i][0], 0);
	}
	list_header("Function keys");
	for (int f = 1; f <= 12; f++) {
		char fv[8], fl[8];
		snprintf(fv, sizeof fv, "f%d", f);
		snprintf(fl, sizeof fl, "F%d", f);
		list_add(ROW_PLAIN, fl, fv, 0);
	}
	list_layout();
}

// ---- the controller's settings -----------------------------------------------------------

static bool s_isNew3ds;      // has ZL, ZR and the C-Stick
static Control s_ctl;        // the button or stick being changed
static bool s_keysForPad;    // the shortcut builder is picking for s_ctl

static const char *s_controlNames[CTL_COUNT] = {
	"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Up", "Down", "Left", "Right", "START", "SELECT", "Circle Pad", "C-Stick",
};

static bool control_exists(Control c) { return s_isNew3ds || (c != CTL_ZL && c != CTL_ZR && c != CTL_CSTICK); }
static bool control_is_stick(Control c) { return c == CTL_CIRCLE || c == CTL_CSTICK; }
static bool control_changed(Control c) { return g_controller.changes[g_controller.preset][c][0] != 0; }

// One key as it's written on it: "x" -> "X", "return" -> "Return", "cmd" -> "Cmd".
static void key_label(const char *name, char *out, size_t size) {
	static const char *more[][2] = {
		{"cmd", "Cmd"}, {"shift", "Shift"}, {"opt", "Opt"}, {"ctrl", "Ctrl"}, {"fn", "fn"}, {"rshift", "Right Shift"}, {"esc", "Esc"}, {"escape", "Esc"},
		{"left", "Left"}, {"right", "Right"}, {"up", "Up"}, {"down", "Down"},
	};
	for (size_t i = 0; i < sizeof more / sizeof *more; i++)
		if (!strcmp(name, more[i][0])) { copy(out, size, more[i][1]); return; }
	for (size_t i = 0; i < sizeof s_keyNames / sizeof *s_keyNames; i++)
		if (!strcmp(name, s_keyNames[i][0])) { copy(out, size, s_keyNames[i][1]); return; }
	copy(out, size, name);
	if (out[0]) out[0] = (char)toupper((unsigned char)out[0]);  // "x" -> "X", "f5" -> "F5"
}

// What a binding does, in words: "Left click", "Cmd + Tab", "Move the pointer".
static void binding_label(const char *b, char *out, size_t size) {
	static const char *named[][2] = {
		{"none", "Nothing"}, {"click:left", "Left click"}, {"click:right", "Right click"}, {"click:middle", "Middle click"},
		{"pointer", "Move the pointer"}, {"scroll", "Scroll"}, {"arrows", "Arrow keys"}, {"wasd", "W A S D keys"},
	};
	for (size_t i = 0; i < sizeof named / sizeof *named; i++)
		if (!strcmp(b, named[i][0])) { copy(out, size, named[i][1]); return; }
	char buf[64], *save = NULL, label[24];
	copy(buf, sizeof buf, b);
	out[0] = 0;
	for (char *k = strtok_r(buf, "+", &save); k; k = strtok_r(NULL, "+", &save)) {
		key_label(k, label, sizeof label);
		snprintf(out + strlen(out), size - strlen(out), "%s%s", out[0] ? " + " : "", label);
	}
}

// The tile at the left of a control's row: its letters, or an arrow for the +Control Pad.
static void control_tile(Item *it, Control c) {
	switch (c) {
	case CTL_UP: it->icon = GLYPH_ARROW_UP; break;
	case CTL_DOWN: it->icon = GLYPH_ARROW_DOWN; break;
	case CTL_LEFT: it->icon = GLYPH_ARROW_LEFT; break;
	case CTL_RIGHT: it->icon = GLYPH_ARROW_RIGHT; break;
	case CTL_CIRCLE: case CTL_CSTICK: it->icon = GLYPH_GAMEPAD; break;
	case CTL_START: copy(it->tag, sizeof it->tag, "ST"); break;
	case CTL_SELECT: copy(it->tag, sizeof it->tag, "SE"); break;
	default: copy(it->tag, sizeof it->tag, s_controlNames[c]); break;
	}
	// the New 3DS's colors
	it->color = c == CTL_A ? C_RED : c == CTL_B ? C_YELLOW : c == CTL_X ? C_BLUE : c == CTL_Y ? C_GREEN : C_GRAY;
}

// "Use as a controller": start, which preset, and changes to it. Built again in place.
static void pad_setup_build(void) {
	free(s_list.items);
	s_list.items = NULL;
	s_list.count = 0;
	Item *it = list_add(ROW_PLAIN, "Start", "start", 0);
	copy(it->sub, sizeof it->sub, "To stop, press START and SELECT together");
	it->icon = GLYPH_PLAY;
	it->color = C_GREEN;

	list_header("Preset");
	static const Glyph glyphs[PRESETS] = {GLYPH_MOUSE, GLYPH_GAMEPAD, GLYPH_KEYS};
	u32 colors[PRESETS] = {C_BLUE, C_PURPLE, C_ORANGE};
	for (int p = 0; p < PRESETS; p++) {
		it = list_add(ROW_CHECK, preset_name(p), "preset", p);
		copy(it->sub, sizeof it->sub, preset_about(p));
		it->icon = glyphs[p];
		it->color = colors[p];
		it->on = p == g_controller.preset;
	}

	list_header(preset_name(g_controller.preset));
	int n = preset_changes(g_controller.preset);
	it = list_add(ROW_CHEVRON, "Change a button", "buttons", 0);
	if (n) snprintf(it->detail, sizeof it->detail, n == 1 ? "1 change" : "%d changes", n);
	it->icon = GLYPH_PENCIL;
	it->color = C_ORANGE;
	if (n) {
		it = list_add(ROW_PLAIN, "Back to preset", "reset", 0);
		copy(it->sub, sizeof it->sub, n == 1 ? "Undo your change" : "Undo your changes");
		it->icon = GLYPH_UNDO;
		it->color = C_GRAY;
	}
	list_layout();
}

static void open_pad_setup(void) {
	list_open(LIST_PAD, "Use as a controller");
	pad_setup_build();
}

// Every button and stick, with what it does. Starts on the one changed last.
static void open_pad_buttons(void) {
	static const struct { const char *title; Control from, to; } groups[] = {
		{"Buttons", CTL_A, CTL_Y}, {"Shoulder buttons", CTL_L, CTL_ZR}, {"+Control Pad", CTL_UP, CTL_RIGHT},
		{"Sticks", CTL_CIRCLE, CTL_CSTICK}, {"START and SELECT", CTL_START, CTL_SELECT},
	};
	list_open(LIST_PADBUTTONS, "Change a button");
	int sel = -1;
	for (size_t g = 0; g < sizeof groups / sizeof *groups; g++) {
		list_header(groups[g].title);
		for (Control c = groups[g].from; c <= groups[g].to; c++) {
			if (!control_exists(c)) continue;
			Item *it = list_add(ROW_CHEVRON, s_controlNames[c], "", c);
			binding_label(control_binding(c), it->detail, sizeof it->detail);
			if (control_changed(c)) {
				char was[32];
				binding_label(preset_binding(g_controller.preset, c), was, sizeof was);
				snprintf(it->sub, sizeof it->sub, "Changed from %s", was);
			} else {
				copy(it->sub, sizeof it->sub, preset_note(g_controller.preset, c));
			}
			control_tile(it, c);
			if (c == s_ctl) sel = s_list.count - 1;
		}
	}
	list_layout();
	if (sel >= 0) {
		s_list.sel = sel;
		list_show_sel();
	}
}

// What one button (or stick) should do.
static void open_pad_bind(void) {
	Control c = s_ctl;
	const char *b = control_binding(c);
	char question[64];
	snprintf(question, sizeof question, control_is_stick(c) ? "What should the %s do?" : "What should %s do?", s_controlNames[c]);
	list_open(control_is_stick(c) ? LIST_PADSTICK : LIST_PADBIND, "Change a button");
	list_header(question);
	Item *it;
	if (control_is_stick(c)) {
		static const char *jobs[][2] = {
			{"pointer", "Move the pointer"}, {"scroll", "Scroll"}, {"arrows", "Arrow keys"}, {"wasd", "W A S D keys"},
		};
		static const Glyph glyphs[] = {GLYPH_MOUSE, GLYPH_ARROW_DOWN, GLYPH_ARROW_RIGHT, GLYPH_KEYS};
		for (size_t i = 0; i < sizeof jobs / sizeof *jobs; i++) {
			it = list_add(ROW_CHECK, jobs[i][1], jobs[i][0], 0);
			it->icon = glyphs[i];
			it->color = i < 2 ? C_BLUE : C_ORANGE;
			it->on = !strcmp(b, jobs[i][0]);
		}
	} else {
		static const char *clicks[][2] = {{"click:left", "Left click"}, {"click:right", "Right click"}, {"click:middle", "Middle click"}};
		static const Glyph glyphs[] = {GLYPH_CLICK_LEFT, GLYPH_CLICK_RIGHT, GLYPH_MOUSE};
		for (int i = 0; i < 3; i++) {
			it = list_add(ROW_CHECK, clicks[i][1], clicks[i][0], 0);
			it->icon = glyphs[i];
			it->color = C_BLUE;
			it->on = !strcmp(b, clicks[i][0]);
		}
		list_header("");
		bool key = strncmp(b, "click:", 6) && strcmp(b, "none"), combo = key && strchr(b, '+');
		it = list_add(ROW_CHEVRON, "A key", "key", 0);
		if (key && !combo) binding_label(b, it->detail, sizeof it->detail);
		it->icon = GLYPH_KEYS;
		it->color = C_ORANGE;
		it = list_add(ROW_CHEVRON, "Keys together", "combo", 0);
		copy(it->sub, sizeof it->sub, "Like Cmd + Tab");
		if (combo) binding_label(b, it->detail, sizeof it->detail);
		it->icon = GLYPH_KEYS;
		it->color = C_PURPLE;
	}
	list_header("");
	it = list_add(ROW_CHECK, "Nothing", "none", 0);
	it->icon = GLYPH_MINUS_CIRCLE;
	it->color = C_GRAY;
	it->on = !strcmp(b, "none");
	if (control_changed(c)) {
		list_header("");
		it = list_add(ROW_PLAIN, "Back to preset", "reset", 0);
		binding_label(preset_binding(g_controller.preset, c), it->detail, sizeof it->detail);
		it->icon = GLYPH_UNDO;
		it->color = C_GRAY;
	}
	list_layout();
	list_select_checked();
}

static void pad_bind(const char *binding) {
	control_set(s_ctl, binding);
	controller_save();
	open_pad_buttons();
}

static void start_pad(void);
static void start_type(void);

static void set_key(KeyKind kind, const char *label, const char *ref, const char *category, const char *icon) {
	PadKey *k = edit_key();
	k->kind = kind;
	copy(k->label, sizeof k->label, label);
	copy(k->ref, sizeof k->ref, ref);
	copy(k->category, sizeof k->category, category);
	copy(k->icon, sizeof k->icon, icon);
	k->glyph[0] = k->color[0] = 0;  // a new job, a fresh look
	pads_save();
	icons_prefetch(false);
	open_look();
}

// "https://www.github.com/x" -> "github.com"
static void url_label(const char *url, char *out, size_t size) {
	const char *p = strstr(url, "://");
	p = p ? p + 3 : url;
	if (!strncmp(p, "www.", 4)) p += 4;
	size_t n = strcspn(p, "/?#");
	if (n >= size) n = size - 1;
	memcpy(out, p, n);
	out[n] = 0;
}

static void start_main(void);
static void start_find(const char *message);
static void start_qr(void);

static void list_picked(int i) {
	Item *it = &s_list.items[i];
	if (it->id == -1 && searchable()) {
		list_search();
		return;
	}
	if (it->id == -2) {
		refresh_picked();
		return;
	}
	if (is_catalog() || s_list.purpose == LIST_SEARCH) {
		// a result: the row's id says which kind of key it makes
		catalogs_finish();
		switch (it->id) {
		case KEY_ACTION: set_key(KEY_ACTION, it->label, it->value, it->category, it->iconName); break;
		case KEY_APP: set_key(KEY_APP, it->label, it->value, "", ""); break;
		case KEY_SHORTCUT: set_key(KEY_SHORTCUT, it->label, it->value, "", ""); break;
		}
		return;
	}
	switch (s_list.purpose) {
	case LIST_KIND:
		switch (it->id) {
		case KEY_ACTION: open_catalog(LIST_ACTIONS, CATALOG_ACTIONS, "Pick a Mac action"); break;
		case KEY_APP: open_catalog(LIST_APPS, CATALOG_APPS, "Pick an app"); break;
		case KEY_SHORTCUT: open_catalog(LIST_SHORTCUTS, CATALOG_SHORTCUTS, "Pick a Shortcut"); break;
		case KEY_WEB: {
			char url[192], label[48];
			PadKey *k = edit_key();
			if (!ask_text("Website address", k->kind == KEY_WEB ? k->ref : "https://", SWKBD_TYPE_WESTERN, url, sizeof url)) break;
			if (!strstr(url, "://")) {
				char full[192];
				snprintf(full, sizeof full, "https://%s", url);
				copy(url, sizeof url, full);
			}
			url_label(url, label, sizeof label);
			set_key(KEY_WEB, label, url, "", "");
			break;
		}
		case KEY_KEYS:
			memset(s_mods, 0, sizeof s_mods);
			s_keyName[0] = s_keyLabel[0] = 0;
			s_keysForPad = false;
			s_screen = SCR_KEYS;
			break;
		case 100:
			open_look();
			break;
		case 101:
			memset(edit_key(), 0, sizeof(PadKey));
			pads_save();
			s_screen = SCR_MAIN;
			break;
		}
		break;
	case LIST_ACTIONS: case LIST_APPS: case LIST_SHORTCUTS: case LIST_SEARCH: break;  // above
	case LIST_LOOK: {
		char id[16];
		copy(id, sizeof id, it->value);  // (the rows go away when the list is built again)
		PadKey *k = edit_key();
		if (!strcmp(id, "name")) {
			char label[48];
			if (ask_text("Name for this key", k->label, SWKBD_TYPE_WESTERN, label, sizeof label)) {
				copy(k->label, sizeof k->label, label);
				pads_save();
			}
			open_look();
		} else if (!strcmp(id, "icon")) {
			open_glyph_list();
		} else if (!strcmp(id, "color")) {
			open_color_list();
		} else if (!strcmp(id, "kind")) {
			open_kind_menu();
		} else if (!strcmp(id, "remove")) {
			memset(k, 0, sizeof *k);
			pads_save();
			s_screen = SCR_MAIN;
		} else {
			s_screen = SCR_MAIN;
		}
		break;
	}
	case LIST_GLYPH:
		copy(edit_key()->glyph, sizeof edit_key()->glyph, it->value);
		pads_save();
		open_look();
		break;
	case LIST_COLOR:
		copy(edit_key()->color, sizeof edit_key()->color, it->value);
		pads_save();
		open_look();
		break;
	case LIST_KEYNAME:
		if (s_keyListForPad) {
			pad_bind(it->value);
			break;
		}
		copy(s_keyName, sizeof s_keyName, it->value);
		copy(s_keyLabel, sizeof s_keyLabel, it->label);
		s_screen = SCR_KEYS;
		break;
	case LIST_MENU: {
		const char *id = it->value;
		if (!strcmp(id, "page")) {
			s_page = it->id;
			menu_build();
		} else if (!strcmp(id, "edit")) {
			s_editing = !s_editing;
			menu_build();
		} else if (!strcmp(id, "addpage")) {
			PadPage *p = &g_pads.pages[g_pads.count];
			memset(p, 0, sizeof *p);
			snprintf(p->name, sizeof p->name, "Page %d", g_pads.count + 1);
			s_page = g_pads.count++;
			s_editing = true;
			pads_save();
			s_screen = SCR_MAIN;
		} else if (!strcmp(id, "renamepage")) {
			char name[32];
			if (ask_text("Page name", g_pads.pages[s_page].name, SWKBD_TYPE_WESTERN, name, sizeof name)) {
				copy(g_pads.pages[s_page].name, sizeof g_pads.pages[s_page].name, name);
				pads_save();
			}
			menu_build();
		} else if (!strcmp(id, "removepage")) {
			memmove(&g_pads.pages[s_page], &g_pads.pages[s_page + 1], (g_pads.count - s_page - 1) * sizeof(PadPage));
			g_pads.count--;
			if (s_page >= g_pads.count) s_page = g_pads.count - 1;
			pads_save();
			menu_build();
		} else if (!strcmp(id, "controller")) {
			open_pad_setup();
		} else if (!strcmp(id, "type")) {
			start_type();
		} else if (!strcmp(id, "awake")) {
			switch_flip(0);
		} else if (!strcmp(id, "jiggler")) {
			switch_flip(1);
		} else if (!strcmp(id, "clock")) {
			g_config.clock24 = !g_config.clock24;
			config_save();
			menu_build();
		} else if (!strcmp(id, "forget")) {
			macapad_stop();
			cache_clear();
			g_config.token[0] = 0;
			config_save();
			start_find(NULL);
		} else if (!strcmp(id, "quit")) {
			s_quit = true;
		}
		break;
	}
	case LIST_PAD:
		if (!strcmp(it->value, "start")) {
			start_pad();
		} else if (!strcmp(it->value, "preset")) {
			g_controller.preset = it->id;
			controller_save();
			pad_setup_build();
		} else if (!strcmp(it->value, "buttons")) {
			open_pad_buttons();
		} else if (!strcmp(it->value, "reset")) {
			memset(g_controller.changes[g_controller.preset], 0, sizeof g_controller.changes[0]);
			controller_save();
			pad_setup_build();
		}
		break;
	case LIST_PADBUTTONS:
		s_ctl = it->id;
		open_pad_bind();
		break;
	case LIST_PADBIND: case LIST_PADSTICK:
		if (!strcmp(it->value, "key")) {
			open_keyname_list(true);
		} else if (!strcmp(it->value, "combo")) {
			memset(s_mods, 0, sizeof s_mods);
			s_keyName[0] = s_keyLabel[0] = 0;
			s_keysForPad = true;
			s_screen = SCR_KEYS;
		} else {
			char binding[24];
			copy(binding, sizeof binding, !strcmp(it->value, "reset") ? "" : it->value);  // (the rows go away)
			pad_bind(binding);
		}
		break;
	}
}

static void list_back(void) {
	if (s_list.purpose == LIST_KEYNAME) {
		if (s_keyListForPad) open_pad_bind();
		else s_screen = SCR_KEYS;
	} else if (s_list.purpose == LIST_PAD) {
		open_menu();
	} else if (s_list.purpose == LIST_PADBUTTONS) {
		open_pad_setup();
	} else if (s_list.purpose == LIST_PADBIND || s_list.purpose == LIST_PADSTICK) {
		open_pad_buttons();
	} else if (s_list.purpose == LIST_GLYPH || s_list.purpose == LIST_COLOR) {
		open_look();
	} else if (is_catalog() || s_list.purpose == LIST_SEARCH) {
		catalogs_finish();
		open_kind_menu();
	} else {
		s_screen = SCR_MAIN;
	}
}

// Rows for the items of one catalog that match the filter. For actions, a title wherever the
// category changes. Each row's id is the kind of key it makes.
static int add_catalog_rows(CatalogKind kind, bool categoryTitles) {
	Catalog *cat = &s_cats[kind];
	char lastCat[16] = "";
	int n = 0;
	for (int i = 0; i < cat->count; i++) {
		CatalogItem *c = &cat->items[i];
		if (!contains_ci(c->label, s_list.filter)) continue;
		n++;
		Item *it;
		switch (kind) {
		case CATALOG_ACTIONS:
			if (categoryTitles && strcmp(c->sub, lastCat)) {
				list_header(c->sub[0] ? c->sub : "Other");
				copy(lastCat, sizeof lastCat, c->sub);
			}
			it = list_add(ROW_PLAIN, c->label, c->value, KEY_ACTION);
			it->icon = action_glyph(c->value, c->icon);
			it->color = category_color(c->sub);
			copy(it->category, sizeof it->category, c->sub);
			copy(it->iconName, sizeof it->iconName, c->icon);
			break;
		case CATALOG_APPS:
			it = list_add(ROW_PLAIN, c->label, c->value, KEY_APP);
			it->tag[0] = (char)toupper((unsigned char)c->label[0]);
			it->color = C_BLUE;
			break;
		case CATALOG_SHORTCUTS:
			it = list_add(ROW_PLAIN, c->label, c->value, KEY_SHORTCUT);
			it->icon = GLYPH_SHORTCUT;
			it->color = C_PURPLE;
			break;
		}
	}
	return n;
}

static int matches_in(CatalogKind kind) {
	int n = 0;
	for (int i = 0; i < s_cats[kind].count; i++) n += contains_ci(s_cats[kind].items[i].label, s_list.filter);
	return n;
}

// The rows of a catalog list: the search field, then what matches. Built again whenever the
// filter changes; the catalogs stay loaded until the list closes.
static void list_fill_catalog(void) {
	static const char *prompts[] = {"Search actions", "Search apps", "Search Shortcuts"};
	static const char *titles[] = {"Mac actions", "Apps", "Shortcuts"};
	free(s_list.items);
	s_list.items = NULL;
	s_list.count = s_list.results = 0;
	s_list.scroll = 0;

	bool all = s_list.purpose == LIST_SEARCH;
	CatalogKind one = s_list.purpose == LIST_ACTIONS ? CATALOG_ACTIONS : s_list.purpose == LIST_APPS ? CATALOG_APPS : CATALOG_SHORTCUTS;
	add_search_row(all ? "Search actions, apps, Shortcuts" : prompts[one]);
	if (all) {
		for (int k = 0; k < 3; k++) {
			if (!matches_in(k)) continue;
			list_header(titles[k]);
			s_list.results += add_catalog_rows(k, false);
		}
	} else {
		s_list.results = add_catalog_rows(one, true);
	}

	if (s_list.filter[0]) snprintf(s_list.items[0].detail, sizeof s_list.items[0].detail, "%d found", s_list.results);
	s_list.empty[0] = 0;
	if (s_list.results == 0) {
		bool anyOk = false;
		const char *error = NULL;
		for (int k = 0; k < 3; k++) {
			if (!(s_wantCats >> k & 1)) continue;
			if (s_cats[k].ok) anyOk = true;
			else if (!error) error = s_cats[k].error;
		}
		if (!anyOk && error) copy(s_list.empty, sizeof s_list.empty, error);
		else if (s_list.filter[0]) snprintf(s_list.empty, sizeof s_list.empty, "Nothing called \xE2\x80\x9C%s\xE2\x80\x9D", s_list.filter);
		else copy(s_list.empty, sizeof s_list.empty, s_list.purpose == LIST_SHORTCUTS ? "No shortcuts in the Shortcuts app yet." : "Nothing here.");
	}
	s_list.sel = s_list.results ? 2 : 0;  // the first match, past the field and the gap

	list_header("");
	Item *it = list_add(ROW_PLAIN, "Refresh from your Mac", "refresh", -2);
	it->icon = GLYPH_REFRESH;
	it->color = C_GRAY;
	s_list.refreshRow = s_list.count - 1;
	refresh_row_update();
	list_layout();
}

// Asks what to look for; an empty answer shows everything again.
static void list_search(void) {
	static const char *hints[] = {"Search Mac actions", "Search apps", "Search Shortcuts"};
	const char *hint = s_list.purpose == LIST_ACTIONS ? hints[0] : s_list.purpose == LIST_APPS ? hints[1]
	                 : s_list.purpose == LIST_SHORTCUTS ? hints[2] : "Search actions, apps and Shortcuts";
	char q[48];
	if (!ask_text_opt(hint, s_list.filter, SWKBD_TYPE_WESTERN, false, q, sizeof q)) return;
	// trim spaces
	char *start = q;
	while (*start == ' ') start++;
	for (char *end = start + strlen(start); end > start && end[-1] == ' '; ) *--end = 0;
	if (s_list.purpose == LIST_KIND) {
		if (*start) open_search(start);  // from the first question: look everywhere
		return;
	}
	copy(s_list.filter, sizeof s_list.filter, start);
	if (s_list.loading) return;  // applied once the Mac answers
	list_fill_catalog();
}

static void list_update(u32 down, u32 held, touchPosition touch) {
	if (is_catalog() || s_list.purpose == LIST_SEARCH) {
		bool changed = false;
		for (int k = 0; k < 3; k++) if ((s_wantCats >> k & 1) && catalog_sync(&s_cats[k])) changed = true;
		if (s_list.loading) {
			list_catalogs_ready();
		} else if (changed) {
			// a refresh landed while the list was open: the rows again, where they were
			float scroll = s_list.scroll;
			int sel = s_list.sel;
			list_fill_catalog();
			s_list.scroll = scroll;
			s_list.sel = sel;
			list_layout();
		}
	}
	refresh_row_update();
	if (s_list.purpose == LIST_MENU) switches_update();

	if (down & KEY_B) { list_back(); return; }
	if ((down & KEY_Y) && searchable()) { list_search(); return; }
	if ((down & KEY_X) && s_list.refreshRow >= 0) { refresh_picked(); return; }
	if ((down & KEY_START) && s_list.purpose == LIST_MENU) { list_back(); return; }
	if (down & (KEY_DOWN | KEY_UP | KEY_L | KEY_R) && s_list.count)
		list_move((down & KEY_DOWN) ? 1 : (down & KEY_UP) ? -1 : (down & KEY_R) ? 5 : -5);
	if ((down & KEY_A) && pickable(s_list.sel)) { list_picked(s_list.sel); return; }

	circlePosition cp;
	hidCircleRead(&cp);
	if (abs(cp.dy) > 20) {
		s_list.scroll -= cp.dy / 12.0f;
		list_clamp();
	}

	// touch: drag scrolls, a tap picks
	if (down & KEY_TOUCH) {
		if (in(touch, 0, 0, 90, 32)) { list_back(); return; }
		if (searchable() && in(touch, BOTTOM_W - 48, 0, 48, 32)) { list_search(); return; }
		s_scrollStart = s_list.scroll;
	}
	if ((held & KEY_TOUCH) && s_touching && s_touchStart.py >= s_list.top) {
		s_list.scroll = s_scrollStart - (touch.py - s_touchStart.py);
		list_clamp();
	}
}

static void list_tap(touchPosition t) {
	if (t.py < s_list.top || s_list.loading) return;
	float y = t.py - s_list.top + s_list.scroll;
	for (int i = 0; i < s_list.count; i++) {
		Item *it = &s_list.items[i];
		if (y < it->y || y >= it->y + it->h) continue;
		if (!pickable(i)) return;
		s_list.sel = i;
		list_picked(i);
		return;
	}
}

static void header(const char *title, const char *back, bool search) {
	C2D_DrawRectSolid(0, 0, 0.5f, BOTTOM_W, 30, C_BG);
	C2D_DrawRectSolid(0, 30, 0.5f, BOTTOM_W, 1, C_LINE);
	if (back) text(10, 7, 0.5f, C_BLUE, 0, back);
	if (search) glyph(GLYPH_SEARCH, BOTTOM_W - 20, 15, 15, C_BLUE, C_BG);
	text_fit(BOTTOM_W / 2, 7, 0.5f, C_TEXT, C2D_AlignCenter, 170, title);
}

// The square at the left of a row, with a glyph or a couple of letters on it.
static void tile(float x, float y, u32 color, Glyph g, const char *tag) {
	rrect(x, y, TILE, TILE, 6, color);
	if (g != GLYPH_NONE) glyph(g, x + TILE / 2, y + TILE / 2, 13, C_TEXT, color);
	else if (tag[0]) text(x + TILE / 2, y + 5, strlen(tag) > 1 ? 0.42f : 0.5f, C_TEXT, C2D_AlignCenter, tag);
}

static void toggle(float rx, float cy, bool on) {
	rrect(rx - 40, cy - 11, 40, 22, 11, on ? C_GREEN : C_CARD_HI);
	disc(on ? rx - 11 : rx - 29, cy, 9, C_TEXT);
}

static void list_draw(void) {
	// the cards first, whole, so one whose first row has scrolled off still has a background
	for (int i = 0; i < s_list.count; i++) {
		Item *it = &s_list.items[i];
		if (it->style == ROW_HEADER || it->style == ROW_SEARCH || !it->first) continue;
		int j = i;
		while (!s_list.items[j].last) j++;
		float y = s_list.top + it->y - s_list.scroll, h = s_list.items[j].y + s_list.items[j].h - it->y;
		if (y + h >= s_list.top && y <= SCREEN_H) rrect(CARD_X, y, CARD_W, h, 10, C_CARD);
	}
	for (int i = 0; i < s_list.count; i++) {
		Item *it = &s_list.items[i];
		float y = s_list.top + it->y - s_list.scroll;
		if (y + it->h < s_list.top || y > SCREEN_H) continue;
		if (it->style == ROW_HEADER) {
			if (it->label[0]) text(CARD_X + 12, y + it->h - 16, 0.36f, C_TEXT2, 0, it->label);
			continue;
		}
		if (it->style == ROW_SEARCH) {
			float cy = y + it->h / 2, right = CARD_X + CARD_W - 14;
			rrect(CARD_X, y, CARD_W, it->h, it->h / 2, i == s_list.sel ? C_CARD_HI : C_CARD);
			glyph(GLYPH_SEARCH, CARD_X + 19, cy, 13, C_TEXT2, C_CARD);
			float w = it->detail[0] ? text_width(it->detail, 0.42f) + 10 : 0;
			if (it->detail[0]) text(right, cy - 7, 0.42f, C_TEXT2, C2D_AlignRight, it->detail);
			text_fit(CARD_X + 34, cy - 8, 0.5f, it->on ? C_TEXT : C_TEXT3, 0, right - w - (CARD_X + 34), it->label);
			continue;
		}
		if (i == s_list.sel) rrect(CARD_X + 2, y + 2, CARD_W - 4, it->h - 4, 8, C_CARD_HI);

		float x = CARD_X + 12, right = CARD_X + CARD_W - 12, cy = y + it->h / 2;
		bool hasTile = it->icon != GLYPH_NONE || it->tag[0] || it->swatch;
		if (hasTile) {
			tile(x, cy - TILE / 2, it->color, it->icon, it->tag);
			x += TILE + 10;
		}
		float textRight = right;
		switch (it->style) {
		case ROW_TOGGLE:
			toggle(right, cy, it->on);
			textRight = right - 48;
			break;
		case ROW_CHEVRON:
			glyph(GLYPH_CHEVRON_R, right - 5, cy, 12, C_TEXT3, C_CARD);
			textRight = right - 16;
			if (it->detail[0]) {
				text_fit(textRight, cy - 7, 0.44f, C_TEXT2, C2D_AlignRight, 120, it->detail);
				float w = text_width(it->detail, 0.44f);
				textRight -= (w > 120 ? 120 : w) + 8;
			}
			break;
		case ROW_CHECK:
			if (it->on) glyph(GLYPH_CHECK, right - 8, cy, 14, C_BLUE, C_CARD);
			textRight = right - 22;
			break;
		default:
			if (it->detail[0]) {
				text_fit(right, cy - 7, 0.44f, C_TEXT2, C2D_AlignRight, 120, it->detail);
				float w = text_width(it->detail, 0.44f);
				textRight = right - (w > 120 ? 120 : w) - 8;
			}
			break;
		}
		u32 color = it->danger ? C_RED : C_TEXT;
		if (it->sub[0]) {
			text_fit(x, y + 5, 0.5f, color, 0, textRight - x, it->label);
			text_fit(x, y + 23, 0.38f, C_TEXT2, 0, textRight - x, it->sub);
		} else {
			text_fit(x, cy - 8, 0.5f, color, 0, textRight - x, it->label);
		}
		if (!it->last) C2D_DrawRectSolid(x, y + it->h - 1, 0.5f, right - x, 1, C_LINE);
	}
	if (s_list.purpose == LIST_LOOK) {
		// the key, as it will look on the pad, above the rows
		C2D_DrawRectSolid(0, LIST_TOP, 0.5f, BOTTOM_W, LOOK_PREVIEW_H, C_BG);
		key_tile(edit_key(), BOTTOM_W / 2 - KEY_W / 2, LIST_TOP + 7, false);
	}
	if (s_list.loading) text(BOTTOM_W / 2, 110, 0.5f, C_TEXT2, C2D_AlignCenter, "Asking your Mac\xE2\x80\xA6");
	else if (s_list.empty[0] && !s_list.results) text_fit(BOTTOM_W / 2, 150, 0.46f, C_TEXT2, C2D_AlignCenter, 300, s_list.empty);  // under the Refresh row
	bool back = s_list.purpose != LIST_MENU && s_list.purpose != LIST_KIND && s_list.purpose != LIST_LOOK;
	header(s_list.title, back ? "\xE2\x80\xB9 Back" : "\xE2\x80\xB9 Done", searchable());
}

// ---- keyboard shortcut builder -------------------------------------------------------

static const char *s_modNames[] = {"cmd", "shift", "opt", "ctrl"};
static const char *s_modLabels[] = {"Command", "Shift", "Option", "Control"};
static const char *s_modShort[] = {"Cmd", "Shift", "Opt", "Ctrl"};

static void keys_combo(char *keys, size_t ksize, char *label, size_t lsize) {
	keys[0] = label[0] = 0;
	// macOS order: ctrl, opt, shift, cmd
	static const int order[] = {3, 2, 1, 0};
	for (int i = 0; i < 4; i++) {
		int m = order[i];
		if (!s_mods[m]) continue;
		snprintf(keys + strlen(keys), ksize - strlen(keys), "%s+", s_modNames[m]);
		snprintf(label + strlen(label), lsize - strlen(label), "%s+", s_modShort[m]);
	}
	snprintf(keys + strlen(keys), ksize - strlen(keys), "%s", s_keyName);
	snprintf(label + strlen(label), lsize - strlen(label), "%s", s_keyLabel);
}

#define MOD_Y 70
#define MOD_W 72

// Back from the shortcut builder: to the key editor, or to what a controller button does.
static void keys_back(void) {
	if (s_keysForPad) open_pad_bind();
	else open_kind_menu();
}

static void keys_tap(touchPosition t) {
	for (int i = 0; i < 4; i++)
		if (in(t, 8 + i * (MOD_W + 4), MOD_Y, MOD_W, 44)) s_mods[i] = !s_mods[i];
	if (in(t, 8, 124, 304, 44)) open_keyname_list(false);
	if (in(t, 0, 0, 90, 32)) keys_back();
	if (s_keyName[0] && in(t, 8, 184, 304, 44)) {
		char keys[64], label[48];
		keys_combo(keys, sizeof keys, label, sizeof label);
		if (s_keysForPad) pad_bind(keys);
		else set_key(KEY_KEYS, label, keys, "", "");
	}
}

static void keys_draw(void) {
	header(s_keysForPad ? "Keys together" : "Keyboard shortcut", "\xE2\x80\xB9 Back", false);
	text(BOTTOM_W / 2, 40, 0.45f, C_TEXT2, C2D_AlignCenter, "Hold down\xE2\x80\xA6");
	for (int i = 0; i < 4; i++) {
		float x = 8 + i * (MOD_W + 4);
		rrect(x, MOD_Y, MOD_W, 44, 10, s_mods[i] ? C_ORANGE : C_CARD);
		text(x + MOD_W / 2, MOD_Y + 13, 0.45f, s_mods[i] ? C_BG : C_TEXT, C2D_AlignCenter, s_modLabels[i]);
	}
	rrect(8, 124, 304, 44, 10, C_CARD);
	text(20, 137, 0.5f, C_TEXT2, 0, "and press");
	text(300, 137, 0.5f, s_keyName[0] ? C_TEXT : C_BLUE, C2D_AlignRight, s_keyName[0] ? s_keyLabel : "Pick a key \xE2\x80\xBA");
	if (s_keyName[0]) {
		char keys[64], label[48], save[64];
		keys_combo(keys, sizeof keys, label, sizeof label);
		snprintf(save, sizeof save, "Save  %s", label);
		rrect(8, 184, 304, 44, 10, C_BLUE);
		text_fit(BOTTOM_W / 2, 197, 0.5f, C_TEXT, C2D_AlignCenter, 290, save);
	}
}

// ---- pad (main bottom screen) ---------------------------------------------------------

static int slot_at(touchPosition t) {
	for (int s = 0; s < SLOTS; s++) {
		float x = GRID_X + (s % COLS) * (KEY_W + GAP), y = GRID_Y + (s / COLS) * (KEY_H + GAP);
		if (in(t, x, y, KEY_W, KEY_H)) return s;
	}
	return -1;
}

static void edit_slot(int slot) {
	s_editSlot = slot;
	open_kind_menu();
}

static void pad_tap(int slot) {
	if (slot < 0) return;
	PadKey *k = &g_pads.pages[s_page].keys[slot];
	if (s_editing || k->kind == KEY_NONE) {
		edit_slot(slot);
		return;
	}
	s_flashSlot = slot;
	s_flashUntil = osGetTime() + 180;
	press_key(k);
}

// To the next (+1) or previous (-1) page; it slides into place over the next few frames.
static void go_page(int dir) {
	if (g_pads.count < 2) return;
	s_page = (s_page + dir + g_pads.count) % g_pads.count;
	s_slide += dir * BOTTOM_W;
	if (s_slide > BOTTOM_W) s_slide = BOTTOM_W;
	if (s_slide < -BOTTOM_W) s_slide = -BOTTOM_W;
}

// A tap on the pad, or on the bar above it: ‹ page name ›, or the editing notice.
static void main_tap(touchPosition t) {
	if (t.py >= GRID_Y) {
		pad_tap(slot_at(t));
		return;
	}
	if (s_editing) {
		s_editing = false;
	} else if (g_pads.count > 1 && t.px < 48) {
		go_page(-1);
	} else if (g_pads.count > 1 && t.px >= BOTTOM_W - 48) {
		go_page(+1);
	} else {
		open_menu();
	}
}

// One key as it looks on the pad, at (x, y): the icon picked in "How it looks", else the app's or
// website's own, else one for what it does; its name under that.
static void key_tile(const PadKey *k, float x, float y, bool flash) {
	float cx = x + KEY_W / 2, iconY = y + 23;
	if (k->kind == KEY_NONE) {
		u32 bg = C2D_Color32(0x1C, 0x1C, 0x1E, 0x80);
		rrect(x, y, KEY_W, KEY_H, 12, bg);
		glyph(GLYPH_PLUS, cx, y + KEY_H / 2, 14, s_editing ? C_ORANGE : C_TEXT3, bg);
		return;
	}
	u32 bg = flash ? C_CARD_HI : C_CARD, accent = key_accent(k);
	rrect(x, y, KEY_W, KEY_H, 12, bg);
	const GlyphChoice *chosen = glyph_choice(k->glyph);
	if (chosen) {
		glyph(chosen->g, cx, iconY, 22, accent, bg);
	} else {
		switch (k->kind) {
		case KEY_ACTION:
			glyph(action_glyph(k->ref, k->icon), cx, iconY, 20, accent, bg);
			break;
		case KEY_APP: case KEY_WEB: {
			Icon *icon = icon_for(k->kind, k->ref);
			if (icon && icon->state == 2) picture_draw(&icon->pic, cx - 16, iconY - 16, 32, 32);
			else if (k->kind == KEY_APP) glyph(GLYPH_APP, cx, iconY, 26, k->color[0] ? accent : C_CARD_HI, bg);
			else glyph(GLYPH_GLOBE, cx, iconY, 22, accent, bg);
			break;
		}
		case KEY_KEYS: glyph(GLYPH_KEYS, cx, iconY, 22, accent, bg); break;
		case KEY_SHORTCUT:
			rrect(cx - 12, iconY - 12, 18, 18, 5, k->color[0] ? accent : C_PINK);
			rrect(cx - 7.5f, iconY - 7.5f, 21, 21, 6, bg);
			rrect(cx - 6, iconY - 6, 18, 18, 5, accent);
			break;
		default: break;
		}
	}
	text_fit(cx, y + KEY_H - 21, 0.4f, C_TEXT, C2D_AlignCenter, KEY_W - 8, k->label);
}

static void key_draw(int page, int slot, float dx, bool pressed) {
	PadKey *k = &g_pads.pages[page].keys[slot];
	float x = GRID_X + (slot % COLS) * (KEY_W + GAP) + dx, y = GRID_Y + (slot / COLS) * (KEY_H + GAP);
	if (x + KEY_W <= 0 || x >= BOTTOM_W) return;
	bool flash = pressed || (page == s_page && slot == s_flashSlot && osGetTime() < s_flashUntil);
	key_tile(k, x, y, flash);
	if (k->kind == KEY_NONE) return;
	if (s_editing) disc(x + KEY_W - 9, y + 9, 4, C_ORANGE);
	// Keep Mac awake and Mouse jiggler show when they're on
	if (k->kind == KEY_ACTION && ((!strcmp(k->ref, "keep-awake") && switch_on(0)) || (!strcmp(k->ref, "jiggler") && switch_on(1))))
		disc(x + 9, y + 9, 3.5f, C_GREEN);
	if (s_st.off) rrect(x, y, KEY_W, KEY_H, 12, C2D_Color32(0, 0, 0, 0x90));
}

static void page_draw(int page, float dx, int pressedSlot) {
	for (int s = 0; s < SLOTS; s++) key_draw(page, s, dx, page == s_page && s == pressedSlot);
}

static void pad_draw(void) {
	PadPage *p = &g_pads.pages[s_page];
	if (s_editing) {
		rrect(4, 3, BOTTOM_W - 8, 25, 8, C_ORANGE);
		text(BOTTOM_W / 2, 7, 0.44f, C_BG, C2D_AlignCenter, "Tap a key to change it \xC2\xB7 tap here when done");
	} else {
		if (g_pads.count > 1) {
			rrect(6, 5, 34, 22, 7, C_CARD);
			glyph(GLYPH_CHEVRON_L, 23, 16, 13, C_TEXT2, C_CARD);
			rrect(BOTTOM_W - 40, 5, 34, 22, 7, C_CARD);
			glyph(GLYPH_CHEVRON_R, BOTTOM_W - 23, 16, 13, C_TEXT2, C_CARD);
		}
		text_fit(BOTTOM_W / 2, 4, 0.48f, C_TEXT, C2D_AlignCenter, 200, p->name);
		float dotsW = g_pads.count * 9;
		for (int i = 0; i < g_pads.count && g_pads.count > 1; i++)
			disc(BOTTOM_W / 2 - dotsW / 2 + i * 9 + 4, 25, 2.5f, i == s_page ? C_TEXT : C_TEXT3);
	}
	int pressed = s_touching && !s_dragging && !s_longFired ? slot_at(s_touchStart) : -1;
	page_draw(s_page, s_slide, pressed);
	if (g_pads.count > 1) {
		if (s_slide > 0.5f) page_draw((s_page + g_pads.count - 1) % g_pads.count, s_slide - BOTTOM_W, -1);
		else if (s_slide < -0.5f) page_draw((s_page + 1) % g_pads.count, s_slide + BOTTOM_W, -1);
	}
}

static void main_update(u32 down) {
	if (s_st.forgotten) {
		macapad_stop();
		g_config.token[0] = 0;
		config_save();
		start_find("Your Mac forgot this 3DS. Pair again.");
		return;
	}
	if (down & KEY_START) { open_menu(); return; }
	if (down & KEY_SELECT) s_editing = !s_editing;
	if (down & KEY_L) go_page(-1);
	if (down & KEY_R) go_page(+1);
	if (down & KEY_X) press_action("play-pause");
	if (down & KEY_DLEFT) press_action("previous-track");
	if (down & KEY_DRIGHT) press_action("next-track");
	if (down & KEY_DUP) press_action("volume-up");
	if (down & KEY_DDOWN) press_action("volume-down");
	if (down & KEY_Y) press_action("mute");
}

// A sideways drag moves the pages with the finger; letting go past the mark (or a quick
// flick) turns the page, otherwise it settles back. Up-and-down drags are left alone.
static void swipe_update(bool released) {
	if (s_touching && s_dragging) {
		int dx = s_touchNow.px - s_touchStart.px, dy = s_touchNow.py - s_touchStart.py;
		if (!s_swipeAxis) {
			s_swipeAxis = true;
			s_swiping = abs(dx) >= abs(dy);
		}
		if (s_swiping) s_slide = g_pads.count > 1 ? dx : dx * 0.25f;  // one page: a little give, no turn
	}
	if (released && s_swiping) {
		s_swiping = false;
		int dx = s_touchNow.px - s_touchStart.px;
		bool flick = osGetTime() - s_touchAt < FLICK_MS && abs(dx) >= FLICK_PX;
		if (abs(dx) >= SWIPE_COMMIT || flick) go_page(dx < 0 ? +1 : -1);
	}
	if (!s_swiping && s_slide != 0) {
		s_slide *= 0.68f;
		if (fabsf(s_slide) < 0.5f) s_slide = 0;
	}
}

// ---- the 3DS as a controller ---------------------------------------------------------------

// The bottom screen: a trackpad, a scroll strip at its right, and two click buttons under them.
#define PAD_TOP 38
#define PAD_BOTTOM 192
#define STRIP_W 48
#define STRIP_X (BOTTOM_W - 8 - STRIP_W)
#define CLICK_Y 198
#define CLICK_H 36
#define TAP_MS 250       // a touch this short that didn't move is a click
#define CLICK_MS 70      // how long that click holds the button down
#define LATE_MS 80       // START and SELECT count only once held this long, so pressing both to stop presses neither

typedef enum { ZONE_NONE, ZONE_DONE, ZONE_TRACKPAD, ZONE_SCROLL, ZONE_LEFT, ZONE_RIGHT } Zone;

static const u32 s_controlKeys[CTL_COUNT] = {
	KEY_A, KEY_B, KEY_X, KEY_Y, KEY_L, KEY_R, KEY_ZL, KEY_ZR, KEY_DUP, KEY_DDOWN, KEY_DLEFT, KEY_DRIGHT, KEY_START, KEY_SELECT,
};

static u32 s_padIgnore;     // buttons already down when the screen opened: they count once let go
static u32 s_padHeld;       // the buttons that counted this frame, lit on the top screen
static bool s_stickLit[2];  // the Circle Pad and C-Stick are pushed
static u64 s_lateSince[2];  // START, SELECT: when they went down (0 = up)
static u64 s_lateUntil[2];  // a quick tap of one, held down this long after the fact
static Zone s_zone;         // where this touch started
static float s_fingerX, s_fingerY;  // where the finger is, smoothed (the touch screen wobbles a little)
static int s_touchFrames;
static u64 s_clickUntil;    // a tap on the trackpad: the left button is down until then
static u64 s_padOpenedAt;

static Zone zone_at(touchPosition t) {
	if (in(t, 0, 0, 90, 32)) return ZONE_DONE;
	if (in(t, 8, CLICK_Y, BOTTOM_W / 2 - 10, CLICK_H)) return ZONE_LEFT;
	if (in(t, BOTTOM_W / 2 + 2, CLICK_Y, BOTTOM_W / 2 - 10, CLICK_H)) return ZONE_RIGHT;
	if (in(t, STRIP_X, PAD_TOP, STRIP_W, PAD_BOTTOM - PAD_TOP)) return ZONE_SCROLL;
	if (in(t, 8, PAD_TOP, STRIP_X - 16, PAD_BOTTOM - PAD_TOP)) return ZONE_TRACKPAD;
	return ZONE_NONE;
}

static void start_pad(void) {
	s_padIgnore = hidKeysHeld();
	s_padHeld = 0;
	memset(s_lateSince, 0, sizeof s_lateSince);
	memset(s_lateUntil, 0, sizeof s_lateUntil);
	s_zone = ZONE_NONE;
	s_clickUntil = 0;
	s_padOpenedAt = osGetTime();
	input_start();
	s_screen = SCR_PAD;
}

// Lets go of everything on the Mac and goes back to the settings.
static void stop_pad(void) {
	input_stop();
	open_pad_setup();
}

// -1..1 past a small dead zone.
static float stick_axis(int v) {
	float f = v / 150.0f, dz = 0.12f;
	if (f > 1) f = 1;
	if (f < -1) f = -1;
	if (fabsf(f) < dz) return 0;
	return (f > 0 ? f - dz : f + dz) / (1 - dz);
}

// A stick's share of this frame: pointer or scroll movement, or keys held.
static bool stick_apply(const char *job, circlePosition p, InputHeld *h, float *mx, float *my, float *sx, float *sy) {
	float x = stick_axis(p.dx), y = -stick_axis(p.dy);  // down is positive, as on the screen
	if (!strcmp(job, "pointer")) {
		// slow near the middle for aiming, quick at the edge
		*mx += x * (fabsf(x) * 16 + 2);
		*my += y * (fabsf(y) * 16 + 2);
	} else if (!strcmp(job, "scroll")) {
		*sx += x * fabsf(x) * 20;
		*sy += y * fabsf(y) * 20;  // pushed down shows what's further down
	} else if (!strcmp(job, "arrows") || !strcmp(job, "wasd")) {
		bool wasd = job[0] == 'w';
		if (x < -0.4f) held_add(h, wasd ? "a" : "left");
		if (x > 0.4f) held_add(h, wasd ? "d" : "right");
		if (y < -0.4f) held_add(h, wasd ? "w" : "up");
		if (y > 0.4f) held_add(h, wasd ? "s" : "down");
	}
	return x || y;
}

static void binding_apply(const char *b, InputHeld *h) {
	if (!strcmp(b, "none")) return;
	if (!strcmp(b, "click:left")) h->buttons |= MOUSE_LEFT;
	else if (!strcmp(b, "click:right")) h->buttons |= MOUSE_RIGHT;
	else if (!strcmp(b, "click:middle")) h->buttons |= MOUSE_MIDDLE;
	else held_add_combo(h, b);
}

// Every frame: what's held and moved, all of it, to the Mac.
static void pad_update(u32 down, u32 held, touchPosition touch, bool released) {
	if ((held & KEY_START) && (held & KEY_SELECT)) {
		stop_pad();
		return;
	}
	u64 now = osGetTime();
	s_padIgnore &= held;
	held &= ~s_padIgnore;
	down &= ~s_padIgnore;

	InputHeld h = {0};
	float mx = 0, my = 0, sx = 0, sy = 0;
	s_padHeld = 0;
	for (int c = 0; c < CTL_COUNT; c++) {
		u32 bit = s_controlKeys[c];
		if (!bit || !control_exists(c)) continue;
		bool on = held & bit;
		if (c == CTL_START || c == CTL_SELECT) {
			int i = c == CTL_SELECT;
			if (down & bit) s_lateSince[i] = now;
			if (!on && s_lateSince[i]) {
				if (now - s_lateSince[i] < LATE_MS) s_lateUntil[i] = now + LATE_MS;  // a quick tap still counts
				s_lateSince[i] = 0;
			}
			on = (on && s_lateSince[i] && now - s_lateSince[i] >= LATE_MS) || now < s_lateUntil[i];
		}
		if (!on) continue;
		s_padHeld |= bit;
		binding_apply(control_binding(c), &h);
	}

	circlePosition cp;
	hidCircleRead(&cp);
	s_stickLit[0] = stick_apply(control_binding(CTL_CIRCLE), cp, &h, &mx, &my, &sx, &sy);
	s_stickLit[1] = false;
	if (s_isNew3ds) {
		hidCstickRead(&cp);
		s_stickLit[1] = stick_apply(control_binding(CTL_CSTICK), cp, &h, &mx, &my, &sx, &sy);
	}

	// the touch screen
	if (down & KEY_TOUCH) {
		s_zone = zone_at(touch);
		s_fingerX = touch.px;
		s_fingerY = touch.py;
		s_touchFrames = 0;
	}
	if (held & KEY_TOUCH) {
		float fx = s_fingerX + (touch.px - s_fingerX) * 0.6f, fy = s_fingerY + (touch.py - s_fingerY) * 0.6f;
		float dx = fx - s_fingerX, dy = fy - s_fingerY;
		s_fingerX = fx;
		s_fingerY = fy;
		if (++s_touchFrames > 2) {  // (the first readings of a touch jump about)
			if (s_zone == ZONE_TRACKPAD) {
				// faster swipes go further, slow ones stay precise
				float speed = sqrtf(dx * dx + dy * dy), gain = 1.3f + (speed > 10 ? 10 : speed) * 0.27f;
				mx += dx * gain;
				my += dy * gain;
			} else if (s_zone == ZONE_SCROLL) {
				sy -= dy * 4;  // the page follows the finger, like a phone
			}
		}
		if (s_zone == ZONE_LEFT) h.buttons |= MOUSE_LEFT;
		if (s_zone == ZONE_RIGHT) h.buttons |= MOUSE_RIGHT;
	}
	if (released && s_zone == ZONE_TRACKPAD && !s_dragging && now - s_touchAt < TAP_MS) s_clickUntil = now + CLICK_MS;
	if (released && s_zone == ZONE_DONE && !s_dragging) {
		stop_pad();
		return;
	}
	if (released) s_zone = ZONE_NONE;
	if (now < s_clickUntil) h.buttons |= MOUSE_LEFT;

	input_hold(&h);
	input_move(mx, my, sx, sy);
}

// One row on the top screen: the button, lit while it's held, and what it does.
static void control_row(Control c, float x, float y, bool lit) {
	float bw = 46, bh = 16;
	u32 bg = lit ? C_BLUE : C_CARD_HI;
	rrect(x, y, bw, bh, 5, bg);
	switch (c) {
	case CTL_UP: glyph(GLYPH_ARROW_UP, x + bw / 2, y + bh / 2, 10, C_TEXT, bg); break;
	case CTL_DOWN: glyph(GLYPH_ARROW_DOWN, x + bw / 2, y + bh / 2, 10, C_TEXT, bg); break;
	case CTL_LEFT: glyph(GLYPH_ARROW_LEFT, x + bw / 2, y + bh / 2, 10, C_TEXT, bg); break;
	case CTL_RIGHT: glyph(GLYPH_ARROW_RIGHT, x + bw / 2, y + bh / 2, 10, C_TEXT, bg); break;
	default:
		text_fit(x + bw / 2, y + 2, 0.36f, C_TEXT, C2D_AlignCenter, bw - 4, c == CTL_CIRCLE ? "Circle" : s_controlNames[c]);
		break;
	}
	const char *b = control_binding(c), *note = control_changed(c) ? "" : preset_note(g_controller.preset, c);
	char job[48];
	binding_label(b, job, sizeof job);
	float tx = x + bw + 8, maxw = 184 - bw - 8;
	if (note[0]) {
		// what it's for, then the keys
		text_fit(tx, y, 0.45f, C_TEXT, 0, maxw, note);
		float nw = text_width(note, 0.45f) + 6;
		if (nw < maxw - 20) text_fit(tx + nw, y + 1.5f, 0.38f, C_TEXT3, 0, maxw - nw, job);
	} else {
		text_fit(tx, y, 0.45f, strcmp(b, "none") ? C_TEXT : C_TEXT3, 0, maxw, job);
	}
}

static void pad_draw_top(void) {
	InputInfo info;
	input_info(&info);
	text(14, 10, 0.7f, C_TEXT, 0, "Controller");

	// whether it's getting through, at the right
	const char *msg = "Connecting\xE2\x80\xA6";
	u32 dot = C_TEXT3;
	switch (info.state) {
	case INPUT_CONNECTING: break;
	case INPUT_LIVE:
		if (info.through) {
			msg = "Working";
			dot = C_GREEN;
		} else if (osGetTime() - s_padOpenedAt > 5000) {
			msg = "Your Mac isn't answering";
			dot = C_ORANGE;
		}
		break;
	case INPUT_OFF: msg = "Paused on your Mac"; dot = C_ORANGE; break;
	case INPUT_OUTDATED: msg = "Update Macapad on your Mac"; dot = C_ORANGE; break;
	case INPUT_FORGOTTEN: msg = "Your Mac forgot this 3DS"; dot = C_RED; break;
	case INPUT_AWAY: msg = "Can't reach your Mac"; dot = C_RED; break;
	}
	float w = text_width(msg, 0.45f);
	text(TOP_W - 14, 15, 0.45f, dot == C_GREEN || dot == C_TEXT3 ? C_TEXT2 : dot, C2D_AlignRight, msg);
	disc(TOP_W - 14 - w - 8, 23, 3.5f, dot);

	// under the title: the preset, or the Mac's reason for being off
	if (info.state == INPUT_OFF && info.message[0]) {
		text_fit(15, 36, 0.45f, C_ORANGE, 0, 370, info.message);
	} else if (info.state == INPUT_OUTDATED) {
		text_fit(15, 36, 0.45f, C_TEXT2, 0, 370, "It needs Macapad 1.0 or later to be a controller.");
	} else {
		char line[96];
		int n = preset_changes(g_controller.preset);
		snprintf(line, sizeof line, "%s%s", preset_name(g_controller.preset), n ? " \xC2\xB7 with your changes" : "");
		text_fit(15, 36, 0.45f, C_TEXT2, 0, 370, line);
	}

	// what each button does, as they sit on the 3DS: left side, right side
	static const Control left[] = {CTL_L, CTL_ZL, CTL_UP, CTL_DOWN, CTL_LEFT, CTL_RIGHT, CTL_CIRCLE, CTL_SELECT};
	static const Control right[] = {CTL_R, CTL_ZR, CTL_A, CTL_B, CTL_X, CTL_Y, CTL_CSTICK, CTL_START};
	for (int side = 0; side < 2; side++) {
		const Control *list = side ? right : left;
		float x = side ? 206 : 14, y = 60;
		for (int i = 0; i < 8; i++) {
			Control c = list[i];
			if (!control_exists(c)) continue;
			bool lit = control_is_stick(c) ? s_stickLit[c == CTL_CSTICK] : (s_padHeld & s_controlKeys[c]) != 0;
			control_row(c, x, y, lit);
			y += 19;
		}
	}
	text(TOP_W / 2, 218, 0.42f, C_TEXT3, C2D_AlignCenter, "To stop, press START and SELECT together.");
}

static void pad_draw_bottom(void) {
	header("Controller", "\xE2\x80\xB9 Done", false);
	bool touching = s_touching && s_zone != ZONE_NONE;

	// the trackpad
	float tw = STRIP_X - 16, th = PAD_BOTTOM - PAD_TOP;
	rrect(8, PAD_TOP, tw, th, 12, C_CARD);
	if (touching && s_zone == ZONE_TRACKPAD) {
		disc(s_fingerX, s_fingerY, 16, C_CARD_HI);
	} else {
		text(8 + tw / 2, PAD_TOP + th / 2 - 18, 0.46f, C_TEXT3, C2D_AlignCenter, "Drag to move the pointer");
		text(8 + tw / 2, PAD_TOP + th / 2 + 2, 0.46f, C_TEXT3, C2D_AlignCenter, "Tap to click");
	}

	// the scroll strip
	u32 bg = touching && s_zone == ZONE_SCROLL ? C_CARD_HI : C_CARD;
	rrect(STRIP_X, PAD_TOP, STRIP_W, th, 12, bg);
	glyph(GLYPH_ARROW_UP, STRIP_X + STRIP_W / 2, PAD_TOP + 20, 12, C_TEXT3, bg);
	text(STRIP_X + STRIP_W / 2, PAD_TOP + th / 2 - 7, 0.38f, C_TEXT3, C2D_AlignCenter, "Scroll");
	glyph(GLYPH_ARROW_DOWN, STRIP_X + STRIP_W / 2, PAD_BOTTOM - 20, 12, C_TEXT3, bg);

	// the click buttons
	float bw = BOTTOM_W / 2 - 10;
	bool left = (touching && s_zone == ZONE_LEFT) || osGetTime() < s_clickUntil, right = touching && s_zone == ZONE_RIGHT;
	rrect(8, CLICK_Y, bw, CLICK_H, 10, left ? C_CARD_HI : C_CARD);
	text(8 + bw / 2, CLICK_Y + 10, 0.46f, C_TEXT, C2D_AlignCenter, "Left click");
	rrect(BOTTOM_W / 2 + 2, CLICK_Y, bw, CLICK_H, 10, right ? C_CARD_HI : C_CARD);
	text(BOTTOM_W / 2 + 2 + bw / 2, CLICK_Y + 10, 0.46f, C_TEXT, C2D_AlignCenter, "Right click");
}

// ---- typing on the Mac ----------------------------------------------------------------------

#define TYPE_MOD_Y 112
#define TYPE_MOD_H 30
#define TYPE_ROW1 150
#define TYPE_ROW2 190
#define TYPE_KEY_H 34

// Keys that work live: held on the Mac while held here, so they repeat. Two rows of four
// columns; the arrows share the last two.
static const struct { const char *key, *label; Glyph g; float x, y, w; } s_typeKeys[] = {
	{"esc", "Esc", GLYPH_NONE, 8, TYPE_ROW1, 71.5f},
	{"tab", "Tab", GLYPH_NONE, 85.5f, TYPE_ROW1, 71.5f},
	{"delete", "Delete", GLYPH_NONE, 163, TYPE_ROW1, 71.5f},
	{"return", "Return", GLYPH_NONE, 240.5f, TYPE_ROW1, 71.5f},
	{"space", "Space", GLYPH_NONE, 8, TYPE_ROW2, 149},
	{"left", "", GLYPH_ARROW_LEFT, 163, TYPE_ROW2, 34.25f},
	{"up", "", GLYPH_ARROW_UP, 201.25f, TYPE_ROW2, 34.25f},
	{"down", "", GLYPH_ARROW_DOWN, 239.5f, TYPE_ROW2, 34.25f},
	{"right", "", GLYPH_ARROW_RIGHT, 277.75f, TYPE_ROW2, 34.25f},
};
#define TYPE_KEYS (int)(sizeof s_typeKeys / sizeof *s_typeKeys)

static bool s_typeMods[4];   // cmd, shift, opt, ctrl: for the next key
static int s_typeKey = -1;   // the key held down on the screen
static bool s_typeLive;      // it's going over live input (else it went as one keystroke)
static char s_typed[64];     // the start of what was sent last

static void start_type(void) {
	memset(s_typeMods, 0, sizeof s_typeMods);
	s_typeKey = -1;
	s_typed[0] = 0;
	input_start();
	s_screen = SCR_TYPE;
}

static void stop_type(void) {
	input_stop();
	s_screen = SCR_MAIN;
}

// "cmd+shift+left": the modifiers that are on, then the key.
static void type_combo(const char *key, char *out, size_t size) {
	static const int order[] = {3, 2, 1, 0};  // macOS order: ctrl, opt, shift, cmd
	out[0] = 0;
	for (int i = 0; i < 4; i++)
		if (s_typeMods[order[i]]) snprintf(out + strlen(out), size - strlen(out), "%s+", s_modNames[order[i]]);
	snprintf(out + strlen(out), size - strlen(out), "%s", key);
}

// The system keyboard, then the text goes to the Mac in one go.
static void type_text(void) {
	InputHeld none = {0};
	input_hold(&none);
	SwkbdState kb;
	char text[512];
	swkbdInit(&kb, SWKBD_TYPE_WESTERN, 2, sizeof text - 1);
	swkbdSetHintText(&kb, "Type something to send to your Mac");
	swkbdSetFeatures(&kb, SWKBD_MULTILINE);
	swkbdSetValidation(&kb, SWKBD_NOTEMPTY, 0, 0);
	if (swkbdInputText(&kb, text, sizeof text) != SWKBD_BUTTON_CONFIRM) return;
	press_text(text);
	copy(s_typed, sizeof s_typed, text);
	char *nl = strchr(s_typed, '\n');
	if (nl) *nl = 0;
}

static void type_update(u32 down, u32 held, touchPosition touch, bool tapped, touchPosition tap) {
	if (down & KEY_B) {
		stop_type();
		return;
	}
	if (tapped && in(tap, 0, 0, 90, 32)) {
		stop_type();
		return;
	}
	if (tapped && in(tap, 8, 38, 304, 50)) {
		type_text();
		return;
	}
	InputInfo info;
	input_info(&info);
	bool live = info.state == INPUT_LIVE && info.through;
	if (down & KEY_TOUCH) {
		for (int i = 0; i < 4; i++)
			if (in(touch, 8 + i * 77.5f, TYPE_MOD_Y, 71.5f, TYPE_MOD_H)) s_typeMods[i] = !s_typeMods[i];
		for (int i = 0; i < TYPE_KEYS; i++) {
			if (!in(touch, s_typeKeys[i].x, s_typeKeys[i].y, s_typeKeys[i].w, TYPE_KEY_H)) continue;
			s_typeKey = i;
			s_typeLive = live;
			if (!live) {
				// no live input (an older Mac, or not through yet): one press, as a keystroke
				char combo[64];
				type_combo(s_typeKeys[i].key, combo, sizeof combo);
				press_combo(combo);
				memset(s_typeMods, 0, sizeof s_typeMods);
			}
		}
	}
	InputHeld h = {0};
	if (s_typeKey >= 0 && !(held & KEY_TOUCH)) {
		if (s_typeLive) memset(s_typeMods, 0, sizeof s_typeMods);  // they were for that key
		s_typeKey = -1;
	}
	if (s_typeKey >= 0 && s_typeLive) {
		char combo[64];
		type_combo(s_typeKeys[s_typeKey].key, combo, sizeof combo);
		held_add_combo(&h, combo);
	}
	// the +Control Pad works as arrow keys here too
	static const struct { u32 bit; const char *key; } arrows[] = {
		{KEY_DUP, "up"}, {KEY_DDOWN, "down"}, {KEY_DLEFT, "left"}, {KEY_DRIGHT, "right"},
	};
	for (int i = 0; i < 4; i++) {
		if (live && (held & arrows[i].bit)) held_add(&h, arrows[i].key);
		else if (!live && (down & arrows[i].bit)) press_combo(arrows[i].key);
	}
	input_hold(&h);
}

static void type_draw(void) {
	header("Type on your Mac", "\xE2\x80\xB9 Done", false);
	rrect(8, 38, 304, 50, 12, C_CARD);
	tile(20, 51, C_ORANGE, GLYPH_KEYS, "");
	if (s_typed[0]) {
		char sub[96];
		snprintf(sub, sizeof sub, "Sent \xE2\x80\x9C%s\xE2\x80\x9D", s_typed);
		text(54, 43, 0.55f, C_TEXT, 0, "Type something\xE2\x80\xA6");
		text_fit(54, 65, 0.4f, C_TEXT2, 0, 230, sub);
	} else {
		text(54, 54, 0.55f, C_TEXT, 0, "Type something\xE2\x80\xA6");
	}
	glyph(GLYPH_CHEVRON_R, 297, 63, 12, C_TEXT3, C_CARD);

	text(20, 95, 0.38f, C_TEXT2, 0, "ADD TO THE NEXT KEY");
	for (int i = 0; i < 4; i++) {
		float x = 8 + i * 77.5f;
		rrect(x, TYPE_MOD_Y, 71.5f, TYPE_MOD_H, 8, s_typeMods[i] ? C_ORANGE : C_CARD);
		text(x + 71.5f / 2, TYPE_MOD_Y + 8, 0.42f, s_typeMods[i] ? C_BG : C_TEXT, C2D_AlignCenter, s_modLabels[i]);
	}
	for (int i = 0; i < TYPE_KEYS; i++) {
		float x = s_typeKeys[i].x, y = s_typeKeys[i].y, w = s_typeKeys[i].w;
		u32 bg = i == s_typeKey ? C_CARD_HI : C_CARD;
		rrect(x, y, w, TYPE_KEY_H, 8, bg);
		if (s_typeKeys[i].g != GLYPH_NONE) glyph(s_typeKeys[i].g, x + w / 2, y + TYPE_KEY_H / 2, 13, C_TEXT, bg);
		else text(x + w / 2, y + 9, 0.46f, C_TEXT, C2D_AlignCenter, s_typeKeys[i].label);
	}
}

// ---- top screen dashboard --------------------------------------------------------------

static const char *s_days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *s_months[] = {"January", "February", "March", "April", "May", "June", "July",
	"August", "September", "October", "November", "December"};

static void fmt_time(char *out, size_t size, int ms) {
	int s = ms / 1000;
	snprintf(out, size, "%d:%02d", s / 60, s % 60);
}

static void fmt_bytes(char *out, size_t size, double bytes) {
	double gb = bytes / 1e9;
	if (gb >= 100) snprintf(out, size, "%.0f GB", gb);
	else snprintf(out, size, "%.1f GB", gb);
}

// The header: time, date and the Mac on the left; the weather on the right.
// Big and small text share a baseline so "PM" and "°" sit level with the digits.
#define HEAD_BASE 40
#define HEAD_LINE2 47
#define HEAD_LINE3 63

static void draw_clock(void) {
	// The Mac's local time: this 3DS's clock (local time already) plus the difference measured at
	// the last answer, which is kept on the SD card, so it's right from the first frame and stays
	// while the Mac is away.
	time_t now = time(NULL) + s_st.clockDelta;
	struct tm *tm = gmtime(&now);
	char clock[16], date[48];
	int hour = tm->tm_hour;
	if (!g_config.clock24) hour = hour % 12 ? hour % 12 : 12;
	snprintf(clock, sizeof clock, g_config.clock24 ? "%02d:%02d" : "%d:%02d", hour, tm->tm_min);
	text(14, HEAD_BASE, 1.3f, C_TEXT, C2D_AtBaseline, clock);
	if (!g_config.clock24)
		text(18 + text_width(clock, 1.3f), HEAD_BASE, 0.5f, C_TEXT2, C2D_AtBaseline, tm->tm_hour < 12 ? "AM" : "PM");
	snprintf(date, sizeof date, "%s, %s %d", s_days[tm->tm_wday], s_months[tm->tm_mon], tm->tm_mday);
	text(15, HEAD_LINE2, 0.5f, C_TEXT2, 0, date);
}

static void draw_mac_line(void) {
	const char *name = s_st.macName[0] ? s_st.macName : g_config.macName;
	u32 dot = !s_st.live ? C_TEXT3 : s_st.online ? C_GREEN : C_RED;
	disc(19, HEAD_LINE3 + 7, 3, dot);
	char line[160];
	if (s_st.live && !s_st.online) {
		text(27, HEAD_LINE3, 0.42f, C_RED, 0, "Can't reach your Mac");
		return;
	}
	snprintf(line, sizeof line, "%s%s%s", name, s_st.frontApp[0] ? " \xC2\xB7 " : "", s_st.frontApp);
	text_fit(27, HEAD_LINE3, 0.42f, C_TEXT3, 0, 170, line);
}

// ---- weather icons, drawn from shapes, centered on (cx, cy), about s px across ----

#define C_CLOUD C2D_Color32(0xC7, 0xC7, 0xCC, 0xFF)

static void sun(float cx, float cy, float s, u32 c) {
	disc(cx, cy, s * 0.24f, c);
	for (int i = 0; i < 8; i++) {
		float a = i * (float)M_PI / 4, ca = cosf(a), sa = sinf(a);
		C2D_DrawLine(cx + ca * s * 0.34f, cy + sa * s * 0.34f, c, cx + ca * s * 0.47f, cy + sa * s * 0.47f, c, s * 0.07f, 0.5f);
	}
}

static void moon(float cx, float cy, float s, u32 c) {
	disc(cx, cy, s * 0.32f, c);
	disc(cx + s * 0.16f, cy - s * 0.1f, s * 0.27f, C_BG);
}

static void cloud(float cx, float cy, float s, u32 c) {
	rrect(cx - s * 0.46f, cy, s * 0.92f, s * 0.3f, s * 0.15f, c);
	disc(cx - s * 0.2f, cy + s * 0.04f, s * 0.2f, c);
	disc(cx + s * 0.08f, cy - s * 0.04f, s * 0.27f, c);
}

static void weather_icon(const char *symbol, bool day, float cx, float cy, float s) {
	if (!strcmp(symbol, "clear")) {
		if (day) sun(cx, cy, s, C_YELLOW);
		else moon(cx, cy, s, C_CLOUD);
	} else if (!strcmp(symbol, "partly-cloudy")) {
		if (day) sun(cx - s * 0.15f, cy - s * 0.15f, s * 0.8f, C_YELLOW);
		else moon(cx - s * 0.12f, cy - s * 0.15f, s * 0.8f, C_CLOUD);
		cloud(cx + s * 0.08f, cy + s * 0.08f, s * 0.8f, C_CLOUD);
	} else if (!strcmp(symbol, "fog")) {
		for (int i = 0; i < 3; i++)
			rrect(cx - s * (i == 1 ? 0.38f : 0.46f), cy - s * 0.26f + i * s * 0.22f, s * (i == 1 ? 0.84f : 0.76f), s * 0.1f, s * 0.05f, C_CLOUD);
	} else if (!strcmp(symbol, "rain") || !strcmp(symbol, "drizzle") || !strcmp(symbol, "snow") || !strcmp(symbol, "storm")) {
		cloud(cx, cy - s * 0.18f, s, C_CLOUD);
		float y = cy + s * 0.2f;
		if (!strcmp(symbol, "storm")) {
			C2D_DrawTriangle(cx + s * 0.06f, y - s * 0.04f, C_YELLOW, cx - s * 0.14f, y + s * 0.16f, C_YELLOW, cx + s * 0.02f, y + s * 0.16f, C_YELLOW, 0.5f);
			C2D_DrawTriangle(cx + s * 0.06f, y + s * 0.12f, C_YELLOW, cx - s * 0.1f, y + s * 0.34f, C_YELLOW, cx - s * 0.02f, y + s * 0.12f, C_YELLOW, 0.5f);
			return;
		}
		for (int i = -1; i <= 1; i++) {
			float x = cx + i * s * 0.22f;
			if (!strcmp(symbol, "rain")) C2D_DrawLine(x + s * 0.05f, y, C_TEAL, x - s * 0.03f, y + s * 0.2f, C_TEAL, s * 0.06f, 0.5f);
			else if (!strcmp(symbol, "drizzle")) disc(x, y + s * 0.1f, s * 0.04f, C_TEAL);
			else disc(x, y + s * 0.1f, s * 0.055f, C_TEXT);
		}
	} else {
		cloud(cx, cy, s, C_CLOUD);
	}
}

static void draw_weather(void) {
	if (!s_st.hasWeather) return;
	char temp[16], line[96];
	float right = TOP_W - 14;
	snprintf(temp, sizeof temp, "%d\xC2\xB0", s_st.temp);
	text(right, HEAD_BASE, 1.3f, C_TEXT, C2D_AlignRight | C2D_AtBaseline, temp);
	weather_icon(s_st.symbol, s_st.isDay, right - text_width(temp, 1.3f) - 22, HEAD_BASE - 13, 32);
	snprintf(line, sizeof line, "%s \xC2\xB7 H %d\xC2\xB0 L %d\xC2\xB0", s_st.condition, s_st.high, s_st.low);
	text_fit(right, HEAD_LINE2, 0.5f, C_TEXT2, C2D_AlignRight, 175, line);
	if (s_st.place[0]) text_fit(right, HEAD_LINE3, 0.42f, C_TEXT3, C2D_AlignRight, 170, s_st.place);
}

static void draw_music_note(float x, float y, float s, u32 c) {
	ellipse(x, y + s * 0.65f, s * 0.4f, s * 0.3f, c);
	C2D_DrawRectSolid(x + s * 0.3f, y, 0.5f, s * 0.1f, s * 0.8f, c);
	C2D_DrawTriangle(x + s * 0.3f, y, c, x + s * 0.7f, y + s * 0.15f, c, x + s * 0.3f, y + s * 0.3f, c, 0.5f);
}

#define CARD_Y 78
#define CARD_H 96

// What the music is doing, centered on (cx, cy): bars that bounce while it plays, or the
// pause sign with "Paused" under it. (The state, not a button: the Play/Pause key is below.)
static void draw_play_state(float cx, float cy) {
	if (s_st.playing) {
		float t = osGetTime() / 1000.0f;
		for (int i = 0; i < 4; i++) {
			float h = 7 + 6 * (0.5f + 0.5f * sinf(t * (5.5f + i * 1.3f) + i * 1.9f));
			rrect(cx - 10 + i * 6, cy + 7 - h, 4, h, 1.5f, C_GREEN);
		}
		return;
	}
	glyph(GLYPH_PAUSE, cx, cy - 2, 11, C_TEXT2, C_CARD);
	text(cx, cy + 7, 0.34f, C_TEXT3, C2D_AlignCenter, "Paused");
}

static void draw_now_playing(void) {
	rrect(12, CARD_Y, TOP_W - 24, CARD_H, 14, C_CARD);
	if (s_st.outdated && !s_st.live) {
		text(TOP_W / 2, CARD_Y + 26, 0.55f, C_ORANGE, C2D_AlignCenter, "Update Macapad on your Mac");
		text(TOP_W / 2, CARD_Y + 50, 0.45f, C_TEXT2, C2D_AlignCenter, "Keys work, but it can't show the song or stats.");
		return;
	}
	if (!s_st.live) {  // the song is never shown from the snapshot: it has to be current
		char msg[96];
		snprintf(msg, sizeof msg, "Connecting to %s\xE2\x80\xA6", g_config.macName[0] ? g_config.macName : "your Mac");
		text_fit(TOP_W / 2, CARD_Y + 38, 0.5f, C_TEXT2, C2D_AlignCenter, 340, msg);
		return;
	}
	if (s_st.off) {
		text(TOP_W / 2, CARD_Y + 26, 0.55f, C_ORANGE, C2D_AlignCenter, "Keys are off right now");
		text_fit(TOP_W / 2, CARD_Y + 50, 0.45f, C_TEXT2, C2D_AlignCenter, 350, s_st.offMessage);
		return;
	}
	if (!s_st.hasTrack) {
		draw_music_note(36, CARD_Y + 28, 40, C_TEXT3);
		text(100, CARD_Y + 28, 0.55f, C_TEXT2, 0, "Nothing playing");
		text(100, CARD_Y + 50, 0.42f, C_TEXT3, 0, "Play something on your Mac.");
		return;
	}
	float ax = 20, ay = CARD_Y + 8, as = 80;
	if (s_art.ok) {
		picture_draw(&s_art, ax, ay, as, as);
	} else {
		rrect(ax, ay, as, as, 10, C_CARD_HI);
		draw_music_note(ax + 22, ay + 20, 40, C_TEXT3);
	}
	float tx = ax + as + 12, tw = TOP_W - 24 - tx;
	text_fit(tx, CARD_Y + 8, 0.62f, C_TEXT, 0, tw - 28, s_st.title);
	text_fit(tx, CARD_Y + 30, 0.5f, C_TEXT2, 0, tw - (s_st.playing ? 0 : 50), s_st.artist);
	char sub[160];
	snprintf(sub, sizeof sub, "%s%s%s", s_st.album, s_st.album[0] ? " \xC2\xB7 " : "", s_st.app);
	text_fit(tx, CARD_Y + 48, 0.42f, C_TEXT3, 0, tw, sub);
	draw_play_state(TOP_W - 30, CARD_Y + 17);

	int pos = s_st.position;
	if (s_st.playing) pos += (int)(osGetTime() - s_st.receivedAt);
	if (s_st.duration && pos > s_st.duration) pos = s_st.duration;
	if (s_st.duration > 0) {
		float bx = tx, bw = tw - 2, by = CARD_Y + 70;
		rrect(bx, by, bw, 4, 2, C_CARD_HI);
		rrect(bx, by, bw * pos / (float)s_st.duration, 4, 2, C_TEXT);
		char a[16], b[16];
		fmt_time(a, sizeof a, pos);
		fmt_time(b, sizeof b, s_st.duration);
		text(bx, by + 6, 0.38f, C_TEXT3, 0, a);
		text(bx + bw, by + 6, 0.38f, C_TEXT3, C2D_AlignRight, b);
	}
}

static void stat_tile(int i, const char *label, const char *value, const char *note, float frac, u32 color) {
	float w = (TOP_W - 24 - 3 * 8) / 4.0f, x = 12 + i * (w + 8), y = 184;
	rrect(x, y, w, 48, 12, C_CARD);
	text(x + 9, y + 5, 0.4f, C_TEXT2, 0, label);
	text_fit(x + 9, y + 18, 0.56f, C_TEXT, 0, w - 14, value);
	if (note) text(x + w - 8, y + 24, 0.36f, color, C2D_AlignRight, note);
	if (frac >= 0) {
		if (frac > 1) frac = 1;
		rrect(x + 9, y + 39, w - 18, 4, 2, C_CARD_HI);
		rrect(x + 9, y + 39, (w - 18) * frac, 4, 2, color);
	}
}

static u32 load_color(double frac) { return frac > 0.85 ? C_RED : frac > 0.6 ? C_YELLOW : C_GREEN; }

static void draw_stats(void) {
	char v[32];
	if (!s_st.have) return;
	snprintf(v, sizeof v, "%.0f%%", s_st.cpu);
	stat_tile(0, "CPU", v, NULL, s_st.cpu / 100, load_color(s_st.cpu / 100));

	double mem = s_st.memTotal ? s_st.memUsed / s_st.memTotal : 0;
	fmt_bytes(v, sizeof v, s_st.memUsed);
	stat_tile(1, "Memory", v, NULL, mem, load_color(mem));

	if (s_st.battery >= 0) {
		snprintf(v, sizeof v, "%d%%", s_st.battery);
		const char *note = s_st.charging ? "charging" : s_st.pluggedIn ? "on power" : NULL;
		stat_tile(2, "Battery", v, note, s_st.battery / 100.0, s_st.battery <= 20 ? C_RED : C_GREEN);
	} else {
		int h = s_st.uptime / 3600;
		if (h >= 48) snprintf(v, sizeof v, "%d days", h / 24);
		else snprintf(v, sizeof v, "%dh %dm", h, s_st.uptime / 60 % 60);
		stat_tile(2, "Up for", v, NULL, -1, C_TEXT);
	}

	fmt_bytes(v, sizeof v, s_st.diskFree);
	double used = s_st.diskTotal ? 1 - s_st.diskFree / s_st.diskTotal : 0;
	stat_tile(3, "Disk free", v, NULL, used, load_color(used));
}

static void draw_toast(void) {
	char msg[160];
	bool error;
	if (!toast_get(msg, sizeof msg, &error)) return;
	float w = text_width(msg, 0.46f) + 28;
	if (w > TOP_W - 24) w = TOP_W - 24;
	rrect(TOP_W / 2 - w / 2, 194, w, 30, 15, error ? C_RED : C_CARD_HI);
	text_fit(TOP_W / 2, 201, 0.46f, C_TEXT, C2D_AlignCenter, w - 24, msg);
}

static void draw_dashboard(void) {
	draw_clock();
	draw_mac_line();
	draw_weather();
	draw_now_playing();
	draw_stats();
	draw_toast();
}

// ---- finding + pairing --------------------------------------------------------------

static void start_find(const char *message) {
	copy(s_findMessage, sizeof s_findMessage, message);
	s_screen = SCR_FIND;
	scan_finish(&s_scan);
	scan_start(&s_scan);
}

static void start_pair(const FoundMac *mac) {
	scan_finish(&s_scan);
	s_pair.mac = *mac;
	pair_start(&s_pair);
	s_screen = SCR_PAIR;
}

static void start_main(void) {
	s_screen = SCR_MAIN;
	s_page = 0;
	memset(&s_st, 0, sizeof s_st);
	s_wasOnline = false;
	macapad_start();
	icons_prefetch(false);
}

#define FIND_ROW_Y 70

static void type_address(void) {
	char addr[64];
	if (!ask_text("Your Mac's address, like 192.168.1.20", NULL, SWKBD_TYPE_NUMPAD, addr, sizeof addr)) return;
	// forgive "http://", a trailing "/" and stray spaces
	char *start = addr;
	while (*start == ' ') start++;
	for (char *end = start + strlen(start); end > start && end[-1] == ' '; ) *--end = 0;
	char host[64];
	int port;
	struct in_addr ip;
	if (!url_split(start, host, sizeof host, &port) || !inet_aton(host, &ip)) {
		snprintf(s_findMessage, sizeof s_findMessage, "\"%s\" isn't an IP address, like 192.168.1.20.", start);
		return;
	}
	FoundMac mac;
	if (probe_mac(host, port, &mac)) start_pair(&mac);
	else snprintf(s_findMessage, sizeof s_findMessage, "Nothing answered at %s:%d. Check the address and Wi-Fi.", host, port);
}

static void find_tap(touchPosition t) {
	for (int i = 0; i < s_scan.count && i < 3; i++)
		if (in(t, 8, FIND_ROW_Y + i * 48, 304, 42)) { start_pair(&s_scan.macs[i]); return; }
	if (!s_scan.running && in(t, 8, 200, 96, 34)) start_find(NULL);
	if (in(t, 112, 200, 96, 34)) type_address();
	if (in(t, 216, 200, 96, 34)) start_qr();
}

// The Macapad website as a QR code, for anyone who doesn't have Macapad on their Mac yet.
// 29x29 modules, a row per u32 (bit 28 = leftmost), made with
//   qrencode -l M -m 0 -t ASCII "https://ad-archer.github.io/Macapad/"
static const u32 s_siteQr[29] = {
	0x1FCBD27F, 0x10567E41, 0x1744E15D, 0x1744C45D,
	0x1756765D, 0x104BA241, 0x1FD5557F, 0x0000FB00,
	0x15405C12, 0x06895149, 0x155E8877, 0x191A2AE2,
	0x04C51BCB, 0x133C91C9, 0x1960547B, 0x0293ED5A,
	0x14C349CB, 0x0F2951ED, 0x17CC8913, 0x0A052D8A,
	0x105B1FF0, 0x00149317, 0x1FC34B5B, 0x104CCF1A,
	0x175C51F3, 0x174B5337, 0x175B4139, 0x1046AD02,
	0x1FD99BD3
};

// A QR code at (x, y), m px per module, on white with a 2-module margin.
static void draw_qr(const u32 *rows, int n, float x, float y, float m) {
	float margin = 2 * m;
	rrect(x, y, n * m + 2 * margin, n * m + 2 * margin, 4, C_TEXT);
	for (int r = 0; r < n; r++) {
		// a run of dark modules is one rectangle
		for (int c = 0; c < n; ) {
			if (!(rows[r] >> (n - 1 - c) & 1)) { c++; continue; }
			int start = c;
			while (c < n && rows[r] >> (n - 1 - c) & 1) c++;
			C2D_DrawRectSolid(x + margin + start * m, y + margin + r * m, 0.5f, (c - start) * m, m, C_BG);
		}
	}
}

static void find_draw_top(void) {
	text(20, 34, 0.9f, C_TEXT, 0, "Find your Mac");
	text(20, 80, 0.48f, C_TEXT2, 0, "Open Macapad on your Mac,");
	text(20, 98, 0.48f, C_TEXT2, 0, "and keep both on the");
	text(20, 116, 0.48f, C_TEXT2, 0, "same Wi-Fi.");
	text(20, 146, 0.44f, C_TEXT3, 0, "Or tap Scan code to scan the");
	text(20, 162, 0.44f, C_TEXT3, 0, "QR code Macapad shows.");

	float qs = 29 * 3 + 12, qx = TOP_W - 24 - qs, qy = 28;
	draw_qr(s_siteQr, 29, qx, qy, 3);
	// centered under the code, but kept on screen when the font runs wide
	const char *site = "ad-archer.github.io/Macapad";
	float cx = qx + qs / 2, half = text_width(site, 0.36f) / 2;
	if (cx + half > TOP_W - 10) cx = TOP_W - 10 - half;
	text(cx, qy + qs + 6, 0.45f, C_TEXT, C2D_AlignCenter, "No Macapad yet?");
	text(cx, qy + qs + 23, 0.36f, C_TEXT2, C2D_AlignCenter, site);

	if (s_findMessage[0]) text_fit(TOP_W / 2, 196, 0.48f, C_ORANGE, C2D_AlignCenter, 370, s_findMessage);
	if (gethostid() == 0) text(TOP_W / 2, 216, 0.48f, C_RED, C2D_AlignCenter, "This 3DS isn't on Wi-Fi.");
}

static void find_draw_bottom(void) {
	header(s_scan.running ? "Looking\xE2\x80\xA6" : s_scan.count ? "Pick your Mac" : "No Mac found", NULL, false);
	if (s_scan.running) {
		rrect(40, 44, 240, 6, 3, C_CARD);
		rrect(40, 44, 240 * s_scan.progress / 254.0f, 6, 3, C_BLUE);
	}
	for (int i = 0; i < s_scan.count && i < 3; i++) {
		float y = FIND_ROW_Y + i * 48;
		FoundMac *m = &s_scan.macs[i];
		rrect(8, y, 304, 42, 12, C_CARD);
		text_fit(20, y + 5, 0.52f, C_TEXT, 0, 270, m->name);
		text(20, y + 23, 0.4f, m->canPair ? C_TEXT3 : C_ORANGE, 0, m->canPair ? m->host : "Turn on pairing in Macapad's Settings");
	}
	if (!s_scan.running && !s_scan.count)
		text(BOTTOM_W / 2, 110, 0.46f, C_TEXT2, C2D_AlignCenter, "Is Macapad open on your Mac?");
	if (!s_scan.running) {
		rrect(8, 200, 96, 34, 10, C_CARD);
		text_fit(56, 210, 0.42f, C_BLUE, C2D_AlignCenter, 88, "Look again");
	}
	rrect(112, 200, 96, 34, 10, C_CARD);
	text_fit(160, 210, 0.42f, C_BLUE, C2D_AlignCenter, 88, "Type address");
	rrect(216, 200, 96, 34, 10, C_CARD);
	text_fit(264, 210, 0.42f, C_BLUE, C2D_AlignCenter, 88, "Scan code");
}

// ---- scanning the Mac's QR code ----------------------------------------------------

static Picture s_camera;
static u16 *s_cameraFrame;
static char s_qrMessage[96];

static void stop_qr(void) {
	qrscan_stop();
	picture_free(&s_camera);
	free(s_cameraFrame);
	s_cameraFrame = NULL;
}

static void start_qr(void) {
	scan_finish(&s_scan);
	pair_finish(&s_pair);
	s_qrMessage[0] = 0;
	s_cameraFrame = malloc(QRSCAN_PREVIEW_W * QRSCAN_PREVIEW_H * sizeof(u16));
	if (!s_cameraFrame || !picture_init565(&s_camera, QRSCAN_PREVIEW_W, QRSCAN_PREVIEW_H) || !qrscan_start()) {
		stop_qr();
		start_find("Can't start the camera.");
		return;
	}
	s_screen = SCR_QR;
}

static void qr_update(u32 down, touchPosition t, bool tapped) {
	if ((down & KEY_B) || (tapped && in(t, 8, 196, 304, 38))) {
		stop_qr();
		start_find(NULL);
		return;
	}
	if (qrscan_preview(s_cameraFrame)) picture_update565(&s_camera, s_cameraFrame);
	char link[1024];
	if (!qrscan_take(link, sizeof link)) return;
	if (pair_qr_start(&s_pair, link)) {
		stop_qr();
		s_screen = SCR_PAIR;
	} else {
		copy(s_qrMessage, sizeof s_qrMessage, "That's not a Macapad pairing code.");
	}
}

static void qr_draw_top(void) {
	float x = (TOP_W - QRSCAN_PREVIEW_W) / 2.0f;
	picture_draw(&s_camera, x, 0, QRSCAN_PREVIEW_W, QRSCAN_PREVIEW_H);
	// corner marks to aim with
	u32 c = C_TEXT;
	float l = 22, th = 3, bx = x + 70, by = 30, bw = QRSCAN_PREVIEW_W - 140, bh = SCREEN_H - 60;
	C2D_DrawRectSolid(bx, by, 0.5f, l, th, c);
	C2D_DrawRectSolid(bx, by, 0.5f, th, l, c);
	C2D_DrawRectSolid(bx + bw - l, by, 0.5f, l, th, c);
	C2D_DrawRectSolid(bx + bw - th, by, 0.5f, th, l, c);
	C2D_DrawRectSolid(bx, by + bh - th, 0.5f, l, th, c);
	C2D_DrawRectSolid(bx, by + bh - l, 0.5f, th, l, c);
	C2D_DrawRectSolid(bx + bw - l, by + bh - th, 0.5f, l, th, c);
	C2D_DrawRectSolid(bx + bw - th, by + bh - l, 0.5f, th, l, c);
	if (s_qrMessage[0]) {
		float w = text_width(s_qrMessage, 0.46f) + 28;
		rrect(TOP_W / 2 - w / 2, 196, w, 30, 15, C_RED);
		text(TOP_W / 2, 203, 0.46f, C_TEXT, C2D_AlignCenter, s_qrMessage);
	}
}

static void qr_draw_bottom(void) {
	header("Scan code", NULL, false);
	text(20, 46, 0.48f, C_TEXT, 0, "1. On your Mac, click Macapad in");
	text(20, 64, 0.48f, C_TEXT, 0, "    the menu bar.");
	text(20, 88, 0.48f, C_TEXT, 0, "2. Choose Pair with QR Code\xE2\x80\xA6");
	text(20, 112, 0.48f, C_TEXT, 0, "3. Point the camera on the back");
	text(20, 130, 0.48f, C_TEXT, 0, "    of this 3DS at the code.");
	text(BOTTOM_W / 2, 162, 0.42f, C_TEXT3, C2D_AlignCenter, "Hold it about a foot away, straight on.");
	rrect(8, 196, 304, 38, 10, C_CARD);
	text(BOTTOM_W / 2, 206, 0.5f, C_RED, C2D_AlignCenter, "Cancel");
}

static void pair_update(u32 down, touchPosition t, bool tapped) {
	PairState st = s_pair.state;
	if (st == PAIR_APPROVED) {
		pair_finish(&s_pair);
		start_main();
		return;
	}
	bool finished = st == PAIR_DENIED || st == PAIR_EXPIRED || st == PAIR_FAILED;
	if ((down & KEY_B) || (tapped && in(t, 8, 196, 304, 38))) {
		pair_finish(&s_pair);
		if (finished && !(down & KEY_B)) {
			// "Try again"
			if (s_pair.viaQr) {
				start_qr();
			} else {
				FoundMac mac = s_pair.mac;
				start_pair(&mac);
			}
		} else {
			start_find(NULL);
		}
	}
}

static void pair_draw_top(void) {
	char line[128];
	snprintf(line, sizeof line, "Pairing with %s", s_pair.mac.name);
	text_fit(TOP_W / 2, 24, 0.55f, C_TEXT2, C2D_AlignCenter, 370, line);
	switch (s_pair.state) {
	case PAIR_STARTING:
		text(TOP_W / 2, 100, 0.6f, C_TEXT2, C2D_AlignCenter, "Asking your Mac\xE2\x80\xA6");
		break;
	case PAIR_WAITING: {
		float w = 64, gap = 12, x = TOP_W / 2 - (4 * w + 3 * gap) / 2;
		for (int i = 0; i < 4 && s_pair.code[i]; i++) {
			char d[2] = {s_pair.code[i], 0};
			rrect(x + i * (w + gap), 62, w, 80, 14, C_CARD);
			text(x + i * (w + gap) + w / 2, 72, 1.8f, C_TEXT, C2D_AlignCenter, d);
		}
		text(TOP_W / 2, 164, 0.5f, C_TEXT, C2D_AlignCenter, "Your Mac is asking if it can trust this 3DS.");
		text(TOP_W / 2, 184, 0.5f, C_TEXT2, C2D_AlignCenter, "If it shows this code, click Allow.");
		break;
	}
	case PAIR_DENIED:
		text(TOP_W / 2, 100, 0.6f, C_ORANGE, C2D_AlignCenter, "Your Mac said no.");
		break;
	case PAIR_EXPIRED:
		text(TOP_W / 2, 100, 0.6f, C_ORANGE, C2D_AlignCenter, "That took too long.");
		break;
	case PAIR_FAILED: {
		// up to two lines, split at a newline: what went wrong, then what to do about it
		char line[160];
		copy(line, sizeof line, s_pair.error);
		char *nl = strchr(line, '\n');
		if (nl) *nl = 0;
		text_fit(TOP_W / 2, nl ? 86 : 100, 0.52f, C_ORANGE, C2D_AlignCenter, 380, line);
		if (nl) text_fit(TOP_W / 2, 112, 0.46f, C_TEXT2, C2D_AlignCenter, 380, nl + 1);
		break;
	}
	case PAIR_APPROVED:
		break;
	}
}

static void pair_draw_bottom(void) {
	PairState st = s_pair.state;
	bool finished = st == PAIR_DENIED || st == PAIR_EXPIRED || st == PAIR_FAILED;
	header("Pair", NULL, false);
	text(BOTTOM_W / 2, 90, 0.48f, C_TEXT2, C2D_AlignCenter, finished ? "Want to try again?" : "Look at your Mac's screen.");
	rrect(8, 196, 304, 38, 10, C_CARD);
	text(BOTTOM_W / 2, 206, 0.5f, finished ? C_BLUE : C_RED, C2D_AlignCenter, finished ? "Try again" : "Cancel");
}

// ---- main --------------------------------------------------------------------------

// Nothing stays held on the Mac while the 3DS sleeps or sits in the HOME Menu.
static void on_apt(APT_HookType hook, void *param) {
	if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP) input_release();
}

int main(void) {
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS * 4);
	C2D_Prepare();
	C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	C3D_RenderTarget *bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	gfx_init();
	osSetSpeedupEnable(true);

	u32 *socBuf = memalign(0x1000, SOC_BUFFERSIZE);
	socInit(socBuf, SOC_BUFFERSIZE);

	macapad_init();
	config_load();
	pads_load();
	controller_load();
	APT_CheckNew3DS(&s_isNew3ds);
	aptHookCookie aptCookie;
	aptHook(&aptCookie, on_apt, NULL);
	if (g_config.token[0]) start_main();
	else start_find(NULL);

	while (aptMainLoop() && !s_quit) {
		hidScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld(), up = hidKeysUp();
		touchPosition touch;
		hidTouchRead(&touch);

		// turn raw touch into taps, drags and long presses
		bool tapped = false, released = false;
		touchPosition tap = s_touchStart;
		if (down & KEY_TOUCH) {
			s_touching = true;
			s_dragging = s_longFired = s_swiping = s_swipeAxis = false;
			s_touchStart = s_touchNow = touch;
			s_touchAt = osGetTime();
		}
		if ((held & KEY_TOUCH) && s_touching) {
			s_touchNow = touch;
			if (abs(touch.px - s_touchStart.px) > 8 || abs(touch.py - s_touchStart.py) > 8) s_dragging = true;
		}
		if ((up & KEY_TOUCH) && s_touching) {
			s_touching = false;
			released = true;
			tapped = !s_dragging && !s_longFired;
		}

		if (s_screen != SCR_FIND && s_screen != SCR_QR && s_screen != SCR_PAIR) status_get(&s_st);
		if (s_st.online && !s_wasOnline) icons_retry();
		s_wasOnline = s_st.online;
		u8 *px;
		int w, h;
		if (artwork_take(&px, &w, &h)) {
			if (px) picture_load(&s_art, px, w, h);
			else picture_free(&s_art);
			free(px);
		}
		icons_collect();

		switch (s_screen) {
		case SCR_FIND:
			if (tapped) find_tap(tap);
			break;
		case SCR_QR:
			qr_update(down, tap, tapped);
			break;
		case SCR_PAIR:
			pair_update(down, tap, tapped);
			break;
		case SCR_MAIN:
			main_update(down);
			if (s_screen != SCR_MAIN) break;
			swipe_update(released);
			// hold a key to change it
			if (s_touching && !s_dragging && !s_longFired && osGetTime() - s_touchAt > LONG_PRESS_MS) {
				int slot = slot_at(s_touchStart);
				s_longFired = true;
				if (slot >= 0) edit_slot(slot);
			} else if (tapped) {
				main_tap(tap);
			}
			if (s_screen != SCR_MAIN) {
				s_slide = 0;
				s_swiping = false;
			}
			break;
		case SCR_LIST:
			list_update(down, held, touch);
			if (tapped && s_screen == SCR_LIST && !in(tap, 0, 0, 90, 32)) list_tap(tap);
			break;
		case SCR_KEYS:
			if (down & KEY_B) keys_back();
			else if (tapped) keys_tap(tap);
			break;
		case SCR_PAD:
			pad_update(down, held, touch, released);
			break;
		case SCR_TYPE:
			type_update(down, held, touch, tapped, tap);
			break;
		}

		gfx_frame();
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C2D_TargetClear(top, C_BG);
		C2D_SceneBegin(top);
		if (s_screen == SCR_FIND) find_draw_top();
		else if (s_screen == SCR_QR) qr_draw_top();
		else if (s_screen == SCR_PAIR) pair_draw_top();
		else if (s_screen == SCR_PAD) pad_draw_top();
		else draw_dashboard();

		C2D_TargetClear(bottom, C_BG);
		C2D_SceneBegin(bottom);
		switch (s_screen) {
		case SCR_FIND: find_draw_bottom(); break;
		case SCR_QR: qr_draw_bottom(); break;
		case SCR_PAIR: pair_draw_bottom(); break;
		case SCR_MAIN: pad_draw(); break;
		case SCR_LIST: list_draw(); break;
		case SCR_KEYS: keys_draw(); break;
		case SCR_PAD: pad_draw_bottom(); break;
		case SCR_TYPE: type_draw(); break;
		}
		C3D_FrameEnd(0);
	}

	input_stop();
	aptUnhook(&aptCookie);
	scan_finish(&s_scan);
	stop_qr();
	pair_finish(&s_pair);
	catalogs_finish();
	macapad_stop();
	picture_free(&s_art);
	for (int i = 0; i < ICON_SLOTS; i++) picture_free(&s_icons[i].pic);
	socExit();
	gfx_exit();
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	return 0;
}
