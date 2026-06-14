/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* paula_11025_probe.c — like paula_lib_probe.c but downsamples 22050 -> 11025
 * (anti-aliased, 2:1 decimation) before handing to AHI. Tests whether feeding
 * a half-rate source through paula reduces the >11 kHz aliasing measured in
 * docs/audio-capture-rig.md — i.e. would lowering Piper's output rate fix the
 * harsh-sibilance problem.
 *
 * Downsampler: a 2-pass cascade of (x + x_prev)/2 averagers (same filter shape
 * as smooth_buf in nw_engine.c) applied in-place to the LE-PCM buffer, then a
 * 2:1 decimation that keeps every other sample. -3 dB around fs/3, deep null
 * at fs/2 — close enough to a proper half-band anti-alias for this test.
 *
 * Otherwise identical to paula_lib_probe.c: AHI library interface, paula mode
 * 0x0002000c, no requester, real-time-paced playback. AHIA_MixFreq is set
 * to the post-downsample rate (rate/2) so the AHI mixer doesn't have to do
 * another resample on top.
 */

#include <exec/memory.h>
#include <exec/io.h>
#include <devices/ahi.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/ahi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_PATH      "Narrator:capture_raw.wav"
#define MODE_ID         0x00020018UL    /* match paula_lib_probe.c — same paula mode in both probes
                                         * so the ONLY variable in the comparison is the rate. */
#define HEADROOM_TICKS  25              /* 0.5 s on top of the audio's duration */

struct Library *AHIBase;

static unsigned long u32_le(const unsigned char *b)
{
    return (unsigned long)b[0]
         | ((unsigned long)b[1] << 8)
         | ((unsigned long)b[2] << 16)
         | ((unsigned long)b[3] << 24);
}

static unsigned short u16_le(const unsigned char *b)
{
    return (unsigned short)b[0] | ((unsigned short)b[1] << 8);
}

/* Read/write little-endian signed 16-bit. */
static int read_le16(const unsigned char *b)
{
    int v = (int)b[0] | ((int)b[1] << 8);
    if (v & 0x8000) v -= 0x10000;
    return v;
}
static void write_le16(unsigned char *b, int v)
{
    b[0] = (unsigned char)(v & 0xff);
    b[1] = (unsigned char)((v >> 8) & 0xff);
}

/* In-place smooth (2-pass [1,1]/2 cascade -> [1,2,1]/4 effective). Operates on
 * the LE-PCM buffer; samples count, NOT bytes. */
static void smooth(unsigned char *buf, long samples)
{
    int prev1 = 0, prev2 = 0;
    long i;
    for (i = 0; i < samples; i++) {
        int x  = read_le16(buf + 2 * i);
        int t1 = (x + prev1) >> 1; prev1 = x;
        int t2 = (t1 + prev2) >> 1; prev2 = t1;
        write_le16(buf + 2 * i, t2);
    }
}

/* 2:1 decimate in place. Returns the new sample count. */
static long decimate_2(unsigned char *buf, long samples)
{
    long out = samples / 2;
    long i;
    for (i = 0; i < out; i++) {
        buf[2 * i]     = buf[4 * i];
        buf[2 * i + 1] = buf[4 * i + 1];
    }
    return out;
}

