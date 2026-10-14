#include "hw.hpp"

#include "dmac.hpp"
#include "gs/gif.hpp"
#include "gs/gs.hpp"
#include "iop/sif.hpp"
#include "ipu/ipu.hpp"
#include "vif.hpp"
#include "vu_micro.hpp"
#include "kernel.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include "ps2/state.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace ps2::hw {

namespace {

constexpr std::uint32_t kIntcStat = 0x1000F000u, kIntcMask = 0x1000F010u;
constexpr std::uint32_t kDmacStat = 0x1000E010u;

// g_intc_latched is what the guest reads back: the kernel acknowledging an interrupt before
// running the handlers must not hide it from code that polls INTC_STAT, the way sceGsSyncV
// waits for VBlank start while a VBlank handler is installed. Only a guest write clears it.
std::uint32_t g_intc_stat = 0, g_intc_mask = 0, g_intc_latched = 0;
std::uint32_t g_dmac_stat = 0;
std::map<std::uint32_t, std::uint64_t> g_regs;
std::set<std::uint32_t> g_reported;
std::uint32_t g_crt[3] = {};

void report(const char *what, std::uint32_t a, std::uint64_t v) {
    if (g_options.trace_io || g_reported.insert(a).second)
        std::fprintf(stderr, "[io] %s 0x%08X = 0x%llX (pc 0x%08X)\n", what, a, static_cast<unsigned long long>(v), g_cpu.pc);
}

struct Timer {
    std::uint32_t mode = 0, comp = 0, hold = 0;
    std::uint64_t base = 0;
    std::uint32_t frozen = 0;
};

Timer g_timers[4];

std::uint64_t timer_divider(const Timer &t) {
    switch (t.mode & 3u) {
    case 0: return 2;
    case 1: return 32;
    case 2: return 512;
    default: return hblank_cycles();
    }
}

std::uint32_t timer_count(int n) {
    const Timer &t = g_timers[n];
    if (!(t.mode & 0x80u)) return t.frozen;
    return static_cast<std::uint32_t>(((now() - t.base) / timer_divider(t)) & 0xFFFFu);
}

void timer_set_count(int n, std::uint32_t value) {
    Timer &t = g_timers[n];
    t.frozen = value & 0xFFFFu;
    t.base = now() - static_cast<std::uint64_t>(value & 0xFFFFu) * timer_divider(t);
}

void timer_schedule(int n);

void timer_event(int n) {
    Timer &t = g_timers[n];
    if (!(t.mode & 0x80u)) return;
    const std::uint32_t count = timer_count(n);
    if (count == (t.comp & 0xFFFFu)) {
        t.mode |= 0x400u;
        if (t.mode & 0x100u) intc_raise(9u + static_cast<unsigned>(n));
        if (t.mode & 0x40u) timer_set_count(n, 0);
    }
    if (count == 0 && !(t.mode & 0x40u && t.comp == 0)) {
        t.mode |= 0x800u;
        if (t.mode & 0x200u) intc_raise(9u + static_cast<unsigned>(n));
    }
    timer_schedule(n);
}

void timer_schedule(int n) {
    Timer &t = g_timers[n];
    if (!(t.mode & 0x80u) || !(t.mode & 0x300u)) {
        schedule(kEventTimer0 + n, ~0ull, [] {});
        return;
    }
    const std::uint32_t count = timer_count(n);
    std::uint32_t to_compare = ((t.comp & 0xFFFFu) - count) & 0xFFFFu;
    if (!to_compare) to_compare = 0x10000u;
    std::uint32_t to_overflow = 0x10000u - count;
    std::uint32_t ticks = to_compare;
    if ((t.mode & 0x200u) && to_overflow < ticks) ticks = to_overflow;
    if (!(t.mode & 0x100u)) ticks = to_overflow;
    const std::uint64_t div = timer_divider(t);
    const std::uint64_t at = t.base + ((now() - t.base) / div + ticks) * div;
    schedule(kEventTimer0 + n, at, [n] { timer_event(n); });
}

int timer_index(std::uint32_t a) { return static_cast<int>((a >> 11) & 3u); }

bool is_timer(std::uint32_t a) { return a >= 0x10000000u && a < 0x10002000u && (a & 0x7FFu) < 0x40u; }

}

