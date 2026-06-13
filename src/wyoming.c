/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* wyoming.c — Wyoming-protocol client framing (see wyoming.h). */
#include "wyoming.h"
#include "net.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

/* Append printf-formatted text to buf at *pos, growing *pos by the bytes
 * written. Returns 0 on success, -1 on truncation or error. snprintf returns
 * "the number of bytes that would be written" (excluding NUL), which can
 * exceed the buffer space -- adding that directly to a running offset risks
 * the next sizeof(buf) - pos underflowing to a huge size_t. This helper
 * pulls all of that bookkeeping into one place so callers can compose JSON
 * without arithmetic landmines. */
static int buf_appendf(char *buf, size_t buflen, size_t *pos,
                       const char *fmt, ...)
{
    va_list ap;
    int n;
    if (*pos >= buflen) return -1;
    va_start(ap, fmt);
    n = vsnprintf(buf + *pos, buflen - *pos, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n >= buflen - *pos) return -1;        /* truncated */
    *pos += (size_t)n;
    return 0;
}

/* ---- tiny, allocation-free helpers over a single JSON header line ---- *
 * These are intentionally minimal: they scan for "key" and read the value
 * that follows ':'. Good enough for Piper's flat TTS events; not a general
 * JSON parser. Returns 1 if found, 0 otherwise.                          */

static const char *find_key(const char *s, const char *key)
{
    size_t klen = strlen(key);
    const char *p = s;
    while ((p = strchr(p, '"')) != NULL) {
        if (strncmp(p + 1, key, klen) == 0 && p[1 + klen] == '"') {
            const char *c = p + 1 + klen + 1;
            while (*c == ' ' || *c == '\t') c++;
            if (*c == ':') {
                c++;
                while (*c == ' ' || *c == '\t') c++;
                return c;
            }
        }
        p++;
    }
    return NULL;
}

static int json_find_int(const char *s, const char *key, long *out)
{
    const char *v = find_key(s, key);
    if (!v) return 0;
    if (*v == 'n') return 0;            /* null */
    *out = strtol(v, NULL, 10);
    return 1;
}

static int json_find_str(const char *s, const char *key, char *buf, size_t buflen)
{
    const char *v = find_key(s, key);
    size_t i = 0;
    if (!v || *v != '"') return 0;
    v++;
    while (*v && *v != '"' && i + 1 < buflen) {
        if (*v == '\\' && v[1]) v++;    /* crude unescape: copy next literally */
        buf[i++] = *v++;
    }
    buf[i] = '\0';
    return 1;
}

/* ---- JSON string escaping for the text we send ---- */

