#include "qrscan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quirc/quirc.h"

#define CAM_W 640
#define CAM_H 480
#define FRAME_BYTES (CAM_W * CAM_H * 2)

// Two threads, as in FBI's camera task: a capture thread that does nothing but take frames
// off the camera, and a decode thread that chews on them. Decoding a 640x480 frame takes a
// good while on a 3DS; if the camera has no receive pending meanwhile its buffer overflows,
// capture stops, and the preview freezes on one frame. So the capture thread runs above the
// main thread's priority and always has the next receive queued, and the decoder waits
// below it.
//
// Three frame buffers go round: the camera fills s_capture, the newest finished frame waits
// in s_ready, and the decoder reads s_decoding. Swapping pointers means nobody copies 600 KB.
static u16 *s_capture, *s_ready, *s_decoding;
static bool s_readyFresh;  // s_ready holds a frame the decoder hasn't looked at
static u16 *s_preview;     // under s_lock
static bool s_previewFresh;

static Thread s_captureThread, s_decodeThread;
static Handle s_cancel;          // signalled to stop the capture thread
static LightEvent s_frameEvent;  // signalled when a frame lands in s_ready
static volatile bool s_quit;     // stops the decode thread
static bool s_camOpen;
static LightLock s_lock;
static char s_text[1024];
static bool s_found;  // s_text holds a code nobody has taken yet
static struct quirc *s_qr;

// Big (quirc_data is ~9 KB), so not on the thread's stack.
static struct quirc_code s_code;
static struct quirc_data s_data;

