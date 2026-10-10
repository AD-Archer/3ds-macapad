// Macapad for 3DS: the top screen shows the time, what's playing and how the Mac is doing;
// the bottom screen is a pad of keys that run things on the Mac.
#include <3ds.h>
#include <arpa/inet.h>
#include <citro2d.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gfx.h"
#include "http.h"
#include "macapad.h"

#define SOC_BUFFERSIZE 0x100000
#define LONG_PRESS_MS 500
#define ROW_H 36
#define LIST_TOP 34

typedef enum { SCR_FIND, SCR_PAIR, SCR_MAIN, SCR_LIST, SCR_KEYS } Screen;

// What the list on screen is for, so a pick knows where to go.
typedef enum {
	LIST_KIND,       // "What should this key do?"
	LIST_ACTIONS,
	LIST_APPS,
	LIST_SHORTCUTS,
	LIST_KEYNAME,    // the key in a keyboard shortcut
	LIST_MENU,       // START menu
} ListPurpose;

typedef struct {
	char label[64];
	char sub[48];
	char value[192];
	u32 color;
	int id;
} Item;

typedef struct {
	ListPurpose purpose;
	char title[48];
	Item *items;
	int count;
	int sel;
	float scroll;
	bool loading;
	char empty[96];
} List;

// ---- state -------------------------------------------------------------------

static Screen s_screen;
static Scan s_scan;
static Pairing s_pair;
static Catalog s_catalog;
static List s_list;
static Status s_st;

static int s_page;
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

// artwork + app icons
static Picture s_art;
typedef struct {
	char id[128];
	int state;  // 1 asked, 2 ready, 3 failed
	Picture pic;
} Icon;
static Icon s_icons[48];

// ---- helpers -------------------------------------------------------------------

static bool in(touchPosition t, float x, float y, float w, float h) {
	return t.px >= x && t.px < x + w && t.py >= y && t.py < y + h;
}

static void copy(char *dst, size_t size, const char *src) { snprintf(dst, size, "%s", src ? src : ""); }

static u32 category_color(const char *cat) {
	if (!strcmp(cat, "media")) return C_GREEN;
	if (!strcmp(cat, "system")) return C_TEAL;
	if (!strcmp(cat, "windows")) return C_YELLOW;
	if (!strcmp(cat, "browser")) return C_BLUE;
	if (!strcmp(cat, "editing")) return C_ORANGE;
	return C_PURPLE;
}

static Icon *icon_for(const char *bundleId) {
	Icon *free_slot = NULL;
	for (int i = 0; i < 48; i++) {
		if (s_icons[i].state && !strcmp(s_icons[i].id, bundleId)) return &s_icons[i];
		if (!s_icons[i].state && !free_slot) free_slot = &s_icons[i];
	}
	if (!free_slot) return NULL;
	copy(free_slot->id, sizeof free_slot->id, bundleId);
	free_slot->state = 1;
	icon_request(bundleId);
	return free_slot;
}

static void icons_collect(void) {
	char id[128];
	u8 *px;
	int w, h;
	while (icon_take(id, sizeof id, &px, &w, &h)) {
		for (int i = 0; i < 48; i++) {
			if (s_icons[i].state != 1 || strcmp(s_icons[i].id, id)) continue;
			s_icons[i].state = picture_load(&s_icons[i].pic, px, w, h) ? 2 : 3;
		}
		free(px);
	}
}

static bool ask_text(const char *hint, const char *initial, SwkbdType type, char *out, size_t size) {
	SwkbdState kb;
	swkbdInit(&kb, type, 2, size - 1);
	swkbdSetHintText(&kb, hint);
	if (initial) swkbdSetInitialText(&kb, initial);
	swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
	if (type == SWKBD_TYPE_NUMPAD) swkbdSetNumpadKeys(&kb, '.', ':');
	return swkbdInputText(&kb, out, size) == SWKBD_BUTTON_CONFIRM;
}

static PadKey *edit_key(void) { return &g_pads.pages[s_page].keys[s_editSlot]; }

// ---- lists -----------------------------------------------------------------------

static void list_open(ListPurpose purpose, const char *title) {
	free(s_list.items);
	memset(&s_list, 0, sizeof s_list);
	s_list.purpose = purpose;
	copy(s_list.title, sizeof s_list.title, title);
	s_screen = SCR_LIST;
}

