#include "macapad.h"
#include "http.h"

#include <3ds.h>
#include <arpa/inet.h>

#include <jansson.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

Config g_config;
Pads g_pads;

#define STATUS_EVERY_MS 1500
#define TIMEOUT_MS 3000

static void copy(char *dst, size_t size, const char *src) {
	int n = snprintf(dst, size, "%s", src ? src : "");
	if (n < 0 || (size_t)n < size) return;
	// cut short: don't leave the start of a multi-byte character at the end (it wouldn't be UTF-8,
	// and the SD card caches are JSON, which refuses that)
	size_t end = size - 1, lead = end;
	while (lead > 0 && (dst[lead - 1] & 0xC0) == 0x80) lead--;
	if (lead > 0) lead--;
	unsigned char c = dst[lead];
	size_t need = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
	if (end - lead < need) dst[lead] = 0;
}

static const char *jstr(json_t *obj, const char *key) {
	const char *s = json_string_value(json_object_get(obj, key));
	return s ? s : "";
}

// ---- files -------------------------------------------------------------------

static void ensure_dirs(void) {
	mkdir("sdmc:/3ds", 0777);
	mkdir(DATA_DIR, 0777);
	mkdir(DATA_DIR "/icons", 0777);
	mkdir(DATA_DIR "/icons/web", 0777);
}

void config_load(void) {
	memset(&g_config, 0, sizeof g_config);
	json_t *root = json_load_file(DATA_DIR "/config.json", 0, NULL);
	if (!root) return;
	copy(g_config.url, sizeof g_config.url, jstr(root, "url"));
	copy(g_config.token, sizeof g_config.token, jstr(root, "token"));
	copy(g_config.macId, sizeof g_config.macId, jstr(root, "macId"));
	copy(g_config.macName, sizeof g_config.macName, jstr(root, "macName"));
	g_config.clock24 = json_is_true(json_object_get(root, "clock24"));
	json_decref(root);
}

void config_save(void) {
	ensure_dirs();
	json_t *root = json_pack("{s:s,s:s,s:s,s:s,s:b}",
		"url", g_config.url, "token", g_config.token, "macId", g_config.macId,
		"macName", g_config.macName, "clock24", g_config.clock24);
	json_dump_file(root, DATA_DIR "/config.json", JSON_INDENT(2));
	json_decref(root);
}

static const char *kinds[] = {"", "action", "app", "web", "keys", "shortcut"};

const char *kind_name(KeyKind kind) { return kinds[kind]; }

static KeyKind kind_from(const char *s) {
	for (int i = 1; i < 6; i++) if (strcmp(s, kinds[i]) == 0) return i;
	return KEY_NONE;
}

static void action_key(PadKey *k, const char *id, const char *label, const char *category) {
	k->kind = KEY_ACTION;
	copy(k->ref, sizeof k->ref, id);
	copy(k->label, sizeof k->label, label);
	copy(k->category, sizeof k->category, category);
}

// First run: a page of things everyone wants, so the pad isn't empty.
static void pads_default(void) {
	memset(&g_pads, 0, sizeof g_pads);
	g_pads.count = 2;
	PadPage *p = &g_pads.pages[0];
	copy(p->name, sizeof p->name, "Media");
	action_key(&p->keys[0], "previous-track", "Previous", "media");
	action_key(&p->keys[1], "play-pause", "Play/Pause", "media");
	action_key(&p->keys[2], "next-track", "Next", "media");
	action_key(&p->keys[3], "mute", "Mute", "media");
	action_key(&p->keys[4], "volume-down", "Volume -", "media");
	action_key(&p->keys[5], "volume-up", "Volume +", "media");
	action_key(&p->keys[6], "mission-control", "Mission Ctrl", "system");
	action_key(&p->keys[7], "spotlight", "Spotlight", "system");
	action_key(&p->keys[8], "screenshot-ui", "Screenshot", "system");
	action_key(&p->keys[9], "app-windows", "App Windows", "system");
	action_key(&p->keys[10], "emoji", "Emoji", "system");
	action_key(&p->keys[11], "lock-screen", "Lock", "system");

	p = &g_pads.pages[1];
	copy(p->name, sizeof p->name, "Browser");
	action_key(&p->keys[0], "previous-tab", "Prev Tab", "browser");
	action_key(&p->keys[1], "new-tab", "New Tab", "browser");
	action_key(&p->keys[2], "next-tab", "Next Tab", "browser");
	action_key(&p->keys[3], "reload", "Reload", "browser");
	action_key(&p->keys[4], "close-tab", "Close Tab", "browser");
	action_key(&p->keys[5], "reopen-tab", "Reopen Tab", "browser");
	action_key(&p->keys[6], "address-bar", "Address Bar", "browser");
	action_key(&p->keys[7], "fullscreen", "Full Screen", "windows");
}

void pads_load(void) {
	json_t *root = json_load_file(DATA_DIR "/pads.json", 0, NULL);
	json_t *pages = json_object_get(root, "pages");
	if (!json_is_array(pages) || json_array_size(pages) == 0) {
		json_decref(root);
		pads_default();
		return;
	}
	memset(&g_pads, 0, sizeof g_pads);
	size_t i;
	json_t *page;
	json_array_foreach(pages, i, page) {
		if (g_pads.count == MAX_PAGES) break;
		PadPage *p = &g_pads.pages[g_pads.count++];
		copy(p->name, sizeof p->name, jstr(page, "name"));
		json_t *keys = json_object_get(page, "keys");
		for (int s = 0; s < SLOTS; s++) {
			json_t *k = json_array_get(keys, s);
			if (!json_is_object(k)) continue;
			p->keys[s].kind = kind_from(jstr(k, "kind"));
			copy(p->keys[s].label, sizeof p->keys[s].label, jstr(k, "label"));
			copy(p->keys[s].ref, sizeof p->keys[s].ref, jstr(k, "ref"));
			copy(p->keys[s].category, sizeof p->keys[s].category, jstr(k, "category"));
			copy(p->keys[s].icon, sizeof p->keys[s].icon, jstr(k, "icon"));
		}
	}
	json_decref(root);
}

void pads_save(void) {
	ensure_dirs();
	json_t *pages = json_array();
	for (int i = 0; i < g_pads.count; i++) {
		PadPage *p = &g_pads.pages[i];
		json_t *keys = json_array();
		for (int s = 0; s < SLOTS; s++) {
			PadKey *k = &p->keys[s];
			if (k->kind == KEY_NONE) {
				json_array_append_new(keys, json_null());
				continue;
			}
			json_t *obj = json_pack("{s:s,s:s,s:s}", "kind", kinds[k->kind], "label", k->label, "ref", k->ref);
			if (k->category[0]) json_object_set_new(obj, "category", json_string(k->category));
			if (k->icon[0]) json_object_set_new(obj, "icon", json_string(k->icon));
			json_array_append_new(keys, obj);
		}
		json_array_append_new(pages, json_pack("{s:s,s:o}", "name", p->name, "keys", keys));
	}
	json_t *root = json_pack("{s:i,s:o}", "version", 1, "pages", pages);
	json_dump_file(root, DATA_DIR "/pads.json", JSON_INDENT(2));
	json_decref(root);
}

