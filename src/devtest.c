/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* devtest.c — exercise narrator.device directly.
 *
 * A normal CLI program (clib2) that drives narrator.device through several
 * targeted scenarios.  Each phase prints what it found; the log is read back
 * from the host to confirm the device behaved correctly on-target.
 *
 *   Phase 1 — Open/Close stress (no CMD_WRITE between cycles).  Covers the
 *             signal-handshake lifecycle (parent allocates ready/dead bits;
 *             task signals on startup + exit).  A deadlock or leak in that
 *             path would surface here long before any CMD_WRITE-related test
 *             gets to run.
 *
 *   Phase 2 — Two async SendIO writes back-to-back over one held connection.
 *             Volume mapping (full vs ~quarter) included.
 *
 *   Phase 3 — One CMD_WRITE with literal ISO-8859-1 bytes ('\xe9', '\xfc').
 *             Routes through codesets.library if installed; the result
 *             confirms the UTF-8 detection + transcoding path.
 *
 *   Phase 4 — AbortIO + CMD_FLUSH on a deliberately stuffed queue.  Queues
 *             four writes under Forbid so the device task can't preempt and
 *             dequeue; AbortIO targets one and CMD_FLUSH sweeps the rest.
 *             WaitIO on each confirms io_Error = IOERR_ABORTED on the
 *             cancelled requests and normal completion on the one we kept.
 *
 *   Phase 5 — One long CMD_WRITE with embedded quoted clauses.  Exercises
 *             nw_split's closing-quote peel (so the split lands on the
 *             punctuation inside the quote, not the quote itself).
 *             Requires `split_words` > 0 in ENV:narrator.wyoming to
 *             actually traverse the split path.
 */

#include <exec/types.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <devices/narrator.h>
#include <proto/exec.h>

#include <stdio.h>
#include <string.h>

#define STRESS_CYCLES 10

/* Phase 1: Open/Close stress -- no CMD_WRITE between cycles. */
static int phase_open_close_stress(struct narrator_rb *io)
{
    int i, ok = 0;
    for (i = 0; i < STRESS_CYCLES; i++) {
        BYTE err = OpenDevice((STRPTR)"narrator.device", 0,
                              (struct IORequest *)io, 0);
        if (err == 0) {
            CloseDevice((struct IORequest *)io);
            ok++;
        } else {
            printf("  cycle %d: OpenDevice -> %d\n", i, (int)err);
        }
    }
    printf("Phase 1 Open/Close stress: %d/%d cycles ok%s\n",
           ok, STRESS_CYCLES, ok == STRESS_CYCLES ? "" : " (FAIL)");
    fflush(stdout);
    return ok == STRESS_CYCLES;
}

/* Phase 2: two async SendIO writes back-to-back. */
static void phase_async_queue(struct MsgPort *mp, struct narrator_rb *io)
{
    char *t1 = "This sentence is spoken at full volume.";
    char *t2 = "And this one is much quieter, at about one quarter volume.";
    struct narrator_rb *io2 =
        (struct narrator_rb *)CreateIORequest(mp, sizeof(struct narrator_rb));

    io->volume              = MAXVOL;
    io->message.io_Command  = CMD_WRITE;
    io->message.io_Data     = (APTR)t1;
    io->message.io_Length   = strlen(t1);

    io2->message.io_Device  = io->message.io_Device;
    io2->message.io_Unit    = io->message.io_Unit;
    io2->sex                = io->sex;
    io2->volume             = 16;
    io2->message.io_Command = CMD_WRITE;
    io2->message.io_Data    = (APTR)t2;
    io2->message.io_Length  = strlen(t2);

    SendIO((struct IORequest *)io);
    SendIO((struct IORequest *)io2);
    printf("Phase 2 both SendIO returned immediately (async)\n");
    fflush(stdout);

    WaitIO((struct IORequest *)io);
    printf("  write1 (vol 64) done: io_Error=%d io_Actual=%lu\n",
           (int)io->message.io_Error, (unsigned long)io->message.io_Actual);
    WaitIO((struct IORequest *)io2);
    printf("  write2 (vol 16) done: io_Error=%d io_Actual=%lu\n",
           (int)io2->message.io_Error, (unsigned long)io2->message.io_Actual);

    DeleteIORequest((struct IORequest *)io2);
}

/* Phase 3: ISO-8859-1 input -- codesets transcoding path. */
static void phase_codesets(struct narrator_rb *io)
{
    char *t3 = "A caf\xe9 in M\xfcnchen.";   /* é + ü literal ISO-8859-1 */
    io->volume              = MAXVOL;
    io->message.io_Command  = CMD_WRITE;
    io->message.io_Data     = (APTR)t3;
    io->message.io_Length   = strlen(t3);
    DoIO((struct IORequest *)io);
    printf("Phase 3 ISO-8859-1 (%ld bytes in): io_Error=%d io_Actual=%lu\n",
           (long)strlen(t3),
           (int)io->message.io_Error, (unsigned long)io->message.io_Actual);
    fflush(stdout);
}

