#include "kernel.hpp"

#include "hw.hpp"
#include "iop/sif.hpp"
#include "ps2/generated.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include "ps2/state.hpp"

#include <algorithm>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

namespace ps2::kernel {

namespace {

enum Status { kRun = 1, kReady = 2, kWait = 4, kSuspend = 8, kWaitSuspend = 12, kDormant = 16 };
enum WaitType { kWaitNone, kWaitSleep, kWaitSema };

constexpr std::uint32_t kKernelStackTop = 0x00080000u;
constexpr std::uint32_t kSyscallTable = 0x00001800u;
constexpr std::uint32_t kContextReserve = 0x2A0u;
constexpr int kMaxPriority = 128;

struct Thread {
    int id{};
    int status{kDormant};
    int priority{};
    int init_priority{};
    std::uint32_t entry{};
    std::uint32_t stack{};
    std::uint32_t stack_size{};
    std::uint32_t gp{};
    std::uint32_t heap{};
    std::uint32_t option{};
    std::uint32_t attr{};
    int wakeups{};
    int suspends{};
    WaitType wait{kWaitNone};
    int wait_id{};
    std::uint64_t order{};
    ThreadContext context{};
};

struct Semaphore {
    int count{};
    int max{};
    int init{};
    std::uint32_t attr{};
    std::uint32_t option{};
    std::deque<int> waiters;
};

struct Handler {
    int id{};
    std::uint32_t cause{};
    std::uint32_t address{};
    std::uint32_t arg{};
    std::uint32_t gp{};
};

struct Alarm {
    std::uint32_t callback{};
    std::uint32_t arg{};
    std::uint32_t gp{};
    std::uint64_t due{};
};

std::map<int, Thread> g_threads;
std::map<int, Semaphore> g_semas;
std::vector<Handler> g_intc_handlers;
std::vector<Handler> g_dmac_handlers;
std::map<int, Alarm> g_alarms;
int g_current = 0;
int g_next_handler = 1;
std::uint64_t g_order = 0;
bool g_in_handler = false;
std::vector<std::uint32_t> g_guest_calls;
bool g_reschedule = false;
std::vector<int> g_pending_alarms;
std::uint32_t g_vsync_flag = 0, g_vsync_data = 0;
std::uint32_t g_osd_config = 0;

template <typename Map>
int lowest_free(const Map &map, int limit, int first = 1) {
    for (int id = first; id < limit; ++id)
        if (!map.count(id)) return id;
    return -1;
}

std::uint32_t arg(const Cpu &c, int i) { return c.r[4 + i].w[0]; }
void ret(Cpu &c, std::int64_t v) { c.r[2].sd[0] = v; }

Thread *current() {
    auto it = g_threads.find(g_current);
    return it == g_threads.end() ? nullptr : &it->second;
}

Thread *thread(int id) {
    if (id == 0) return current();
    auto it = g_threads.find(id);
    return it == g_threads.end() ? nullptr : &it->second;
}

void make_ready(Thread &t) {
    t.status = kReady;
    t.order = ++g_order;
    g_reschedule = true;
}

Thread *best_ready() {
    Thread *best = nullptr;
    for (auto &[id, t] : g_threads) {
        if (t.status != kReady && t.status != kRun) continue;
        if (!best || t.priority < best->priority || (t.priority == best->priority && t.order < best->order)) best = &t;
    }
    return best;
}

void switch_to(Cpu &c, Thread *next) {
    Thread *cur = current();
    // After ExitDeleteThread, both pointers can be null. Still enter idle
    // and unwind instead of continuing the deleted thread's guest code.
    if (cur && cur == next) return;
    if (cur && (cur->status == kRun || cur->status == kReady || cur->status == kWait || cur->status == kSuspend || cur->status == kWaitSuspend)) {
        save_context(c, cur->context);
        if (cur->status == kRun) cur->status = kReady;
    }
    if (next) {
        next->status = kRun;
        g_current = next->id;
        load_context(c, next->context);
        if (g_options.trace_threads) std::fprintf(stderr, "[thread] -> %d at 0x%08X\n", next->id, next->context.pc);
    } else {
        g_current = 0;
        c.pc = kIdle;
        if (g_options.trace_threads) std::fprintf(stderr, "[thread] -> idle\n");
    }
    c.pc |= kUnwind;
}

void reschedule(Cpu &c) {
    g_reschedule = false;
    if (g_in_handler) return;
    Thread *cur = current();
    Thread *best = best_ready();
    if (best == cur && cur && cur->status == kRun) return;
    switch_to(c, best);
}

void block_current(Cpu &c, WaitType type, int id) {
    Thread *t = current();
    t->status = kWait;
    t->wait = type;
    t->wait_id = id;
    save_context(c, t->context);
    reschedule(c);
}

void release(Thread &t, std::int64_t result) {
    t.wait = kWaitNone;
    t.context.r[2].sd[0] = result;
    if (t.status == kWaitSuspend) t.status = kSuspend;
    else make_ready(t);
}

std::uint32_t run_guest(Cpu &c, std::uint32_t address, std::uint32_t a0, std::uint32_t a1, std::uint32_t a2, std::uint32_t gp,
                        std::uint32_t a3 = 0) {
    ThreadContext saved;
    save_context(c, saved);
    const bool nested = g_in_handler;
    g_in_handler = true;
    const std::uint32_t status = c.cop0[12];
    c.cop0[12] = (status & ~0x10001u) | 0x2u;
    c.r[4].sd[0] = static_cast<std::int32_t>(a0);
    c.r[5].sd[0] = static_cast<std::int32_t>(a1);
    c.r[6].sd[0] = static_cast<std::int32_t>(a2);
    c.r[7].sd[0] = static_cast<std::int32_t>(a3);
    if (gp) c.r[28].sd[0] = static_cast<std::int32_t>(gp);
    c.r[29].sd[0] = static_cast<std::int32_t>(kKernelStackTop - (nested ? 0x4000u : 0u));
    c.r[31].sd[0] = static_cast<std::int32_t>(kHandlerReturn);
    c.pc = address;
    g_guest_calls.push_back(address);
    while (!halted()) {
        c.pc &= ~kUnwind;
        if (c.pc == kHandlerReturn) break;
        if (c.budget <= 0) set_budget(c, 20000);
        dispatch(c, 0, 0);
    }
    g_guest_calls.pop_back();
    const std::uint32_t result = c.r[2].w[0];
    load_context(c, saved);
    c.cop0[12] = status;
    g_in_handler = nested;
    return result;
}

void run_intc(Cpu &c, unsigned cause) {
    hw::intc_ack(cause);
    const std::vector<Handler> handlers = g_intc_handlers;
    for (const Handler &h : handlers) {
        if (h.cause != cause) continue;
        const std::int32_t result = static_cast<std::int32_t>(run_guest(c, h.address, cause, h.arg, 0, h.gp));
        if (result < 0) break;
    }
}

void run_dmac(Cpu &c, unsigned channel) {
    hw::dmac_ack(channel);
    static const bool trace = std::getenv("PS2_TRACE_DMA") != nullptr;
    if (trace) std::fprintf(stderr, "[dmac] interrupt ch%u thread %d\n", channel, g_current);
    const std::vector<Handler> handlers = g_dmac_handlers;
    for (const Handler &h : handlers) {
        if (h.cause != channel) continue;
        const std::int32_t result = static_cast<std::int32_t>(run_guest(c, h.address, channel, h.arg, 0, h.gp));
        if (result < 0) break;
    }
}

bool interrupts_enabled() {
    const std::uint32_t status = g_cpu.cop0[12];
    return (status & 0x10001u) == 0x10001u && !(status & 6u);
}

void add_handler(Cpu &c, std::vector<Handler> &list) {
    Handler h{g_next_handler++, arg(c, 0), arg(c, 1), arg(c, 3), c.r[28].w[0]};
    const auto next = static_cast<std::int32_t>(arg(c, 2));
    if (next == 0) list.insert(list.begin(), h);
    else if (next == -1) list.push_back(h);
    else {
        auto it = std::find_if(list.begin(), list.end(), [&](const Handler &x) { return x.id == next; });
        list.insert(it, h);
    }
    ret(c, h.id);
}

void remove_handler(Cpu &c, std::vector<Handler> &list) {
    const int id = static_cast<int>(arg(c, 1));
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Handler &h) { return h.id == id; }), list.end());
    ret(c, 0);
}