// ---- talking to the Mac ------------------------------------------------------

// conn may be NULL for a one-off connection.
static bool request(HttpConn *conn, const char *method, const char *path, const char *body, HttpResponse *res, int timeout) {
	char host[64];
	int port;
	if (!url_split(g_config.url, host, sizeof host, &port)) {
		memset(res, 0, sizeof *res);
		return false;
	}
	return http_request_on(conn, method, host, port, path, g_config.token, body, res, timeout);
}

// ---- status thread -----------------------------------------------------------

static LightLock s_lock;
static Status s_status;
static Thread s_statusThread, s_pressThread, s_iconThread;
static volatile bool s_quit;
static LightEvent s_pressEvent, s_iconEvent;
// The status and press threads each keep their connection to the Mac open between requests.
static HttpConn s_statusConn, s_pressConn;

static u8 *s_artPixels;
static int s_artW, s_artH;
static bool s_artFresh;

void status_get(Status *out) {
	LightLock_Lock(&s_lock);
	*out = s_status;
	LightLock_Unlock(&s_lock);
}

bool artwork_take(u8 **pixels, int *w, int *h) {
	LightLock_Lock(&s_lock);
	bool fresh = s_artFresh;
	if (fresh) {
		*pixels = s_artPixels;
		*w = s_artW;
		*h = s_artH;
		s_artPixels = NULL;
		s_artFresh = false;
	}
	LightLock_Unlock(&s_lock);
	return fresh;
}

static void set_artwork(u8 *pixels, int w, int h) {
	LightLock_Lock(&s_lock);
	free(s_artPixels);
	s_artPixels = pixels;
	s_artW = w;
	s_artH = h;
	s_artFresh = true;
	LightLock_Unlock(&s_lock);
}

// "2026-10-10T20:31:36Z" -> ms since 1970
static s64 parse_iso(const char *s) {
	int y, mo, d, h, mi, sec;
	if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) return 0;
	y -= mo <= 2;
	int era = (y >= 0 ? y : y - 399) / 400;
	int yoe = y - era * 400;
	int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	s64 days = (s64)era * 146097 + doe - 719468;
	return ((days * 86400) + h * 3600 + mi * 60 + sec) * 1000;
}

// The weather and stats objects, as the Mac sends them (and as the snapshot keeps them).
static void apply_weather(Status *st, json_t *wx) {
	st->temp = json_integer_value(json_object_get(wx, "temp"));
	st->high = json_integer_value(json_object_get(wx, "high"));
	st->low = json_integer_value(json_object_get(wx, "low"));
	copy(st->unit, sizeof st->unit, jstr(wx, "unit"));
	copy(st->symbol, sizeof st->symbol, jstr(wx, "symbol"));
	copy(st->condition, sizeof st->condition, jstr(wx, "condition"));
	st->isDay = json_is_true(json_object_get(wx, "isDay"));
	copy(st->place, sizeof st->place, jstr(wx, "place"));
}

static void apply_stats(Status *st, json_t *stats) {
	st->cpu = json_number_value(json_object_get(stats, "cpu"));
	st->memUsed = json_number_value(json_object_get(stats, "memoryUsed"));
	st->memTotal = json_number_value(json_object_get(stats, "memoryTotal"));
	json_t *bat = json_object_get(stats, "battery");
	st->battery = json_is_integer(bat) ? (int)json_integer_value(bat) : -1;
	st->charging = json_is_true(json_object_get(stats, "charging"));
	st->pluggedIn = json_is_true(json_object_get(stats, "pluggedIn"));
	st->diskFree = json_number_value(json_object_get(stats, "diskFree"));
	st->diskTotal = json_number_value(json_object_get(stats, "diskTotal"));
	st->uptime = json_integer_value(json_object_get(stats, "uptime"));
	copy(st->thermal, sizeof st->thermal, jstr(stats, "thermal"));
}

// ---- status snapshot -----------------------------------------------------------

// What the dashboard shows before the Mac answers, and while it's away: the clock's offset, the
// weather and the Mac's numbers from last time. Not the song: that has to be current.
#define SNAPSHOT_FILE DATA_DIR "/status.json"
#define SNAPSHOT_EVERY_MS (10 * 60 * 1000)
static u64 s_snapshotAt;  // osGetTime() of the last save; 0 = none yet this session

// Before the threads start, so no lock.
static void snapshot_load(void) {
	json_t *root = json_load_file(SNAPSHOT_FILE, 0, NULL);
	if (!root) return;
	if (strcmp(jstr(root, "macId"), g_config.macId) == 0) {
		Status *st = &s_status;
		st->have = true;
		st->utcOffset = json_integer_value(json_object_get(root, "utcOffset"));
		st->clockDelta = json_integer_value(json_object_get(root, "clockDelta"));
		copy(st->macName, sizeof st->macName, jstr(root, "macName"));
		json_t *wx = json_object_get(root, "weather");
		st->hasWeather = json_is_object(wx);
		if (st->hasWeather) apply_weather(st, wx);
		json_t *stats = json_object_get(root, "stats");
		if (json_is_object(stats)) apply_stats(st, stats);
	}
	json_decref(root);
}

static void snapshot_save(void) {
	Status st;
	status_get(&st);
	if (!st.live) return;
	json_t *root = json_pack("{s:s,s:I,s:i,s:i,s:s}", "macId", g_config.macId, "savedAt", (json_int_t)time(NULL),
		"utcOffset", st.utcOffset, "clockDelta", st.clockDelta, "macName", st.macName);
	if (!root) return;
	if (st.hasWeather) {
		json_object_set_new(root, "weather", json_pack("{s:i,s:i,s:i,s:s,s:s,s:s,s:b,s:s}",
			"temp", st.temp, "high", st.high, "low", st.low, "unit", st.unit, "symbol", st.symbol,
			"condition", st.condition, "isDay", st.isDay, "place", st.place));
	}
	json_t *stats = json_pack("{s:f,s:f,s:f,s:b,s:b,s:f,s:f,s:i,s:s}",
		"cpu", st.cpu, "memoryUsed", st.memUsed, "memoryTotal", st.memTotal, "charging", st.charging,
		"pluggedIn", st.pluggedIn, "diskFree", st.diskFree, "diskTotal", st.diskTotal, "uptime", st.uptime,
		"thermal", st.thermal);
	if (st.battery >= 0) json_object_set_new(stats, "battery", json_integer(st.battery));
	json_object_set_new(root, "stats", stats);
	ensure_dirs();
	json_dump_file(root, SNAPSHOT_FILE, JSON_COMPACT);
	json_decref(root);
	s_snapshotAt = osGetTime();
}

