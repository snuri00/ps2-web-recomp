#include "gs.hpp"

#include "gif.hpp"
#include "queue.hpp"
#include "raster.hpp"
#include "../hw.hpp"
#ifdef PS2_GPU
#include "gpu/gpu.hpp"
#endif

#include "ps2/state.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <functional>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ps2::gs {

State g_state;
std::uint32_t g_setup_generation = 0;

void serve_state(const void *payload);

namespace {

alignas(64) std::uint8_t g_memory[kMemorySize];

std::uint64_t g_pmode, g_smode1, g_smode2, g_srfsh, g_synch1, g_synch2, g_syncv;
std::uint64_t g_dispfb[2], g_display[2], g_extbuf, g_extdata, g_extwrite, g_bgcolor;
std::uint64_t g_csr = 0x551B4000ull, g_imr = 0x7F00ull, g_busdir, g_siglblid;
std::uint64_t g_primitives = 0;

#ifdef PS2_GPU
bool gpu_on() { return gpu::mode() != gpu::Mode::off; }
bool cpu_raster() { return gpu::mode() != gpu::Mode::primary; }
#else
constexpr bool gpu_on() { return false; }
constexpr bool cpu_raster() { return true; }
#endif

bool trace_display() {
    static const bool enabled = std::getenv("PS2_TRACE_DISPLAY") != nullptr;
    return enabled;
}
std::uint64_t g_writes[128];
std::vector<std::uint32_t> g_gpu_frame;
bool g_odd_field = false;

struct Transfer {
    bool active = false;
    std::uint32_t x = 0, y = 0;
    std::uint32_t pending_bits = 0, pending = 0;
} g_transfer;

std::uint32_t g_off32[32][64], g_offz32[32][64];
std::uint32_t g_off16[64][64], g_off16s[64][64], g_offz16[64][64], g_offz16s[64][64];
std::uint32_t g_off8[64][128], g_off4[128][128];

const int kBlock32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23}, {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
const int kBlockZ32[4][8] = {{24, 25, 28, 29, 8, 9, 12, 13}, {26, 27, 30, 31, 10, 11, 14, 15}, {16, 17, 20, 21, 0, 1, 4, 5}, {18, 19, 22, 23, 2, 3, 6, 7}};
const int kColumn32[2][8] = {{0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15}};
const int kBlock16[8][4] = {{0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15}, {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
const int kBlock16S[8][4] = {{0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26}, {9, 11, 25, 27}, {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};
const int kBlockZ16[8][4] = {{24, 26, 16, 18}, {25, 27, 17, 19}, {28, 30, 20, 22}, {29, 31, 21, 23}, {8, 10, 0, 2}, {9, 11, 1, 3}, {12, 14, 4, 6}, {13, 15, 5, 7}};
const int kBlockZ16S[8][4] = {{24, 26, 8, 10}, {25, 27, 9, 11}, {16, 18, 0, 2}, {17, 19, 1, 3}, {28, 30, 12, 14}, {29, 31, 13, 15}, {20, 22, 4, 6}, {21, 23, 5, 7}};
const int kColumn16[2][16] = {{0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27}, {4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31}};
const int kColumnWord[2][2][8] = {{{0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15}}, {{8, 9, 12, 13, 0, 1, 4, 5}, {10, 11, 14, 15, 2, 3, 6, 7}}};

void build_tables() {
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 64; ++x) {
            const int column = (y & 7) / 2, cy = y & 1, cx = x & 7;
            g_off32[y][x] = static_cast<std::uint32_t>(kBlock32[y / 8][x / 8] * 256 + column * 64 + kColumn32[cy][cx] * 4);
            g_offz32[y][x] = static_cast<std::uint32_t>(kBlockZ32[y / 8][x / 8] * 256 + column * 64 + kColumn32[cy][cx] * 4);
        }
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const int column = (y & 7) / 2, cy = y & 1, cx = x & 15;
            const int inner = column * 64 + kColumn16[cy][cx] * 2;
            g_off16[y][x] = static_cast<std::uint32_t>(kBlock16[y / 8][x / 16] * 256 + inner);
            g_off16s[y][x] = static_cast<std::uint32_t>(kBlock16S[y / 8][x / 16] * 256 + inner);
            g_offz16[y][x] = static_cast<std::uint32_t>(kBlockZ16[y / 8][x / 16] * 256 + inner);
            g_offz16s[y][x] = static_cast<std::uint32_t>(kBlockZ16S[y / 8][x / 16] * 256 + inner);
        }
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 128; ++x) {
            const int block = kBlock32[(y / 16) & 3][(x / 16) & 7];
            int wx = x % 16, wy = y % 16;
            const int column = wy / 4;
            wy %= 4;
            std::uint32_t table = static_cast<std::uint32_t>((wy & 2) >> 1);
            const std::uint32_t byte = static_cast<std::uint32_t>(((wx & 8) >> 2) + ((wy & 2) >> 1));
            table ^= static_cast<std::uint32_t>((y / 4) & 1);
            g_off8[y][x] = static_cast<std::uint32_t>(block * 256 + column * 64 + kColumnWord[table][wy & 1][wx & 7] * 4) + byte;
        }
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            const int block = kBlock16[(y / 16) & 7][(x / 32) & 3];
            int wx = x % 32, wy = y % 16;
            const int column = wy / 4;
            wy %= 4;
            const std::uint32_t shift = static_cast<std::uint32_t>((wx & 0x18) + ((wy & 2) << 1));
            const std::uint32_t nibble = shift / 4;
            std::uint32_t table = static_cast<std::uint32_t>((wy & 2) >> 1);
            table ^= static_cast<std::uint32_t>(column & 1);
            g_off4[y][x] = static_cast<std::uint32_t>((column * 64 + block * 256 + kColumnWord[table][wy & 1][wx & 7] * 4) * 2) + nibble;
        }
}

std::uint32_t wrap(std::uint32_t a) { return a & (kMemorySize - 1); }

template <typename T>
T get(std::uint32_t a) {
    T v;
    std::memcpy(&v, g_memory + wrap(a), sizeof v);
    return v;
}

template <typename T>
void put(std::uint32_t a, T v) { std::memcpy(g_memory + wrap(a), &v, sizeof v); }

std::uint32_t page_address(std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y, std::uint32_t pw, std::uint32_t ph) {
    const std::uint32_t per_row = std::max<std::uint32_t>(1, bw * 64 / pw);
    return bp * 256 + ((x / pw) + (y / ph) * per_row) * 8192;
}

void raise(std::uint32_t bit) {
    g_csr |= bit;
    if (!(g_imr & (bit << 8))) hw::intc_raise(0);
}

std::uint32_t bits_per_pixel(std::uint32_t psm) {
    switch (psm) {
    case PSMCT24: case PSMZ24: return 24;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
    case PSMT8: return 8;
    case PSMT4: return 4;
    case PSMT8H: return 8;
    case PSMT4HL: case PSMT4HH: return 4;
    default: return 32;
    }
}

struct DownloadKey {
    std::uint64_t buf, pos, reg;
    std::uint32_t offset, qwords;
    bool operator==(const DownloadKey &) const = default;
};

