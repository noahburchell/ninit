#include "parser.h"
#include "../util.h"
#include "../../src/ngraph.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
		*--e = '\0';
	return s;
}

static void split_into(struct strv *out, char *val, int comma)
{
	char *p = val;

	while (*p) {
		char *start;

		while (*p == ' ' || *p == '\t' || (comma && *p == ','))
			p++;
		if (!*p)
			break;
		start = p;
		while (*p && *p != ' ' && *p != '\t' && !(comma && *p == ','))
			p++;
		if (*p)
			*p++ = '\0';
		strv_push(out, start);
	}
}

static uint32_t parse_ms(const char *val, const char *fname, const char *key)
{
	char *end;
	unsigned long long v, mul = 1;

	errno = 0;
	v = strtoull(val, &end, 10);
	if (*val == '-' || errno || end == val)
		die("%s/%s: %s:%s is not a duration", g_dir, fname, key, val);
	if (!strcmp(end, "ms") || !*end)
		mul = 1;
	else if (!strcmp(end, "s"))
		mul = 1000;
	else if (!strcmp(end, "m"))
		mul = 60000;
	else if (!strcmp(end, "h"))
		mul = 3600000;
	else
		die("%s/%s: %s:%s has an unknown unit '%s', expected ms, s, m or h",
		    g_dir, fname, key, val, end);

	if (v > NG_MAX_MS / mul)
		die("%s/%s: %s:%s exceeds the %u ms maximum",
		    g_dir, fname, key, val, NG_MAX_MS);
	v *= mul;
	if (!v)
		die("%s/%s: %s:%s must not be zero", g_dir, fname, key, val);

	return (uint32_t)v;
}

static int has_code(const char *buf)
{
	const char *p = buf;

	while (*p) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p && *p != '#' && *p != '\n' && *p != '\r')
			return 1;
		p = strchr(p, '\n');
		if (!p)
			break;
		p++;
	}
	return 0;
}

static const char *word_at(const char **p, const char *end, size_t *len)
{
	const char *w;

	while (*p < end && (**p == ' ' || **p == '\t' || **p == '\r'))
		(*p)++;
	w = *p;
	while (*p < end && **p != ' ' && **p != '\t' && **p != '\r')
		(*p)++;
	*len = (size_t)(*p - w);
	return *len ? w : NULL;
}

static const char *base_of(const char *w, size_t *len)
{
	const char *b = w;

	for (size_t k = 0; k < *len; k++)
		if (w[k] == '/')
			b = w + k + 1;
	*len -= (size_t)(b - w);
	return b;
}

// the basename of the interpreter a #! line names, through env and its options
static const char *shebang_interp(const char *line, size_t n, size_t *len)
{
	const char *p = line + 2, *end = line + n, *w;
	size_t wl;

	w = word_at(&p, end, &wl);
	if (!w)
		return NULL;
	w = base_of(w, &wl);
	if (wl == 3 && !memcmp(w, "env", 3)) {
		while ((w = word_at(&p, end, &wl))) {
			if (wl > 2 && w[0] == '-' && w[1] == 'S') {
				w += 2;
				wl -= 2;
				break;
			}
			if (w[0] == '-') {
				if (wl == 2 && (w[1] == 'u' || w[1] == 'C'))
					word_at(&p, end, &wl);
				continue;
			}
			if (!memchr(w, '=', wl))
				break;
		}
		if (!w)
			return NULL;
		w = base_of(w, &wl);
	}
	*len = wl;
	return wl ? w : NULL;
}

static int is_name(const char *w, size_t len, const char *path)
{
	const char *b = strrchr(path, '/');

	b = b ? b + 1 : path;
	return strlen(b) == len && !memcmp(w, b, len);
}

static void check_shebang(const char *fname, const char *body)
{
	const char *nl = strchr(body, '\n'), *w;
	size_t n = nl ? (size_t)(nl - body) : strlen(body), len;

	w = shebang_interp(body, n, &len);
	if (!w || is_name(w, len, NG_SHELL) || is_name(w, len, NG_SHELL_ARGV0) ||
	    (g_sh_lexed && is_name(w, len, "sh")))
		return;
	while (n && (body[n - 1] == ' ' || body[n - 1] == '\t' || body[n - 1] == '\r'))
		n--;
	fprintf(stderr, "ninitctl: warning: %s/%s: '%.*s' is ignored, the script runs under %s\n",
		g_dir, fname, (int)n, body, NG_SHELL);
}