static void apply_status(json_t *root) {
	Status st;
	LightLock_Lock(&s_lock);
	st = s_status;
	LightLock_Unlock(&s_lock);

	st.have = st.online = st.live = true;
	st.receivedAt = osGetTime();
	st.macTime = parse_iso(jstr(root, "time"));
	st.utcOffset = json_integer_value(json_object_get(root, "utcOffset"));
	if (st.macTime) st.clockDelta = (int)(st.macTime / 1000 + st.utcOffset - (s64)time(NULL));
	copy(st.macName, sizeof st.macName, jstr(json_object_get(root, "mac"), "name"));
	copy(st.frontApp, sizeof st.frontApp, jstr(root, "frontApp"));

	json_t *np = json_object_get(root, "nowPlaying");
	st.hasTrack = json_is_object(np);
	if (st.hasTrack) {
		st.playing = json_is_true(json_object_get(np, "playing"));
		copy(st.app, sizeof st.app, jstr(np, "app"));
		copy(st.title, sizeof st.title, jstr(np, "title"));
		copy(st.artist, sizeof st.artist, jstr(np, "artist"));
		copy(st.album, sizeof st.album, jstr(np, "album"));
		st.duration = json_integer_value(json_object_get(np, "duration"));
		st.position = json_integer_value(json_object_get(np, "position"));
	}
	const char *art = st.hasTrack ? jstr(np, "artworkId") : "";
	bool artChanged = strcmp(art, st.artworkId) != 0;
	copy(st.artworkId, sizeof st.artworkId, art);

	json_t *wx = json_object_get(root, "weather");
	st.hasWeather = json_is_object(wx);
	if (st.hasWeather) apply_weather(&st, wx);
	apply_stats(&st, json_object_get(root, "stats"));

	json_t *off = json_object_get(root, "off");
	st.off = json_is_object(off);
	copy(st.offMessage, sizeof st.offMessage, st.off ? jstr(off, "message") : "");

	LightLock_Lock(&s_lock);
	s_status = st;
	LightLock_Unlock(&s_lock);

	if (!artChanged) return;
	if (!*art) {
		set_artwork(NULL, 0, 0);
		return;
	}
	HttpResponse res;
	if (request(NULL, "GET", "/api/status/artwork?size=128", NULL, &res, TIMEOUT_MS) && res.status == 200) {
		int w, h;
		u8 *px = png_decode(res.body, res.len, &w, &h);
		set_artwork(px, w, h);
	} else {
		set_artwork(NULL, 0, 0);
	}
	http_free(&res);
}

static void mark_offline(bool forgotten) {
	LightLock_Lock(&s_lock);
	s_status.online = false;
	s_status.forgotten = s_status.forgotten || forgotten;
	LightLock_Unlock(&s_lock);
}

// After a few misses the Mac may have a new address; look for it by id on the network.
static void rediscover(void) {
	Scan scan = {0};
	ScanHit hits[8];
	int n = net_scan(7317, hits, 8, &scan.progress, &s_quit);
	for (int i = 0; i < n && !s_quit; i++) {
		FoundMac mac;
		if (probe_mac(hits[i].host, hits[i].port, &mac) && strcmp(mac.macId, g_config.macId) == 0) {
			snprintf(g_config.url, sizeof g_config.url, "http://%s:%d", hits[i].host, hits[i].port);
			config_save();
			return;
		}
	}
}

static void status_thread(void *arg) {
	int misses = 0;
	while (!s_quit) {
		u64 started = osGetTime();
		HttpResponse res;
		if (request(&s_statusConn, "GET", "/api/status", NULL, &res, TIMEOUT_MS) && res.status == 200) {
			json_t *root = json_loadb(res.body, res.len, 0, NULL);
			if (root) apply_status(root);
			json_decref(root);
			misses = 0;
			if (!s_snapshotAt || osGetTime() - s_snapshotAt > SNAPSHOT_EVERY_MS) snapshot_save();
		} else if (res.status == 404) {
			// a Macapad from before /api/status: reachable, just can't say what it's doing
			LightLock_Lock(&s_lock);
			s_status.online = s_status.outdated = true;
			LightLock_Unlock(&s_lock);
			misses = 0;
		} else {
			mark_offline(res.status == 401);
			if (++misses == 4 && g_config.macId[0]) rediscover();
		}
		http_free(&res);
		u64 spent = osGetTime() - started;
		for (u64 waited = spent; waited < STATUS_EVERY_MS && !s_quit; waited += 50) svcSleepThread(50 * 1000000LL);
	}
}

// ---- toasts --------------------------------------------------------------------

static char s_toast[160];
static bool s_toastError;
static u64 s_toastAt;

void toast_set(const char *msg, bool isError) {
	LightLock_Lock(&s_lock);
	copy(s_toast, sizeof s_toast, msg);
	s_toastError = isError;
	s_toastAt = osGetTime();
	LightLock_Unlock(&s_lock);
}

bool toast_get(char *out, size_t size, bool *isError) {
	LightLock_Lock(&s_lock);
	bool show = s_toast[0] && osGetTime() - s_toastAt < 3500;
	if (show) {
		copy(out, size, s_toast);
		*isError = s_toastError;
	}
	LightLock_Unlock(&s_lock);
	return show;
}

// ---- press queue -----------------------------------------------------------------

// Steps waiting to go to the Mac. Everything queued by the time the press thread gets to it
// goes in one request, in order, so tapping quickly doesn't cost a round trip per tap.
#define QUEUE 32
static json_t *s_queue[QUEUE];
static int s_qHead, s_qTail;

static void enqueue(json_t *step) {
	LightLock_Lock(&s_lock);
	int next = (s_qTail + 1) % QUEUE;
	if (next != s_qHead) {
		s_queue[s_qTail] = step;
		s_qTail = next;
		step = NULL;
	}
	LightLock_Unlock(&s_lock);
	json_decref(step);  // queue full: drop it
	LightEvent_Signal(&s_pressEvent);
}

static json_t *step(const char *type, const char *field, const char *value) {
	return json_pack("{s:s,s:s}", "type", type, field, value);
}

void press_key(const PadKey *key) {
	switch (key->kind) {
	case KEY_ACTION: enqueue(step("action", "id", key->ref)); break;
	case KEY_APP: enqueue(step("openApp", "app", key->ref)); break;
	case KEY_WEB: enqueue(step("openURL", "url", key->ref)); break;
	case KEY_KEYS: enqueue(step("keystroke", "keys", key->ref)); break;
	case KEY_SHORTCUT: enqueue(step("shortcut", "name", key->ref)); break;
	default: break;
	}
}

