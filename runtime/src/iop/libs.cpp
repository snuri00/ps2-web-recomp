#include "kernel.hpp"

#include "hw.hpp"

#include "ps2/state.hpp"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <vector>

namespace ps2::iop::kernel {

namespace {

constexpr int kErrorUnknownThread = -407;
constexpr int kErrorUnknownSema = -408;
constexpr int kErrorUnknownEvent = -409;
constexpr int kErrorUnknownMbx = -410;
constexpr int kErrorUnknownFpl = -412;
constexpr int kErrorUnknownVpl = -452;
constexpr int kErrorSemaZero = -419;
constexpr int kErrorEventCondition = -421;
constexpr int kErrorMbxNone = -424;
constexpr int kErrorNoMemory = -400;
constexpr int kErrorUnknownModule = -202;

std::uint32_t arg(const Cpu &c, int i) { return i < 4 ? c.r[4 + i] : read32(c.r[29] + 16u + static_cast<std::uint32_t>(i - 4) * 4u); }

void def(const char *library, std::uint32_t index, const char *name, std::function<void(Cpu &)> body) {
    register_hle(library, index, [library, name, body = std::move(body)](Cpu &c) {
        trace_hle(library, name, c);
        body(c);
    });
}

std::string format(Cpu &c, std::uint32_t fmt_address, int first) {
    const std::string fmt = ram_string(fmt_address, 1024);
    std::string out;
    int next = first;
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') { out += fmt[i]; continue; }
        std::string spec = "%";
        ++i;
        while (i < fmt.size() && std::strchr("-+ #0", fmt[i])) spec += fmt[i++];
        while (i < fmt.size() && (std::isdigit(static_cast<unsigned char>(fmt[i])) || fmt[i] == '.')) spec += fmt[i++];
        while (i < fmt.size() && (fmt[i] == 'l' || fmt[i] == 'h')) ++i;
        if (i >= fmt.size()) break;
        const char conv = fmt[i];
        char buf[512];
        switch (conv) {
        case 'd': case 'i': std::snprintf(buf, sizeof buf, (spec + "d").c_str(), static_cast<int>(arg(c, next++))); break;
        case 'u': std::snprintf(buf, sizeof buf, (spec + "u").c_str(), arg(c, next++)); break;
        case 'x': case 'X': case 'o': std::snprintf(buf, sizeof buf, (spec + conv).c_str(), arg(c, next++)); break;
        case 'p': std::snprintf(buf, sizeof buf, "%08x", arg(c, next++)); break;
        case 'c': std::snprintf(buf, sizeof buf, (spec + "c").c_str(), static_cast<int>(arg(c, next++))); break;
        case 's': std::snprintf(buf, sizeof buf, (spec + "s").c_str(), ram_string(arg(c, next++)).c_str()); break;
        case '%': std::snprintf(buf, sizeof buf, "%%"); break;
        default: std::snprintf(buf, sizeof buf, "%s%c", spec.c_str(), conv); break;
        }
        out += buf;
    }
    return out;
}

void print(const std::string &text) {
    static std::string line;
    for (char ch : text) {
        if (ch == '\n') {
            std::fprintf(stderr, "[iop-tty] %s\n", line.c_str());
            line.clear();
        } else if (ch != '\r') {
            line += ch;
        }
    }
}

struct Sema {
    int count, max;
    std::uint32_t attr, option;
    std::deque<int> waiters;
};

struct EventFlag {
    std::uint32_t bits, attr, option;
    std::deque<int> waiters;
};

struct Mbx {
    std::uint32_t attr, option;
    std::deque<std::uint32_t> messages;
    std::deque<int> waiters;
};

struct Fpl {
    std::uint32_t attr, option, block_size, blocks, base;
    std::vector<std::uint32_t> free;
    std::deque<int> waiters;
};

// Kingdom Hearts II's sound driver allocates its working memory with the
// kernel's variable sized thread pool library: one pool of 0x39000 bytes whose
//blocks, rounded to 128, hold streaming buffers and thread stacks. A
// variable pool is an area the driver sub-allocates from, so keep the used
// spans per id and serve them first fit.
struct Vpl {
    std::uint32_t attr, option, size, base;
    std::map<std::uint32_t, std::uint32_t> used;
    std::deque<int> waiters;
};

std::map<int, Vpl> g_vpls;

struct VblankHandler {
    int edge;
    std::uint32_t handler, arg;
};

struct Alarm {
    std::uint32_t handler, arg;
    bool active;
};

std::map<int, Sema> g_semas;
std::map<int, EventFlag> g_events;
std::map<int, Mbx> g_mbxs;
std::map<int, Fpl> g_fpls;
std::map<std::uint32_t, std::set<std::uint32_t>> g_heaps;
std::vector<VblankHandler> g_vblank_handlers;
std::vector<int> g_vblank_start_waiters, g_vblank_end_waiters;
std::map<std::uint64_t, Alarm> g_alarms;
std::uint64_t g_next_alarm = 1;
int g_next_object = 1;
std::uint64_t g_rotate_order = 1ull << 40;
std::uint32_t g_strtok_next = 0;
std::uint32_t g_next_hard_timer = 1;

void signal_sema(Cpu &c, int id, bool from_interrupt) {
    auto it = g_semas.find(id);
    if (it == g_semas.end()) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownSema); return; }
    Sema &s = it->second;
    c.r[2] = 0;
    if (!s.waiters.empty()) {
        const int t = s.waiters.front();
        s.waiters.pop_front();
        if (Thread *th = thread(t)) wake(*th, 0);
        if (!from_interrupt) reschedule();
    } else if (s.count < s.max || s.max <= 0) {
        ++s.count;
    }
}

bool event_matches(const EventFlag &e, std::uint32_t bits, std::uint32_t mode) {
    return (mode & 1u) ? (e.bits & bits) != 0 : (e.bits & bits) == bits;
}

void event_take(EventFlag &e, std::uint32_t bits, std::uint32_t mode, std::uint32_t result_ptr) {
    if (result_ptr) write32(result_ptr, e.bits);
    if (mode & 0x10u) e.bits &= ~bits;
    if (mode & 0x20u) e.bits = 0;
}

void set_event(Cpu &c, int id, std::uint32_t bits, bool from_interrupt) {
    auto it = g_events.find(id);
    if (it == g_events.end()) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownEvent); return; }
    EventFlag &e = it->second;
    e.bits |= bits;
    c.r[2] = 0;
    for (auto w = e.waiters.begin(); w != e.waiters.end();) {
        Thread *t = thread(*w);
        if (t && event_matches(e, t->event_bits, t->event_mode)) {
            event_take(e, t->event_bits, t->event_mode, t->event_result);
            wake(*t, 0);
            w = e.waiters.erase(w);
            if (!from_interrupt) reschedule();
        } else {
            ++w;
        }
    }
}

void send_mbx(Cpu &c, int id, std::uint32_t message, bool from_interrupt) {
    auto it = g_mbxs.find(id);
    if (it == g_mbxs.end()) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownMbx); return; }
    Mbx &m = it->second;
    c.r[2] = 0;
    if (!m.waiters.empty()) {
        const int t = m.waiters.front();
        m.waiters.pop_front();
        if (Thread *th = thread(t)) {
            if (th->message_out) write32(th->message_out, message);
            wake(*th, 0);
        }
        if (!from_interrupt) reschedule();
    } else {
        m.messages.push_back(message);
    }
}

struct HardTimer {
    std::uint32_t compare = 0, handler = 0, common = 0, source = 1, prescale = 1;
    bool running = false;
    std::uint64_t generation = 0;
};
std::map<std::uint32_t, HardTimer> g_hard_timers;
std::map<std::uint32_t, std::uint64_t> g_hard_timer_start;