void reset() {
    g_intc_stat = g_intc_mask = g_intc_latched = g_dmac_stat = 0;
    gs::reset();
    gif::reset();
    vif::reset();
    vu::reset();
    dmac::reset();
    ipu::reset();
    g_regs.clear();
    for (Timer &t : g_timers) t = Timer{};
    g_timers[3].mode = 0x183u;
}

void vblank_start() {
    intc_raise(2);
    gs::vblank(true);
    schedule(kEventVblankEnd, now() + hblank_cycles() * 22u, [] { intc_raise(3); });
}

std::uint32_t intc_pending() { return g_intc_stat & g_intc_mask; }

void intc_raise(unsigned cause) {
    g_intc_stat |= 1u << cause;
    g_intc_latched |= 1u << cause;
    if (g_intc_mask & (1u << cause)) kernel::wake_idle();
}

void intc_ack(unsigned cause) { g_intc_stat &= ~(1u << cause); }

bool intc_set_mask(unsigned cause, bool enabled) {
    const std::uint32_t bit = 1u << cause;
    const bool was = g_intc_mask & bit;
    if (enabled) g_intc_mask |= bit;
    else g_intc_mask &= ~bit;
    return was != enabled;
}

std::uint32_t dmac_pending() {
    const std::uint32_t cis = g_dmac_stat & 0x3FFu, cim = (g_dmac_stat >> 16) & 0x3FFu;
    return cis & cim;
}

void dmac_ack(unsigned channel) { g_dmac_stat &= ~(1u << channel); }

bool dmac_set_mask(unsigned channel, bool enabled) {
    const std::uint32_t bit = 0x10000u << channel;
    const bool was = g_dmac_stat & bit;
    if (enabled) g_dmac_stat |= bit;
    else g_dmac_stat &= ~bit;
    return was != enabled;
}

std::uint64_t gs_imr() { return gs::read_privileged(0x12001010u); }
std::uint64_t gs_csr() { return gs::read_privileged(0x12001000u); }
void gs_set_imr(std::uint64_t value) { gs::write_privileged(0x12001010u, value); }

void gs_set_crt(std::uint32_t interlace, std::uint32_t mode, std::uint32_t field) {
    g_crt[0] = interlace;
    g_crt[1] = mode;
    g_crt[2] = field;
    std::fprintf(stderr, "[gs] SetGsCrt interlace %u mode 0x%X field %u\n", interlace, mode, field);
    if (!g_options.rate_forced) g_options.pal = mode == 3u;
}

void dmac_raise(unsigned channel) {
    g_dmac_stat |= 1u << channel;
    if (g_dmac_stat & (0x10000u << channel)) kernel::wake_idle();
}

std::uint32_t read32(std::uint32_t a) {
    if (a == kIntcStat) return g_intc_stat | g_intc_latched;
    if (a == kIntcMask) return g_intc_mask;
    if (a == kDmacStat) return g_dmac_stat;
    if (a >= 0x12000000u && a < 0x12002000u) return static_cast<std::uint32_t>(gs::read_privileged(a & ~7u) >> ((a & 4u) * 8));
    if (dmac::owns(a)) return dmac::read(a);
    if (a >= 0x10003000u && a < 0x10003800u) return gif::read(a);
    if (a >= 0x10003800u && a < 0x10003C00u) return vif::read(0, a & 0x3FFu);
    if (a >= 0x10003C00u && a < 0x10004000u) return vif::read(1, a & 0x3FFu);
    if (a >= 0x10002000u && a < 0x10002040u) return ipu::read32(a);
    if (is_timer(a)) {
        const int n = timer_index(a);
        switch (a & 0x30u) {
        case 0x00: return timer_count(n);
        case 0x10: return g_timers[n].mode;
        case 0x20: return g_timers[n].comp;
        default: return g_timers[n].hold;
        }
    }
    if (a >= 0x1000F200u && a < 0x1000F240u) return sif::get_reg(((a >> 4) & 7u) + 1u);
    if (a == 0x1000F130u) return 0;
    if (a == 0x1000F520u) return static_cast<std::uint32_t>(g_regs[0x1000F590u]);
    auto it = g_regs.find(a);
    const std::uint64_t v = it == g_regs.end() ? 0 : it->second;
    report("read", a, v);
    return static_cast<std::uint32_t>(v);
}