struct CachedDownload {
    DownloadKey key;
    std::vector<std::uint8_t> data;
    std::uint64_t used;
};

std::mutex g_cache_mutex;
std::vector<CachedDownload> g_download_cache;
std::uint64_t g_cache_clock = 0;
DownloadKey g_shadow{};

void remember(const DownloadKey &key, const std::uint8_t *data, std::size_t bytes) {
    std::lock_guard lock(g_cache_mutex);
    auto it = std::find_if(g_download_cache.begin(), g_download_cache.end(), [&](const CachedDownload &c) { return c.key == key; });
    if (it == g_download_cache.end()) {
        if (g_download_cache.size() >= 16)
            g_download_cache.erase(std::min_element(g_download_cache.begin(), g_download_cache.end(),
                                                    [](const CachedDownload &a, const CachedDownload &b) { return a.used < b.used; }));
        it = g_download_cache.insert(g_download_cache.end(), CachedDownload{key, {}, 0});
    }
    it->data.assign(data, data + bytes);
    it->used = ++g_cache_clock;
}

void forget(const DownloadKey &key) {
    std::lock_guard lock(g_cache_mutex);
    std::erase_if(g_download_cache, [&](const CachedDownload &c) { return c.key == key; });
}

#ifdef PS2_GPU
std::vector<std::pair<std::uint32_t, std::uint32_t>> g_seen_ranges;
std::uint32_t g_transfer_serial = 0, g_stale_first = 0, g_stale_bytes = 0;
bool g_transfer_stale = false;

void download_source(const State &s) {
    const std::uint32_t sbp = static_cast<std::uint32_t>(s.bitbltbuf) & 0x3FFFu, sbw = static_cast<std::uint32_t>(s.bitbltbuf >> 16) & 0x3Fu;
    const std::uint32_t spsm = static_cast<std::uint32_t>(s.bitbltbuf >> 24) & 0x3Fu;
    const std::uint32_t x = static_cast<std::uint32_t>(s.trxpos) & 0x7FFu, y = static_cast<std::uint32_t>(s.trxpos >> 16) & 0x7FFu;
    const std::uint32_t w = static_cast<std::uint32_t>(s.trxreg) & 0xFFFu, h = static_cast<std::uint32_t>(s.trxreg >> 32) & 0xFFFu;
    const bool half = bits_per_pixel(spsm) == 16;
    const std::uint32_t pw = spsm == PSMT8 || spsm == PSMT4 ? 128 : 64, ph = spsm == PSMT4 ? 128 : spsm == PSMT8 || half ? 64 : 32;
    const std::uint32_t first = page_address(sbp, sbw, x, y, pw, ph);
    const std::uint32_t end = page_address(sbp, sbw, x + w - 1, y + h - 1, pw, ph) + 8192;
    ++g_transfer_serial;
    g_transfer_stale = false;
    if (!w || !h || x + w > 2048 || y + h > 2048 || end > kMemorySize) {
        gpu::download(g_memory);
        return;
    }
    const std::pair<std::uint32_t, std::uint32_t> range{first, end - first};
    if (queue::active() && std::find(g_seen_ranges.begin(), g_seen_ranges.end(), range) != g_seen_ranges.end()) {
        gpu::download_async(g_memory, first, end - first, g_transfer_serial);
        g_transfer_stale = true;
        g_stale_first = first;
        g_stale_bytes = end - first;
        return;
    }
    gpu::download(g_memory, first, end - first);
    g_seen_ranges.push_back(range);
    if (g_seen_ranges.size() > 8) g_seen_ranges.erase(g_seen_ranges.begin());
}
#endif

void exact_download() {
#ifdef PS2_GPU
    if (!g_transfer_stale) return;
    g_transfer_stale = false;
    if (!gpu::finish_download(g_transfer_serial)) gpu::download(g_memory, g_stale_first, g_stale_bytes);
#endif
}

void transfer_pixel(std::uint32_t value) {
    const std::uint64_t buf = g_state.bitbltbuf, pos = g_state.trxpos, reg = g_state.trxreg;
    const std::uint32_t dbp = static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu;
    const std::uint32_t dbw = static_cast<std::uint32_t>(buf >> 48) & 0x3Fu;
    const std::uint32_t dpsm = static_cast<std::uint32_t>(buf >> 56) & 0x3Fu;
    const std::uint32_t dsax = static_cast<std::uint32_t>(pos >> 32) & 0x7FFu;
    const std::uint32_t dsay = static_cast<std::uint32_t>(pos >> 48) & 0x7FFu;
    const std::uint32_t rrw = static_cast<std::uint32_t>(reg) & 0xFFFu;
    const std::uint32_t rrh = static_cast<std::uint32_t>(reg >> 32) & 0xFFFu;
    if (!g_transfer.active || !rrw) return;
    write_pixel(dpsm, dbp, dbw, (dsax + g_transfer.x) & 0x7FFu, (dsay + g_transfer.y) & 0x7FFu, value);
    if (++g_transfer.x >= rrw) {
        g_transfer.x = 0;
        if (++g_transfer.y >= rrh) g_transfer.active = false;
    }
}

std::uint8_t g_carry[4];
std::size_t g_carried = 0;

void feed_image(const std::uint8_t *data, std::size_t bytes) {
#ifdef PS2_GPU
    if (gpu_on() && (g_state.trxdir & 3u) == 0) gpu::image_data(data, bytes);
#endif
    if (!cpu_raster()) return;
    if (!g_transfer.active || (g_state.trxdir & 3u) != 0) return;
    const std::uint32_t psm = static_cast<std::uint32_t>(g_state.bitbltbuf >> 56) & 0x3Fu;
    const std::uint32_t bits = bits_per_pixel(psm);
    if (bits == 24) {
        for (std::size_t i = 0; i < bytes && g_transfer.active; ++i) {
            g_carry[g_carried++] = data[i];
            if (g_carried == 3) {
                transfer_pixel(g_carry[0] | (g_carry[1] << 8) | (g_carry[2] << 16));
                g_carried = 0;
            }
        }
        return;
    }
    for (std::size_t bit = 0; bit + bits <= bytes * 8 && g_transfer.active; bit += bits) {
        std::uint32_t v = 0;
        if (bits == 4) v = (data[bit / 8] >> (bit & 4)) & 15u;
        else std::memcpy(&v, data + bit / 8, bits / 8);
        transfer_pixel(v);
    }
}