std::uint64_t timer_cycles(const HardTimer &t, std::uint32_t ticks) {
    const std::uint64_t per_tick = (t.source & 4u) ? 2343u : std::max<std::uint32_t>(t.prescale, 1u);
    return std::max<std::uint64_t>(static_cast<std::uint64_t>(ticks) * per_tick, 1u);
}

void arm_hard_timer(std::uint32_t id);

std::uint32_t hard_timer_counter(std::uint32_t id) {
    const auto it = g_hard_timers.find(id);
    if (it == g_hard_timers.end()) return 0;
    const std::uint64_t start = g_hard_timer_start.try_emplace(id, now()).first->second;
    return static_cast<std::uint32_t>((now() - start) / timer_cycles(it->second, 1));
}

void fire_hard_timer(std::uint32_t id, std::uint64_t generation) {
    auto it = g_hard_timers.find(id);
    if (it == g_hard_timers.end() || !it->second.running || it->second.generation != generation || !it->second.handler) return;
    const std::uint32_t next = call_guest(it->second.handler, it->second.common);
    it = g_hard_timers.find(id);
    if (it == g_hard_timers.end() || !it->second.running || it->second.generation != generation) return;
    if (!next) {
        it->second.running = false;
        return;
    }
    it->second.compare = next;
    arm_hard_timer(id);
}

void arm_hard_timer(std::uint32_t id) {
    HardTimer &t = g_hard_timers[id];
    const std::uint64_t generation = ++t.generation;
    if (!t.running || !t.handler || !t.compare) return;
    schedule(now() + timer_cycles(t, t.compare), Job{kJobHardTimer, id, static_cast<std::uint32_t>(generation), static_cast<std::uint32_t>(generation >> 32)});
}

void fire_alarm(std::uint64_t key) {
    auto it = g_alarms.find(key);
    if (it == g_alarms.end() || !it->second.active) return;
    const Alarm a = it->second;
    g_alarms.erase(it);
    const std::uint32_t again = call_guest(a.handler, a.arg);
    if (again) {
        const std::uint64_t k = g_next_alarm++;
        g_alarms[k] = Alarm{a.handler, a.arg, true};
        schedule(now() + again, Job{kJobAlarm, static_cast<std::uint32_t>(k), static_cast<std::uint32_t>(k >> 32)});
    }
}

void write_clock(std::uint32_t p, std::uint64_t cycles) {
    write32(p, static_cast<std::uint32_t>(cycles));
    write32(p + 4, static_cast<std::uint32_t>(cycles >> 32));
}

std::uint64_t read_clock(std::uint32_t p) {
    return read32(p) | (static_cast<std::uint64_t>(read32(p + 4)) << 32);
}

void install_sysmem() {
    register_hle_library("sysmem", 0x0101);
    def("sysmem", 4, "AllocSysMemory", [](Cpu &c) { c.r[2] = alloc(arg(c, 1), static_cast<int>(arg(c, 0)), arg(c, 2)); });
    def("sysmem", 5, "FreeSysMemory", [](Cpu &c) { free(arg(c, 0)); c.r[2] = 0; });
    def("sysmem", 6, "QueryMemSize", [](Cpu &c) { c.r[2] = kRamSize; });
    def("sysmem", 7, "QueryMaxFreeMemSize", [](Cpu &c) { c.r[2] = free_max(); });
    def("sysmem", 8, "QueryTotalFreeMemSize", [](Cpu &c) { c.r[2] = free_total(); });
    def("sysmem", 9, "QueryBlockTopAddress", [](Cpu &c) { c.r[2] = arg(c, 0); });
    def("sysmem", 10, "QueryBlockSize", [](Cpu &c) { c.r[2] = 0; });
    def("sysmem", 14, "Kprintf", [](Cpu &c) { print(format(c, arg(c, 0), 1)); c.r[2] = 0; });
}

void install_loadcore() {
    register_hle_library("loadcore", 0x0101);
    def("loadcore", 4, "FlushIcache", [](Cpu &c) { c.r[2] = 0; });
    def("loadcore", 5, "FlushDcache", [](Cpu &c) { c.r[2] = 0; });
    def("loadcore", 6, "RegisterLibraryEntries", [](Cpu &c) {
        const std::uint32_t table = arg(c, 0);
        char raw[9] = {};
        std::memcpy(raw, ram_ptr(table + 12), 8);
        register_library(raw, table);
        c.r[2] = 0;
    });
    def("loadcore", 7, "ReleaseLibraryEntries", [](Cpu &c) { c.r[2] = 0; });
    def("loadcore", 12, "QueryBootMode", [](Cpu &c) { c.r[2] = 0; });
    def("loadcore", 27, "SetRebootTimeLibraryHandlingMode", [](Cpu &c) { c.r[2] = 0; });
}

void install_intrman() {
    register_hle_library("intrman", 0x0102);
    def("intrman", 4, "RegisterIntrHandler", [](Cpu &c) {
        register_interrupt(arg(c, 0), arg(c, 2), arg(c, 3), arg(c, 1));
        c.r[2] = 0;
    });
    def("intrman", 5, "ReleaseIntrHandler", [](Cpu &c) { release_interrupt(arg(c, 0)); c.r[2] = 0; });
    def("intrman", 6, "EnableIntr", [](Cpu &c) { enable_interrupt(arg(c, 0), true); c.r[2] = 0; });
    def("intrman", 7, "DisableIntr", [](Cpu &c) {
        enable_interrupt(arg(c, 0), false);
        if (arg(c, 1)) write32(arg(c, 1), arg(c, 0));
        c.r[2] = 0;
    });
    def("intrman", 8, "CpuDisableIntr", [](Cpu &c) { interrupt_state(true); c.r[2] = 0; });
    def("intrman", 9, "CpuEnableIntr", [](Cpu &c) { interrupt_state(false); c.r[2] = 0; });
    def("intrman", 17, "CpuSuspendIntr", [](Cpu &c) {
        const bool was = interrupts_suspended();
        if (arg(c, 0)) write32(arg(c, 0), was ? 0u : 1u);
        interrupt_state(true);
        c.r[2] = was ? static_cast<std::uint32_t>(-102) : 0u;
    });
    def("intrman", 18, "CpuResumeIntr", [](Cpu &c) {
        interrupt_state(arg(c, 0) == 0);
        c.r[2] = 0;
    });
    def("intrman", 23, "QueryIntrContext", [](Cpu &c) { c.r[2] = in_interrupt() ? 1u : 0u; });
}