// bash line numbers must match the file
void parse_src(struct src *s, const char *fname, char *body, size_t len)
{
	char *scratch = xmalloc(len + 1);
	char *line = scratch;
	int code;

	memcpy(scratch, body, len + 1);

	s->name = strdup(fname);
	if (!s->name)
		die("out of memory");
	s->type = NG_TYPE_TARGET;
	if (body[0] == '#' && body[1] == '!')
		check_shebang(fname, body);

	while (line && *line) {
		char *nl = strchr(line, '\n');
		char *colon, *key, *val;

		if (nl)
			*nl = '\0';

		key = trim(line);
		if (*key && *key != '#') {
			if (nl)
				*nl = '\n';
			break;
		}
		line = nl ? nl + 1 : NULL;
		if (key[0] != '#' || key[1] != '%')
			continue;
		key = trim(key + 2);
		if (!*key)
			continue;

		colon = strchr(key, ':');
		if (!colon)
			die("%s/%s: directive without a key: #%%%s", g_dir, fname, key);
		*colon = '\0';
		val = trim(colon + 1);
		key = trim(key);

		if (!strcmp(key, "name")) {
			if (strcmp(val, fname))
				die("%s/%s: name:%s does not match its filename",
				    g_dir, fname, val);
		} else if (!strcmp(key, "depon")) {
			split_into(&s->depon, val, 1);
		} else if (!strcmp(key, "depof")) {
			split_into(&s->depof, val, 1);
		} else if (!strcmp(key, "onfail")) {
			if (!strcmp(val, "warn"))
				s->onfail = NG_ONFAIL_WARN;
			else if (!strcmp(val, "stop"))
				s->onfail = NG_ONFAIL_STOP;
			else if (!strcmp(val, "shell"))
				s->onfail = NG_ONFAIL_SHELL;
			else
				die("%s/%s: unknown onfail:%s, expected warn, stop or shell",
				    g_dir, fname, val);
			s->have_onfail = 1;
		} else if (!strcmp(key, "restart")) {
			if (!strcmp(val, "always"))
				s->restart = 1;
			else if (!strcmp(val, "no") || !strcmp(val, "never"))
				s->restart = 0;
			else
				die("%s/%s: unknown restart:%s, expected always or no",
				    g_dir, fname, val);
		} else if (!strcmp(key, "notify")) {
			char *end;
			unsigned long fd;

			errno = 0;
			fd = strtoul(val, &end, 10);
			if (*val == '-' || errno || end == val || *end ||
			    fd < NG_NOTIFY_MIN || fd > NG_NOTIFY_MAX)
				die("%s/%s: notify:%s must be between %u and %u",
				    g_dir, fname, val, NG_NOTIFY_MIN, NG_NOTIFY_MAX);
			s->notify = (uint16_t)fd;
		} else if (!strcmp(key, "start-timeout")) {
			s->start_ms = parse_ms(val, fname, key);
		} else if (!strcmp(key, "stop-timeout")) {
			s->stop_ms = parse_ms(val, fname, key);
		} else if (!strcmp(key, "start-delay")) {
			uint32_t d = parse_ms(val, fname, key);

			if (d > UINT16_MAX)
				die("%s/%s: start-delay:%s exceeds the %u ms maximum",
				    g_dir, fname, val, UINT16_MAX);
			s->retry_ms = (uint16_t)d;
		} else if (!strcmp(key, "start-tries")) {
			char *end;
			unsigned long t;

			errno = 0;
			t = strtoul(val, &end, 10);
			if (*val == '-' || errno || end == val || *end ||
			    t < 1 || t > NG_MAX_TRIES)
				die("%s/%s: start-tries:%s must be between 1 and %u",
				    g_dir, fname, val, NG_MAX_TRIES);
			s->start_tries = (uint8_t)t;
		} else if (!strcmp(key, "deps")) {
			if (!strcmp(val, "uptime"))
				s->pflags &= (uint8_t)~NG_PF_ORDER_ONLY;
			else if (!strcmp(val, "order"))
				s->pflags |= NG_PF_ORDER_ONLY;
			else
				die("%s/%s: unknown deps:%s, expected uptime or order",
				    g_dir, fname, val);
		} else if (!strcmp(key, "type")) {
			if (!strcmp(val, "oneshot"))
				s->type = NG_TYPE_ONESHOT;
			else if (!strcmp(val, "daemon"))
				s->type = NG_TYPE_DAEMON;
			else if (!strcmp(val, "target"))
				s->type = NG_TYPE_TARGET;
			else
				die("%s/%s: unknown type:%s", g_dir, fname, val);
			s->have_type = 1;
		} else {
			die("%s/%s: unknown directive '%s'", g_dir, fname, key);
		}
	}

	if (strlen(body) != len)
		die("%s/%s: contains a NUL byte", g_dir, fname);

	code = has_code(body);
	if (!s->have_type)
		s->type = code ? NG_TYPE_ONESHOT : NG_TYPE_TARGET;

	if (s->notify && s->type != NG_TYPE_DAEMON)
		die("%s/%s: notify: requires type:daemon, not type:%s",
		    g_dir, fname, ng_typename(s->type));

	if (s->restart && s->type != NG_TYPE_DAEMON)
		die("%s/%s: restart: requires type:daemon, not type:%s",
		    g_dir, fname, ng_typename(s->type));

	if (s->type == NG_TYPE_TARGET) {
		if (code)
			die("%s/%s: type:target must contain no commands", g_dir, fname);
	} else {
		if (!code)
			die("%s/%s: type:%s has no commands to run",
			    g_dir, fname, ng_typename(s->type));
		if (g_sh_lexed)
			len = compact(body, len, line ? (size_t)(line - scratch) : len, fname);
		if (len > NG_MAX_SCRIPT)
			die("%s/%s: script is %zu bytes%s, the maximum is %u", g_dir, fname, len,
			    g_sh_lexed ? " without comments" : "", NG_MAX_SCRIPT);
		s->script = body;
	}
}
