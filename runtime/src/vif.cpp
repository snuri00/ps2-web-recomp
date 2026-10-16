#include "vif.hpp"

#include "dmac.hpp"
#include "gs/gif.hpp"
#include "gs/queue.hpp"
#include "hw.hpp"
#include "vu_micro.hpp"

#include "ps2/state.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace ps2::vif {

namespace {

struct Unit {
    std::uint32_t stat = 0, err = 0, mark = 0, cycle = 0, mode = 0, num = 0, mask = 0, code = 0;
    std::uint32_t itops = 0, base = 0, ofst = 0, tops = 0, itop = 0, top = 0;
    std::uint32_t row[4] = {}, col[4] = {};
    bool dbf = false, stalled = false;
    std::vector<std::uint32_t> pending;
};

Unit g_units[2];
Unit g_vu_side;
std::uint64_t g_commands[2][128];
std::uint8_t g_dry_run[16384];

bool on_vu_thread() { return gs::queue::role() == gs::queue::Role::Vu; }
bool shadow(int unit) { return unit == 1 && gs::queue::vu_active() && !on_vu_thread(); }
Unit &state(int unit) { return unit == 1 && on_vu_thread() ? g_vu_side : g_units[unit]; }

std::uint8_t *vu_data(int unit) { return unit ? g_vu1_mem : g_vu0_mem; }
std::uint32_t vu_qwords(int unit) { return unit ? 1024u : 256u; }

std::uint32_t element_bits(std::uint32_t vl) {
    switch (vl) {
    case 0: return 32;
    case 1: return 16;
    case 2: return 8;
    default: return 16;
    }
}

std::uint32_t unpack_words(std::uint32_t cmd, const Unit &u, std::uint32_t num) {
    const std::uint32_t vn = (cmd >> 26) & 3u, vl = (cmd >> 24) & 3u;
    const std::uint32_t cl = u.cycle & 0xFFu, wl = (u.cycle >> 8) & 0xFFu;
    std::uint32_t data = num;
    if (wl > cl && wl) data = (num / wl) * cl + std::min(num % wl, cl);
    const std::uint32_t bits = (vn == 3 && vl == 3) ? 16u : (vn + 1) * element_bits(vl);
    return (data * bits + 31) / 32;
}

void run_vu(int unit, std::uint32_t address) {
    Unit &u = state(unit);
    if (unit == 1) {
        u.top = u.tops;
        u.itop = u.itops;
        u.dbf = !u.dbf;
        u.tops = u.base + (u.dbf ? u.ofst : 0);
        if (shadow(unit)) return;
        vu::set_tops(u.top, u.itop);
    } else {
        u.itop = u.itops;
        vu::set_tops(0, u.itop);
    }
    vu::run(unit, address);
}

template <std::uint32_t Bits, std::uint32_t Components, bool Usn>
void unpack_fast(std::uint8_t *mem, std::uint32_t mask, std::uint32_t addr, std::uint32_t wl, std::uint32_t skip, const std::uint8_t *bytes, std::uint32_t num) {
    if constexpr (Bits == 32 && Components == 4) {
        if (skip == 0) {
            const std::uint32_t start_idx = addr & mask;
            const std::uint32_t limit = mask + 1;
            if (start_idx + num <= limit) {
                std::memcpy(mem + start_idx * 16, bytes, static_cast<std::size_t>(num) * 16);
                return;
            } else {
                const std::uint32_t first = limit - start_idx;
                std::memcpy(mem + start_idx * 16, bytes, static_cast<std::size_t>(first) * 16);
                std::memcpy(mem, bytes + first * 16, static_cast<std::size_t>(num - first) * 16);
                return;
            }
        }
    }
    std::uint32_t cycle = 0;
    for (std::uint32_t n = 0; n < num; ++n) {
        std::uint32_t value[4];
        for (std::uint32_t j = 0; j < Components; ++j) {
            const std::size_t at = static_cast<std::size_t>(n) * Components + j;
            if constexpr (Bits == 32) {
                std::memcpy(&value[j], bytes + at * 4, 4);
            } else if constexpr (Bits == 16) {
                std::uint16_t h;
                std::memcpy(&h, bytes + at * 2, 2);
                value[j] = Usn ? h : static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(h)));
            } else {
                value[j] = Usn ? bytes[at] : static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(bytes[at])));
            }
        }
        if constexpr (Components == 1) {
            value[1] = value[2] = value[3] = value[0];
        } else if constexpr (Components == 2) {
            value[2] = value[0];
            value[3] = value[1];
        } else if constexpr (Components == 3) {
            value[3] = 0;
        }
        std::memcpy(mem + (addr & mask) * 16, value, 16);
        ++addr;
        if (++cycle >= wl) {
            addr += skip;
            cycle = 0;
        }
    }
}