int main(void)
{
    FILE *f;
    unsigned char  hdr[44];
    unsigned char *buf;
    long           buf_bytes, i;
    unsigned long  rate, data_size, samples;
    unsigned long  out_rate, out_samples;
    unsigned short channels, bits;
    struct MsgPort       *port;
    struct AHIRequest    *req;
    struct AHIAudioCtrl  *ctrl;
    struct AHISampleInfo  si;
    ULONG                 rc;
    long                  wait_ticks;

    printf("paula_11025_probe: reading %s\n", INPUT_PATH);
    fflush(stdout);

    f = fopen(INPUT_PATH, "rb");
    if (!f)                          { printf("FAIL: fopen\n");        return 20; }
    if (fread(hdr, 1, 44, f) != 44)  { printf("FAIL: short header\n"); return 20; }
    if (memcmp(hdr,     "RIFF", 4)) { printf("FAIL: not RIFF\n");     return 20; }
    if (memcmp(hdr + 8, "WAVE", 4)) { printf("FAIL: not WAVE\n");     return 20; }
    channels  = u16_le(hdr + 22);
    rate      = u32_le(hdr + 24);
    bits      = u16_le(hdr + 34);
    data_size = u32_le(hdr + 40);
    samples   = data_size / (channels * (bits / 8));
    printf("  input  : %ldHz %d-bit %d ch (%ld samples)\n",
           (long)rate, (int)bits, (int)channels, (long)samples);
    fflush(stdout);
    if (channels != 1 || bits != 16) {
        printf("FAIL: need mono 16-bit\n"); return 20;
    }

    buf_bytes = (long)samples * 2;
    buf = (unsigned char *)malloc(buf_bytes);
    if (!buf)                                            { printf("FAIL: malloc\n");       return 20; }
    if ((long)fread(buf, 1, buf_bytes, f) != buf_bytes) { printf("FAIL: short read\n"); return 20; }
    fclose(f);

    /* Downsample 2:1 with anti-aliasing — produces half the samples at half rate. */
    smooth(buf, samples);
    out_samples = decimate_2(buf, samples);
    out_rate    = rate / 2;
    printf("  downsampled to %ldHz (%ld samples = %ld.%03lds)\n",
           (long)out_rate, (long)out_samples,
           out_samples / out_rate,
           ((out_samples - (out_samples / out_rate) * out_rate) * 1000UL) / out_rate);
    fflush(stdout);

    /* LE -> BE for AHIST_M16S. Use the new sample count. */
    for (i = 0; i + 1 < (long)(out_samples * 2); i += 2) {
        unsigned char t = buf[i]; buf[i] = buf[i + 1]; buf[i + 1] = t;
    }

    /* AHI library interface, same as paula_lib_probe but with out_rate. */
    port = CreateMsgPort();
    if (!port) { printf("FAIL: CreateMsgPort\n"); return 20; }
    req = (struct AHIRequest *)CreateIORequest(port, sizeof(struct AHIRequest));
    if (!req) { printf("FAIL: CreateIORequest\n"); return 20; }
    req->ahir_Version = 4;
    if (OpenDevice((STRPTR)"ahi.device", AHI_NO_UNIT,
                   (struct IORequest *)req, 0) != 0) {
        printf("FAIL: OpenDevice\n"); return 20;
    }
    AHIBase = (struct Library *)req->ahir_Std.io_Device;

    printf("  AHI_AllocAudio (paula mode 0x%lx, MixFreq=%ldHz)\n",
           MODE_ID, (long)out_rate);
    fflush(stdout);
    ctrl = AHI_AllocAudio(
        AHIA_AudioID,  MODE_ID,
        AHIA_MixFreq,  out_rate,
        AHIA_Channels, 1UL,
        AHIA_Sounds,   1UL,
        TAG_END);
    if (!ctrl) { printf("FAIL: AHI_AllocAudio\n"); return 20; }

    si.ahisi_Type    = AHIST_M16S;
    si.ahisi_Address = buf;
    si.ahisi_Length  = out_samples * 2;
    rc = AHI_LoadSound(0, AHIST_SAMPLE, &si, ctrl);
    if (rc != 0) { printf("FAIL: AHI_LoadSound rc=%lu\n", (unsigned long)rc); return 20; }

    rc = AHI_ControlAudio(ctrl, AHIC_Play, TRUE, TAG_END);
    if (rc != 0) { printf("FAIL: AHIC_Play TRUE rc=%lu\n", (unsigned long)rc); return 20; }

    AHI_Play(ctrl,
             AHIP_BeginChannel, 0UL,
             AHIP_Freq,         out_rate,
             AHIP_Vol,          0x10000UL,
             AHIP_Pan,          0x8000UL,
             AHIP_Sound,        0UL,
             AHIP_EndChannel,   0UL,
             TAG_END);

    wait_ticks = (long)((out_samples * 50UL) / out_rate) + HEADROOM_TICKS;
    printf("  AHI_Play; Delay(%ld ticks = %ld.%lds wall)\n",
           wait_ticks, wait_ticks / 50, ((wait_ticks % 50) * 10) / 50);
    fflush(stdout);
    Delay(wait_ticks);

    AHI_ControlAudio(ctrl, AHIC_Play, FALSE, TAG_END);
    AHI_UnloadSound(0, ctrl);
    AHI_FreeAudio(ctrl);
    free(buf);
    CloseDevice((struct IORequest *)req);
    DeleteIORequest((struct IORequest *)req);
    DeleteMsgPort(port);
    printf("done\n");
    return 0;
}