void install_thbase() {
    register_hle_library("thbase", 0x0101);
    def("thbase", 4, "CreateThread", [](Cpu &c) {
        const std::uint32_t p = arg(c, 0);
        c.r[2] = static_cast<std::uint32_t>(create_thread(read32(p + 8), read32(p + 12), static_cast<int>(read32(p + 16)), c.r[28], read32(p), read32(p + 4)));
    });
    def("thbase", 5, "DeleteThread", [](Cpu &c) {
        Thread *t = thread(static_cast<int>(arg(c, 0)));
        if (!t) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownThread); return; }
        c.r[2] = delete_thread(t->id) ? 0u : static_cast<std::uint32_t>(-414);
    });
    def("thbase", 6, "StartThread", [](Cpu &c) {
        Thread *t = thread(static_cast<int>(arg(c, 0)));
        if (!t) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownThread); return; }
        start_thread(t->id, arg(c, 1));
        c.r[2] = 0;
    });
    def("thbase", 7, "StartThreadArgs", [](Cpu &c) {
        Thread *t = thread(static_cast<int>(arg(c, 0)));
        if (!t) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownThread); return; }
        start_thread(t->id, arg(c, 1), arg(c, 2));
        c.r[2] = 0;
    });
    auto exit_thread = [](Cpu &c) {
        Thread *t = current();
        if (!t) return;
        t->context = c;
        t->status = kDormant;
        auto on_exit = std::move(t->on_exit);
        t->on_exit = nullptr;
        if (on_exit) on_exit();
        reschedule();
    };
    def("thbase", 8, "ExitThread", exit_thread);
    def("thbase", 9, "ExitDeleteThread", [exit_thread](Cpu &c) {
        Thread *t = current();
        const int id = t ? t->id : 0;
        exit_thread(c);
        if (id) schedule(now() + 1, Job{kJobDeleteThread, static_cast<std::uint32_t>(id)});
    });
    auto terminate = [](Cpu &c) {
        Thread *t = thread(static_cast<int>(arg(c, 0)));
        if (t && t != current()) {
            t->status = kDormant;
            t->wait = kWaitNone;
        }
        c.r[2] = t ? 0u : static_cast<std::uint32_t>(kErrorUnknownThread);
    };
    def("thbase", 10, "TerminateThread", terminate);
    def("thbase", 11, "iTerminateThread", terminate);
    def("thbase", 12, "DisableDispatchThread", [](Cpu &c) { c.r[2] = 0; });
    def("thbase", 13, "EnableDispatchThread", [](Cpu &c) { c.r[2] = 0; });
    auto change_priority = [](Cpu &c) {
        Thread *t = thread(static_cast<int>(arg(c, 0)));
        if (!t) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownThread); return; }
        int prio = static_cast<int>(arg(c, 1));
        if (prio == 0) prio = t->init_priority;
        t->priority = prio;
        reschedule();
        c.r[2] = 0;
    };
    def("thbase", 14, "ChangeThreadPriority", change_priority);
    def("thbase", 15, "iChangeThreadPriority", change_priority);
    auto rotate = [](Cpu &c) {
        Thread *t = current();
        if (t && (arg(c, 0) == 0 || static_cast<int>(arg(c, 0)) == t->priority)) {
            t->order = ++g_rotate_order;
            reschedule();
        }
        c.r[2] = 0;
    };
    def("thbase", 16, "RotateThreadReadyQueue", rotate);
    def("thbase", 17, "iRotateThreadReadyQueue", rotate);
    auto release_wait = [](Cpu &c) {
        Thread *t = thread(static_cast<int>(arg(c, 0)));
        if (t) {
            for (auto &[id, s] : g_semas) s.waiters.erase(std::remove(s.waiters.begin(), s.waiters.end(), t->id), s.waiters.end());
            for (auto &[id, e] : g_events) e.waiters.erase(std::remove(e.waiters.begin(), e.waiters.end(), t->id), e.waiters.end());
            wake(*t, static_cast<std::uint32_t>(-418));
            reschedule();
        }
        c.r[2] = t ? 0u : static_cast<std::uint32_t>(kErrorUnknownThread);
    };
    def("thbase", 18, "ReleaseWaitThread", release_wait);
    def("thbase", 19, "iReleaseWaitThread", release_wait);
    def("thbase", 20, "GetThreadId", [](Cpu &c) { c.r[2] = current() ? static_cast<std::uint32_t>(current()->id) : 0u; });
    def("thbase", 21, "CheckThreadStack", [](Cpu &c) { c.r[2] = 0x1000; });
    auto refer = [](Cpu &c) {
        Thread *t = thread(static_cast<int>(arg(c, 0)));
        const std::uint32_t p = arg(c, 1);
        if (!t) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownThread); return; }
        if (p) {
            write32(p + 0, t->attr);
            write32(p + 4, t->option);
            write32(p + 8, static_cast<std::uint32_t>(t->status));
            write32(p + 12, t->entry);
            write32(p + 16, t->stack);
            write32(p + 20, t->stack_size);
            write32(p + 24, t->gp);
            write32(p + 28, static_cast<std::uint32_t>(t->init_priority));
            write32(p + 32, static_cast<std::uint32_t>(t->priority));
            write32(p + 36, static_cast<std::uint32_t>(t->wait));
            write32(p + 40, static_cast<std::uint32_t>(t->wait_id));
            write32(p + 44, static_cast<std::uint32_t>(t->wakeups));
        }
        c.r[2] = 0;
    };
    def("thbase", 22, "ReferThreadStatus", refer);
    def("thbase", 23, "iReferThreadStatus", refer);
    def("thbase", 24, "SleepThread", [](Cpu &c) {
        Thread *t = current();
        c.r[2] = 0;
        if (t && t->wakeups > 0) { --t->wakeups; return; }
        block(c, kWaitSleep, 0);
    });
    auto wakeup = [](Cpu &c) {
        Thread *t = thread(static_cast<int>(arg(c, 0)));
        if (!t) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownThread); return; }
        if (t->status == kWait && t->wait == kWaitSleep) {
            wake(*t, 0);
            reschedule();
        } else {
            ++t->wakeups;
        }
        c.r[2] = 0;
    };
    def("thbase", 25, "WakeupThread", wakeup);
    def("thbase", 26, "iWakeupThread", wakeup);
    auto cancel_wakeup = [](Cpu &c) {
        Thread *t = thread(static_cast<int>(arg(c, 0)));
        c.r[2] = t ? static_cast<std::uint32_t>(t->wakeups) : static_cast<std::uint32_t>(kErrorUnknownThread);
        if (t) t->wakeups = 0;
    };
    def("thbase", 27, "CancelWakeupThread", cancel_wakeup);
    def("thbase", 28, "iCancelWakeupThread", cancel_wakeup);
    def("thbase", 29, "SuspendThread", [](Cpu &c) { c.r[2] = 0; });
    def("thbase", 31, "ResumeThread", [](Cpu &c) { c.r[2] = 0; });
    def("thbase", 33, "DelayThread", [](Cpu &c) {
        Thread *t = current();
        c.r[2] = 0;
        if (!t) return;
        const int id = t->id;
        block(c, kWaitDelay, 0);
        schedule(now() + std::max<std::uint64_t>(1, usec_to_cycles(arg(c, 0))), Job{kJobDelayWake, static_cast<std::uint32_t>(id)});
    });
    def("thbase", 34, "GetSystemTime", [](Cpu &c) { write_clock(arg(c, 0), now()); c.r[2] = 0; });
    auto set_alarm = [](Cpu &c) {
        const std::uint64_t delay = read_clock(arg(c, 0));
        const std::uint64_t key = g_next_alarm++;
        g_alarms[key] = Alarm{arg(c, 1), arg(c, 2), true};
        schedule(now() + std::max<std::uint64_t>(1, delay), Job{kJobAlarm, static_cast<std::uint32_t>(key), static_cast<std::uint32_t>(key >> 32)});
        c.r[2] = 0;
    };
    def("thbase", 35, "SetAlarm", set_alarm);
    def("thbase", 36, "iSetAlarm", set_alarm);
    auto cancel_alarm = [](Cpu &c) {
        for (auto &[k, a] : g_alarms)
            if (a.handler == arg(c, 0) && a.arg == arg(c, 1)) a.active = false;
        c.r[2] = 0;
    };
    def("thbase", 37, "CancelAlarm", cancel_alarm);
    def("thbase", 38, "iCancelAlarm", cancel_alarm);
    def("thbase", 39, "USec2SysClock", [](Cpu &c) { write_clock(arg(c, 1), usec_to_cycles(arg(c, 0))); c.r[2] = 0; });
    def("thbase", 40, "SysClock2USec", [](Cpu &c) {
        const std::uint64_t usec = read_clock(arg(c, 0)) * 1000u / 36864u;
        write32(arg(c, 1), static_cast<std::uint32_t>(usec / 1000000u));
        write32(arg(c, 2), static_cast<std::uint32_t>(usec % 1000000u));
        c.r[2] = 0;
    });
    def("thbase", 41, "GetSystemStatusFlag", [](Cpu &c) { c.r[2] = 0; });
    def("thbase", 43, "GetSystemTimeLow", [](Cpu &c) { c.r[2] = static_cast<std::uint32_t>(now()); });
}