void local_copy() {
    const std::uint64_t buf = g_state.bitbltbuf, pos = g_state.trxpos, reg = g_state.trxreg;
    const std::uint32_t sbp = static_cast<std::uint32_t>(buf) & 0x3FFFu, sbw = static_cast<std::uint32_t>(buf >> 16) & 0x3Fu, spsm = static_cast<std::uint32_t>(buf >> 24) & 0x3Fu;
    const std::uint32_t dbp = static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu, dbw = static_cast<std::uint32_t>(buf >> 48) & 0x3Fu, dpsm = static_cast<std::uint32_t>(buf >> 56) & 0x3Fu;
    const std::uint32_t ssax = static_cast<std::uint32_t>(pos) & 0x7FFu, ssay = static_cast<std::uint32_t>(pos >> 16) & 0x7FFu;
    const std::uint32_t dsax = static_cast<std::uint32_t>(pos >> 32) & 0x7FFu, dsay = static_cast<std::uint32_t>(pos >> 48) & 0x7FFu;
    const std::uint32_t dir = static_cast<std::uint32_t>(pos >> 59) & 3u;
    const std::uint32_t rrw = static_cast<std::uint32_t>(reg) & 0xFFFu, rrh = static_cast<std::uint32_t>(reg >> 32) & 0xFFFu;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(rrw) * rrh);
    for (std::uint32_t y = 0; y < rrh; ++y)
        for (std::uint32_t x = 0; x < rrw; ++x)
            pixels[y * rrw + x] = read_pixel(spsm, sbp, sbw, (ssax + x) & 0x7FFu, (ssay + y) & 0x7FFu);
    for (std::uint32_t y = 0; y < rrh; ++y) {
        const std::uint32_t sy = (dir & 2u) ? rrh - 1 - y : y;
        for (std::uint32_t x = 0; x < rrw; ++x) {
            const std::uint32_t sx = (dir & 1u) ? rrw - 1 - x : x;
            write_pixel(dpsm, dbp, dbw, (dsax + x) & 0x7FFu, (dsay + y) & 0x7FFu, pixels[sy * rrw + sx]);
        }
    }
}

void load_clut(std::uint64_t tex0) {
    const std::uint32_t cld = static_cast<std::uint32_t>(tex0 >> 61) & 7u;
    static std::uint32_t cbp0 = ~0u, cbp1 = ~0u;
    const std::uint32_t cbp = static_cast<std::uint32_t>(tex0 >> 37) & 0x3FFFu;
    switch (cld) {
    case 0: return;
    case 1: break;
    case 2: cbp0 = cbp; break;
    case 3: cbp1 = cbp; break;
    case 4: if (cbp0 == cbp) return; cbp0 = cbp; break;
    case 5: if (cbp1 == cbp) return; cbp1 = cbp; break;
    default: return;
    }
    const std::uint32_t psm = static_cast<std::uint32_t>(tex0 >> 20) & 0x3Fu;
    const std::uint32_t cpsm = static_cast<std::uint32_t>(tex0 >> 51) & 0xFu;
    const std::uint32_t csm = static_cast<std::uint32_t>(tex0 >> 55) & 1u;
    const std::uint32_t csa = static_cast<std::uint32_t>(tex0 >> 56) & 0x1Fu;
#ifdef PS2_GPU
    if (gpu_on()) gpu::load_clut(tex0, g_state.texclut);
#endif
    if (!cpu_raster()) return;
    const bool eight = psm == PSMT8 || psm == PSMT8H;
    const std::uint32_t entries = eight ? 256u : 16u;
    const std::uint32_t texclut = static_cast<std::uint32_t>(g_state.texclut);
    for (std::uint32_t i = 0; i < entries; ++i) {
        std::uint32_t x, y;
        if (csm) {
            const std::uint32_t cbw = texclut & 0x3Fu, cou = (texclut >> 6) & 0x3Fu, cov = (texclut >> 12) & 0x3FFu;
            x = cou * 16 + i;
            y = cov;
            const std::uint32_t c = read_pixel(cpsm == 0 ? PSMCT32 : cpsm, cbp, cbw, x, y);
            const std::uint32_t slot = (csa * 16 + i) & 511u;
            g_state.clut[slot] = c;
            continue;
        }
        if (eight) {
            const std::uint32_t j = (i & ~0x18u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1);
            x = j & 15u;
            y = j >> 4;
        } else {
            x = i & 7u;
            y = i >> 3;
        }
        const std::uint32_t c = read_pixel(cpsm == 0 ? PSMCT32 : cpsm, cbp, 1, x, y);
        const std::uint32_t slot = eight ? i : ((csa & 15u) * 16u + i) & 255u;
        g_state.clut[slot] = c;
    }
}

}

std::uint8_t *memory() { return g_memory; }

void swizzle_tables(std::vector<std::uint32_t> &out) {
    out.clear();
    for (auto &row : g_off32) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_offz32) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_off16) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_off16s) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_offz16) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_offz16s) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_off8) out.insert(out.end(), std::begin(row), std::end(row));
    for (auto &row : g_off4) out.insert(out.end(), std::begin(row), std::end(row));
}

std::uint32_t address32(std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y) {
    return wrap(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63]);
}

std::uint32_t read_pixel(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y) {
    switch (psm) {
    case PSMCT32: return get<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63]);
    case PSMCT24: return get<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63]) & 0xFFFFFFu;
    case PSMZ32: return get<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_offz32[y & 31][x & 63]);
    case PSMZ24: return get<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_offz32[y & 31][x & 63]) & 0xFFFFFFu;
    case PSMCT16: return get<std::uint16_t>(page_address(bp, bw, x, y, 64, 64) + g_off16[y & 63][x & 63]);
    case PSMCT16S: return get<std::uint16_t>(page_address(bp, bw, x, y, 64, 64) + g_off16s[y & 63][x & 63]);
    case PSMZ16: return get<std::uint16_t>(page_address(bp, bw, x, y, 64, 64) + g_offz16[y & 63][x & 63]);
    case PSMZ16S: return get<std::uint16_t>(page_address(bp, bw, x, y, 64, 64) + g_offz16s[y & 63][x & 63]);
    case PSMT8: return get<std::uint8_t>(page_address(bp, bw, x, y, 128, 64) + g_off8[y & 63][x & 127]);
    case PSMT4: {
        const std::uint32_t n = g_off4[y & 127][x & 127];
        const std::uint8_t b = get<std::uint8_t>(page_address(bp, bw, x, y, 128, 128) + n / 2);
        return (n & 1u) ? b >> 4 : b & 15u;
    }
    case PSMT8H: return get<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63]) >> 24;
    case PSMT4HL: return (get<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63]) >> 24) & 15u;
    case PSMT4HH: return get<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63]) >> 28;
    default: return get<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63]);
    }
}

