#include "hw.hpp"
#include "vu_micro.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <cstdio>
#include <cstring>

namespace ps2 {

alignas(64) std::uint8_t g_ram[kRamSize + 64];
alignas(64) std::uint8_t g_spr[kScratchpadSize];
alignas(64) std::uint8_t g_vu0_code[4096];
alignas(64) std::uint8_t g_vu0_mem[4096];
alignas(64) std::uint8_t g_vu1_code[16384];
alignas(64) std::uint8_t g_vu1_mem[16384];

namespace {

enum class Region { Ram, Scratchpad, Io, Vu, Rom, None };

struct Target {
    Region region;
    std::uint8_t *host;
    std::uint32_t address;
};

std::uint8_t *vu_memory(std::uint32_t p) {
    switch ((p >> 12) & 0xFu) {
    case 0x0: return g_vu0_code + (p & 0xFFFu);
    case 0x4: return g_vu0_mem + (p & 0xFFFu);
    case 0x8: case 0x9: case 0xA: case 0xB: return g_vu1_code + (p & 0x3FFFu);
    case 0xC: case 0xD: case 0xE: case 0xF: return g_vu1_mem + (p & 0x3FFFu);
    default: return nullptr;
    }
}

Target resolve(std::uint32_t a) {
    if ((a >> 16) == (kScratchpadBase >> 16)) return {Region::Scratchpad, g_spr + (a & (kScratchpadSize - 1)), a};
    const std::uint32_t p = physical(a);
    if (p < 0x10000000u) return {Region::Ram, g_ram + (p & (kRamSize - 1)), p};
    if (p >= 0x11000000u && p < 0x11010000u) {
        if (std::uint8_t *host = vu_memory(p)) return {Region::Vu, host, p};
    }
    if (p >= 0x10000000u && p < 0x13000000u) return {Region::Io, nullptr, p};
    if (p >= 0x1FC00000u && p < 0x20000000u) return {Region::Rom, nullptr, p};
    return {Region::None, nullptr, p};
}

void unmapped(const char *what, std::uint32_t a) {
    std::fprintf(stderr, "[mem] %s at unmapped 0x%08X (pc 0x%08X)\n", what, a, g_cpu.pc);
}

template <typename T>
T read_slow(std::uint32_t a) {
    const Target t = resolve(a);
    if (t.host) {
        T v;
        std::memcpy(&v, t.host, sizeof v);
        return v;
    }
    if (t.region == Region::Io) {
        if constexpr (sizeof(T) == 8) return hw::read64(t.address);
        else {
            const std::uint32_t word = hw::read32(t.address & ~3u);
            return static_cast<T>(word >> ((t.address & 3u) * 8u));
        }
    }
    if (t.region != Region::Rom) unmapped("read", a);
    return 0;
}

template <typename T>
void write_slow(std::uint32_t a, T v) {
    const Target t = resolve(a);
    if (t.host) {
        std::memcpy(t.host, &v, sizeof v);
        if (t.region == Region::Vu && (t.address & 0x4000u) == 0) vu::code_changed((t.address >> 15) & 1u);
        return;
    }
    if (t.region == Region::Io) {
        if constexpr (sizeof(T) == 8) hw::write64(t.address, v);
        else hw::write32(t.address, static_cast<std::uint32_t>(v));
        return;
    }
    unmapped("write", a);
}

}

std::uint32_t physical(std::uint32_t a) {
    if (a >= 0x80000000u && a < 0xC0000000u) return a & 0x1FFFFFFFu;
    const std::uint32_t top = a >> 28;
    if (top == 2u || top == 3u) return a & 0x0FFFFFFFu;
    return a;
}

std::uint8_t slow_ld8(std::uint32_t a) { return read_slow<std::uint8_t>(a); }
std::uint16_t slow_ld16(std::uint32_t a) { return read_slow<std::uint16_t>(a); }
std::uint32_t slow_ld32(std::uint32_t a) { return read_slow<std::uint32_t>(a); }
std::uint64_t slow_ld64(std::uint32_t a) { return read_slow<std::uint64_t>(a); }

void slow_ld128(std::uint32_t a, Reg128 &out) {
    const Target t = resolve(a);
    if (t.host) std::memcpy(&out, t.host, 16);
    else if (t.region == Region::Io) hw::read128(t.address, out);
    else std::memset(&out, 0, 16);
}

void slow_st8(std::uint32_t a, std::uint8_t v) { write_slow(a, v); }
void slow_st16(std::uint32_t a, std::uint16_t v) { write_slow(a, v); }
void slow_st32(std::uint32_t a, std::uint32_t v) { write_slow(a, v); }
void slow_st64(std::uint32_t a, std::uint64_t v) { write_slow(a, v); }

void slow_st128(std::uint32_t a, const Reg128 &v) {
    const Target t = resolve(a);
    if (t.host) std::memcpy(t.host, &v, 16);
    else if (t.region == Region::Io) hw::write128(t.address, v);
    else unmapped("write128", a);
}

}