void install_thsemap() {
    register_hle_library("thsemap", 0x0101);
    def("thsemap", 4, "CreateSema", [](Cpu &c) {
        const std::uint32_t p = arg(c, 0);
        const int id = g_next_object++;
        g_semas[id] = Sema{static_cast<int>(read32(p + 8)), static_cast<int>(read32(p + 12)), read32(p), read32(p + 4), {}};
        c.r[2] = static_cast<std::uint32_t>(id);
    });
    def("thsemap", 5, "DeleteSema", [](Cpu &c) {
        auto it = g_semas.find(static_cast<int>(arg(c, 0)));
        if (it == g_semas.end()) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownSema); return; }
        for (int t : it->second.waiters)
            if (Thread *th = thread(t)) wake(*th, static_cast<std::uint32_t>(-425));
        g_semas.erase(it);
        reschedule();
        c.r[2] = 0;
    });
    def("thsemap", 6, "SignalSema", [](Cpu &c) { signal_sema(c, static_cast<int>(arg(c, 0)), false); });
    def("thsemap", 7, "iSignalSema", [](Cpu &c) { signal_sema(c, static_cast<int>(arg(c, 0)), true); });
    def("thsemap", 8, "WaitSema", [](Cpu &c) {
        auto it = g_semas.find(static_cast<int>(arg(c, 0)));
        if (it == g_semas.end()) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownSema); return; }
        c.r[2] = 0;
        if (it->second.count > 0) { --it->second.count; return; }
        it->second.waiters.push_back(current() ? current()->id : 0);
        block(c, kWaitSema, it->first);
    });
    def("thsemap", 9, "PollSema", [](Cpu &c) {
        auto it = g_semas.find(static_cast<int>(arg(c, 0)));
        if (it == g_semas.end()) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownSema); return; }
        if (it->second.count > 0) { --it->second.count; c.r[2] = 0; }
        else c.r[2] = static_cast<std::uint32_t>(kErrorSemaZero);
    });
    auto refer = [](Cpu &c) {
        auto it = g_semas.find(static_cast<int>(arg(c, 0)));
        if (it == g_semas.end()) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownSema); return; }
        const std::uint32_t p = arg(c, 1);
        write32(p + 0, it->second.attr);
        write32(p + 4, it->second.option);
        write32(p + 8, 0);
        write32(p + 12, static_cast<std::uint32_t>(it->second.max));
        write32(p + 16, static_cast<std::uint32_t>(it->second.count));
        write32(p + 20, static_cast<std::uint32_t>(it->second.waiters.size()));
        c.r[2] = 0;
    };
    def("thsemap", 11, "ReferSemaStatus", refer);
    def("thsemap", 12, "iReferSemaStatus", refer);
}

void install_thevent() {
    register_hle_library("thevent", 0x0101);
    def("thevent", 4, "CreateEventFlag", [](Cpu &c) {
        const std::uint32_t p = arg(c, 0);
        const int id = g_next_object++;
        g_events[id] = EventFlag{read32(p + 8), read32(p), read32(p + 4), {}};
        c.r[2] = static_cast<std::uint32_t>(id);
    });
    def("thevent", 5, "DeleteEventFlag", [](Cpu &c) {
        auto it = g_events.find(static_cast<int>(arg(c, 0)));
        if (it == g_events.end()) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownEvent); return; }
        for (int t : it->second.waiters)
            if (Thread *th = thread(t)) wake(*th, static_cast<std::uint32_t>(-425));
        g_events.erase(it);
        c.r[2] = 0;
    });
    def("thevent", 6, "SetEventFlag", [](Cpu &c) { set_event(c, static_cast<int>(arg(c, 0)), arg(c, 1), false); });
    def("thevent", 7, "iSetEventFlag", [](Cpu &c) { set_event(c, static_cast<int>(arg(c, 0)), arg(c, 1), true); });
    auto clear = [](Cpu &c) {
        auto it = g_events.find(static_cast<int>(arg(c, 0)));
        if (it == g_events.end()) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownEvent); return; }
        it->second.bits &= arg(c, 1);
        c.r[2] = 0;
    };
    def("thevent", 8, "ClearEventFlag", clear);
    def("thevent", 9, "iClearEventFlag", clear);
    def("thevent", 10, "WaitEventFlag", [](Cpu &c) {
        auto it = g_events.find(static_cast<int>(arg(c, 0)));
        if (it == g_events.end()) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownEvent); return; }
        EventFlag &e = it->second;
        c.r[2] = 0;
        if (event_matches(e, arg(c, 1), arg(c, 2))) {
            event_take(e, arg(c, 1), arg(c, 2), arg(c, 3));
            return;
        }
        Thread *t = current();
        if (!t) return;
        t->event_bits = arg(c, 1);
        t->event_mode = arg(c, 2);
        t->event_result = arg(c, 3);
        e.waiters.push_back(t->id);
        block(c, kWaitEvent, it->first);
    });
    auto poll = [](Cpu &c) {
        auto it = g_events.find(static_cast<int>(arg(c, 0)));
        if (it == g_events.end()) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownEvent); return; }
        if (event_matches(it->second, arg(c, 1), arg(c, 2))) {
            event_take(it->second, arg(c, 1), arg(c, 2), arg(c, 3));
            c.r[2] = 0;
        } else {
            c.r[2] = static_cast<std::uint32_t>(kErrorEventCondition);
        }
    };
    def("thevent", 11, "PollEventFlag", poll);
    def("thevent", 12, "iPollEventFlag", poll);
    auto refer = [](Cpu &c) {
        auto it = g_events.find(static_cast<int>(arg(c, 0)));
        if (it == g_events.end()) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownEvent); return; }
        const std::uint32_t p = arg(c, 1);
        write32(p + 0, it->second.attr);
        write32(p + 4, it->second.option);
        write32(p + 8, 0);
        write32(p + 12, it->second.bits);
        write32(p + 16, static_cast<std::uint32_t>(it->second.waiters.size()));
        c.r[2] = 0;
    };
    def("thevent", 13, "ReferEventFlagStatus", refer);
    def("thevent", 14, "iReferEventFlagStatus", refer);
}

void install_thmsgbx() {
    register_hle_library("thmsgbx", 0x0101);
    def("thmsgbx", 4, "CreateMbx", [](Cpu &c) {
        const std::uint32_t p = arg(c, 0);
        const int id = g_next_object++;
        g_mbxs[id] = Mbx{read32(p), read32(p + 4), {}, {}};
        c.r[2] = static_cast<std::uint32_t>(id);
    });
    def("thmsgbx", 5, "DeleteMbx", [](Cpu &c) { g_mbxs.erase(static_cast<int>(arg(c, 0))); c.r[2] = 0; });
    def("thmsgbx", 6, "SendMbx", [](Cpu &c) { send_mbx(c, static_cast<int>(arg(c, 0)), arg(c, 1), false); });
    def("thmsgbx", 7, "iSendMbx", [](Cpu &c) { send_mbx(c, static_cast<int>(arg(c, 0)), arg(c, 1), true); });
    def("thmsgbx", 8, "ReceiveMbx", [](Cpu &c) {
        auto it = g_mbxs.find(static_cast<int>(arg(c, 1)));
        if (it == g_mbxs.end()) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownMbx); return; }
        c.r[2] = 0;
        if (!it->second.messages.empty()) {
            write32(arg(c, 0), it->second.messages.front());
            it->second.messages.pop_front();
            return;
        }
        Thread *t = current();
        if (!t) return;
        t->message_out = arg(c, 0);
        it->second.waiters.push_back(t->id);
        block(c, kWaitMbx, it->first);
    });
    def("thmsgbx", 9, "PollMbx", [](Cpu &c) {
        auto it = g_mbxs.find(static_cast<int>(arg(c, 1)));
        if (it == g_mbxs.end()) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownMbx); return; }
        if (it->second.messages.empty()) { c.r[2] = static_cast<std::uint32_t>(kErrorMbxNone); return; }
        write32(arg(c, 0), it->second.messages.front());
        it->second.messages.pop_front();
        c.r[2] = 0;
    });
}