using FastUnpack = void (*)(std::uint8_t *, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, const std::uint8_t *, std::uint32_t);

template <std::uint32_t Bits>
constexpr FastUnpack fast_pair(std::uint32_t components, bool usn) {
    switch (components) {
    case 1: return usn ? unpack_fast<Bits, 1, true> : unpack_fast<Bits, 1, false>;
    case 2: return usn ? unpack_fast<Bits, 2, true> : unpack_fast<Bits, 2, false>;
    case 3: return usn ? unpack_fast<Bits, 3, true> : unpack_fast<Bits, 3, false>;
    default: return usn ? unpack_fast<Bits, 4, true> : unpack_fast<Bits, 4, false>;
    }
}

constexpr FastUnpack kFastUnpack[4][4][2] = {
    {{fast_pair<32>(1, false), fast_pair<32>(1, true)}, {fast_pair<32>(2, false), fast_pair<32>(2, true)}, {fast_pair<32>(3, false), fast_pair<32>(3, true)}, {fast_pair<32>(4, false), fast_pair<32>(4, true)}},
    {{fast_pair<16>(1, false), fast_pair<16>(1, true)}, {fast_pair<16>(2, false), fast_pair<16>(2, true)}, {fast_pair<16>(3, false), fast_pair<16>(3, true)}, {fast_pair<16>(4, false), fast_pair<16>(4, true)}},
    {{fast_pair<8>(1, false), fast_pair<8>(1, true)}, {fast_pair<8>(2, false), fast_pair<8>(2, true)}, {fast_pair<8>(3, false), fast_pair<8>(3, true)}, {fast_pair<8>(4, false), fast_pair<8>(4, true)}},
    {{fast_pair<16>(1, false), fast_pair<16>(1, true)}, {fast_pair<16>(2, false), fast_pair<16>(2, true)}, {fast_pair<16>(3, false), fast_pair<16>(3, true)}, {fast_pair<16>(4, false), fast_pair<16>(4, true)}},
};

void unpack(int unit, std::uint32_t cmd, const std::uint32_t *data) {
    Unit &u = state(unit);
    const bool dry = shadow(unit);
    if (dry && u.mode != 2) {
        u.num = 0;
        return;
    }
    const std::uint32_t vn = (cmd >> 26) & 3u, vl = (cmd >> 24) & 3u;
    const bool masked = (cmd >> 28) & 1u;
    const bool usn = (cmd >> 14) & 1u;
    std::uint32_t num = (cmd >> 16) & 0xFFu;
    if (!num) num = 256;
    std::uint32_t addr = cmd & 0x3FFu;
    if (unit == 1 && ((cmd >> 15) & 1u)) addr += u.tops;
    const std::uint32_t cl = u.cycle & 0xFFu, wl = (u.cycle >> 8) & 0xFFu;
    const std::uint32_t components = vn + 1;
    const std::uint32_t bits = element_bits(vl);
    std::size_t bit_pos = 0;
    auto read_element = [&]() -> std::uint32_t {
        const std::size_t word = bit_pos / 32, shift = bit_pos % 32;
        std::uint32_t raw = data[word] >> shift;
        if (bits < 32) raw &= (1u << bits) - 1u;
        bit_pos += bits;
        if (bits == 16) return usn ? raw : static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(raw)));
        if (bits == 8) return usn ? raw : static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(raw)));
        return raw;
    };
    std::uint32_t cycle = 0;
    std::uint8_t *mem = dry ? g_dry_run : vu_data(unit);
    const std::uint32_t wrap = vu_qwords(unit);
    if (!masked && u.mode == 0 && !(wl > cl) && !(vn == 3 && vl == 3)) {
        kFastUnpack[vl][vn][usn ? 1 : 0](mem, wrap - 1, addr, wl, cl - wl, reinterpret_cast<const std::uint8_t *>(data), num);
        u.num = 0;
        return;
    }
    for (std::uint32_t n = 0; n < num; ++n) {
        const bool fill = wl > cl && cycle >= cl;
        std::uint32_t value[4] = {0, 0, 0, 0};
        if (!fill) {
            if (vn == 3 && vl == 3) {
                const std::uint32_t c = read_element() & 0xFFFFu;
                value[0] = (c & 0x1Fu) << 3;
                value[1] = ((c >> 5) & 0x1Fu) << 3;
                value[2] = ((c >> 10) & 0x1Fu) << 3;
                value[3] = ((c >> 15) & 1u) << 7;
            } else {
                for (std::uint32_t j = 0; j < components; ++j) value[j] = read_element();
                if (components == 1) value[1] = value[2] = value[3] = value[0];
                else if (components == 2) { value[2] = value[0]; value[3] = value[1]; }
            }
        }
        std::uint8_t *q = mem + (addr % wrap) * 16;
        const std::uint32_t mrow = std::min<std::uint32_t>(cycle, 3u);
        for (std::uint32_t j = 0; j < 4; ++j) {
            std::uint32_t m = masked ? (u.mask >> ((mrow * 4 + j) * 2)) & 3u : 0u;
            if (fill && m == 0) m = 1;
            std::uint32_t out;
            if (m == 3) continue;
            if (m == 1) out = u.row[j];
            else if (m == 2) out = u.col[mrow];
            else if (u.mode == 1) out = value[j] + u.row[j];
            else if (u.mode == 2) { out = value[j] + u.row[j]; u.row[j] = out; }
            else out = value[j];
            std::memcpy(q + j * 4, &out, 4);
        }
        ++addr;
        ++cycle;
        if (cl >= wl) {
            if (cycle >= wl) {
                addr += cl - wl;
                cycle = 0;
            }
        } else if (cycle >= wl) {
            cycle = 0;
        }
    }
    u.num = 0;
}

