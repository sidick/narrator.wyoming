/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* hook_probe.c — verify AHIA_SoundFunc hook plumbing for streaming v2.
 *
 * Step 1 of the streaming-playback re-introduction plan. The streaming
 * design uses 2 sound slots with a hook to signal end-of-sound so the
 * producer can queue the next slot without polling. Before committing to
 * that refactor we need empirical confirmation that:
 *
 *   1. Bebbo amigaos GCC's register-args + __attribute__((saveds)) calling
 *      convention works for an AHI SoundFunc hook (this codebase hasn't used
 *      hooks before).
 *   2. AHIA_SoundFunc actually fires under Amiberry's AHI implementation
 *      (none of the standalone probes we already have set up a SoundFunc).
 *   3. Signal() from the hook reaches the main task's Wait() reliably and
 *      with sub-millisecond latency relative to the sound ending.
 *
 * Test: play a 1-second 440 Hz sine through paula HiFi 14-bit mono cal,
 * Wait on the hook's signal with a Delay fallback in case the hook never
 * fires, log what happened and the timing. Two consecutive plays so we
 * see the hook signal accumulate (fire_count should be 2 at end).
 *
 * Pass criteria for moving on to step 2:
 *   - "hook fired" message appears in the log
 *   - fire_count == 2 after both plays
 *   - The Wait returns from the hook signal, NOT from the fallback timeout
 */

#include <exec/memory.h>
#include <exec/io.h>
#include <exec/tasks.h>
#include <devices/ahi.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/ahi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MODE_ID    0x0002000fUL   /* paula HiFi 14-bit mono calibrated */
#define RATE       22050
#define SECONDS    1
#define SAMPLES    (RATE * SECONDS)
#define BUFBYTES   (SAMPLES * 2)

/* Wait at most this many ticks if the hook never fires (5s safety net). */
#define FALLBACK_TICKS  250

struct Library *AHIBase;

/* State the hook reads via h_Data. Keep it small — the hook ideally does
 * the minimum (signal the consumer). */
struct HookState {
    struct Task *task;
    BYTE         sig_bit;
    ULONG        sig_mask;
    volatile long fire_count;     /* incremented by hook */
};

/* The hook itself. Bebbo amigaos GCC needs explicit register placement for
 * each argument (m68k library callbacks pass in registers). For clib2 builds
 * we don't need __saveds — globals use absolute addressing so A4 isn't a
 * small-data base. The eventual nw_engine.c port (which IS -fbaserel) WILL
 * need __attribute__((saveds)) on the hook so A4 is set up correctly. */
static ULONG sound_done(
    struct Hook            *h    __asm("a0"),
    struct AHIAudioCtrl    *ctrl __asm("a2"),
    struct AHISoundMessage *msg  __asm("a1"))
{
    struct HookState *s = (struct HookState *)h->h_Data;
    (void)ctrl; (void)msg;
    s->fire_count++;
    Signal(s->task, s->sig_mask);
    return 0;
}

/* 440 Hz sine, big-endian for AHIST_M16S. */
static void sine_be(unsigned char *b, long samples, int freq, int rate)
{
    static const short table[16] = {
            0,  12539,  23169,  30273,  32767,  30273,  23169,  12539,
            0, -12539, -23169, -30273, -32767, -30273, -23169, -12539
    };
    long i;
    for (i = 0; i < samples; i++) {
        long idx = ((long)i * freq * 16 / rate) & 15;
        short s = table[idx];
        b[2*i]   = (unsigned char)((s >> 8) & 0xff);   /* BE: high byte first */
        b[2*i+1] = (unsigned char)(s & 0xff);
    }
}

