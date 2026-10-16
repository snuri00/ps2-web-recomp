#include "hw.hpp"

#include "iop.hpp"
#include "kernel.hpp"
#include "sio2.hpp"
#include "spu2.hpp"

#include "ps2/state.hpp"

#include <cstdio>
#include <map>
#include <set>

namespace ps2::iop {

namespace {

alignas(16) std::uint8_t g_scratchpad[1024];

bool is_ram(std::uint32_t p) { return p < 0x00800000u; }
bool is_scratchpad(std::uint32_t p) { return (p & ~0x3FFu) == 0x1F800000u; }

}

std::uint8_t read8(std::uint32_t a) {
    const std::uint32_t p = phys(a);
    if (is_ram(p)) return g_ram[p & (kRamSize - 1)];
    if (is_scratchpad(p)) return g_scratchpad[p & 0x3FFu];
    return static_cast<std::uint8_t>(hw::read(p, 1));
}

std::uint16_t read16(std::uint32_t a) {
    const std::uint32_t p = phys(a);
    std::uint16_t v;
    if (is_ram(p)) { std::memcpy(&v, g_ram + (p & (kRamSize - 1)), 2); return v; }
    if (is_scratchpad(p)) { std::memcpy(&v, g_scratchpad + (p & 0x3FEu), 2); return v; }
    return static_cast<std::uint16_t>(hw::read(p, 2));
}

std::uint32_t read32(std::uint32_t a) {
    const std::uint32_t p = phys(a);
    std::uint32_t v;
    if (is_ram(p)) { std::memcpy(&v, g_ram + (p & (kRamSize - 1)), 4); return v; }
    if (is_scratchpad(p)) { std::memcpy(&v, g_scratchpad + (p & 0x3FCu), 4); return v; }
    return hw::read(p, 4);
}

void write8(std::uint32_t a, std::uint8_t v) {
    const std::uint32_t p = phys(a);
    if (is_ram(p)) { g_ram[p & (kRamSize - 1)] = v; return; }
    if (is_scratchpad(p)) { g_scratchpad[p & 0x3FFu] = v; return; }
    hw::write(p, v, 1);
}

void write16(std::uint32_t a, std::uint16_t v) {
    const std::uint32_t p = phys(a);
    if (is_ram(p)) { std::memcpy(g_ram + (p & (kRamSize - 1)), &v, 2); return; }
    if (is_scratchpad(p)) { std::memcpy(g_scratchpad + (p & 0x3FEu), &v, 2); return; }
    hw::write(p, v, 2);
}

void write32(std::uint32_t a, std::uint32_t v) {
    const std::uint32_t p = phys(a);
    if (is_ram(p)) { std::memcpy(g_ram + (p & (kRamSize - 1)), &v, 4); return; }
    if (is_scratchpad(p)) { std::memcpy(g_scratchpad + (p & 0x3FCu), &v, 4); return; }
    hw::write(p, v, 4);
}

namespace hw {

namespace {

constexpr std::uint32_t kIStat = 0x1F801070u, kIMask = 0x1F801074u, kICtrl = 0x1F801078u;
constexpr std::uint32_t kDicr = 0x1F8010F4u, kDicr2 = 0x1F801574u;

std::uint32_t g_istat = 0, g_imask = 0, g_ictrl = 1;
std::uint32_t g_dicr = 0, g_dicr2 = 0;
std::map<std::uint32_t, std::uint32_t> g_regs;
std::set<std::uint32_t> g_reported;

struct Dma {
    std::uint32_t madr, bcr, chcr, tadr;
};
Dma g_dma[14];
std::uint32_t g_ohci[0x18];
std::uint8_t g_ilink_phy[16];

int dma_channel(std::uint32_t a, std::uint32_t &reg) {
    if (a >= 0x1F801080u && a < 0x1F8010F0u) {
        reg = (a & 0xFu) >> 2;
        return static_cast<int>((a - 0x1F801080u) >> 4);
    }
    if (a >= 0x1F801500u && a < 0x1F801570u) {
        reg = (a & 0xFu) >> 2;
        return 7 + static_cast<int>((a - 0x1F801500u) >> 4);
    }
    return -1;
}

void report(const char *what, std::uint32_t a, std::uint32_t v) {
    if (g_reported.insert(a).second)
        std::fprintf(stderr, "[iop-io] %s 0x%08X = 0x%X (pc 0x%08X)\n", what, a, v, g_cpu.pc);
}

void start_dma(unsigned ch) {
    Dma &d = g_dma[ch];
    if (kernel::g_trace_modules) std::fprintf(stderr, "[iop-dma] start ch %u madr 0x%08X bcr 0x%08X chcr 0x%08X\n", ch, d.madr, d.bcr, d.chcr);
    const std::uint32_t words = (d.bcr & 0xFFFFu) * ((d.bcr >> 16) ? (d.bcr >> 16) : 1u);
    if (ch == 4 || ch == 7) {
        spu2::dma(ch == 4 ? 0 : 1, d.madr, words, (d.chcr & 1u) != 0, static_cast<int>(ch));
        return;
    }
    if (ch == 11 || ch == 12) {
        sio2::arm_dma(static_cast<int>(ch), d.madr, d.bcr & 0xFFFFu, d.bcr >> 16);
        return;
    }
    std::fprintf(stderr, "[iop-dma] channel %u start madr 0x%08X bcr 0x%08X chcr 0x%08X (not emulated)\n", ch, d.madr, d.bcr, d.chcr);
    dma_done(ch);
}

}

void reset() {
    kernel::register_job(kernel::kJobDmaDone, [](const kernel::Job &j) { dma_done(j.a); });
    g_istat = g_imask = g_dicr = g_dicr2 = 0;
    g_ictrl = 1;
    g_regs.clear();
    std::memset(g_dma, 0, sizeof g_dma);
    std::memset(g_scratchpad, 0, sizeof g_scratchpad);
    spu2::reset();
    sio2::reset();
    std::memset(g_ohci, 0, sizeof g_ohci);
    std::memset(g_ilink_phy, 0, sizeof g_ilink_phy);
}

void raise_interrupt(unsigned line) {
    if (line < 32) g_istat |= 1u << line;
}

void set_interrupt_mask(unsigned line, bool enabled) {
    if (line < 32) {
        if (enabled) g_imask |= 1u << line;
        else g_imask &= ~(1u << line);
        return;
    }
    if (enabled) {
        g_imask |= 8u;
        g_dicr |= 0x800000u;
    }
    if (line < 39) {
        const std::uint32_t bit = 1u << (16 + line - 32);
        if (enabled) g_dicr |= bit;
        else g_dicr &= ~bit;
    } else if (line >= 40 && line < 46) {
        const std::uint32_t bit = 1u << (16 + line - 40);
        if (enabled) g_dicr2 |= bit;
        else g_dicr2 &= ~bit;
    }
}

int next_pending_interrupt() {
    if (!g_ictrl) return -1;
    const std::uint32_t pending = g_istat & g_imask;
    if (pending & 8u) {
        const std::uint32_t flags = (g_dicr >> 24) & (g_dicr >> 16) & 0x7Fu;
        if (flags) return 32 + __builtin_ctz(flags);
        const std::uint32_t flags2 = (g_dicr2 >> 24) & (g_dicr2 >> 16) & 0x3Fu;
        if (flags2) return 40 + __builtin_ctz(flags2);
        g_istat &= ~8u;
    }
    const std::uint32_t rest = g_istat & g_imask & ~8u;
    return rest ? __builtin_ctz(rest) : -1;
}

void acknowledge_interrupt(unsigned line) {
    if (line < 32) {
        g_istat &= ~(1u << line);
        return;
    }
    if (line < 39) g_dicr &= ~(1u << (24 + line - 32));
    else if (line >= 40) g_dicr2 &= ~(1u << (24 + line - 40));
    if (!(((g_dicr >> 24) & 0x7Fu) | ((g_dicr2 >> 24) & 0x3Fu))) g_istat &= ~8u;
}

void dma_done(unsigned ch) {
    g_dma[ch].chcr &= ~0x01000000u;
    if (ch < 7) g_dicr |= 1u << (24 + ch);
    else g_dicr2 |= 1u << (24 + ch - 7);
    g_istat |= 8u;
}

std::uint32_t ohci_read(std::uint32_t a) {
    const std::uint32_t reg = (a - 0x1F801600u) >> 2;
    switch (reg) {
    case 0x00: return 0x110u;
    case 0x02: return g_ohci[reg] & ~1u;
    case 0x0E: return static_cast<std::uint32_t>(kernel::now() / 36864u) & 0xFFFFu;
    case 0x12: return 0x00000202u;
    default: return reg < 0x18 ? g_ohci[reg] : 0u;
    }
}

void ohci_write(std::uint32_t a, std::uint32_t v) {
    const std::uint32_t reg = (a - 0x1F801600u) >> 2;
    if (reg >= 0x18) return;
    if (reg == 0x03) g_ohci[reg] &= ~v;
    else if (reg == 0x05) g_ohci[0x04] &= ~v;
    else if (reg == 0x04) g_ohci[reg] |= v;
    else g_ohci[reg] = v;
}

std::uint32_t read(std::uint32_t a, unsigned size) {
    if (a == 0x1F808400u) return 0xFFC00001u;
    if (a == 0x1F80847Cu) return 0x10000001u;
    if (a >= 0x1F900000u && a < 0x1F900800u) return spu2::read(a, size);
    if (a >= 0x1F801600u && a < 0x1F801660u) return ohci_read(a);
    if (a >= 0x1F808200u && a < 0x1F808280u) return sio2::read(a);
    if (a == kIStat) return g_istat;
    if (a == kIMask) return g_imask;
    if (a == kICtrl) {
        const std::uint32_t v = g_ictrl;
        g_ictrl = 0;
        return v;
    }
    if (a == kDicr) return g_dicr;
    if (a == kDicr2) return g_dicr2;
    std::uint32_t reg = 0;
    const int ch = dma_channel(a, reg);
    if (ch >= 0) {
        const Dma &d = g_dma[ch];
        return (reg == 0 ? d.madr : reg == 1 ? d.bcr : reg == 2 ? d.chcr : d.tadr) >> ((a & 3u) * 8);
    }
    if ((a >= 0x1F801100u && a < 0x1F801130u) || (a >= 0x1F801480u && a < 0x1F8014B0u)) {
        if ((a & 0xFu) == 0) return static_cast<std::uint32_t>(kernel::now());
    }
    const auto it = g_regs.find(a);
    const std::uint32_t v = it == g_regs.end() ? 0 : it->second;
    report("read", a, v);
    return v;
}

void write(std::uint32_t a, std::uint32_t v, unsigned size) {
    // Minimal local i.Link initialization with no attached peers. Complete
    // PHY accesses and report a ready serial clock; no link traffic is modeled.
    if (a == 0x1F808410u) { g_regs[a] = (v & 2u) | 8u; return; }
    if (a == 0x1F808408u) { g_regs[a] = v & ~0x00800000u; return; }
    if (a == 0x1F808420u || a == 0x1F808428u || a == 0x1F808430u) {
        g_regs[a] &= ~v;
        return;
    }
    if (a == 0x1F808414u) {
        if (v & 0x40000000u) {
            g_ilink_phy[(v >> 8) & 15u] = static_cast<std::uint8_t>(v);
            g_regs[a] = v & ~0x4000FFFFu;
        } else if (v & 0x80000000u) {
            const std::uint32_t reg = (v >> 24) & 15u;
            g_regs[a] = (v & ~0x8000FFFFu) | (reg << 8) | g_ilink_phy[reg];
            g_regs[0x1F808420u] |= 0x40000000u;
            if (g_regs[0x1F808424u] & 0x40000000u) raise_interrupt(24);
        } else g_regs[a] = v;
        return;
    }
    if (a >= 0x1F900000u && a < 0x1F900800u) { spu2::write(a, v, size); return; }
    if (a >= 0x1F801600u && a < 0x1F801660u) { ohci_write(a, v); return; }
    if (a >= 0x1F808200u && a < 0x1F808280u) { sio2::write(a, v); return; }
    if (a == kIStat) { g_istat &= v; return; }
    if (a == kIMask) { g_imask = v; return; }
    if (a == kICtrl) { g_ictrl = v & 1u; return; }
    if (a == kDicr) {
        g_dicr = (g_dicr & 0x7F000000u & ~(v & 0x7F000000u)) | (v & 0x00FFFFFFu);
        return;
    }
    if (a == kDicr2) {
        g_dicr2 = (g_dicr2 & 0x7F000000u & ~(v & 0x7F000000u)) | (v & 0x00FFFFFFu);
        return;
    }
    std::uint32_t reg = 0;
    const int ch = dma_channel(a, reg);
    if (ch >= 0) {
        Dma &d = g_dma[ch];
        std::uint32_t &target = reg == 0 ? d.madr : reg == 1 ? d.bcr : reg == 2 ? d.chcr : d.tadr;
        const unsigned shift = (a & 3u) * 8;
        const std::uint32_t mask = size >= 4 ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1u) << shift;
        target = (target & ~mask) | ((v << shift) & mask);
        if (reg == 2 && (d.chcr & 0x01000000u) && (mask & 0x01000000u)) start_dma(static_cast<unsigned>(ch));
        return;
    }
    report("write", a, v);
    g_regs[a] = v;
}

std::uint16_t *spu2_ram() { return spu2::ram(); }

}

}

namespace ps2::iop::hw {

void state(ps2::state::Stream &s) {
    s.tag("iophw");
    s.pod(g_istat);
    s.pod(g_imask);
    s.pod(g_ictrl);
    s.pod(g_dicr);
    s.pod(g_dicr2);
    s.map(g_regs);
    s.pod(g_dma);
    s.pod(g_ohci);
    s.pod(g_ilink_phy);
    s.pod(g_scratchpad);
}

}
