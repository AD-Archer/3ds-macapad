#pragma once
#include <3ds.h>
#include <stdbool.h>

// Live input: keys and mouse buttons held on the Mac for as long as they're held here, and the
// pointer moved in real time (Macapad 1.0's /api/input/session + UDP). A thread sends the whole
// state each time, so a lost packet can't leave a key down.

#define INPUT_MAX_KEYS 16
#define INPUT_KEY_LEN 16

#define MOUSE_LEFT 1
#define MOUSE_RIGHT 2
#define MOUSE_MIDDLE 4

typedef enum {
	INPUT_CONNECTING,  // asking the Mac for a session
	INPUT_LIVE,        // sending; see through
	INPUT_OFF,         // paused or outside the Mac's hours: message says which
	INPUT_OUTDATED,    // the Mac's Macapad is older than 1.0
	INPUT_FORGOTTEN,   // the Mac doesn't know this 3DS any more
	INPUT_AWAY,        // the Mac couldn't be reached
} InputState;

typedef struct {
	InputState state;
	bool through;       // the Mac answered lately, so input is getting through
	char message[160];  // the Mac's words when it's off
} InputInfo;

typedef struct {
	char keys[INPUT_MAX_KEYS][INPUT_KEY_LEN];
	int count;
	u8 buttons;  // MOUSE_*
} InputHeld;

// Adds a key (no repeats, up to INPUT_MAX_KEYS). "escape" goes as "esc".
void held_add(InputHeld *h, const char *key);
// Adds every key of a combo like "cmd+tab".
void held_add_combo(InputHeld *h, const char *combo);

void input_start(void);  // when a screen that sends input opens
void input_stop(void);   // when it closes: lets go of everything (three empty packets)
bool input_running(void);

// What's held right now; call every frame. Sent at once when it changes.
void input_hold(const InputHeld *held);
// Pointer and scroll moved since the last call, in points (fractions carry over).
void input_move(float dx, float dy, float sx, float sy);
// Lets go of everything at once, from any thread (sleep, the HOME Menu).
void input_release(void);

void input_info(InputInfo *out);
