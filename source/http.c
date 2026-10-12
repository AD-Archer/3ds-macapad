#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define CLIENT_NAME "Nintendo 3DS"

// IP addresses only: the 3DS can't resolve .local names, and a failed gethostbyname
// crashes Azahar (its reply carries a static buffer the app never set up).
static bool resolve(const char *host, struct in_addr *out) {
	return inet_aton(host, out);
}

// Starts a non-blocking connect; returns the socket, or -1.
static int start_connect(struct in_addr addr, int port) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
	struct sockaddr_in sa = {0};
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr = addr;
	// in progress is the normal answer for a non-blocking connect; some stacks say "would block" instead
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0 && errno != EINPROGRESS && errno != EALREADY &&
	    errno != EAGAIN && errno != EWOULDBLOCK) {
		close(fd);
		return -1;
	}
	return fd;
}

// Whether a non-blocking connect has gone through: 1 yes, 0 not yet, -1 it failed. A connected
// socket has a peer; libctru isn't sure which bits the 3DS's poll() uses for errors and hangups,
// so this doesn't read them. SO_ERROR comes first and a failure ends the attempt: reading it
// clears it, and Azahar crashes when getpeername() fails (it converts the address it never got),
// so getpeername() is only asked while nothing has gone wrong.
static int connected(int fd) {
	int err = 0;
	socklen_t len = sizeof err;
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err != 0) {
		errno = err;
		return -1;
	}
	struct sockaddr_in peer;
	len = sizeof peer;
	if (getpeername(fd, (struct sockaddr *)&peer, &len) == 0) return 1;
	return errno == ENOTCONN ? 0 : 1;  // can't tell; the send will
}

// Every connection attempt, appended to the SD card (kept small), for finding out what the
// 3DS's socket API did on real hardware when nothing answers.
#define NET_LOG "sdmc:/3ds/macapad/net.log"
static char s_lastFailure[96];
static void net_log(const char *host, int port, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s_lastFailure, sizeof s_lastFailure, fmt, ap);
	va_end(ap);
	struct stat st;
	FILE *f = fopen(NET_LOG, stat(NET_LOG, &st) == 0 && st.st_size > 48 * 1024 ? "w" : "a");
	if (!f) return;
	fprintf(f, "%llu %s:%d %s\n", (unsigned long long)osGetTime(), host, port, s_lastFailure);
	fclose(f);
}

void http_last_failure(char *out, size_t size) {
	snprintf(out, size, "%s", s_lastFailure);
}

static int connect_timeout(const char *host, int port, int timeout_ms) {
	struct in_addr addr;
	if (!resolve(host, &addr)) {
		net_log(host, port, "not an IP address");
		return -1;
	}
	int fd = start_connect(addr, port);
	if (fd < 0) {
		net_log(host, port, "connect failed: errno %d", errno);
		return -1;
	}
	// Any answer from poll() counts; whether it's a connection is for connected() to say.
	// (Its revents bits are logged in case they turn out to matter.)
	u64 deadline = osGetTime() + timeout_ms;
	for (;;) {
		struct pollfd p = {fd, POLLOUT, 0};
		int left = (int)(deadline - osGetTime());
		int r = left > 0 ? poll(&p, 1, left) : 0;
		if (r < 0) {
			net_log(host, port, "poll failed: errno %d", errno);
			break;
		}
		if (r == 0) {
			net_log(host, port, "no answer in %d ms", timeout_ms);
			break;
		}
		int state = connected(fd);
		if (state > 0) {
			int one = 1;  // small requests go out at once instead of waiting to be coalesced
			setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
			return fd;
		}
		if (state < 0 || (p.revents & (POLLERR | POLLNVAL))) {
			net_log(host, port, "refused: errno %d, revents 0x%x", state < 0 ? errno : 0, p.revents);
			break;
		}
		if (osGetTime() >= deadline) {
			net_log(host, port, "woke with revents 0x%x but never connected", p.revents);
			break;
		}
		svcSleepThread(20 * 1000000LL);  // woke but not connected yet; ask again shortly
	}
	close(fd);
	return -1;
}

static bool send_all(int fd, const char *data, size_t len, int timeout_ms) {
	while (len > 0) {
		struct pollfd p = {fd, POLLOUT, 0};
		if (poll(&p, 1, timeout_ms) <= 0) return false;
		ssize_t n = send(fd, data, len, 0);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
			return false;
		}
		data += n;
		len -= n;
	}
	return true;
}

// Finds `name:` among the headers (case-insensitive) and returns its value, or NULL.
static const char *header(const char *headers, const char *end, const char *name) {
	size_t n = strlen(name);
	for (const char *line = headers; line && line < end; ) {
		if (strncasecmp(line, name, n) == 0 && line[n] == ':') {
			const char *v = line + n + 1;
			while (*v == ' ') v++;
			return v;
		}
		line = strstr(line, "\r\n");
		if (line) line += 2;
	}
	return NULL;
}