void fire_alarm(int id);

void arm_alarm(int id) {
    schedule(hw::kEventCount + id, g_alarms[id].due, [id]() { fire_alarm(id); });
}

void fire_alarm(int id) {
    if (g_alarms.count(id)) g_pending_alarms.push_back(id);
}

void run_alarms(Cpu &c) {
    while (!g_pending_alarms.empty()) {
        const int id = g_pending_alarms.front();
        g_pending_alarms.erase(g_pending_alarms.begin());
        auto it = g_alarms.find(id);
        if (it == g_alarms.end()) continue;
        const Alarm alarm = it->second;
        g_alarms.erase(it);
        run_guest(c, alarm.callback, static_cast<std::uint32_t>(id), static_cast<std::uint32_t>(now() / hblank_cycles()), alarm.arg, alarm.gp);
        g_reschedule = true;
    }
}

int create_thread(Cpu &c) {
    const std::uint32_t p = arg(c, 0);
    Thread t;
    t.id = lowest_free(g_threads, 256);
    if (t.id < 0) return -1;
    t.status = kDormant;
    t.entry = ld32(p + 4);
    t.stack = ld32(p + 8);
    t.stack_size = ld32(p + 12);
    t.gp = ld32(p + 16);
    t.init_priority = t.priority = static_cast<int>(ld32(p + 20));
    t.attr = ld32(p + 28);
    t.option = ld32(p + 32);
    t.heap = current() ? current()->heap : 0;
    g_threads[t.id] = t;
    if (g_options.trace_threads)
        std::fprintf(stderr, "[thread] create %d entry 0x%08X stack 0x%08X+0x%X prio %d\n", t.id, t.entry, t.stack, t.stack_size, t.priority);
    return t.id;
}