void install_thfpool() {
    register_hle_library("thfpool", 0x0101);
    def("thfpool", 4, "CreateFpl", [](Cpu &c) {
        const std::uint32_t p = arg(c, 0);
        Fpl f{read32(p), read32(p + 4), (read32(p + 8) + 3u) & ~3u, read32(p + 12), 0, {}, {}};
        f.base = alloc(f.block_size * f.blocks);
        if (!f.base) { c.r[2] = static_cast<std::uint32_t>(kErrorNoMemory); return; }
        for (std::uint32_t i = f.blocks; i-- > 0;) f.free.push_back(f.base + i * f.block_size);
        const int id = g_next_object++;
        g_fpls[id] = f;
        c.r[2] = static_cast<std::uint32_t>(id);
    });
    def("thfpool", 5, "DeleteFpl", [](Cpu &c) {
        auto it = g_fpls.find(static_cast<int>(arg(c, 0)));
        if (it != g_fpls.end()) { free(it->second.base); g_fpls.erase(it); }
        c.r[2] = 0;
    });
    def("thfpool", 6, "AllocateFpl", [](Cpu &c) {
        auto it = g_fpls.find(static_cast<int>(arg(c, 0)));
        if (it == g_fpls.end()) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownFpl); return; }
        if (!it->second.free.empty()) { c.r[2] = it->second.free.back(); it->second.free.pop_back(); return; }
        it->second.waiters.push_back(current() ? current()->id : 0);
        block(c, kWaitFpl, it->first);
    });
    auto poll = [](Cpu &c) {
        auto it = g_fpls.find(static_cast<int>(arg(c, 0)));
        if (it == g_fpls.end()) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownFpl); return; }
        if (it->second.free.empty()) { c.r[2] = 0; return; }
        c.r[2] = it->second.free.back();
        it->second.free.pop_back();
    };
    def("thfpool", 7, "pAllocateFpl", poll);
    def("thfpool", 8, "ipAllocateFpl", poll);
    def("thfpool", 9, "FreeFpl", [](Cpu &c) {
        auto it = g_fpls.find(static_cast<int>(arg(c, 0)));
        if (it == g_fpls.end()) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownFpl); return; }
        if (!it->second.waiters.empty()) {
            const int t = it->second.waiters.front();
            it->second.waiters.pop_front();
            if (Thread *th = thread(t)) wake(*th, arg(c, 1));
            reschedule();
        } else {
            it->second.free.push_back(arg(c, 1));
        }
        c.r[2] = 0;
    });
}

void install_thvpool() {
    register_hle_library("thvpool", 0x0101);
    def("thvpool", 4, "CreateVpl", [](Cpu &c) {
        const std::uint32_t p = arg(c, 0);
        Vpl v{};
        v.attr = read32(p);
        v.option = read32(p + 4);
        v.size = read32(p + 8);
        v.size = (v.size + 0xFFu) & ~0xFFu;
        if (!v.size) v.size = 0x100u;
        v.base = alloc(v.size);
        if (!v.base) { c.r[2] = static_cast<std::uint32_t>(kErrorNoMemory); return; }
        const int id = g_next_object++;
        g_vpls[id] = v;
        c.r[2] = static_cast<std::uint32_t>(id);
    });
    def("thvpool", 5, "DeleteVpl", [](Cpu &c) {
        auto it = g_vpls.find(static_cast<int>(arg(c, 0)));
        if (it != g_vpls.end()) { free(it->second.base); g_vpls.erase(it); }
        c.r[2] = 0;
    });
    const auto allocate = [](Cpu &c, bool wait) {
        auto it = g_vpls.find(static_cast<int>(arg(c, 0)));
        if (it == g_vpls.end()) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownVpl); return; }
        Vpl &v = it->second;
        const std::uint32_t size = (arg(c, 1) + 3u) & ~3u;
        std::uint32_t at = v.base;
        for (const auto &[a, s] : v.used) {
            if (a > at && a - at >= size) break;
            at = a + s;
        }
        if (size <= v.size && at - v.base + size <= v.size) { v.used[at] = size; c.r[2] = at; return; }
        if (!wait) { c.r[2] = 0; return; }
        v.waiters.push_back(current() ? current()->id : 0);
        block(c, kWaitVpl, it->first);
    };
    def("thvpool", 6, "AllocateVpl", [allocate](Cpu &c) { allocate(c, true); });
    def("thvpool", 7, "pAllocateVpl", [allocate](Cpu &c) { allocate(c, false); });
    def("thvpool", 8, "ipAllocateVpl", [allocate](Cpu &c) { allocate(c, false); });
    def("thvpool", 9, "FreeVpl", [](Cpu &c) {
        auto it = g_vpls.find(static_cast<int>(arg(c, 0)));
        if (it == g_vpls.end()) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownVpl); return; }
        const std::uint32_t block_addr = arg(c, 1);
        it->second.used.erase(block_addr);
        if (!it->second.waiters.empty()) {
            const int t = it->second.waiters.front();
            it->second.waiters.pop_front();
            if (Thread *th = thread(t)) wake(*th, block_addr);
            reschedule();
        }
        c.r[2] = 0;
    });
    def("thvpool", 11, "ReferVplStatus", [](Cpu &c) {
        auto it = g_vpls.find(static_cast<int>(arg(c, 0)));
        if (it == g_vpls.end()) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownVpl); return; }
        std::uint32_t used = 0;
        for (const auto &[a, s] : it->second.used) used += s;
        const std::uint32_t p = arg(c, 1);
        write32(p + 0, it->second.attr);
        write32(p + 4, used);
        write32(p + 8, it->second.size - used);
        c.r[2] = 0;
    });
}