std::uint32_t data_words(int unit, std::uint32_t cmd) {
    const Unit &u = state(unit);
    const std::uint32_t op = (cmd >> 24) & 0x7Fu;
    const std::uint32_t num = (cmd >> 16) & 0xFFu;
    switch (op) {
    case 0x20: return 1;
    case 0x30: case 0x31: return 4;
    case 0x4A: return (num ? num : 256u) * 2u;
    case 0x50: case 0x51: return ((cmd & 0xFFFFu) ? (cmd & 0xFFFFu) : 0x10000u) * 4u;
    default:
        if ((op & 0x60u) == 0x60u) return unpack_words(cmd, u, num ? num : 256u);
        return 0;
    }
}

void execute(int unit, std::uint32_t cmd, const std::uint32_t *data) {
    Unit &u = state(unit);
    const std::uint32_t op = (cmd >> 24) & 0x7Fu, imm = cmd & 0xFFFFu, num = (cmd >> 16) & 0xFFu;
    u.code = cmd;
    if (!on_vu_thread()) ++g_commands[unit][op];
    switch (op) {
    case 0x00: break;
    case 0x01: u.cycle = imm; break;
    case 0x02: u.ofst = imm & 0x3FFu; u.dbf = false; u.tops = u.base; break;
    case 0x03: u.base = imm & 0x3FFu; break;
    case 0x04: u.itops = imm & 0x3FFu; break;
    case 0x05: u.mode = imm & 3u; break;
    case 0x06: gif::set_path3_masked((imm >> 15) & 1u); break;
    case 0x07: u.mark = imm; break;
    case 0x10: case 0x11: case 0x13: break;
    case 0x14: case 0x15: run_vu(unit, imm * 8u); break;
    case 0x17: run_vu(unit, shadow(unit) ? 0u : vu::tpc(unit)); break;
    case 0x20: u.mask = data[0]; break;
    case 0x30: std::memcpy(u.row, data, 16); break;
    case 0x31: std::memcpy(u.col, data, 16); break;
    case 0x4A: {
        if (shadow(unit)) break;
        std::uint8_t *code = unit ? g_vu1_code : g_vu0_code;
        const std::uint32_t size = unit ? 16384u : 4096u;
        const std::uint32_t count = num ? num : 256u;
        bool changed = false;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint8_t *at = code + ((imm * 8u + i * 8u) & (size - 1));
            if (std::memcmp(at, data + i * 2, 8) == 0) continue;
            std::memcpy(at, data + i * 2, 8);
            changed = true;
        }
        if (changed) vu::code_changed(unit);
        break;
    }
    case 0x50: case 0x51:
        if (shadow(unit)) gif::scan_signals(2, reinterpret_cast<const std::uint8_t *>(data), imm ? imm : 0x10000u);
        else gif::transfer(2, reinterpret_cast<const std::uint8_t *>(data), imm ? imm : 0x10000u);
        break;
    default:
        if ((op & 0x60u) == 0x60u) unpack(unit, cmd, data);
        else std::fprintf(stderr, "[vif%d] unknown command 0x%08X\n", unit, cmd);
        break;
    }
    if ((cmd & 0x80000000u) && !(u.err & 1u)) {
        u.stat |= 0xC00u;
        u.stalled = true;
        if (!on_vu_thread()) hw::intc_raise(4u + static_cast<unsigned>(unit));
    }
}

