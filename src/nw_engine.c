/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* nw_engine.c — freestanding Wyoming + AHI engine for narrator.device.
 *
 * Connects to Piper, sends a `synthesize`, streams the returned PCM straight
 * into AHI (v4, double-buffered) as it arrives. Exec + bsdsocket + ahi.device
 * only; NO C runtime, NO stdio, NO malloc, and NO C globals/statics — every
 * library base is a local fed from the passed-in SysBase / the allocated
 * context (see CLAUDE.md "Device build" for why globals corrupt memory here).
 */
#ifdef PLATFORM_AMIGA

#include "nw_engine.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <bsdsocket/socketbasetags.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <exec/tasks.h>
#include <devices/ahi.h>
#include <devices/narrator.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>

#include <proto/exec.h>
#include <proto/bsdsocket.h>
#include <proto/dos.h>

/* Optional: codesets.library for non-UTF-8 text input (ISO-8859-1 etc.).
 * Opened soft — if the library isn't installed, the engine falls back to
 * passing text through unchanged (today's behaviour). Headers are vendored
 * from upstream jens-maus/libcodesets under third_party/codesets/include/
 * (see that directory's README + LICENSE). */
#define __NOLIBBASE__               /* suppress upstream's extern CodesetsBase */
#include <libraries/codesets.h>
#include <proto/codesets.h>

#define RBUFSZ   16384
/* Two ping-ponging AHI buffers, chained with ahir_Link for gapless playback
 * (AHI plays linked CMD_WRITEs sequentially; without the link it plays them
 * concurrently). Both are primed before playback starts so the first transition
 * is gapless. ~0.19s/buffer at 8K — small for low start latency. */
#define AHIBUFSZ 8192

/* Max passes of the high-cut averager (smooth_buf); each pass is one short of
 * persistent state. More than a few would over-dull speech anyway. */
#define SMOOTH_MAX 4

/* Graceful-failure timeouts (seconds): an unreachable server fails fast instead
 * of hanging on the OS TCP timeout, and a mid-stream stall doesn't block forever. */
#define NW_CONNECT_TIMEOUT 5
#define NW_RECV_TIMEOUT    20
#ifndef FIONBIO
#define FIONBIO 0x8004667EUL    /* _IOW('f', 126, int) */
#endif
#ifndef EINPROGRESS
#define EINPROGRESS 36
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK 35
#endif

/* Compiled-in fallback defaults; the prefs file (ENV:narrator.wyoming) overrides
 * any of these. Host defaults to localhost (no LAN IP baked in). The voice
 * defaults are common Piper voices — they must exist on your server, else set
 * them in the prefs (or leave empty for the server's own default voice). */
#ifndef NW_DEFAULT_HOST
#define NW_DEFAULT_HOST "127.0.0.1"
#endif
#ifndef NW_DEFAULT_PORT
#define NW_DEFAULT_PORT 10200
#endif
#ifndef NW_DEFAULT_VOICE
#define NW_DEFAULT_VOICE "en_US-lessac-medium"
#endif
#ifndef NW_DEFAULT_VOICE_MALE
/* MALE is Say's default (DEFSEX), so this is the everyday voice — use one that
 * articulates final consonants cleanly. ryan-high clips short words ("test" ->
 * "tess"); lessac doesn't. */
#define NW_DEFAULT_VOICE_MALE "en_US-lessac-medium"
#endif
#ifndef NW_DEFAULT_VOICE_FEMALE
#define NW_DEFAULT_VOICE_FEMALE "en_US-amy-medium"
#endif
#define NW_PREFS_PATH "ENV:narrator.wyoming"

struct NW {
    struct ExecBase *SysBase;
    struct Library  *SocketBase;
    int   sock;
    int   err;                  /* bsdsocket errno target (per context) */
    long  rpos, rlen;           /* socket read buffer cursor            */

    /* AHI playback state (two ping-ponging buffers) */
    struct MsgPort    *ahiPort;
    struct AHIRequest *ahiReq[2];
    unsigned char     *ahiBuf[2];
    int   ahiOpened;
    int   ahiQueued[2];
    int   fill;                 /* buffer being filled */
    long  fillpos;
    int   primed;               /* both buffers submitted once (gapless start) */
    int   prerolled;            /* silence pre-roll injected this utterance     */
    unsigned long rate;
    int   ahiType;              /* AHIST_M16S / AHIST_S16S */
    long  volume;               /* AHI ahir_Volume (Fixed 16.16) */
    long  ahiUnit;              /* ahi.device unit (configurable; default 0)    */
    long  ahiUnitOpen;          /* unit AHI is currently open on (-1 if closed) */
    int   smooth;               /* high-cut passes (0=off); see smooth_buf       */
    short smz[SMOOTH_MAX];      /* per-pass previous-sample state for the filter  */

    /* PCM capture: optional WAV tee of what we feed AHI (post-smooth, pre-swap)
     * to the path in prefs.capture. Opened lazily on first ahi_submit; sizes are
     * patched at session_close. DOSBase is open whenever captureFh != 0. */
    struct DosLibrary *captureDOSBase;
    BPTR  captureFh;            /* 0 = not open / disabled                       */
    long  captureBytes;         /* data-chunk bytes written so far                */

    /* persistent-connection bookkeeping (the held endpoint) */
    char  chost[128];
    int   cport;

    /* Optional codesets.library state. NULL when the library isn't installed;
     * cs_iso/cs_utf may also be NULL if the named codesets aren't registered,
     * in which case transcoding is skipped (pass-through). */
    struct Library *CodesetsBase;
    struct codeset *cs_iso;       /* ISO-8859-1 source */
    struct codeset *cs_utf;       /* UTF-8 dest */

    /* Runtime prefs, snapshotted at task start (see nw_dev_task). Caching
     * here means each CMD_WRITE doesn't re-open dos.library + reparse
     * ENV:narrator.wyoming. The trade-off: prefs edits don't take effect
     * until the device is reopened. For interactive `Say`, each invocation
     * opens a fresh device anyway, so live edits keep working in practice;
     * a long-running consumer that holds the device open across edits will
     * keep using the values it loaded at OpenDevice. */
    struct nwprefs prefs;

    unsigned char rbuf[RBUFSZ];
};

/* ---- tiny freestanding helpers (no libc) ---- */

static long nw_strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (long)(p - s);
}

static const char *find_val(const char *s, const char *key)
{
    long klen = nw_strlen(key);
    const char *p = s;
    for (; *p; p++) {
        if (*p != '"') continue;
        {
            long i = 0;
            while (i < klen && p[1 + i] == key[i]) i++;
            if (i == klen && p[1 + klen] == '"') {
                const char *c = p + 1 + klen + 1;
                while (*c == ' ' || *c == '\t' || *c == ':') c++;
                return c;
            }
        }
    }
    return 0;
}

static int find_int(const char *s, const char *key, long *out)
{
    const char *v = find_val(s, key);
    long n = 0; int neg = 0, any = 0;
    if (!v) return 0;
    if (*v == 'n') return 0;
    if (*v == '-') { neg = 1; v++; }
    while (*v >= '0' && *v <= '9') { n = n * 10 + (*v - '0'); v++; any = 1; }
    if (!any) return 0;
    *out = neg ? -n : n;
    return 1;
}

static int contains(const char *s, const char *sub)
{
    long sl = nw_strlen(sub);
    const char *p = s;
    for (; *p; p++) {
        long i = 0;
        while (i < sl && p[i] == sub[i]) i++;
        if (i == sl) return 1;
    }
    return 0;
}

