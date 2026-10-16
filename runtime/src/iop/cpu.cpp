#include "iop.hpp"
#include "kernel.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ps2::iop {

alignas(64) std::uint8_t g_ram[kRamSize + 16];
Cpu g_cpu;

namespace {

inline std::int32_t simm(std::uint32_t w) { return static_cast<std::int16_t>(w & 0xFFFFu); }

std::vector<std::uint8_t> g_probes;

bool load_probes() {
    const char *spec = std::getenv("PS2_IOP_PROBE");
    if (!spec) return false;
    g_probes.assign(kRamSize / 32, 0);
    for (const char *p = spec; *p;) {
        char *end = nullptr;
        const std::uint32_t a = static_cast<std::uint32_t>(std::strtoul(p, &end, 16)) & (kRamSize - 1);
        g_probes[a >> 5] |= static_cast<std::uint8_t>(1u << ((a >> 2) & 7u));
        p = *end ? end + 1 : end;
    }
    return true;
}

void probe(const Cpu &c, std::uint32_t pc) {
    const kernel::Thread *t = kernel::current();
    std::fprintf(stderr, "[iop-probe] 0x%08X a0 %08X a1 %08X a2 %08X a3 %08X v0 %08X v1 %08X s0 %08X s1 %08X ra %08X thread %d time %llu\n", pc, c.r[4], c.r[5], c.r[6],
                 c.r[7], c.r[2], c.r[3], c.r[16], c.r[17], c.r[31], t ? t->id : 0, static_cast<unsigned long long>(kernel::now()));
    static const char *reg = std::getenv("PS2_IOP_PROBE_MEM");
    if (reg) {
        const std::uint32_t base = phys(c.r[std::atoi(reg) & 31]);
        if (base + 64 <= kRamSize) {
            std::fprintf(stderr, "[iop-probe]   r%s:", reg);
            for (std::uint32_t i = 0; i < 16; ++i) std::fprintf(stderr, " %08X", ram32(base + i * 4));
            std::fprintf(stderr, "\n");
        }
    }
}

void bad(Cpu &c, std::uint32_t pc, std::uint32_t w) {
    std::fprintf(stderr, "[iop] unknown instruction %08X at 0x%08X (ra 0x%08X)\n", w, pc, c.r[31]);
    kernel::fault("unknown instruction");
}

}

inline std::uint32_t fetch(std::uint32_t pc) {
    const std::uint32_t p = pc & 0x1FFFFFFFu;
    if (p < 0x00800000u) {
        std::uint32_t val;
        std::memcpy(&val, g_ram + (p & (kRamSize - 1)), 4);
        return val;
    }
    return kernel::fetch_special(pc);
}

