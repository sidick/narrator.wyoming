/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Simon Dick
 */

/* codesets_proto.h — minimal proto/codesets.h shim.
 *
 * narrator.wyoming opens codesets.library at runtime to transcode caller
 * text from the system codeset (typically ISO-8859-1 on classic Amigas) to
 * UTF-8 before it goes into Piper's Wyoming JSON. Bebbo's m68k-amigaos NDK
 * doesn't ship the codesets headers, so we declare the small subset we use
 * here. Tag IDs and LVO offsets are the upstream library's public ABI
 * (jens-maus/libcodesets, LGPL — we use the ABI, we don't include its
 * sources).
 *
 * Calls go via the same LP* macro mechanism the NDK uses for exec/dos/etc.,
 * so callers must have a `struct Library *CodesetsBase` in scope when they
 * invoke any of these macros (same pattern as SocketBase / DOSBase). */

#ifndef NARRATOR_CODESETS_PROTO_H
#define NARRATOR_CODESETS_PROTO_H

#include <exec/types.h>
#include <utility/tagitem.h>
#include <inline/macros.h>

/* Opaque codeset descriptor; the library hands us pointers to these and we
 * only ever pass them back via tags — no field access needed on our side. */
struct codeset;

/* Tag IDs we use. Upstream defines them as 0xfec901f4 + n. */
#define CODESETSLIB_TAG(n)   ((ULONG)0xfec901f4 + (n))
#define CSA_SourceLen        CODESETSLIB_TAG(1)
#define CSA_Source           CODESETSLIB_TAG(2)
#define CSA_DestLenPtr       CODESETSLIB_TAG(6)
#define CSA_SourceCodeset    CODESETSLIB_TAG(7)
#define CSA_DestCodeset      CODESETSLIB_TAG(13)

/* Library identification. */
#define CODESETSNAME "codesets.library"

#ifndef CODESETS_BASE_NAME
#define CODESETS_BASE_NAME CodesetsBase
#endif

/* LVO offsets are from upstream include/inline/codesets.h. The LP* macros
 * (defined in <inline/macros.h>, included above) handle the m68k library-call
 * setup: a6 = CODESETS_BASE_NAME, args in the listed registers, jsr -off(a6),
 * return in d0. Keep this list to functions we actually use. */

/* APTR CodesetsFindA(CONST_STRPTR name, struct TagItem *attrs); */
#define CodesetsFindA(name, attrs) \
    LP2(0x66, struct codeset *, CodesetsFindA, \
        CONST_STRPTR, name, a0, struct TagItem *, attrs, a1, \
        , CODESETS_BASE_NAME)

/* STRPTR CodesetsConvertStrA(struct TagItem *tags); */
#define CodesetsConvertStrA(tags) \
    LP1(0xa2, STRPTR, CodesetsConvertStrA, \
        struct TagItem *, tags, a0, \
        , CODESETS_BASE_NAME)

/* void CodesetsFreeA(APTR obj, struct TagItem *attrs); */
#define CodesetsFreeA(obj, attrs) \
    LP2NR(0x5a, CodesetsFreeA, \
        APTR, obj, a0, struct TagItem *, attrs, a1, \
        , CODESETS_BASE_NAME)

#endif /* NARRATOR_CODESETS_PROTO_H */
