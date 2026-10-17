#!/usr/bin/env python3
"""Code window support: the segments a game rewrites at runtime.

An executable LOAD segment with no file contents is a code window: the game
unpacks a different image into it as it goes, so no single translation of the
area can be right. The recompiler instead takes one module per image of the
window, and the runtime picks the image in place by its check words.

  windows.py list <elf>              print "base size" per window, one per line
  windows.py capture <elf> <dump> <dir> [<misses file>]
                                     save the image of every window found in
                                     the RAM dump to <dir>/<base>_<crc>.bin,
                                     and rewrite plain entry misses inside a
                                     window as <base>_<crc>:<offset>, naming
                                     the image the dump holds

The RAM dump is the one the runner writes with PS2_DUMP, so at a halt it holds
exactly the image the game was running when it called the missing entry.
"""
import binascii
import os
import struct
import sys

UNIT = 0x4000


def load_phdrs(elf):
    phoff, = struct.unpack_from('<I', elf, 28)
    phentsize, phnum = struct.unpack_from('<HH', elf, 42)
    for i in range(phnum):
        yield struct.unpack_from('<8I', elf, phoff + i * phentsize)


def windows(elf):
    out = []
    for v in load_phdrs(elf):
        if v[0] != 1 or v[4] or not (v[6] & 1):  # no PT_LOAD, has contents, not executable
            continue
        base = v[2] & ~(UNIT - 1)
        while base in [b for b, _ in out]:
            base += UNIT
        out.append((base, UNIT))
    return out


def extent(dump, base, cap=0x40000):
    # The ELF only says the window begins here; what the game unpacks decides
    # how far it reaches, so the image covers the units it wrote, stopping at
    # the first entirely empty one.
    size = 0
    while size < cap and size + UNIT <= len(dump) - base:
        if dump[base + size:base + size + UNIT] == b'\0' * UNIT:
            break
        size += UNIT
    return size or UNIT


def main() -> None:
    if len(sys.argv) not in (3, 5, 6) or sys.argv[1] not in ('list', 'capture'):
        sys.exit(__doc__)
    mode = sys.argv[1]
    elf = open(sys.argv[2], 'rb').read()
    wins = windows(elf)
    if mode == 'list':
        for base, size in wins:
            print('%08x %x' % (base, size))
        return

    dump = open(sys.argv[3], 'rb').read()
    outdir = sys.argv[4]
    misses = sys.argv[5] if len(sys.argv) > 5 else None
    for base, _ in wins:
        size = extent(dump, base)
        blob = dump[base:base + size]
        if not any(blob):
            continue
        crc = binascii.crc32(blob) & 0xFFFFFFFF
        stem = '%08X_%08X' % (base, crc)
        path = os.path.join(outdir, stem + '.bin')
        if not os.path.exists(path):
            os.makedirs(outdir, exist_ok=True)
            open(path, 'wb').write(blob)
            print('captured %s, %d bytes' % (stem, size))
        if misses and os.path.exists(misses) and blob == open(path, 'rb').read():
            lines = open(misses).read().split()
            names = {}
            for k, token in enumerate(lines):
                if ':' in token:
                    continue
                pc = int(token, 16)
                if base <= pc < base + size:
                    names[token] = '%s:%x' % (stem, pc - base)
            if names:
                open(misses, 'w').write('\n'.join(names.get(t, t) for t in lines) + '\n')


if __name__ == '__main__':
    main()