void execute(Cpu &c, std::int64_t &budget) {
    static const bool probing = load_probes();
    while (budget-- > 0) {
        const std::uint32_t pc = c.pc;
        if (__builtin_expect(probing, 0)) {
            const std::uint32_t a = pc & (kRamSize - 1);
            if (g_probes[a >> 5] & (1u << ((a >> 2) & 7u))) probe(c, pc);
        }
        const std::uint32_t w = fetch(pc);
        c.pc = c.npc;
        c.npc += 4;
        const std::uint32_t rs = (w >> 21) & 31u, rt = (w >> 16) & 31u, rd = (w >> 11) & 31u, sa = (w >> 6) & 31u;
        std::uint32_t *r = c.r;
        switch (w >> 26) {
        case 0x00:
            switch (w & 63u) {
            case 0x00: r[rd] = r[rt] << sa; break;
            case 0x02: r[rd] = r[rt] >> sa; break;
            case 0x03: r[rd] = static_cast<std::uint32_t>(static_cast<std::int32_t>(r[rt]) >> sa); break;
            case 0x04: r[rd] = r[rt] << (r[rs] & 31u); break;
            case 0x06: r[rd] = r[rt] >> (r[rs] & 31u); break;
            case 0x07: r[rd] = static_cast<std::uint32_t>(static_cast<std::int32_t>(r[rt]) >> (r[rs] & 31u)); break;
            case 0x08: c.npc = r[rs]; break;
            case 0x09: { const std::uint32_t t = r[rs]; r[rd] = pc + 8; c.npc = t; break; }
            case 0x0C:
                kernel::syscall(c, pc);
                break;
            case 0x0D:
                std::fprintf(stderr, "[iop] break at 0x%08X\n", pc);
                break;
            case 0x0F: break;
            case 0x10: r[rd] = c.hi; break;
            case 0x11: c.hi = r[rs]; break;
            case 0x12: r[rd] = c.lo; break;
            case 0x13: c.lo = r[rs]; break;
            case 0x18: {
                const std::int64_t p = static_cast<std::int64_t>(static_cast<std::int32_t>(r[rs])) * static_cast<std::int32_t>(r[rt]);
                c.lo = static_cast<std::uint32_t>(p);
                c.hi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(p) >> 32);
                break;
            }
            case 0x19: {
                const std::uint64_t p = static_cast<std::uint64_t>(r[rs]) * r[rt];
                c.lo = static_cast<std::uint32_t>(p);
                c.hi = static_cast<std::uint32_t>(p >> 32);
                break;
            }
            case 0x1A: {
                const auto n = static_cast<std::int32_t>(r[rs]), d = static_cast<std::int32_t>(r[rt]);
                if (d == 0) { c.lo = n < 0 ? 1u : 0xFFFFFFFFu; c.hi = static_cast<std::uint32_t>(n); }
                else if (n == INT32_MIN && d == -1) { c.lo = static_cast<std::uint32_t>(INT32_MIN); c.hi = 0; }
                else { c.lo = static_cast<std::uint32_t>(n / d); c.hi = static_cast<std::uint32_t>(n % d); }
                break;
            }
            case 0x1B:
                if (r[rt] == 0) { c.lo = 0xFFFFFFFFu; c.hi = r[rs]; }
                else { c.lo = r[rs] / r[rt]; c.hi = r[rs] % r[rt]; }
                break;
            case 0x20: case 0x21: r[rd] = r[rs] + r[rt]; break;
            case 0x22: case 0x23: r[rd] = r[rs] - r[rt]; break;
            case 0x24: r[rd] = r[rs] & r[rt]; break;
            case 0x25: r[rd] = r[rs] | r[rt]; break;
            case 0x26: r[rd] = r[rs] ^ r[rt]; break;
            case 0x27: r[rd] = ~(r[rs] | r[rt]); break;
            case 0x2A: r[rd] = static_cast<std::int32_t>(r[rs]) < static_cast<std::int32_t>(r[rt]); break;
            case 0x2B: r[rd] = r[rs] < r[rt]; break;
            default: bad(c, pc, w); return;
            }
            break;
        case 0x01: {
            const bool link = (rt & 0x1Eu) == 0x10u;
            const bool taken = (rt & 1u) ? static_cast<std::int32_t>(r[rs]) >= 0 : static_cast<std::int32_t>(r[rs]) < 0;
            if (link) r[31] = pc + 8;
            if (taken) c.npc = pc + 4 + (static_cast<std::uint32_t>(simm(w)) << 2);
            break;
        }
        case 0x02: c.npc = (pc & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2); break;
        case 0x03: r[31] = pc + 8; c.npc = (pc & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2); break;
        case 0x04: if (r[rs] == r[rt]) c.npc = pc + 4 + (static_cast<std::uint32_t>(simm(w)) << 2); break;
        case 0x05: if (r[rs] != r[rt]) c.npc = pc + 4 + (static_cast<std::uint32_t>(simm(w)) << 2); break;
        case 0x06: if (static_cast<std::int32_t>(r[rs]) <= 0) c.npc = pc + 4 + (static_cast<std::uint32_t>(simm(w)) << 2); break;
        case 0x07: if (static_cast<std::int32_t>(r[rs]) > 0) c.npc = pc + 4 + (static_cast<std::uint32_t>(simm(w)) << 2); break;
        case 0x08: case 0x09: r[rt] = r[rs] + static_cast<std::uint32_t>(simm(w)); break;
        case 0x0A: r[rt] = static_cast<std::int32_t>(r[rs]) < simm(w); break;
        case 0x0B: r[rt] = r[rs] < static_cast<std::uint32_t>(simm(w)); break;
        case 0x0C: r[rt] = r[rs] & (w & 0xFFFFu); break;
        case 0x0D: r[rt] = r[rs] | (w & 0xFFFFu); break;
        case 0x0E: r[rt] = r[rs] ^ (w & 0xFFFFu); break;
        case 0x0F: r[rt] = w << 16; break;
        case 0x10:
            if (rs == 0) r[rt] = rd == 12 ? c.status : rd == 13 ? c.cause : rd == 14 ? c.epc : rd == 15 ? 0x1Fu : 0u;
            else if (rs == 4) {
                if (rd == 12) kernel::set_status(c, r[rt]);
                else if (rd == 13) c.cause = r[rt];
                else if (rd == 14) c.epc = r[rt];
            } else if ((w & 63u) == 0x10) {
                c.status = (c.status & ~0xFu) | ((c.status >> 2) & 0xFu);
            }
            break;
        case 0x20: r[rt] = static_cast<std::uint32_t>(static_cast<std::int8_t>(read8(r[rs] + static_cast<std::uint32_t>(simm(w))))); break;
        case 0x21: r[rt] = static_cast<std::uint32_t>(static_cast<std::int16_t>(read16(r[rs] + static_cast<std::uint32_t>(simm(w))))); break;
        case 0x22: {
            const std::uint32_t a = r[rs] + static_cast<std::uint32_t>(simm(w)), shift = (3u - (a & 3u)) * 8u;
            const std::uint32_t mem = read32(a & ~3u);
            r[rt] = (mem << shift) | (shift ? (r[rt] & ((1u << shift) - 1u)) : 0u);
            break;
        }
        case 0x23: r[rt] = read32(r[rs] + static_cast<std::uint32_t>(simm(w))); break;
        case 0x24: r[rt] = read8(r[rs] + static_cast<std::uint32_t>(simm(w))); break;
        case 0x25: r[rt] = read16(r[rs] + static_cast<std::uint32_t>(simm(w))); break;
        case 0x26: {
            const std::uint32_t a = r[rs] + static_cast<std::uint32_t>(simm(w)), shift = (a & 3u) * 8u;
            const std::uint32_t mem = read32(a & ~3u);
            r[rt] = (mem >> shift) | (shift ? (r[rt] & ~(0xFFFFFFFFu >> shift)) : 0u);
            break;
        }
        case 0x28: write8(r[rs] + static_cast<std::uint32_t>(simm(w)), static_cast<std::uint8_t>(r[rt])); break;
        case 0x29: write16(r[rs] + static_cast<std::uint32_t>(simm(w)), static_cast<std::uint16_t>(r[rt])); break;
        case 0x2A: {
            const std::uint32_t a = r[rs] + static_cast<std::uint32_t>(simm(w)), shift = (3u - (a & 3u)) * 8u;
            const std::uint32_t mem = read32(a & ~3u);
            write32(a & ~3u, (r[rt] >> shift) | (shift ? (mem & ~(0xFFFFFFFFu >> shift)) : 0u));
            break;
        }
        case 0x2B: write32(r[rs] + static_cast<std::uint32_t>(simm(w)), r[rt]); break;
        case 0x2E: {
            const std::uint32_t a = r[rs] + static_cast<std::uint32_t>(simm(w)), shift = (a & 3u) * 8u;
            const std::uint32_t mem = read32(a & ~3u);
            write32(a & ~3u, (r[rt] << shift) | (shift ? (mem & ((1u << shift) - 1u)) : 0u));
            break;
        }
        case 0x2F: break;
        case kHleOpcode:
            c.pc = r[31];
            c.npc = c.pc + 4;
            r[0] = 0;
            call_hle(c, w & 0x03FFFFFFu);
            r[0] = 0;
            if (kernel::switch_pending()) return;
            continue;
        default:
            bad(c, pc, w);
            return;
        }
        r[0] = 0;
        if (__builtin_expect(kernel::switch_pending(), 0)) return;
    }
}

}