void write32(std::uint32_t a, std::uint32_t v) {
    if (a == kIntcStat) { g_intc_stat &= ~v; g_intc_latched &= ~v; return; }
    if (a == kIntcMask) { g_intc_mask ^= v; return; }
    if (a == kDmacStat) {
        g_dmac_stat &= ~(v & 0xFFFFu);
        g_dmac_stat ^= v & 0xFFFF0000u;
        return;
    }
    if (a >= 0x12000000u && a < 0x12002000u) {
        if (!(a & 4u)) gs::write_privileged(a, v);
        return;
    }
    if (dmac::owns(a)) { dmac::write(a, v); return; }
    if (a >= 0x10003000u && a < 0x10003800u) { gif::write(a, v); return; }
    if (a >= 0x10003800u && a < 0x10003C00u) { vif::write(0, a & 0x3FFu, v); return; }
    if (a >= 0x10003C00u && a < 0x10004000u) { vif::write(1, a & 0x3FFu, v); return; }
    if ((a >= 0x10002000u && a < 0x10002040u) || (a >= 0x10007010u && a < 0x10007020u)) { ipu::write32(a, v); return; }
    if (is_timer(a)) {
        const int n = timer_index(a);
        Timer &t = g_timers[n];
        switch (a & 0x30u) {
        case 0x00: timer_set_count(n, v); break;
        case 0x10: {
            const std::uint32_t count = timer_count(n);
            t.mode = (t.mode & 0xC00u & ~(v & 0xC00u)) | (v & 0x3FFu);
            timer_set_count(n, count);
            break;
        }
        case 0x20: t.comp = v & 0xFFFFu; break;
        default: t.hold = v & 0xFFFFu; break;
        }
        timer_schedule(n);
        return;
    }
    if (a >= 0x1000F200u && a < 0x1000F240u) { sif::set_reg(((a >> 4) & 7u) + 1u, v); return; }
    report("write", a, v);
    g_regs[a] = v;
}

std::uint64_t read64(std::uint32_t a) {
    if (a >= 0x12000000u && a < 0x12002000u) return gs::read_privileged(a);
    return read32(a) | (static_cast<std::uint64_t>(read32(a + 4)) << 32);
}

void write64(std::uint32_t a, std::uint64_t v) {
    if (a >= 0x12000000u && a < 0x12002000u) {
        gs::write_privileged(a, v);
        return;
    }
    write32(a, static_cast<std::uint32_t>(v));
    write32(a + 4, static_cast<std::uint32_t>(v >> 32));
}

void read128(std::uint32_t a, Reg128 &out) {
    std::memset(&out, 0, sizeof out);
    if (a >= 0x10005000u && a < 0x10006000u) {
        gs::read_image(reinterpret_cast<std::uint8_t *>(&out), 1);
        return;
    }
    if (a >= 0x10007000u && a < 0x10007010u) {
        ipu::read_fifo(reinterpret_cast<std::uint8_t *>(&out));
        return;
    }
    report("read128", a, 0);
}

void write128(std::uint32_t a, const Reg128 &v) {
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&v);
    if (a >= 0x10004000u && a < 0x10005000u) { vif::transfer(0, bytes, 1); return; }
    if (a >= 0x10005000u && a < 0x10006000u) { vif::transfer(1, bytes, 1); return; }
    if (a >= 0x10006000u && a < 0x10007000u) { gif::transfer(3, bytes, 1); return; }
    if (a >= 0x10007010u && a < 0x10007020u) { ipu::write_fifo(bytes); return; }
    report("write128", a, v.d[0]);
}

}

namespace ps2::hw {

std::function<void()> event_action(int id) {
    if (id == kEventVblankEnd) return [] { intc_raise(3); };
    if (id >= kEventTimer0 && id <= kEventTimer3) return [n = id - kEventTimer0] { timer_event(n); };
    return [] {};
}

void state(ps2::state::Stream &s) {
    s.tag("eehw");
    s.pod(g_intc_stat);
    s.pod(g_intc_latched);
    s.pod(g_intc_mask);
    s.pod(g_dmac_stat);
    s.map(g_regs);
    s.pod(g_crt);
    s.pod(g_timers);
}

}
