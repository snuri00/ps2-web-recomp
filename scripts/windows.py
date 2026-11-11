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

  windows.py disc <elf> <image> <dir>
                                     save the overlays an AFS archive on the
                                     disc holds, so a mode the exploring runs
                                     never enter is translated as well

The RAM dump is the one the runner writes with PS2_DUMP, so at a halt it holds
exactly the image the game was running when it called the missing entry.

  --extra <file>                     also treat the areas listed in <file> as
                                     windows, "base size" in hex per line: code
                                     a loader copies past the executable, which
                                     no segment announces; the image is always
                                     the whole listed size
"""
import binascii
import os
import struct
import sys

UNIT = 0x4000
CAP = 0x400000
JR_RA = 0x03E00008
OVERLAY_MAGIC = b'MWo3'
OVERLAY_HEADER = 0x40
MIN_IMAGE = 0x400
MAX_ARCHIVE = 64 << 20


def load_phdrs(elf):
    phoff, = struct.unpack_from('<I', elf, 28)
    phentsize, phnum = struct.unpack_from('<HH', elf, 42)
    for i in range(phnum):
        yield struct.unpack_from('<8I', elf, phoff + i * phentsize)


def windows(elf):
    sizes = {}
    for v in load_phdrs(elf):
        if v[0] != 1 or v[4] or not v[5] or not (v[6] & 1):  # no PT_LOAD, has contents, empty, not executable
            continue
        sizes[v[2]] = max(sizes.get(v[2], 0), v[5])
    bases = sorted(sizes)
    out = []
    for k, base in enumerate(bases):
        size = sizes[base]
        if k + 1 < len(bases):
            size = min(size, bases[k + 1] - base)
        out.append((base, min(size, CAP)))
    return out


def extent(dump, base, cap):
    # The ELF only says how far the largest overlay can reach; what the game
    # unpacks decides how far this one does, so the image covers the units it
    # wrote, stopping at the first entirely empty one.
    size = 0
    while size < cap and base + size < len(dump):
        unit = dump[base + size:base + min(size + UNIT, cap)]
        if unit == b'\0' * len(unit):
            break
        size += len(unit)
    return size & ~3


def save(outdir, base, blob):
    stem = '%08X_%08X' % (base, binascii.crc32(blob) & 0xFFFFFFFF)
    path = os.path.join(outdir, stem + '.bin')
    if os.path.exists(path):
        return False
    os.makedirs(outdir, exist_ok=True)
    open(path, 'wb').write(blob)
    return True


def starts_function(words, at):
    word = words[at]
    if word >> 16 in (0x27BD, 0x67BD) and word & 0x8000:
        return True
    return at >= 2 and (words[at - 1] == JR_RA or words[at - 2] == JR_RA) and not at & 3 and word != 0


def load_address(blob, bases):
    if blob[:4] == OVERLAY_MAGIC:
        base, text = struct.unpack_from('<II', blob, 8)
        return base if JR_RA in struct.unpack('<%dI' % (text // 4), blob[OVERLAY_HEADER:OVERLAY_HEADER + (text & ~3)]) else None
    words = struct.unpack('<%dI' % (len(blob) // 4), blob[:len(blob) & ~3])
    if not words.count(JR_RA):
        return None
    targets = [(w & 0x03FFFFFF) << 2 for w in words if w >> 26 == 3]
    best, best_hits = None, 3
    for base in bases:
        hits = sum(1 for t in targets if base <= t < base + len(words) * 4 and starts_function(words, (t - base) >> 2))
        if hits > best_hits:
            best, best_hits = base, hits
    return best


def disc_images(wins, path, outdir):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import disc
    image = disc.Image(path)
    bases = [base for base, _ in wins]
    saved = found = 0
    for _, _, sector, size in image.files():
        head = disc.read_at(image, sector, 0, 8)
        if head[:4] != b'AFS\0' or size > MAX_ARCHIVE:
            continue
        count = struct.unpack_from('<I', head, 4)[0]
        table = disc.read_at(image, sector, 8, count * 8)
        for index in range(count):
            offset, length = struct.unpack_from('<II', table, index * 8)
            if length < MIN_IMAGE or offset + length > size:
                continue
            blob = disc.read_at(image, sector, offset, length)
            base = load_address(blob, bases)
            if base is None:
                continue
            found += 1
            if save(outdir, base, blob[:len(blob) & ~3]):
                saved += 1
    print('%d overlays in the disc archives, %d new window images' % (found, saved))


def extra_windows(path):
    out = []
    for line in open(path):
        words = line.split('#', 1)[0].split()
        if len(words) == 2:
            out.append((int(words[0], 16) & ~(UNIT - 1), int(words[1], 16)))
    return out


def main() -> None:
    extra = []
    if '--extra' in sys.argv:
        at = sys.argv.index('--extra')
        extra = extra_windows(sys.argv[at + 1])
        del sys.argv[at:at + 2]
    if len(sys.argv) not in (3, 5, 6) or sys.argv[1] not in ('list', 'capture', 'disc'):
        sys.exit(__doc__)
    mode = sys.argv[1]
    elf = open(sys.argv[2], 'rb').read()
    wins = windows(elf)
    if mode == 'disc':
        disc_images(wins, sys.argv[3], sys.argv[4])
        return
    if mode == 'list':
        for base, size in wins + extra:
            print('%08x %x' % (base, size))
        return

    dump = open(sys.argv[3], 'rb').read()
    outdir = sys.argv[4]
    misses = sys.argv[5] if len(sys.argv) > 5 else None
    fixed = dict(extra)
    for base, cap in wins + extra:
        size = fixed.get(base) or extent(dump, base, cap)
        blob = dump[base:base + size]
        if not blob:
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