void install_sysclib() {
    register_hle_library("sysclib", 0x0103);
    def("sysclib", 4, "setjmp", [](Cpu &c) {
        const std::uint32_t p = arg(c, 0);
        const int regs[12] = {31, 29, 30, 16, 17, 18, 19, 20, 21, 22, 23, 28};
        for (int i = 0; i < 12; ++i) write32(p + 4u * static_cast<std::uint32_t>(i), c.r[regs[i]]);
        c.r[2] = 0;
    });
    def("sysclib", 5, "longjmp", [](Cpu &c) {
        const std::uint32_t p = arg(c, 0), value = arg(c, 1);
        const int regs[12] = {31, 29, 30, 16, 17, 18, 19, 20, 21, 22, 23, 28};
        for (int i = 0; i < 12; ++i) c.r[regs[i]] = read32(p + 4u * static_cast<std::uint32_t>(i));
        c.pc = c.r[31];
        c.npc = c.pc + 4;
        c.r[2] = value ? value : 1u;
    });
    def("sysclib", 6, "_toupper", [](Cpu &c) { c.r[2] = static_cast<std::uint32_t>(std::toupper(static_cast<int>(arg(c, 0) & 0xFF))); });
    def("sysclib", 7, "_tolower", [](Cpu &c) { c.r[2] = static_cast<std::uint32_t>(std::tolower(static_cast<int>(arg(c, 0) & 0xFF))); });
    def("sysclib", 8, "look_ctype_table", [](Cpu &c) {
        const int ch = static_cast<int>(arg(c, 0) & 0xFF);
        std::uint32_t t = 0;
        if (std::isupper(ch)) t |= 1;
        if (std::islower(ch)) t |= 2;
        if (std::isdigit(ch)) t |= 4;
        if (std::isspace(ch)) t |= 8;
        if (std::ispunct(ch)) t |= 16;
        if (std::iscntrl(ch)) t |= 32;
        if (std::isxdigit(ch)) t |= 64;
        c.r[2] = t;
    });
    def("sysclib", 10, "memchr", [](Cpu &c) {
        for (std::uint32_t i = 0; i < arg(c, 2); ++i)
            if (read8(arg(c, 0) + i) == (arg(c, 1) & 0xFF)) { c.r[2] = arg(c, 0) + i; return; }
        c.r[2] = 0;
    });
    def("sysclib", 11, "memcmp", [](Cpu &c) {
        for (std::uint32_t i = 0; i < arg(c, 2); ++i) {
            const int d = read8(arg(c, 0) + i) - read8(arg(c, 1) + i);
            if (d) { c.r[2] = static_cast<std::uint32_t>(d); return; }
        }
        c.r[2] = 0;
    });
    auto copy = [](std::uint32_t dst, std::uint32_t src, std::uint32_t n) {
        if (phys(dst) + n <= kRamSize && phys(src) + n <= kRamSize) std::memmove(ram_ptr(dst), ram_ptr(src), n);
        else if (dst < src) for (std::uint32_t i = 0; i < n; ++i) write8(dst + i, read8(src + i));
        else for (std::uint32_t i = n; i-- > 0;) write8(dst + i, read8(src + i));
    };
    def("sysclib", 12, "memcpy", [copy](Cpu &c) { copy(arg(c, 0), arg(c, 1), arg(c, 2)); c.r[2] = arg(c, 0); });
    def("sysclib", 13, "memmove", [copy](Cpu &c) { copy(arg(c, 0), arg(c, 1), arg(c, 2)); c.r[2] = arg(c, 0); });
    def("sysclib", 14, "memset", [](Cpu &c) {
        if (phys(arg(c, 0)) + arg(c, 2) <= kRamSize) std::memset(ram_ptr(arg(c, 0)), static_cast<int>(arg(c, 1) & 0xFF), arg(c, 2));
        else for (std::uint32_t i = 0; i < arg(c, 2); ++i) write8(arg(c, 0) + i, static_cast<std::uint8_t>(arg(c, 1)));
        c.r[2] = arg(c, 0);
    });
    def("sysclib", 15, "bcmp", [](Cpu &c) {
        c.r[2] = std::memcmp(ram_ptr(arg(c, 0)), ram_ptr(arg(c, 1)), arg(c, 2)) ? 1u : 0u;
    });
    def("sysclib", 16, "bcopy", [copy](Cpu &c) { copy(arg(c, 1), arg(c, 0), arg(c, 2)); });
    def("sysclib", 17, "bzero", [](Cpu &c) { std::memset(ram_ptr(arg(c, 0)), 0, arg(c, 1)); });
    def("sysclib", 19, "sprintf", [](Cpu &c) {
        const std::string s = format(c, arg(c, 1), 2);
        std::memcpy(ram_ptr(arg(c, 0)), s.c_str(), s.size() + 1);
        c.r[2] = static_cast<std::uint32_t>(s.size());
    });
    def("sysclib", 20, "strcat", [](Cpu &c) {
        const std::string a = ram_string(arg(c, 0), 4096), b = ram_string(arg(c, 1), 4096);
        std::memcpy(ram_ptr(arg(c, 0) + static_cast<std::uint32_t>(a.size())), b.c_str(), b.size() + 1);
        c.r[2] = arg(c, 0);
    });
    def("sysclib", 21, "strchr", [](Cpu &c) {
        for (std::uint32_t p = arg(c, 0);; ++p) {
            const std::uint8_t ch = read8(p);
            if (ch == (arg(c, 1) & 0xFF)) { c.r[2] = p; return; }
            if (!ch) { c.r[2] = 0; return; }
        }
    });
    def("sysclib", 22, "strcmp", [](Cpu &c) {
        const int r = std::strcmp(ram_string(arg(c, 0), 4096).c_str(), ram_string(arg(c, 1), 4096).c_str());
        c.r[2] = static_cast<std::uint32_t>(r < 0 ? -1 : r > 0 ? 1 : 0);
    });
    def("sysclib", 23, "strcpy", [](Cpu &c) {
        const std::string s = ram_string(arg(c, 1), 4096);
        std::memcpy(ram_ptr(arg(c, 0)), s.c_str(), s.size() + 1);
        c.r[2] = arg(c, 0);
    });
    def("sysclib", 25, "index", [](Cpu &c) {
        const std::string s = ram_string(arg(c, 0), 4096);
        const auto pos = s.find(static_cast<char>(arg(c, 1)));
        c.r[2] = pos == std::string::npos ? 0u : arg(c, 0) + static_cast<std::uint32_t>(pos);
    });
    def("sysclib", 27, "strlen", [](Cpu &c) { c.r[2] = static_cast<std::uint32_t>(ram_string(arg(c, 0), 65536).size()); });
    def("sysclib", 28, "strncat", [](Cpu &c) {
        const std::string a = ram_string(arg(c, 0), 4096), b = ram_string(arg(c, 1), arg(c, 2));
        std::memcpy(ram_ptr(arg(c, 0) + static_cast<std::uint32_t>(a.size())), b.c_str(), b.size() + 1);
        c.r[2] = arg(c, 0);
    });
    def("sysclib", 29, "strncmp", [](Cpu &c) {
        const int r = std::strncmp(ram_string(arg(c, 0), 4096).c_str(), ram_string(arg(c, 1), 4096).c_str(), arg(c, 2));
        c.r[2] = static_cast<std::uint32_t>(r < 0 ? -1 : r > 0 ? 1 : 0);
    });
    def("sysclib", 30, "strncpy", [](Cpu &c) {
        const std::string s = ram_string(arg(c, 1), arg(c, 2));
        for (std::uint32_t i = 0; i < arg(c, 2); ++i) write8(arg(c, 0) + i, i < s.size() ? static_cast<std::uint8_t>(s[i]) : 0);
        c.r[2] = arg(c, 0);
    });
    def("sysclib", 32, "strrchr", [](Cpu &c) {
        const std::string s = ram_string(arg(c, 0), 4096);
        const auto pos = s.rfind(static_cast<char>(arg(c, 1)));
        c.r[2] = pos == std::string::npos ? 0u : arg(c, 0) + static_cast<std::uint32_t>(pos);
    });
    def("sysclib", 34, "strstr", [](Cpu &c) {
        const std::string s = ram_string(arg(c, 0), 4096), n = ram_string(arg(c, 1), 4096);
        const auto pos = s.find(n);
        c.r[2] = pos == std::string::npos ? 0u : arg(c, 0) + static_cast<std::uint32_t>(pos);
    });
    def("sysclib", 35, "strtok", [](Cpu &c) {
        std::uint32_t p = arg(c, 0) ? arg(c, 0) : g_strtok_next;
        const std::string delim = ram_string(arg(c, 1), 256);
        const auto is_delim = [&delim](std::uint8_t ch) { return ch && delim.find(static_cast<char>(ch)) != std::string::npos; };
        while (p && is_delim(read8(p))) ++p;
        if (!p || !read8(p)) {
            g_strtok_next = 0;
            c.r[2] = 0;
            return;
        }
        const std::uint32_t token = p;
        while (read8(p) && !is_delim(read8(p))) ++p;
        if (read8(p)) {
            write8(p, 0);
            g_strtok_next = p + 1;
        } else {
            g_strtok_next = p;
        }
        c.r[2] = token;
    });
    def("sysclib", 36, "strtol", [](Cpu &c) {
        const std::string s = ram_string(arg(c, 0), 256);
        char *end = nullptr;
        const long v = std::strtol(s.c_str(), &end, static_cast<int>(arg(c, 2)));
        if (arg(c, 1)) write32(arg(c, 1), arg(c, 0) + static_cast<std::uint32_t>(end - s.c_str()));
        c.r[2] = static_cast<std::uint32_t>(v);
    });
    def("sysclib", 38, "strtoul", [](Cpu &c) {
        const std::string s = ram_string(arg(c, 0), 256);
        char *end = nullptr;
        const unsigned long v = std::strtoul(s.c_str(), &end, static_cast<int>(arg(c, 2)));
        if (arg(c, 1)) write32(arg(c, 1), arg(c, 0) + static_cast<std::uint32_t>(end - s.c_str()));
        c.r[2] = static_cast<std::uint32_t>(v);
    });
    def("sysclib", 40, "_wmemcopy", [copy](Cpu &c) { copy(arg(c, 0), arg(c, 1), arg(c, 2)); c.r[2] = arg(c, 0); });
    def("sysclib", 41, "_wmemset", [](Cpu &c) {
        for (std::uint32_t i = 0; i < arg(c, 2); i += 4) write32(arg(c, 0) + i, arg(c, 1));
        c.r[2] = arg(c, 0);
    });
    def("sysclib", 42, "vsprintf", [](Cpu &c) {
        const std::uint32_t list = arg(c, 2);
        std::string out;
        const std::string fmt = ram_string(arg(c, 1), 1024);
        std::uint32_t ap = list;
        for (std::size_t i = 0; i < fmt.size(); ++i) {
            if (fmt[i] != '%') { out += fmt[i]; continue; }
            std::string spec = "%";
            ++i;
            while (i < fmt.size() && std::strchr("-+ #0.0123456789", fmt[i])) spec += fmt[i++];
            while (i < fmt.size() && (fmt[i] == 'l' || fmt[i] == 'h')) ++i;
            if (i >= fmt.size()) break;
            char buf[512];
            const char conv = fmt[i];
            if (conv == '%') { out += '%'; continue; }
            const std::uint32_t v = read32(ap);
            ap += 4;
            if (conv == 's') std::snprintf(buf, sizeof buf, (spec + "s").c_str(), ram_string(v).c_str());
            else if (conv == 'd' || conv == 'i') std::snprintf(buf, sizeof buf, (spec + "d").c_str(), static_cast<int>(v));
            else if (conv == 'c') std::snprintf(buf, sizeof buf, (spec + "c").c_str(), static_cast<int>(v));
            else std::snprintf(buf, sizeof buf, (spec + conv).c_str(), v);
            out += buf;
        }
        std::memcpy(ram_ptr(arg(c, 0)), out.c_str(), out.size() + 1);
        c.r[2] = static_cast<std::uint32_t>(out.size());
    });
}