/* Phase 4: AbortIO + CMD_FLUSH.  Two sub-phases:
 *
 *   4a: send io[0] alone and let it run to natural completion.  Proves the
 *       "kept" path -- a write that isn't on the queue when AbortIO/Flush
 *       fires is unaffected.  Earlier rev of this test queued io[0] with
 *       the others under Forbid; the next phase's CMD_FLUSH then swept it
 *       out too (correctly -- flush is documented as "kill the queue"),
 *       which gave a misleading IOERR_ABORTED on the "kept" io.
 *
 *   4b: queue io[1], io[2], io[3] under Forbid so the device task can't
 *       preempt and dequeue.  AbortIO targets io[2]; CMD_FLUSH sweeps
 *       io[1] and io[3].  Each comes back from WaitIO with
 *       io_Error == IOERR_ABORTED (-2).
 *
 * Exec's AbortIO() inline is declared void in Bebbo's NDK (per the RKM
 * "Returns: nothing" convention) -- the device's __AbortIO does return
 * the abort status, but the exec wrapper discards it.  Rely on the
 * post-WaitIO io_Error as the user-visible signal instead. */
static void phase_abortio_flush(struct MsgPort *mp, struct narrator_rb *io)
{
    char *t = "This is a short utterance, three.";
    struct narrator_rb *ios[3];
    struct narrator_rb *fio;
    int  i;

    /* --- 4a: the "kept" write completes normally. --- */
    io->volume              = MAXVOL;
    io->message.io_Command  = CMD_WRITE;
    io->message.io_Data     = (APTR)t;
    io->message.io_Length   = strlen(t);
    DoIO((struct IORequest *)io);
    printf("Phase 4a kept write: io_Error=%d io_Actual=%lu\n",
           (int)io->message.io_Error, (unsigned long)io->message.io_Actual);
    fflush(stdout);

    /* --- 4b: three queued writes; abort #1, flush #0 and #2. --- */
    for (i = 0; i < 3; i++) {
        ios[i] = (struct narrator_rb *)
                 CreateIORequest(mp, sizeof(struct narrator_rb));
        ios[i]->message.io_Device  = io->message.io_Device;
        ios[i]->message.io_Unit    = io->message.io_Unit;
        ios[i]->sex                = io->sex;
        ios[i]->volume             = MAXVOL;
        ios[i]->message.io_Command = CMD_WRITE;
        ios[i]->message.io_Data    = (APTR)t;
        ios[i]->message.io_Length  = strlen(t);
    }
    fio = (struct narrator_rb *)
          CreateIORequest(mp, sizeof(struct narrator_rb));
    fio->message.io_Device  = io->message.io_Device;
    fio->message.io_Unit    = io->message.io_Unit;
    fio->message.io_Command = CMD_FLUSH;

    Forbid();
    for (i = 0; i < 3; i++)
        SendIO((struct IORequest *)ios[i]);
    AbortIO((struct IORequest *)ios[1]);
    DoIO((struct IORequest *)fio);                 /* sync; returns at once */
    Permit();

    printf("Phase 4b AbortIO/CMD_FLUSH (3 writes queued)\n");
    fflush(stdout);

    for (i = 0; i < 3; i++) {
        const char *tag = (i == 1) ? "aborted" : "flushed";
        WaitIO((struct IORequest *)ios[i]);
        printf("  io[%d] (%s): io_Error=%d io_Actual=%lu\n",
               i, tag,
               (int)ios[i]->message.io_Error,
               (unsigned long)ios[i]->message.io_Actual);
    }

    for (i = 0; i < 3; i++) DeleteIORequest((struct IORequest *)ios[i]);
    DeleteIORequest((struct IORequest *)fio);
}

/* Phase 5: long quoted-clause text -- exercises nw_split's closing-quote
 * peel when split_words > 0 in prefs.  Functional only (we don't observe
 * the split timing here); a non-zero io_Error or zero io_Actual would
 * flag a regression. */
static void phase_quoted_split(struct narrator_rb *io)
{
    char *q =
        "He paused, then said, \"Welcome to the Amiga.\" "
        "She replied with a smile, \"Yes, the future feels like the past.\" "
        "The room fell quiet for a moment.";
    io->volume              = 24;
    io->message.io_Command  = CMD_WRITE;
    io->message.io_Data     = (APTR)q;
    io->message.io_Length   = strlen(q);
    DoIO((struct IORequest *)io);
    printf("Phase 5 quoted+split (%ld bytes in): io_Error=%d io_Actual=%lu\n",
           (long)strlen(q),
           (int)io->message.io_Error, (unsigned long)io->message.io_Actual);
    fflush(stdout);
}

/* flush_mem(): equivalent of CLI `Avail FLUSH`. AllocMem(~0UL, ...) always
 * fails (no Amiga has 4 GB of free RAM), but on its way to failure exec
 * walks every loaded library with LIBF_DELEXP + refcount 0 and calls its
 * Expunge() vector. We bracket each AvailMem reading with a flush so
 * "before" and "after" are taken in the same maximally-collected state --
 * otherwise codesets.library / bsdsocket.library staying loaded across
 * cycles would skew the delta. */