void write_pixel(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y, std::uint32_t value) {
    switch (psm) {
    case PSMCT32: put<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63], value); break;
    case PSMZ32: put<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_offz32[y & 31][x & 63], value); break;
    case PSMCT24: {
        const std::uint32_t a = page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63];
        put<std::uint32_t>(a, (get<std::uint32_t>(a) & 0xFF000000u) | (value & 0xFFFFFFu));
        break;
    }
    case PSMZ24: {
        const std::uint32_t a = page_address(bp, bw, x, y, 64, 32) + g_offz32[y & 31][x & 63];
        put<std::uint32_t>(a, (get<std::uint32_t>(a) & 0xFF000000u) | (value & 0xFFFFFFu));
        break;
    }
    case PSMCT16: put<std::uint16_t>(page_address(bp, bw, x, y, 64, 64) + g_off16[y & 63][x & 63], static_cast<std::uint16_t>(value)); break;
    case PSMCT16S: put<std::uint16_t>(page_address(bp, bw, x, y, 64, 64) + g_off16s[y & 63][x & 63], static_cast<std::uint16_t>(value)); break;
    case PSMZ16: put<std::uint16_t>(page_address(bp, bw, x, y, 64, 64) + g_offz16[y & 63][x & 63], static_cast<std::uint16_t>(value)); break;
    case PSMZ16S: put<std::uint16_t>(page_address(bp, bw, x, y, 64, 64) + g_offz16s[y & 63][x & 63], static_cast<std::uint16_t>(value)); break;
    case PSMT8: put<std::uint8_t>(page_address(bp, bw, x, y, 128, 64) + g_off8[y & 63][x & 127], static_cast<std::uint8_t>(value)); break;
    case PSMT4: {
        const std::uint32_t n = g_off4[y & 127][x & 127];
        const std::uint32_t a = page_address(bp, bw, x, y, 128, 128) + n / 2;
        std::uint8_t b = get<std::uint8_t>(a);
        b = (n & 1u) ? static_cast<std::uint8_t>((b & 0x0Fu) | ((value & 15u) << 4)) : static_cast<std::uint8_t>((b & 0xF0u) | (value & 15u));
        put<std::uint8_t>(a, b);
        break;
    }
    case PSMT8H: {
        const std::uint32_t a = page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63];
        put<std::uint32_t>(a, (get<std::uint32_t>(a) & 0x00FFFFFFu) | (value << 24));
        break;
    }
    case PSMT4HL: {
        const std::uint32_t a = page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63];
        put<std::uint32_t>(a, (get<std::uint32_t>(a) & 0xF0FFFFFFu) | ((value & 15u) << 24));
        break;
    }
    case PSMT4HH: {
        const std::uint32_t a = page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63];
        put<std::uint32_t>(a, (get<std::uint32_t>(a) & 0x0FFFFFFFu) | ((value & 15u) << 28));
        break;
    }
    default: put<std::uint32_t>(page_address(bp, bw, x, y, 64, 32) + g_off32[y & 31][x & 63], value); break;
    }
}

void init() {
    static bool built = false;
    if (built) return;
    build_tables();
    built = true;
#ifdef PS2_GPU
    const char *backend = std::getenv("PS2_GS");
    const gpu::Mode wanted = !backend || std::strcmp(backend, "gpu") == 0 ? gpu::Mode::primary : std::strcmp(backend, "both") == 0 ? gpu::Mode::mirror : gpu::Mode::off;
    if (wanted != gpu::Mode::off && !gpu::init(wanted)) std::fprintf(stderr, "[gs] WebGPU unavailable, using the software renderer\n");
#endif
}

void render_reset() {
#ifdef PS2_GPU
    g_seen_ranges.clear();
    g_transfer_stale = false;
#endif
    init();
    std::memset(&g_state, 0, sizeof g_state);
    std::memset(g_memory, 0, sizeof g_memory);
    ++g_setup_generation;
    g_transfer = Transfer{};
    g_primitives = 0;
    raster::reset();
}

void reset() {
    g_shadow = DownloadKey{};
    {
        std::lock_guard lock(g_cache_mutex);
        g_download_cache.clear();
    }
    g_csr = 0x551B4000ull;
    g_imr = 0x7F00ull;
    if (queue::active()) queue::post(queue::kReset, 0, 0, nullptr, 0);
    else render_reset();
}

void kick(bool draw) {
    State &s = g_state;
    const std::uint32_t prim = static_cast<std::uint32_t>(s.prim) & 7u;
    const int needed = prim == 0 ? 1 : (prim == 1 || prim == 2 || prim == 6) ? 2 : prim == 7 ? 1 : 3;
    if (s.count < needed) return;
    if (draw) {
        if (cpu_raster()) raster::draw(s, s.queue, needed);
#ifdef PS2_GPU
        if (gpu_on()) gpu::draw(s, s.queue, needed);
#endif
        ++g_primitives;
    }
    switch (prim) {
    case 2:
        s.queue[0] = s.queue[1];
        s.count = 1;
        break;
    case 4:
        s.queue[0] = s.queue[1];
        s.queue[1] = s.queue[2];
        s.count = 2;
        break;
    case 5:
        s.queue[1] = s.queue[2];
        s.count = 2;
        break;
    default:
        s.count = 0;
        break;
    }
}

void vertex(std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint8_t f, bool draw) {
    State &s = g_state;
    if (s.count >= 3) s.count = 2;
    Vertex &v = s.queue[s.count++];
    v.x = x;
    v.y = y;
    v.z = z;
    v.f = f;
    v.r = static_cast<std::uint8_t>(s.rgbaq);
    v.g = static_cast<std::uint8_t>(s.rgbaq >> 8);
    v.b = static_cast<std::uint8_t>(s.rgbaq >> 16);
    v.a = static_cast<std::uint8_t>(s.rgbaq >> 24);
    std::uint32_t qbits = static_cast<std::uint32_t>(s.rgbaq >> 32), sbits = static_cast<std::uint32_t>(s.st), tbits = static_cast<std::uint32_t>(s.st >> 32);
    std::memcpy(&v.q, &qbits, 4);
    std::memcpy(&v.s, &sbits, 4);
    std::memcpy(&v.t, &tbits, 4);
    v.u = static_cast<std::uint32_t>(s.uv) & 0x3FFFu;
    v.v = static_cast<std::uint32_t>(s.uv >> 16) & 0x3FFFu;
    kick(draw);
}

