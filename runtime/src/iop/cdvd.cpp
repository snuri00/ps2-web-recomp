#include "kernel.hpp"

#include "ps2/disc.hpp"

#include "ps2/state.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>
#include <memory>

namespace ps2::iop {

namespace kernel {

namespace {

std::uint32_t arg(const Cpu &c, int i) { return i < 4 ? c.r[4 + i] : read32(c.r[29] + 16u + static_cast<std::uint32_t>(i - 4) * 4u); }

void def(const char *library, std::uint32_t index, const char *name, std::function<void(Cpu &)> body) {
    register_hle(library, index, [library, name, body = std::move(body)](Cpu &c) {
        trace_hle(library, name, c);
        body(c);
    });
}

struct CdState {
    bool busy = false;
    std::uint32_t callback = 0;
    std::uint32_t error = 0;
    std::vector<int> sync_waiters;
} g_cd;

std::uint8_t bcd(int v) { return static_cast<std::uint8_t>(((v / 10) << 4) | (v % 10)); }

void finish_read() {
    g_cd.busy = false;
    for (int id : g_cd.sync_waiters)
        if (Thread *t = thread(id)) wake(*t, 0);
    if (!g_cd.sync_waiters.empty()) reschedule();
    g_cd.sync_waiters.clear();
    if (g_cd.callback) call_guest(g_cd.callback, 1);
}

std::uint64_t read_cycles(std::uint32_t sectors) {
    return usec_to_cycles(400) + static_cast<std::uint64_t>(sectors) * usec_to_cycles(250);
}

bool start_read(std::uint32_t lsn, std::uint32_t sectors, std::uint32_t buffer, std::uint32_t mode, bool ee_memory);

}

std::vector<std::uint8_t> read_disc_file(const std::string &path) {
    const auto entry = disc::find(path);
    if (!entry || entry->directory) return {};
    std::vector<std::uint8_t> data((entry->size + disc::kSector - 1) / disc::kSector * disc::kSector);
    disc::read(entry->sector, static_cast<std::uint32_t>(data.size() / disc::kSector), data.data());
    data.resize(entry->size);
    return data;
}

namespace {

bool start_read(std::uint32_t lsn, std::uint32_t sectors, std::uint32_t buffer, std::uint32_t, bool) {
    if (g_cd.busy) return false;
    g_cd.busy = true;
    g_cd.error = 0;
    disc::ready(lsn, sectors);
    schedule(now() + read_cycles(sectors), Job{kJobCdRead, lsn, sectors, buffer});
    return true;
}

}

void install_cdvd() {
    g_cd = CdState{};
    register_job(kJobCdRead, [](const Job &j) {
        if (!disc::ready(j.a, j.b)) {
            schedule(now() + usec_to_cycles(1000), j);
            return;
        }
        std::vector<std::uint8_t> data(static_cast<std::size_t>(j.b) * disc::kSector);
        disc::read(j.a, j.b, data.data());
        for (std::size_t i = 0; i < data.size(); ++i) write8(j.c + static_cast<std::uint32_t>(i), data[i]);
        finish_read();
    });
    register_hle_library("cdvdman", 0x0101);
    def("cdvdman", 4, "sceCdInit", [](Cpu &c) { c.r[2] = 1; });
    def("cdvdman", 5, "sceCdStandby", [](Cpu &c) { c.r[2] = 1; });
    def("cdvdman", 6, "sceCdRead", [](Cpu &c) {
        if (g_trace_hle) std::fprintf(stderr, "[cdvd] read lsn %u count %u to 0x%08X\n", arg(c, 0), arg(c, 1), arg(c, 2));
        c.r[2] = start_read(arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), false) ? 1u : 0u;
    });
    def("cdvdman", 7, "sceCdSeek", [](Cpu &c) { c.r[2] = 1; });
    def("cdvdman", 8, "sceCdGetError", [](Cpu &c) { c.r[2] = g_cd.error; });
    def("cdvdman", 9, "sceCdGetReadPos", [](Cpu &c) { c.r[2] = 0; });
    def("cdvdman", 10, "sceCdSearchFile", [](Cpu &c) {
        const std::string name = ram_string(arg(c, 1), 256);
        const auto entry = disc::find(name);
        if (!entry) { c.r[2] = 0; return; }
        const std::uint32_t p = arg(c, 0);
        write32(p, entry->sector);
        write32(p + 4, entry->size);
        const std::string base = name.substr(name.find_last_of("\\/") == std::string::npos ? 0 : name.find_last_of("\\/") + 1);
        for (std::uint32_t i = 0; i < 16; ++i) write8(p + 8 + i, i < base.size() ? static_cast<std::uint8_t>(base[i]) : 0);
        c.r[2] = 1;
    });
    def("cdvdman", 11, "sceCdSync", [](Cpu &c) {
        if (arg(c, 0) == 1 || arg(c, 0) == 17) { c.r[2] = g_cd.busy ? 1u : 0u; return; }
        c.r[2] = 0;
        if (!g_cd.busy || !current()) return;
        g_cd.sync_waiters.push_back(current()->id);
        block(c, kWaitHle, 0);
    });
    def("cdvdman", 12, "sceCdGetDiskType", [](Cpu &c) { c.r[2] = 0x14; });
    def("cdvdman", 13, "sceCdDiskReady", [](Cpu &c) { c.r[2] = 2; });
    def("cdvdman", 14, "sceCdTrayReq", [](Cpu &c) { if (arg(c, 1)) write32(arg(c, 1), 0); c.r[2] = 1; });
    def("cdvdman", 15, "sceCdStop", [](Cpu &c) { c.r[2] = 1; });
    def("cdvdman", 16, "sceCdGetToc", [](Cpu &c) { c.r[2] = 1; });
    def("cdvdman", 22, "sceCdRI", [](Cpu &c) {
        for (std::uint32_t i = 0; i < 8; ++i) write8(arg(c, 0) + i, 0);
        if (arg(c, 1)) write32(arg(c, 1), 0);
        c.r[2] = 1;
    });
    def("cdvdman", 24, "sceCdReadClock", [](Cpu &c) {
        const std::time_t t = std::time(nullptr);
        const std::tm *tm = std::gmtime(&t);
        const std::uint32_t p = arg(c, 0);
        write8(p + 0, 0);
        write8(p + 1, bcd(tm->tm_sec));
        write8(p + 2, bcd(tm->tm_min));
        write8(p + 3, bcd((tm->tm_hour + 9) % 24));
        write8(p + 4, 0);
        write8(p + 5, bcd(tm->tm_mday));
        write8(p + 6, bcd(tm->tm_mon + 1));
        write8(p + 7, bcd(tm->tm_year % 100));
        c.r[2] = 1;
    });
    def("cdvdman", 25, "sceCdDecSet", [](Cpu &c) { c.r[2] = 1; });
    def("cdvdman", 28, "sceCdStatus", [](Cpu &c) { c.r[2] = g_cd.busy ? 0x06u : 0x0Au; });
    def("cdvdman", 37, "sceCdCallback", [](Cpu &c) { c.r[2] = g_cd.callback; g_cd.callback = arg(c, 0); });
    def("cdvdman", 38, "sceCdPause", [](Cpu &c) { c.r[2] = 1; });
    def("cdvdman", 39, "sceCdBreak", [](Cpu &c) { c.r[2] = 1; });
    def("cdvdman", 44, "sceCdGetSubQ", [](Cpu &c) { c.r[2] = 0; });
    def("cdvdman", 56, "sceCdStInit", [](Cpu &c) { c.r[2] = 1; });
    def("cdvdman", 57, "sceCdStRead", [](Cpu &c) { c.r[2] = 1; });
    def("cdvdman", 58, "sceCdStSeek", [](Cpu &c) { c.r[2] = 1; });
    def("cdvdman", 59, "sceCdStStart", [](Cpu &c) { c.r[2] = 1; });
    def("cdvdman", 60, "sceCdStStat", [](Cpu &c) { c.r[2] = 0; });
    def("cdvdman", 61, "sceCdStStop", [](Cpu &c) { c.r[2] = 1; });
    def("cdvdman", 62, "sceCdStPause", [](Cpu &c) { c.r[2] = 1; });
    def("cdvdman", 63, "sceCdStResume", [](Cpu &c) { c.r[2] = 1; });
    def("cdvdman", 74, "sceCdPowerOff", [](Cpu &c) { c.r[2] = 1; });
    def("cdvdman", 75, "sceCdMmode", [](Cpu &c) { c.r[2] = 1; });
    def("cdvdman", 78, "sceCdPOffCallback", [](Cpu &c) { c.r[2] = 0; });
    def("cdvdman", 83, "sceCdReadDvdDualInfo", [](Cpu &c) {
        write32(arg(c, 0), disc::layer1_start() ? 1u : 0u);
        write32(arg(c, 1), disc::layer1_start());
        c.r[2] = 1;
    });
    def("cdvdman", 84, "sceCdLayerSearchFile", [](Cpu &c) {
        const std::string name = ram_string(arg(c, 1), 256);
        const auto entry = disc::find(name);
        if (!entry) { c.r[2] = 0; return; }
        write32(arg(c, 0), entry->sector);
        write32(arg(c, 0) + 4, entry->size);
        c.r[2] = 1;
    });
}

bool cd_busy() { return g_cd.busy; }

}

std::int32_t load_module(const std::string &path, const std::string &args, std::function<void(int, int)> done) {
    std::vector<std::uint8_t> data;
    std::string lower = path;
    for (char &ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lower.rfind("cdrom", 0) == 0) data = kernel::read_disc_file(path);
    if (data.empty() && lower.rfind("rom0:", 0) != 0) {
        std::fprintf(stderr, "[iop] cannot read module %s\n", path.c_str());
    }
    return load_module_buffer(data, path, args, std::move(done));
}

}

namespace ps2::iop::kernel {

void cdvd_state(ps2::state::Stream &s) {
    s.tag("cdvd");
    s.pod(g_cd.busy);
    s.pod(g_cd.callback);
    s.pod(g_cd.error);
    s.vec(g_cd.sync_waiters);
}

}
