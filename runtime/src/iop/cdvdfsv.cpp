#include "kernel.hpp"
#include "sif.hpp"

#include "ps2/disc.hpp"
#include "ps2/memory.hpp"

#include "ps2/state.hpp"

#include <cstdio>
#include <ctime>
#include <vector>

namespace ps2::iop::kernel {

namespace {

std::uint32_t ee32(std::uint32_t a) { return load<std::uint32_t>(ps2::g_ram + (a & (ps2::kRamSize - 1))); }
void set_ee32(std::uint32_t a, std::uint32_t v) { store<std::uint32_t>(ps2::g_ram + (a & (ps2::kRamSize - 1)), v); }

std::uint8_t bcd(int v) { return static_cast<std::uint8_t>(((v / 10) << 4) | (v % 10)); }

bool g_streaming = false;
std::uint32_t g_stream_pos = 0;

std::uint64_t delay(std::uint32_t sectors) { return usec_to_cycles(400) + static_cast<std::uint64_t>(sectors) * usec_to_cycles(200); }

void read_to_ee(std::uint32_t sector, std::uint32_t count, std::uint32_t dst) {
    std::vector<std::uint8_t> data(static_cast<std::size_t>(count) * disc::kSector);
    disc::read(sector, count, data.data());
    const std::uint32_t at = dst & (ps2::kRamSize - 1);
    std::memcpy(ps2::g_ram + at, data.data(), std::min<std::size_t>(data.size(), ps2::kRamSize - at));
    ps2::watch_span(at, static_cast<std::uint32_t>(std::min<std::size_t>(data.size(), ps2::kRamSize - at)), "cdvd:read");
}

}

void install_cdvdfsv() {
    g_streaming = false;
    g_stream_pos = 0;
    register_job(kJobFsvRead, [](const Job &j) {
        if (g_trace_hle) std::fprintf(stderr, "[cdvdfsv] stream write lsn %u count %u to 0x%08X\n", j.a, j.b, j.c);
        if (!disc::ready(j.a, j.b)) {
            schedule(now() + usec_to_cycles(1000), j);
            return;
        }
        read_to_ee(j.a, j.b, j.c);
        sif::complete_call(0x80000595u);
    });
    register_job(kJobFsvComplete, [](const Job &) { sif::complete_call(0x80000595u); });
    register_job(kJobFsvIopRead, [](const Job &j) {
        if (!disc::ready(j.a, j.b)) {
            schedule(now() + usec_to_cycles(1000), j);
            return;
        }
        std::vector<std::uint8_t> data(static_cast<std::size_t>(j.b) * disc::kSector);
        disc::read(j.a, j.b, data.data());
        for (std::size_t i = 0; i < data.size(); ++i) write8(j.c + static_cast<std::uint32_t>(i), data[i]);
        sif::complete_call(0x80000595u);
    });
    sif::register_hle_server(0x80000592u, [](std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t ret, std::uint32_t ret_size) {
        if (ret_size >= 16) set_ee32(ret + 12, 0xFF);
        return true;
    });
    sif::register_hle_server(0x80000593u, [](std::uint32_t method, std::uint32_t args, std::uint32_t, std::uint32_t ret, std::uint32_t ret_size) {
        switch (method) {
        case 0x01: {
            const std::time_t t = std::time(nullptr);
            const std::tm *tm = std::gmtime(&t);
            set_ee32(ret, 1);
            std::uint8_t *clock = ps2::g_ram + ret + 4;
            clock[0] = 0;
            clock[1] = bcd(tm->tm_sec);
            clock[2] = bcd(tm->tm_min);
            clock[3] = bcd((tm->tm_hour + 9) % 24);
            clock[4] = 0;
            clock[5] = bcd(tm->tm_mday);
            clock[6] = bcd(tm->tm_mon + 1);
            clock[7] = bcd(tm->tm_year % 100);
            break;
        }
        case 0x03: set_ee32(ret, 0x14); break;
        case 0x04: set_ee32(ret, 0); break;
        case 0x05: set_ee32(ret, 1); set_ee32(ret + 4, 0); break;
        case 0x0C: set_ee32(ret, g_streaming ? 0x12u : 0x0Au); break;
        case 0x16: set_ee32(ret, 1); break;
        case 0x22: set_ee32(ret, 1); break;
        case 0x27:
            set_ee32(ret, 1);
            set_ee32(ret + 4, disc::layer1_start() ? 1u : 0u);
            if (ret_size >= 12) set_ee32(ret + 8, disc::layer1_start());
            break;
        default:
            std::fprintf(stderr, "[cdvdfsv] S command 0x%X\n", method);
            set_ee32(ret, 1);
            break;
        }
        (void)args;
        return true;
    });
    sif::register_hle_server(0x80000595u, [](std::uint32_t method, std::uint32_t args, std::uint32_t, std::uint32_t ret, std::uint32_t ret_size) {
        const std::uint32_t sector = ee32(args), count = ee32(args + 4), dst = ee32(args + 8);
        if (g_trace_hle) std::fprintf(stderr, "[cdvdfsv] N 0x%X sector %u count %u to 0x%08X\n", method, sector, count, dst);
        switch (method) {
        case 0x01:
            if (ret_size >= 4) set_ee32(ret, 0);
            disc::ready(sector, count);
            schedule(now() + delay(count), Job{kJobFsvRead, sector, count, dst});
            return false;
        case 0x04: set_ee32(ret, 1); return true;
        case 0x05: return true;
        case 0x08:
            schedule(now() + delay(0), Job{kJobFsvComplete});
            return false;
        case 0x09: {
            const std::uint32_t cmd = ee32(args + 12);
            if (g_trace_hle) std::fprintf(stderr, "[cdvdfsv] stream cmd %u (pos %u)\n", cmd, g_stream_pos);
            switch (cmd) {
            case 1: g_stream_pos = sector; g_streaming = true; set_ee32(ret, 1); return true;
            case 2: {
                const std::uint32_t pos = g_stream_pos;
                g_stream_pos += count;
                set_ee32(ret, count);
                disc::ready(pos, count);
                schedule(now() + delay(count), Job{kJobFsvRead, pos, count, dst});
                return false;
            }
            case 3: g_streaming = false; set_ee32(ret, 1); return true;
            case 4: case 9: g_stream_pos = sector; set_ee32(ret, 1); return true;
            case 5: set_ee32(ret, 1); return true;
            case 6: set_ee32(ret, sector); return true;
            default: set_ee32(ret, 1); return true;
            }
        }
        case 0x0D:
            if (ret_size >= 4) set_ee32(ret, 0);
            disc::ready(sector, count);
            schedule(now() + delay(count), Job{kJobFsvIopRead, sector, count, dst});
            return false;
        case 0x0E: set_ee32(ret, 2); return true;
        default:
            std::fprintf(stderr, "[cdvdfsv] N command 0x%X\n", method);
            set_ee32(ret, 1);
            return true;
        }
    });
    sif::register_hle_server(0x80000596u, [](std::uint32_t method, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {
        return method != 0x01;
    });
    sif::register_hle_server(0x80000597u, [](std::uint32_t, std::uint32_t args, std::uint32_t size, std::uint32_t ret, std::uint32_t) {
        const std::uint32_t offset = size == 0x124 ? 0x20u : 0x24u;
        const std::string path(reinterpret_cast<const char *>(ps2::g_ram + (args & (ps2::kRamSize - 1)) + offset));
        const auto entry = disc::find(path);
        if (entry) {
            set_ee32(args, entry->sector);
            set_ee32(args + 4, entry->size);
        }
        std::fprintf(stderr, "[cdvdfsv] search %s -> %s\n", path.c_str(), entry ? "found" : "missing");
        set_ee32(ret, entry ? 1u : 0u);
        return true;
    });
    const auto ready = [](std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t ret, std::uint32_t) {
        set_ee32(ret, 2);
        return true;
    };
    sif::register_hle_server(0x8000059Au, ready);
    sif::register_hle_server(0x8000059Cu, ready);
}

}

namespace ps2::iop::kernel {

void cdvdfsv_state(ps2::state::Stream &s) {
    s.tag("cdvdfsv");
    s.pod(g_streaming);
    s.pod(g_stream_pos);
}

}
