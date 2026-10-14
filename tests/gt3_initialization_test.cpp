#include "hw.hpp"
#include "iop/hw.hpp"
#include "ps2/state.hpp"

#include <cstdint>
#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char *message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

void interrupt_latch() {
    using namespace ps2;
    hw::reset();
    hw::intc_set_mask(2, true);
    hw::intc_set_mask(3, true);
    hw::intc_raise(2);
    check(hw::intc_pending() == 4, "vblank is pending");
    hw::intc_ack(2);
    check(hw::intc_pending() == 0, "delivered interrupt is not repeatedly dispatched");
    check(hw::read32(0x1000F000) == 4, "guest can still poll the delivered interrupt");
    hw::intc_raise(3);
    state::Writer saved;
    hw::state(saved);
    saved.tag("done");
    hw::reset();
    state::Reader restored(saved.out.data(), saved.out.size());
    hw::state(restored);
    restored.tag("done");
    check(hw::read32(0x1000F000) == 12, "snapshot retains delivered and pending interrupt latches");
    check(hw::intc_pending() == 8, "restoring a delivered latch does not redeliver its handler");
    hw::write32(0x1000F000, 4);
    check(hw::read32(0x1000F000) == 8, "write-one-to-clear preserves unrelated status");
    check(hw::intc_pending() == 8, "write-one-to-clear preserves unrelated pending delivery");
    hw::intc_raise(2);
    check(hw::intc_pending() == 12, "a new vblank can be delivered again");
}

void ilink_initialization() {
    using namespace ps2;
    iop::hw::reset();
    check(iop::hw::read(0x1F808400, 4) == 0xFFC00001, "local i.Link identification stub");
    check(iop::hw::read(0x1F80847C, 4) == 0x10000001, "local i.Link capability stub");
    iop::hw::write(0x1F808410, 2, 4);
    check((iop::hw::read(0x1F808410, 4) & 10) == 10, "link initialization reports serial clock ready");
    iop::hw::write(0x1F808408, 0x00800000, 4);
    check((iop::hw::read(0x1F808408, 4) & 0x00800000) == 0, "local initialization request completes");
    iop::hw::write(0x1F808414, 0x400004A5, 4);
    check((iop::hw::read(0x1F808414, 4) & 0x40000000) == 0, "PHY write completes");
    iop::hw::write(0x1F808414, 0x84000000, 4);
    check((iop::hw::read(0x1F808414, 4) & 0x8000FFFF) == 0x04A5, "PHY read completes and returns its register");
    iop::hw::set_interrupt_mask(24, true);
    check(iop::hw::next_pending_interrupt() != 24, "masked PHY completion does not raise the device IRQ");
    iop::hw::write(0x1F808424, 0x40000000, 4);
    iop::hw::write(0x1F808414, 0x84000000, 4);
    check(iop::hw::next_pending_interrupt() == 24, "enabled PHY completion raises i.Link IRQ");
    check((iop::hw::read(0x1F808420, 4) & 0x40000000) != 0, "PHY completion sets status");
    iop::hw::write(0x1F808420, 0x40000000, 4);
    check(iop::hw::read(0x1F808420, 4) == 0, "PHY status is write-one-to-clear");
    iop::hw::write(0x1F808414, 0x4000053C, 4);
    state::Writer saved;
    iop::hw::state(saved);
    saved.tag("done");
    iop::hw::reset();
    state::Reader restored(saved.out.data(), saved.out.size());
    iop::hw::state(restored);
    restored.tag("done");
    iop::hw::write(0x1F808414, 0x84000000, 4);
    check((iop::hw::read(0x1F808414, 4) & 0xFFFF) == 0x04A5, "snapshot restores PHY register 4");
    iop::hw::write(0x1F808414, 0x85000000, 4);
    check((iop::hw::read(0x1F808414, 4) & 0xFFFF) == 0x053C, "snapshot restores a distinct PHY register");
    iop::hw::reset();
    iop::hw::write(0x1F808414, 0x84000000, 4);
    check((iop::hw::read(0x1F808414, 4) & 0xFF) == 0, "reset clears PHY register storage");
}
}

int main() {
    interrupt_latch();
    ilink_initialization();
    if (failures) return 1;
    std::puts("GT3 initialization regression PASS");
    return 0;
}
