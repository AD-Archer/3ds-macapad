#pragma once
#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
	int status;       // HTTP status, or 0 when the Mac couldn't be reached
	char *body;       // NUL-terminated, owned; free with http_free
	size_t len;
} HttpResponse;

// One request over a fresh connection (Connection: close). token and body may be NULL.
// Returns false when nothing came back at all.
bool http_request(const char *method, const char *host, int port, const char *path,
                  const char *token, const char *body, HttpResponse *out, int timeout_ms);
void http_free(HttpResponse *res);

// "http://192.168.1.5:7317" -> host + port. Port defaults to 7317.
bool url_split(const char *url, char *host, size_t hostsz, int *port);
// Why the last connection attempt failed, in a few words (also in sdmc:/3ds/macapad/net.log).
void http_last_failure(char *out, size_t size);

typedef struct {
	char host[64];
	int port;
} ScanHit;

// Knocks on the Macapad port of every address on this 3DS's /24 network.
// progress counts 0..254; set *cancel to stop early. Returns how many answered.
int net_scan(int port, ScanHit *hits, int max, volatile int *progress, volatile bool *cancel);