static Item *list_add(const char *label, const char *sub, const char *value, u32 color, int id) {
	s_list.items = realloc(s_list.items, (s_list.count + 1) * sizeof(Item));
	Item *it = &s_list.items[s_list.count++];
	memset(it, 0, sizeof *it);
	copy(it->label, sizeof it->label, label);
	copy(it->sub, sizeof it->sub, sub);
	copy(it->value, sizeof it->value, value);
	it->color = color;
	it->id = id;
	return it;
}

static float list_max_scroll(void) {
	float h = s_list.count * ROW_H - (SCREEN_H - LIST_TOP);
	return h > 0 ? h : 0;
}

static void list_clamp(void) {
	if (s_list.scroll < 0) s_list.scroll = 0;
	if (s_list.scroll > list_max_scroll()) s_list.scroll = list_max_scroll();
}

static void list_show_sel(void) {
	float top = s_list.sel * ROW_H, bottom = top + ROW_H, view = SCREEN_H - LIST_TOP;
	if (top < s_list.scroll) s_list.scroll = top;
	if (bottom > s_list.scroll + view) s_list.scroll = bottom - view;
	list_clamp();
}

// The editor's first question.
static void open_kind_menu(void) {
	PadKey *k = edit_key();
	list_open(LIST_KIND, k->kind ? k->label : "New key");
	list_add("Mac action", "Media, windows, browser, editing", "", C_GREEN, KEY_ACTION);
	list_add("Open an app", "Any app on your Mac", "", C_BLUE, KEY_APP);
	list_add("Open a website", "In your Mac's browser", "", C_TEAL, KEY_WEB);
	list_add("Keyboard shortcut", "Like Cmd + Shift + T", "", C_ORANGE, KEY_KEYS);
	list_add("Run a Shortcut", "From the Shortcuts app", "", C_PURPLE, KEY_SHORTCUT);
	if (k->kind) {
		list_add("Rename this key", k->label, "", C_TEXT2, 100);
		list_add("Remove this key", "", "", C_RED, 101);
	}
}

static void open_catalog(ListPurpose purpose, CatalogKind kind, const char *title) {
	list_open(purpose, title);
	s_list.loading = true;
	catalog_start(&s_catalog, kind);
}


static void open_menu(void) {
	list_open(LIST_MENU, "Menu");
	list_add(s_editing ? "Stop editing keys" : "Edit keys", "Or hold any key to change it", "edit", C_ORANGE, 0);
	if (g_pads.count < MAX_PAGES) list_add("Add a page", "", "addpage", C_BLUE, 1);
	list_add("Rename this page", g_pads.pages[s_page].name, "renamepage", C_TEXT2, 2);
	if (g_pads.count > 1) list_add("Remove this page", g_pads.pages[s_page].name, "removepage", C_RED, 3);
	list_add(g_config.clock24 ? "Use 12-hour clock" : "Use 24-hour clock", "", "clock", C_TEXT2, 4);
	list_add("Forget this Mac", g_config.macName, "forget", C_RED, 5);
	list_add("Quit", "", "quit", C_TEXT2, 6);
}

static const char *s_keyNames[][2] = {
	{"return", "Return"}, {"space", "Space"}, {"tab", "Tab"}, {"delete", "Delete"}, {"escape", "Escape"},
	{"left", "Left arrow"}, {"right", "Right arrow"}, {"up", "Up arrow"}, {"down", "Down arrow"},
	{"home", "Home"}, {"end", "End"}, {"pageup", "Page Up"}, {"pagedown", "Page Down"},
	{"forwarddelete", "Forward Delete"},
	{"-", "-"}, {"=", "="}, {"[", "["}, {"]", "]"}, {"\\", "\\"}, {";", ";"}, {"'", "'"}, {",", ","},
	{".", "."}, {"/", "/"}, {"`", "`"},
};