void install_stdio() {
    register_hle_library("stdio", 0x0102);
    def("stdio", 4, "printf", [](Cpu &c) {
        const std::string s = format(c, arg(c, 0), 1);
        print(s);
        c.r[2] = static_cast<std::uint32_t>(s.size());
    });
    def("stdio", 6, "putchar", [](Cpu &c) { print(std::string(1, static_cast<char>(arg(c, 0)))); c.r[2] = arg(c, 0); });
    def("stdio", 7, "puts", [](Cpu &c) { print(ram_string(arg(c, 0), 4096) + "\n"); c.r[2] = 0; });
}

void install_modload() {
    register_hle_library("modload", 0x0102);
    def("modload", 13, "SetCheckKelfPathCallback", [](Cpu &c) { c.r[2] = 0; });
    def("modload", 16, "GetModuleIdList", [](Cpu &c) {
        std::uint32_t n = 0;
        for (const auto &[id, m] : modules()) {
            if (n < arg(c, 1)) write32(arg(c, 0) + 4 * n, static_cast<std::uint32_t>(id));
            ++n;
        }
        if (arg(c, 2)) write32(arg(c, 2), n);
        c.r[2] = 0;
    });
    def("modload", 18, "GetModuleIdListByName", [](Cpu &c) {
        std::string name = ram_string(arg(c, 0), 64);
        for (char &ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        std::uint32_t n = 0;
        for (const auto &[id, m] : modules()) {
            std::string a = m.name;
            for (char &ch : a) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (a != name) continue;
            if (n < arg(c, 2)) write32(arg(c, 1) + 4 * n, static_cast<std::uint32_t>(id));
            ++n;
        }
        if (arg(c, 3)) write32(arg(c, 3), n);
        c.r[2] = 0;
    });
    def("modload", 17, "ReferModuleStatus", [](Cpu &c) {
        auto it = modules().find(static_cast<int>(arg(c, 0)));
        if (it == modules().end()) { c.r[2] = static_cast<std::uint32_t>(kErrorUnknownModule); return; }
        const std::uint32_t p = arg(c, 1);
        std::memset(ram_ptr(p), 0, 0x50);
        std::memcpy(ram_ptr(p), it->second.name.c_str(), std::min<std::size_t>(it->second.name.size(), 55));
        write32(p + 0x38, static_cast<std::uint32_t>(it->first));
        write32(p + 0x40, it->second.entry);
        write32(p + 0x44, it->second.gp);
        write32(p + 0x48, it->second.base);
        write32(p + 0x4C, it->second.size);
        c.r[2] = 0;
    });
    def("modload", 22, "SearchModuleByName", [](Cpu &c) {
        std::string name = ram_string(arg(c, 0), 64);
        for (const auto &[id, m] : modules()) {
            std::string a = m.name, b = name;
            for (char &ch : a) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            for (char &ch : b) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (a == b) { c.r[2] = static_cast<std::uint32_t>(id); return; }
        }
        c.r[2] = static_cast<std::uint32_t>(kErrorUnknownModule);
    });
}

void install_vblank() {
    register_hle_library("vblank", 0x0101);
    def("vblank", 4, "WaitVblankStart", [](Cpu &c) {
        c.r[2] = 0;
        if (!current()) return;
        g_vblank_start_waiters.push_back(current()->id);
        block(c, kWaitVblank, 0);
    });
    def("vblank", 5, "WaitVblankEnd", [](Cpu &c) {
        c.r[2] = 0;
        if (!current()) return;
        g_vblank_end_waiters.push_back(current()->id);
        block(c, kWaitVblank, 1);
    });
    def("vblank", 6, "WaitVblank", [](Cpu &c) {
        c.r[2] = 0;
        if (!current()) return;
        g_vblank_start_waiters.push_back(current()->id);
        block(c, kWaitVblank, 0);
    });
    def("vblank", 7, "WaitNonVblank", [](Cpu &c) {
        c.r[2] = 0;
        if (!current()) return;
        g_vblank_end_waiters.push_back(current()->id);
        block(c, kWaitVblank, 1);
    });
    def("vblank", 8, "RegisterVblankHandler", [](Cpu &c) {
        g_vblank_handlers.push_back(VblankHandler{static_cast<int>(arg(c, 0)), arg(c, 2), arg(c, 3)});
        c.r[2] = 0;
    });
    def("vblank", 9, "ReleaseVblankHandler", [](Cpu &c) {
        g_vblank_handlers.erase(std::remove_if(g_vblank_handlers.begin(), g_vblank_handlers.end(), [&](const VblankHandler &h) {
            return h.edge == static_cast<int>(arg(c, 0)) && h.handler == arg(c, 1);
        }), g_vblank_handlers.end());
        c.r[2] = 0;
    });
}

void install_timrman() {
    register_hle_library("timrman", 0x0101);
    def("timrman", 4, "AllocHardTimer", [](Cpu &c) { c.r[2] = g_next_hard_timer++; });
    def("timrman", 5, "ReferHardTimer", [](Cpu &c) { c.r[2] = 1; });
    def("timrman", 6, "FreeHardTimer", [](Cpu &c) { c.r[2] = 0; });
    def("timrman", 7, "SetTimerMode", [](Cpu &c) { c.r[2] = 0; });
    def("timrman", 9, "SetTimerCounter", [](Cpu &c) {
        const auto it = g_hard_timers.find(arg(c, 0));
        if (it != g_hard_timers.end()) g_hard_timer_start[arg(c, 0)] = now() - std::min<std::uint64_t>(now(), static_cast<std::uint64_t>(arg(c, 1)) * timer_cycles(it->second, 1));
        c.r[2] = 0;
    });
    def("timrman", 10, "GetTimerCounter", [](Cpu &c) { c.r[2] = hard_timer_counter(arg(c, 0)); });
    def("timrman", 11, "SetTimerCompare", [](Cpu &c) { c.r[2] = 0; });
    def("timrman", 16, "GetHardTimerIntrCode", [](Cpu &c) { c.r[2] = 14; });
    def("timrman", 20, "SetTimerHandler", [](Cpu &c) {
        HardTimer &t = g_hard_timers[arg(c, 0)];
        t.compare = arg(c, 1);
        t.handler = arg(c, 2);
        t.common = arg(c, 3);
        if (t.running) arm_hard_timer(arg(c, 0));
        c.r[2] = 0;
    });
    def("timrman", 22, "SetupHardTimer", [](Cpu &c) {
        HardTimer &t = g_hard_timers[arg(c, 0)];
        t.source = arg(c, 1);
        t.prescale = arg(c, 3);
        c.r[2] = 0;
    });
    def("timrman", 23, "StartHardTimer", [](Cpu &c) {
        g_hard_timers[arg(c, 0)].running = true;
        g_hard_timer_start[arg(c, 0)] = now();
        arm_hard_timer(arg(c, 0));
        c.r[2] = 0;
    });
    def("timrman", 24, "StopHardTimer", [](Cpu &c) {
        HardTimer &t = g_hard_timers[arg(c, 0)];
        t.running = false;
        ++t.generation;
        c.r[2] = 0;
    });
}

std::uint32_t dma_register(std::uint32_t ch, std::uint32_t reg) {
    return (ch < 7 ? 0x1F801080u + ch * 0x10u : 0x1F801500u + (ch - 7) * 0x10u) + reg * 4u;
}

std::uint32_t g_dma_direction[16];

void install_dmacman() {
    register_hle_library("dmacman", 0x0101);
    def("dmacman", 16, "dmac_set_dpcr2", [](Cpu &c) { c.r[2] = 0; });
    def("dmacman", 17, "dmac_get_dpcr2", [](Cpu &c) { c.r[2] = 0; });
    def("dmacman", 28, "sceSetSliceDMA", [](Cpu &c) {
        const std::uint32_t ch = arg(c, 0) & 15u;
        write32(dma_register(ch, 0), arg(c, 1));
        write32(dma_register(ch, 1), (arg(c, 3) << 16) | (arg(c, 2) & 0xFFFFu));
        g_dma_direction[ch] = arg(c, 4);
        c.r[2] = 1;
    });
    def("dmacman", 32, "sceStartDMA", [](Cpu &c) {
        const std::uint32_t ch = arg(c, 0) & 15u;
        write32(dma_register(ch, 2), 0x01000200u | (g_dma_direction[ch] & 1u));
        c.r[2] = 1;
    });
    def("dmacman", 33, "sceSetDMAPriority", [](Cpu &c) { c.r[2] = 1; });
    def("dmacman", 34, "sceEnableDMAChannel", [](Cpu &c) { c.r[2] = 1; });
    def("dmacman", 35, "sceDisableDMAChannel", [](Cpu &c) { c.r[2] = 1; });
}

void install_heaplib() {
    register_hle_library("heaplib", 0x0101);
    def("heaplib", 4, "CreateHeap", [](Cpu &c) {
        c.r[2] = kernel_alloc(16);
        g_heaps[c.r[2]];
    });
    def("heaplib", 5, "DeleteHeap", [](Cpu &c) {
        auto it = g_heaps.find(arg(c, 0));
        if (it != g_heaps.end()) {
            for (std::uint32_t block : it->second) free(block);
            g_heaps.erase(it);
        }
        c.r[2] = 0;
    });
    def("heaplib", 6, "AllocHeapMemory", [](Cpu &c) {
        c.r[2] = alloc(arg(c, 1));
        if (c.r[2]) g_heaps[arg(c, 0)].insert(c.r[2]);
    });
    def("heaplib", 7, "FreeHeapMemory", [](Cpu &c) {
        free(arg(c, 1));
        g_heaps[arg(c, 0)].erase(arg(c, 1));
        c.r[2] = 0;
    });
}

void install_misc() {
    register_hle_library("secrman", 0x0101);
    for (std::uint32_t i : {4u, 5u}) def("secrman", i, "secrman", [](Cpu &c) { c.r[2] = 0; });
    def("secrman", 6, "SecrAuthCard", [](Cpu &c) { c.r[2] = 1; });
}

}