void reset_thread(Thread &t, std::uint32_t argument) {
    std::memset(&t.context, 0, sizeof t.context);
    t.context.pc = t.entry;
    t.context.r[4].sd[0] = static_cast<std::int32_t>(argument);
    t.context.r[28].sd[0] = static_cast<std::int32_t>(t.gp);
    t.context.r[29].sd[0] = static_cast<std::int32_t>(t.stack + t.stack_size - kContextReserve);
    t.context.r[31].sd[0] = static_cast<std::int32_t>(kIdle);
    t.priority = t.init_priority;
    t.wakeups = 0;
    t.wait = kWaitNone;
}

void exit_current(Cpu &c, bool remove) {
    Thread *t = current();
    if (!t) return;
    t->status = kDormant;
    if (remove) {
        g_threads.erase(t->id);
        g_current = 0;
    }
    reschedule(c);
}

void write_thread_status(int id, std::uint32_t p) {
    Thread *t = thread(id);
    if (!t || !p) return;
    st32(p + 0, static_cast<std::uint32_t>(t->status));
    st32(p + 4, t->entry);
    st32(p + 8, t->stack);
    st32(p + 12, t->stack_size);
    st32(p + 16, t->gp);
    st32(p + 20, static_cast<std::uint32_t>(t->init_priority));
    st32(p + 24, static_cast<std::uint32_t>(t->priority));
    st32(p + 28, t->attr);
    st32(p + 32, t->option);
    st32(p + 36, t->wait == kWaitSema ? 2u : t->wait == kWaitSleep ? 1u : 0u);
    st32(p + 40, static_cast<std::uint32_t>(t->wait_id));
    st32(p + 44, static_cast<std::uint32_t>(t->wakeups));
}

void signal_sema(Cpu &c, int id, bool interrupt) {
    auto it = g_semas.find(id);
    if (it == g_semas.end()) {
        ret(c, -1);
        return;
    }
    Semaphore &s = it->second;
    ret(c, id);
    if (!s.waiters.empty()) {
        const int waiter = s.waiters.front();
        s.waiters.pop_front();
        if (Thread *t = thread(waiter)) release(*t, id);
        if (!interrupt) reschedule(c);
    } else {
        ++s.count;
    }
}

void wait_sema(Cpu &c, int id) {
    auto it = g_semas.find(id);
    if (it == g_semas.end()) {
        ret(c, -1);
        return;
    }
    Semaphore &s = it->second;
    ret(c, id);
    if (s.count > 0) {
        --s.count;
        return;
    }
    if (g_in_handler) {
        std::fprintf(stderr, "[kernel] WaitSema(%d) inside an interrupt handler (ra 0x%08X, thread %d, handler 0x%08X)\n", id, c.r[31].w[0], g_current, g_guest_calls.empty() ? 0u : g_guest_calls.back());
        return;
    }
    s.waiters.push_back(g_current);
    block_current(c, kWaitSema, id);
}

const char *syscall_name(std::uint32_t n);

}

