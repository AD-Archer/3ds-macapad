#pragma once
#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

// Reading a QR code with the outer camera, for pairing with the code Macapad shows on the Mac.
// Frames come in at 640x480 so the code's squares stay sharp; the preview is half that.
#define QRSCAN_PREVIEW_W 320
#define QRSCAN_PREVIEW_H 240

// Turns the camera on and starts reading on its own thread. False if the camera won't start.
bool qrscan_start(void);
void qrscan_stop(void);

// Copies the newest preview frame (RGB565, QRSCAN_PREVIEW_W x QRSCAN_PREVIEW_H) into out.
// False when there's no new frame since the last call.
bool qrscan_preview(u16 *out);

// The text of a QR code it read, once; then it goes back to looking.
bool qrscan_take(char *out, size_t size);
