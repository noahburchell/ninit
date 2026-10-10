#include "control.h"
#include "command.h"
#include "fail.h"
#include "logging.h"
#include "nctl.h"
#include "ngraph.h"
#include "ninit.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

int ctl_lfd = -1;
uint32_t n_ops;
struct ctl ctl_conn[CTL_MAX];
int n_ctl;

const char *state_name(uint32_t i)
{
	switch (state[i]) {
	case NG_ST_PENDING:	return "pending";
	case NG_ST_RUNNING:	return "starting";
	case NG_ST_DONE:	return up[i] ? "up" : "down";
	case NG_ST_FAILED:	return "failed";
	case NG_ST_SKIPPED:	return "skipped";
	default:		return "?";
	}
}

void ctl_init(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd;

	static_assert(sizeof(NINIT_CTL_SOCK) <= sizeof(sa.sun_path),
		      "the control socket path does not fit in sockaddr_un");

	if (mkdir(NINIT_CTL_DIR, 0700) < 0 && errno != EEXIST) {
		log_warn("control: mkdir %s: %s", NINIT_CTL_DIR, strerror(errno));
		return;
	}
	if (chmod(NINIT_CTL_DIR, 0700) < 0)
		log_warn("control: chmod %s: %s", NINIT_CTL_DIR, strerror(errno));

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		log_warn("control: socket: %s", strerror(errno));
		return;
	}
	memcpy(sa.sun_path, NINIT_CTL_SOCK, sizeof(NINIT_CTL_SOCK));
	unlink(NINIT_CTL_SOCK);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, CTL_MAX) < 0) {
		log_warn("control: bind %s: %s", NINIT_CTL_SOCK, strerror(errno));
		close(fd);
		return;
	}
	if (chmod(NINIT_CTL_SOCK, 0600) < 0)
		log_warn("control: chmod %s: %s", NINIT_CTL_SOCK, strerror(errno));
	ctl_lfd = fd;
	log_note("control: listening on %s", NINIT_CTL_SOCK);
}

void ctl_out(struct ctl *c, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (c->done || c->out_len >= CTL_OUT - 128)
		return;
	va_start(ap, fmt);
	n = vsnprintf(c->out + c->out_len, CTL_OUT - c->out_len, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n >= (size_t)(CTL_OUT - c->out_len))
		n = CTL_OUT - c->out_len - 1;
	c->out_len += (uint16_t)n;
}

void ctl_end(struct ctl *c, int ok, const char *fmt, ...)
{
	char buf[CTL_BUF];
	va_list ap;

	if (c->done)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	c->listing = 0;
	c->svc = UINT32_MAX;
	ctl_out(c, "%s%s\n", ok ? NCTL_OK : NCTL_ERR, buf);
	c->done = 1;
}

static void ctl_drop(struct ctl *c)
{
	close(c->fd);
	*c = ctl_conn[--n_ctl];
}

static void ctl_fill(struct ctl *c)
{
	while (c->listing && c->out_len < CTL_OUT - 256) {
		uint32_t i = c->list_at;

		if (c->listing == 2) {
			size_t n;

			if (c->out_len > CTL_OUT - LOG_LINE - NCTL_TAG_LEN)
				return;
			n = log_line(&c->log_at, c->out + c->out_len + NCTL_TAG_LEN);
			if (!n) {
				if (!c->watch) {
					c->listing = 0;
					ctl_end(c, 1, "end of log");
				}
				return;
			}
			memcpy(c->out + c->out_len, NCTL_DATA, NCTL_TAG_LEN);
			c->out_len += (uint16_t)(n + NCTL_TAG_LEN);
			continue;
		}
		if (i >= n_svc) {
			c->listing = 0;
			ctl_end(c, 1, "%u services, %u up", n_svc, n_up);
			return;
		}
		c->list_at++;
		ctl_out(c, NCTL_DATA "%s %s %s pid %d\n", ng_name(map, i), state_name(i),
			want[i] ? "stopped" : "wanted", (int)runs[i].pid);
	}
}

// 0 while output is still pending, 1 once it is all written, -1 if the peer is gone
static int ctl_flush(struct ctl *c)
{
	while (c->out_at < c->out_len) {
		ssize_t k = write(c->fd, c->out + c->out_at,
				  (size_t)(c->out_len - c->out_at));

		if (k > 0) {
			c->out_at += (uint16_t)k;
			continue;
		}
		if (k < 0 && errno == EINTR)
			continue;
		if (k < 0 && errno == EAGAIN)
			return 0;
		return -1;
	}
	c->out_at = c->out_len = 0;
	return 1;
}

// a burst can outrun the poll loop, so a watcher half a ring behind is written now,
// a full socket is left to the poll loop and nothing is dropped here
void ctl_feed(void)
{
	if (shutting_down)
		return;
	for (int k = 0; k < n_ctl; k++) {
		struct ctl *c = &ctl_conn[k];

		if (!c->watch || c->out_at < c->out_len || log_end() - c->log_at < LOG_RING / 2)
			continue;
		do
			ctl_fill(c);
		while (ctl_flush(c) == 1 && c->log_at < log_end());
	}
}

void ctl_pump(struct ctl *c)
{
	int rc;

	ctl_fill(c);
	rc = ctl_flush(c);
	if (!rc)
		return;
	if (rc < 0 || c->done)
		ctl_drop(c);
}

void ctl_read(struct ctl *c)
{
	char *nl;
	ssize_t k;

	if (c->done || c->listing || c->svc != UINT32_MAX) {
		char skip[256];

		k = read(c->fd, skip, sizeof(skip));
		if (k == 0 || (k < 0 && errno != EAGAIN && errno != EINTR))
			ctl_drop(c);
		return;
	}

	k = read(c->fd, c->in + c->in_len, sizeof(c->in) - c->in_len - 1);
	if (k == 0) {
		ctl_drop(c);
		return;
	}
	if (k < 0) {
		if (errno == EAGAIN || errno == EINTR)
			return;
		ctl_drop(c);
		return;
	}
	c->in_len += (uint16_t)k;
	c->in[c->in_len] = '\0';

	nl = memchr(c->in, '\n', c->in_len);
	if (!nl) {
		if (c->in_len >= sizeof(c->in) - 1) {
			ctl_end(c, 0, "request too long");
			ctl_pump(c);
		}
		return;
	}
	*nl = '\0';
	c->in[strcspn(c->in, "\r")] = '\0';
	ctl_cmd(c, c->in);
	c->in_len = 0;
	ctl_pump(c);
}

void ctl_accept(void)
{
	static const char busy[] = NCTL_ERR "too many control connections\n";

	for (;;) {
		int fd = accept4(ctl_lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
		struct ctl *c;

		if (fd < 0)
			return;
		if (n_ctl == CTL_MAX) {
			(void)!write(fd, busy, sizeof(busy) - 1);
			close(fd);
			continue;
		}
		c = &ctl_conn[n_ctl++];
		memset(c, 0, sizeof(*c));
		c->fd = fd;
		c->svc = UINT32_MAX;
	}
}

long long ctl_due(void)
{
	long long best = -1, now, d;
	uint32_t k;

	if (!n_ops)
		return -1;
	now = now_ms();
	for (k = 0; k < n_live; k++) {
		const struct run *r = &runs[live[k]];

		if (r->op == SVC_OP_NONE)
			continue;
		d = r->op_at - now;
		if (d < 0)
			d = 0;
		if (best < 0 || d < best)
			best = d;
	}
	return best;
}