void press_action(const char *actionId) { enqueue(step("action", "id", actionId)); }

// Takes every queued step; NULL when there are none.
static json_t *take_steps(void) {
	json_t *steps = NULL;
	LightLock_Lock(&s_lock);
	while (s_qHead != s_qTail) {
		if (!steps) steps = json_array();
		json_array_append_new(steps, s_queue[s_qHead]);
		s_qHead = (s_qHead + 1) % QUEUE;
	}
	LightLock_Unlock(&s_lock);
	return steps;
}

static void press_thread(void *arg) {
	while (!s_quit) {
		LightEvent_Wait(&s_pressEvent);
		for (;;) {
			json_t *steps = take_steps();
			if (!steps || s_quit) {
				json_decref(steps);
				break;
			}
			json_t *req = json_pack("{s:o}", "steps", steps);
			char *body = json_dumps(req, JSON_COMPACT);
			json_decref(req);
			HttpResponse res;
			bool sent = request(&s_pressConn, "POST", "/api/run", body, &res, 8000);
			free(body);
			json_t *root = sent ? json_loadb(res.body, res.len, 0, NULL) : NULL;
			if (!sent) {
				toast_set("Can't reach your Mac", true);
			} else if (res.status == 423) {
				toast_set(jstr(json_object_get(root, "off"), "message"), true);
			} else if (res.status == 401) {
				toast_set("Your Mac forgot this 3DS. Pair again from the menu.", true);
			} else if (res.status != 200) {
				toast_set(jstr(json_object_get(root, "error"), "message"), true);
			} else if (!json_is_true(json_object_get(root, "ok"))) {
				toast_set(jstr(root, "error"), true);
			}
			json_decref(root);
			http_free(&res);
		}
	}
}

// ---- app and website icons -------------------------------------------------------

#define ICON_QUEUE 128
#define ICON_FRESH_FOR (7 * 24 * 3600)  // seconds; a cached icon older than this is fetched again behind the scenes
typedef struct { char id[128]; bool fresh; } IconWant;
static IconWant s_iconWant[ICON_QUEUE];
static int s_iconWantCount;
typedef struct { char id[128]; u8 *px; int w, h; } IconDone;
static IconDone s_iconDone[ICON_QUEUE];
static int s_iconDoneCount;
static HttpConn s_iconConn;
// Set once the Mac says it has no /api/web/icon (a Macapad from before website icons), so the
// rest of the websites don't each ask.
static bool s_noWebIcons;

void icon_id(KeyKind kind, const char *ref, char *out, size_t size) {
	out[0] = 0;
	if (kind == KEY_APP) {
		copy(out, size, ref);
	} else if (kind == KEY_WEB) {
		char host[64];
		int port;
		if (!url_split(ref, host, sizeof host, &port)) return;
		const char *h = strncmp(host, "www.", 4) == 0 ? host + 4 : host;
		// the host is the file's name on the SD card, so only letters, digits, dots and dashes
		snprintf(out, size, "web:%s", h);
		for (char *c = out + 4; *c; c++) {
			bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '.' || *c == '-';
			if (!ok) *c = '_';
		}
	}
}

void icon_request(const char *id, bool fresh) {
	if (!id[0]) return;
	LightLock_Lock(&s_lock);
	int i = 0;
	while (i < s_iconWantCount && strcmp(s_iconWant[i].id, id)) i++;
	if (i < s_iconWantCount) {
		s_iconWant[i].fresh |= fresh;  // already waiting
	} else if (s_iconWantCount < ICON_QUEUE) {
		copy(s_iconWant[s_iconWantCount].id, sizeof s_iconWant[0].id, id);
		s_iconWant[s_iconWantCount++].fresh = fresh;
	}
	LightLock_Unlock(&s_lock);
	LightEvent_Signal(&s_iconEvent);
}

bool icon_take(char *id, size_t size, u8 **pixels, int *w, int *h) {
	LightLock_Lock(&s_lock);
	bool got = s_iconDoneCount > 0;
	if (got) {
		IconDone *d = &s_iconDone[--s_iconDoneCount];
		copy(id, size, d->id);
		*pixels = d->px;
		*w = d->w;
		*h = d->h;
	}
	LightLock_Unlock(&s_lock);
	return got;
}

static void icon_done(const char *id, u8 *px, int w, int h) {
	LightLock_Lock(&s_lock);
	if (s_iconDoneCount < ICON_QUEUE) {
		IconDone *d = &s_iconDone[s_iconDoneCount++];
		copy(d->id, sizeof d->id, id);
		d->px = px;  // NULL = none; the main thread stops asking until the Mac is back
		d->w = w;
		d->h = h;
	} else {
		free(px);
	}
	LightLock_Unlock(&s_lock);
}

static void *read_file(const char *path, size_t *len) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *data = malloc(*len);
	if (data && fread(data, 1, *len, f) != *len) {
		free(data);
		data = NULL;
	}
	fclose(f);
	return data;
}

// Where an icon lives on the SD card, and the path on the Mac that gives it.
static void icon_paths(const char *id, char *file, size_t fsize, char *url, size_t usize) {
	if (strncmp(id, "web:", 4) == 0) {
		snprintf(file, fsize, DATA_DIR "/icons/web/%s.png", id + 4);
		snprintf(url, usize, "/api/web/icon?host=%s&size=48", id + 4);
	} else {
		snprintf(file, fsize, DATA_DIR "/icons/%s.png", id);
		snprintf(url, usize, "/api/apps/icon?bundleId=%s&size=48", id);
	}
}

// Asks the Mac for the icon and keeps it on the SD card. NULL when there's none (or the Mac
// couldn't be reached: *reached says which).
static void *icon_fetch(const char *id, const char *url, const char *file, size_t *len, bool *reached) {
	bool web = strncmp(id, "web:", 4) == 0;
	*reached = false;
	if (web && s_noWebIcons) return NULL;
	void *png = NULL;
	HttpResponse res;
	if (request(&s_iconConn, "GET", url, NULL, &res, TIMEOUT_MS)) {
		*reached = true;
		if (res.status == 200 && res.len) {
			ensure_dirs();
			FILE *f = fopen(file, "wb");
			if (f) {
				fwrite(res.body, 1, res.len, f);
				fclose(f);
			}
			png = res.body;
			*len = res.len;
			res.body = NULL;
		} else if (web && res.status == 404) {
			s_noWebIcons = true;  // no such route; "no icon for this site" is a 204
		}
	}
	http_free(&res);
	return png;
}

