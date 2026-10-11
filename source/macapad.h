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

// ---- live status (filled by the status thread) ------------------------------

typedef struct {
	bool have;           // at least one answer since connecting
	bool online;         // the last poll worked
	bool forgotten;      // the Mac rejected our token: pair again
	bool outdated;       // the Mac's Macapad has no /api/status (older than 0.4)
	u64 receivedAt;      // osGetTime() when it arrived
	s64 macTime;         // ms since 1970 on the Mac's clock
	int utcOffset;       // seconds
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

// The last press that went wrong (or the last notice); empty when there is none.
// Each message stays for a few seconds.
bool toast_get(char *out, size_t size, bool *isError);
void toast_set(const char *msg, bool isError);

// ---- threads ----------------------------------------------------------------

void macapad_init(void);   // once, before anything else
void macapad_start(void);  // status + press threads; call once paired
void macapad_stop(void);

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

typedef struct {
	CatalogKind kind;
	volatile bool running, done;
	bool ok;
	char error[96];
	int count;
	CatalogItem *items;
	Thread thread;
} Catalog;

// Each catalog fetches on its own thread, so several can load at once.
void catalog_start(Catalog *cat, CatalogKind kind);
void catalog_finish(Catalog *cat);  // joins and frees the items; fine on one never started

// ---- app icons ---------------------------------------------------------------

// Queues an app icon fetch (from the SD card cache, else the Mac). Results come back via icon_take.
void icon_request(const char *bundleId);
bool icon_take(char *bundleId, size_t size, u8 **pixels, int *w, int *h);

// Decodes a PNG into RGBA8 pixels (malloc'd).
u8 *png_decode(const void *data, size_t len, int *w, int *h);
