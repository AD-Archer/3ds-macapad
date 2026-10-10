#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0 && errno != EINPROGRESS && errno != EALREADY) {
		close(fd);
		return -1;
	}
	return fd;
}

static bool connected(int fd) {
	int err = 0;
	socklen_t len = sizeof err;
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) return true; // can't tell; the send will
	return err == 0;
}

static int connect_timeout(const char *host, int port, int timeout_ms) {
	struct in_addr addr;
	if (!resolve(host, &addr)) return -1;
	int fd = start_connect(addr, port);
	if (fd < 0) return -1;
	struct pollfd p = {fd, POLLOUT, 0};
	if (poll(&p, 1, timeout_ms) <= 0 || (p.revents & (POLLERR | POLLHUP)) || !(p.revents & POLLOUT) || !connected(fd)) {
		close(fd);
		return -1;
	}
	return fd;
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

bool http_request(const char *method, const char *host, int port, const char *path,
                  const char *token, const char *body, HttpResponse *out, int timeout_ms) {
	memset(out, 0, sizeof *out);
	int fd = connect_timeout(host, port, timeout_ms);
	if (fd < 0) return false;

	char head[768];
	int n = snprintf(head, sizeof head,
		"%s %s HTTP/1.1\r\nHost: %s:%d\r\nUser-Agent: Macapad3DS/1.0\r\nAccept: */*\r\n"
		"Connection: close\r\nX-Macapad-Client: " CLIENT_NAME "\r\n",
		method, path, host, port);
	if (token && *token) n += snprintf(head + n, sizeof head - n, "Authorization: Bearer %s\r\n", token);
	if (body) n += snprintf(head + n, sizeof head - n, "Content-Type: application/json\r\nContent-Length: %d\r\n", (int)strlen(body));
	n += snprintf(head + n, sizeof head - n, "\r\n");

	if (!send_all(fd, head, n, timeout_ms) || (body && !send_all(fd, body, strlen(body), timeout_ms))) {
		close(fd);
		return false;
	}

	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	for (;;) {
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
		if (r <= 0) break;
		len += r;
	}
	close(fd);
	buf[len] = 0;

	char *split = strstr(buf, "\r\n\r\n");
	if (len < 12 || strncmp(buf, "HTTP/", 5) != 0 || !split) {
		free(buf);
		return false;
	}
	out->status = atoi(strchr(buf, ' ') + 1);
	char *bodyStart = split + 4;
	size_t bodyLen = len - (bodyStart - buf);
	const char *te = header(buf, split, "Transfer-Encoding");
	if (te && strncasecmp(te, "chunked", 7) == 0) bodyLen = unchunk(bodyStart, bodyLen);
	memmove(buf, bodyStart, bodyLen);
	buf[bodyLen] = 0;
	out->body = buf;
	out->len = bodyLen;
	return true;
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
				bool ok = (live[i].revents & POLLOUT) && !(live[i].revents & (POLLERR | POLLHUP)) && connected(fds[k].fd);
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
