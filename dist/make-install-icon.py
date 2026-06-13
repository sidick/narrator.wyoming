#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Simon Dick
"""
Generate dist/Install.info — an Amiga Workbench Project icon for the Install
script.

The icon is a tiny stylised speaker emitting sound waves (it's a narrator).
Double-clicking it from Workbench runs the Install script via `C:IconX`, the
stock AmigaDOS script-runner Workbench expects for script icons.

Output: a binary .info file in classic OS 1.x/2.x DiskObject format. Two
bitplanes (4-colour palette: bg/black/white/blue using the standard
Workbench colour set). No ToolTypes, no DrawerData, no SelectRender.

Re-run this whenever you change the art:
    python3 dist/make-install-icon.py
and commit dist/Install.info alongside the script change.
"""

import struct
from pathlib import Path

# ---------------------------------------------------------------------------
# Pixel art (24 x 16, two-colour). '#' = colour 3 (foreground), ' ' = colour 1
# (background — Workbench's default light shade). The outer frame uses '#'
# too so the icon has a clear silhouette on any palette.
# ---------------------------------------------------------------------------
PIXELS = [
    "########################",   # 0  top border
    "#                      #",   # 1
    "#  ##                  #",   # 2  speaker stem
    "#  ##                  #",   # 3
    "#  ###    #     #      #",   # 4  cone + first arc
    "#  ####   ##    ##     #",   # 5
    "#  #####  ###   ###    #",   # 6
    "#  ######   #     #    #",   # 7
    "#  ######   #     #    #",   # 8
    "#  #####  ###   ###    #",   # 9
    "#  ####   ##    ##     #",   # 10
    "#  ###    #     #      #",   # 11
    "#  ##                  #",   # 12
    "#  ##                  #",   # 13
    "#                      #",   # 14
    "########################",   # 15  bottom border
]
assert all(len(r) == 24 for r in PIXELS), "pixel rows must all be 24 chars"
assert len(PIXELS) == 16, "icon must be 16 rows tall"

WIDTH, HEIGHT, DEPTH = 24, 16, 2

# 4-colour Workbench mapping per palette index. The icon paints colour 3 on
# the foreground pixels, leaving the background (colour 1 = light grey).
FG_COLOUR = 3   # binary 11 — sets both planes


def rasterise_planes(rows, width, height, depth, fg_colour):
    """Return depth bitplanes, each plane is a list of rows, each row a list
    of 16-bit big-endian words. Bits go MSB-left within each word."""
    words_per_row = (width + 15) // 16
    planes = [[] for _ in range(depth)]
    for r in rows:
        plane_rows = [[0] * words_per_row for _ in range(depth)]
        for x in range(width):
            if r[x] == '#':
                for p in range(depth):
                    if (fg_colour >> p) & 1:
                        word_idx = x // 16
                        bit_idx = 15 - (x % 16)
                        plane_rows[p][word_idx] |= (1 << bit_idx)
        for p in range(depth):
            planes[p].append(plane_rows[p])
    return planes


def build_image(width, height, depth, planes):
    """Image struct from intuition/intuition.h followed by raw bitplane data.

    struct Image {
        WORD  LeftEdge, TopEdge, Width, Height, Depth;   /* 10 bytes */
        APTR  ImageData;                                  /* 4  */
        UBYTE PlanePick, PlaneOnOff;                      /* 2  */
        struct Image *NextImage;                          /* 4  */
    };                                                    /* total 20 */
    """
    hdr = struct.pack('>5H', 0, 0, width, height, depth)
    hdr += struct.pack('>I', 0x40000000)              # ImageData placeholder
    hdr += struct.pack('>BB', (1 << depth) - 1, 0)    # PlanePick = all planes
    hdr += struct.pack('>I', 0)                       # NextImage = NULL
    assert len(hdr) == 20
    data = b''
    for plane in planes:
        for row in plane:
            for word in row:
                data += struct.pack('>H', word)
    return hdr + data


def build_gadget(width, height):
    """Embedded struct Gadget — most pointers are placeholders that Workbench
    rewrites when it loads the icon. The non-NULL GadgetRender slot is what
    tells the loader "yes, there's an image after the header"."""
    g = struct.pack('>I', 0)                          # NextGadget
    g += struct.pack('>4H', 0, 0, width, height)      # LeftEdge, TopEdge, W, H
    g += struct.pack('>H', 0x0004)                    # Flags  = GADGIMAGE
    g += struct.pack('>H', 0x0003)                    # Activation
    g += struct.pack('>H', 0x0001)                    # GadgetType = BOOLGADGET
    g += struct.pack('>I', 0x40000000)                # GadgetRender (placeholder)
    g += struct.pack('>I', 0)                         # SelectRender
    g += struct.pack('>I', 0)                         # GadgetText
    g += struct.pack('>i', 0)                         # MutualExclude
    g += struct.pack('>I', 0)                         # SpecialInfo
    g += struct.pack('>H', 0)                         # GadgetID
    g += struct.pack('>I', 1)                         # UserData = 1 (OS2+)
    assert len(g) == 44
    return g


def build_string_block(s: str) -> bytes:
    """A string in icon data is a ULONG byte-length (including the NUL) then
    the bytes. Used for DefaultTool, ToolWindow, and each ToolTypes entry."""
    raw = s.encode('latin-1') + b'\x00'
    return struct.pack('>I', len(raw)) + raw


def main():
    planes = rasterise_planes(PIXELS, WIDTH, HEIGHT, DEPTH, FG_COLOUR)
    image = build_image(WIDTH, HEIGHT, DEPTH, planes)

    WBPROJECT = 4
    header = struct.pack('>HH', 0xE310, 0x0001)       # magic, version
    header += build_gadget(WIDTH, HEIGHT)
    header += struct.pack('>BB', WBPROJECT, 0)        # do_Type, pad
    header += struct.pack('>I', 0x40000000)           # do_DefaultTool placeholder
    header += struct.pack('>I', 0)                    # do_ToolTypes
    # NO_ICON_POSITION = 0x80000000 (libraries/workbench.h). Tells Workbench
    # to auto-place the icon when the drawer opens, rather than pinning it
    # to (0,0) which would dump it in the top-left corner.
    NO_ICON_POSITION = -0x80000000
    header += struct.pack('>i', NO_ICON_POSITION)     # do_CurrentX
    header += struct.pack('>i', NO_ICON_POSITION)     # do_CurrentY
    header += struct.pack('>I', 0)                    # do_DrawerData (project)
    header += struct.pack('>I', 0)                    # do_ToolWindow
    header += struct.pack('>i', 4096)                 # do_StackSize for IconX
    assert len(header) == 78

    default_tool = build_string_block("C:IconX")

    out = header + image + default_tool
    Path(__file__).resolve().parent.joinpath("Install.info").write_bytes(out)
    print(f"wrote dist/Install.info ({len(out)} bytes)")


if __name__ == "__main__":
    main()