void reset() {
    g_threads.clear();
    g_semas.clear();
    g_intc_handlers.clear();
    g_dmac_handlers.clear();
    g_alarms.clear();
    g_current = 0;
    g_next_handler = 1;
    g_in_handler = g_reschedule = false;
    g_pending_alarms.clear();
    std::memset(g_ram + kSyscallTable, 0, 0x400);
}

std::uint32_t g_boot_argc = 0, g_boot_argv = 0;

void start_main_thread(std::uint32_t entry, std::uint32_t gp, std::uint32_t argc, std::uint32_t argv) {
    g_boot_argc = argc;
    g_boot_argv = argv;
    Thread t;
    t.id = 1;
    t.status = kRun;
    t.entry = entry;
    t.gp = gp;
    g_threads[t.id] = t;
    g_current = t.id;
    Cpu &c = g_cpu;
    std::memset(&c, 0, sizeof c);
    c.vu0.vf[0].w[3] = 0x3F800000u;
    c.pc = entry;
    c.r[4].sd[0] = static_cast<std::int32_t>(argc);
    c.r[5].sd[0] = static_cast<std::int32_t>(argv);
    c.cop0[12] = 0x70010C01u;
    c.cop0[15] = 0x2E20u;
}

bool must_unwind() {
    if (g_in_handler) return false;
    if (g_reschedule || !g_pending_alarms.empty()) return true;
    return interrupts_enabled() && (hw::intc_pending() || hw::dmac_pending());
}

bool in_handler() { return g_in_handler; }
bool interruptible() { return interrupts_enabled() && !g_in_handler; }
int current_thread() { return g_current; }

void service(Cpu &c) {
    if (g_in_handler) return;
    run_alarms(c);
    if (interrupts_enabled()) {
        for (unsigned guard = 0; guard < 16; ++guard) {
            const std::uint32_t pending = hw::intc_pending();
            const std::uint32_t dmac = hw::dmac_pending();
            if (!pending && !dmac) break;
            if (dmac) run_dmac(c, static_cast<unsigned>(std::countr_zero(dmac)));
            else run_intc(c, static_cast<unsigned>(std::countr_zero(pending)));
        }
    }
    if (g_reschedule || c.pc == kIdle) reschedule(c);
    c.pc &= ~kUnwind;
}

void on_vblank_start() {
    if (!g_vsync_flag) return;
    st32(g_vsync_flag, 1);
    if (g_vsync_data) st64(g_vsync_data, hw::gs_csr());
    g_vsync_flag = g_vsync_data = 0;
}

void wake_idle() { g_reschedule = true; }

void dump() {
    for (const auto &[id, t] : g_threads) {
        std::fprintf(stderr, "[thread] %d status %d prio %d wait %d/%d pc 0x%08X ra 0x%08X\n", id, t.status, t.priority, t.wait, t.wait_id,
                     id == g_current ? g_cpu.pc : t.context.pc, id == g_current ? g_cpu.r[31].w[0] : t.context.r[31].w[0]);
        const std::uint32_t sp = id == g_current ? g_cpu.r[29].w[0] : t.context.r[29].w[0];
        std::fprintf(stderr, "[thread]   stack:");
        for (std::uint32_t i = 0; i < 512 && sp + i * 4 < kRamSize; ++i) {
            const std::uint32_t v = load<std::uint32_t>(g_ram + sp + i * 4);
            if (v >= 0x00100000u && v < 0x00617A14u && has_code(v) && (v & 3u) == 0) std::fprintf(stderr, " %08X", v);
        }
        std::fprintf(stderr, "\n");
    }
    for (const auto &[id, s] : g_semas)
        if (!s.waiters.empty()) std::fprintf(stderr, "[sema] %d count %d waiters %zu\n", id, s.count, s.waiters.size());
}