static void open_keyname_list(void) {
	list_open(LIST_KEYNAME, "Which key?");
	char v[4], l[4];
	for (char c = 'a'; c <= 'z'; c++) {
		snprintf(v, sizeof v, "%c", c);
		snprintf(l, sizeof l, "%c", c - 32);
		list_add(l, "", v, C_TEXT, 0);
	}
	for (char c = '0'; c <= '9'; c++) {
		snprintf(v, sizeof v, "%c", c);
		list_add(v, "", v, C_TEXT, 0);
	}
	for (size_t i = 0; i < sizeof s_keyNames / sizeof *s_keyNames; i++) list_add(s_keyNames[i][1], "", s_keyNames[i][0], C_TEXT, 0);
	for (int f = 1; f <= 12; f++) {
		char fv[8], fl[8];
		snprintf(fv, sizeof fv, "f%d", f);
		snprintf(fl, sizeof fl, "F%d", f);
		list_add(fl, "", fv, C_TEXT, 0);
	}
}

static void set_key(KeyKind kind, const char *label, const char *ref, const char *category) {
	PadKey *k = edit_key();
	k->kind = kind;
	copy(k->label, sizeof k->label, label);
	copy(k->ref, sizeof k->ref, ref);
	copy(k->category, sizeof k->category, category);
	pads_save();
	s_screen = SCR_MAIN;
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

static void list_picked(int i) {
	Item *it = &s_list.items[i];
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
			set_key(KEY_WEB, label, url, "");
			break;
		}
		case KEY_KEYS:
			memset(s_mods, 0, sizeof s_mods);
			s_keyName[0] = s_keyLabel[0] = 0;
			s_screen = SCR_KEYS;
			break;
		case 100: {
			char label[48];
			if (ask_text("Name for this key", edit_key()->label, SWKBD_TYPE_WESTERN, label, sizeof label)) {
				copy(edit_key()->label, sizeof edit_key()->label, label);
				pads_save();
			}
			s_screen = SCR_MAIN;
			break;
		}
		case 101:
			memset(edit_key(), 0, sizeof(PadKey));
			pads_save();
			s_screen = SCR_MAIN;
			break;
		}
		break;
	case LIST_ACTIONS: set_key(KEY_ACTION, it->label, it->value, it->sub); break;
	case LIST_APPS: set_key(KEY_APP, it->label, it->value, ""); break;
	case LIST_SHORTCUTS: set_key(KEY_SHORTCUT, it->label, it->value, ""); break;
	case LIST_KEYNAME:
		copy(s_keyName, sizeof s_keyName, it->value);
		copy(s_keyLabel, sizeof s_keyLabel, it->label);
		s_screen = SCR_KEYS;
		break;
	case LIST_MENU: {
		const char *id = it->value;
		s_screen = SCR_MAIN;
		if (!strcmp(id, "edit")) {
			s_editing = !s_editing;
		} else if (!strcmp(id, "addpage")) {
			PadPage *p = &g_pads.pages[g_pads.count];
			memset(p, 0, sizeof *p);
			snprintf(p->name, sizeof p->name, "Page %d", g_pads.count + 1);
			s_page = g_pads.count++;
			s_editing = true;
			pads_save();
		} else if (!strcmp(id, "renamepage")) {
			char name[32];
			if (ask_text("Page name", g_pads.pages[s_page].name, SWKBD_TYPE_WESTERN, name, sizeof name)) {
				copy(g_pads.pages[s_page].name, sizeof g_pads.pages[s_page].name, name);
				pads_save();
			}
		} else if (!strcmp(id, "removepage")) {
			memmove(&g_pads.pages[s_page], &g_pads.pages[s_page + 1], (g_pads.count - s_page - 1) * sizeof(PadPage));
			g_pads.count--;
			if (s_page >= g_pads.count) s_page = g_pads.count - 1;
			pads_save();
		} else if (!strcmp(id, "clock")) {
			g_config.clock24 = !g_config.clock24;
			config_save();
		} else if (!strcmp(id, "forget")) {
			macapad_stop();
			g_config.token[0] = 0;
			config_save();
			start_find(NULL);
		} else if (!strcmp(id, "quit")) {
			s_quit = true;
		}
		break;
	}
	}
}

static void list_back(void) {
	if (s_list.purpose == LIST_KEYNAME) {
		s_screen = SCR_KEYS;
	} else if (s_list.purpose == LIST_ACTIONS || s_list.purpose == LIST_APPS || s_list.purpose == LIST_SHORTCUTS) {
		catalog_finish(&s_catalog);
		open_kind_menu();
	} else {
		s_screen = SCR_MAIN;
	}
}

