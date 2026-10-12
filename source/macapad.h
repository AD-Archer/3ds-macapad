#pragma once
#include <3ds.h>
#include <stdbool.h>

#define DATA_DIR "sdmc:/3ds/macapad"
#define COLS 4
#define ROWS 3
#define SLOTS (COLS * ROWS)
#define MAX_PAGES 8

// ---- saved state ------------------------------------------------------------

typedef struct {
	char url[96];      // http://host:port
	char token[128];
	char macId[64];
	char macName[64];
	bool clock24;
} Config;

typedef enum { KEY_NONE, KEY_ACTION, KEY_APP, KEY_WEB, KEY_KEYS, KEY_SHORTCUT } KeyKind;

typedef struct {
	KeyKind kind;
	char label[48];
	char ref[192];      // action id, bundle id, url, key combo or shortcut name
	char category[16];  // actions only: media, system, windows, browser, editing, custom
	char icon[24];      // actions only: the Material Symbols name the Mac suggests, for custom actions
	char glyph[24];     // the icon picked in "How it looks" (a name from the glyph list); empty = automatic
	char color[12];     // the color picked there: blue, green, orange, red, purple, yellow, teal, pink or gray; empty = automatic
} PadKey;

typedef struct {
	char name[32];
	PadKey keys[SLOTS];
} PadPage;

typedef struct {
	int count;
	PadPage pages[MAX_PAGES];
} Pads;

extern Config g_config;
extern Pads g_pads;

void config_load(void);
void config_save(void);
void pads_load(void);
void pads_save(void);
const char *kind_name(KeyKind kind);

// ---- the 3DS as a controller (saved too) ------------------------------------------

// Every button and stick that can be given a job, in the order they're listed.
typedef enum {
	CTL_A, CTL_B, CTL_X, CTL_Y, CTL_L, CTL_R, CTL_ZL, CTL_ZR,
	CTL_UP, CTL_DOWN, CTL_LEFT, CTL_RIGHT, CTL_START, CTL_SELECT,
	CTL_CIRCLE, CTL_CSTICK,
	CTL_COUNT
} Control;

#define PRESETS 3  // Mouse & keys, Retro games, PC games

// What a control does. A button: a key or keys held together ("x", "cmd+tab"), a mouse button
// ("click:left", "click:right", "click:middle") or "none". A stick: "pointer", "scroll", "arrows",
// "wasd" or "none".
typedef struct {
	int preset;
	char changes[PRESETS][CTL_COUNT][24];  // what's been changed in each preset; "" = as the preset has it
} Controller;

extern Controller g_controller;

void controller_load(void);
void controller_save(void);
const char *preset_name(int preset);
const char *preset_about(int preset);
// The preset's own job for c, and what it's for in a game ("Jump"), or "".
const char *preset_binding(int preset, Control c);
const char *preset_note(int preset, Control c);
// What c does now: the change, else the preset's.
const char *control_binding(Control c);
// Sets what c does in the current preset ("" = back to the preset's).
void control_set(Control c, const char *binding);
int preset_changes(int preset);

// ---- live status (filled by the status thread) ------------------------------

typedef struct {
	bool have;           // something to show: the snapshot saved last time, or an answer from the Mac
	bool live;           // at least one answer since connecting (the song and stats are current)
	bool online;         // the last poll worked
	bool forgotten;      // the Mac rejected our token: pair again
	bool outdated;       // the Mac's Macapad has no /api/status (older than 0.4)
	u64 receivedAt;      // osGetTime() when it arrived
	s64 macTime;         // ms since 1970 on the Mac's clock
	int utcOffset;       // seconds
	// Seconds the Mac's local clock is ahead of this 3DS's clock. Saved on the SD card, so the
	// clock shows the Mac's time from the first frame, and keeps it while the Mac is away.
	int clockDelta;
	char macName[64];
	char frontApp[64];

	bool hasTrack, playing;
	char app[16], title[128], artist[128], album[128];
	int duration, position;  // ms
	char artworkId[24];

	bool hasWeather;
	int temp, high, low;
	char unit[4];        // "F" or "C"
	char symbol[16];     // clear, partly-cloudy, cloudy, fog, drizzle, rain, snow, storm
	char condition[32];
	bool isDay;
	char place[48];

	double cpu;
	double memUsed, memTotal;     // bytes
	int battery;                  // -1 = no battery
	bool charging, pluggedIn;
	double diskFree, diskTotal;
	int uptime;
	char thermal[12];

	bool off;
	char offMessage[160];

	// Macapad 1.0's switches; hasSwitches is false for an older Mac
	bool hasSwitches;
	bool keepAwake, jiggler;
	s64 keepAwakeUntil, jigglerUntil;  // ms since 1970 when it turns itself off; 0 = until turned off
} Status;

// Copies the latest status under the lock.
void status_get(Status *out);

// Artwork the status thread fetched: RGBA pixels the main thread turns into a texture.
// Returns true once per new image; the caller frees *pixels. *pixels == NULL means "no artwork".
bool artwork_take(u8 **pixels, int *w, int *h);