void handle_syscall(Cpu &c, std::uint32_t pc) {
    std::uint32_t n = c.r[3].w[0];
    const bool interrupt_variant = (n & 0x80000000u) != 0;
    if (interrupt_variant) n = 0u - n;
    if (n < 0x100u) {
        const std::uint32_t custom = ld32(kSyscallTable + n * 4u);
        if (custom && has_code(custom & 0x1FFFFFFFu)) {
            const std::uint32_t result = run_guest(c, custom & 0x1FFFFFFFu, arg(c, 0), arg(c, 1), arg(c, 2), c.r[28].w[0], arg(c, 3));
            ret(c, static_cast<std::int32_t>(result));
            return;
        }
    }
    if (g_options.trace_syscalls)
        std::fprintf(stderr, "[sys] %s(0x%02X) %08X %08X %08X %08X from 0x%08X thread %d\n",
                     syscall_name(n), n, arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), pc, g_current);
    switch (n) {
    case 0x02: hw::gs_set_crt(arg(c, 0), arg(c, 1), arg(c, 2)); ret(c, 0); break;
    case 0x04: halt("Exit"); break;
    case 0x06: case 0x07: halt("program asked to run another executable"); break;
    case 0x10: add_handler(c, g_intc_handlers); break;
    case 0x11: remove_handler(c, g_intc_handlers); break;
    case 0x12: add_handler(c, g_dmac_handlers); break;
    case 0x13: remove_handler(c, g_dmac_handlers); break;
    case 0x14: case 0x1A: ret(c, hw::intc_set_mask(arg(c, 0), true) ? 1 : 0); break;
    case 0x15: case 0x1B: ret(c, hw::intc_set_mask(arg(c, 0), false) ? 1 : 0); break;
    case 0x16: case 0x1C: ret(c, hw::dmac_set_mask(arg(c, 0), true) ? 1 : 0); break;
    case 0x17: case 0x1D: ret(c, hw::dmac_set_mask(arg(c, 0), false) ? 1 : 0); break;
    case 0x18: case 0x1E: case 0xFC: case 0xFE: {
        const int id = lowest_free(g_alarms, 64);
        if (id < 0) { ret(c, -1); break; }
        g_alarms[id] = Alarm{arg(c, 1), arg(c, 2), c.r[28].w[0], now() + static_cast<std::uint64_t>(arg(c, 0) & 0xFFFFu) * hblank_cycles()};
        arm_alarm(id);
        ret(c, id);
        break;
    }
    case 0x19: case 0x1F: case 0xFD: case 0xFF: g_alarms.erase(static_cast<int>(arg(c, 0))); ret(c, 0); break;
    case 0x20: ret(c, create_thread(c)); break;
    case 0x21: g_threads.erase(static_cast<int>(arg(c, 0))); ret(c, 0); break;
    case 0x22: {
        Thread *t = thread(static_cast<int>(arg(c, 0)));
        if (!t) { ret(c, -1); break; }
        reset_thread(*t, arg(c, 1));
        ret(c, t->id);
        make_ready(*t);
        reschedule(c);
        break;
    }
    case 0x23: exit_current(c, false); break;
    case 0x24: exit_current(c, true); break;
    case 0x25: case 0x26: {
        Thread *t = thread(static_cast<int>(arg(c, 0)));
        if (t) {
            for (auto &[id, s] : g_semas) s.waiters.erase(std::remove(s.waiters.begin(), s.waiters.end(), t->id), s.waiters.end());
            t->status = kDormant;
            t->wait = kWaitNone;
        }
        ret(c, t ? 0 : -1);
        break;
    }
    case 0x29: case 0x2A: {
        Thread *t = thread(static_cast<int>(arg(c, 0)));
        if (!t) { ret(c, -1); break; }
        ret(c, t->priority);
        t->priority = static_cast<int>(arg(c, 1)) & (kMaxPriority - 1);
        if (t->status == kReady) t->order = ++g_order;
        if (n == 0x29) reschedule(c);
        else g_reschedule = true;
        break;
    }
    case 0x2B: case 0x2C: {
        const int prio = static_cast<int>(arg(c, 0)) & (kMaxPriority - 1);
        Thread *t = current();
        ret(c, prio);
        if (t && t->priority == prio) {
            t->order = ++g_order;
        } else {
            Thread *first = nullptr;
            for (auto &[id, x] : g_threads)
                if (x.status == kReady && x.priority == prio && (!first || x.order < first->order)) first = &x;
            if (first) first->order = ++g_order;
        }
        if (n == 0x2B) reschedule(c);
        else g_reschedule = true;
        break;
    }
    case 0x2D: case 0x2E: {
        Thread *t = thread(static_cast<int>(arg(c, 0)));
        if (t && (t->status == kWait || t->status == kWaitSuspend)) {
            for (auto &[id, s] : g_semas) s.waiters.erase(std::remove(s.waiters.begin(), s.waiters.end(), t->id), s.waiters.end());
            release(*t, -418);
            if (n == 0x2D) reschedule(c);
        }
        ret(c, t ? t->id : -1);
        break;
    }
    case 0x2F: ret(c, g_current); break;
    case 0x30: case 0x31: write_thread_status(static_cast<int>(arg(c, 0)), arg(c, 1)); ret(c, thread(static_cast<int>(arg(c, 0))) ? thread(static_cast<int>(arg(c, 0)))->status : -1); break;
    case 0x32: {
        Thread *t = current();
        ret(c, g_current);
        if (t->wakeups > 0) { --t->wakeups; break; }
        block_current(c, kWaitSleep, 0);
        break;
    }
    case 0x33: case 0x34: {
        Thread *t = thread(static_cast<int>(arg(c, 0)));
        ret(c, t ? t->id : -1);
        if (!t) break;
        if (t->wait == kWaitSleep) {
            release(*t, t->id);
            if (n == 0x33) reschedule(c);
        } else {
            ++t->wakeups;
        }
        break;
    }
    case 0x35: case 0x36: {
        Thread *t = thread(static_cast<int>(arg(c, 0)));
        ret(c, t ? t->wakeups : -1);
        if (t) t->wakeups = 0;
        break;
    }
    case 0x37: case 0x38: {
        Thread *t = thread(static_cast<int>(arg(c, 0)));
        if (t && t != current()) {
            if (t->status == kWait) t->status = kWaitSuspend;
            else if (t->status == kReady) t->status = kSuspend;
            ++t->suspends;
        }
        ret(c, t ? t->id : -1);
        break;
    }
    case 0x39: case 0x3A: {
        Thread *t = thread(static_cast<int>(arg(c, 0)));
        if (t && t->suspends > 0 && --t->suspends == 0) {
            if (t->status == kWaitSuspend) t->status = kWait;
            else if (t->status == kSuspend) make_ready(*t);
            if (n == 0x39) reschedule(c);
        }
        ret(c, t ? t->id : -1);
        break;
    }
    case 0x3C: {
        Thread *t = current();
        const std::uint32_t stack = arg(c, 1), size = arg(c, 2);
        const std::uint32_t top = stack == 0xFFFFFFFFu ? kRamSize - 0x1000u : stack + size;
        t->stack = top - size;
        t->stack_size = size;
        t->gp = arg(c, 0);
        if (const std::uint32_t args = arg(c, 3); args && g_boot_argv) {
            const std::uint32_t count = std::min<std::uint32_t>(g_boot_argc, 16u);
            std::uint32_t payload = args + 4u + 16u * 4u;
            const std::uint32_t end = payload + 256u;
            std::uint32_t used = 0;
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::uint32_t from = ld32(g_boot_argv + 4u * i);
                st32(args + 4u + 4u * i, payload);
                std::uint32_t k = 0;
                for (; payload + k + 1u < end; ++k) {
                    const std::uint8_t ch = ld8(from + k);
                    st8(payload + k, ch);
                    if (!ch) break;
                }
                st8(payload + k, 0);
                payload += k + 1u;
                ++used;
                if (payload >= end) break;
            }
            st32(args, used);
        }
        ret(c, top - kContextReserve);
        break;
    }
    case 0x3D: {
        Thread *t = current();
        t->heap = arg(c, 1) == 0xFFFFFFFFu ? t->stack : arg(c, 0) + arg(c, 1);
        ret(c, t->heap);
        break;
    }
    case 0x3E: ret(c, current()->heap); break;
    case 0x40: {
        const std::uint32_t p = arg(c, 0);
        Semaphore s;
        s.count = static_cast<int>(ld32(p + 0));
        s.max = static_cast<int>(ld32(p + 4));
        s.init = static_cast<int>(ld32(p + 8));
        s.attr = ld32(p + 16);
        s.option = ld32(p + 20);
        s.count = s.init;
        const int id = lowest_free(g_semas, 256, 0);
        if (id < 0) { ret(c, -1); break; }
        g_semas[id] = s;
        ret(c, id);
        break;
    }
    case 0x41: {
        auto it = g_semas.find(static_cast<int>(arg(c, 0)));
        if (it == g_semas.end()) { ret(c, -1); break; }
        for (int waiter : it->second.waiters)
            if (Thread *t = thread(waiter)) release(*t, -425);
        g_semas.erase(it);
        ret(c, static_cast<std::int32_t>(arg(c, 0)));
        reschedule(c);
        break;
    }
    case 0x42: signal_sema(c, static_cast<int>(arg(c, 0)), false); break;
    case 0x43: signal_sema(c, static_cast<int>(arg(c, 0)), true); break;
    case 0x44: wait_sema(c, static_cast<int>(arg(c, 0))); break;
    case 0x45: case 0x46: {
        auto it = g_semas.find(static_cast<int>(arg(c, 0)));
        if (it == g_semas.end() || it->second.count <= 0) { ret(c, -1); break; }
        --it->second.count;
        ret(c, it->first);
        break;
    }
    case 0x47: case 0x48: {
        auto it = g_semas.find(static_cast<int>(arg(c, 0)));
        if (it == g_semas.end()) { ret(c, -1); break; }
        const std::uint32_t p = arg(c, 1);
        st32(p + 0, static_cast<std::uint32_t>(it->second.count));
        st32(p + 4, static_cast<std::uint32_t>(it->second.max));
        st32(p + 8, static_cast<std::uint32_t>(it->second.init));
        st32(p + 12, static_cast<std::uint32_t>(it->second.waiters.size()));
        st32(p + 16, it->second.attr);
        st32(p + 20, it->second.option);
        ret(c, it->first);
        break;
    }
    case 0x4A: g_osd_config = ld32(arg(c, 0)); ret(c, 0); break;
    case 0x4B: st32(arg(c, 0), g_osd_config); ret(c, 0); break;
    case 0x64: case 0x68: ret(c, 0); break;
    case 0x5B: case 0x6B: ret(c, 0); break;
    case 0x70: ret(c, static_cast<std::int64_t>(hw::gs_imr())); break;
    case 0x71: { const std::uint64_t old = hw::gs_imr(); hw::gs_set_imr(arg(c, 0)); ret(c, static_cast<std::int64_t>(old)); break; }
    case 0x73: g_vsync_flag = arg(c, 0); g_vsync_data = arg(c, 1); ret(c, 0); break;
    case 0x74:
        if (arg(c, 0) < 0x100u) st32(kSyscallTable + arg(c, 0) * 4u, arg(c, 1));
        else std::fprintf(stderr, "[sys] SetSyscall(0x%X) not supported\n", arg(c, 0));
        ret(c, 0);
        break;
    case 0x76: ret(c, sif::dma_stat(arg(c, 0))); break;
    case 0x77: ret(c, sif::set_dma(arg(c, 0), arg(c, 1))); request_iop_sync(); break;
    case 0x78:
        if (interrupt_variant) sif::packet_processed();
        ret(c, 0);
        break;
    case 0x79: sif::set_reg(arg(c, 0), arg(c, 1)); ret(c, 0); break;
    case 0x7A: ret(c, static_cast<std::int32_t>(sif::get_reg(arg(c, 0)))); break;
    case 0x7C: {
        // Deci2Call: a retail console has no DECI2 manager, so opening a socket fails. A game
        // that gets a valid socket sends its TTY output over it and waits forever for the
        // write-done event that only a debugger would deliver.
        const std::uint32_t fn = arg(c, 0);
        ret(c, fn >= 1 && fn <= 6 ? -1 : 0);
        break;
    }
    case 0x7E: ret(c, 0); break;
    case 0x7F: ret(c, kRamSize); break;
    default:
        std::fprintf(stderr, "[sys] unhandled syscall 0x%02X from 0x%08X\n", n, pc);
        ret(c, 0);
        break;
    }
}

