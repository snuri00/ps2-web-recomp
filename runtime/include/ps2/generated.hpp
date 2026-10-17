#pragma once

#include "cpu.hpp"
#include "fpu.hpp"
#include "memory.hpp"
#include "mmi.hpp"
#include "vu.hpp"

#include <cstdint>

namespace ps2 {

using UnitFn = void (*)(Cpu &, std::uint32_t entry, std::uint32_t depth);

inline constexpr std::uint32_t kMaxDepth = 192;

struct UnitInfo {
    std::uint32_t base;
    std::uint32_t size;
    UnitFn function;
    const std::uint16_t *entries;
};

struct ModuleInfo {
    const char *name;
    // Zero for relocatable modules, which are found in memory by their header.
    // A fixed base is a code window the game itself rewrites: the module is a
    // precompiled image of one content that can occupy it, and the checks say
    // whether it is the one in place now.
    std::uint32_t fixed_base;
    std::uint32_t size;
    const std::uint32_t *checks;
    std::uint32_t check_count;
    const UnitInfo *units;
    std::uint32_t count;
};

void register_units(const UnitInfo *units, std::uint32_t count);
void register_modules(const ModuleInfo *modules, std::uint32_t count);

void dispatch(Cpu &c, std::uint32_t entry, std::uint32_t depth);
void call(Cpu &c, std::uint32_t target, std::uint32_t depth);
void bad_entry(Cpu &c, std::uint32_t unit_base);
bool preempt(Cpu &c);

void syscall(Cpu &c, std::uint32_t pc);
void breakpoint(Cpu &c, std::uint32_t pc);
void trap(Cpu &c, std::uint32_t pc);
void stop(Cpu &c, std::uint32_t pc, const char *what);
void probe(Cpu &c, std::uint32_t pc);

std::uint32_t mfc0(Cpu &c, std::uint32_t reg, std::uint32_t sel);
void mtc0(Cpu &c, std::uint32_t reg, std::uint32_t value);
void set_interrupts(Cpu &c, bool enabled);
std::uint32_t eret(Cpu &c);
bool cpcond0(Cpu &c);

std::uint32_t cfc1(Cpu &c, std::uint32_t reg);
void ctc1(Cpu &c, std::uint32_t reg, std::uint32_t value);

std::uint32_t cfc2(Cpu &c, std::uint32_t reg);
void ctc2(Cpu &c, std::uint32_t reg, std::uint32_t value);
void vcallms(Cpu &c, std::uint32_t address);
bool vu0_busy(Cpu &c);
void vu0_wait(Cpu &c);
std::uint8_t *vu0_data();

inline void set32(Reg128 &r, std::uint32_t v) { r.sd[0] = static_cast<std::int32_t>(v); }
inline void set64(Reg128 &r, std::uint64_t v) { r.d[0] = v; }

inline void fpu_compare(Cpu &c, bool condition) {
    c.fcr31 = condition ? (c.fcr31 | kFpuCondition) : (c.fcr31 & ~kFpuCondition);
}

inline void fpu_div(Cpu &c, unsigned fd, std::uint32_t a, std::uint32_t b) {
    const fp::Result r = fp::div(a, b);
    c.f[fd] = r.value;
    if (r.invalid) c.fcr31 |= 0x30000u;
}

inline void fpu_rsqrt(Cpu &c, unsigned fd, std::uint32_t a, std::uint32_t b) {
    const fp::Result r = fp::rsqrt(a, b);
    c.f[fd] = r.value;
    if (r.invalid) c.fcr31 |= 0x30000u;
}

inline std::uint32_t vu0_addr(std::uint32_t vi) { return (vi & 0xFFu) * 16u; }

inline void lqc2_vu(Cpu &c, unsigned dest, unsigned ft, std::uint32_t vi) {
    Reg128 data;
    __builtin_memcpy(&data, vu0_data() + vu0_addr(vi), 16);
    vu::apply(c.vu0, dest, ft, [&](int i) { return data.w[i]; });
}

inline void sqc2_vu(Cpu &c, unsigned dest, unsigned fs, std::uint32_t vi) {
    std::uint8_t *p = vu0_data() + vu0_addr(vi);
    for (int i = 0; i < 4; ++i)
        if (vu::lane(dest, i)) __builtin_memcpy(p + i * 4, &c.vu0.vf[fs].w[i], 4);
}

}