std::size_t run(int unit, const std::uint32_t *words, std::size_t count) {
    Unit &u = state(unit);
    std::size_t pos = 0;
    while (pos < count && !u.stalled) {
        const std::uint32_t cmd = words[pos];
        const std::size_t start = pos + 1;
        const std::uint32_t need = data_words(unit, cmd);
        if (start + need > count) break;
        execute(unit, cmd, words + start);
        pos = start + need;
    }
    return pos;
}

void process(int unit) {
    Unit &u = state(unit);
    const std::size_t pos = run(unit, u.pending.data(), u.pending.size());
    u.pending.erase(u.pending.begin(), u.pending.begin() + static_cast<std::ptrdiff_t>(pos));
}

}

void reset() {
    for (Unit &u : g_units) u = Unit{};
}

void reset_vu_side() { g_vu_side = Unit{}; }

void stats() {
    for (int u = 0; u < 2; ++u) {
        std::fprintf(stderr, "[vif%d]", u);
        for (int op = 0; op < 128; ++op)
            if (g_commands[u][op]) std::fprintf(stderr, " %02X:%llu", op, static_cast<unsigned long long>(g_commands[u][op]));
        std::fprintf(stderr, "\n");
    }
}

void transfer(int unit, const std::uint8_t *data, std::uint32_t qwc) {
    if (shadow(unit)) gs::queue::vu().post(gs::queue::kVif1, qwc, 0, data, qwc * 16);
    Unit &u = state(unit);
    if (u.pending.empty() && !u.stalled) {
        const auto *words = reinterpret_cast<const std::uint32_t *>(data);
        const std::size_t count = static_cast<std::size_t>(qwc) * 4;
        const std::size_t pos = run(unit, words, count);
        if (pos < count) u.pending.assign(words + pos, words + count);
        return;
    }
    const std::size_t old = u.pending.size();
    u.pending.resize(old + static_cast<std::size_t>(qwc) * 4);
    std::memcpy(u.pending.data() + old, data, static_cast<std::size_t>(qwc) * 16);
    process(unit);
}

bool stalled(int unit) { return state(unit).stalled; }

std::uint32_t read(int unit, std::uint32_t reg) {
    const Unit &u = state(unit);
    switch (reg) {
    case 0x00: return u.stat | (u.dbf ? 0x80u : 0u);
    case 0x20: return u.err;
    case 0x30: return u.mark;
    case 0x40: return u.cycle;
    case 0x50: return u.mode;
    case 0x60: return u.num;
    case 0x70: return u.mask;
    case 0x80: return u.code;
    case 0x90: return u.itops;
    case 0xA0: return u.base;
    case 0xB0: return u.ofst;
    case 0xC0: return u.tops;
    case 0xD0: return u.itop;
    case 0xE0: return u.top;
    default:
        if (reg >= 0x100 && reg < 0x140) return u.row[(reg - 0x100) >> 4];
        if (reg >= 0x140 && reg < 0x180) return u.col[(reg - 0x140) >> 4];
        return 0;
    }
}

void write(int unit, std::uint32_t reg, std::uint32_t value) {
    if (shadow(unit)) gs::queue::vu().post(gs::queue::kVif1Write, reg, value, nullptr, 0);
    Unit &u = state(unit);
    switch (reg) {
    case 0x10:
        if (value & 1u) {
            u = Unit{};
        }
        if (value & 8u) {
            u.stat &= ~0x3F00u;
            if (u.stalled) {
                u.stalled = false;
                process(unit);
                if (!u.stalled && !on_vu_thread()) dmac::resume(unit);
            }
        }
        break;
    case 0x20: u.err = value; break;
    case 0x30: u.mark = value; break;
    default:
        if (reg >= 0x100 && reg < 0x140) u.row[(reg - 0x100) >> 4] = value;
        else if (reg >= 0x140 && reg < 0x180) u.col[(reg - 0x140) >> 4] = value;
        break;
    }
}

}

namespace ps2::vif {

void state(ps2::state::Stream &s) {
    s.tag("vif");
    for (Unit *u : {&g_units[0], &g_units[1], &g_vu_side}) {
        s.pod(u->stat);
        s.pod(u->err);
        s.pod(u->mark);
        s.pod(u->cycle);
        s.pod(u->mode);
        s.pod(u->num);
        s.pod(u->mask);
        s.pod(u->code);
        s.pod(u->itops);
        s.pod(u->base);
        s.pod(u->ofst);
        s.pod(u->tops);
        s.pod(u->itop);
        s.pod(u->top);
        s.pod(u->row);
        s.pod(u->col);
        s.pod(u->dbf);
        s.pod(u->stalled);
        s.vec(u->pending);
    }
}

}
