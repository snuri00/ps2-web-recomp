#pragma once

#include "iop.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ps2::state {
class Stream;
}

namespace ps2::iop::kernel {

void state(ps2::state::Stream &s);
void libs_state(ps2::state::Stream &s);
void cdvd_state(ps2::state::Stream &s);
void cdvdfsv_state(ps2::state::Stream &s);

enum Status { kRun = 1, kReady = 2, kWait = 4, kSuspend = 8, kWaitSuspend = 12, kDormant = 16 };
enum WaitKind { kWaitNone, kWaitSleep, kWaitDelay, kWaitSema, kWaitEvent, kWaitMbx, kWaitFpl, kWaitVblank, kWaitRpc, kWaitHle, kWaitVpl };

struct Thread {
    int id{};
    int status{kDormant};
    int priority{};
    int init_priority{};
    std::uint32_t entry{}, stack{}, stack_size{}, gp{}, attr{}, option{};
    int wakeups{};
    WaitKind wait{kWaitNone};
    int wait_id{};
    std::uint64_t order{};
    Cpu context{};
    std::uint32_t rpc_queue{};
    std::uint32_t rpc_server{};
    std::uint32_t event_bits{}, event_mode{}, event_result{};
    std::uint32_t message_out{};
    bool suspended{};
    std::function<void()> on_exit;
};

void fault(const char *reason);
std::uint32_t fetch_special(std::uint32_t pc);
void syscall(Cpu &c, std::uint32_t pc);
void set_status(Cpu &c, std::uint32_t value);
bool switch_pending();

std::uint64_t now();
std::uint64_t usec_to_cycles(std::uint64_t usec);
void schedule(std::uint64_t at, std::function<void()> action);

enum JobKind : std::uint32_t {
    kJobNone,
    kJobHardTimer,
    kJobAlarm,
    kJobDeleteThread,
    kJobDelayWake,
    kJobSpuTick,
    kJobDmaDone,
    kJobCdRead,
    kJobFsvRead,
    kJobFsvComplete,
    kJobFsvIopRead,
    kJobSifDmaCallback,
    kJobCount
};
struct Job {
    std::uint32_t kind = kJobNone;
    std::uint32_t a = 0, b = 0, c = 0, d = 0;
};
using JobHandler = void (*)(const Job &);
void register_job(std::uint32_t kind, JobHandler handler);
void schedule(std::uint64_t at, const Job &job);

Thread *current();
Thread *thread(int id);
int create_thread(std::uint32_t entry, std::uint32_t stack_size, int priority, std::uint32_t gp, std::uint32_t attr, std::uint32_t option);
void start_thread(int id, std::uint32_t a0, std::uint32_t a1 = 0);
bool delete_thread(int id);
void ready(Thread &t);
void block(Cpu &c, WaitKind kind, int id);
void wake(Thread &t, std::uint32_t result);
void reschedule();
bool in_interrupt();
std::uint32_t call_guest(std::uint32_t address, std::uint32_t a0 = 0, std::uint32_t a1 = 0, std::uint32_t a2 = 0, std::uint32_t a3 = 0);

std::uint32_t alloc(std::uint32_t size, int mode = 0, std::uint32_t at = 0);
void free(std::uint32_t address);
std::uint32_t free_total();
std::uint32_t free_max();
std::uint32_t kernel_alloc(std::uint32_t size);
std::uint32_t hle_stub(const HleFunction &function);

void register_library(const std::string &name, std::uint32_t table);
void register_hle_library(const std::string &name, std::uint32_t version);
bool library_exists(const std::string &name);

struct Module {
    int id;
    std::string name;
    std::uint32_t base, size, entry, gp;
};
const std::map<int, Module> &modules();
using HleModuleStart = std::function<void()>;
void register_hle_module(const std::string &name, HleModuleStart start);

void register_interrupt(unsigned line, std::uint32_t handler, std::uint32_t arg, std::uint32_t mode);
void release_interrupt(unsigned line);
void enable_interrupt(unsigned line, bool enabled);
void interrupt_state(bool suspended);
bool interrupts_suspended();
void deliver_interrupts();

void install_libraries();
void vblank_edge(bool start);
void install_sif();
void install_cdvdfsv();
void install_cdvd();
void install_io();

void trace_hle(const char *library, const char *function, const Cpu &c);
extern bool g_trace_hle;
extern bool g_trace_modules;

}
