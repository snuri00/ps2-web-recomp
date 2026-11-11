// mgs_stages: write out the stage code images of Metal Gear Solid 2 or 3 (PS2), for the template builder
//
// usage: mgs_stages <disc image> <out dir>
//
// Both keep each stage's code in STAGE.DAT (/MGS2/ and /MGS/) as an encrypted zlib stream,
// a memory image prelinked for the address it loads at, which is its first word. The games
// decrypt with a stream keyed by the stream's first halfword (MGS3's loader is at
// 0x100950/0x100980 in the unpacked core):
//   h = first halfword ^ 0x9385, key = h | (h ^ 0x6576) << 16, step = h * 0x116
//   every word ^= key, key = key * 0x02E90EDD + step; then halfword 0 += 0x8F3 (zlib's 78 9C)
// Code images are the streams that inflate to one starting with the game's load address.
// The load address goes to <out dir>/load.txt for the template builder.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <zlib.h>

namespace {

struct Game {
    const char *dir;
    std::uint32_t load;
};
constexpr Game kGames[] = {{"MGS2", 0x001EF600u}, {"MGS", 0x00252180u}};

void decrypt(std::uint8_t *bytes, std::size_t n) {
    std::uint32_t *w = reinterpret_cast<std::uint32_t *>(bytes);
    const std::uint32_t h = (w[0] & 0xFFFFu) ^ 0x9385u;
    std::uint32_t key = h | ((h ^ 0x6576u) << 16);
    const std::uint32_t step = h * 0x116u;
    for (std::size_t i = 0; i < n / 4; ++i) {
        w[i] ^= key;
        key = key * 0x02E90EDDu + step;
    }
    w[0] = (w[0] & 0xFFFF0000u) | ((w[0] + 0x8F3u) & 0xFFFFu);
}

// inflates the stream at bytes; returns the plain bytes and how many were read, or nothing
bool inflate_at(const std::uint8_t *bytes, std::size_t n, std::vector<std::uint8_t> &plain, std::size_t &used, std::size_t limit) {
    z_stream z{};
    if (inflateInit(&z) != Z_OK) return false;
    plain.resize(limit);
    z.next_in = const_cast<std::uint8_t *>(bytes);
    z.avail_in = static_cast<uInt>(n);
    z.next_out = plain.data();
    z.avail_out = static_cast<uInt>(plain.size());
    const int r = inflate(&z, Z_FINISH);
    plain.resize(z.total_out);
    used = z.total_in;
    inflateEnd(&z);
    return r == Z_STREAM_END || (r == Z_BUF_ERROR && z.total_out >= 1024);
}

bool find_file(std::ifstream &iso, const char *dir, const char *name, std::uint32_t &lba, std::uint32_t &size) {
    std::vector<std::uint8_t> sec(2048);
    iso.seekg(16 * 2048);
    iso.read(reinterpret_cast<char *>(sec.data()), 2048);
    std::uint32_t at, len;
    std::memcpy(&at, &sec[156 + 2], 4);
    std::memcpy(&len, &sec[156 + 10], 4);
    for (const char *want : {dir, name}) {
        std::vector<std::uint8_t> d(len);
        iso.seekg(static_cast<std::streamoff>(at) * 2048);
        iso.read(reinterpret_cast<char *>(d.data()), len);
        bool found = false;
        for (std::size_t i = 0; i < len;) {
            const std::uint8_t l = d[i];
            if (!l) { i = (i / 2048 + 1) * 2048; continue; }
            const std::string id(reinterpret_cast<const char *>(&d[i + 33]), d[i + 32]);
            if (id == want || id == std::string(want) + ";1") {
                std::memcpy(&at, &d[i + 2], 4);
                std::memcpy(&len, &d[i + 10], 4);
                found = true;
                break;
            }
            i += l;
        }
        if (!found) return false;
    }
    lba = at;
    size = len;
    return true;
}

}

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: mgs_stages <disc image> <out dir>\n");
        return 2;
    }
    std::ifstream iso(argv[1], std::ios::binary);
    std::uint32_t lba = 0, size = 0, load = 0;
    for (const Game &g : kGames) {
        if (!iso) break;
        if (find_file(iso, g.dir, "STAGE.DAT", lba, size)) {
            load = g.load;
            break;
        }
        iso.clear();
    }
    if (!load) {
        std::fprintf(stderr, "no /MGS2/STAGE.DAT or /MGS/STAGE.DAT on %s\n", argv[1]);
        return 1;
    }
    std::vector<std::uint8_t> dat(size);
    iso.seekg(static_cast<std::streamoff>(lba) * 2048);
    iso.read(reinterpret_cast<char *>(dat.data()), size);
    const std::size_t sectors = size / 2048;
    std::vector<std::uint8_t> probe, plain;
    std::size_t images = 0;
    for (std::size_t s = 0; s < sectors;) {
        // a cheap look first: decrypt 8 KiB and see whether it inflates
        probe.assign(dat.begin() + s * 2048, dat.begin() + std::min(size_t{size}, s * 2048 + 8192));
        decrypt(probe.data(), probe.size());
        std::size_t used = 0;
        if (probe.size() < 8192 || !inflate_at(probe.data(), probe.size(), plain, used, 1 << 16)) {
            ++s;
            continue;
        }
        std::vector<std::uint8_t> stream(dat.begin() + s * 2048, dat.begin() + std::min(size_t{size}, s * 2048 + (16u << 20)));
        decrypt(stream.data(), stream.size());
        if (!inflate_at(stream.data(), stream.size(), plain, used, 64u << 20)) {
            ++s;
            continue;
        }
        std::uint32_t first = 0;
        if (plain.size() >= 4) std::memcpy(&first, plain.data(), 4);
        if (first == load && plain.size() > 0x10000) {
            const std::string path = std::string(argv[2]) + "/" + std::to_string(s) + ".bin";
            std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(plain.data()), static_cast<std::streamsize>(plain.size()));
            ++images;
        }
        s += (used + 2047) / 2048;
    }
    std::ofstream(std::string(argv[2]) + "/load.txt") << std::hex << load << "\n";
    std::printf("%zu stage code images (load address 0x%08X) written to %s\n", images, load, argv[2]);
    return 0;
}
