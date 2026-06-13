# Vendored upstream: codesets.library headers

These four headers are taken verbatim from
[jens-maus/libcodesets](https://github.com/jens-maus/libcodesets), the
canonical AmigaOS / MorphOS / AROS implementation of `codesets.library`.
narrator.wyoming opens the library at runtime to transcode caller text
from the system codeset (typically ISO-8859-1 on classic Amigas) into
the UTF-8 that the Wyoming JSON requires.

We vendor the headers (rather than depending on a system-installed SDK
copy) because Bebbo's `m68k-amigaos-gcc` cross-toolchain does not ship
the codesets headers, and because consumers of the library should pin
to a known interface revision rather than whatever the build host
happens to have.

## Files

| Path | Upstream path |
|---|---|
| `include/proto/codesets.h`        | `include/proto/codesets.h` |
| `include/libraries/codesets.h`    | `include/libraries/codesets.h` |
| `include/clib/codesets_protos.h`  | `include/clib/codesets_protos.h` |
| `include/inline/codesets.h`       | `include/inline/codesets.h` |

The headers depend on `<inline/macros.h>`, `<exec/types.h>`, and
`<utility/tagitem.h>` from the Bebbo NDK — same toolchain headers our
other proto/* includes already use.

## Source revision

Vendored from upstream commit
[`34442b75bb42666f5d133d74db3350d2159c0a65`](https://github.com/jens-maus/libcodesets/commit/34442b75bb42666f5d133d74db3350d2159c0a65)
on 2026-06-13. If you refresh these files, update this README + the
commit hash, and re-run the on-target devtest (write 3 exercises the
ISO-8859-1 → UTF-8 transcoding path through `CodesetsConvertStrA`).

## License

The headers are licensed under the **GNU Lesser General Public License
v2.1 or later**; see `LICENSE` in this directory. The original
copyright notices are preserved in each header verbatim:

> Copyright (C) 2001-2005 by Alfonso [alfie] Ranieri <alforan@tin.it>.
> Copyright (C) 2005-2021 codesets.library Open Source Team

LGPL-licensed interface headers are explicitly intended to be included
in non-LGPL programs that link against the library — that's the whole
point of the "Lesser" variant. narrator.wyoming itself remains MIT
licensed; including these headers does not change that.