static void icon_thread(void *arg) {
	while (!s_quit) {
		LightEvent_Wait(&s_iconEvent);
		for (;;) {
			IconWant want = {{0}, false};
			LightLock_Lock(&s_lock);
			bool have = s_iconWantCount > 0;
			if (have) want = s_iconWant[--s_iconWantCount];
			LightLock_Unlock(&s_lock);
			if (!have || s_quit) break;

			char file[200], url[256];
			icon_paths(want.id, file, sizeof file, url, sizeof url);
			size_t len = 0;
			void *png = NULL;
			bool old = false;
			if (!want.fresh) {
				struct stat st;
				if (stat(file, &st) == 0) {
					png = read_file(file, &len);
					old = st.st_mtime > 0 && time(NULL) - st.st_mtime > ICON_FRESH_FOR;
				}
			}
			bool reached = false;
			if (!png) png = icon_fetch(want.id, url, file, &len, &reached);
			int w = 0, h = 0;
			u8 *px = png ? png_decode(png, len, &w, &h) : NULL;
			free(png);
			icon_done(want.id, px, w, h);

			// the cached one is showing now; a week old, so ask the Mac for a current one behind it
			if (old && !s_quit) {
				png = icon_fetch(want.id, url, file, &len, &reached);
				px = png ? png_decode(png, len, &w, &h) : NULL;
				free(png);
				if (px) icon_done(want.id, px, w, h);
			}
		}
	}
}

// ---- thread lifetime ---------------------------------------------------------------

static bool s_haveCore1;

static Thread spawn(ThreadFunc fn, void *arg) {
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	// On the system core when the app got a share of it (macapad_init), so parsing answers and
	// decoding images never take a frame from the screen. Else next to the main thread, where
	// they run while it waits for the screen.
	Thread t = s_haveCore1 ? threadCreate(fn, arg, 32 * 1024, prio + 1, 1, false) : NULL;
	if (!t) t = threadCreate(fn, arg, 32 * 1024, prio + 1, -2, false);
	return t;
}

static bool s_started;
static void catalog_thread(void *arg);
static Thread s_catalogThread;
static Handle s_catalogWake;  // signalled to fetch now instead of at the next check
static HttpConn s_catalogConn;
static void catalog_forget(void);

void macapad_init(void) {
	LightLock_Init(&s_lock);
	s_haveCore1 = R_SUCCEEDED(APT_SetAppCpuTimeLimit(30));
}

void macapad_start(void) {
	if (s_started) return;
	memset(&s_status, 0, sizeof s_status);
	s_status.battery = -1;
	snapshot_load();
	s_snapshotAt = 0;
	s_noWebIcons = false;
	s_quit = false;
	LightEvent_Init(&s_pressEvent, RESET_ONESHOT);
	LightEvent_Init(&s_iconEvent, RESET_ONESHOT);
	svcCreateEvent(&s_catalogWake, RESET_ONESHOT);
	http_conn_init(&s_statusConn);
	http_conn_init(&s_pressConn);
	http_conn_init(&s_iconConn);
	http_conn_init(&s_catalogConn);
	s_statusThread = spawn(status_thread, NULL);
	s_pressThread = spawn(press_thread, NULL);
	s_iconThread = spawn(icon_thread, NULL);
	s_catalogThread = spawn(catalog_thread, NULL);
	s_started = true;
}

void macapad_stop(void) {
	if (!s_started) return;
	s_quit = true;
	LightEvent_Signal(&s_pressEvent);
	LightEvent_Signal(&s_iconEvent);
	svcSignalEvent(s_catalogWake);
	Thread threads[] = {s_statusThread, s_pressThread, s_iconThread, s_catalogThread};
	for (int i = 0; i < 4; i++) {
		threadJoin(threads[i], U64_MAX);
		threadFree(threads[i]);
	}
	svcCloseHandle(s_catalogWake);
	s_catalogWake = 0;
	snapshot_save();  // the threads are done: nothing else writes the status now
	http_conn_close(&s_statusConn);
	http_conn_close(&s_pressConn);
	http_conn_close(&s_iconConn);
	http_conn_close(&s_catalogConn);
	json_t *left = take_steps();
	json_decref(left);
	free(s_artPixels);
	s_artPixels = NULL;
	s_artFresh = false;
	while (s_iconDoneCount > 0) free(s_iconDone[--s_iconDoneCount].px);
	s_iconWantCount = 0;
	catalog_forget();
	s_started = false;
}

// ---- finding Macs --------------------------------------------------------------

bool probe_mac(const char *host, int port, FoundMac *mac) {
	HttpResponse res;
	bool ok = false;
	if (http_request("GET", host, port, "/api/health", NULL, NULL, &res, 1500) && res.status == 200) {
		json_t *root = json_loadb(res.body, res.len, 0, NULL);
		if (root && strcmp(jstr(root, "name"), "macapad") == 0) {
			memset(mac, 0, sizeof *mac);
			copy(mac->host, sizeof mac->host, host);
			mac->port = port;
			json_t *info = json_object_get(root, "mac");
			mac->canPair = json_is_true(json_object_get(root, "pairing"));
			copy(mac->macId, sizeof mac->macId, jstr(info, "id"));
			if (json_is_object(info)) copy(mac->name, sizeof mac->name, jstr(info, "name"));
			else snprintf(mac->name, sizeof mac->name, "Mac at %s", host);
			ok = true;
		}
		json_decref(root);
	}
	http_free(&res);
	return ok;
}

static void scan_thread(void *arg) {
	Scan *scan = arg;
	ScanHit hits[16];
	int n = net_scan(7317, hits, 16, &scan->progress, &scan->cancel);
	for (int i = 0; i < n && !scan->cancel && scan->count < 8; i++) {
		if (probe_mac(hits[i].host, hits[i].port, &scan->macs[scan->count])) scan->count++;
	}
	scan->running = false;
}

static Thread s_scanThread, s_pairThread;

void scan_start(Scan *scan) {
	memset(scan, 0, sizeof *scan);
	scan->running = true;
	s_scanThread = spawn(scan_thread, scan);
}

void scan_finish(Scan *scan) {
	if (!s_scanThread) return;
	scan->cancel = true;
	threadJoin(s_scanThread, U64_MAX);
	threadFree(s_scanThread);
	s_scanThread = NULL;
}

// ---- pairing -------------------------------------------------------------------

// "<console name>'s 3DS", or just "Nintendo 3DS".
static void device_name(char *out, size_t size) {
	u16 name[0x1C / 2 + 1] = {0};
	copy(out, size, "Nintendo 3DS");
	if (R_FAILED(cfguInit())) return;
	if (R_SUCCEEDED(CFGU_GetConfigInfoBlk2(0x1C, 0x000A0000, name)) && name[0]) {
		u8 utf8[64] = {0};
		utf16_to_utf8(utf8, name, sizeof utf8 - 1);
		snprintf(out, size, "%s's 3DS", (char *)utf8);
	}
	cfguExit();
}