namespace {

const char *syscall_name(std::uint32_t n) {
    switch (n) {
    case 0x02: return "GsSetCrt";
    case 0x04: return "Exit";
    case 0x06: return "LoadExecPS2";
    case 0x07: return "ExecPS2";
    case 0x10: return "AddIntcHandler";
    case 0x11: return "RemoveIntcHandler";
    case 0x12: return "AddDmacHandler";
    case 0x13: return "RemoveDmacHandler";
    case 0x14: return "EnableIntc";
    case 0x15: return "DisableIntc";
    case 0x16: return "EnableDmac";
    case 0x17: return "DisableDmac";
    case 0x18: return "SetAlarm";
    case 0x19: return "ReleaseAlarm";
    case 0x1A: return "iEnableIntc";
    case 0x1B: return "iDisableIntc";
    case 0x1C: return "iEnableDmac";
    case 0x1D: return "iDisableDmac";
    case 0x1E: return "iSetAlarm";
    case 0x1F: return "iReleaseAlarm";
    case 0x20: return "CreateThread";
    case 0x21: return "DeleteThread";
    case 0x22: return "StartThread";
    case 0x23: return "ExitThread";
    case 0x24: return "ExitDeleteThread";
    case 0x25: return "TerminateThread";
    case 0x26: return "iTerminateThread";
    case 0x29: return "ChangeThreadPriority";
    case 0x2A: return "iChangeThreadPriority";
    case 0x2B: return "RotateThreadReadyQueue";
    case 0x2C: return "iRotateThreadReadyQueue";
    case 0x2D: return "ReleaseWaitThread";
    case 0x2E: return "iReleaseWaitThread";
    case 0x2F: return "GetThreadId";
    case 0x30: return "ReferThreadStatus";
    case 0x31: return "iReferThreadStatus";
    case 0x32: return "SleepThread";
    case 0x33: return "WakeupThread";
    case 0x34: return "iWakeupThread";
    case 0x35: return "CancelWakeupThread";
    case 0x36: return "iCancelWakeupThread";
    case 0x37: return "SuspendThread";
    case 0x38: return "iSuspendThread";
    case 0x39: return "ResumeThread";
    case 0x3A: return "iResumeThread";
    case 0x3C: return "SetupThread";
    case 0x3D: return "SetupHeap";
    case 0x3E: return "EndOfHeap";
    case 0x40: return "CreateSema";
    case 0x41: return "DeleteSema";
    case 0x42: return "SignalSema";
    case 0x43: return "iSignalSema";
    case 0x44: return "WaitSema";
    case 0x45: return "PollSema";
    case 0x46: return "iPollSema";
    case 0x47: return "ReferSemaStatus";
    case 0x48: return "iReferSemaStatus";
    case 0x4A: return "SetOsdConfigParam";
    case 0x4B: return "GetOsdConfigParam";
    case 0x5B: return "GetEntryAddress";
    case 0x64: return "FlushCache";
    case 0x6B: return "SifStopDma";
    case 0x68: return "iFlushCache";
    case 0x70: return "GsGetIMR";
    case 0x71: return "GsPutIMR";
    case 0x73: return "SetVSyncFlag";
    case 0x74: return "SetSyscall";
    case 0x76: return "SifDmaStat";
    case 0x77: return "SifSetDma";
    case 0x78: return "SifSetDChain";
    case 0x79: return "SifSetReg";
    case 0x7A: return "SifGetReg";
    case 0x7C: return "Deci2Call";
    case 0x7E: return "MachineType";
    case 0x7F: return "GetMemorySize";
    default: return "?";
    }
}

}

}

