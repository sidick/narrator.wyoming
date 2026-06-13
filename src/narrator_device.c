/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* narrator_device.c — narrator.wyoming, the drop-in narrator.device.
 *
 * Built as a real loadable Amiga device via libnix's devinit.o framework:
 *   m68k-amigaos-gcc -nostdlib -fbaserel devinit.o narrator_device.c -o narrator.device
 *
 * libnix provides the library-management vectors (Open/Close/Expunge/ExtFunc)
 * and the ROMTag; we provide __UserDevInit/Open/Close/Cleanup and the device
 * vectors __BeginIO/__AbortIO (registered into __FuncTable__ via ADDTABL_*).
 *
 * Freestanding device — NO C runtime, NO stdio, NO malloc; exec only.
 *
 * !!! IMPORTANT: NO C GLOBAL/STATIC VARIABLES !!!
 * This is built -fbaserel (A4-relative small data), but the libnix device hooks
 * are NOT entered with A4 pointing at this device's data segment, so any access
 * to a C global reads/writes RANDOM memory and corrupts the exec free list
 * (Guru #8100 0005, AN_MemCorrupt). Confirmed on-target. So: get SysBase
 * locally from absolute address 4, and hang all per-open state off allocated
 * structures reached via the IORequest — never a file-scope variable.
 *
 * Milestone 1 (this file): load, open (unit 0), dispatch the narrator command
 * set as stubs so callers (incl. `Say`) can drive the device without faulting.
 * The Wyoming->AHI pipeline is wired in behind CMD_WRITE next.
 */

#include <exec/errors.h>
#include <exec/io.h>
#include <exec/execbase.h>
#include <devices/narrator.h>
#include <proto/exec.h>

#include "stabs.h"
#include "nw_engine.h"

/* Server address and voice mapping are read at runtime from the prefs file
 * (ENV:narrator.wyoming) via nw_read_prefs() — nothing is baked into the binary.
 * `sex` comes at runtime from the caller's narrator_rb; the prefs only hold
 * which Piper voice each of MALE/FEMALE maps to. */

/* exported identity (libnix reads these for the ROMTag; const -> in code/rodata,
 * NOT A4 data, so they're safe). */
/* Version: NW_VERSION.NW_REVISION come from version.mk via the Makefile (-D),
 * so the number lives in one place. VERSION 44 sits above every stock version
 * (Commodore narrator topped out at 37, translator at ~43) so this pair is easy
 * to tell apart AND still satisfies any minimum-version check (44 >= V33/V37);
 * we do implement the V37 extended narrator_rb / NDF_NEWIORB. The $VER string
 * lets the Version command read it; NW_BUILD_DATE is stamped at build time
 * (two-digit day/month: "(dd.mm.yyyy)"). */
#ifndef NW_VERSION
#define NW_VERSION  44
#endif
#ifndef NW_REVISION
#define NW_REVISION 0
#endif
#ifndef NW_BUILD_DATE
#define NW_BUILD_DATE "(unknown date)"
#endif
#define NW_STR_(x) #x
#define NW_STR(x)  NW_STR_(x)
#define NW_VERSTR  NW_STR(NW_VERSION) "." NW_STR(NW_REVISION)   /* "44.0" */

const char  DevName[]     = "narrator.device";
const char  DevIdString[] = "$VER: narrator.device " NW_VERSTR " " NW_BUILD_DATE "\r\n";
const UWORD DevVersion    = NW_VERSION;
const UWORD DevRevision   = NW_REVISION;

/* SysBase from address 4 — a plain absolute read, no A4 needed. Used as a local
 * so the proto/exec inlines resolve against it (never a global). */
#define EXECBASE (*(struct ExecBase **)4UL)

/* =====================================================================
 * Device lifecycle hooks (run Forbid()'d; keep them quick)
 * ===================================================================== */

int __UserDevInit(struct Device *dev)
{
    (void)dev;
    /* Nothing global to set up (see the no-globals warning above). The Piper
     * TCP connection + AHI are opened per-open in __UserDevOpen. */
    return 1;   /* libnix devinit: NON-ZERO = success (0 -> init fails) */
}

void __UserDevCleanup(void)
{
}

int __UserDevOpen(struct IORequest *io, ULONG unit, ULONG flags)
{
    struct narrator_rb *nrb = (struct narrator_rb *)io;
    struct ExecBase    *SysBase = EXECBASE;
    struct nwctx       *ctx;
    (void)flags;
    if (unit != 0) {                 /* narrator.device has only unit 0 */
        io->io_Error = ND_UnitErr;
        return ND_UnitErr;           /* non-zero -> open fails */
    }
    /* RKM: OpenDevice initializes the base narrator_rb fields to defaults so a
     * caller that doesn't set them (e.g. volume) gets sane values. These fields
     * exist in both the old and V37 rb, so it's safe regardless of NDF_NEWIORB.
     * `nrb` is the caller's struct (a parameter), not a device global. */
    nrb->rate     = DEFRATE;
    nrb->pitch    = DEFPITCH;
    nrb->mode     = DEFMODE;
    nrb->sex      = DEFSEX;
    nrb->volume   = DEFVOL;
    nrb->sampfreq = DEFFREQ;
    nrb->mouths   = 0;

    /* Spin up this open's device task (it will own the Piper connection). We run
     * Forbid()'d here, so the task can't start until we return — safe to set it
     * up. The task pointer is stashed in io_Unit; BeginIO finds it there. */
    ctx = nw_dev_open(SysBase);
    if (!ctx) {
        io->io_Error = IOERR_OPENFAIL;
        return IOERR_OPENFAIL;
    }
    io->io_Unit  = (struct Unit *)ctx;
    io->io_Error = 0;
    return 0;                        /* 0 -> open succeeds */
}

void __UserDevClose(struct IORequest *io)
{
    nw_dev_close((struct nwctx *)io->io_Unit);   /* asks the task to shut down */
    io->io_Unit = (struct Unit *)0;
}

/* =====================================================================
 * Command dispatch
 * ===================================================================== */

ADDTABL_1(__BeginIO, a1);
void __BeginIO(struct IORequest *io)
{
    struct ExecBase *SysBase = EXECBASE;   /* local, not a global */

    io->io_Error = 0;

    switch (io->io_Command) {
    case CMD_WRITE:
        /* Hand to this open's device task (it owns the connection). Asynchronous:
         * clear IOF_QUICK and DON'T reply here — the task does the synthesize +
         * AHI playback and ReplyMsg()s when done. So DoIO callers WaitIO and
         * SendIO callers get the reply later (no longer blocking BeginIO). */
        io->io_Flags &= ~IOF_QUICK;
        nw_dev_submit((struct nwctx *)io->io_Unit, io);
        return;

    case CMD_READ:
        io->io_Error = ND_NoWrite;     /* no mouth data without a write yet */
        break;

    case CMD_FLUSH:
        /* Abort every queued CMD_WRITE. The one currently being processed by
         * the task continues to natural completion (cancelling a mid-Wyoming
         * synthesize would need a flag the task polls between socket reads). */
        nw_dev_flush((struct nwctx *)io->io_Unit);
        break;

    case CMD_RESET:
    case CMD_START:
    case CMD_STOP:
    case CMD_CLEAR:
        break;                          /* nothing queued yet -> succeed */

    default:
        io->io_Error = IOERR_NOCMD;
        break;
    }

    /* Synchronous completion: for DoIO (IOF_QUICK set) leave it set and don't
     * reply; for SendIO (quick clear) reply to the port. */
    if (!(io->io_Flags & IOF_QUICK))
        ReplyMsg(&io->io_Message);
}

ADDTABL_1(__AbortIO, a1);
LONG __AbortIO(struct IORequest *io)
{
    /* Try to pull this IORequest out of the device task's queue. If it's
     * still queued we reply it with IOERR_ABORTED and return 0; if it's
     * already in-flight (or done) there's nothing we can do — return -1 so
     * the caller's WaitIO sees natural completion. */
    if (!io || !io->io_Unit) return -1;
    return nw_dev_abort((struct nwctx *)io->io_Unit, io);
}

ADDTABL_END();