static void pair_thread(void *arg) {
	Pairing *pair = arg;
	char name[64], body[160];
	device_name(name, sizeof name);
	json_t *req = json_pack("{s:s,s:s}", "name", name, "app", "Macapad for 3DS");
	char *text = json_dumps(req, JSON_COMPACT);
	json_decref(req);
	copy(body, sizeof body, text);
	free(text);

	HttpResponse res;
	bool sent = http_request("POST", pair->mac.host, pair->mac.port, "/api/pair", NULL, body, &res, TIMEOUT_MS);
	json_t *root = sent ? json_loadb(res.body, res.len, 0, NULL) : NULL;
	if (!sent || res.status != 200 || !root) {
		const char *msg = jstr(json_object_get(root, "error"), "message");
		if (!sent) {
			char why[96];
			http_last_failure(why, sizeof why);
			snprintf(pair->error, sizeof pair->error, "Nothing answered at %s:%d.\n%s", pair->mac.host, pair->mac.port, why);
		}
		else copy(pair->error, sizeof pair->error, *msg ? msg : "The Mac said no.");
		pair->state = PAIR_FAILED;
		json_decref(root);
		http_free(&res);
		pair->running = false;
		return;
	}
	char id[64], path[128];
	copy(id, sizeof id, jstr(root, "id"));
	copy(pair->code, sizeof pair->code, jstr(root, "code"));
	json_decref(root);
	http_free(&res);
	pair->state = PAIR_WAITING;

	snprintf(path, sizeof path, "/api/pair/%s?wait=5", id);
	while (!pair->cancel && pair->state == PAIR_WAITING) {
		if (!http_request("GET", pair->mac.host, pair->mac.port, path, NULL, NULL, &res, 9000)) {
			svcSleepThread(1000 * 1000000LL);
			continue;
		}
		root = json_loadb(res.body, res.len, 0, NULL);
		const char *status = jstr(root, "status");
		if (strcmp(status, "approved") == 0) {
			snprintf(g_config.url, sizeof g_config.url, "http://%s:%d", pair->mac.host, pair->mac.port);
			copy(g_config.token, sizeof g_config.token, jstr(root, "token"));
			copy(g_config.macId, sizeof g_config.macId, pair->mac.macId);
			copy(g_config.macName, sizeof g_config.macName, pair->mac.name);
			config_save();
			pair->state = PAIR_APPROVED;
		} else if (strcmp(status, "denied") == 0) {
			pair->state = PAIR_DENIED;
		} else if (strcmp(status, "expired") == 0 || res.status == 404) {
			pair->state = PAIR_EXPIRED;
		}
		json_decref(root);
		http_free(&res);
	}
	pair->running = false;
}

