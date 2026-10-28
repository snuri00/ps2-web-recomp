#!/usr/bin/env python3
"""Re-cut captured code window images along new window bounds.

usage: split_windows.py <old overlays dir> <windows.txt> <entries file> <new overlays dir> [<ram dump>]

A window drawn too wide holds data the game rewrites all the time next to
code that stays put, so every capture is a new image and none stays in
place. Once windows.txt lists narrower windows, this slices every old capture
into the new windows (one image per slice content) and moves each entry
token <old image>:<offset> to the slice of the same capture holding it, so
nothing explored so far is lost. The entries file is rewritten in place.

A window drawn wider than the old ones (code and the jump tables it reads in
one image) has no old capture covering it. Given a RAM dump, the window is
captured from it instead, and a token of an old image moves to it only when
the code around the token reads the same in both: a token of some other
image would seed bytes that are not code here.
"""
import binascii
import os
import sys

UNIT = 0x4000


def windows(path):
    out = []
    for line in open(path):
        words = line.split('#', 1)[0].split()
        if len(words) == 2:
            out.append((int(words[0], 16) & ~(UNIT - 1), int(words[1], 16)))
    return out


AROUND = 32  # bytes from the token on that must match to carry it to a dump capture


def main():
    if len(sys.argv) not in (5, 6):
        sys.exit(__doc__)
    old_dir, windows_path, entries_path, new_dir = sys.argv[1:5]
    dump = open(sys.argv[5], 'rb').read() if len(sys.argv) == 6 else None
    wins = windows(windows_path)
    os.makedirs(new_dir, exist_ok=True)
    olds = {}  # old stem -> its bytes
    for name in os.listdir(old_dir):
        if name.endswith('.bin'):
            olds[name[:-4]] = open(os.path.join(old_dir, name), 'rb').read()
    fresh = {}  # base -> (size, stem) of windows captured from the dump
    if dump is not None:
        for base, size in wins:
            covered = any(int(s.split('_', 1)[0], 16) <= base and base + size <= int(s.split('_', 1)[0], 16) + len(b)
                          for s, b in olds.items())
            if covered or base + size > len(dump):
                continue
            piece = dump[base:base + size]
            stem = '%08X_%08X' % (base, binascii.crc32(piece) & 0xFFFFFFFF)
            open(os.path.join(new_dir, stem + '.bin'), 'wb').write(piece)
            fresh[base] = (size, stem)
    stems = {}  # old stem -> [(base, size, new stem)]
    for name in sorted(os.listdir(old_dir)):
        if not name.endswith('.bin'):
            continue
        old = name[:-4]
        old_base = int(old.split('_', 1)[0], 16)
        blob = open(os.path.join(old_dir, name), 'rb').read()
        for base, size in wins:
            lo = base - old_base
            if lo < 0 or lo + size > len(blob):
                continue
            piece = blob[lo:lo + size]
            stem = '%08X_%08X' % (base, binascii.crc32(piece) & 0xFFFFFFFF)
            path = os.path.join(new_dir, stem + '.bin')
            if not os.path.exists(path):
                open(path, 'wb').write(piece)
            stems.setdefault(old, []).append((base, size, stem))
    out, moved, dropped = [], 0, 0
    for token in open(entries_path).read().split():
        name, sep, offset = token.rpartition(':')
        if not sep or name not in olds:
            out.append(token)
            continue
        address = int(name.split('_', 1)[0], 16) + int(offset, 16)
        target = None
        for base, size, stem in stems.get(name, []):
            if base <= address < base + size:
                target = '%s:%x' % (stem, address - base)
                break
        if target is None:
            old_base = int(name.split('_', 1)[0], 16)
            for base, (size, stem) in fresh.items():
                if base <= address < base + size:
                    mine = olds[name][address - old_base:address - old_base + AROUND]
                    if mine == dump[address:address + len(mine)]:
                        target = '%s:%x' % (stem, address - base)
                    break
        if target:
            out.append(target)
            moved += 1
        else:
            dropped += 1
    seen = set()
    with open(entries_path, 'w') as f:
        for token in out:
            if token not in seen:
                seen.add(token)
                f.write(token + '\n')
    print('%d images in %s, %d tokens moved, %d outside every window dropped' % (len(os.listdir(new_dir)), new_dir, moved, dropped))


if __name__ == '__main__':
    main()