static void copy_str(char *dst, long dstlen, const char *src)
{
    long i = 0;
    while (src[i] && i < dstlen - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* Find a "key value" line (key at line start, then whitespace) in NUL-terminated
 * `buf` and copy the value token into out. Leaves out unchanged if not found.
 *
 * The mandatory `p[klen] == ' ' || '\t'` check after the key match is what
 * makes call order at the caller irrelevant: a query for `voice` against a
 * `voice_male foo` line fails (next char is `_`, not whitespace), so prefix
 * keys never collide with longer ones. Don't drop that check. */
static void pref_get(const char *buf, const char *key, char *out, long outlen)
{
    long klen = nw_strlen(key);
    const char *p = buf;
    while (*p) {
        long i = 0;
        while (i < klen && p[i] == key[i]) i++;
        if (i == klen && (p[klen] == ' ' || p[klen] == '\t')) {
            const char *v = p + klen;
            long o = 0;
            while (*v == ' ' || *v == '\t') v++;
            while (*v && *v != ' ' && *v != '\t' && *v != '\n' && *v != '\r'
                   && o < outlen - 1)
                out[o++] = *v++;
            out[o] = '\0';
            return;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
}

void nw_read_prefs(struct ExecBase *sysbase, struct nwprefs *pr)
{
    struct ExecBase   *SysBase = sysbase;
    struct DosLibrary *DOSBase;

    /* compiled defaults; the prefs file overrides any of these */
    copy_str(pr->host,         (long)sizeof(pr->host),         NW_DEFAULT_HOST);
    pr->port = NW_DEFAULT_PORT;
    copy_str(pr->voice,        (long)sizeof(pr->voice),        NW_DEFAULT_VOICE);
    copy_str(pr->voice_male,   (long)sizeof(pr->voice_male),   NW_DEFAULT_VOICE_MALE);
    copy_str(pr->voice_female, (long)sizeof(pr->voice_female), NW_DEFAULT_VOICE_FEMALE);
    pr->ahi_unit = 0;          /* AHI_DEFAULT_UNIT */
    pr->split_words = 0;       /* off by default = whole text in one request */
    pr->gain = 80;             /* % of full scale; <100 = headroom for hot peaks */
    pr->smooth = 2;            /* high-cut: tame Paula-chain sibilance (validated) */
    pr->capture[0] = '\0';     /* capture off unless user sets a path             */

    /* local named DOSBase so the proto/dos.h inlines (Open/Read/Close) use it */
    DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 0);
    if (!DOSBase) return;
    {
        BPTR fh = Open((STRPTR)NW_PREFS_PATH, MODE_OLDFILE);
        if (fh) {
            char *buf = (char *)AllocMem(2048, MEMF_PUBLIC);
            if (buf) {
                LONG n = Read(fh, buf, 2047);
                if (n > 0) {
                    char portstr[16], splitstr[16], unitstr[16], gainstr[16];
                    char smoothstr[16];
                    buf[n] = '\0';
                    portstr[0] = '\0'; splitstr[0] = '\0'; unitstr[0] = '\0';
                    gainstr[0] = '\0'; smoothstr[0] = '\0';
                    pref_get(buf, "host", pr->host, (long)sizeof(pr->host));
                    pref_get(buf, "port", portstr, (long)sizeof(portstr));
                    pref_get(buf, "voice", pr->voice, (long)sizeof(pr->voice));
                    pref_get(buf, "voice_male", pr->voice_male, (long)sizeof(pr->voice_male));
                    pref_get(buf, "voice_female", pr->voice_female, (long)sizeof(pr->voice_female));
                    pref_get(buf, "ahi_unit", unitstr, (long)sizeof(unitstr));
                    pref_get(buf, "split_words", splitstr, (long)sizeof(splitstr));
                    pref_get(buf, "gain", gainstr, (long)sizeof(gainstr));
                    pref_get(buf, "smooth", smoothstr, (long)sizeof(smoothstr));
                    pref_get(buf, "capture", pr->capture, (long)sizeof(pr->capture));
                    if (portstr[0]) {
                        long v = 0; const char *q = portstr;
                        while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; }
                        if (v > 0) pr->port = (int)v;
                    }
                    if (unitstr[0]) {
                        long v = 0; const char *q = unitstr;
                        while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; }
                        pr->ahi_unit = (int)v;           /* 0 = AHI_DEFAULT_UNIT */
                    }
                    if (splitstr[0]) {
                        long v = 0; const char *q = splitstr;
                        while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; }
                        pr->split_words = (int)v;        /* 0 stays off */
                    }
                    if (gainstr[0]) {
                        long v = 0; const char *q = gainstr;
                        while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; }
                        if (v < 1)   v = 1;              /* keep audible          */
                        if (v > 100) v = 100;            /* no boost above unity  */
                        pr->gain = (int)v;
                    }
                    if (smoothstr[0]) {
                        long v = 0; const char *q = smoothstr;
                        while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; }
                        pr->smooth = (int)v;             /* 0 = off; clamped at use */
                    }
                }
                FreeMem(buf, 2048);
            }
            Close(fh);
        }
    }
    CloseLibrary((struct Library *)DOSBase);
}

/* ---- AHI double-buffered playback (contextual port of audio_ahi.c) ---- */

/* Piper/Wyoming PCM is 16-bit signed LITTLE-endian; AHI (AHIST_M16S) wants the
 * Amiga's native BIG-endian order. Swap each sample in place before playback,
 * or it plays as static. Buffer boundaries are even (AHIBUFSZ even, totals
 * even), so samples never straddle a submitted buffer. */
static void swap16(unsigned char *b, long n)
{
    long i;
    for (i = 0; i + 1 < n; i += 2) {
        unsigned char t = b[i]; b[i] = b[i + 1]; b[i + 1] = t;
    }
}

/* Gentle high-cut applied to the still-little-endian 16-bit PCM before swap16.
 * A cascade of N one-tap averagers y[n] = (x[n] + x[n-1]) / 2: a null at Nyquist
 * (~-8 dB at 8 kHz, ~-0.7 dB at 2.7 kHz), so it tames the 8-11 kHz sibilance
 * that AHI's resample + 8-bit Paula make harsh, while leaving the speech body
 * intact. No coeffs, no trig, no float, and (a+b)>>1 of two int16s can't overflow
 * — friendly to the freestanding -nostdlib device. State persists across buffers
 * (submitted in playback order), so the filter is continuous within a session. */
static void smooth_buf(struct NW *nw, unsigned char *b, long bytes)
{
    int passes = nw->smooth, p;
    long i;
    if (passes <= 0) return;
    if (passes > SMOOTH_MAX) passes = SMOOTH_MAX;
    for (i = 0; i + 1 < bytes; i += 2) {
        int s = (int)(short)(b[i] | (b[i + 1] << 8));   /* decode LE signed 16 */
        for (p = 0; p < passes; p++) {
            int avg = (s + (int)nw->smz[p]) >> 1;       /* (x + x_prev) / 2     */
            nw->smz[p] = (short)s;                      /* this pass's input    */
            s = avg;                                    /* feeds the next pass  */
        }
        b[i]     = (unsigned char)(s & 0xff);           /* re-encode LE         */
        b[i + 1] = (unsigned char)((s >> 8) & 0xff);
    }
}

/* ---- optional WAV capture (tee of the PCM we hand AHI) ---- */

