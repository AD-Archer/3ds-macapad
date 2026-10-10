#include "qrscan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quirc/quirc.h"

#define CAM_W 640
#define CAM_H 480
#define FRAME_BYTES (CAM_W * CAM_H * 2)

static Thread s_thread;
static volatile bool s_quit;
static LightLock s_lock;
static u16 *s_frame;    // the camera writes here; the thread's alone
static u16 *s_preview;  // under s_lock
static bool s_previewFresh;
static char s_text[1024];
static bool s_found;    // s_text holds a code nobody has taken yet
static struct quirc *s_qr;

// Big (quirc_data is ~9 KB), so not on the thread's stack.
static struct quirc_code s_code;
static struct quirc_data s_data;

// Reads every code in the frame; keeps the first that decodes.
static void decode_frame(void) {
	int w, h;
	u8 *gray = quirc_begin(s_qr, &w, &h);
	for (int i = 0; i < CAM_W * CAM_H; i++) {
		u16 p = s_frame[i];
		int r = (p >> 11) << 3, g = ((p >> 5) & 0x3F) << 2, b = (p & 0x1F) << 3;
		gray[i] = (r * 77 + g * 150 + b * 29) >> 8;
	}
	quirc_end(s_qr);
	for (int i = 0; i < quirc_count(s_qr); i++) {
		quirc_extract(s_qr, i, &s_code);
		if (quirc_decode(&s_code, &s_data) != QUIRC_SUCCESS) {
			// seen in a mirror (or the camera's flipped): try it the other way round
			quirc_flip(&s_code);
			if (quirc_decode(&s_code, &s_data) != QUIRC_SUCCESS) continue;
		}
		LightLock_Lock(&s_lock);
		snprintf(s_text, sizeof s_text, "%.*s", s_data.payload_len, (const char *)s_data.payload);
		s_found = true;
		LightLock_Unlock(&s_lock);
		return;
	}
}

static void scan_thread(void *arg) {
	u32 unit = 0;
	CAMU_GetMaxBytes(&unit, CAM_W, CAM_H);
	CAMU_SetTransferBytes(PORT_CAM1, unit, CAM_W, CAM_H);
	CAMU_Activate(SELECT_OUT1);
	CAMU_ClearBuffer(PORT_CAM1);
	CAMU_StartCapture(PORT_CAM1);
	while (!s_quit) {
		Handle done = 0;
		if (R_FAILED(CAMU_SetReceiving(&done, s_frame, PORT_CAM1, FRAME_BYTES, (s16)unit))) break;
		// 0 = a frame arrived; anything else is a timeout or an error, so start the transfer over
		Result r = svcWaitSynchronization(done, 500 * 1000000LL);
		svcCloseHandle(done);
		if (r != 0) {
			CAMU_ClearBuffer(PORT_CAM1);
			continue;
		}

		// the preview: every other pixel of every other row
		LightLock_Lock(&s_lock);
		for (int y = 0; y < QRSCAN_PREVIEW_H; y++) {
			const u16 *src = s_frame + y * 2 * CAM_W;
			u16 *dst = s_preview + y * QRSCAN_PREVIEW_W;
			for (int x = 0; x < QRSCAN_PREVIEW_W; x++) dst[x] = src[x * 2];
		}
		s_previewFresh = true;
		bool waiting = s_found;
		LightLock_Unlock(&s_lock);

		if (!waiting) decode_frame();
	}
	CAMU_StopCapture(PORT_CAM1);
	CAMU_Activate(SELECT_NONE);
}

bool qrscan_start(void) {
	if (s_thread) return true;
	LightLock_Init(&s_lock);
	s_frame = malloc(FRAME_BYTES);
	s_preview = calloc(QRSCAN_PREVIEW_W * QRSCAN_PREVIEW_H, sizeof(u16));
	s_qr = quirc_new();
	if (!s_frame || !s_preview || !s_qr || quirc_resize(s_qr, CAM_W, CAM_H) < 0 || R_FAILED(camInit())) {
		qrscan_stop();
		return false;
	}
	CAMU_SetSize(SELECT_OUT1, SIZE_VGA, CONTEXT_A);
	CAMU_SetOutputFormat(SELECT_OUT1, OUTPUT_RGB_565, CONTEXT_A);
	CAMU_SetFrameRate(SELECT_OUT1, FRAME_RATE_15);  // decoding can't keep up with more anyway
	CAMU_SetNoiseFilter(SELECT_OUT1, true);
	CAMU_SetAutoExposure(SELECT_OUT1, true);
	CAMU_SetAutoWhiteBalance(SELECT_OUT1, true);
	CAMU_SetTrimming(PORT_CAM1, false);

	s_quit = false;
	s_found = s_previewFresh = false;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	// quirc_decode keeps ~18 KB on the stack
	s_thread = threadCreate(scan_thread, NULL, 128 * 1024, prio + 1, -2, false);
	if (!s_thread) {
		camExit();
		qrscan_stop();
		return false;
	}
	return true;
}

void qrscan_stop(void) {
	if (s_thread) {
		s_quit = true;
		threadJoin(s_thread, U64_MAX);
		threadFree(s_thread);
		s_thread = NULL;
		camExit();
	}
	if (s_qr) quirc_destroy(s_qr);
	s_qr = NULL;
	free(s_frame);
	free(s_preview);
	s_frame = s_preview = NULL;
}

bool qrscan_preview(u16 *out) {
	if (!s_preview) return false;
	LightLock_Lock(&s_lock);
	bool fresh = s_previewFresh;
	if (fresh) memcpy(out, s_preview, QRSCAN_PREVIEW_W * QRSCAN_PREVIEW_H * sizeof(u16));
	s_previewFresh = false;
	LightLock_Unlock(&s_lock);
	return fresh;
}

bool qrscan_take(char *out, size_t size) {
	if (!s_thread) return false;
	LightLock_Lock(&s_lock);
	bool found = s_found;
	if (found) snprintf(out, size, "%s", s_text);
	s_found = false;
	LightLock_Unlock(&s_lock);
	return found;
}
