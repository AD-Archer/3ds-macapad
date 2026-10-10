#include "macapad.h"
#include "http.h"

#include <jansson.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

Config g_config;
Pads g_pads;

#define STATUS_EVERY_MS 1500
#define TIMEOUT_MS 3000

static void copy(char *dst, size_t size, const char *src) {
	snprintf(dst, size, "%s", src ? src : "");
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
			json_array_append_new(keys, obj);
		}
		json_array_append_new(pages, json_pack("{s:s,s:o}", "name", p->name, "keys", keys));
	}
	json_t *root = json_pack("{s:i,s:o}", "version", 1, "pages", pages);
	json_dump_file(root, DATA_DIR "/pads.json", JSON_INDENT(2));
	json_decref(root);
}

// ---- talking to the Mac ------------------------------------------------------

static bool request(const char *method, const char *path, const char *body, HttpResponse *res, int timeout) {
	char host[64];
	int port;
	if (!url_split(g_config.url, host, sizeof host, &port)) return false;
	return http_request(method, host, port, path, g_config.token, body, res, timeout);
}

// ---- status thread -----------------------------------------------------------

static LightLock s_lock;
static Status s_status;
static Thread s_statusThread, s_pressThread, s_iconThread;
static volatile bool s_quit;
static LightEvent s_pressEvent, s_iconEvent;

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

