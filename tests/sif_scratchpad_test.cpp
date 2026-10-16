#include "iop/iop.hpp"
#include "iop/sif.hpp"
#include "ps2/memory.hpp"

#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace {
int failures = 0;

void check(bool ok, const char *message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void descriptor(std::uint32_t at, std::uint32_t source, std::uint32_t dest, std::uint32_t size) {
    ps2::st32(at, source);
    ps2::st32(at + 4, dest);
    ps2::st32(at + 8, size);
    ps2::st32(at + 12, 0);
}

void payload(std::uint32_t at, unsigned size, std::uint8_t first) {
    for (unsigned i = 0; i < size; ++i) ps2::st8(at + i, static_cast<std::uint8_t>(first + i));
}

bool copied(std::uint32_t at, unsigned size, std::uint8_t first) {
    for (unsigned i = 0; i < size; ++i)
        if (ps2::iop::read8(at + i) != static_cast<std::uint8_t>(first + i)) return false;
    return true;
}

void ram_descriptors() {
    // All these EE addresses resolve to the same main-RAM descriptor.
    for (const std::uint32_t address : {0x00001000u, 0x80001000u, 0xA0001000u, 0x20001000u, 0x30001000u}) {
        ps2::sif::reset();
        descriptor(0x1000, 0x2000, 0x8000, 16);
        payload(0x2000, 16, 0x40);
        for (unsigned i = 0; i < 16; ++i) ps2::iop::write8(0x8000 + i, 0);
        check(ps2::sif::set_dma(address, 1) != 0, "RAM descriptor returns a DMA ID");
        check(copied(0x8000, 16, 0x40), "main-RAM descriptor and its aliases copy the expected payload");
    }
}

void scratchpad_descriptors() {
    ps2::sif::reset();
    constexpr std::uint32_t list = ps2::kScratchpadBase + 0x1000;
    descriptor(list, 0x2000, 0x8000, 16);
    descriptor(list + 16, 0x2100, 0x8010, 12);
    payload(0x2000, 16, 0xA0);
    payload(0x2100, 12, 0xC0);
    payload(0x2200, 16, 0x10);
    // Conflicting, valid descriptors at the addresses obtained by masking the
    // scratchpad list into RAM. The old reader will copy these instead.
    descriptor(0x1000, 0x2200, 0x8000, 16);
    descriptor(0x1010, 0x2200, 0x8010, 12);
    for (unsigned i = 0; i < 32; ++i) ps2::iop::write8(0x8000 + i, 0xEE);
    check(ps2::sif::set_dma(list, 2) != 0, "scratchpad descriptor list returns a DMA ID");
    check(copied(0x8000, 16, 0xA0), "first descriptor is read from scratchpad, not its RAM alias");
    check(copied(0x8010, 12, 0xC0), "second descriptor is read from scratchpad, not its RAM alias");
    check(ps2::iop::read8(0x801C) == 0xEE, "bytes beyond the requested transfers remain untouched");
}
}

int main() {
    ram_descriptors();
    scratchpad_descriptors();
    if (failures) return 1;
    std::puts("SIF scratchpad descriptor regression PASS");
    return 0;
}