static void json_escape(const char *in, char *out, size_t outlen)
{
    size_t o = 0;
    for (; *in && o + 2 < outlen; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            out[o++] = '\\'; out[o++] = (char)c;
        } else if (c == '\n') {
            out[o++] = '\\'; out[o++] = 'n';
        } else if (c == '\r') {
            out[o++] = '\\'; out[o++] = 'r';
        } else if (c == '\t') {
            out[o++] = '\\'; out[o++] = 't';
        } else if (c < 0x20) {
            if (o + 6 >= outlen) break;
            if (buf_appendf(out, outlen, &o, "\\u%04x", c) < 0) break;
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

/* Write all bytes, looping over short writes. */
static int write_all(int sock, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        long n = net_write(sock, p, len);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int wyo_send_synthesize(int sock, const char *text,
                        const char *voice, const char *language,
                        const char *speaker)
{
    char esc[2048];
    char line[4096];
    int n;

    json_escape(text ? text : "", esc, sizeof(esc));

    /* Build the voice sub-object only if any voice field is present. Voice
     * and language are caller-supplied strings and must be JSON-escaped
     * before substitution — otherwise a name containing a " or \ would
     * break the request. Speaker is documented as an integer, so it is
     * inserted unquoted and not escaped. */
    if ((voice && *voice) || (language && *language) || (speaker && *speaker)) {
        char vbuf[512];
        char escv[256];
        char escl[64];
        size_t o = 0;
        if (buf_appendf(vbuf, sizeof(vbuf), &o, ", \"voice\": {") < 0)
            return -1;
        {
            int first = 1;
            if (voice && *voice) {
                json_escape(voice, escv, sizeof(escv));
                if (buf_appendf(vbuf, sizeof(vbuf), &o,
                                "\"name\": \"%s\"", escv) < 0)
                    return -1;
                first = 0;
            }
            if (language && *language) {
                json_escape(language, escl, sizeof(escl));
                if (buf_appendf(vbuf, sizeof(vbuf), &o,
                                "%s\"language\": \"%s\"",
                                first ? "" : ", ", escl) < 0)
                    return -1;
                first = 0;
            }
            if (speaker && *speaker) {
                if (buf_appendf(vbuf, sizeof(vbuf), &o,
                                "%s\"speaker\": %s",
                                first ? "" : ", ", speaker) < 0)
                    return -1;
            }
        }
        if (buf_appendf(vbuf, sizeof(vbuf), &o, "}") < 0)
            return -1;
        n = snprintf(line, sizeof(line),
                     "{\"type\": \"synthesize\", \"data\": {\"text\": \"%s\"%s}}\n",
                     esc, vbuf);
    } else {
        n = snprintf(line, sizeof(line),
                     "{\"type\": \"synthesize\", \"data\": {\"text\": \"%s\"}}\n",
                     esc);
    }

    if (n <= 0 || (size_t)n >= sizeof(line))
        return -1;
    return write_all(sock, line, (size_t)n);
}

/* ---- connection-level read buffer ----
 * Headers are '\n'-delimited and binary payloads follow immediately, so a
 * single recv() routinely straddles a line boundary and the start of binary
 * data. Reading a byte at a time (one recv per byte) is murder on the emulated
 * Amiga bsdsocket -- thousands of library traps per response. Buffer instead:
 * one recv fills rbuf, and both the line reader and the payload reader drain
 * from it. Single connection at a time, so a static buffer is fine.
 * Call wyo_reset() at the start of each connection. */
static unsigned char rbuf[16384];
static size_t        rpos = 0, rlen = 0;

void wyo_reset(void)
{
    rpos = rlen = 0;
}

/* Ensure at least one byte is buffered. Returns >0 bytes available, 0 on clean
 * EOF, <0 on error. */
static long buf_fill(int sock)
{
    if (rpos < rlen)
        return (long)(rlen - rpos);
    rpos = 0;
    {
        long n = net_read(sock, rbuf, sizeof(rbuf));
        if (n <= 0) { rlen = 0; return n; }
        rlen = (size_t)n;
    }
    return (long)rlen;
}

/* Read one '\n'-terminated header line into buf (NUL-terminated, '\n' dropped).
 * Returns line length, 0 on clean EOF before any byte, <0 on error. */
static int read_line(int sock, char *buf, size_t buflen)
{
    size_t i = 0;
    while (i + 1 < buflen) {
        long avail = buf_fill(sock);
        if (avail == 0) return (i == 0) ? 0 : (int)i;
        if (avail < 0)  return -1;
        {
            unsigned char c = rbuf[rpos++];
            if (c == '\n') break;
            buf[i++] = (char)c;
        }
    }
    buf[i] = '\0';
    return (int)i;
}

/* Read exactly len bytes (from the buffer, refilling as needed).
 * Returns 0 on success, <0 otherwise. */
static int read_exact(int sock, void *buf, long len)
{
    char *p = (char *)buf;
    while (len > 0) {
        long avail = buf_fill(sock);
        size_t take;
        if (avail <= 0) return -1;
        take = (size_t)avail < (size_t)len ? (size_t)avail : (size_t)len;
        memcpy(p, rbuf + rpos, take);
        rpos += take;
        p += take;
        len -= (long)take;
    }
    return 0;
}

int wyo_read_event(int sock, WyoEvent *ev)
{
    char header[1024];
    long data_length = -1;
    int  ln;

    memset(ev, 0, sizeof(*ev));
    ev->payload_length = -1;

    ln = read_line(sock, header, sizeof(header));
    if (ln == 0)  return 0;   /* clean close */
    if (ln < 0)   return -1;

    json_find_str(header, "type", ev->type, sizeof(ev->type));
    json_find_int(header, "payload_length", &ev->payload_length);
    {
        long tmp;
        if (json_find_int(header, "rate", &tmp))     ev->rate = (int)tmp;
        if (json_find_int(header, "width", &tmp))    ev->width = (int)tmp;
        if (json_find_int(header, "channels", &tmp)) ev->channels = (int)tmp;
    }

    /* If the event carries a separate data block, read it and mine the same
     * fields from it (Piper usually inlines "data", but the spec allows this). */
    if (json_find_int(header, "data_length", &data_length) && data_length > 0) {
        char *data = (char *)malloc((size_t)data_length + 1);
        if (!data) return -1;
        if (read_exact(sock, data, data_length) != 0) { free(data); return -1; }
        data[data_length] = '\0';
        {
            long tmp;
            if (!ev->type[0]) json_find_str(data, "type", ev->type, sizeof(ev->type));
            if (json_find_int(data, "rate", &tmp))     ev->rate = (int)tmp;
            if (json_find_int(data, "width", &tmp))    ev->width = (int)tmp;
            if (json_find_int(data, "channels", &tmp)) ev->channels = (int)tmp;
            if (ev->payload_length < 0 &&
                json_find_int(data, "payload_length", &tmp))
                ev->payload_length = tmp;
        }
        free(data);
    }

    return 1;
}

int wyo_read_payload(int sock, void *buf, long len)
{
    if (len <= 0) return 0;
    return read_exact(sock, buf, len);
}