static void apply_status(json_t *root) {
	Status st;
	LightLock_Lock(&s_lock);
	st = s_status;
	LightLock_Unlock(&s_lock);

	st.have = st.online = true;
	st.receivedAt = osGetTime();
	st.macTime = parse_iso(jstr(root, "time"));
	st.utcOffset = json_integer_value(json_object_get(root, "utcOffset"));
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
	if (st.hasWeather) {
		st.temp = json_integer_value(json_object_get(wx, "temp"));
		st.high = json_integer_value(json_object_get(wx, "high"));
		st.low = json_integer_value(json_object_get(wx, "low"));
		copy(st.unit, sizeof st.unit, jstr(wx, "unit"));
		copy(st.symbol, sizeof st.symbol, jstr(wx, "symbol"));
		copy(st.condition, sizeof st.condition, jstr(wx, "condition"));
		st.isDay = json_is_true(json_object_get(wx, "isDay"));
		copy(st.place, sizeof st.place, jstr(wx, "place"));
	}

	json_t *stats = json_object_get(root, "stats");
	st.cpu = json_number_value(json_object_get(stats, "cpu"));
	st.memUsed = json_number_value(json_object_get(stats, "memoryUsed"));
	st.memTotal = json_number_value(json_object_get(stats, "memoryTotal"));
	json_t *bat = json_object_get(stats, "battery");
	st.battery = json_is_integer(bat) ? (int)json_integer_value(bat) : -1;
	st.charging = json_is_true(json_object_get(stats, "charging"));
	st.pluggedIn = json_is_true(json_object_get(stats, "pluggedIn"));
	st.diskFree = json_number_value(json_object_get(stats, "diskFree"));
	st.diskTotal = json_number_value(json_object_get(stats, "diskTotal"));
	st.uptime = json_integer_value(json_object_get(stats, "uptime"));
	copy(st.thermal, sizeof st.thermal, jstr(stats, "thermal"));

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
	if (request("GET", "/api/status/artwork?size=128", NULL, &res, TIMEOUT_MS) && res.status == 200) {
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
		if (request("GET", "/api/status", NULL, &res, TIMEOUT_MS) && res.status == 200) {
			json_t *root = json_loadb(res.body, res.len, 0, NULL);
			if (root) apply_status(root);
			json_decref(root);
			misses = 0;
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

#define QUEUE 16
static char *s_queue[QUEUE];
static int s_qHead, s_qTail;

static void enqueue(json_t *steps) {
	json_t *body = json_pack("{s:o}", "steps", steps);
	char *text = json_dumps(body, JSON_COMPACT);
	json_decref(body);
	LightLock_Lock(&s_lock);
	int next = (s_qTail + 1) % QUEUE;
	if (next != s_qHead) {
		s_queue[s_qTail] = text;
		s_qTail = next;
		text = NULL;
	}
	LightLock_Unlock(&s_lock);
	free(text);  // queue full: drop it
	LightEvent_Signal(&s_pressEvent);
}

static json_t *step(const char *type, const char *field, const char *value) {
	return json_pack("[{s:s,s:s}]", "type", type, field, value);
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

static void press_thread(void *arg) {
	while (!s_quit) {
		LightEvent_Wait(&s_pressEvent);
		for (;;) {
			LightLock_Lock(&s_lock);
			char *body = s_qHead != s_qTail ? s_queue[s_qHead] : NULL;
			if (body) s_qHead = (s_qHead + 1) % QUEUE;
			LightLock_Unlock(&s_lock);
			if (!body || s_quit) {
				free(body);
				break;
			}
			HttpResponse res;
			bool sent = request("POST", "/api/run", body, &res, 8000);
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

// ---- icons ---------------------------------------------------------------------

#define ICON_QUEUE 64
static char s_iconWant[ICON_QUEUE][128];
static int s_iconWantCount;
typedef struct { char id[128]; u8 *px; int w, h; } IconDone;
static IconDone s_iconDone[ICON_QUEUE];
static int s_iconDoneCount;

void icon_request(const char *bundleId) {
	LightLock_Lock(&s_lock);
	if (s_iconWantCount < ICON_QUEUE) copy(s_iconWant[s_iconWantCount++], 128, bundleId);
	LightLock_Unlock(&s_lock);
	LightEvent_Signal(&s_iconEvent);
}

bool icon_take(char *bundleId, size_t size, u8 **pixels, int *w, int *h) {
	LightLock_Lock(&s_lock);
	bool got = s_iconDoneCount > 0;
	if (got) {
		IconDone *d = &s_iconDone[--s_iconDoneCount];
		copy(bundleId, size, d->id);
		*pixels = d->px;
		*w = d->w;
		*h = d->h;
	}
	LightLock_Unlock(&s_lock);
	return got;
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

static void icon_thread(void *arg) {
	while (!s_quit) {
		LightEvent_Wait(&s_iconEvent);
		for (;;) {
			char id[128];
			LightLock_Lock(&s_lock);
			bool have = s_iconWantCount > 0;
			if (have) copy(id, sizeof id, s_iconWant[--s_iconWantCount]);
			LightLock_Unlock(&s_lock);
			if (!have || s_quit) break;

			char path[200];
			snprintf(path, sizeof path, DATA_DIR "/icons/%s.png", id);
			size_t len = 0;
			void *png = read_file(path, &len);
			if (!png) {
				char url[200];
				snprintf(url, sizeof url, "/api/apps/icon?bundleId=%s&size=48", id);
				HttpResponse res;
				if (request("GET", url, NULL, &res, TIMEOUT_MS) && res.status == 200) {
					ensure_dirs();
					FILE *f = fopen(path, "wb");
					if (f) {
						fwrite(res.body, 1, res.len, f);
						fclose(f);
					}
					png = res.body;
					len = res.len;
					res.body = NULL;
				}
				http_free(&res);
			}
			int w = 0, h = 0;
			u8 *px = png ? png_decode(png, len, &w, &h) : NULL;
			free(png);
			LightLock_Lock(&s_lock);
			if (s_iconDoneCount < ICON_QUEUE) {
				IconDone *d = &s_iconDone[s_iconDoneCount++];
				copy(d->id, sizeof d->id, id);
				d->px = px;  // NULL = failed; the main thread stops asking
				d->w = w;
				d->h = h;
			} else {
				free(px);
			}
			LightLock_Unlock(&s_lock);
		}
	}
}

// ---- thread lifetime ---------------------------------------------------------------

static Thread spawn(ThreadFunc fn, void *arg) {
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	return threadCreate(fn, arg, 32 * 1024, prio + 1, -2, false);
}

static bool s_started;

void macapad_init(void) {
	LightLock_Init(&s_lock);
}

void macapad_start(void) {
	if (s_started) return;
	memset(&s_status, 0, sizeof s_status);
	s_status.battery = -1;
	s_quit = false;
	LightEvent_Init(&s_pressEvent, RESET_ONESHOT);
	LightEvent_Init(&s_iconEvent, RESET_ONESHOT);
	s_statusThread = spawn(status_thread, NULL);
	s_pressThread = spawn(press_thread, NULL);
	s_iconThread = spawn(icon_thread, NULL);
	s_started = true;
}

void macapad_stop(void) {
	if (!s_started) return;
	s_quit = true;
	LightEvent_Signal(&s_pressEvent);
	LightEvent_Signal(&s_iconEvent);
	Thread threads[] = {s_statusThread, s_pressThread, s_iconThread};
	for (int i = 0; i < 3; i++) {
		threadJoin(threads[i], U64_MAX);
		threadFree(threads[i]);
	}
	free(s_artPixels);
	s_artPixels = NULL;
	s_artFresh = false;
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

static Thread s_scanThread, s_pairThread, s_catalogThread;

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
		if (!sent) snprintf(pair->error, sizeof pair->error, "Nothing answered at %s:%d.", pair->mac.host, pair->mac.port);
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

void pair_start(Pairing *pair) {
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

// ---- catalog ---------------------------------------------------------------------

static void catalog_thread(void *arg) {
	Catalog *cat = arg;
	static const char *paths[] = {"/api/actions", "/api/apps", "/api/shortcuts"};
	static const char *lists[] = {"actions", "apps", "shortcuts"};
	HttpResponse res;
	if (!request("GET", paths[cat->kind], NULL, &res, 8000) || res.status != 200) {
		copy(cat->error, sizeof cat->error, res.status ? "Your Mac said no." : "Can't reach your Mac.");
		http_free(&res);
		cat->done = true;
		cat->running = false;
		return;
	}
	json_t *root = json_loadb(res.body, res.len, 0, NULL);
	json_t *list = json_object_get(root, lists[cat->kind]);
	size_t n = json_array_size(list);
	cat->items = calloc(n ? n : 1, sizeof(CatalogItem));
	size_t i;
	json_t *item;
	json_array_foreach(list, i, item) {
		CatalogItem *c = &cat->items[cat->count];
		switch (cat->kind) {
		case CATALOG_ACTIONS:
			copy(c->label, sizeof c->label, jstr(item, "name"));
			copy(c->sub, sizeof c->sub, jstr(item, "category"));
			copy(c->value, sizeof c->value, jstr(item, "id"));
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
		if (c->value[0]) cat->count++;
	}
	json_decref(root);
	http_free(&res);
	cat->ok = true;
	cat->done = true;
	cat->running = false;
}

void catalog_start(Catalog *cat, CatalogKind kind) {
	memset(cat, 0, sizeof *cat);
	cat->kind = kind;
	cat->running = true;
	s_catalogThread = spawn(catalog_thread, cat);
}

void catalog_finish(Catalog *cat) {
	if (s_catalogThread) {
		threadJoin(s_catalogThread, U64_MAX);
		threadFree(s_catalogThread);
		s_catalogThread = NULL;
	}
	free(cat->items);
	cat->items = NULL;
	cat->count = 0;
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