void vblank_edge(bool start) {
    auto &waiters = start ? g_vblank_start_waiters : g_vblank_end_waiters;
    for (int id : waiters)
        if (Thread *t = thread(id)) wake(*t, 0);
    if (!waiters.empty()) reschedule();
    waiters.clear();
    for (const VblankHandler h : std::vector<VblankHandler>(g_vblank_handlers))
        if ((h.edge == 0) == start) call_guest(h.handler, h.arg);
}

void install_libraries() {
    g_semas.clear();
    g_events.clear();
    g_mbxs.clear();
    g_fpls.clear();
    g_heaps.clear();
    g_vblank_handlers.clear();
    g_vblank_start_waiters.clear();
    g_vblank_end_waiters.clear();
    g_alarms.clear();
    g_hard_timers.clear();
    g_hard_timer_start.clear();
    g_vpls.clear();
    g_next_object = 1;
    g_next_alarm = 1;
    g_rotate_order = 1ull << 40;
    g_strtok_next = 0;
    g_next_hard_timer = 1;
    register_job(kJobHardTimer, [](const Job &j) { fire_hard_timer(j.a, j.b | (static_cast<std::uint64_t>(j.c) << 32)); });
    register_job(kJobAlarm, [](const Job &j) { fire_alarm(j.a | (static_cast<std::uint64_t>(j.b) << 32)); });
    register_job(kJobDeleteThread, [](const Job &j) { delete_thread(static_cast<int>(j.a)); });
    register_job(kJobDelayWake, [](const Job &j) {
        Thread *th = thread(static_cast<int>(j.a));
        if (th && th->wait == kWaitDelay) wake(*th, 0);
    });
    install_sysmem();
    install_loadcore();
    install_intrman();
    install_thbase();
    install_thsemap();
    install_thevent();
    install_thmsgbx();
    install_thfpool();
    install_thvpool();
    install_sysclib();
    install_stdio();
    install_modload();
    install_vblank();
    install_timrman();
    install_dmacman();
    install_heaplib();
    install_misc();
}

void libs_state(ps2::state::Stream &s) {
    s.tag("ioplibs");
    s.map(g_semas, [&s](Sema &v) {
        s.pod(v.count);
        s.pod(v.max);
        s.pod(v.attr);
        s.pod(v.option);
        s.deque(v.waiters);
    });
    s.map(g_events, [&s](EventFlag &v) {
        s.pod(v.bits);
        s.pod(v.attr);
        s.pod(v.option);
        s.deque(v.waiters);
    });
    s.map(g_mbxs, [&s](Mbx &v) {
        s.pod(v.attr);
        s.pod(v.option);
        s.deque(v.messages);
        s.deque(v.waiters);
    });
    s.map(g_fpls, [&s](Fpl &v) {
        s.pod(v.attr);
        s.pod(v.option);
        s.pod(v.block_size);
        s.pod(v.blocks);
        s.pod(v.base);
        s.vec(v.free);
        s.deque(v.waiters);
    });
    s.map(g_heaps, [&s](std::set<std::uint32_t> &v) { s.set(v); });
    s.vec(g_vblank_handlers);
    s.vec(g_vblank_start_waiters);
    s.vec(g_vblank_end_waiters);
    s.map(g_alarms);
    s.map(g_hard_timers);
    s.pod(g_next_alarm);
    s.pod(g_next_object);
    s.pod(g_rotate_order);
    s.pod(g_strtok_next);
    s.pod(g_next_hard_timer);
    s.pod(g_dma_direction);
}

}

namespace ps2::iop {

void vblank(bool start) {
    hw::raise_interrupt(start ? 0u : 11u);
    kernel::vblank_edge(start);
}

}