// Rewrites a chunked body in place; returns the new length.
static size_t unchunk(char *body, size_t len) {
	char *in = body, *out = body, *end = body + len;
	while (in < end) {
		char *nl = strstr(in, "\r\n");
		if (!nl) break;
		size_t size = strtoul(in, NULL, 16);
		in = nl + 2;
		if (size == 0 || in + size > end) break;
		memmove(out, in, size);
		out += size;
		in += size + 2;
	}
	*out = 0;
	return out - body;
}

// Reads one response into a malloc'd buffer. Stops at the end of the body when the headers say
// where that is (Content-Length, or the last chunk), else when the Mac hangs up. *headLen is where
// the body starts (0 if no headers arrived); *reusable says the connection can carry another request.
static char *read_response(int fd, size_t *lenOut, size_t *headLen, bool *chunked, bool *reusable, int timeout_ms) {
	size_t cap = 4096, len = 0;
	long want = -1;  // body bytes promised by Content-Length
	bool willClose = false, complete = false;
	*headLen = 0;
	*chunked = false;
	char *buf = malloc(cap);
	if (!buf) return NULL;
	buf[0] = 0;
	for (;;) {
		if (*headLen) {
			if (want >= 0 && len >= *headLen + (size_t)want) { complete = true; break; }
			if (*chunked && len >= *headLen + 5 && memcmp(buf + len - 5, "0\r\n\r\n", 5) == 0) { complete = true; break; }
		}
		struct pollfd p = {fd, POLLIN, 0};
		if (poll(&p, 1, timeout_ms) <= 0) break;
		if (len + 2048 > cap) {
			cap *= 2;
			char *grown = realloc(buf, cap);
			if (!grown) break;
			buf = grown;
		}
		ssize_t r = recv(fd, buf + len, cap - len - 1, 0);
		if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
		if (r <= 0) { willClose = true; break; }
		len += r;
		buf[len] = 0;
		if (!*headLen) {
			char *split = strstr(buf, "\r\n\r\n");
			if (!split) continue;
			*headLen = split + 4 - buf;
			const char *cl = header(buf, split, "Content-Length");
			const char *te = header(buf, split, "Transfer-Encoding");
			const char *cn = header(buf, split, "Connection");
			if (cl) want = atol(cl);
			else if (te && strncasecmp(te, "chunked", 7) == 0) *chunked = true;
			// 204 and 304 never carry a body, whether or not the headers say so
			if (strncmp(buf, "HTTP/", 5) == 0 && (atoi(buf + 9) == 204 || atoi(buf + 9) == 304)) want = 0;
			if (cn && strncasecmp(cn, "close", 5) == 0) willClose = true;
		}
	}
	buf[len] = 0;
	*lenOut = len;
	*reusable = complete && !willClose;
	return buf;
}

// The request as one buffer, so head and body leave in a single packet.
static char *build_request(const char *method, const char *host, int port, const char *path,
                           const char *token, const char *body, bool keep, size_t *len) {
	size_t bodyLen = body ? strlen(body) : 0;
	char *req = malloc(768 + bodyLen + (token ? strlen(token) : 0));
	if (!req) return NULL;
	int n = sprintf(req,
		"%s %s HTTP/1.1\r\nHost: %s:%d\r\nUser-Agent: Macapad3DS/1.0\r\nAccept: */*\r\n"
		"Connection: %s\r\nX-Macapad-Client: " CLIENT_NAME "\r\n",
		method, path, host, port, keep ? "keep-alive" : "close");
	if (token && *token) n += sprintf(req + n, "Authorization: Bearer %s\r\n", token);
	if (body) n += sprintf(req + n, "Content-Type: application/json\r\nContent-Length: %d\r\n", (int)bodyLen);
	n += sprintf(req + n, "\r\n");
	if (body) memcpy(req + n, body, bodyLen);
	*len = n + bodyLen;
	return req;
}

