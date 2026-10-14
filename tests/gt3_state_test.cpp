#include "hw.hpp"
#include "iop/hw.hpp"
#include "iop/iop.hpp"
#include "kernel.hpp"
#include "ps2/memory.hpp"
#include "ps2/state.hpp"
#include "ps2/system.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

int main() {
    using namespace ps2;
    const auto dir = std::filesystem::temp_directory_path() /
        ("ps2-state-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!std::filesystem::create_directory(dir)) return 1;
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    } cleanup{dir};
    int failures = 0;
    const auto check = [&](bool ok, const char *message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
    };
    state::Writer empty;
    empty.bytes(nullptr, 0);
    check(empty.out.empty(), "zero-length write accepts a null pointer");
    const std::uint8_t sentinel = 0xA5;
    state::Reader zero(&sentinel, 1);
    zero.bytes(nullptr, 0);
    std::uint8_t value = 0;
    zero.pod(value);
    check(value == sentinel, "zero-length read accepts null and does not advance");
    hw::reset();
    iop::reset();
    kernel::reset();
    kernel::start_main_thread(0x10000, 0, 0, 0);
    hw::intc_set_mask(2, true);
    hw::intc_raise(2);
    hw::intc_ack(2);
    iop::hw::write(0x1F808414, 0x400004A5, 4);
    const auto path = (dir / "synthetic.state").string();
    if (!save_state(path)) return 1;
    std::ifstream input(path, std::ios::binary);
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
    if (bytes.size() < 12) return 1;
    std::uint32_t version;
    std::memcpy(&version, bytes.data() + 8, sizeof version);
    check(version == 3, "new snapshot layout uses version 3");
    hw::reset();
    iop::hw::reset();
    g_cpu.pc = 0x20000;
    check(load_state(bytes.data(), bytes.size()), "new snapshot loads successfully");
    check(g_cpu.pc == 0x10000, "snapshot restores the CPU");
    check((hw::read32(0x1000F000) & 4) != 0, "snapshot restores the guest-visible delivered latch");
    check(hw::intc_pending() == 0, "snapshot does not redeliver that interrupt");
    iop::hw::write(0x1F808414, 0x84000000, 4);
    check((iop::hw::read(0x1F808414, 4) & 0xFFFF) == 0x04A5, "snapshot restores the PHY register");
    // Test the version gate, not a migration: no old-layout body is embedded.
    version = 2;
    std::memcpy(bytes.data() + 8, &version, sizeof version);
    g_cpu.pc = 0x30000;
    st32(0x9000, 0x12345678);
    check(!load_state(bytes.data(), bytes.size()), "old version is explicitly rejected");
    check(g_cpu.pc == 0x30000 && ld32(0x9000) == 0x12345678,
          "version rejection happens before CPU or RAM mutation");
    if (failures) return 1;
    std::puts("GT3 snapshot schema regression PASS");
    return 0;
}
