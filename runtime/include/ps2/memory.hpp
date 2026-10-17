#pragma once

#include "cpu.hpp"

#include <cstdint>
#include <cstring>

namespace ps2 {

inline constexpr std::uint32_t kRamSize = 32u << 20;
inline constexpr std::uint32_t kScratchpadBase = 0x70000000u;
inline constexpr std::uint32_t kScratchpadSize = 16u << 10;

extern std::uint8_t g_ram[];
extern std::uint8_t g_spr[];

extern bool g_watch_on;
extern std::uint32_t g_watch_base, g_watch_end;
void watch_hit(std::uint32_t a, std::uint32_t v, const char *via);
void watch_span(std::uint32_t a, std::uint32_t n, const char *via);
void init_watch();

template <typename T>
inline bool watched(std::uint32_t a, std::uint32_t v) {
    if (g_watch_on && a >= g_watch_base && a < g_watch_end) [[unlikely]] watch_hit(a, v, "store");
    return false;
}

std::uint8_t slow_ld8(std::uint32_t a);
std::uint16_t slow_ld16(std::uint32_t a);
std::uint32_t slow_ld32(std::uint32_t a);
std::uint64_t slow_ld64(std::uint32_t a);
void slow_ld128(std::uint32_t a, Reg128 &out);
void slow_st8(std::uint32_t a, std::uint8_t v);
void slow_st16(std::uint32_t a, std::uint16_t v);
void slow_st32(std::uint32_t a, std::uint32_t v);
void slow_st64(std::uint32_t a, std::uint64_t v);
void slow_st128(std::uint32_t a, const Reg128 &v);

template <typename T>
inline T *fast(std::uint32_t a) {
    if (a < kRamSize) [[likely]] return reinterpret_cast<T *>(g_ram + a);
    if ((a >> 14) == (kScratchpadBase >> 14)) return reinterpret_cast<T *>(g_spr + (a & (kScratchpadSize - 1)));
    return nullptr;
}

template <typename T>
inline T load(const void *p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

template <typename T>
inline void store(void *p, T v) { std::memcpy(p, &v, sizeof v); }

inline std::uint8_t ld8(std::uint32_t a) {
    if (auto *p = fast<std::uint8_t>(a)) return *p;
    return slow_ld8(a);
}

inline std::uint16_t ld16(std::uint32_t a) {
    if (auto *p = fast<std::uint8_t>(a)) return load<std::uint16_t>(p);
    return slow_ld16(a);
}

inline std::uint32_t ld32(std::uint32_t a) {
    if (auto *p = fast<std::uint8_t>(a)) return load<std::uint32_t>(p);
    return slow_ld32(a);
}

inline std::uint64_t ld64(std::uint32_t a) {
    if (auto *p = fast<std::uint8_t>(a)) return load<std::uint64_t>(p);
    return slow_ld64(a);
}

inline void ld128(std::uint32_t a, Reg128 &out) {
    if (auto *p = fast<std::uint8_t>(a)) { std::memcpy(&out, p, 16); return; }
    slow_ld128(a, out);
}

inline void st8(std::uint32_t a, std::uint8_t v) {
    watched<std::uint8_t>(a, v);
    if (auto *p = fast<std::uint8_t>(a)) { *p = v; return; }
    slow_st8(a, v);
}

inline void st16(std::uint32_t a, std::uint16_t v) {
    watched<std::uint16_t>(a, v);
    if (auto *p = fast<std::uint8_t>(a)) { store(p, v); return; }
    slow_st16(a, v);
}

inline void st32(std::uint32_t a, std::uint32_t v) {
    watched<std::uint32_t>(a, v);
    if (auto *p = fast<std::uint8_t>(a)) { store(p, v); return; }
    slow_st32(a, v);
}

inline void st64(std::uint32_t a, std::uint64_t v) {
    watched<std::uint32_t>(a, static_cast<std::uint32_t>(v));
    if (auto *p = fast<std::uint8_t>(a)) { store(p, v); return; }
    slow_st64(a, v);
}

inline void st128(std::uint32_t a, const Reg128 &v) {
    watched<std::uint32_t>(a, v.w[0]);
    if (auto *p = fast<std::uint8_t>(a)) { std::memcpy(p, &v, 16); return; }
    slow_st128(a, v);
}

inline std::uint64_t mask_below(std::uint32_t bits) { return bits >= 64 ? ~0ull : (1ull << bits) - 1; }

inline void lwl(Reg128 &rt, std::uint32_t a) {
    const std::uint32_t shift = (3u - (a & 3u)) * 8u;
    const std::uint32_t mem = ld32(a & ~3u);
    const std::uint32_t v = (mem << shift) | (rt.w[0] & static_cast<std::uint32_t>(mask_below(shift)));
    rt.sd[0] = static_cast<std::int32_t>(v);
}

inline void lwr(Reg128 &rt, std::uint32_t a) {
    const std::uint32_t shift = (a & 3u) * 8u;
    const std::uint32_t mem = ld32(a & ~3u);
    const std::uint32_t v = (mem >> shift) | (rt.w[0] & ~static_cast<std::uint32_t>(mask_below(32 - shift)));
    if (shift == 0) rt.sd[0] = static_cast<std::int32_t>(v);
    else rt.w[0] = v;
}

inline void ldl(Reg128 &rt, std::uint32_t a) {
    const std::uint32_t shift = (7u - (a & 7u)) * 8u;
    const std::uint64_t mem = ld64(a & ~7u);
    rt.d[0] = (mem << shift) | (rt.d[0] & mask_below(shift));
}

inline void ldr(Reg128 &rt, std::uint32_t a) {
    const std::uint32_t shift = (a & 7u) * 8u;
    const std::uint64_t mem = ld64(a & ~7u);
    rt.d[0] = (mem >> shift) | (rt.d[0] & ~mask_below(64 - shift));
}

inline void swl(const Reg128 &rt, std::uint32_t a) {
    const std::uint32_t shift = (3u - (a & 3u)) * 8u;
    const std::uint32_t mem = ld32(a & ~3u);
    st32(a & ~3u, (rt.w[0] >> shift) | (mem & ~static_cast<std::uint32_t>(mask_below(32 - shift))));
}

inline void swr(const Reg128 &rt, std::uint32_t a) {
    const std::uint32_t shift = (a & 3u) * 8u;
    const std::uint32_t mem = ld32(a & ~3u);
    st32(a & ~3u, (rt.w[0] << shift) | (mem & static_cast<std::uint32_t>(mask_below(shift))));
}

inline void sdl(const Reg128 &rt, std::uint32_t a) {
    const std::uint32_t shift = (7u - (a & 7u)) * 8u;
    const std::uint64_t mem = ld64(a & ~7u);
    st64(a & ~7u, (rt.d[0] >> shift) | (mem & ~mask_below(64 - shift)));
}

inline void sdr(const Reg128 &rt, std::uint32_t a) {
    const std::uint32_t shift = (a & 7u) * 8u;
    const std::uint64_t mem = ld64(a & ~7u);
    st64(a & ~7u, (rt.d[0] << shift) | (mem & mask_below(shift)));
}

}