bool http_request_on(HttpConn *conn, const char *method, const char *host, int port, const char *path,
                     const char *token, const char *body, HttpResponse *out, int timeout_ms) {
	memset(out, 0, sizeof *out);
	bool keep = conn != NULL;
	size_t reqLen;
	char *req = build_request(method, host, port, path, token, body, keep, &reqLen);
	if (!req) return false;

	int fd = -1;
	bool reused = false;
	if (keep && conn->fd >= 0) {
		if (conn->port == port && strcmp(conn->host, host) == 0) {
			fd = conn->fd;
			reused = true;
		} else {
			close(conn->fd);  // the Mac moved; that one's no use now
		}
		conn->fd = -1;
	}

	char *buf = NULL;
	size_t len = 0, headLen = 0;
	bool chunked = false, reusable = false;
	for (;;) {
		if (fd < 0) {
			fd = connect_timeout(host, port, timeout_ms);
			if (fd < 0) { free(req); return false; }
		}
		bool sent = send_all(fd, req, reqLen, timeout_ms);
		if (sent) buf = read_response(fd, &len, &headLen, &chunked, &reusable, timeout_ms);
		if (sent && buf && len > 0) break;
		// nothing came back; on a kept connection that usually means the Mac had already closed it
		close(fd);
		fd = -1;
		free(buf);
		buf = NULL;
		if (reused) { reused = false; continue; }
		net_log(host, port, sent ? "sent %s %s, but nothing came back" : "connected, but sending %s %s failed: errno %d", method, path, errno);
		free(req);
		return false;
	}
	free(req);

	if (keep && reusable) {
		conn->fd = fd;
		snprintf(conn->host, sizeof conn->host, "%s", host);
		conn->port = port;
	} else {
		close(fd);
	}

	if (len < 12 || strncmp(buf, "HTTP/", 5) != 0 || !headLen) {
		net_log(host, port, "sent %s %s, but the reply made no sense (%u bytes)", method, path, (unsigned)len);
		free(buf);
		if (keep) http_conn_close(conn);
		return false;
	}
	out->status = atoi(strchr(buf, ' ') + 1);
	char *bodyStart = buf + headLen;
	size_t bodyLen = len - headLen;
	if (chunked) bodyLen = unchunk(bodyStart, bodyLen);
	memmove(buf, bodyStart, bodyLen);
	buf[bodyLen] = 0;
	out->body = buf;
	out->len = bodyLen;
	return true;
}

bool http_request(const char *method, const char *host, int port, const char *path,
                  const char *token, const char *body, HttpResponse *out, int timeout_ms) {
	return http_request_on(NULL, method, host, port, path, token, body, out, timeout_ms);
}

void http_conn_init(HttpConn *conn) {
	memset(conn, 0, sizeof *conn);
	conn->fd = -1;
}

void http_conn_close(HttpConn *conn) {
	if (conn->fd >= 0) close(conn->fd);
	conn->fd = -1;
}

void http_free(HttpResponse *res) {
	free(res->body);
	res->body = NULL;
	res->len = 0;
}

bool url_split(const char *url, char *host, size_t hostsz, int *port) {
	const char *p = strstr(url, "://");
	p = p ? p + 3 : url;
	size_t n = strcspn(p, ":/");
	if (n == 0 || n >= hostsz) return false;
	memcpy(host, p, n);
	host[n] = 0;
	*port = p[n] == ':' ? atoi(p + n + 1) : 7317;
	return *port > 0;
}

#define SCAN_BATCH 24
#define SCAN_WAIT_MS 700

int net_scan(int port, ScanHit *hits, int max, volatile int *progress, volatile bool *cancel) {
	u32 me = ntohl((u32)gethostid());
	u32 base = me & 0xFFFFFF00;
	int found = 0;
	*progress = 0;
	for (int start = 1; start < 255 && !*cancel && found < max; start += SCAN_BATCH) {
		struct pollfd fds[SCAN_BATCH];
		u32 ips[SCAN_BATCH];
		int count = 0;
		for (int h = start; h < start + SCAN_BATCH && h < 255; h++) {
			u32 ip = base | h;
			if (ip == me) continue;
			struct in_addr addr = {htonl(ip)};
			int fd = start_connect(addr, port);
			if (fd < 0) continue;
			fds[count] = (struct pollfd){fd, POLLOUT, 0};
			ips[count++] = ip;
		}
		// wait for this batch, taking each socket out as it answers
		u64 deadline = osGetTime() + SCAN_WAIT_MS;
		int pending = count;
		bool done[SCAN_BATCH] = {0};
		while (pending > 0 && osGetTime() < deadline) {
			struct pollfd live[SCAN_BATCH];
			int map[SCAN_BATCH], n = 0;
			for (int i = 0; i < count; i++) if (!done[i]) { live[n] = fds[i]; live[n].revents = 0; map[n++] = i; }
			int r = poll(live, n, (int)(deadline - osGetTime()));
			if (r <= 0) break;
			for (int i = 0; i < n; i++) {
				if (!live[i].revents) continue;
				int k = map[i];
				done[k] = true;
				pending--;
				bool ok = !(live[i].revents & (POLLERR | POLLNVAL)) && connected(fds[k].fd) > 0;
				if (ok && found < max) {
					struct in_addr a = {htonl(ips[k])};
					snprintf(hits[found].host, sizeof hits[found].host, "%s", inet_ntoa(a));
					hits[found++].port = port;
				}
			}
		}
		for (int i = 0; i < count; i++) close(fds[i].fd);
		*progress = start + SCAN_BATCH - 1 > 254 ? 254 : start + SCAN_BATCH - 1;
	}
	*progress = 254;
	return found;
}
