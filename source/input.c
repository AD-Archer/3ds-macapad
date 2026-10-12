#include "input.h"
#include "http.h"
#include "macapad.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <jansson.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SESSION_TIMEOUT_MS 2000  // short: leaving the screen waits for it
#define RETRY_MS 3000            // asking for a session again after it didn't work
#define RENEW_MS 2000            // at most this often when the Mac says the session's gone
#define REPEAT_MS 50             // while anything is held, the same state again this often (the Mac wants 100 at most)
#define HEARTBEAT_MS 500         // with nothing held (the Mac wants one a second)
#define ACK_EVERY_MS 1000
#define THROUGH_MS 2500          // input counts as getting through while the Mac answered this recently
#define MAX_MOVE 500

static LightLock s_lock;      // what's held and moved, and s_info: shared with the main thread
static LightLock s_sendLock;  // the socket, the session and the packet number
static bool s_ready;
static Thread s_thread;
static volatile bool s_quit;
static LightEvent s_wake;

static InputHeld s_held;
static float s_dx, s_dy, s_sx, s_sy;
static InputInfo s_info;

static int s_fd = -1;
static char s_session[40];
static struct sockaddr_in s_mac;
static long long s_n;  // the packet number; starts from the clock, so it keeps going up across launches

static const char *jstr(json_t *obj, const char *key) {
	const char *s = json_string_value(json_object_get(obj, key));
	return s ? s : "";
}

// ---- what's held ---------------------------------------------------------------

void held_add(InputHeld *h, const char *key) {
	if (!key || !*key) return;
	if (!strcmp(key, "escape")) key = "esc";  // (the shortcut picker's name for it)
	for (int i = 0; i < h->count; i++) if (!strcmp(h->keys[i], key)) return;
	if (h->count == INPUT_MAX_KEYS) return;
	snprintf(h->keys[h->count++], INPUT_KEY_LEN, "%s", key);
}

void held_add_combo(InputHeld *h, const char *combo) {
	char buf[64], *save = NULL;
	snprintf(buf, sizeof buf, "%s", combo);
	for (char *k = strtok_r(buf, "+", &save); k; k = strtok_r(NULL, "+", &save)) held_add(h, k);
}

static bool held_same(const InputHeld *a, const InputHeld *b) {
	if (a->count != b->count || a->buttons != b->buttons) return false;
	for (int i = 0; i < a->count; i++) {
		int j = 0;
		while (j < b->count && strcmp(a->keys[i], b->keys[j])) j++;
		if (j == b->count) return false;
	}
	return true;
}

// ---- state for the screen ---------------------------------------------------------

static void set_state(InputState state, const char *message) {
	LightLock_Lock(&s_lock);
	s_info.state = state;
	snprintf(s_info.message, sizeof s_info.message, "%s", message ? message : "");
	if (state != INPUT_LIVE) s_info.through = false;
	LightLock_Unlock(&s_lock);
}

void input_info(InputInfo *out) {
	if (!s_ready) {
		memset(out, 0, sizeof *out);
		return;
	}
	LightLock_Lock(&s_lock);
	*out = s_info;
	LightLock_Unlock(&s_lock);
}

// ---- talking to the Mac -----------------------------------------------------------

// Asks the Mac for a session. False when there's none to be had right now; s_info says why.
static bool open_session(void) {
	char host[64];
	int port;
	if (!url_split(g_config.url, host, sizeof host, &port)) {
		set_state(INPUT_AWAY, NULL);
		return false;
	}
	HttpResponse res;
	bool sent = http_request("POST", host, port, "/api/input/session", g_config.token, "{}", &res, SESSION_TIMEOUT_MS);
	json_t *root = sent ? json_loadb(res.body, res.len, 0, NULL) : NULL;
	bool ok = false;
	if (!sent) {
		set_state(INPUT_AWAY, NULL);
	} else if (res.status == 200 && *jstr(root, "session")) {
		int udp = (int)json_integer_value(json_object_get(root, "udpPort"));
		LightLock_Lock(&s_sendLock);
		snprintf(s_session, sizeof s_session, "%s", jstr(root, "session"));
		memset(&s_mac, 0, sizeof s_mac);
		s_mac.sin_family = AF_INET;
		s_mac.sin_port = htons(udp > 0 ? udp : port);
		inet_aton(host, &s_mac.sin_addr);
		LightLock_Unlock(&s_sendLock);
		set_state(INPUT_LIVE, NULL);
		ok = true;
	} else if (res.status == 404) {
		set_state(INPUT_OUTDATED, NULL);  // a Macapad from before live input
	} else if (res.status == 401) {
		set_state(INPUT_FORGOTTEN, NULL);
	} else if (res.status == 423) {
		const char *msg = jstr(json_object_get(root, "off"), "message");
		set_state(INPUT_OFF, *msg ? msg : jstr(json_object_get(root, "error"), "message"));
	} else {
		set_state(INPUT_AWAY, jstr(json_object_get(root, "error"), "message"));
	}
	json_decref(root);
	http_free(&res);
	return ok;
}