// ---- pressing keys ----------------------------------------------------------

void press_key(const PadKey *key);
// Steps for the physical buttons: a built-in action id like "play-pause".
void press_action(const char *actionId);
// Types text on the Mac, as if typed on its keyboard.
void press_text(const char *text);
// Presses one key combo, like "cmd+left".
void press_combo(const char *keys);
// Turns one of the Mac's switches on or off: "keepAwake" or "jiggler".
void press_switch(const char *type, bool on);

// The last press that went wrong (or the last notice); empty when there is none.
// Each message stays for a few seconds.
bool toast_get(char *out, size_t size, bool *isError);
void toast_set(const char *msg, bool isError);

// ---- threads ----------------------------------------------------------------

void macapad_init(void);   // once, before anything else
void macapad_start(void);  // status + press threads; call once paired
void macapad_stop(void);
// A worker thread, on the system core when the app has a share of it.
Thread macapad_spawn(ThreadFunc fn, void *arg);

// ---- finding and pairing (each runs on its own thread) ----------------------

typedef struct {
	char host[64];
	int port;
	char macId[64];
	char name[64];
	bool canPair;
} FoundMac;

typedef struct {
	volatile bool running, cancel;
	volatile int progress;  // 0..254
	int count;
	FoundMac macs[8];
} Scan;

void scan_start(Scan *scan);
void scan_finish(Scan *scan);  // joins the thread
// Checks one typed address; fills *mac. Blocking (short).
bool probe_mac(const char *host, int port, FoundMac *mac);

typedef enum { PAIR_STARTING, PAIR_WAITING, PAIR_APPROVED, PAIR_DENIED, PAIR_EXPIRED, PAIR_FAILED } PairState;

typedef struct {
	FoundMac mac;
	volatile bool running, cancel;
	volatile PairState state;
	char code[8];
	char error[160];  // one or two lines (split at a newline)

	// From the QR code the Mac shows: no code to check, the key pairs right away
	bool viaQr;
	char qrKey[80];
	int urlCount;
	char urls[4][64];  // "http://192.168.1.5:7317", best first
} Pairing;

void pair_start(Pairing *pair);
// Pairs with a "macapad://pair?..." link read from the Mac's QR code. False if it isn't one.
bool pair_qr_start(Pairing *pair, const char *link);
void pair_finish(Pairing *pair);

// ---- catalog for the key editor ----------------------------------------------

typedef struct {
	char label[64];
	char sub[32];
	char value[192];
	char icon[24];
} CatalogItem;

typedef enum { CATALOG_ACTIONS, CATALOG_APPS, CATALOG_SHORTCUTS } CatalogKind;

// The catalogs are cached on the SD card and kept fresh by a thread in the background: actions
// every 5 minutes, Shortcuts every 10, apps every 30 (and whenever asked). A list opens from the
// cache at once, so there's nothing to wait for after the first time.
typedef struct {
	CatalogKind kind;
	bool done;        // items can be shown (from the cache, or once the Mac answered)
	bool ok;          // done without an error
	char error[96];
	int count;
	CatalogItem *items;
	u32 generation;   // the version of the cache these came from
} Catalog;

// Fills cat from the cache when it has that kind (done right away); else asks the Mac.
void catalog_start(Catalog *cat, CatalogKind kind);
// Picks up a newer version from the cache, or the error when a first fetch failed.
// True when cat changed; call it every frame while the list is open.
bool catalog_sync(Catalog *cat);
void catalog_finish(Catalog *cat);  // frees the items; fine on one never started

// Asks the Mac again now, for one kind or all three (kind = -1).
void catalog_refresh(int kind);

typedef struct {
	bool loaded;      // there's something to show
	bool fetching;    // an answer is on its way
	bool failed;      // the last fetch didn't work; error says why
	s64 ageSec;       // seconds since the Mac last answered; -1 = never
	char error[96];
} CatalogInfo;
void catalog_info(CatalogKind kind, CatalogInfo *info);

// Removes the catalogs, the status snapshot and the icons from the SD card (forgetting a Mac).
void cache_clear(void);

// ---- app and website icons ---------------------------------------------------

// The id an icon is asked for and comes back under: an app's bundle id, or "web:<host>" for a
// website. Empty when that kind has no icon.
void icon_id(KeyKind kind, const char *ref, char *out, size_t size);
// Queues an icon fetch: from the SD card cache, else from the Mac (which also runs on its own
// now and then for icons older than a week). fresh skips the cache. Results come back via icon_take.
void icon_request(const char *id, bool fresh);
// *pixels is NULL when there's no icon; then *retry says the Mac couldn't be reached, so ask again in a while.
bool icon_take(char *id, size_t size, u8 **pixels, int *w, int *h, bool *retry);

// Decodes a PNG into RGBA8 pixels (malloc'd).
u8 *png_decode(const void *data, size_t len, int *w, int *h);
