#include "kernel.hpp"
#include "ps2/generated.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <cstdint>
#include <cstdio>

namespace {
constexpr std::uint32_t kMainEntry = 0x10000;
constexpr std::uint32_t kWorkerEntry = 0x12000;
int failures = 0;

void check(bool ok, const char *message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void start() {
    ps2::kernel::reset();
    ps2::kernel::start_main_thread(kMainEntry, 0, 0, 0);
    ps2::g_cpu.pc = kMainEntry + 4;
}

void syscall(std::uint32_t number) {
    ps2::g_cpu.r[3].w[0] = number;
    ps2::kernel::handle_syscall(ps2::g_cpu, kMainEntry);
}

void check_idle(const char *message) {
    check(ps2::kernel::current_thread() == 0, "no thread is running after the last runnable thread exits");
    check((ps2::g_cpu.pc & ~ps2::kUnwind) == ps2::kIdle, message);
    check((ps2::g_cpu.pc & ps2::kUnwind) != 0, "idle transition requests unwind of the translated call stack");
    ps2::kernel::service(ps2::g_cpu);
    check(ps2::g_cpu.pc == ps2::kIdle, "kernel service clears unwind and remains idle with no ready thread");
}

int create_worker() {
    constexpr std::uint32_t descriptor = 0x3000;
    for (unsigned i = 0; i < 36; i += 4) ps2::st32(descriptor + i, 0);
    ps2::st32(descriptor + 4, kWorkerEntry);
    ps2::st32(descriptor + 8, 0x20000);
    ps2::st32(descriptor + 12, 0x1000);
    ps2::st32(descriptor + 20, 16); // Lower priority than the main thread (0).
    ps2::g_cpu.r[4].w[0] = descriptor;
    syscall(0x20); // CreateThread
    const int id = static_cast<int>(ps2::g_cpu.r[2].w[0]);
    check(id > 1, "a dormant worker thread is created");
    return id;
}

void exit_without_delete() {
    start();
    syscall(0x23); // ExitThread: current thread remains in the table, dormant.
    check_idle("ExitThread of the only runnable thread enters idle");
}

void delete_last_thread() {
    start();
    syscall(0x24); // ExitDeleteThread: both current() and best_ready() become null.
    check_idle("ExitDeleteThread enters idle instead of continuing deleted guest code");
}

void delete_with_dormant_worker() {
    start();
    create_worker();
    syscall(0x24);
    check_idle("a dormant thread must not prevent ExitDeleteThread from entering idle");
}

void delete_with_ready_worker() {
    start();
    const int id = create_worker();
    ps2::g_cpu.r[4].w[0] = static_cast<std::uint32_t>(id);
    ps2::g_cpu.r[5].w[0] = 0x1234;
    syscall(0x22); // StartThread: ready, but it should not preempt main's priority.
    check(ps2::kernel::current_thread() == 1, "starting a lower-priority worker keeps the main thread running");
    syscall(0x24);
    check(ps2::kernel::current_thread() == id, "deleting main schedules the ready worker");
    check((ps2::g_cpu.pc & ~ps2::kUnwind) == kWorkerEntry, "ready worker's entry PC is restored");
    check((ps2::g_cpu.pc & ps2::kUnwind) != 0, "switching to the worker requests unwind");
    check(ps2::g_cpu.r[4].w[0] == 0x1234, "ready worker's argument is restored");
}
}

int main() {
    exit_without_delete();
    delete_last_thread();
    delete_with_dormant_worker();
    delete_with_ready_worker();
    if (failures) return 1;
    std::puts("ExitDeleteThread idle regression PASS");
    return 0;
}