static void drop_session(void) {
	LightLock_Lock(&s_sendLock);
	s_session[0] = 0;
	LightLock_Unlock(&s_sendLock);
}

// One packet: the whole state, with the movement since the last one. From any thread.
static bool send_state(const InputHeld *h, int dx, int dy, int sx, int sy, bool ack) {
	LightLock_Lock(&s_sendLock);
	bool ok = false;
	if (s_fd >= 0 && s_session[0]) {
		json_t *o = json_object();
		json_object_set_new(o, "s", json_string(s_session));
		json_object_set_new(o, "n", json_integer(++s_n));
		json_t *keys = json_array();
		for (int i = 0; i < h->count; i++) json_array_append_new(keys, json_string(h->keys[i]));
		json_object_set_new(o, "keys", keys);
		json_t *buttons = json_array();
		if (h->buttons & MOUSE_LEFT) json_array_append_new(buttons, json_string("left"));
		if (h->buttons & MOUSE_RIGHT) json_array_append_new(buttons, json_string("right"));
		if (h->buttons & MOUSE_MIDDLE) json_array_append_new(buttons, json_string("middle"));
		json_object_set_new(o, "buttons", buttons);
		if (dx) json_object_set_new(o, "dx", json_integer(dx));
		if (dy) json_object_set_new(o, "dy", json_integer(dy));
		if (sx) json_object_set_new(o, "sx", json_integer(sx));
		if (sy) json_object_set_new(o, "sy", json_integer(sy));
		if (ack) json_object_set_new(o, "ack", json_true());
		char *text = json_dumps(o, JSON_COMPACT);
		json_decref(o);
		if (text) {
			ok = sendto(s_fd, text, strlen(text), 0, (struct sockaddr *)&s_mac, sizeof s_mac) > 0;
			free(text);
		}
	}
	LightLock_Unlock(&s_sendLock);
	return ok;
}

// The whole points moved, clamped; the fractions stay for next time.
static int take_move(float *acc) {
	int v = (int)*acc;
	*acc -= v;
	return v > MAX_MOVE ? MAX_MOVE : v < -MAX_MOVE ? -MAX_MOVE : v;
}