// Reads every code in s_decoding; keeps the first that decodes.
static void decode_frame(void) {
	int w, h;
	u8 *gray = quirc_begin(s_qr, &w, &h);
	for (int i = 0; i < CAM_W * CAM_H; i++) {
		u16 p = s_decoding[i];
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

static void decode_thread(void *arg) {
	while (!s_quit) {
		LightEvent_Wait(&s_frameEvent);
		if (s_quit) break;
		LightLock_Lock(&s_lock);
		// nothing to do while a found code waits to be taken
		bool fresh = s_readyFresh && !s_found;
		if (fresh) {
			u16 *t = s_decoding;
			s_decoding = s_ready;
			s_ready = t;
			s_readyFresh = false;
		}
		LightLock_Unlock(&s_lock);
		if (fresh) decode_frame();
	}
}

enum { EV_CANCEL, EV_RECV, EV_ERROR, EV_COUNT };

static void capture_thread(void *arg) {
	Handle ev[EV_COUNT] = {s_cancel, 0, 0};
	u32 unit = 0;
	bool running = R_SUCCEEDED(CAMU_GetBufferErrorInterruptEvent(&ev[EV_ERROR], PORT_CAM1)) &&
	               R_SUCCEEDED(CAMU_GetMaxBytes(&unit, CAM_W, CAM_H)) &&
	               R_SUCCEEDED(CAMU_SetTransferBytes(PORT_CAM1, unit, CAM_W, CAM_H)) &&
	               R_SUCCEEDED(CAMU_Activate(SELECT_OUT1)) &&
	               R_SUCCEEDED(CAMU_ClearBuffer(PORT_CAM1)) &&
	               // the receive goes in before capture starts, so the first frame has somewhere to go
	               R_SUCCEEDED(CAMU_SetReceiving(&ev[EV_RECV], s_capture, PORT_CAM1, FRAME_BYTES, (s16)unit)) &&
	               R_SUCCEEDED(CAMU_StartCapture(PORT_CAM1));
	while (running) {
		s32 which = -1;
		if (R_FAILED(svcWaitSynchronizationN(&which, ev, EV_COUNT, false, U64_MAX)) || which == EV_CANCEL) break;
		if (ev[EV_RECV]) svcCloseHandle(ev[EV_RECV]);
		ev[EV_RECV] = 0;
		if (which == EV_RECV) {
			LightLock_Lock(&s_lock);
			// the preview: every other pixel of every other row
			for (int y = 0; y < QRSCAN_PREVIEW_H; y++) {
				const u16 *src = s_capture + y * 2 * CAM_W;
				u16 *dst = s_preview + y * QRSCAN_PREVIEW_W;
				for (int x = 0; x < QRSCAN_PREVIEW_W; x++) dst[x] = src[x * 2];
			}
			s_previewFresh = true;
			u16 *t = s_ready;
			s_ready = s_capture;
			s_capture = t;
			s_readyFresh = true;
			LightLock_Unlock(&s_lock);
			LightEvent_Signal(&s_frameEvent);
			running = R_SUCCEEDED(CAMU_SetReceiving(&ev[EV_RECV], s_capture, PORT_CAM1, FRAME_BYTES, (s16)unit));
		} else {
			// the camera's buffer overflowed, which stops capture: start it over
			running = R_SUCCEEDED(CAMU_ClearBuffer(PORT_CAM1)) &&
			          R_SUCCEEDED(CAMU_SetReceiving(&ev[EV_RECV], s_capture, PORT_CAM1, FRAME_BYTES, (s16)unit)) &&
			          R_SUCCEEDED(CAMU_StartCapture(PORT_CAM1));
		}
	}
	CAMU_StopCapture(PORT_CAM1);
	bool busy = false;
	while (R_SUCCEEDED(CAMU_IsBusy(&busy, PORT_CAM1)) && busy) svcSleepThread(1000000);
	CAMU_ClearBuffer(PORT_CAM1);
	CAMU_Activate(SELECT_NONE);
	if (ev[EV_RECV]) svcCloseHandle(ev[EV_RECV]);
	if (ev[EV_ERROR]) svcCloseHandle(ev[EV_ERROR]);
}

bool qrscan_start(void) {
	if (s_captureThread) return true;
	LightLock_Init(&s_lock);
	LightEvent_Init(&s_frameEvent, RESET_ONESHOT);
	s_capture = malloc(FRAME_BYTES);
	s_ready = malloc(FRAME_BYTES);
	s_decoding = malloc(FRAME_BYTES);
	s_preview = calloc(QRSCAN_PREVIEW_W * QRSCAN_PREVIEW_H, sizeof(u16));
	s_qr = quirc_new();
	if (!s_capture || !s_ready || !s_decoding || !s_preview || !s_qr || quirc_resize(s_qr, CAM_W, CAM_H) < 0 ||
	    R_FAILED(svcCreateEvent(&s_cancel, RESET_STICKY))) {
		qrscan_stop();
		return false;
	}
	if (R_FAILED(camInit())) {
		qrscan_stop();
		return false;
	}
	s_camOpen = true;
	CAMU_SetSize(SELECT_OUT1, SIZE_VGA, CONTEXT_A);
	CAMU_SetOutputFormat(SELECT_OUT1, OUTPUT_RGB_565, CONTEXT_A);
	CAMU_SetFrameRate(SELECT_OUT1, FRAME_RATE_15);  // decoding can't keep up with more anyway
	CAMU_SetNoiseFilter(SELECT_OUT1, true);
	CAMU_SetAutoExposure(SELECT_OUT1, true);
	CAMU_SetAutoWhiteBalance(SELECT_OUT1, true);
	CAMU_SetTrimming(PORT_CAM1, false);

	s_quit = false;
	s_found = s_previewFresh = s_readyFresh = false;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	// quirc_decode keeps ~18 KB on the stack
	s_decodeThread = threadCreate(decode_thread, NULL, 128 * 1024, prio + 1, -2, false);
	if (s_decodeThread) s_captureThread = threadCreate(capture_thread, NULL, 32 * 1024, prio - 2, -2, false);
	if (!s_decodeThread || !s_captureThread) {
		qrscan_stop();
		return false;
	}
	return true;
}

void qrscan_stop(void) {
	if (s_captureThread) {
		svcSignalEvent(s_cancel);
		threadJoin(s_captureThread, U64_MAX);
		threadFree(s_captureThread);
		s_captureThread = NULL;
	}
	if (s_decodeThread) {
		s_quit = true;
		LightEvent_Signal(&s_frameEvent);
		threadJoin(s_decodeThread, U64_MAX);
		threadFree(s_decodeThread);
		s_decodeThread = NULL;
	}
	if (s_camOpen) camExit();
	s_camOpen = false;
	if (s_cancel) svcCloseHandle(s_cancel);
	s_cancel = 0;
	if (s_qr) quirc_destroy(s_qr);
	s_qr = NULL;
	free(s_capture);
	free(s_ready);
	free(s_decoding);
	free(s_preview);
	s_capture = s_ready = s_decoding = s_preview = NULL;
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
	if (!s_captureThread) return false;
	LightLock_Lock(&s_lock);
	bool found = s_found;
	if (found) snprintf(out, size, "%s", s_text);
	s_found = false;
	LightLock_Unlock(&s_lock);
	return found;
}
