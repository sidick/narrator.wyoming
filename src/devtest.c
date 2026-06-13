/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* devtest.c — exercise narrator.device directly.
 *
 * A normal CLI program (clib2): open narrator.device by name from DEVS:,
 * issue a CMD_WRITE, a CMD_READ, and close. Confirms the device loads, opens,
 * dispatches commands, and completes IORequests without faulting. Logs results
 * so the on-target run can be read back from the host.
 */
#include <exec/types.h>
#include <exec/io.h>
#include <exec/tasks.h>
#include <devices/narrator.h>
#include <proto/exec.h>

#include <stdio.h>
#include <string.h>

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

    io->flags = NDF_NEWIORB;                 /* request V37 extended rb */
    openErr = OpenDevice((STRPTR)"narrator.device", 0, (struct IORequest *)io, 0);
    printf("OpenDevice(narrator.device, unit 0) -> %d\n", (int)openErr);

    if (openErr == 0) {
        /* Async + queue demo (M5): SendIO two writes back-to-back. BeginIO must
         * return immediately (the task does the work), and the two requests are
         * processed in order over one held connection. Also shows the volume
         * map (first full, second ~quarter). io2 is a clone of io (same
         * device/unit) — the standard way to have two requests in flight. */
        char *t1 = "This sentence is spoken at full volume.";
        char *t2 = "And this one is much quieter, at about one quarter volume.";
        struct narrator_rb *io2 =
            (struct narrator_rb *)CreateIORequest(mp, sizeof(struct narrator_rb));

        io->volume = MAXVOL;
        io->message.io_Command = CMD_WRITE;
        io->message.io_Data    = (APTR)t1;
        io->message.io_Length  = strlen(t1);

        io2->message.io_Device = io->message.io_Device;   /* clone the open */
        io2->message.io_Unit   = io->message.io_Unit;
        io2->sex      = io->sex;
        io2->volume   = 16;
        io2->message.io_Command = CMD_WRITE;
        io2->message.io_Data    = (APTR)t2;
        io2->message.io_Length  = strlen(t2);

        SendIO((struct IORequest *)io);                   /* async */
        SendIO((struct IORequest *)io2);
        printf("both SendIO returned immediately (async; queued in the task)\n");
        fflush(stdout);

        WaitIO((struct IORequest *)io);
        printf("write1 (vol 64) done: io_Error=%d io_Actual=%lu\n",
               (int)io->message.io_Error, (unsigned long)io->message.io_Actual);
        WaitIO((struct IORequest *)io2);
        printf("write2 (vol 16) done: io_Error=%d io_Actual=%lu\n",
               (int)io2->message.io_Error, (unsigned long)io2->message.io_Actual);

        /* Codesets path: literal ISO-8859-1 bytes in the string (the \xNN
         * escapes compile as the raw byte). The engine's UTF-8 detector
         * should reject this as not-valid-UTF-8 and route it through
         * codesets.library for transcoding to UTF-8 before Piper sees it.
         * If transcoding is wired up, io_Error=0 and io_Actual>0. If it's
         * skipped, Piper's JSON parser rejects the invalid UTF-8 and we
         * get a non-zero io_Error. */
        {
            char *t3 = "A caf\xe9 in M\xfcnchen.";   /* é + ü in ISO-8859-1 */
            io->volume = MAXVOL;
            io->message.io_Command = CMD_WRITE;
            io->message.io_Data    = (APTR)t3;
            io->message.io_Length  = strlen(t3);
            DoIO((struct IORequest *)io);
            printf("write3 (ISO-8859-1, %ld bytes in): io_Error=%d io_Actual=%lu\n",
                   (long)strlen(t3),
                   (int)io->message.io_Error, (unsigned long)io->message.io_Actual);
        }

        CloseDevice((struct IORequest *)io);
        DeleteIORequest((struct IORequest *)io2);
        printf("CloseDevice ok\n");
    }

    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(mp);
    printf("devtest done\n");
    return 0;
}