static void input_thread(void *arg) {
	InputHeld sent = {0};
	u64 lastSent = 0, lastAck = 0, lastOk = 0, retryAt = 0, sessionAt = 0;
	bool fresh = true;  // a new session: send at once
	while (!s_quit) {
		if (!s_session[0]) {
			LightLock_Lock(&s_lock);
			s_dx = s_dy = s_sx = s_sy = 0;  // nothing piles up while there's nowhere to send it
			LightLock_Unlock(&s_lock);
			if (osGetTime() < retryAt) {
				LightEvent_WaitTimeout(&s_wake, 100 * 1000000LL);
				continue;
			}
			sessionAt = osGetTime();
			if (!open_session()) {
				retryAt = osGetTime() + RETRY_MS;
				continue;
			}
			fresh = true;
			lastOk = 0;
		}
		LightEvent_WaitTimeout(&s_wake, 20 * 1000000LL);
		if (s_quit) break;

		LightLock_Lock(&s_lock);
		InputHeld held = s_held;
		int dx = take_move(&s_dx), dy = take_move(&s_dy), sx = take_move(&s_sx), sy = take_move(&s_sy);
		LightLock_Unlock(&s_lock);

		u64 now = osGetTime();
		bool active = held.count || held.buttons;
		if (fresh || dx || dy || sx || sy || !held_same(&held, &sent) || now - lastSent >= (active ? REPEAT_MS : HEARTBEAT_MS)) {
			bool ack = now - lastAck >= ACK_EVERY_MS;
			if (send_state(&held, dx, dy, sx, sy, ack) && ack) lastAck = now;
			sent = held;
			lastSent = now;
			fresh = false;
		}

		// the Mac's answers: "ok" for an ack, or why it didn't take a packet
		char buf[512];
		int n;
		while (s_fd >= 0 && (n = recvfrom(s_fd, buf, sizeof buf - 1, MSG_DONTWAIT, NULL, NULL)) > 0) {
			buf[n] = 0;
			json_t *r = json_loads(buf, 0, NULL);
			const char *type = jstr(r, "type"), *reason = jstr(r, "reason");
			if (!strcmp(type, "ok")) {
				lastOk = now;
				LightLock_Lock(&s_lock);
				if (s_info.state != INPUT_LIVE) s_info.message[0] = 0;
				s_info.state = INPUT_LIVE;
				LightLock_Unlock(&s_lock);
			} else if (!strcmp(type, "error") && !strcmp(reason, "session")) {
				// expired, the Mac restarted, or this 3DS was unpaired: ask for a new one and carry on
				drop_session();
				retryAt = sessionAt + RENEW_MS;
			} else if (!strcmp(type, "error") && !strcmp(reason, "off")) {
				set_state(INPUT_OFF, jstr(r, "message"));
				drop_session();
				retryAt = now + RETRY_MS;
			}
			json_decref(r);
		}
		LightLock_Lock(&s_lock);
		s_info.through = lastOk && now - lastOk < THROUGH_MS;
		LightLock_Unlock(&s_lock);
	}
	// leaving: let go of everything, three times over in case one is lost
	InputHeld none = {0};
	for (int i = 0; i < 3; i++) send_state(&none, 0, 0, 0, 0, false);
}

// ---- lifetime -----------------------------------------------------------------------

void input_start(void) {
	if (!s_ready) {
		LightLock_Init(&s_lock);
		LightLock_Init(&s_sendLock);
		s_ready = true;
	}
	if (s_thread) return;
	if (!s_n) s_n = (long long)osGetTime();
	memset(&s_held, 0, sizeof s_held);
	s_dx = s_dy = s_sx = s_sy = 0;
	memset(&s_info, 0, sizeof s_info);
	s_info.state = INPUT_CONNECTING;
	s_session[0] = 0;
	s_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (s_fd >= 0) fcntl(s_fd, F_SETFL, fcntl(s_fd, F_GETFL, 0) | O_NONBLOCK);
	s_quit = false;
	LightEvent_Init(&s_wake, RESET_ONESHOT);
	s_thread = macapad_spawn(input_thread, NULL);
}

void input_stop(void) {
	if (!s_thread) return;
	s_quit = true;
	LightEvent_Signal(&s_wake);
	threadJoin(s_thread, U64_MAX);
	threadFree(s_thread);
	s_thread = NULL;
	LightLock_Lock(&s_sendLock);
	if (s_fd >= 0) close(s_fd);
	s_fd = -1;
	s_session[0] = 0;
	LightLock_Unlock(&s_sendLock);
}

bool input_running(void) { return s_thread != NULL; }

void input_hold(const InputHeld *held) {
	if (!s_thread) return;
	LightLock_Lock(&s_lock);
	bool changed = !held_same(held, &s_held);
	s_held = *held;
	LightLock_Unlock(&s_lock);
	if (changed) LightEvent_Signal(&s_wake);
}

void input_move(float dx, float dy, float sx, float sy) {
	if (!s_thread || (!dx && !dy && !sx && !sy)) return;
	LightLock_Lock(&s_lock);
	s_dx += dx;
	s_dy += dy;
	s_sx += sx;
	s_sy += sy;
	bool whole = s_dx >= 1 || s_dx <= -1 || s_dy >= 1 || s_dy <= -1 || s_sx >= 1 || s_sx <= -1 || s_sy >= 1 || s_sy <= -1;
	LightLock_Unlock(&s_lock);
	if (whole) LightEvent_Signal(&s_wake);
}

void input_release(void) {
	if (!s_thread) return;
	LightLock_Lock(&s_lock);
	memset(&s_held, 0, sizeof s_held);
	s_dx = s_dy = s_sx = s_sy = 0;
	LightLock_Unlock(&s_lock);
	// sent from here, at once: the system may put the thread to sleep before it gets a turn
	InputHeld none = {0};
	send_state(&none, 0, 0, 0, 0, false);
	LightEvent_Signal(&s_wake);
}