void write(std::uint32_t reg, std::uint64_t value) {
    State &s = g_state;
    ++g_writes[reg & 127u];
    constexpr std::uint64_t kVertexRegs = (1ull << 0x01) | (1ull << 0x02) | (1ull << 0x03) | (1ull << 0x04) | (1ull << 0x05) | (1ull << 0x0A) | (1ull << 0x0C) | (1ull << 0x0D);
    if (reg >= 64 || !((kVertexRegs >> reg) & 1u)) ++g_setup_generation;
    static const char *seq = std::getenv("PS2_TRACE_GSSEQ");
    if (__builtin_expect(seq != nullptr, 0)) {
        static std::uint64_t n = 0, start = std::strtoull(seq, nullptr, 10);
        if (++n >= start && n < start + 100000) std::fprintf(stderr, "[gs] w %02X %016llX\n", reg, static_cast<unsigned long long>(value));
    }
    switch (reg) {
    case 0x00:
        s.prim = value;
        s.count = 0;
        break;
    case 0x01: s.rgbaq = value; break;
    case 0x02: s.st = value; break;
    case 0x03: s.uv = value; break;
    case 0x04: vertex(static_cast<std::uint32_t>(value) & 0xFFFFu, static_cast<std::uint32_t>(value >> 16) & 0xFFFFu,
                      static_cast<std::uint32_t>(value >> 32) & 0xFFFFFFu, static_cast<std::uint8_t>(value >> 56), true); break;
    case 0x05: vertex(static_cast<std::uint32_t>(value) & 0xFFFFu, static_cast<std::uint32_t>(value >> 16) & 0xFFFFu,
                      static_cast<std::uint32_t>(value >> 32), static_cast<std::uint8_t>(s.fog >> 56), true); break;
    case 0x06: case 0x07:
        s.ctx[reg - 6].tex0 = value;
        load_clut(value);
        break;
    case 0x08: case 0x09: s.ctx[reg - 8].clamp = value; break;
    case 0x0A: s.fog = value; break;
    case 0x0C: vertex(static_cast<std::uint32_t>(value) & 0xFFFFu, static_cast<std::uint32_t>(value >> 16) & 0xFFFFu,
                      static_cast<std::uint32_t>(value >> 32) & 0xFFFFFFu, static_cast<std::uint8_t>(value >> 56), false); break;
    case 0x0D: vertex(static_cast<std::uint32_t>(value) & 0xFFFFu, static_cast<std::uint32_t>(value >> 16) & 0xFFFFu,
                      static_cast<std::uint32_t>(value >> 32), static_cast<std::uint8_t>(s.fog >> 56), false); break;
    case 0x14: case 0x15: s.ctx[reg - 0x14].tex1 = value; break;
    case 0x16: case 0x17: {
        Context &c = s.ctx[reg - 0x16];
        const std::uint64_t mask = 0xFFFFFFE0'03F00000ull;
        c.tex0 = (c.tex0 & ~mask) | (value & mask);
        load_clut(c.tex0);
        break;
    }
    case 0x18: case 0x19: s.ctx[reg - 0x18].xyoffset = value; break;
    case 0x1A: s.prmodecont = value; break;
    case 0x1B: s.prmode = value; break;
    case 0x1C: s.texclut = value; break;
    case 0x22: s.scanmsk = value; break;
    case 0x34: case 0x35: s.ctx[reg - 0x34].miptbp1 = value; break;
    case 0x36: case 0x37: s.ctx[reg - 0x36].miptbp2 = value; break;
    case 0x3B: s.texa = value; break;
    case 0x3D: s.fogcol = value; break;
    case 0x3F:
#ifdef PS2_GPU
        if (gpu_on()) gpu::texflush();
#endif
        break;
    case 0x40: case 0x41: s.ctx[reg - 0x40].scissor = value; break;
    case 0x42: case 0x43: s.ctx[reg - 0x42].alpha = value; break;
    case 0x44: s.dimx = value; break;
    case 0x45: s.dthe = value; break;
    case 0x46: s.colclamp = value; break;
    case 0x47: case 0x48: s.ctx[reg - 0x47].test = value; break;
    case 0x49: s.pabe = value; break;
    case 0x4A: case 0x4B: s.ctx[reg - 0x4A].fba = value; break;
    case 0x4C: case 0x4D: s.ctx[reg - 0x4C].frame = value; break;
    case 0x4E: case 0x4F: s.ctx[reg - 0x4E].zbuf = value; break;
    case 0x50: s.bitbltbuf = value; break;
    case 0x51: s.trxpos = value; break;
    case 0x52: s.trxreg = value; break;
    case 0x53:
        if (trace_display()) {
            std::fprintf(stderr, "[gs] trx dir %llu buf 0x%016llX pos 0x%016llX reg 0x%016llX\n", static_cast<unsigned long long>(value & 3u),
                         static_cast<unsigned long long>(s.bitbltbuf), static_cast<unsigned long long>(s.trxpos), static_cast<unsigned long long>(s.trxreg));
        }
        s.trxdir = value;
        g_transfer = Transfer{};
        g_carried = 0;
#ifdef PS2_GPU
        if (gpu_on()) {
            if ((value & 3u) == 0) gpu::image_begin(s);
            else if ((value & 3u) == 2) gpu::local_copy(s);
            else if ((value & 3u) == 1 && !cpu_raster()) download_source(s);
        }
#endif
        if ((value & 3u) == 0) g_transfer.active = true;
        else if ((value & 3u) == 2) { if (cpu_raster()) local_copy(); }
        else if ((value & 3u) == 1) g_transfer.active = true;
        break;
    case 0x54:
        feed_image(reinterpret_cast<const std::uint8_t *>(&value), 8);
        break;
    case 0x60: case 0x61: case 0x62:
        if (!queue::active()) signal(reg, value);
        break;
    default:
        break;
    }
}

void signal(std::uint32_t reg, std::uint64_t value) {
    const std::uint32_t mask = static_cast<std::uint32_t>(value >> 32), id = static_cast<std::uint32_t>(value);
    switch (reg) {
    case 0x60:
        g_siglblid = (g_siglblid & ~static_cast<std::uint64_t>(mask)) | (id & mask);
        raise(1);
        break;
    case 0x61: raise(2); break;
    case 0x62: {
        const std::uint64_t label = (g_siglblid >> 32 & ~static_cast<std::uint64_t>(mask)) | (id & mask);
        g_siglblid = (g_siglblid & 0xFFFFFFFFull) | (label << 32);
        break;
    }
    default: break;
    }
}

void write_packed(std::uint32_t reg, const Reg128 &q) {
    State &s = g_state;
    switch (reg) {
    case 0x0: write(0x00, q.d[0] & 0x7FFu); break;
    case 0x1: {
        std::uint32_t qbits;
        std::memcpy(&qbits, &s.q, 4);
        s.rgbaq = (q.w[0] & 0xFFu) | ((q.w[1] & 0xFFu) << 8) | ((q.w[2] & 0xFFu) << 16) | ((q.w[3] & 0xFFu) << 24) |
                  (static_cast<std::uint64_t>(qbits) << 32);
        break;
    }
    case 0x2:
        s.st = q.d[0];
        std::memcpy(&s.q, &q.w[2], 4);
        break;
    case 0x3: s.uv = (q.w[0] & 0x3FFFu) | ((q.w[1] & 0x3FFFu) << 16); break;
    case 0x4: {
        const bool nodraw = ((q.d[1] >> 47) & 1u);
        vertex(q.w[0] & 0xFFFFu, q.w[1] & 0xFFFFu, (q.w[2] >> 4) & 0xFFFFFFu, static_cast<std::uint8_t>((q.w[3] >> 4) & 0xFFu), !nodraw);
        break;
    }
    case 0x5: {
        const bool nodraw = ((q.d[1] >> 47) & 1u);
        vertex(q.w[0] & 0xFFFFu, q.w[1] & 0xFFFFu, q.w[2], static_cast<std::uint8_t>(s.fog >> 56), !nodraw);
        break;
    }
    case 0xA: s.fog = static_cast<std::uint64_t>((q.w[3] >> 4) & 0xFFu) << 56; break;
    case 0xC: vertex(q.w[0] & 0xFFFFu, q.w[1] & 0xFFFFu, (q.w[2] >> 4) & 0xFFFFFFu, static_cast<std::uint8_t>((q.w[3] >> 4) & 0xFFu), false); break;
    case 0xD: vertex(q.w[0] & 0xFFFFu, q.w[1] & 0xFFFFu, q.w[2], static_cast<std::uint8_t>(s.fog >> 56), false); break;
    case 0xE: write(static_cast<std::uint32_t>(q.d[1] & 0xFFu), q.d[0]); break;
    case 0xF: break;
    default: write(reg, q.d[0]); break;
    }
}

void write_image(const std::uint8_t *data, std::uint32_t qwords) { feed_image(data, static_cast<std::size_t>(qwords) * 16); }

