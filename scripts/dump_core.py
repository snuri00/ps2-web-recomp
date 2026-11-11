#!/usr/bin/env python3
"""Turn a program an executable unpacks into RAM into an ELF the recompiler can read.

usage: dump_core.py <ram dump> <start> <end> <entry> <output>

Some boot executables are only a decompressor: they unpack the real program
into low RAM and jump to it, so the code to translate never exists on disc.
The RAM dump the runner writes with PS2_DUMP when it halts at that jump holds
the program exactly as unpacked; this saves start..end of it (hex addresses)
as one segment entered at <entry>, for games/<name>/core.elf.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gt4_core import elf  # noqa: E402


def main():
    if len(sys.argv) != 6:
        sys.exit(__doc__)
    dump = open(sys.argv[1], 'rb').read()
    start, end, entry = (int(v, 16) for v in sys.argv[2:5])
    if not start <= entry < end <= len(dump):
        sys.exit('entry 0x%x is outside 0x%x-0x%x' % (entry, start, end))
    image = dump[start:end]
    open(sys.argv[5], 'wb').write(elf(entry, [(start, image)]))
    print('0x%08x-0x%08x entry 0x%08x -> %s' % (start, end, entry, sys.argv[5]))


if __name__ == '__main__':
    main()