// 10/8, 172.16/12 or 192.168/16: an address on a home or office network, which a 3DS can reach.
// (The code can also carry Tailscale addresses, 100.64/10, which it can't.)
static bool is_lan_ip(struct in_addr a) {
	u32 ip = ntohl(a.s_addr);
	return (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8;
}

// Why no address in the code answered, as two lines: the 3DS is off Wi-Fi, on a different
// network from the Mac, the code only has addresses a 3DS can't use, or the Mac isn't answering.
static void qr_unreachable(Pairing *pair) {
	struct in_addr me = {0}, mask = {0}, bc = {0};
	if (SOCU_GetIPInfo(&me, &mask, &bc) != 0 || me.s_addr == 0) me.s_addr = (u32)gethostid();
	if (me.s_addr == 0) {
		copy(pair->error, sizeof pair->error, "This 3DS isn't on Wi-Fi.\nJoin the same network as your Mac.");
		return;
	}
	if (mask.s_addr == 0) mask.s_addr = htonl(0xFFFFFF00);
	char lan[64] = "";
	int lanPort = 0;
	bool sameNet = false;
	for (int i = 0; i < pair->urlCount; i++) {
		char host[64];
		int port;
		struct in_addr a;
		if (!url_split(pair->urls[i], host, sizeof host, &port) || !inet_aton(host, &a) || !is_lan_ip(a)) continue;
		if (!lan[0]) {
			copy(lan, sizeof lan, host);
			lanPort = port;
		}
		if (((a.s_addr ^ me.s_addr) & mask.s_addr) == 0) sameNet = true;
	}
	char mine[20];
	copy(mine, sizeof mine, inet_ntoa(me));
	if (!lan[0]) {
		copy(pair->error, sizeof pair->error,
		     "This code has no local-network address.\nAllow local-network pairing in Macapad.");
	} else if (!sameNet) {
		snprintf(pair->error, sizeof pair->error, "Your Mac is on a different network.\nMac %s, this 3DS %s.", lan, mine);
	} else {
		char why[96];
		http_last_failure(why, sizeof why);
		snprintf(pair->error, sizeof pair->error, "Nothing answered at %s:%d.\n%s", lan, lanPort, why);
	}
}

// Pairing with the QR code: POST the key to each address in the link until one answers.
static void pair_qr_thread(void *arg) {
	Pairing *pair = arg;
	char name[64], body[256];
	device_name(name, sizeof name);
	json_t *req = json_pack("{s:s,s:s,s:s}", "key", pair->qrKey, "name", name, "app", "Macapad for 3DS");
	char *text = json_dumps(req, JSON_COMPACT);
	json_decref(req);
	copy(body, sizeof body, text);
	free(text);

	bool reached = false;
	for (int i = 0; i < pair->urlCount && !pair->cancel && !reached; i++) {
		char host[64];
		int port;
		HttpResponse res;
		if (!url_split(pair->urls[i], host, sizeof host, &port)) continue;
		if (!http_request("POST", host, port, "/api/pair/qr", NULL, body, &res, 4000)) continue;
		reached = true;
		json_t *root = json_loadb(res.body, res.len, 0, NULL);
		const char *token = jstr(root, "token");
		if (res.status == 200 && *token) {
			json_t *mac = json_object_get(root, "mac");
			copy(pair->mac.host, sizeof pair->mac.host, host);
			pair->mac.port = port;
			if (*jstr(mac, "name")) copy(pair->mac.name, sizeof pair->mac.name, jstr(mac, "name"));
			if (*jstr(mac, "id")) copy(pair->mac.macId, sizeof pair->mac.macId, jstr(mac, "id"));
			snprintf(g_config.url, sizeof g_config.url, "http://%s:%d", host, port);
			copy(g_config.token, sizeof g_config.token, token);
			copy(g_config.macId, sizeof g_config.macId, pair->mac.macId);
			copy(g_config.macName, sizeof g_config.macName, pair->mac.name);
			config_save();
			pair->state = PAIR_APPROVED;
		} else {
			const char *msg = jstr(json_object_get(root, "error"), "message");
			copy(pair->error, sizeof pair->error, *msg ? msg : "Your Mac said no.");
			pair->state = PAIR_FAILED;
		}
		json_decref(root);
		http_free(&res);
	}
	if (!reached && !pair->cancel) {
		qr_unreachable(pair);
		pair->state = PAIR_FAILED;
	}
	pair->running = false;
}

// "%20" -> " ", in place
static void percent_decode(char *s) {
	char *out = s;
	for (; *s; s++) {
		int hi, lo;
		if (*s == '%' && s[1] && s[2] && sscanf(s + 1, "%1x%1x", &hi, &lo) == 2) {
			*out++ = (char)(hi << 4 | lo);
			s += 2;
		} else {
			*out++ = *s;
		}
	}
	*out = 0;
}

bool pair_qr_start(Pairing *pair, const char *link) {
	static const char prefix[] = "macapad://pair?";
	if (strncmp(link, prefix, sizeof prefix - 1) != 0) return false;
	memset(&pair->mac, 0, sizeof pair->mac);
	pair->qrKey[0] = 0;
	pair->urlCount = 0;
	char query[1024];
	copy(query, sizeof query, link + sizeof prefix - 1);
	char *save = NULL;
	for (char *kv = strtok_r(query, "&", &save); kv; kv = strtok_r(NULL, "&", &save)) {
		char *eq = strchr(kv, '=');
		if (!eq) continue;
		*eq = 0;
		char *value = eq + 1;
		percent_decode(value);
		if (!strcmp(kv, "key")) copy(pair->qrKey, sizeof pair->qrKey, value);
		else if (!strcmp(kv, "name")) copy(pair->mac.name, sizeof pair->mac.name, value);
		else if (!strcmp(kv, "id")) copy(pair->mac.macId, sizeof pair->mac.macId, value);
		else if (!strcmp(kv, "url") && pair->urlCount < 4) copy(pair->urls[pair->urlCount++], sizeof pair->urls[0], value);
	}
	if (!pair->qrKey[0] || !pair->urlCount) return false;
	if (!pair->mac.name[0]) copy(pair->mac.name, sizeof pair->mac.name, "your Mac");
	pair->viaQr = true;
	pair->running = true;
	pair->cancel = false;
	pair->state = PAIR_STARTING;
	pair->code[0] = pair->error[0] = 0;
	s_pairThread = spawn(pair_qr_thread, pair);
	return true;
}

void pair_start(Pairing *pair) {
	pair->viaQr = false;
	pair->running = true;
	pair->cancel = false;
	pair->state = PAIR_STARTING;
	pair->code[0] = pair->error[0] = 0;
	s_pairThread = spawn(pair_thread, pair);
}

void pair_finish(Pairing *pair) {
	if (!s_pairThread) return;
	pair->cancel = true;
	threadJoin(s_pairThread, U64_MAX);
	threadFree(s_pairThread);
	s_pairThread = NULL;
}

// ---- catalog cache ---------------------------------------------------------------

// The three lists the key editor picks from, kept in memory and on the SD card. A thread fetches
// each again when it's older than its interval (asking the Mac for its apps and Shortcuts takes
// a second or two), or right away when asked.
typedef struct {
	CatalogItem *items;
	int count;
	u32 generation;         // bumps on every change; 0 = nothing yet
	s64 savedAt;            // when the Mac answered, seconds since 1970 on this 3DS's clock; 0 = never
	u64 triedAt;            // osGetTime() of the last fetch, to space out retries
	bool loaded;
	bool failed;            // the last fetch didn't work: error says why
	bool wanted;            // a list or Refresh asked for it
	bool fetching;
	char error[96];
} CatalogCache;

static CatalogCache s_cache[3];
static const s64 s_catalogEvery[3] = {5 * 60, 30 * 60, 10 * 60};  // seconds: actions, apps, Shortcuts
static const char *s_catalogPaths[3] = {"/api/actions", "/api/apps", "/api/shortcuts"};
static const char *s_catalogFiles[3] = {
	DATA_DIR "/catalog-actions.json", DATA_DIR "/catalog-apps.json", DATA_DIR "/catalog-shortcuts.json"};
#define CATALOG_RETRY_MS 30000       // after a failed fetch on its own
#define CATALOG_START_DELAY_MS 3000  // the first status poll and the pad's icons go first

// The items of a list from the Mac (its field names) or from the SD card copy (ours).
static CatalogItem *catalog_parse(CatalogKind kind, json_t *list, bool fromMac, int *count) {
	size_t n = json_array_size(list);
	CatalogItem *items = calloc(n ? n : 1, sizeof(CatalogItem));
	if (!items) return NULL;
	*count = 0;
	size_t i;
	json_t *item;
	json_array_foreach(list, i, item) {
		CatalogItem *c = &items[*count];
		if (!fromMac) {
			copy(c->label, sizeof c->label, jstr(item, "label"));
			copy(c->sub, sizeof c->sub, jstr(item, "sub"));
			copy(c->value, sizeof c->value, jstr(item, "value"));
			copy(c->icon, sizeof c->icon, jstr(item, "icon"));
		} else {
			switch (kind) {
			case CATALOG_ACTIONS:
				copy(c->label, sizeof c->label, jstr(item, "name"));
				copy(c->sub, sizeof c->sub, jstr(item, "category"));
				copy(c->value, sizeof c->value, jstr(item, "id"));
				copy(c->icon, sizeof c->icon, jstr(item, "icon"));
				break;
			case CATALOG_APPS:
				copy(c->label, sizeof c->label, jstr(item, "name"));
				copy(c->value, sizeof c->value, jstr(item, "bundleId"));
				break;
			case CATALOG_SHORTCUTS:
				copy(c->label, sizeof c->label, json_string_value(item));
				copy(c->value, sizeof c->value, json_string_value(item));
				break;
			}
		}
		if (c->value[0]) (*count)++;
	}
	return items;
}

// Takes ownership of items.
static void catalog_store(CatalogKind kind, CatalogItem *items, int count, s64 savedAt) {
	LightLock_Lock(&s_lock);
	CatalogCache *c = &s_cache[kind];
	free(c->items);
	c->items = items;
	c->count = count;
	c->savedAt = savedAt;
	c->loaded = true;
	c->failed = false;
	c->error[0] = 0;
	c->generation++;
	LightLock_Unlock(&s_lock);
}

static void catalog_load_file(CatalogKind kind) {
	json_t *root = json_load_file(s_catalogFiles[kind], 0, NULL);
	if (!root) return;
	if (strcmp(jstr(root, "macId"), g_config.macId) == 0) {
		int count = 0;
		CatalogItem *items = catalog_parse(kind, json_object_get(root, "items"), false, &count);
		if (items) catalog_store(kind, items, count, json_integer_value(json_object_get(root, "savedAt")));
	}
	json_decref(root);
}

// Written next to the real file first, so a copy half-written when the app closes is never read.
static void catalog_save_file(CatalogKind kind, const CatalogItem *items, int count, s64 savedAt) {
	json_t *arr = json_array();
	for (int i = 0; i < count; i++) {
		json_t *o = json_pack("{s:s,s:s,s:s,s:s}", "label", items[i].label, "sub", items[i].sub,
			"value", items[i].value, "icon", items[i].icon);
		if (o) json_array_append_new(arr, o);
	}
	json_t *root = json_pack("{s:s,s:I}", "macId", g_config.macId, "savedAt", (json_int_t)savedAt);
	if (!root) {
		json_decref(arr);
		return;
	}
	json_object_set_new(root, "items", arr);
	ensure_dirs();
	char tmp[128];
	snprintf(tmp, sizeof tmp, "%s.tmp", s_catalogFiles[kind]);
	if (json_dump_file(root, tmp, JSON_COMPACT) == 0) {
		remove(s_catalogFiles[kind]);
		rename(tmp, s_catalogFiles[kind]);
	}
	json_decref(root);
}

static void catalog_fetch(CatalogKind kind) {
	CatalogCache *c = &s_cache[kind];
	LightLock_Lock(&s_lock);
	c->fetching = true;
	c->wanted = false;
	c->triedAt = osGetTime();
	LightLock_Unlock(&s_lock);

	static const char *lists[] = {"actions", "apps", "shortcuts"};
	HttpResponse res;
	bool sent = request(&s_catalogConn, "GET", s_catalogPaths[kind], NULL, &res, 8000);
	if (sent && res.status == 200) {
		json_t *root = json_loadb(res.body, res.len, 0, NULL);
		int count = 0;
		CatalogItem *items = root ? catalog_parse(kind, json_object_get(root, lists[kind]), true, &count) : NULL;
		json_decref(root);
		if (items) {
			s64 now = time(NULL);
			catalog_save_file(kind, items, count, now);
			catalog_store(kind, items, count, now);
		}
	} else {
		LightLock_Lock(&s_lock);
		c->failed = true;
		copy(c->error, sizeof c->error, !sent ? "Can't reach your Mac." : res.status == 401 ? "Your Mac forgot this 3DS." : "Your Mac said no.");
		LightLock_Unlock(&s_lock);
	}
	http_free(&res);
	LightLock_Lock(&s_lock);
	c->fetching = false;
	LightLock_Unlock(&s_lock);
}

static void catalog_thread(void *arg) {
	for (int k = 0; k < 3 && !s_quit; k++) catalog_load_file(k);
	u64 startedAt = osGetTime();
	while (!s_quit) {
		for (int k = 0; k < 3 && !s_quit; k++) {
			CatalogCache *c = &s_cache[k];
			LightLock_Lock(&s_lock);
			bool wanted = c->wanted;
			bool due = !c->loaded || time(NULL) - c->savedAt > s_catalogEvery[k];
			bool recentTry = c->triedAt && osGetTime() - c->triedAt < CATALOG_RETRY_MS;
			bool online = s_status.online;
			LightLock_Unlock(&s_lock);
			if (wanted) {
				catalog_fetch(k);
				continue;
			}
			// On its own: after the start, spaced out after a failure, and not while the Mac is away.
			if (!due || osGetTime() - startedAt < CATALOG_START_DELAY_MS || recentTry || !online) continue;
			catalog_fetch(k);
		}
		if (s_catalogWake) svcWaitSynchronization(s_catalogWake, 2000 * 1000000LL);
		else svcSleepThread(2000 * 1000000LL);  // no event to wait on: just look again in a while
	}
}

// Drops what's in memory (the files stay): the next start loads them again, for the Mac paired then.
static void catalog_forget(void) {
	for (int k = 0; k < 3; k++) {
		CatalogCache *c = &s_cache[k];
		free(c->items);
		c->items = NULL;
		c->count = 0;
		c->savedAt = 0;
		c->triedAt = 0;
		c->loaded = c->failed = c->wanted = c->fetching = false;
		c->error[0] = 0;
		c->generation++;
	}
}

bool catalog_sync(Catalog *cat) {
	bool changed = false;
	LightLock_Lock(&s_lock);
	CatalogCache *c = &s_cache[cat->kind];
	if (c->loaded && c->generation != cat->generation) {
		CatalogItem *items = malloc((c->count ? c->count : 1) * sizeof(CatalogItem));
		if (items) {
			if (c->count) memcpy(items, c->items, c->count * sizeof(CatalogItem));
			free(cat->items);
			cat->items = items;
			cat->count = c->count;
			cat->generation = c->generation;
			cat->done = cat->ok = true;
			cat->error[0] = 0;
			changed = true;
		}
	} else if (!c->loaded && !cat->done && c->failed && !c->fetching && !c->wanted) {
		cat->done = true;
		cat->ok = false;
		copy(cat->error, sizeof cat->error, c->error);
		changed = true;
	}
	LightLock_Unlock(&s_lock);
	return changed;
}

void catalog_start(Catalog *cat, CatalogKind kind) {
	catalog_finish(cat);
	memset(cat, 0, sizeof *cat);
	cat->kind = kind;
	LightLock_Lock(&s_lock);
	bool loaded = s_cache[kind].loaded;
	LightLock_Unlock(&s_lock);
	if (!loaded) catalog_refresh(kind);
	catalog_sync(cat);
}

void catalog_finish(Catalog *cat) {
	free(cat->items);
	cat->items = NULL;
	cat->count = 0;
	cat->done = cat->ok = false;
}

void catalog_refresh(int kind) {
	LightLock_Lock(&s_lock);
	for (int k = 0; k < 3; k++) if (kind < 0 || k == kind) s_cache[k].wanted = true;
	LightLock_Unlock(&s_lock);
	if (s_catalogWake) svcSignalEvent(s_catalogWake);
}

void catalog_info(CatalogKind kind, CatalogInfo *info) {
	LightLock_Lock(&s_lock);
	CatalogCache *c = &s_cache[kind];
	info->loaded = c->loaded;
	info->fetching = c->fetching || c->wanted;
	info->failed = c->failed;
	info->ageSec = c->savedAt ? time(NULL) - c->savedAt : -1;
	copy(info->error, sizeof info->error, c->error);
	LightLock_Unlock(&s_lock);
}

void cache_clear(void) {
	for (int k = 0; k < 3; k++) remove(s_catalogFiles[k]);
	remove(SNAPSHOT_FILE);
}

// ---- PNG ---------------------------------------------------------------------------

u8 *png_decode(const void *data, size_t len, int *w, int *h) {
	png_image img;
	memset(&img, 0, sizeof img);
	img.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_memory(&img, data, len)) return NULL;
	img.format = PNG_FORMAT_RGBA;
	u8 *px = malloc(PNG_IMAGE_SIZE(img));
	if (!px || !png_image_finish_read(&img, NULL, px, 0, NULL)) {
		free(px);
		png_image_free(&img);
		return NULL;
	}
	*w = img.width;
	*h = img.height;
	return px;
}