static void list_update(u32 down, u32 held, touchPosition touch) {
	if (s_list.loading && s_catalog.done) {
		s_list.loading = false;
		if (!s_catalog.ok) copy(s_list.empty, sizeof s_list.empty, s_catalog.error);
		for (int i = 0; i < s_catalog.count; i++) {
			CatalogItem *c = &s_catalog.items[i];
			u32 color = s_list.purpose == LIST_ACTIONS ? category_color(c->sub) : s_list.purpose == LIST_APPS ? C_BLUE : C_PURPLE;
			list_add(c->label, c->sub, c->value, color, 0);
		}
		if (s_catalog.ok && s_catalog.count == 0)
			copy(s_list.empty, sizeof s_list.empty, s_list.purpose == LIST_SHORTCUTS ? "No shortcuts in the Shortcuts app yet." : "Nothing here.");
		catalog_finish(&s_catalog);
	}

	if (down & KEY_B) { list_back(); return; }
	if (down & (KEY_DOWN | KEY_UP | KEY_L | KEY_R) && s_list.count) {
		int step = (down & KEY_DOWN) ? 1 : (down & KEY_UP) ? -1 : (down & KEY_R) ? 6 : -6;
		s_list.sel += step;
		if (s_list.sel < 0) s_list.sel = 0;
		if (s_list.sel >= s_list.count) s_list.sel = s_list.count - 1;
		list_show_sel();
	}
	if ((down & KEY_A) && s_list.count) { list_picked(s_list.sel); return; }

	circlePosition cp;
	hidCircleRead(&cp);
	if (abs(cp.dy) > 20) {
		s_list.scroll -= cp.dy / 12.0f;
		list_clamp();
	}

	// touch: drag scrolls, a tap picks
	if (down & KEY_TOUCH) {
		if (in(touch, 0, 0, 90, 32)) { list_back(); return; }
		s_scrollStart = s_list.scroll;
	}
	if ((held & KEY_TOUCH) && s_touching && s_touchStart.py >= LIST_TOP) {
		s_list.scroll = s_scrollStart - (touch.py - s_touchStart.py);
		list_clamp();
	}
}

static void list_tap(touchPosition t) {
	if (t.py < LIST_TOP || s_list.loading) return;
	int i = (int)((t.py - LIST_TOP + s_list.scroll) / ROW_H);
	if (i >= 0 && i < s_list.count) {
		s_list.sel = i;
		list_picked(i);
	}
}

static void header(const char *title, const char *back) {
	C2D_DrawRectSolid(0, 0, 0.5f, BOTTOM_W, 30, C_BG);
	C2D_DrawRectSolid(0, 30, 0.5f, BOTTOM_W, 1, C_LINE);
	if (back) text(10, 7, 0.5f, C_BLUE, 0, back);
	text_fit(BOTTOM_W / 2, 7, 0.5f, C_TEXT, C2D_AlignCenter, 170, title);
}