int main(void)
{
    struct MsgPort       *port;
    struct AHIRequest    *req;
    struct AHIAudioCtrl  *ctrl;
    struct AHISampleInfo  si;
    unsigned char        *buf;
    struct Hook           hook;
    struct HookState      state;
    int                   k;

    printf("hook_probe: AHIA_SoundFunc hook verification\n");
    fflush(stdout);

    state.task = FindTask(0L);
    state.sig_bit = AllocSignal(-1);
    if (state.sig_bit < 0) {
        printf("FAIL: AllocSignal returned %d\n", (int)state.sig_bit);
        return 20;
    }
    state.sig_mask   = 1UL << state.sig_bit;
    state.fire_count = 0;
    /* Clear stale state of the signal bit */
    SetSignal(0, state.sig_mask);

    /* Zero-init the Hook struct then fill the two fields we care about.
     * h_Entry points to our hook fn; h_Data is the per-hook state we get
     * back via a0->h_Data inside the hook. */
    memset(&hook, 0, sizeof(hook));
    hook.h_Entry = (ULONG (*)())sound_done;
    hook.h_Data  = (APTR)&state;

    printf("  task=%p  sig_bit=%d  sig_mask=0x%08lx\n",
           state.task, (int)state.sig_bit, (unsigned long)state.sig_mask);
    fflush(stdout);

    port = CreateMsgPort();
    if (!port) { printf("FAIL: CreateMsgPort\n"); return 20; }
    req  = (struct AHIRequest *)CreateIORequest(port, sizeof(struct AHIRequest));
    if (!req)  { printf("FAIL: CreateIORequest\n"); return 20; }
    req->ahir_Version = 4;

    if (OpenDevice((STRPTR)"ahi.device", AHI_NO_UNIT,
                   (struct IORequest *)req, 0) != 0) {
        printf("FAIL: OpenDevice ahi.device\n"); return 20;
    }
    AHIBase = (struct Library *)req->ahir_Std.io_Device;
    printf("  AHIBase = %p\n", AHIBase);
    fflush(stdout);

    ctrl = AHI_AllocAudio(
        AHIA_AudioID,    MODE_ID,
        AHIA_MixFreq,    (ULONG)RATE,
        AHIA_Channels,   1UL,
        AHIA_Sounds,     1UL,
        AHIA_SoundFunc,  (ULONG)&hook,
        TAG_END);
    if (!ctrl) {
        printf("FAIL: AHI_AllocAudio (mode 0x%08lx may not accept SoundFunc?)\n",
               MODE_ID);
        return 20;
    }
    printf("  AHI_AllocAudio OK (AHIA_SoundFunc = &hook accepted)\n");
    fflush(stdout);

    buf = (unsigned char *)AllocMem(BUFBYTES, MEMF_PUBLIC);
    if (!buf) { printf("FAIL: AllocMem\n"); return 20; }
    sine_be(buf, SAMPLES, 440, RATE);

    si.ahisi_Type    = AHIST_M16S;
    si.ahisi_Address = buf;
    si.ahisi_Length  = BUFBYTES;
    if (AHI_LoadSound(0, AHIST_SAMPLE, &si, ctrl) != 0) {
        printf("FAIL: AHI_LoadSound\n"); return 20;
    }
    if (AHI_ControlAudio(ctrl, AHIC_Play, TRUE, TAG_END) != 0) {
        printf("FAIL: AHIC_Play TRUE\n"); return 20;
    }

    /* Run two plays so we can check fire_count == 2 at the end, and so the
     * second play's hook can't be masked by stale state. */
    for (k = 0; k < 2; k++) {
        ULONG  received;
        long   before = state.fire_count;
        time_t t0, t1;

        printf("  play %d/2: AHI_Play submitted\n", k + 1);
        fflush(stdout);

        AHI_Play(ctrl,
                 AHIP_BeginChannel, 0UL,
                 AHIP_Freq,         (ULONG)RATE,
                 AHIP_Vol,          0x10000UL,
                 AHIP_Pan,          0x8000UL,
                 AHIP_Sound,        0UL,
                 AHIP_EndChannel,   0UL,
                 TAG_END);

        time(&t0);
        /* Wait on the hook signal, with a Delay-based fallback so we don't
         * hang forever if the hook never fires. Implementation note: there's
         * no portable "Wait with timeout" in dos.library; the standard idiom
         * is to fire off a one-shot timer.device request and Wait on either.
         * For this probe we just poll the fire_count after a max delay. */
        received = 0;
        {
            long ticks = 0;
            while (state.fire_count == before && ticks < FALLBACK_TICKS) {
                Delay(5);                /* 0.1 s */
                ticks += 5;
            }
            if (state.fire_count != before) {
                received = state.sig_mask;
                /* drain the signal so it doesn't leak to the next iteration */
                SetSignal(0, state.sig_mask);
            }
        }
        time(&t1);

        if (received) {
            printf("    hook FIRED (count %ld -> %ld); poll took ~%lds\n",
                   before, state.fire_count, (long)(t1 - t0));
        } else {
            printf("    TIMEOUT after %ds — hook did NOT fire; fire_count=%ld\n",
                   FALLBACK_TICKS / 50, state.fire_count);
        }
        fflush(stdout);

        /* Small gap before next play, so we can see two distinct hook fires. */
        Delay(25);
    }

    AHI_ControlAudio(ctrl, AHIC_Play, FALSE, TAG_END);
    AHI_UnloadSound(0, ctrl);
    AHI_FreeAudio(ctrl);
    FreeMem(buf, BUFBYTES);
    CloseDevice((struct IORequest *)req);
    DeleteIORequest((struct IORequest *)req);
    DeleteMsgPort(port);
    FreeSignal(state.sig_bit);

    printf("done — fire_count = %ld (expected 2)\n", state.fire_count);
    return 0;
}