/* Little-endian writers for WAV header fields. */
static void put_u16_le(unsigned char *p, unsigned short v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
}
static void put_u32_le(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

/* Lazy capture file open + 44-byte WAV skeleton (sizes patched at finalize).
 * Best-effort: if dos.library or Open() fails, capture stays off silently. */
static void capture_open(struct NW *nw)
{
    struct DosLibrary *DOSBase;
    unsigned char hdr[44];
    unsigned long sr   = nw->rate;
    unsigned short ch  = (nw->ahiType == AHIST_S16S) ? 2 : 1;
    unsigned long brate = sr * ch * 2UL;

    if (!nw->prefs.capture[0]) return;          /* feature off */
    {
        struct ExecBase *SysBase = nw->SysBase;
        nw->captureDOSBase = (struct DosLibrary *)
            OpenLibrary((STRPTR)"dos.library", 0);
    }
    if (!nw->captureDOSBase) return;
    DOSBase = nw->captureDOSBase;
    nw->captureFh = Open((STRPTR)nw->prefs.capture, MODE_NEWFILE);
    if (!nw->captureFh) {
        struct ExecBase *SysBase = nw->SysBase;
        CloseLibrary((struct Library *)nw->captureDOSBase);
        nw->captureDOSBase = 0;
        return;
    }
    /* RIFF header with placeholder sizes (patched in capture_finalize). */
    hdr[0]='R'; hdr[1]='I'; hdr[2]='F'; hdr[3]='F';
    put_u32_le(hdr + 4, 36);                    /* placeholder: 36 + data */
    hdr[8]='W'; hdr[9]='A'; hdr[10]='V'; hdr[11]='E';
    hdr[12]='f'; hdr[13]='m'; hdr[14]='t'; hdr[15]=' ';
    put_u32_le(hdr + 16, 16);                   /* fmt chunk size */
    put_u16_le(hdr + 20, 1);                    /* PCM */
    put_u16_le(hdr + 22, ch);
    put_u32_le(hdr + 24, sr);
    put_u32_le(hdr + 28, brate);
    put_u16_le(hdr + 32, (unsigned short)(ch * 2));
    put_u16_le(hdr + 34, 16);
    hdr[36]='d'; hdr[37]='a'; hdr[38]='t'; hdr[39]='a';
    put_u32_le(hdr + 40, 0);                    /* placeholder */
    Write(nw->captureFh, hdr, 44);
    nw->captureBytes = 0;
}

/* Append `bytes` of LE PCM to the capture file; no-op if disabled. */
static void capture_write(struct NW *nw, const unsigned char *data, long bytes)
{
    struct DosLibrary *DOSBase;
    if (!nw->captureFh || bytes <= 0) return;
    DOSBase = nw->captureDOSBase;
    Write(nw->captureFh, (APTR)data, bytes);
    nw->captureBytes += bytes;
}

/* Patch the RIFF/data sizes and close. Safe to call when capture is off. */
static void capture_finalize(struct NW *nw)
{
    struct DosLibrary *DOSBase;
    unsigned char le[4];
    if (!nw->captureFh) {
        if (nw->captureDOSBase) {
            struct ExecBase *SysBase = nw->SysBase;
            CloseLibrary((struct Library *)nw->captureDOSBase);
            nw->captureDOSBase = 0;
        }
        return;
    }
    DOSBase = nw->captureDOSBase;
    Seek(nw->captureFh, 4, OFFSET_BEGINNING);
    put_u32_le(le, (unsigned long)(36 + nw->captureBytes));
    Write(nw->captureFh, le, 4);
    Seek(nw->captureFh, 40, OFFSET_BEGINNING);
    put_u32_le(le, (unsigned long)nw->captureBytes);
    Write(nw->captureFh, le, 4);
    Close(nw->captureFh);
    nw->captureFh = 0;
    {
        struct ExecBase *SysBase = nw->SysBase;
        CloseLibrary((struct Library *)nw->captureDOSBase);
    }
    nw->captureDOSBase = 0;
}

static void ahi_submit(struct NW *nw, int i, long bytes)
{
    struct ExecBase   *SysBase = nw->SysBase;
    struct AHIRequest *r = nw->ahiReq[i];
    smooth_buf(nw, nw->ahiBuf[i], bytes);  /* high-cut (LE) before the byte-swap */
    if (nw->prefs.capture[0] && !nw->captureFh && !nw->captureDOSBase)
        capture_open(nw);                  /* lazy: rate/channels known by now    */
    capture_write(nw, nw->ahiBuf[i], bytes); /* tee LE PCM (no-op if disabled)    */
    swap16(nw->ahiBuf[i], bytes);          /* little-endian PCM -> AHI big-endian */
    r->ahir_Std.io_Command = CMD_WRITE;
    r->ahir_Std.io_Data    = nw->ahiBuf[i];
    r->ahir_Std.io_Length  = (ULONG)bytes;
    r->ahir_Std.io_Offset  = 0;
    r->ahir_Frequency      = nw->rate;
    r->ahir_Type           = nw->ahiType;
    r->ahir_Volume         = (Fixed)nw->volume;
    r->ahir_Position       = 0x8000;
    r->ahir_Link           = nw->ahiQueued[1 - i] ? nw->ahiReq[1 - i] : 0;  /* gapless */
    SendIO((struct IORequest *)r);
    nw->ahiQueued[i] = 1;
}

static void ahi_wait_free(struct NW *nw, int i)
{
    struct ExecBase *SysBase = nw->SysBase;
    if (nw->ahiQueued[i]) {
        WaitIO((struct IORequest *)nw->ahiReq[i]);
        nw->ahiQueued[i] = 0;
    }
}

static int ahi_open(struct NW *nw, long rate, long width, long channels)
{
    struct ExecBase *SysBase = nw->SysBase;
    if (width != 2) return -1;
    nw->rate     = (unsigned long)rate;
    nw->ahiType  = (channels >= 2) ? AHIST_S16S : AHIST_M16S;
    nw->fill     = 0;
    nw->fillpos  = 0;
    nw->primed   = 0;
    nw->prerolled = 0;
    nw->ahiQueued[0] = nw->ahiQueued[1] = 0;
    { int k; for (k = 0; k < SMOOTH_MAX; k++) nw->smz[k] = 0; }  /* fresh filter */

    nw->ahiPort = CreateMsgPort();
    if (!nw->ahiPort) return -1;
    nw->ahiReq[0] = (struct AHIRequest *)CreateIORequest(nw->ahiPort, sizeof(struct AHIRequest));
    if (!nw->ahiReq[0]) return -1;
    nw->ahiReq[0]->ahir_Version = 4;
    if (OpenDevice((STRPTR)"ahi.device", (ULONG)nw->ahiUnit,
                   (struct IORequest *)nw->ahiReq[0], 0) != 0)
        return -1;
    nw->ahiOpened = 1;
    nw->ahiUnitOpen = nw->ahiUnit;
    nw->ahiReq[1] = (struct AHIRequest *)AllocMem((ULONG)sizeof(struct AHIRequest),
                                                  MEMF_PUBLIC | MEMF_CLEAR);
    if (!nw->ahiReq[1]) return -1;
    CopyMem(nw->ahiReq[0], nw->ahiReq[1], (ULONG)sizeof(struct AHIRequest));
    nw->ahiBuf[0] = (unsigned char *)AllocMem(AHIBUFSZ, MEMF_PUBLIC);
    nw->ahiBuf[1] = (unsigned char *)AllocMem(AHIBUFSZ, MEMF_PUBLIC);
    if (!nw->ahiBuf[0] || !nw->ahiBuf[1]) return -1;
    return 0;
}

/* Feed PCM (or, when data == NULL, silence) into the double-buffer. A NULL
 * source zero-fills instead of copying — used for the per-utterance pre-roll. */
static void ahi_write(struct NW *nw, const unsigned char *data, long len)
{
    struct ExecBase *SysBase = nw->SysBase;   /* for CopyMem */
    while (len > 0) {
        long space = AHIBUFSZ - nw->fillpos;
        long take  = len < space ? len : space;
        unsigned char *dst = nw->ahiBuf[nw->fill] + nw->fillpos;
        if (data) { CopyMem((APTR)data, dst, (ULONG)take); data += take; }
        else { /* silence; volatile so GCC can't fold it into a memset() we
                * don't link (-nostdlib) */
            volatile unsigned char *z = dst; long k;
            for (k = 0; k < take; k++) z[k] = 0;
        }
        nw->fillpos += take;
        len         -= take;
        if (nw->fillpos == AHIBUFSZ) {           /* current buffer full */
            if (!nw->primed) {
                /* Fill buffer 0, then buffer 1, then submit BOTH back-to-back so
                 * the 0->1 transition is gapless (buffer 1 already queued). */
                if (nw->fill == 0) {
                    nw->fill = 1; nw->fillpos = 0;   /* hold 0, fill 1 */
                } else {
                    ahi_submit(nw, 0, AHIBUFSZ);     /* link NULL -> plays now  */
                    ahi_submit(nw, 1, AHIBUFSZ);     /* link req0 -> gapless     */
                    nw->primed = 1;
                    nw->fill = 0;
                    ahi_wait_free(nw, 0);            /* reclaim 0 to refill      */
                    nw->fillpos = 0;
                }
            } else {
                int other = 1 - nw->fill;
                ahi_submit(nw, nw->fill, AHIBUFSZ);
                ahi_wait_free(nw, other);
                nw->fill    = other;
                nw->fillpos = 0;
            }
        }
    }
}

/* Finish the current utterance: flush whatever is buffered, wait for playback to
 * drain, but KEEP AHI open for the next utterance. Resets the fill state. */
static void ahi_drain(struct NW *nw)
{
    if (!nw->ahiOpened) return;
    /* Post-roll: a little trailing silence so the channel-stop click lands on
     * silence, and voices with almost no built-in trailing silence (e.g.
     * en_US-ryan-high) don't lose their final moment. */
    if (nw->prerolled)
        ahi_write(nw, 0, ((long)nw->rate >> 4) * 2);   /* ~64ms */
    if (!nw->primed) {
        /* utterance ended before pre-roll completed (< 2 full buffers) */
        if (nw->fill == 1) {                 /* buf0 full + held, buf1 partial */
            ahi_submit(nw, 0, AHIBUFSZ);
            if (nw->fillpos > 0) ahi_submit(nw, 1, nw->fillpos);
        } else if (nw->fillpos > 0) {         /* only a partial buf0 */
            ahi_submit(nw, 0, nw->fillpos);
        }
    } else if (nw->fillpos > 0) {
        ahi_submit(nw, nw->fill, nw->fillpos);
    }
    ahi_wait_free(nw, 0);
    ahi_wait_free(nw, 1);
    nw->primed    = 0;
    nw->prerolled = 0;          /* next utterance pre-rolls again (channel idle) */
    nw->fill      = 0;
    nw->fillpos   = 0;
}

static void ahi_close(struct NW *nw)
{
    struct ExecBase *SysBase = nw->SysBase;
    if (nw->ahiOpened) {
        if (nw->fillpos > 0) ahi_submit(nw, nw->fill, nw->fillpos);
        ahi_wait_free(nw, 0);
        ahi_wait_free(nw, 1);
    }
    if (nw->ahiBuf[0]) FreeMem(nw->ahiBuf[0], AHIBUFSZ);
    if (nw->ahiBuf[1]) FreeMem(nw->ahiBuf[1], AHIBUFSZ);
    if (nw->ahiReq[1]) FreeMem(nw->ahiReq[1], (ULONG)sizeof(struct AHIRequest));
    if (nw->ahiOpened) CloseDevice((struct IORequest *)nw->ahiReq[0]);
    if (nw->ahiReq[0]) DeleteIORequest((struct IORequest *)nw->ahiReq[0]);
    if (nw->ahiPort)   DeleteMsgPort(nw->ahiPort);
    /* Reset so ahi_open can re-run (e.g. the unit changed at runtime). */
    nw->ahiBuf[0] = nw->ahiBuf[1] = 0;
    nw->ahiReq[0] = nw->ahiReq[1] = 0;
    nw->ahiPort = 0;
    nw->ahiOpened = 0;
    nw->ahiUnitOpen = -1;
}

/* ---- socket I/O (buffered) ---- */

static long buf_fill(struct NW *nw)
{
    struct Library *SocketBase = nw->SocketBase;
    if (nw->rpos < nw->rlen)
        return nw->rlen - nw->rpos;
    nw->rpos = 0;
    {
        long n = recv(nw->sock, nw->rbuf, RBUFSZ, 0);
        if (n <= 0) { nw->rlen = 0; return n; }
        nw->rlen = n;
    }
    return nw->rlen;
}

static long read_line(struct NW *nw, char *buf, long buflen)
{
    long i = 0;
    while (i + 1 < buflen) {
        long avail = buf_fill(nw);
        unsigned char c;
        if (avail == 0) return (i == 0) ? 0 : i;
        if (avail < 0)  return -1;
        c = nw->rbuf[nw->rpos++];
        if (c == '\n') break;
        buf[i++] = (char)c;
    }
    buf[i] = '\0';
    return i;
}

/* Read exactly `len` bytes into buf. Returns 0 ok, <0 error. */
static int read_block(struct NW *nw, char *buf, long len)
{
    struct ExecBase *SysBase = nw->SysBase;   /* for CopyMem */
    long o = 0;
    while (len > 0) {
        long avail = buf_fill(nw);
        long take;
        if (avail <= 0) return -1;
        take = avail < len ? avail : len;
        CopyMem(nw->rbuf + nw->rpos, buf + o, (ULONG)take);
        nw->rpos += take;
        o += take;
        len -= take;
    }
    return 0;
}

/* Consume `len` payload bytes; feed AHI if open. Returns bytes, or <0. */
static long play_payload(struct NW *nw, long len)
{
    long got = 0;
    while (len > 0) {
        long avail = buf_fill(nw);
        long take;
        if (avail <= 0) return -1;
        take = avail < len ? avail : len;
        if (nw->ahiOpened) ahi_write(nw, nw->rbuf + nw->rpos, take);
        nw->rpos += take;
        len -= take;
        got += take;
    }
    return got;
}

static int write_all(struct NW *nw, const void *buf, long len)
{
    struct Library *SocketBase = nw->SocketBase;
    const char *p = (const char *)buf;
    while (len > 0) {
        long n = send(nw->sock, (void *)p, len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

/* ---- persistent session (held by the device task) ---- */

static struct NW *session_create(struct ExecBase *SysBase)
{
    struct NW *s = (struct NW *)AllocMem((ULONG)sizeof(struct NW),
                                         MEMF_PUBLIC | MEMF_CLEAR);
    if (!s) return 0;
    s->SysBase = SysBase;
    s->sock = -1;
    s->cport = -1;
    s->ahiUnitOpen = -1;        /* AHI not yet opened on any unit */
    s->SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    if (!s->SocketBase) { FreeMem(s, (ULONG)sizeof(struct NW)); return 0; }
    {
        struct Library *SocketBase = s->SocketBase;
        SocketBaseTags(SBTM_SETVAL(SBTC_ERRNOPTR(sizeof(s->err))),
                       (long)&s->err, TAG_END);
    }
    /* Soft-open codesets.library: only used when caller text is non-UTF-8.
     * If the library or either codeset isn't there, transcoding is skipped
     * (pass-through — same as before this feature existed). */
    s->CodesetsBase = OpenLibrary((STRPTR)CODESETSNAME, 0);
    if (s->CodesetsBase) {
        struct Library *CodesetsBase = s->CodesetsBase;
        s->cs_iso = CodesetsFindA((CONST_STRPTR)"ISO-8859-1", (struct TagItem *)0);
        s->cs_utf = CodesetsFindA((CONST_STRPTR)"UTF-8",      (struct TagItem *)0);
    }
    return s;
}

static void session_disconnect(struct NW *s)
{
    struct Library *SocketBase = s->SocketBase;
    if (s->sock >= 0) { CloseSocket(s->sock); s->sock = -1; }
    s->cport = -1;
    s->rpos = s->rlen = 0;
}

static void session_close(struct NW *s)
{
    struct ExecBase *SysBase;
    if (!s) return;
    SysBase = s->SysBase;
    ahi_close(s);                        /* drain + free AHI (last ahi_submits) */
    capture_finalize(s);                 /* patch WAV sizes + close, if open    */
    session_disconnect(s);
    if (s->CodesetsBase) CloseLibrary(s->CodesetsBase);
    if (s->SocketBase)   CloseLibrary(s->SocketBase);
    FreeMem(s, (ULONG)sizeof(struct NW));
}

/* ---- text codeset handling ----
 *
 * Caller text comes from the IOSpeech `io_Data` field and may be in whatever
 * codeset the calling app uses. Piper's Wyoming JSON is UTF-8 only, so we
 * need to recognise non-UTF-8 input and transcode it.
 *
 * Strategy: cheap heuristic first — if the bytes are valid UTF-8, pass
 * through (covers pure ASCII, which always passes, and apps that already
 * speak UTF-8). Otherwise, if codesets.library and an ISO-8859-1 codeset
 * are available, ask it to transcode to UTF-8. Otherwise pass through
 * (today's behaviour — Piper will see mojibake but at least the request
 * goes out). ISO-8859-1 is the right default guess on classic Amigas. */

/* Strict UTF-8 validation per RFC 3629 — rejects 5/6-byte sequences,
 * surrogate halves (D800-DFFF), and overlongs. Returns 1 if every byte in
 * t[0..len) belongs to a valid UTF-8 codepoint, 0 otherwise. Pure ASCII
 * is valid UTF-8 so always passes. */
static int is_valid_utf8(const unsigned char *t, long len)
{
    long i = 0;
    while (i < len) {
        unsigned int c = t[i];
        int  trail;
        unsigned long cp;
        if (c < 0x80)        { i++; continue; }
        if (c < 0xC2)        return 0;             /* lone continuation or overlong */
        if (c < 0xE0)        { trail = 1; cp = c & 0x1F; }
        else if (c < 0xF0)   { trail = 2; cp = c & 0x0F; }
        else if (c < 0xF5)   { trail = 3; cp = c & 0x07; }
        else                 return 0;             /* 5/6-byte sequences invalid */
        if (i + trail >= len) return 0;
        {
            int k;
            for (k = 1; k <= trail; k++) {
                unsigned int cc = t[i + k];
                if ((cc & 0xC0) != 0x80) return 0;
                cp = (cp << 6) | (cc & 0x3F);
            }
        }
        /* Reject overlongs and surrogate halves. */
        if (trail == 1 && cp < 0x80)            return 0;
        if (trail == 2 && cp < 0x800)           return 0;
        if (trail == 3 && cp < 0x10000)         return 0;
        if (cp >= 0xD800 && cp <= 0xDFFF)       return 0;
        if (cp > 0x10FFFF)                      return 0;
        i += 1 + trail;
    }
    return 1;
}

/* If `text` is already valid UTF-8 (or codesets isn't available), return 0
 * and leave *out and *outlen alone (caller uses the original text).
 * Otherwise transcode from ISO-8859-1 to UTF-8 into a freshly-allocated
 * buffer, set *out + *outlen, and return 1. Caller must release the buffer
 * with nw_release_transcoded(s, *out). */
static int nw_transcode_to_utf8(struct NW *s, const char *text, long len,
                                const char **out, long *outlen)
{
    struct Library *CodesetsBase;
    /* Pure-ASCII fast path: scan for any high byte first; if none, no work. */
    {
        long i;
        for (i = 0; i < len; i++)
            if ((unsigned char)text[i] >= 0x80) goto needs_check;
        return 0;                                /* all ASCII -> already UTF-8 */
    needs_check: ;
    }
    if (is_valid_utf8((const unsigned char *)text, len))
        return 0;                                /* well-formed UTF-8 already */
    if (!s->CodesetsBase || !s->cs_iso || !s->cs_utf)
        return 0;                                /* no transcoder -> pass through */

    CodesetsBase = s->CodesetsBase;
    {
        ULONG newlen = 0;
        struct TagItem tags[] = {
            { CSA_Source,        (ULONG)text },
            { CSA_SourceLen,     (ULONG)len  },
            { CSA_SourceCodeset, (ULONG)s->cs_iso },
            { CSA_DestCodeset,   (ULONG)s->cs_utf },
            { CSA_DestLenPtr,    (ULONG)&newlen },
            { TAG_DONE,          0 }
        };
        STRPTR conv = CodesetsConvertStrA(tags);
        if (!conv) return 0;                     /* convert failed -> pass through */
        *out    = (const char *)conv;
        *outlen = (long)newlen;
        return 1;
    }
}

static void nw_release_transcoded(struct NW *s, const char *buf)
{
    struct Library *CodesetsBase;
    if (!buf || !s->CodesetsBase) return;
    CodesetsBase = s->CodesetsBase;
    {
        struct TagItem tags[] = { { TAG_DONE, 0 } };
        CodesetsFreeA((APTR)buf, tags);
    }
}

/* Ensure the held socket is connected to host:port (reconnect if the endpoint
 * changed or the link dropped). Returns 0 or NWERR_*. */
static long session_connect(struct NW *s, const char *host, unsigned short port)
{
    struct ExecBase *SysBase    = s->SysBase;      /* for CopyMem */
    struct Library  *SocketBase = s->SocketBase;
    struct sockaddr_in sa;
    long i;

    if (s->sock >= 0) {
        int same = (s->cport == (int)port);
        for (i = 0; same && (host[i] || s->chost[i]); i++)
            if (host[i] != s->chost[i]) same = 0;
        if (same) return 0;              /* reuse the held connection */
        session_disconnect(s);           /* endpoint changed */
    }

    for (i = 0; i < (long)sizeof(sa); i++) ((char *)&sa)[i] = 0;
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = inet_addr((STRPTR)host);
    if (sa.sin_addr.s_addr == (unsigned long)-1) {
        struct hostent *he = gethostbyname((STRPTR)host);
        if (!he || !he->h_addr_list[0]) return NWERR_RESOLVE;
        CopyMem(he->h_addr_list[0], &sa.sin_addr, (ULONG)sizeof(sa.sin_addr));
    }

    s->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (s->sock < 0) return NWERR_CONNECT;

    /* Big receive buffer, set BEFORE connect so the TCP window is sized for it:
     * with split_words the server pipelines several chunks, but we read at AHI
     * (playback) speed, so without lookahead headroom the server gets throttled
     * and the next chunk isn't ready when we reach the seam -> AHI underruns.
     * ~256KB ~= 3s of audio buffered ahead. */
    { int rb = 256 * 1024;
      setsockopt(s->sock, SOL_SOCKET, SO_RCVBUF, (char *)&rb, (socklen_t)sizeof(rb)); }

    /* Non-blocking connect with a timeout (see NW_CONNECT_TIMEOUT). */
    {
        long nb = 1;
        IoctlSocket(s->sock, FIONBIO, (char *)&nb);
        if (connect(s->sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
            int e = Errno();
            if (e == EINPROGRESS || e == EWOULDBLOCK) {
                fd_set wfds; struct timeval tv; long so_err = 0;
                socklen_t slen = (socklen_t)sizeof(so_err);
                FD_ZERO(&wfds); FD_SET(s->sock, &wfds);
                tv.tv_sec = NW_CONNECT_TIMEOUT; tv.tv_usec = 0;
                if (WaitSelect(s->sock + 1, NULL, &wfds, NULL, &tv, NULL) <= 0) {
                    session_disconnect(s); return NWERR_CONNECT;
                }
                if (getsockopt(s->sock, SOL_SOCKET, SO_ERROR,
                               (char *)&so_err, &slen) < 0 || so_err != 0) {
                    session_disconnect(s); return NWERR_CONNECT;
                }
            } else { session_disconnect(s); return NWERR_CONNECT; }
        }
        nb = 0;
        IoctlSocket(s->sock, FIONBIO, (char *)&nb);
        { struct timeval rtv; rtv.tv_sec = NW_RECV_TIMEOUT; rtv.tv_usec = 0;
          setsockopt(s->sock, SOL_SOCKET, SO_RCVTIMEO,
                     (char *)&rtv, (socklen_t)sizeof(rtv)); }
    }

    for (i = 0; host[i] && i < (long)sizeof(s->chost) - 1; i++) s->chost[i] = host[i];
    s->chost[i] = '\0';
    s->cport = (int)port;
    return 0;
}

/* Send one synthesize request on the held connection and play the response.
 * Returns PCM bytes (>=0) or NWERR_*. Drops the connection on a socket error
 * (so the next call reconnects). Keeps AHI open between utterances. */
/* Split `text` into up to `maxseg` chunks of roughly `max_words` words each for
 * pipelined synthesis. Breaks at sentence terminators (. ! ?) first, then at
 * clause punctuation (, ; :) once a chunk has reached the word target, and as a
 * last resort at a word boundary (a run with no punctuation). off[]/len[] get
 * each chunk's [offset,length) into `text`. Returns the chunk count (>=1).
 * max_words<=0 (or no room) yields a single chunk: the whole text. */
static int nw_split(const char *text, long textlen, int max_words,
                    long *off, long *len, int maxseg)
{
    int  nseg = 0;
    long i = 0, seg_start = 0, words = 0;
    if (max_words <= 0 || maxseg <= 1) { off[0] = 0; len[0] = textlen; return 1; }
    while (i < textlen) {
        long we; char last; int brk;
        while (i < textlen && (text[i]==' '||text[i]=='\t'||text[i]=='\n'||text[i]=='\r')) i++;
        if (i >= textlen) break;
        while (i < textlen && !(text[i]==' '||text[i]=='\t'||text[i]=='\n'||text[i]=='\r')) i++;
        we = i;                                  /* the word ends just before we */
        words++;
        /* Peel a trailing closing-quote / -bracket run so we classify on the
         * punctuation it wraps, not the quote itself. Without this, `"Hello."
         * World.` never splits after the quoted sentence because the last
         * char is `"`, not `.`. */
        {
            long e = we - 1;
            while (e > seg_start &&
                   (text[e]=='"'||text[e]=='\''||text[e]==')'
                   ||text[e]==']'||text[e]=='}'))
                e--;
            last = text[e];
        }
        brk = 0;
        /* Only ever break at PUNCTUATION: a sentence end always, a clause comma/
         * semicolon/colon once we're past the word target. Never mid-phrase — a
         * seam there is audible, but at a punctuation mark Piper already pauses,
         * so the chunk boundary hides in the natural pause. An unpunctuated
         * run-on simply won't split (one request: slower start, but no gaps). */
        if (last=='.'||last=='!'||last=='?') brk = 1;                       /* sentence: always */
        else if ((last==','||last==';'||last==':') && words >= max_words) brk = 1; /* clause when long enough */
        if (brk && nseg < maxseg - 1) {
            off[nseg] = seg_start; len[nseg] = we - seg_start; nseg++;
            seg_start = we; words = 0;
        }
    }
    if (seg_start < textlen) { off[nseg] = seg_start; len[nseg] = textlen - seg_start; nseg++; }
    if (nseg == 0) { off[0] = 0; len[0] = textlen; nseg = 1; }
    return nseg;
}

/* Append src[0..len) to *pp with the minimum JSON escaping Piper needs:
 * \" \\ \n \r get backslash-escaped, anything else <0x20 gets replaced with
 * space (Piper rejects raw control chars in strings). Bytes >= 0x80 pass
 * through unchanged — caller's responsibility to feed valid UTF-8. Advances
 * *pp by the bytes written. */
static void json_escape_append(char **pp, const char *src, long len)
{
    char *p = *pp;
    long  i;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') { *p++ = '\\'; *p++ = (char)c; }
        else if (c == '\n')        { *p++ = '\\'; *p++ = 'n'; }
        else if (c == '\r')        { *p++ = '\\'; *p++ = 'r'; }
        else if (c < 0x20)         { *p++ = ' '; }
        else                       { *p++ = (char)c; }
    }
    *pp = p;
}

/* Build + send ONE synthesize request for text[0..textlen). 0 ok, NWERR_* on
 * failure (caller disconnects). */
static long session_send_one(struct NW *s, const char *text, long textlen,
                             const char *voice)
{
    struct ExecBase *SysBase = s->SysBase;
    char *req;
    long  voicelen = voice ? nw_strlen(voice) : 0;
    /* Both text and voice can grow 2x under escaping (every byte -> "\X"). */
    long  reqlen = textlen * 2 + voicelen * 2 + 96;

    req = (char *)AllocMem((ULONG)reqlen, MEMF_PUBLIC);
    if (!req) return NWERR_NOMEM;
    {
        static const char head[]  = "{\"type\": \"synthesize\", \"data\": {\"text\": \"";
        static const char vhead[] = "\", \"voice\": {\"name\": \"";
        static const char vtail[] = "\"}";
        static const char tail[]  = "}}\n";
        char *p = req; long i;
        for (i = 0; head[i]; i++) *p++ = head[i];
        json_escape_append(&p, text, textlen);
        if (voicelen > 0) {
            for (i = 0; vhead[i]; i++) *p++ = vhead[i];
            json_escape_append(&p, voice, voicelen);
            for (i = 0; vtail[i]; i++) *p++ = vtail[i];
        } else { *p++ = '"'; }
        for (i = 0; tail[i]; i++) *p++ = tail[i];
        if (write_all(s, req, (long)(p - req)) != 0) {
            FreeMem(req, (ULONG)reqlen);
            session_disconnect(s);
            return NWERR_SEND;
        }
    }
    FreeMem(req, (ULONG)reqlen);
    return 0;
}

#define NW_MAXSEG 32

/* Send the text as one request, or — when split_words>0 — as several pipelined
 * synthesize requests (sent up front), then stream every response into AHI as
 * one continuous, gapless utterance. Pipelining is what keeps it gapless: while
 * AHI plays chunk 1 in real time, the server is already synthesizing the rest.
 * Returns total PCM bytes (>=0) or NWERR_*. */
static long session_send_recv(struct NW *s, const char *text, long textlen,
                              const char *voice, long volume, int split_words)
{
    long off[NW_MAXSEG], len[NW_MAXSEG];
    int  nseg, k;
    long result;

    s->volume = volume;
    s->rpos = s->rlen = 0;

    nseg = nw_split(text, textlen, split_words, off, len, NW_MAXSEG);
    for (k = 0; k < nseg; k++) {
        long r = session_send_one(s, text + off[k], len[k], voice);
        if (r != 0) return r;
    }

    {
        char hdr[512], dbuf[640];
        long total = 0, rate = 0, width = 0, channels = 0;
        int  stops = 0, sockerr = 0;
        for (;;) {
            long ln = read_line(s, hdr, (long)sizeof(hdr));
            long data_len = 0, pay_len = 0;
            if (ln < 0)  { sockerr = 1; break; }   /* recv error */
            if (ln == 0) break;                    /* peer closed */

            if (find_int(hdr, "data_length", &data_len) && data_len > 0) {
                if (data_len < (long)sizeof(dbuf) - 1) {
                    if (read_block(s, dbuf, data_len) != 0) { sockerr = 1; break; }
                    dbuf[data_len] = '\0';
                    find_int(dbuf, "rate", &rate);
                    find_int(dbuf, "width", &width);
                    find_int(dbuf, "channels", &channels);
                } else {
                    long left = data_len;
                    while (left > 0) {
                        long a = buf_fill(s), t;
                        if (a <= 0) { left = -1; break; }
                        t = a < left ? a : left;
                        s->rpos += t; left -= t;
                    }
                    if (left < 0) { sockerr = 1; break; }
                }
            }
            find_int(hdr, "rate", &rate);
            find_int(hdr, "width", &width);
            find_int(hdr, "channels", &channels);

            if (contains(hdr, "audio-stop")) {
                if (++stops >= nseg) break;   /* all pipelined chunks finished */
                continue;                      /* more chunks still streaming   */
            }

            if (find_int(hdr, "payload_length", &pay_len) && pay_len > 0) {
                long got;
                /* If AHI is held open on a different unit than the prefs now
                 * ask for, close it so it reopens on the configured unit. */
                if (s->ahiOpened && s->ahiUnitOpen != s->ahiUnit)
                    ahi_close(s);
                if (!s->ahiOpened && rate && width && channels)
                    ahi_open(s, rate, width, channels);
                /* Per-utterance silence pre-roll: each utterance restarts the
                 * idle AHI channel, and the cold-start DAC transient would
                 * otherwise clip the word's onset (a one-word "test" loses its
                 * 't'). ~64ms of leading silence absorbs the transient. */
                if (s->ahiOpened && !s->prerolled) {
                    ahi_write(s, 0, ((long)s->rate >> 4) * 2);   /* rate/16 samples */
                    s->prerolled = 1;
                }
                got = play_payload(s, pay_len);
                if (got < 0) { sockerr = 1; break; }
                total += got;
            }
        }
        ahi_drain(s);                    /* finish playback; keep AHI open */
        if (sockerr) session_disconnect(s);
        result = (stops >= nseg) ? total : NWERR_PROTO;
    }
    return result;
}

/* Synthesize one utterance on the session, (re)connecting as needed. Retries
 * once if a send fails because the held connection had silently dropped. */
static long nw_session_say(struct NW *s, const char *host, unsigned short port,
                           const char *text, long textlen,
                           const char *voice, long volume, int split_words)
{
    long r;
    int  attempt;
    for (attempt = 0; attempt < 2; attempt++) {
        r = session_connect(s, host, port);
        if (r != 0) return r;
        r = session_send_recv(s, text, textlen, voice, volume, split_words);
        if (r != NWERR_SEND) return r;   /* success, or a non-recoverable error */
        /* send failed (stale connection dropped) -> loop reconnects + retries */
    }
    return r;
}

/* ============================================================
 * Device task (M5)
 * ============================================================ */

/* Classic narrator.device takes ARPABET PHONEMES; Piper wants English (our
 * pass-through translator.library hands the device English). A program that
 * drives narrator.device directly with phonemes — the old contract, bypassing
 * translator.library — would send uppercase phoneme codes with stress digits,
 * which Piper can only mangle. Detect that and silently discard it (see
 * docs/narrator-device.md) rather than speak gibberish.
 *
 * Heuristic, biased HARD toward speaking so real text is never eaten: treat as
 * phonetic only when there is NO lowercase letter anywhere AND a stress-digit
 * pattern appears (a digit immediately after an A-Z letter, e.g. "EH1"). English
 * never writes digits inside words, so normal prose and even all-caps words like
 * "WARNING" are always spoken. The cost: unmarked all-caps phonemes (no stress
 * digits) slip through, and an all-caps token with an embedded digit ("MP3")
 * would be dropped — both rare and acceptable versus eating English. */
static int looks_phonetic(const char *t, long len)
{
    long i; int hasStress = 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)t[i];
        if (c >= 'a' && c <= 'z') return 0;          /* any lowercase -> English */
        if (c >= '0' && c <= '9' && i > 0) {
            unsigned char p = (unsigned char)t[i - 1];
            if (p >= 'A' && p <= 'Z') hasStress = 1;  /* stress mark like EH1 */
        }
    }
    return hasStress;
}

/* Process one CMD_WRITE on the held session: map voice/volume from this
 * request's narrator_rb against the prefs already cached on `s`, then
 * synthesize+play (reusing the persistent connection). Runs in the device
 * task. Prefs are read once at task start in nw_dev_task, NOT here — see
 * struct NW::prefs for the rationale. */
static void nw_do_write(struct NW *s, struct narrator_rb *nrb)
{
    const struct nwprefs *prefs = &s->prefs;
    const char *voice;
    long vol = nrb->volume, avol, n;

    if (!nrb->message.io_Data || (long)nrb->message.io_Length <= 0) {
        nrb->message.io_Actual = 0;
        nrb->message.io_Error  = 0;
        return;                          /* nothing to say */
    }

    /* ARPABET phonemes (the old direct contract): accept but stay silent —
     * Piper can't speak phonemes, so emitting anything would be gibberish. */
    if (looks_phonetic((const char *)nrb->message.io_Data,
                        (long)nrb->message.io_Length)) {
        nrb->message.io_Actual = 0;
        nrb->message.io_Error  = 0;
        return;
    }

    if      (nrb->sex == FEMALE && prefs->voice_female[0]) voice = prefs->voice_female;
    else if (nrb->sex == MALE   && prefs->voice_male[0])   voice = prefs->voice_male;
    else if (prefs->voice[0])                              voice = prefs->voice;
    else                                                   voice = 0;

    if (vol < 0) vol = 0;
    if (vol > MAXVOL) vol = MAXVOL;

    /* Scale the AHI volume by the `gain` headroom (% of full scale). Piper peaks
     * touch 0 dBFS and AHI's resample to Paula's rate overshoots full-scale
     * peaks -> harsh clipping; gain < 100 leaves room so peaks don't hit the
     * ceiling. Fixed 16.16, so unity (vol == MAXVOL) * gain/100 stays <= 0x10000. */
    avol = (vol * (0x10000L / MAXVOL)) * prefs->gain / 100;

    {
        const char *txt    = (const char *)nrb->message.io_Data;
        long        txtlen = (long)nrb->message.io_Length;
        const char *conv   = 0;
        long        convlen = 0;
        int         did_convert = nw_transcode_to_utf8(s, txt, txtlen, &conv, &convlen);
        if (did_convert) { txt = conv; txtlen = convlen; }
        n = nw_session_say(s, prefs->host, (unsigned short)prefs->port,
                           txt, txtlen,
                           voice, avol, prefs->split_words);
        if (did_convert) nw_release_transcoded(s, conv);
    }
    if (n >= 0) { nrb->message.io_Actual = (ULONG)n;    nrb->message.io_Error = 0; }
    else        { nrb->message.io_Actual = (ULONG)(-n); nrb->message.io_Error = (BYTE)-1; }
}

/* Per-open context. Allocated by nw_dev_open and freed by nw_dev_close AFTER
 * the device task has fully unwound (signal handshake; see nw_dev_open).
 * Reached from BeginIO via io->io_Unit, so no globals are needed.
 *
 * Assumption: the task that calls OpenDevice is the same task that calls
 * CloseDevice — the standard Amiga convention. The signal bits live on that
 * task; FreeSignal would no-op (or worse) from a different task. */
struct nwctx {
    struct ExecBase   *SysBase;
    struct DosLibrary *DOSBase;          /* opened by parent; closed by parent  */
    struct Process    *proc;             /* the device task                     */
    struct MsgPort * volatile cmdPort;   /* task's request port (task creates)   */
    volatile LONG      shutting;         /* close() set this                     */
    struct Message     shutMsg;          /* the shutdown message                 */
    struct Task       *parent;           /* the OpenDevice/CloseDevice caller    */
    LONG               ready_bit;        /* parent's signal bit; task sets it   */
                                         /* once it has published cmdPort (or   */
                                         /* hit a startup error)                */
    LONG               dead_bit;         /* parent's signal bit; task sets it   */
                                         /* as its absolute last act before RTS */
};

/* The device task. A4 is undefined here, so NO globals — everything via ctx
 * (passed through tc_UserData) and SysBase (from address 4). */
static void nw_dev_task(void)
{
    struct ExecBase   *SysBase = *(struct ExecBase **)4UL;
    struct nwctx      *ctx     = (struct nwctx *)(FindTask(0L)->tc_UserData);
    struct MsgPort    *port    = CreateMsgPort();
    struct NW         *sess    = session_create(SysBase);  /* the held connection */

    /* Snapshot the prefs once for this device-open's lifetime. Subsequent
     * CMD_WRITEs read sess->prefs directly — no per-write disk hit. */
    if (sess) {
        nw_read_prefs(SysBase, &sess->prefs);
        sess->ahiUnit = sess->prefs.ahi_unit;
        sess->smooth  = sess->prefs.smooth;
    }

    ctx->cmdPort = port;                 /* publish (single aligned write, atomic) */

    /* Tell parent we've reached this point. Parent's Wait(ready) unblocks; on
     * port == NULL, parent will still see a valid ctx but submit() will fail
     * and close() will Wait on dead_sig (which we'll set below). */
    Signal(ctx->parent, 1UL << ctx->ready_bit);

    if (port) {
        int run = !ctx->shutting;
        while (run) {
            struct Message *m;
            WaitPort(port);
            while ((m = GetMsg(port)) != 0) {
                if (m == &ctx->shutMsg) { run = 0; break; }
                {
                    struct narrator_rb *nrb = (struct narrator_rb *)m;
                    if (sess) nw_do_write(sess, nrb);   /* reuses the connection */
                    else { nrb->message.io_Actual = 0;  /* no session -> fail */
                           nrb->message.io_Error = (BYTE)-1; }
                }
                ReplyMsg(m);
            }
            if (ctx->shutting) run = 0;
        }
        /* Drain anything still queued so callers waiting on SendIO/AbortIO
         * don't leak. FIFO can land shutMsg before queued CMD_WRITEs; without
         * this drain those writes would never be replied. */
        {
            struct Message *m;
            while ((m = GetMsg(port)) != 0) {
                if (m == &ctx->shutMsg) continue;
                ((struct IORequest *)m)->io_Error = IOERR_ABORTED;
                ReplyMsg(m);
            }
        }
    }

    if (sess) session_close(sess);       /* close held connection + AHI */
    if (port) DeleteMsgPort(port);

    /* Snapshot what we need from ctx, because the moment we Signal dead the
     * parent's Wait may return and FreeMem(ctx). A small window remains
     * between this Signal and the implicit RTS where we're still executing
     * segment code; expunge isn't synchronous with CloseDevice in practice,
     * so a fully out-of-segment finalizer isn't justified here. */
    {
        struct Task *p   = ctx->parent;
        ULONG       mask = 1UL << ctx->dead_bit;
        Signal(p, mask);
    }
    /* implicit RTS to dos.library process-exit handler */
}

struct nwctx *nw_dev_open(struct ExecBase *sysbase)
{
    struct ExecBase   *SysBase = sysbase;
    struct DosLibrary *DOSBase;
    struct nwctx      *ctx;
    LONG rb, db;

    ctx = (struct nwctx *)AllocMem((ULONG)sizeof(struct nwctx),
                                   MEMF_PUBLIC | MEMF_CLEAR);
    if (!ctx) return 0;
    ctx->SysBase = SysBase;
    ctx->shutMsg.mn_Length = (UWORD)sizeof(ctx->shutMsg);
    ctx->parent    = FindTask(0L);
    ctx->ready_bit = -1;
    ctx->dead_bit  = -1;

    /* Allocate the two signals on the caller's task. They must outlive the
     * device task and are released in nw_dev_close on this same caller's task. */
    rb = AllocSignal(-1L);
    db = AllocSignal(-1L);
    if (rb < 0 || db < 0) {
        if (rb >= 0) FreeSignal(rb);
        if (db >= 0) FreeSignal(db);
        FreeMem(ctx, (ULONG)sizeof(struct nwctx));
        return 0;
    }
    ctx->ready_bit = rb;
    ctx->dead_bit  = db;

    DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 37);
    if (!DOSBase) {
        FreeSignal(rb); FreeSignal(db);
        FreeMem(ctx, (ULONG)sizeof(struct nwctx));
        return 0;
    }
    ctx->DOSBase = DOSBase;

    /* Forbid across CreateNewProc + tc_UserData so the task cannot run before
     * its context pointer is set (libnix's OpenDevice wrapper Forbid status is
     * not contractual — be defensive). */
    Forbid();
    {
        struct Process *proc = CreateNewProcTags(
            NP_Entry,     (Tag)nw_dev_task,
            NP_Name,      (Tag)"narrator.wyoming",
            NP_StackSize, (Tag)48000,    /* bsdsocket+AHI deep paths; 24K was tight */
            NP_Priority,  (Tag)1,        /* run promptly so the ready signal lands  */
            TAG_END);
        if (!proc) {
            Permit();
            CloseLibrary((struct Library *)DOSBase);
            FreeSignal(rb); FreeSignal(db);
            FreeMem(ctx, (ULONG)sizeof(struct nwctx));
            return 0;
        }
        proc->pr_Task.tc_UserData = ctx;
        ctx->proc = proc;
    }
    Permit();

    /* Block until the task has either published cmdPort or hit a startup
     * error. Wait() temporarily releases any pending Forbid so the task can
     * run. After this returns, cmdPort is either valid or NULL — in the NULL
     * case the task has continued on to set dead_bit too, and close() will
     * see it already signaled. */
    Wait(1UL << rb);

    return ctx;
}

void nw_dev_close(struct nwctx *ctx)
{
    struct ExecBase *SysBase;
    LONG rb, db;
    if (!ctx) return;
    SysBase = ctx->SysBase;

    /* Tell the task to stop. shutMsg is how a WaitPort'd task wakes; the
     * shutting flag covers the gap between message-loop iterations and the
     * "no port" failure path. */
    ctx->shutting = 1;
    if (ctx->cmdPort)
        PutMsg(ctx->cmdPort, &ctx->shutMsg);

    /* Block until the task has run all its cleanup (socket, AHI, port) and
     * signaled dead. Wait() releases the OpenDevice/CloseDevice-implicit
     * Forbid for the duration so the task can actually execute. */
    Wait(1UL << ctx->dead_bit);

    rb = ctx->ready_bit;
    db = ctx->dead_bit;

    /* Task is gone; safe to release everything it referenced through ctx. */
    if (ctx->DOSBase) CloseLibrary((struct Library *)ctx->DOSBase);
    FreeMem(ctx, (ULONG)sizeof(struct nwctx));
    FreeSignal(rb);
    FreeSignal(db);
}

void nw_dev_submit(struct nwctx *ctx, struct IORequest *io)
{
    struct ExecBase *SysBase = ctx->SysBase;
    /* nw_dev_open waited until the task published cmdPort (or signaled startup
     * failure with cmdPort still NULL). No polling needed here — if cmdPort
     * is NULL the task failed to start; fail the request cleanly. */
    if (!ctx->cmdPort) {
        io->io_Error = (BYTE)-1;
        if (!(io->io_Flags & IOF_QUICK)) ReplyMsg(&io->io_Message);
        return;
    }
    PutMsg(ctx->cmdPort, &io->io_Message);   /* task processes + ReplyMsg()s */
}

long nw_dev_abort(struct nwctx *ctx, struct IORequest *io)
{
    struct ExecBase *SysBase;
    struct MsgPort  *port;
    struct Node     *n;
    if (!ctx || !io) return -1;
    SysBase = ctx->SysBase;
    port    = ctx->cmdPort;
    if (!port) return -1;

    /* Walk the port's queue under Forbid so the task can't GetMsg the request
     * out from under us. ReplyMsg happens after Permit (it touches the
     * caller's reply port — keep the Forbid window short). */
    Forbid();
    for (n = port->mp_MsgList.lh_Head; n->ln_Succ; n = n->ln_Succ) {
        if ((struct Message *)n == &io->io_Message) {
            Remove(n);
            Permit();
            io->io_Error = IOERR_ABORTED;
            ReplyMsg(&io->io_Message);
            return 0;
        }
    }
    Permit();
    return -1;                          /* not queued: in-flight or already done */
}

void nw_dev_flush(struct nwctx *ctx)
{
    struct ExecBase *SysBase;
    struct MsgPort  *port;
    struct Node     *n, *next;
    if (!ctx) return;
    SysBase = ctx->SysBase;
    port    = ctx->cmdPort;
    if (!port) return;

    /* Snapshot ln_Succ before Remove because Remove leaves n's own pointers
     * untouched but unlinks it from the list — `next` stays valid as a
     * stepping pointer. The shutMsg, if it happens to be queued, is left
     * alone (it isn't an IORequest and is owned by close, not the caller). */
    Forbid();
    n = port->mp_MsgList.lh_Head;
    while ((next = n->ln_Succ) != 0) {
        if ((struct Message *)n != &ctx->shutMsg) {
            Remove(n);
            ((struct IORequest *)n)->io_Error = IOERR_ABORTED;
            ReplyMsg((struct Message *)n);
        }
        n = next;
    }
    Permit();
}

#endif /* PLATFORM_AMIGA */