static void list_draw(void) {
	for (int i = 0; i < s_list.count; i++) {
		float y = LIST_TOP + i * ROW_H - s_list.scroll;
		if (y + ROW_H < LIST_TOP || y > SCREEN_H) continue;
		Item *it = &s_list.items[i];
		if (i == s_list.sel) rrect(4, y + 1, BOTTOM_W - 8, ROW_H - 2, 8, C_CARD_HI);
		disc(18, y + ROW_H / 2, 5, it->color);
		bool twoLine = it->sub[0] && s_list.purpose != LIST_ACTIONS;
		text_fit(32, y + (twoLine ? 3 : 9), 0.52f, it->color == C_RED ? C_RED : C_TEXT, 0, twoLine ? 270 : 200, it->label);
		if (twoLine) text_fit(32, y + 19, 0.4f, C_TEXT2, 0, 270, it->sub);
		else if (it->sub[0]) text(BOTTOM_W - 14, y + 11, 0.42f, C_TEXT3, C2D_AlignRight, it->sub);
		if (i < s_list.count - 1) C2D_DrawRectSolid(32, y + ROW_H - 1, 0.5f, BOTTOM_W - 40, 1, C_LINE);
	}
	if (s_list.loading) text(BOTTOM_W / 2, 110, 0.5f, C_TEXT2, C2D_AlignCenter, "Asking your Mac\xE2\x80\xA6");
	else if (!s_list.count) text(BOTTOM_W / 2, 110, 0.48f, C_TEXT2, C2D_AlignCenter, s_list.empty);
	bool back = s_list.purpose != LIST_MENU && s_list.purpose != LIST_KIND;
	header(s_list.title, back ? "\xE2\x80\xB9 Back" : "\xE2\x80\xB9 Done");
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

static void keys_tap(touchPosition t) {
	for (int i = 0; i < 4; i++)
		if (in(t, 8 + i * (MOD_W + 4), MOD_Y, MOD_W, 44)) s_mods[i] = !s_mods[i];
	if (in(t, 8, 124, 304, 44)) open_keyname_list();
	if (in(t, 0, 0, 90, 32)) open_kind_menu();
	if (s_keyName[0] && in(t, 8, 184, 304, 44)) {
		char keys[64], label[48];
		keys_combo(keys, sizeof keys, label, sizeof label);
		set_key(KEY_KEYS, label, keys, "");
	}
}

static void keys_draw(void) {
	header("Keyboard shortcut", "\xE2\x80\xB9 Back");
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

#define GRID_X 6
#define GRID_Y 34
#define GAP 6
#define KEY_W ((BOTTOM_W - 2 * GRID_X - (COLS - 1) * GAP) / (float)COLS)
#define KEY_H ((SCREEN_H - GRID_Y - GRID_X - (ROWS - 1) * GAP) / (float)ROWS)

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

static Glyph action_glyph(const char *id, bool *found) {
	*found = true;
	if (!strcmp(id, "play-pause")) return s_st.hasTrack && s_st.playing ? GLYPH_PAUSE : GLYPH_PLAY;
	if (!strcmp(id, "next-track")) return GLYPH_NEXT;
	if (!strcmp(id, "previous-track")) return GLYPH_PREV;
	if (!strcmp(id, "volume-up")) return GLYPH_VOL_UP;
	if (!strcmp(id, "volume-down")) return GLYPH_VOL_DOWN;
	if (!strcmp(id, "mute")) return GLYPH_MUTE;
	*found = false;
	return GLYPH_BOLT;
}

static void key_draw(int slot) {
	PadKey *k = &g_pads.pages[s_page].keys[slot];
	float x = GRID_X + (slot % COLS) * (KEY_W + GAP), y = GRID_Y + (slot / COLS) * (KEY_H + GAP);
	float cx = x + KEY_W / 2, iconY = y + 23;
	bool flash = slot == s_flashSlot && osGetTime() < s_flashUntil;

	if (k->kind == KEY_NONE) {
		rrect(x, y, KEY_W, KEY_H, 12, C2D_Color32(0x1C, 0x1C, 0x1E, 0x80));
		glyph(GLYPH_PLUS, cx, y + KEY_H / 2, 14, s_editing ? C_ORANGE : C_TEXT3);
		return;
	}
	rrect(x, y, KEY_W, KEY_H, 12, flash ? C_CARD_HI : C_CARD);
	switch (k->kind) {
	case KEY_ACTION: {
		bool media;
		Glyph g = action_glyph(k->ref, &media);
		glyph(g, cx, iconY, media ? 20 : 18, category_color(k->category));
		break;
	}
	case KEY_APP: {
		Icon *icon = icon_for(k->ref);
		if (icon && icon->state == 2) picture_draw(&icon->pic, cx - 16, iconY - 16, 32, 32);
		else rrect(cx - 14, iconY - 14, 28, 28, 7, C_CARD_HI);
		break;
	}
	case KEY_WEB: glyph(GLYPH_GLOBE, cx, iconY, 22, C_TEAL); break;
	case KEY_KEYS: glyph(GLYPH_KEYS, cx, iconY, 24, C_ORANGE); break;
	case KEY_SHORTCUT:
		rrect(cx - 12, iconY - 12, 18, 18, 5, C_PINK);
		rrect(cx - 6, iconY - 6, 18, 18, 5, C_PURPLE);
		break;
	default: break;
	}
	text_fit(cx, y + KEY_H - 21, 0.4f, C_TEXT, C2D_AlignCenter, KEY_W - 8, k->label);
	if (s_editing) disc(x + KEY_W - 9, y + 9, 4, C_ORANGE);
	if (s_st.off) rrect(x, y, KEY_W, KEY_H, 12, C2D_Color32(0, 0, 0, 0x90));
}

static void pad_draw(void) {
	PadPage *p = &g_pads.pages[s_page];
	if (s_editing) {
		rrect(4, 3, BOTTOM_W - 8, 25, 8, C_ORANGE);
		text(BOTTOM_W / 2, 7, 0.46f, C_BG, C2D_AlignCenter, "Tap a key to change it. SELECT when done.");
	} else {
		if (g_pads.count > 1) {
			rrect(6, 6, 26, 18, 6, C_CARD);
			text(19, 7, 0.42f, C_TEXT2, C2D_AlignCenter, "L");
			rrect(BOTTOM_W - 32, 6, 26, 18, 6, C_CARD);
			text(BOTTOM_W - 19, 7, 0.42f, C_TEXT2, C2D_AlignCenter, "R");
		}
		text_fit(BOTTOM_W / 2, 4, 0.48f, C_TEXT, C2D_AlignCenter, 200, p->name);
		float dotsW = g_pads.count * 9;
		for (int i = 0; i < g_pads.count && g_pads.count > 1; i++)
			disc(BOTTOM_W / 2 - dotsW / 2 + i * 9 + 4, 25, 2.5f, i == s_page ? C_TEXT : C_TEXT3);
	}
	for (int s = 0; s < SLOTS; s++) key_draw(s);
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
	if (g_pads.count > 1) {
		if (down & KEY_L) s_page = (s_page + g_pads.count - 1) % g_pads.count;
		if (down & KEY_R) s_page = (s_page + 1) % g_pads.count;
	}
	if (down & KEY_X) press_action("play-pause");
	if (down & KEY_DLEFT) press_action("previous-track");
	if (down & KEY_DRIGHT) press_action("next-track");
	if (down & KEY_DUP) press_action("volume-up");
	if (down & KEY_DDOWN) press_action("volume-down");
	if (down & KEY_Y) press_action("mute");
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
	time_t now;
	if (s_st.have && s_st.macTime) now = (s_st.macTime + (s64)(osGetTime() - s_st.receivedAt)) / 1000 + s_st.utcOffset;
	else now = time(NULL);  // the 3DS clock is already local time
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
	u32 dot = !s_st.have ? C_TEXT3 : s_st.online ? C_GREEN : C_RED;
	disc(19, HEAD_LINE3 + 7, 3, dot);
	char line[160];
	if (s_st.have && !s_st.online) {
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

static void draw_now_playing(void) {
	rrect(12, CARD_Y, TOP_W - 24, CARD_H, 14, C_CARD);
	if (s_st.outdated && !s_st.have) {
		text(TOP_W / 2, CARD_Y + 26, 0.55f, C_ORANGE, C2D_AlignCenter, "Update Macapad on your Mac");
		text(TOP_W / 2, CARD_Y + 50, 0.45f, C_TEXT2, C2D_AlignCenter, "Keys work, but it can't show the song or stats.");
		return;
	}
	if (!s_st.have) {
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
	text_fit(tx, CARD_Y + 8, 0.62f, C_TEXT, 0, tw - 20, s_st.title);
	text_fit(tx, CARD_Y + 30, 0.5f, C_TEXT2, 0, tw, s_st.artist);
	char sub[160];
	snprintf(sub, sizeof sub, "%s%s%s", s_st.album, s_st.album[0] ? " \xC2\xB7 " : "", s_st.app);
	text_fit(tx, CARD_Y + 48, 0.42f, C_TEXT3, 0, tw, sub);
	glyph(s_st.playing ? GLYPH_PAUSE : GLYPH_PLAY, TOP_W - 30, CARD_Y + 18, 12, C_TEXT2);

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
	macapad_start();
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
	if (!s_scan.running && in(t, 8, 200, 148, 34)) start_find(NULL);
	if (in(t, 164, 200, 148, 34)) type_address();
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
	header(s_scan.running ? "Looking\xE2\x80\xA6" : s_scan.count ? "Pick your Mac" : "No Mac found", NULL);
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
		rrect(8, 200, 148, 34, 10, C_CARD);
		text(82, 209, 0.46f, C_BLUE, C2D_AlignCenter, "Look again");
	}
	rrect(164, 200, 148, 34, 10, C_CARD);
	text(238, 209, 0.46f, C_BLUE, C2D_AlignCenter, "Type address");
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
			FoundMac mac = s_pair.mac;
			start_pair(&mac);  // "Try again"
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
	case PAIR_FAILED:
		text_fit(TOP_W / 2, 100, 0.55f, C_ORANGE, C2D_AlignCenter, 370, s_pair.error);
		break;
	case PAIR_APPROVED:
		break;
	}
}

static void pair_draw_bottom(void) {
	PairState st = s_pair.state;
	bool finished = st == PAIR_DENIED || st == PAIR_EXPIRED || st == PAIR_FAILED;
	header("Pair", NULL);
	text(BOTTOM_W / 2, 90, 0.48f, C_TEXT2, C2D_AlignCenter, finished ? "Want to try again?" : "Look at your Mac's screen.");
	rrect(8, 196, 304, 38, 10, C_CARD);
	text(BOTTOM_W / 2, 206, 0.5f, finished ? C_BLUE : C_RED, C2D_AlignCenter, finished ? "Try again" : "Cancel");
}

// ---- main --------------------------------------------------------------------------

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
	if (g_config.token[0]) start_main();
	else start_find(NULL);

	while (aptMainLoop() && !s_quit) {
		hidScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld(), up = hidKeysUp();
		touchPosition touch;
		hidTouchRead(&touch);

		// turn raw touch into taps, drags and long presses
		bool tapped = false;
		touchPosition tap = s_touchStart;
		if (down & KEY_TOUCH) {
			s_touching = true;
			s_dragging = s_longFired = false;
			s_touchStart = touch;
			s_touchAt = osGetTime();
		}
		if ((held & KEY_TOUCH) && s_touching) {
			s_touchNow = touch;
			if (abs(touch.px - s_touchStart.px) > 8 || abs(touch.py - s_touchStart.py) > 8) s_dragging = true;
		}
		if ((up & KEY_TOUCH) && s_touching) {
			s_touching = false;
			tapped = !s_dragging && !s_longFired;
		}

		if (s_screen == SCR_MAIN) status_get(&s_st);
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
		case SCR_PAIR:
			pair_update(down, tap, tapped);
			break;
		case SCR_MAIN:
			main_update(down);
			if (s_screen != SCR_MAIN) break;
			// hold a key to change it
			if (s_touching && !s_dragging && !s_longFired && osGetTime() - s_touchAt > LONG_PRESS_MS) {
				int slot = slot_at(s_touchStart);
				s_longFired = true;
				if (slot >= 0) edit_slot(slot);
			} else if (tapped) {
				pad_tap(slot_at(tap));
			}
			break;
		case SCR_LIST:
			list_update(down, held, touch);
			if (tapped && s_screen == SCR_LIST && !in(tap, 0, 0, 90, 32)) list_tap(tap);
			break;
		case SCR_KEYS:
			if (down & KEY_B) open_kind_menu();
			else if (tapped) keys_tap(tap);
			break;
		}

		gfx_frame();
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C2D_TargetClear(top, C_BG);
		C2D_SceneBegin(top);
		if (s_screen == SCR_FIND) find_draw_top();
		else if (s_screen == SCR_PAIR) pair_draw_top();
		else draw_dashboard();

		C2D_TargetClear(bottom, C_BG);
		C2D_SceneBegin(bottom);
		switch (s_screen) {
		case SCR_FIND: find_draw_bottom(); break;
		case SCR_PAIR: pair_draw_bottom(); break;
		case SCR_MAIN: pad_draw(); break;
		case SCR_LIST: list_draw(); break;
		case SCR_KEYS: keys_draw(); break;
		}
		C3D_FrameEnd(0);
	}

	scan_finish(&s_scan);
	pair_finish(&s_pair);
	catalog_finish(&s_catalog);
	macapad_stop();
	picture_free(&s_art);
	for (int i = 0; i < 48; i++) picture_free(&s_icons[i].pic);
	socExit();
	gfx_exit();
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	return 0;
}
