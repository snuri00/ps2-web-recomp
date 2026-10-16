import argparse
import struct
import sys
import zlib

R5900_FLAGS = 0x20924001


def unpack(data):
    flags, size = struct.unpack_from('<HI', data, 0)
    try:
        image = zlib.decompress(data[6:], -15)
    except zlib.error as error:
        sys.exit('CORE is not a plain deflate stream (flags 0x%04x): %s' % (flags, error))
    if len(image) != size:
        sys.exit('CORE inflated to %d bytes, header says %d' % (len(image), size))
    return image


def sections(image):
    offset = 0
    for _ in range(2):
        offset += 2 + struct.unpack_from('<H', image, offset)[0]
    count, entry = struct.unpack_from('<iI', image, offset)
    offset += 8
    result = []
    for _ in range(count):
        address, size = struct.unpack_from('<II', image, offset)
        result.append((address, image[offset + 8:offset + 8 + size]))
        offset += 8 + size
    if offset != len(image):
        sys.exit('CORE section table ends at 0x%x of 0x%x' % (offset, len(image)))
    return entry, result


def merge(parts):
    segments = []
    for address, data in sorted(parts, key=lambda p: (p[0], -len(p[1]))):
        if segments:
            start, body = segments[-1]
            if address < start + len(body):
                overlap = body[address - start:address - start + len(data)]
                if overlap != data[:len(overlap)]:
                    sys.exit('CORE sections overlap with different bytes at 0x%08x' % address)
                if address + len(data) > start + len(body):
                    segments[-1] = (start, body + data[start + len(body) - address:])
                continue
        segments.append((address, bytes(data)))
    return segments


def elf(entry, segments):
    header_size, ph_size = 52, 32
    offset = header_size + ph_size * len(segments)
    offset = (offset + 0xFFF) & ~0xFFF
    headers, bodies = b'', b''
    for address, data in segments:
        pad = (-(offset + len(bodies))) & 0x7F
        bodies += b'\0' * pad
        headers += struct.pack('<8I', 1, offset + len(bodies), address, address, len(data), len(data), 7, 0x80)
        bodies += data
    ident = b'\x7fELF\x01\x01\x01' + b'\0' * 9
    header = ident + struct.pack('<HHIIIIIHHHHHH', 2, 8, 1, entry, header_size, 0, R5900_FLAGS,
                                 header_size, ph_size, len(segments), 40, 0, 0)
    body = header + headers
    return body + b'\0' * (offset - len(body)) + bodies


def main():
    parser = argparse.ArgumentParser(description="Turn Gran Turismo 3/4's CORE.GT3/CORE.GT4 into an ELF the recompiler can read.")
    parser.add_argument('core')
    parser.add_argument('output')
    args = parser.parse_args()
    entry, parts = sections(unpack(open(args.core, 'rb').read()))
    segments = merge(parts)
    open(args.output, 'wb').write(elf(entry, segments))
    for address, data in segments:
        print('0x%08x-0x%08x' % (address, address + len(data)))
    print('entry 0x%08x -> %s' % (entry, args.output))


if __name__ == '__main__':
    main()