std::atomic<std::uint32_t> g_downloads{0};

void read_image(std::uint8_t *data, std::uint32_t qwords) {
    if (!queue::active()) {
        render_read_image(data, qwords);
        return;
    }
    DownloadKey key = g_shadow;
    key.qwords = qwords;
    g_shadow.offset += qwords * 16;
    const std::size_t bytes = static_cast<std::size_t>(qwords) * 16;
    {
        std::lock_guard lock(g_cache_mutex);
        const auto it = std::find_if(g_download_cache.begin(), g_download_cache.end(), [&](const CachedDownload &c) { return c.key == key; });
        if (it != g_download_cache.end() && it->data.size() == bytes) {
            std::memcpy(data, it->data.data(), bytes);
            it->used = ++g_cache_clock;
            queue::post(queue::kDownloadAsync, qwords, 0, &key, sizeof key);
            return;
        }
    }
    const std::uint32_t seen = g_downloads.load();
    const std::uintptr_t target = reinterpret_cast<std::uintptr_t>(data);
    queue::post(queue::kDownload, qwords, 0, &target, sizeof target);
    while (g_downloads.load() == seen) queue::wait(g_downloads, seen);
    remember(key, data, bytes);
}

void note_transfer(std::uint32_t reg, std::uint64_t value) {
    switch (reg) {
    case 0x50: g_shadow.buf = value; break;
    case 0x51: g_shadow.pos = value; break;
    case 0x52: g_shadow.reg = value; break;
    default: g_shadow.offset = 0; break;
    }
}

void render_read_image(std::uint8_t *data, std::uint32_t qwords) {
    const std::size_t bytes = static_cast<std::size_t>(qwords) * 16;
    std::memset(data, 0, bytes);
    if (!g_transfer.active || (g_state.trxdir & 3u) != 1) return;
    const std::uint64_t buf = g_state.bitbltbuf, pos = g_state.trxpos, reg = g_state.trxreg;
    const std::uint32_t sbp = static_cast<std::uint32_t>(buf) & 0x3FFFu, sbw = static_cast<std::uint32_t>(buf >> 16) & 0x3Fu;
    const std::uint32_t spsm = static_cast<std::uint32_t>(buf >> 24) & 0x3Fu;
    const std::uint32_t ssax = static_cast<std::uint32_t>(pos) & 0x7FFu, ssay = static_cast<std::uint32_t>(pos >> 16) & 0x7FFu;
    const std::uint32_t rrw = static_cast<std::uint32_t>(reg) & 0xFFFu, rrh = static_cast<std::uint32_t>(reg >> 32) & 0xFFFu;
    const std::uint32_t bits = bits_per_pixel(spsm);
    auto next = [&]() {
        const std::uint32_t v = read_pixel(spsm, sbp, sbw, (ssax + g_transfer.x) & 0x7FFu, (ssay + g_transfer.y) & 0x7FFu);
        if (++g_transfer.x >= rrw) {
            g_transfer.x = 0;
            if (++g_transfer.y >= rrh) g_transfer.active = false;
        }
        return v;
    };
    if (bits == 24) {
        for (std::size_t i = 0; i < bytes && (g_transfer.active || g_carried); ++i) {
            if (!g_carried) {
                const std::uint32_t v = next();
                g_carry[0] = static_cast<std::uint8_t>(v);
                g_carry[1] = static_cast<std::uint8_t>(v >> 8);
                g_carry[2] = static_cast<std::uint8_t>(v >> 16);
                g_carried = 3;
            }
            data[i] = g_carry[3 - g_carried--];
        }
        return;
    }
    for (std::size_t bit = 0; bit + bits <= bytes * 8 && g_transfer.active; bit += bits) {
        const std::uint32_t v = next();
        if (bits == 4) data[bit / 8] |= static_cast<std::uint8_t>((v & 15u) << (bit & 4));
        else std::memcpy(data + bit / 8, &v, bits / 8);
    }
}

std::uint64_t read_privileged(std::uint32_t address) {
    switch (address & 0x1FFFu) {
    case 0x1000: return g_csr | (g_odd_field ? 0x2000u : 0u);
    case 0x1010: return g_imr;
    case 0x1040: return g_busdir;
    case 0x1080: return g_siglblid;
    default: return 0;
    }
}

void write_privileged(std::uint32_t address, std::uint64_t value) {
    switch (address & 0x1FF0u) {
    case 0x0000:
        if (trace_display() && value != g_pmode) std::fprintf(stderr, "[gs] pmode 0x%llX\n", static_cast<unsigned long long>(value));
        g_pmode = value;
        break;
    case 0x0010: g_smode1 = value; break;
    case 0x0020: g_smode2 = value; break;
    case 0x0030: g_srfsh = value; break;
    case 0x0040: g_synch1 = value; break;
    case 0x0050: g_synch2 = value; break;
    case 0x0060: g_syncv = value; break;
    case 0x0070:
        if (trace_display() && value != g_dispfb[0]) std::fprintf(stderr, "[gs] dispfb1 0x%llX\n", static_cast<unsigned long long>(value));
        g_dispfb[0] = value;
        break;
    case 0x0080: g_display[0] = value; break;
    case 0x0090:
        if (trace_display() && value != g_dispfb[1]) std::fprintf(stderr, "[gs] dispfb2 0x%llX\n", static_cast<unsigned long long>(value));
        g_dispfb[1] = value;
        break;
    case 0x00A0: g_display[1] = value; break;
    case 0x00B0: g_extbuf = value; break;
    case 0x00C0: g_extdata = value; break;
    case 0x00D0: g_extwrite = value; break;
    case 0x00E0: g_bgcolor = value; break;
    case 0x1000:
        g_csr &= ~(value & 0x1Fu);
        if (value & 0x200u) {
            g_csr = 0x551B4000ull;
            if (queue::active()) queue::post(queue::kVertexReset, 0, 0, nullptr, 0);
            else g_state.count = 0;
        }
        break;
    case 0x1010:
        g_imr = value;
        if (g_csr & ~(g_imr >> 8) & 0x1Fu) hw::intc_raise(0);
        break;
    case 0x1040: g_busdir = value; break;
    case 0x1080: g_siglblid = value; break;
    default: break;
    }
}

void vblank(bool start) {
    if (start) {
        g_odd_field = !g_odd_field;
        raise(8);
        if (queue::active()) queue::post(queue::kVblank, 0, 0, nullptr, 0);
#ifdef PS2_GPU
        else if (gpu_on()) gpu::flush();
#endif
    }
}

bool interlaced_odd_field() { return g_odd_field; }

bool attach_window(int kind, void *display, std::uint64_t window, void *surface, const char *selector, std::uint32_t width, std::uint32_t height) {
#ifdef PS2_GPU
    if (gpu::mode() != gpu::Mode::primary) return false;
    const gpu::SurfaceTarget target{static_cast<gpu::SurfaceTarget::Kind>(kind), display, window, surface, selector, width, height};
    return gpu::attach_surface(target);
#else
    (void)kind, (void)display, (void)window, (void)surface, (void)selector, (void)width, (void)height;
    return false;
#endif
}