namespace ps2 {

void syscall(Cpu &c, std::uint32_t pc) { kernel::handle_syscall(c, pc); }

std::uint32_t mfc0(Cpu &c, std::uint32_t reg, std::uint32_t) {
    switch (reg) {
    case 9: return static_cast<std::uint32_t>(now());
    case 12: return c.cop0[12];
    case 15: return 0x2E20u;
    default: return c.cop0[reg];
    }
}

void mtc0(Cpu &c, std::uint32_t reg, std::uint32_t value) {
    c.cop0[reg] = value;
}

std::uint32_t eret(Cpu &c) {
    std::uint32_t &status = c.cop0[12];
    if (status & 4u) {
        status &= ~4u;
        return c.cop0[30];
    }
    status &= ~2u;
    return c.cop0[14];
}

void set_interrupts(Cpu &c, bool enabled) {
    c.cop0[12] = enabled ? (c.cop0[12] | 0x10000u) : (c.cop0[12] & ~0x10000u);
}

}

namespace ps2::kernel {

std::function<void()> alarm_action(int id) {
    return [id]() { fire_alarm(id); };
}

void state(ps2::state::Stream &s) {
    s.tag("eekern");
    if (s.saving() && (g_in_handler || !g_guest_calls.empty())) throw std::runtime_error("the EE is inside an interrupt handler");
    s.map(g_threads);
    s.map(g_semas, [&s](Semaphore &v) {
        s.pod(v.count);
        s.pod(v.max);
        s.pod(v.init);
        s.pod(v.attr);
        s.pod(v.option);
        s.deque(v.waiters);
    });
    s.vec(g_intc_handlers);
    s.vec(g_dmac_handlers);
    s.map(g_alarms);
    s.pod(g_current);
    s.pod(g_next_handler);
    s.pod(g_order);
    s.pod(g_reschedule);
    s.vec(g_pending_alarms);
    s.pod(g_vsync_flag);
    s.pod(g_vsync_data);
    s.pod(g_osd_config);
    s.pod(g_boot_argc);
    s.pod(g_boot_argv);
}

}
