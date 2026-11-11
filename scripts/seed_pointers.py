#!/usr/bin/env python3
"""Add the code pointers a RAM dump holds into fixed windows as misses.

usage: seed_pointers.py <dump> <windows.txt> <misses file> [--dry] [--extra ranges.txt]

ranges.txt (games/<name>/seed_pointers) may list more "<base> <size>" ranges,
such as the executable's own code, whose handlers are called the same way.

Some games call window code through pointers to places that are no function
start at all (MGS3 resumes script handlers mid-function through jalr), so
neither the static pass nor seed_windows.py finds them and exploring pays a
run for each. Every aligned word of the dump that points into a window, at a
word that is not zero, is appended to the misses as a bare pc; windows.py
capture then names it against the dump's image like a real miss. Data that
merely looks like such a pointer costs translated code, not correctness: the
recompiler rejects what does not decode.
"""
import struct
import sys


def main():
    args = [a for a in sys.argv[1:] if a != '--dry']
    paths = []
    if '--extra' in args:
        at = args.index('--extra')
        paths.append(args[at + 1])
        del args[at:at + 2]
    if len(args) != 3:
        sys.exit(__doc__)
    dump_path, windows_path, misses_path = args
    dump = open(dump_path, 'rb').read()
    wins = []
    for path in [windows_path] + paths:
        for line in open(path):
            line = line.split('#')[0].split()
            if len(line) == 2:
                wins.append((int(line[0], 16), int(line[1], 16)))
    n = len(dump) // 4
    ws = struct.unpack_from('<%dI' % n, dump)
    found = set()
    for w in set(ws):
        if w & 3:
            continue
        for base, size in wins:
            if base <= w < base + size and w + 4 <= len(dump) and ws[w // 4]:
                found.add(w)
    print(len(found))
    if '--dry' in sys.argv:
        return
    with open(misses_path, 'a') as out:
        for pc in sorted(found):
            out.write('%x\n' % pc)


if __name__ == '__main__':
    main()