void resize_window(std::uint32_t width, std::uint32_t height) {
#ifdef PS2_GPU
    gpu::resize_surface(width, height);
#else
    (void)width, (void)height;
#endif
}

double gpu_busy_ms() {
#ifdef PS2_GPU
    return gpu::busy_ms();
#else
    return 0.0;
#endif
}

DisplayRegs display_regs() { return DisplayRegs{g_pmode, g_smode2, {g_dispfb[0], g_dispfb[1]}, {g_display[0], g_display[1]}}; }

bool present() { return present(display_regs()); }

bool present(const DisplayRegs &r) {
#ifdef PS2_GPU
    if (!gpu::has_surface()) return false;
    const int circuit = (r.pmode & 2u) ? 1 : (r.pmode & 1u) ? 0 : -1;
    if (circuit < 0) return true;
    const std::uint64_t fb = r.dispfb[circuit], disp = r.display[circuit];
    const std::uint32_t magh = (static_cast<std::uint32_t>(disp >> 23) & 0xFu) + 1, magv = (static_cast<std::uint32_t>(disp >> 27) & 3u) + 1;
    const std::uint32_t width = ((static_cast<std::uint32_t>(disp >> 32) & 0xFFFu) + 1) / magh, height = ((static_cast<std::uint32_t>(disp >> 44) & 0x7FFu) + 1) / magv;
    if (!width || !height || width > 2048 || height > 2048) return true;
    const gpu::DisplaySource src{(static_cast<std::uint32_t>(fb) & 0x1FFu) * 32, static_cast<std::uint32_t>(fb >> 9) & 0x3Fu, static_cast<std::uint32_t>(fb >> 15) & 0x1Fu,
                                 static_cast<std::uint32_t>(fb >> 32) & 0x7FFu, static_cast<std::uint32_t>(fb >> 43) & 0x7FFu, width, height, (r.smode2 & 3u) == 3u};
    return gpu::present(src);
#else
    return false;
#endif
}

bool display(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height) { return display(display_regs(), rgba, width, height); }

bool display(const DisplayRegs &r, std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height) {
    const int circuit = (r.pmode & 2u) ? 1 : (r.pmode & 1u) ? 0 : -1;
    if (circuit < 0) return false;
    const std::uint64_t fb = r.dispfb[circuit], disp = r.display[circuit];
    const std::uint32_t fbp = static_cast<std::uint32_t>(fb) & 0x1FFu, fbw = static_cast<std::uint32_t>(fb >> 9) & 0x3Fu;
    const std::uint32_t psm = static_cast<std::uint32_t>(fb >> 15) & 0x1Fu;
    const std::uint32_t dbx = static_cast<std::uint32_t>(fb >> 32) & 0x7FFu, dby = static_cast<std::uint32_t>(fb >> 43) & 0x7FFu;
    const std::uint32_t magh = (static_cast<std::uint32_t>(disp >> 23) & 0xFu) + 1, magv = (static_cast<std::uint32_t>(disp >> 27) & 3u) + 1;
    const std::uint32_t dw = (static_cast<std::uint32_t>(disp >> 32) & 0xFFFu) + 1, dh = (static_cast<std::uint32_t>(disp >> 44) & 0x7FFu) + 1;
    width = dw / magh;
    height = dh / magv;
    if (!width || !height || width > 2048 || height > 2048) return false;
    const bool halved = (r.smode2 & 3u) == 3u;
#ifdef PS2_GPU
    if (gpu_on()) {
        const gpu::DisplaySource src{fbp * 32, fbw, psm, dbx, dby, width, height, halved};
        if (!cpu_raster()) return gpu::display(src, rgba, width, height);
        static const bool compare = std::getenv("PS2_GS_COMPARE") != nullptr;
        std::uint32_t gw = 0, gh = 0;
        if (compare) g_gpu_frame.clear(), gpu::display(src, g_gpu_frame, gw, gh);
    }
#endif
    rgba.resize(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint32_t c = read_pixel(psm, fbp * 32, fbw, dbx + x, dby + (halved ? y / 2 : y));
            if (psm == PSMCT16 || psm == PSMCT16S)
                c = ((c & 0x1Fu) << 3) | ((c & 0x3E0u) << 6) | ((c & 0x7C00u) << 9) | 0xFF000000u;
            rgba[static_cast<std::size_t>(y) * width + x] = c | 0xFF000000u;
        }
#ifdef PS2_GPU
    if (!g_gpu_frame.empty() && g_gpu_frame.size() == rgba.size()) {
        std::size_t differ = 0;
        int worst = 0;
        for (std::size_t i = 0; i < rgba.size(); ++i) {
            int delta = 0;
            for (int ch = 0; ch < 24; ch += 8)
                delta = std::max(delta, std::abs(static_cast<int>((rgba[i] >> ch) & 0xFFu) - static_cast<int>((g_gpu_frame[i] >> ch) & 0xFFu)));
            if (delta > 2) ++differ;
            worst = std::max(worst, delta);
        }
        std::fprintf(stderr, "[gs] gpu differs in %zu of %zu pixels (max %d)\n", differ, rgba.size(), worst);
        if (std::strcmp(std::getenv("PS2_GS_COMPARE"), "show") == 0) rgba.swap(g_gpu_frame);
    }
#endif
    return true;
}

bool copy_frame(std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height) { return copy_frame(display_regs(), rgba, width, height); }

bool copy_frame(const DisplayRegs &r, std::vector<std::uint32_t> &rgba, std::uint32_t &width, std::uint32_t &height) {
    static std::vector<std::uint32_t> previous;
    bool got = false;
#ifdef PS2_GPU
    if (gpu_on() && !cpu_raster()) {
        const int circuit = (r.pmode & 2u) ? 1 : (r.pmode & 1u) ? 0 : -1;
        if (circuit < 0) return false;
        const std::uint64_t fb = r.dispfb[circuit], disp = r.display[circuit];
        const std::uint32_t magh = (static_cast<std::uint32_t>(disp >> 23) & 0xFu) + 1, magv = (static_cast<std::uint32_t>(disp >> 27) & 3u) + 1;
        const std::uint32_t w = ((static_cast<std::uint32_t>(disp >> 32) & 0xFFFu) + 1) / magh, h = ((static_cast<std::uint32_t>(disp >> 44) & 0x7FFu) + 1) / magv;
        if (!w || !h || w > 2048 || h > 2048) return false;
        const gpu::DisplaySource src{(static_cast<std::uint32_t>(fb) & 0x1FFu) * 32, static_cast<std::uint32_t>(fb >> 9) & 0x3Fu, static_cast<std::uint32_t>(fb >> 15) & 0x1Fu,
                                     static_cast<std::uint32_t>(fb >> 32) & 0x7FFu, static_cast<std::uint32_t>(fb >> 43) & 0x7FFu, w, h, (r.smode2 & 3u) == 3u};
        got = gpu::display_async(src, rgba, width, height);
    } else
#endif
        got = display(r, rgba, width, height);
    if (!got) return false;
    if (previous.size() != rgba.size()) {
        previous = rgba;
        return true;
    }
    static const bool weave_off = std::getenv("PS2_NO_DEINTERLACE") != nullptr;
    const bool blend = (r.smode2 & 3u) == 3u && !weave_off;
    for (std::size_t i = 0; i < rgba.size(); ++i) {
        const std::uint32_t c = rgba[i];
        if (blend) rgba[i] = (c & previous[i]) + (((c ^ previous[i]) & 0xFEFEFEFEu) >> 1);
        previous[i] = c;
    }
    return true;
}

