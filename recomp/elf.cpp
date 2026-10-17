#include "elf.hpp"

#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace recomp {

namespace {

template <typename T>
T read(const std::vector<std::uint8_t> &data, std::size_t offset) {
    if (offset + sizeof(T) > data.size()) throw std::runtime_error("truncated ELF file");
    T value;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

}

Image Image::load(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open " + path);
    const std::vector<std::uint8_t> data{std::istreambuf_iterator<char>(file), {}};
    if (data.size() < 52 || std::memcmp(data.data(), "\x7f" "ELF\x01\x01", 6) != 0)
        throw std::runtime_error(path + " is not a little endian ELF32 file");

    Image image;
    image.entry_ = read<std::uint32_t>(data, 24);
    const auto phoff = read<std::uint32_t>(data, 28);
    const auto shoff = read<std::uint32_t>(data, 32);
    const auto phentsize = read<std::uint16_t>(data, 42), phnum = read<std::uint16_t>(data, 44);
    const auto shentsize = read<std::uint16_t>(data, 46), shnum = read<std::uint16_t>(data, 48);

    for (std::uint32_t i = 0; i < phnum; ++i) {
        const std::size_t ph = phoff + i * phentsize;
        if (read<std::uint32_t>(data, ph) != 1) continue;
        const auto offset = read<std::uint32_t>(data, ph + 4), vaddr = read<std::uint32_t>(data, ph + 8);
        const auto filesz = read<std::uint32_t>(data, ph + 16), memsz = read<std::uint32_t>(data, ph + 20);
        const auto flags = read<std::uint32_t>(data, ph + 24);
        if (offset + filesz > data.size()) throw std::runtime_error("segment outside the file");
        // An executable segment with no file contents is a code window: the
        // game rewrites it at runtime, so it holds no code to translate here
        // and any call into it walks to the images the game puts there.
        if (!filesz && (flags & 1u)) continue;
        Segment segment{vaddr, std::vector<std::uint8_t>(memsz), (flags & 1u) != 0};
        std::memcpy(segment.bytes.data(), data.data() + offset, filesz);
        image.segments_.push_back(std::move(segment));
    }

    const auto shstrndx = read<std::uint16_t>(data, 50);
    for (std::uint32_t i = 0; shoff && i < shnum; ++i) {
        const std::size_t sh = shoff + i * shentsize;
        if (read<std::uint32_t>(data, sh + 4) == 1 && shstrndx < shnum) {
            const std::size_t name = read<std::uint32_t>(data, shoff + shstrndx * shentsize + 16) + read<std::uint32_t>(data, sh);
            if (name + 6 <= data.size() && std::memcmp(data.data() + name, ".text", 6) == 0) {
                const auto address = read<std::uint32_t>(data, sh + 12), size = read<std::uint32_t>(data, sh + 20);
                image.text_ = {address, address + size};
            }
        }
        if (read<std::uint32_t>(data, sh + 4) != 2) continue;
        const auto symoff = read<std::uint32_t>(data, sh + 16), symsize = read<std::uint32_t>(data, sh + 20);
        const auto link = read<std::uint32_t>(data, sh + 24);
        const auto stroff = read<std::uint32_t>(data, shoff + link * shentsize + 16);
        for (std::size_t s = symoff; s + 16 <= symoff + symsize; s += 16) {
            const auto name = read<std::uint32_t>(data, s), value = read<std::uint32_t>(data, s + 4);
            if ((data[s + 12] & 15u) != 2u || !value) continue;
            image.symbols_[value] = reinterpret_cast<const char *>(data.data() + stroff + name);
        }
    }
    return image;
}

Image Image::raw(std::uint32_t address, std::vector<std::uint8_t> bytes, std::pair<std::uint32_t, std::uint32_t> text) {
    Image image;
    image.segments_.push_back(Segment{address, std::move(bytes), true});
    image.text_ = text;
    return image;
}

std::optional<std::uint32_t> Image::word(std::uint32_t address) const {
    for (const auto &segment : segments_) {
        if (address >= segment.address && address - segment.address + 4 <= segment.bytes.size()) {
            std::uint32_t value;
            std::memcpy(&value, segment.bytes.data() + (address - segment.address), 4);
            return value;
        }
    }
    return std::nullopt;
}

bool Image::executable(std::uint32_t address) const {
    for (const auto &segment : segments_)
        if (segment.executable && address >= segment.address && address - segment.address < segment.bytes.size())
            return true;
    return false;
}

}