static void flush_mem(void)
{
    APTR dummy = AllocMem(~0UL, MEMF_ANY);
    (void)dummy;       /* always NULL on every real machine; nothing to free */
}

/* Phase 6: Resource-leak audit -- 100 Open/Close cycles, no CMD_WRITE in
 * between. Each cycle creates and tears down a device task, opens and closes
 * bsdsocket.library + codesets.library (when present), allocates and frees
 * struct NW, allocates and frees two signal bits. A leak in any of those
 * paths accumulates over 100 cycles and shows as a non-zero AvailMem delta.
 *
 * Both snapshots are taken after a flush_mem() so DELEXP-pending libraries
 * have been swept. "Delta = 0" is the ideal; a few hundred bytes from
 * interrupt activity is noise. Single-digit-KB deltas warrant investigation;
 * >10 KB is almost certainly a leak. */
static void phase_leak_open_close(struct narrator_rb *io)
{
    const int N = 100;
    ULONG before, after;
    int   i, ok = 0;

    flush_mem();
    before = AvailMem(MEMF_ANY);
    for (i = 0; i < N; i++) {
        BYTE err = OpenDevice((STRPTR)"narrator.device", 0,
                              (struct IORequest *)io, 0);
        if (err == 0) {
            CloseDevice((struct IORequest *)io);
            ok++;
        }
    }
    flush_mem();
    after = AvailMem(MEMF_ANY);

    printf("Phase 6 leak audit (Open/Close x %d): %d ok  AvailMem delta = %ld bytes\n",
           N, ok, (long)before - (long)after);
    fflush(stdout);
}

/* Phase 7: Resource-leak audit -- 10 Open + CMD_WRITE + Close cycles. Each
 * exercises the full per-write allocation path on a fresh session: bsdsocket
 * connect, Wyoming request buffer alloc/free, PCM streaming, AHI open/close.
 * A per-write leak would accumulate ~10x its size; the per-session
 * allocations get one more sweep here on top of phase 6. */
static void phase_leak_write_cycle(struct narrator_rb *io)
{
    const int N = 10;
    char *t = "Quick.";
    ULONG before, after;
    int   i, ok = 0;

    flush_mem();
    before = AvailMem(MEMF_ANY);
    for (i = 0; i < N; i++) {
        BYTE err = OpenDevice((STRPTR)"narrator.device", 0,
                              (struct IORequest *)io, 0);
        if (err == 0) {
            io->volume              = MAXVOL;
            io->message.io_Command  = CMD_WRITE;
            io->message.io_Data     = (APTR)t;
            io->message.io_Length   = strlen(t);
            DoIO((struct IORequest *)io);
            CloseDevice((struct IORequest *)io);
            ok++;
        }
    }
    flush_mem();
    after = AvailMem(MEMF_ANY);

    printf("Phase 7 leak audit (Open+CMD_WRITE+Close x %d): %d ok  AvailMem delta = %ld bytes\n",
           N, ok, (long)before - (long)after);
    fflush(stdout);
}

int main(void)
{
    struct MsgPort     *mp;
    struct narrator_rb *io;
    BYTE   openErr;
    struct Task *me = FindTask(NULL);

    printf("stack: %ld bytes (SPLower=%p SPUpper=%p)\n",
           (long)((char *)me->tc_SPUpper - (char *)me->tc_SPLower),
           me->tc_SPLower, me->tc_SPUpper);
    fflush(stdout);

    mp = CreateMsgPort();
    if (!mp) { printf("CreateMsgPort failed\n"); return 1; }

    io = (struct narrator_rb *)CreateIORequest(mp, sizeof(struct narrator_rb));
    if (!io) { printf("CreateIORequest failed\n"); DeleteMsgPort(mp); return 1; }

    io->flags = NDF_NEWIORB;             /* V37 extended rb */

    /* --- Phase 1: Open/Close stress (own opens, no other I/O between) --- */
    if (!phase_open_close_stress(io)) {
        printf("stress failed; skipping CMD_WRITE phases\n");
        DeleteIORequest((struct IORequest *)io);
        DeleteMsgPort(mp);
        return 1;
    }

    /* --- Open a session and run the CMD_WRITE / AbortIO phases on it --- */
    openErr = OpenDevice((STRPTR)"narrator.device", 0,
                         (struct IORequest *)io, 0);
    printf("OpenDevice (session for phases 2-5) -> %d\n", (int)openErr);
    fflush(stdout);

    if (openErr == 0) {
        phase_async_queue(mp, io);
        phase_codesets(io);
        phase_abortio_flush(mp, io);
        phase_quoted_split(io);
        CloseDevice((struct IORequest *)io);
        printf("CloseDevice ok\n");
    }

    /* --- Phases 6,7: leak audit (own opens; device must be closed first) --- */
    phase_leak_open_close(io);
    phase_leak_write_cycle(io);

    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(mp);
    printf("devtest done\n");
    return 0;
}