std::atomic<std::uint32_t> g_presented{0};
std::uint32_t g_posted_presents = 0;

void flush_and_wait() {
#ifdef PS2_GPU
    if (gpu_on()) gpu::wait_idle();
#endif
}

void start_thread() { queue::render().start(32u << 20); }

void post_present() {
    const DisplayRegs regs = display_regs();
    queue::post(queue::kPresent, 0, 0, &regs, sizeof regs);
    ++g_posted_presents;
    for (std::uint32_t done = g_presented.load(); g_posted_presents - done > 2; done = g_presented.load()) queue::wait(g_presented, done);
}

Rendered render_pending(const std::function<void(const DisplayRegs &)> &on_present, double budget_ms) {
    queue::role() = queue::Role::Render;
    queue::Header h;
    const std::uint8_t *payload = nullptr;
    const auto start = std::chrono::steady_clock::now();
    for (std::uint32_t n = 1; queue::render().peek(h, payload); ++n) {
        if (!(n & 7u) && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() > budget_ms) return Rendered::busy;
        bool presented = false;
        switch (h.type) {
        case queue::kGif: gif::render(static_cast<int>(h.arg0), payload, h.arg1); break;
        case queue::kGifReset: gif::render_reset(); break;
        case queue::kReset: render_reset(); break;
        case queue::kVertexReset: g_state.count = 0; break;
        case queue::kVblank:
#ifdef PS2_GPU
            if (gpu_on()) gpu::flush();
#endif
            break;
        case queue::kPresent: {
            DisplayRegs regs;
            std::memcpy(&regs, payload, sizeof regs);
            on_present(regs);
            presented = true;
            break;
        }
        case queue::kState: serve_state(payload); break;
        case queue::kDownload: {
            std::uintptr_t target;
            std::memcpy(&target, payload, sizeof target);
            exact_download();
            render_read_image(reinterpret_cast<std::uint8_t *>(target), h.arg0);
            g_downloads.fetch_add(1);
            queue::notify(g_downloads);
            break;
        }
        case queue::kDownloadAsync: {
            DownloadKey key;
            std::memcpy(&key, payload, sizeof key);
            std::vector<std::uint8_t> fresh(static_cast<std::size_t>(h.arg0) * 16);
            render_read_image(fresh.data(), h.arg0);
            if (key.buf == g_state.bitbltbuf && key.pos == g_state.trxpos && key.reg == g_state.trxreg) remember(key, fresh.data(), fresh.size());
            else forget(key);
            break;
        }
        default: break;
        }
        queue::render().pop(h);
        if (presented) {
            g_presented.fetch_add(1);
            queue::notify(g_presented);
            return Rendered::presented;
        }
    }
    return Rendered::idle;
}

std::uint64_t primitives_drawn() { return g_primitives; }

void dump_region(const char *spec) {
#ifdef PS2_GPU
    if (gpu_on() && !cpu_raster()) gpu::download(g_memory);
#endif
    unsigned bp = 0, bw = 0, psm = 0, w = 0, h = 0;
    char path[256] = {};
    if (std::sscanf(spec, "%x,%u,%u,%u,%u,%255s", &bp, &bw, &psm, &w, &h, path) != 6) return;
    std::FILE *f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            const std::uint32_t c = read_pixel(psm, bp, bw, x, y);
            const std::uint8_t px[3] = {static_cast<std::uint8_t>(c), static_cast<std::uint8_t>(c >> 8), static_cast<std::uint8_t>(c >> 16)};
            std::fwrite(px, 1, 3, f);
        }
    std::fclose(f);
}

void stats() {
    if (const char *spec = std::getenv("PS2_DUMP_VRAM")) dump_region(spec);
#ifdef PS2_GPU
    gpu::stats();
#endif
    std::fprintf(stderr, "[gs] %llu primitives drawn, register writes:", static_cast<unsigned long long>(g_primitives));
    for (int r = 0; r < 128; ++r)
        if (g_writes[r]) std::fprintf(stderr, " %02X:%llu", r, static_cast<unsigned long long>(g_writes[r]));
    std::fprintf(stderr, "\n");
}

}

namespace ps2::gs {

namespace {

std::atomic<std::uint32_t> g_state_done{0};
std::string g_state_error;

void render_state(ps2::state::Stream &s) {
    flush_and_wait();
    s.pod(g_state);
    ++g_setup_generation;
    s.pod(g_pmode);
    s.pod(g_smode1);
    s.pod(g_smode2);
    s.pod(g_srfsh);
    s.pod(g_synch1);
    s.pod(g_synch2);
    s.pod(g_syncv);
    s.pod(g_dispfb);
    s.pod(g_display);
    s.pod(g_extbuf);
    s.pod(g_extdata);
    s.pod(g_extwrite);
    s.pod(g_bgcolor);
    s.pod(g_csr);
    s.pod(g_imr);
    s.pod(g_busdir);
    s.pod(g_siglblid);
    s.pod(g_primitives);
    s.pod(g_odd_field);
    s.pod(g_transfer);
    s.pod(g_carry);
    s.size(g_carried);
    std::vector<std::uint32_t> vram, clut;
    std::uint32_t scale = 1;
#ifdef PS2_GPU
    if (s.saving() && gpu_on()) {
        gpu::download(g_memory);
        gpu::read_state(vram, scale, clut);
    }
#endif
    s.bytes(g_memory, kMemorySize);
    s.pod(scale);
    s.vec(vram);
    s.vec(clut);
#ifdef PS2_GPU
    if (s.loading() && gpu_on()) {
        if (vram.empty()) {
            vram.resize(kMemorySize / 4);
            std::memcpy(vram.data(), g_memory, kMemorySize);
            clut.assign(g_state.clut, g_state.clut + 512);
            scale = 1;
        }
        gpu::write_state(vram, scale, clut);
    }
#endif
}

}

void serve_state(const void *payload) {
    std::uintptr_t target;
    std::memcpy(&target, payload, sizeof target);
    try {
        render_state(*reinterpret_cast<ps2::state::Stream *>(target));
    } catch (const std::exception &e) {
        g_state_error = e.what();
    }
    g_state_done.fetch_add(1);
    queue::notify(g_state_done);
}

void state(ps2::state::Stream &s) {
    s.tag("gs");
    if (!queue::active()) {
        render_state(s);
        return;
    }
    g_state_error.clear();
    const std::uint32_t seen = g_state_done.load();
    const std::uintptr_t target = reinterpret_cast<std::uintptr_t>(&s);
    queue::post(queue::kState, 0, 0, &target, sizeof target);
    while (g_state_done.load() == seen) queue::wait(g_state_done, seen);
    if (!g_state_error.empty()) throw std::runtime_error(g_state_error);
}

}
