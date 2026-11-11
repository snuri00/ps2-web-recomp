#!/usr/bin/env python3
"""Seed every captured code window image with the functions it holds.

usage: seed_windows.py <overlays dir> <entries file>

Exploring finds a function of a window only once a run calls it, and a game
that reaches most of them through pointers costs a run per function. A
function the compiler emitted starts right after the previous one returned,
with its frame set up first:

    jr    ra
    <delay slot>
    [nop ...]             alignment padding, a few words at most
    addiu sp, sp, -N      <- a function

A leaf function sets up no frame, so the first word there counts as well
when another jr ra follows within LEAF_REACH words: data rarely holds one.

Some compilers (MGS3's) end a function with a branch back into a shared
epilogue or a tail jump instead, so a frame set up right after b, j or jr of
any register counts too. Those also end blocks inside a function, so a leaf
counts there only after alignment padding.

so every such spot in an image becomes an entry token <image>:<offset>. The
recompiler still rejects a seed that does not decode as code.
"""
import bisect
import os
import struct
import sys

JR_RA = 0x03E00008
MAX_PADDING = 4
LEAF_REACH = 256


def ends_function(w):
    return ((w & 0xFC1FFFFF) == 0x00000008     # jr rs
            or w >> 16 == 0x1000                 # b (beq zero, zero)
            or w >> 26 == 2)                     # j


def functions(blob):
    words = len(blob) // 4
    ws = struct.unpack_from('<%dI' % words, blob)
    returns = [k for k, w in enumerate(ws) if w == JR_RA]
    out = []
    for k in range(words - 2):
        end = ws[k]
        if end != JR_RA and not ends_function(end):
            continue
        at = k + 2
        while at < min(words, k + 2 + MAX_PADDING) and ws[at] == 0:
            at += 1
        if at >= words:
            continue
        w = ws[at]
        # addiu sp, sp, -N  (op 9, rs = rt = 29, negative immediate)
        if w >> 16 == 0x27BD and w & 0x8000:
            out.append(4 * at)
        # padding after a b/j/jr marks a function boundary as well, so a leaf counts there too
        elif (end == JR_RA or at > k + 2) and w and w != JR_RA:
            i = bisect.bisect_right(returns, at)
            if i < len(returns) and returns[i] < at + LEAF_REACH:
                out.append(4 * at)
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    folder, entries_path = sys.argv[1:]
    known = set(open(entries_path).read().split()) if os.path.exists(entries_path) else set()
    added = []
    for name in sorted(os.listdir(folder)):
        if not name.endswith('.bin'):
            continue
        stem = name[:-4]
        blob = open(os.path.join(folder, name), 'rb').read()
        for offset in functions(blob):
            token = '%s:%x' % (stem, offset)
            if token not in known:
                known.add(token)
                added.append(token)
    with open(entries_path, 'a') as out:
        for token in added:
            out.write(token + '\n')
    print(len(added))


if __name__ == '__main__':
    main()
