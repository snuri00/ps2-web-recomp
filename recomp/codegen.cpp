#include "codegen.hpp"

#include <algorithm>
#include <bit>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace recomp {

namespace {

namespace fs = std::filesystem;
using enum Op;

std::string hex(std::uint32_t v) { return std::format("0x{:08X}u", v); }
std::string label(std::uint32_t v) { return std::format("L_{:08X}", v); }
std::string w(std::uint32_t r) { return r ? std::format("c.r[{}].w[0]", r) : "0u"; }
std::string d(std::uint32_t r) { return r ? std::format("c.r[{}].d[0]", r) : "0ull"; }
std::string sd(std::uint32_t r) { return r ? std::format("c.r[{}].sd[0]", r) : "0ll"; }
std::string reg(std::uint32_t r) { return std::format("c.r[{}]", r); }
std::string f(std::uint32_t r) { return std::format("c.f[{}]", r); }
std::string vf(std::uint32_t r) { return std::format("c.vu0.vf[{}]", r); }
std::string vi(std::uint32_t r) { return std::format("c.vu0.vi[{}]", r); }

std::string offset(std::int32_t imm) {
    if (imm < 0) return std::format(" - {}u", -static_cast<std::int64_t>(imm));
    if (imm > 0) return std::format(" + {}u", imm);
    return "";
}

class Layout {
public:
    Layout(const Program &p, std::uint32_t bits) : bits_(bits) {
        origin_ = p.base & ~(size() - 1);
        count_ = ((p.end - origin_) + size() - 1) >> bits;
    }

    std::uint32_t size() const { return 1u << bits_; }
    std::uint32_t origin() const { return origin_; }
    std::uint32_t count() const { return count_; }
    std::uint32_t unit_of(std::uint32_t a) const { return (a - origin_) >> bits_; }
    std::uint32_t base_of(std::uint32_t u) const { return origin_ + (u << bits_); }

private:
    std::uint32_t bits_;
    std::uint32_t origin_{};
    std::uint32_t count_{};
};

void write_if_changed(const fs::path &path, const std::string &text) {
    std::ifstream in(path, std::ios::binary);
    if (in) {
        std::string old((std::istreambuf_iterator<char>(in)), {});
        if (old == text) return;
    }
    std::ofstream(path, std::ios::binary) << text;
}

struct UnitRow {
    std::uint32_t base;
    std::uint32_t size;
    std::string function;
    std::string table;
};

class Generator {
public:
    Generator(const Translation &t, const CodegenOptions &options)
        : image_(*t.image), p_(*t.program), links_(t.links), prefix_(t.prefix), relocatable_(t.relocatable), window_(t.fixed_module),
          template_(t.template_module), options_(options), layout_(*t.program, options.unit_bits) {}

    std::vector<UnitRow> run() {
        add_required_entries();
        assign_ids();
        std::vector<UnitRow> rows;
        for (std::uint32_t u = 0; u < layout_.count(); ++u)
            if (has_code_[u]) {
                write_unit(u);
                rows.push_back({layout_.base_of(u), layout_.size(), unit_name(u), table_name(u)});
            }
        return rows;
    }

private:
    std::string unit_name(std::uint32_t u) const { return std::format("{}unit_{:04X}", prefix_, u); }
    std::string table_name(std::uint32_t u) const { return std::format("{}entries_{:04X}", prefix_, u); }
    std::string pc(std::uint32_t a) const { return relocatable_ ? std::format("(B + {})", hex(a)) : hex(a); }
    std::string rebase() const { return relocatable_ ? "c.base = B; " : ""; }
    // a template's j/jal outside itself goes where its word says, the same in every place
    std::string target(const Insn &i, std::uint32_t t) const {
        return template_ && (i.op == J || i.op == Jal) && (t < p_.base || t >= p_.end) ? hex(t) : pc(t);
    }

    const Link *link_at(std::uint32_t a) const {
        if (!links_) return nullptr;
        const auto it = links_->find(a);
        return it == links_->end() ? nullptr : &it->second;
    }

    bool dynamic_jump(std::uint32_t a) const {
        const Link *l = link_at(a);
        return l && l->kind == Fix::Jump && (l->external || !relocatable_);
    }

    std::optional<std::string> field(const Insn &i) const {
        const Link *l = link_at(i.pc);
        if (!l || (l->kind != Fix::High && l->kind != Fix::Low)) return std::nullopt;
        if (l->external || !relocatable_) return std::format("static_cast<std::uint32_t>(ld16({}))", pc(i.pc));
        if (l->kind == Fix::High) return std::format("((B + {}) >> 16)", hex(l->value + 0x8000u));
        return std::format("(B + {})", hex(i.uimm()));
    }

    std::string imm(const Insn &i) const {
        if (const auto e = field(i)) return " + static_cast<std::uint32_t>(static_cast<std::int16_t>(" + *e + "))";
        return offset(i.simm());
    }

    std::string address(const Insn &i) const { return "(" + w(i.rs()) + imm(i) + ")"; }

    std::string uimm(const Insn &i) const {
        if (const auto e = field(i)) return "(static_cast<std::uint64_t>(" + *e + ") & 0xFFFFull)";
        return std::format("{}ull", i.uimm());
    }

    Insn at(std::uint32_t a) const { return decode(*image_.word(a), a); }

    void add_required_entries() {
        for (std::uint32_t a = p_.base; a < p_.end; a += 4) {
            if (!p_.is_code(a)) continue;
            const Insn i = at(a);
            const Flow fl = flow(i.op);
            if ((fl == Flow::Branch || fl == Flow::BranchLikely || fl == Flow::Jump || i.op == Jal) && !dynamic_jump(a)) {
                const std::uint32_t t = i.branch_target_of();
                if (p_.is_code(t) && layout_.unit_of(t) != layout_.unit_of(a)) p_.entries.insert(t);
            }
            const std::uint32_t next = a + 4;
            if (layout_.unit_of(next) != layout_.unit_of(a) && p_.is_code(next)) p_.entries.insert(next);
            if (has_delay_slot(i.op) && layout_.unit_of(a + 8) != layout_.unit_of(a) && p_.is_code(a + 8))
                p_.entries.insert(a + 8);
        }
    }

    void assign_ids() {
        ids_.assign(layout_.count(), {});
        has_code_.assign(layout_.count(), false);
        for (std::uint32_t a = p_.base; a < p_.end; a += 4)
            if (p_.is_code(a)) has_code_[layout_.unit_of(a)] = true;
        for (std::uint32_t a : p_.entries) {
            if (!p_.is_code(a)) continue;
            auto &unit = ids_[layout_.unit_of(a)];
            const auto id = static_cast<std::uint16_t>(unit.size() + 1);
            unit.emplace(a, id);
        }
    }

    std::uint16_t id_of(std::uint32_t a) const {
        const auto &unit = ids_[layout_.unit_of(a)];
        const auto it = unit.find(a);
        if (it == unit.end()) throw std::runtime_error(std::format("no entry for {:08X}", a));
        return it->second;
    }

    bool in_unit(std::uint32_t a) const { return a >= base_ && a < base_ + layout_.size(); }

    void write_unit(std::uint32_t u) {
        base_ = layout_.base_of(u);
        labels_.clear();
        for (const auto &[a, id] : ids_[u]) labels_.insert(a);
        const std::uint32_t end = base_ + layout_.size();
        for (std::uint32_t a = base_; a < end; a += 4) {
            if (!p_.is_code(a)) continue;
            const Insn i = at(a);
            const Flow fl = flow(i.op);
            if ((fl == Flow::Branch || fl == Flow::BranchLikely || fl == Flow::Jump) && !dynamic_jump(a)) {
                const std::uint32_t t = i.branch_target_of();
                if (in_unit(t) && p_.is_code(t)) labels_.insert(t);
            }
            if (has_delay_slot(i.op) && labels_.count(a + 4) && in_unit(a + 8)) labels_.insert(a + 8);
        }

        out_.str({});
        out_ << "#include \"units.hpp\"\n\nnamespace gen {\n\nusing namespace ps2;\n\n";
        out_ << "const std::uint16_t " << table_name(u) << "[" << (layout_.size() / 4) << "] = {";
        std::vector<std::uint16_t> dense(layout_.size() / 4, 0);
        for (const auto &[a, id] : ids_[u]) dense[(a - base_) >> 2] = id;
        for (std::size_t k = 0; k < dense.size(); ++k) out_ << (k % 32 ? " " : "\n    ") << dense[k] << ",";
        out_ << "\n};\n\n";
        out_ << "void " << unit_name(u) << "(Cpu &c, std::uint32_t entry, std::uint32_t depth) {\n";
        if (relocatable_) out_ << "    [[maybe_unused]] const std::uint32_t B = c.base;\n";
        out_ << "dispatch_entry:\n    switch (entry) {\n";
        for (const auto &[a, id] : ids_[u]) out_ << "    case " << id << "u: goto " << label(a) << ";\n";
        out_ << "    default: bad_entry(c, " << pc(base_) << "); return;\n    }\n";

        pending_ = 0;
        bool falls = false, skip = false;
        for (std::uint32_t a = base_; a < end; a += 4) {
            if (!p_.is_code(a)) {
                if (falls) exit_to(a);
                falls = skip = false;
                continue;
            }
            const bool labelled = labels_.count(a) != 0;
            if (skip && !labelled) {
                skip = false;
                continue;
            }
            skip = false;
            if (labelled) {
                flush();
                out_ << label(a) << ":\n";
            }
            if (options_.probes.count(a)) line(std::format("probe(c, {});", pc(a)));
            const Insn i = at(a);
            const Flow fl = flow(i.op);
            if (fl == Flow::None) {
                simple(i);
                ++pending_;
                falls = true;
            } else if (i.op == Eret) {
                flush();
                line("{ const std::uint32_t t = eret(c); c.pc = t; [[clang::musttail]] return dispatch(c, 0u, depth); }");
                falls = false;
            } else if (fl == Flow::Stop) {
                flush();
                line(std::format("stop(c, {}, \"{}\"); return;", pc(a), name(i.op)));
                falls = false;
            } else {
                const Insn ds = p_.is_code(a + 4) ? at(a + 4) : Insn{a + 4, 0, Invalid};
                control(i, ds);
                falls = fl != Flow::Jump && fl != Flow::JumpRegister;
                skip = true;
                if (falls && (labels_.count(a + 4) || !in_unit(a + 4))) {
                    // a+8 gets a label only as code; a seed that decodes data can leave it none
                    if (in_unit(a + 8) && labels_.count(a + 8) && p_.is_code(a + 8)) line("goto " + label(a + 8) + ";");
                    else exit_to(a + 8);
                    falls = false;
                }
            }
        }
        if (falls) exit_to(end);
        out_ << "}\n\n}\n";
        write_if_changed(fs::path(options_.output) / std::format("{}.cpp", unit_name(u)), out_.str());
    }

    void line(const std::string &text) {
        if (capture_) *capture_ += text + " ";
        else out_ << "    " << text << "\n";
    }

    void flush() {
        if (pending_) line(std::format("c.budget -= {};", pending_));
        pending_ = 0;
    }

    void exit_to(std::uint32_t a) {
        flush();
        if (p_.is_code(a) && !window_) {
            line(std::format("{}[[clang::musttail]] return {}(c, {}u, depth);", rebase(), unit_name(layout_.unit_of(a)), id_of(a)));
        } else {
            line(std::format("c.pc = {}; [[clang::musttail]] return dispatch(c, 0u, depth);", pc(a)));
        }
    }

    std::string jump(std::uint32_t t, std::uint32_t from, const std::string &where) {
        std::string check = t <= from ? std::format("if (c.budget < 0 && preempt(c)) {{ c.pc = {} | kUnwind; return; }} ", where) : "";
        if (in_unit(t) && p_.is_code(t)) return check + "goto " + label(t) + ";";
        if (p_.is_code(t) && !window_)
            return check + std::format("{}[[clang::musttail]] return {}(c, {}u, depth);", rebase(), unit_name(layout_.unit_of(t)), id_of(t));
        if (p_.is_code(t)) return check + std::format("c.pc = {}; [[clang::musttail]] return dispatch(c, 0u, depth);", where);
        return std::format("c.pc = {}; [[clang::musttail]] return dispatch(c, 0u, depth);", where);
    }

    std::string call_to(std::uint32_t t, std::uint32_t ret, const std::string &where) {
        std::string invoke;
        if (p_.is_code(t) && !(window_ && !in_unit(t)))
            invoke = relocatable_ ? std::format("if (depth < kMaxDepth) {{ c.base = B; {}(c, {}u, depth + 1u); }} else {{ c.pc = {}; return; }}",
                                                unit_name(layout_.unit_of(t)), id_of(t), where)
                                  : std::format("if (depth < kMaxDepth) {}(c, {}u, depth + 1u); else {{ c.pc = {}; return; }}",
                                                unit_name(layout_.unit_of(t)), id_of(t), where);
        else
            invoke = std::format("call(c, {}, depth + 1u);", where);
        return invoke + std::format(" if (c.pc != {}) [[unlikely]] return;", pc(ret));
    }

    std::string condition(const Insn &i) {
        switch (i.op) {
        case Beq: case Beql: return d(i.rs()) + " == " + d(i.rt());
        case Bne: case Bnel: return d(i.rs()) + " != " + d(i.rt());
        case Blez: case Blezl: return sd(i.rs()) + " <= 0";
        case Bgtz: case Bgtzl: return sd(i.rs()) + " > 0";
        case Bltz: case Bltzl: case Bltzal: case Bltzall: return sd(i.rs()) + " < 0";
        case Bgez: case Bgezl: case Bgezal: case Bgezall: return sd(i.rs()) + " >= 0";
        case Bc0f: case Bc0fl: return "!cpcond0(c)";
        case Bc0t: case Bc0tl: return "cpcond0(c)";
        case Bc1f: case Bc1fl: return "!(c.fcr31 & kFpuCondition)";
        case Bc1t: case Bc1tl: return "(c.fcr31 & kFpuCondition) != 0";
        case Bc2f: case Bc2fl: return "!vu0_busy(c)";
        case Bc2t: case Bc2tl: return "vu0_busy(c)";
        default: throw std::runtime_error(std::string("no condition for ") + name(i.op));
        }
    }

    std::string delay(const Insn &ds) {
        if (ds.op == Invalid || has_delay_slot(ds.op) || ds.op == Syscall)
            return std::format("stop(c, {}, \"unsupported delay slot\"); return;", pc(ds.pc));
        std::string text;
        capture_ = &text;
        simple(ds);
        capture_ = nullptr;
        return text;
    }

    void control(const Insn &i, const Insn &ds) {
        const std::uint32_t a = i.pc, ret = a + 8;
        line(std::format("c.budget -= {};", pending_ + 2));
        pending_ = 0;
        const std::string slot = delay(ds);
        const std::string link = std::format("c.r[31].sd[0] = static_cast<std::int32_t>({});", pc(ret));
        const std::uint32_t t = i.branch_target_of();
        // a game can load another image over the window while a function of it is
        // still running (MGS2 swaps its stage code that way): before calling out,
        // window code checks its call is still the one in memory and otherwise
        // goes on in whatever image is there now
        if (window_ && !dynamic_jump(a) && (i.op == Jal || i.op == Bltzal || i.op == Bgezal || i.op == Bltzall || i.op == Bgezall))
            line(std::format("if (ld32({}) != {}) [[unlikely]] {{ c.pc = {}; [[clang::musttail]] return dispatch(c, 0u, depth); }}", pc(a), hex(i.word), pc(a)));
        if (dynamic_jump(a)) {
            const std::string target = std::format("const std::uint32_t t = (({} + 4u) & 0xF0000000u) | ((ld32({}) & 0x03FFFFFFu) << 2);", pc(a), pc(a));
            if (i.op == Jal)
                line(std::format("{{ {} {} {} call(c, t, depth + 1u); if (c.pc != {}) [[unlikely]] return; }}", target, link, slot, pc(ret)));
            else
                line(std::format("{{ {} {} c.pc = t; [[clang::musttail]] return dispatch(c, 0u, depth); }}", target, slot));
            return;
        }
        const std::string where = target(i, t);
        switch (i.op) {
        case Jal:
            line(std::format("{{ {} {} {} }}", link, slot, call_to(t, ret, where)));
            return;
        case Bltzal: case Bgezal:
            line(std::format("{{ const bool t = {}; {} {} if (t) {{ {} }} }}", condition(i), link, slot, call_to(t, ret, where)));
            return;
        case Bltzall: case Bgezall:
            line(std::format("{{ const bool t = {}; {} if (t) {{ {} {} }} }}", condition(i), link, slot, call_to(t, ret, where)));
            return;
        default:
            break;
        }
        switch (flow(i.op)) {
        case Flow::Branch:
            line(std::format("{{ const bool t = {}; {} if (t) {{ {} }} }}", condition(i), slot, jump(t, a, where)));
            break;
        case Flow::BranchLikely:
            line(std::format("if ({}) {{ {} {} }}", condition(i), slot, jump(t, a, where)));
            break;
        case Flow::Jump:
            line(std::format("{{ {} {} }}", slot, jump(t, a, where)));
            break;
        case Flow::JumpRegister:
            if (i.rs() == 31) {
                line(std::format("{{ const std::uint32_t t = {}; {} c.pc = t; return; }}", w(31), slot));
            } else {
                line(std::format("{{ const std::uint32_t t = {}; {} c.pc = t; if (t - {} < {}u) {{ entry = {}[(t - {}) >> 2]; if (entry) goto dispatch_entry; }} [[clang::musttail]] return dispatch(c, 0u, depth); }}",
                                 w(i.rs()), slot, pc(base_), layout_.size(), table_name(layout_.unit_of(base_)), pc(base_)));
            }
            break;
        case Flow::CallRegister: {
            const std::string set = i.rd() ? std::format("c.r[{}].sd[0] = static_cast<std::int32_t>({});", i.rd(), pc(ret)) : "";
            line(std::format("{{ const std::uint32_t t = {}; {} {} c.pc = t; call(c, t, depth + 1u); if (c.pc != {}) [[unlikely]] return; }}",
                             w(i.rs()), set, slot, pc(ret)));
            break;
        }
        default:
            throw std::runtime_error("unexpected control flow");
        }
    }

    void set_d(std::uint32_t r, const std::string &value) {
        if (r) line(std::format("c.r[{}].d[0] = {};", r, value));
    }

    void set_w(std::uint32_t r, const std::string &value) {
        if (r) line(std::format("set32(c.r[{}], {});", r, value));
    }

    void mmi3(const char *op, const Insn &i) {
        if (i.rd()) line(std::format("mmi::{}({}, {}, {});", op, reg(i.rd()), reg(i.rs()), reg(i.rt())));
    }

    void mmi2(const char *op, const Insn &i) {
        if (i.rd()) line(std::format("mmi::{}({}, {});", op, reg(i.rd()), reg(i.rt())));
    }

    std::string dest_reg(const Insn &i) { return i.rd() ? reg(i.rd()) : "scratch"; }

    void vec(const char *op, const Insn &i) {
        line(std::format("vu::{}(c.vu0, {}u, {}u, {}u, {}u);", op, i.dest(), i.fd(), i.fs(), i.ft()));
    }

    void vecbc(const char *op, const Insn &i, const std::string &t) {
        line(std::format("vu::{}(c.vu0, {}u, {}u, {}u, {});", op, i.dest(), i.fd(), i.fs(), t));
    }

    void vacc(const char *op, const Insn &i) {
        line(std::format("vu::{}(c.vu0, {}u, {}u, {}u);", op, i.dest(), i.fs(), i.ft()));
    }

    void vaccbc(const char *op, const Insn &i, const std::string &t) {
        line(std::format("vu::{}(c.vu0, {}u, {}u, {});", op, i.dest(), i.fs(), t));
    }

    std::string bcv(const Insn &i) { return std::format("{}.w[{}]", vf(i.ft()), i.bc()); }

    void simple(const Insn &i) {
        const std::uint32_t rs = i.rs(), rt = i.rt(), rd = i.rd(), sa = i.sa();
        const std::uint32_t fsf = (i.word >> 21) & 3u, ftf = (i.word >> 23) & 3u;
        switch (i.op) {
        case Sll: if (i.word) set_w(rd, std::format("{} << {}", w(rt), sa)); break;
        case Srl: set_w(rd, std::format("{} >> {}", w(rt), sa)); break;
        case Sra: set_w(rd, std::format("static_cast<std::uint32_t>(static_cast<std::int32_t>({}) >> {})", w(rt), sa)); break;
        case Sllv: set_w(rd, std::format("{} << ({} & 31u)", w(rt), w(rs))); break;
        case Srlv: set_w(rd, std::format("{} >> ({} & 31u)", w(rt), w(rs))); break;
        case Srav: set_w(rd, std::format("static_cast<std::uint32_t>(static_cast<std::int32_t>({}) >> ({} & 31u))", w(rt), w(rs))); break;
        case Movz: if (rd) line(std::format("if ({} == 0) c.r[{}].d[0] = {};", d(rt), rd, d(rs))); break;
        case Movn: if (rd) line(std::format("if ({} != 0) c.r[{}].d[0] = {};", d(rt), rd, d(rs))); break;
        case Syscall:
            flush();
            line(std::format("c.pc = {}; syscall(c, {}); if (c.pc != {}) return;", pc(i.pc + 4), pc(i.pc), pc(i.pc + 4)));
            break;
        case Break: line(std::format("breakpoint(c, {});", pc(i.pc))); break;
        case Sync: case Cache: case Pref: break;
        case Mfhi: set_d(rd, "c.hi.d[0]"); break;
        case Mflo: set_d(rd, "c.lo.d[0]"); break;
        case Mfhi1: set_d(rd, "c.hi.d[1]"); break;
        case Mflo1: set_d(rd, "c.lo.d[1]"); break;
        case Mthi: line("c.hi.d[0] = " + d(rs) + ";"); break;
        case Mtlo: line("c.lo.d[0] = " + d(rs) + ";"); break;
        case Mthi1: line("c.hi.d[1] = " + d(rs) + ";"); break;
        case Mtlo1: line("c.lo.d[1] = " + d(rs) + ";"); break;
        case Dsllv: set_d(rd, std::format("{} << ({} & 63u)", d(rt), w(rs))); break;
        case Dsrlv: set_d(rd, std::format("{} >> ({} & 63u)", d(rt), w(rs))); break;
        case Dsrav: set_d(rd, std::format("static_cast<std::uint64_t>({} >> ({} & 63u))", sd(rt), w(rs))); break;
        case Mult: case Multu: case Mult1: case Multu1: case Madd: case Maddu: case Madd1: case Maddu1: {
            const bool one = i.op == Mult1 || i.op == Multu1 || i.op == Madd1 || i.op == Maddu1;
            const char *fn = (i.op == Mult || i.op == Mult1) ? "mult" : (i.op == Multu || i.op == Multu1) ? "multu"
                           : (i.op == Madd || i.op == Madd1) ? "madd" : "maddu";
            line(std::format("mmi::{}(c, {}, {}, {});", fn, one ? 1 : 0, w(rs), w(rt)));
            set_d(rd, std::format("c.lo.d[{}]", one ? 1 : 0));
            break;
        }
        case Div: line(std::format("mmi::div(c, 0, {}, {});", w(rs), w(rt))); break;
        case Divu: line(std::format("mmi::divu(c, 0, {}, {});", w(rs), w(rt))); break;
        case Div1: line(std::format("mmi::div(c, 1, {}, {});", w(rs), w(rt))); break;
        case Divu1: line(std::format("mmi::divu(c, 1, {}, {});", w(rs), w(rt))); break;
        case Add: case Addu: set_w(rd, w(rs) + " + " + w(rt)); break;
        case Sub: case Subu: set_w(rd, w(rs) + " - " + w(rt)); break;
        case And: set_d(rd, d(rs) + " & " + d(rt)); break;
        case Or: set_d(rd, d(rs) + " | " + d(rt)); break;
        case Xor: set_d(rd, d(rs) + " ^ " + d(rt)); break;
        case Nor: set_d(rd, "~(" + d(rs) + " | " + d(rt) + ")"); break;
        case Mfsa: set_d(rd, "c.sa"); break;
        case Mtsa: line(std::format("c.sa = {} & 15u;", w(rs))); break;
        case Mtsab: line(std::format("c.sa = ({} ^ {}u) & 15u;", w(rs), i.uimm())); break;
        case Mtsah: line(std::format("c.sa = (({} ^ {}u) & 7u) * 2u;", w(rs), i.uimm())); break;
        case Slt: set_d(rd, std::format("{} < {} ? 1ull : 0ull", sd(rs), sd(rt))); break;
        case Sltu: set_d(rd, std::format("{} < {} ? 1ull : 0ull", d(rs), d(rt))); break;
        case Dadd: case Daddu: set_d(rd, d(rs) + " + " + d(rt)); break;
        case Dsub: case Dsubu: set_d(rd, d(rs) + " - " + d(rt)); break;
        case Tge: line(std::format("if ({} >= {}) trap(c, {});", sd(rs), sd(rt), pc(i.pc))); break;
        case Tgeu: line(std::format("if ({} >= {}) trap(c, {});", d(rs), d(rt), pc(i.pc))); break;
        case Tlt: line(std::format("if ({} < {}) trap(c, {});", sd(rs), sd(rt), pc(i.pc))); break;
        case Tltu: line(std::format("if ({} < {}) trap(c, {});", d(rs), d(rt), pc(i.pc))); break;
        case Teq: line(std::format("if ({} == {}) trap(c, {});", d(rs), d(rt), pc(i.pc))); break;
        case Tne: line(std::format("if ({} != {}) trap(c, {});", d(rs), d(rt), pc(i.pc))); break;
        case Tgei: line(std::format("if ({} >= {}ll) trap(c, {});", sd(rs), i.simm(), pc(i.pc))); break;
        case Tgeiu: line(std::format("if ({} >= static_cast<std::uint64_t>({}ll)) trap(c, {});", d(rs), i.simm(), pc(i.pc))); break;
        case Tlti: line(std::format("if ({} < {}ll) trap(c, {});", sd(rs), i.simm(), pc(i.pc))); break;
        case Tltiu: line(std::format("if ({} < static_cast<std::uint64_t>({}ll)) trap(c, {});", d(rs), i.simm(), pc(i.pc))); break;
        case Teqi: line(std::format("if ({} == {}ll) trap(c, {});", sd(rs), i.simm(), pc(i.pc))); break;
        case Tnei: line(std::format("if ({} != {}ll) trap(c, {});", sd(rs), i.simm(), pc(i.pc))); break;
        case Dsll: set_d(rd, std::format("{} << {}", d(rt), sa)); break;
        case Dsrl: set_d(rd, std::format("{} >> {}", d(rt), sa)); break;
        case Dsra: set_d(rd, std::format("static_cast<std::uint64_t>({} >> {})", sd(rt), sa)); break;
        case Dsll32: set_d(rd, std::format("{} << {}", d(rt), sa + 32)); break;
        case Dsrl32: set_d(rd, std::format("{} >> {}", d(rt), sa + 32)); break;
        case Dsra32: set_d(rd, std::format("static_cast<std::uint64_t>({} >> {})", sd(rt), sa + 32)); break;
        case Addi: case Addiu: set_w(rt, w(rs) + imm(i)); break;
        case Slti: set_d(rt, std::format("{} < {}ll ? 1ull : 0ull", sd(rs), i.simm())); break;
        case Sltiu: set_d(rt, std::format("{} < static_cast<std::uint64_t>({}ll) ? 1ull : 0ull", d(rs), i.simm())); break;
        case Andi: set_d(rt, std::format("{} & {}", d(rs), uimm(i))); break;
        case Ori: set_d(rt, std::format("{} | {}", d(rs), uimm(i))); break;
        case Xori: set_d(rt, std::format("{} ^ {}", d(rs), uimm(i))); break;
        case Lui:
            if (!rt) break;
            if (const auto e = field(i)) line(std::format("c.r[{}].sd[0] = static_cast<std::int32_t>({} << 16);", rt, *e));
            else line(std::format("c.r[{}].sd[0] = static_cast<std::int32_t>({});", rt, hex(i.uimm() << 16)));
            break;
        case Daddi: case Daddiu:
            if (const auto e = field(i)) set_d(rt, std::format("{} + static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>({})))", d(rs), *e));
            else set_d(rt, std::format("{} + static_cast<std::uint64_t>({}ll)", d(rs), i.simm()));
            break;
        case Ldl: if (rt) line(std::format("ldl({}, {});", reg(rt), address(i))); break;
        case Ldr: if (rt) line(std::format("ldr({}, {});", reg(rt), address(i))); break;
        case Lwl: if (rt) line(std::format("lwl({}, {});", reg(rt), address(i))); break;
        case Lwr: if (rt) line(std::format("lwr({}, {});", reg(rt), address(i))); break;
        case Sdl: line(std::format("sdl({}, {});", reg(rt), address(i))); break;
        case Sdr: line(std::format("sdr({}, {});", reg(rt), address(i))); break;
        case Swl: line(std::format("swl({}, {});", reg(rt), address(i))); break;
        case Swr: line(std::format("swr({}, {});", reg(rt), address(i))); break;
        case Lq:
            if (rt) line(std::format("ld128({} & ~15u, {});", address(i), reg(rt)));
            else line(std::format("{{ Reg128 scratch; ld128({} & ~15u, scratch); }}", address(i)));
            break;
        case Sq: line(std::format("st128({} & ~15u, {});", address(i), reg(rt))); break;
        case Lb: if (rt) line(std::format("c.r[{}].sd[0] = static_cast<std::int8_t>(ld8({}));", rt, address(i))); else line(std::format("(void)ld8({});", address(i))); break;
        case Lbu: if (rt) line(std::format("c.r[{}].d[0] = ld8({});", rt, address(i))); else line(std::format("(void)ld8({});", address(i))); break;
        case Lh: if (rt) line(std::format("c.r[{}].sd[0] = static_cast<std::int16_t>(ld16({}));", rt, address(i))); else line(std::format("(void)ld16({});", address(i))); break;
        case Lhu: if (rt) line(std::format("c.r[{}].d[0] = ld16({});", rt, address(i))); else line(std::format("(void)ld16({});", address(i))); break;
        case Lw: if (rt) line(std::format("set32(c.r[{}], ld32({}));", rt, address(i))); else line(std::format("(void)ld32({});", address(i))); break;
        case Lwu: if (rt) line(std::format("c.r[{}].d[0] = ld32({});", rt, address(i))); else line(std::format("(void)ld32({});", address(i))); break;
        case Ld: if (rt) line(std::format("c.r[{}].d[0] = ld64({});", rt, address(i))); else line(std::format("(void)ld64({});", address(i))); break;
        case Sb: line(std::format("st8({}, static_cast<std::uint8_t>({}));", address(i), w(rt))); break;
        case Sh: line(std::format("st16({}, static_cast<std::uint16_t>({}));", address(i), w(rt))); break;
        case Sw: line(std::format("st32({}, {});", address(i), w(rt))); break;
        case Sd: line(std::format("st64({}, {});", address(i), d(rt))); break;
        case Lwc1: line(std::format("{} = ld32({});", f(i.ft()), address(i))); break;
        case Swc1: line(std::format("st32({}, {});", address(i), f(i.ft()))); break;
        case Lqc2:
            if (i.ft()) line(std::format("ld128({} & ~15u, {});", address(i), vf(i.ft())));
            else line(std::format("{{ Reg128 scratch; ld128({} & ~15u, scratch); }}", address(i)));
            break;
        case Sqc2: line(std::format("st128({} & ~15u, {});", address(i), vf(i.ft()))); break;

        case Plzcw: if (rd) line(std::format("mmi::plzcw({}, {});", reg(rd), reg(rs))); break;
        case Pmfhl: if (rd) line(std::format("mmi::pmfhl(c, {}, {}u);", reg(rd), sa)); break;
        case Pmthl: line(std::format("mmi::pmthl(c, {});", reg(rs))); break;
        case Psllh: if (rd) line(std::format("mmi::psllh({}, {}, {}u);", reg(rd), reg(rt), sa)); break;
        case Psrlh: if (rd) line(std::format("mmi::psrlh({}, {}, {}u);", reg(rd), reg(rt), sa)); break;
        case Psrah: if (rd) line(std::format("mmi::psrah({}, {}, {}u);", reg(rd), reg(rt), sa)); break;
        case Psllw: if (rd) line(std::format("mmi::psllw({}, {}, {}u);", reg(rd), reg(rt), sa)); break;
        case Psrlw: if (rd) line(std::format("mmi::psrlw({}, {}, {}u);", reg(rd), reg(rt), sa)); break;
        case Psraw: if (rd) line(std::format("mmi::psraw({}, {}, {}u);", reg(rd), reg(rt), sa)); break;
        case Paddw: mmi3("paddw", i); break;
        case Psubw: mmi3("psubw", i); break;
        case Pcgtw: mmi3("pcgtw", i); break;
        case Pmaxw: mmi3("pmaxw", i); break;
        case Paddh: mmi3("paddh", i); break;
        case Psubh: mmi3("psubh", i); break;
        case Pcgth: mmi3("pcgth", i); break;
        case Pmaxh: mmi3("pmaxh", i); break;
        case Paddb: mmi3("paddb", i); break;
        case Psubb: mmi3("psubb", i); break;
        case Pcgtb: mmi3("pcgtb", i); break;
        case Paddsw: mmi3("paddsw", i); break;
        case Psubsw: mmi3("psubsw", i); break;
        case Pextlw: mmi3("pextlw", i); break;
        case Ppacw: mmi3("ppacw", i); break;
        case Paddsh: mmi3("paddsh", i); break;
        case Psubsh: mmi3("psubsh", i); break;
        case Pextlh: mmi3("pextlh", i); break;
        case Ppach: mmi3("ppach", i); break;
        case Paddsb: mmi3("paddsb", i); break;
        case Psubsb: mmi3("psubsb", i); break;
        case Pextlb: mmi3("pextlb", i); break;
        case Ppacb: mmi3("ppacb", i); break;
        case Pext5: mmi2("pext5", i); break;
        case Ppac5: mmi2("ppac5", i); break;
        case Pabsw: mmi2("pabsw", i); break;
        case Pceqw: mmi3("pceqw", i); break;
        case Pminw: mmi3("pminw", i); break;
        case Padsbh: mmi3("padsbh", i); break;
        case Pabsh: mmi2("pabsh", i); break;
        case Pceqh: mmi3("pceqh", i); break;
        case Pminh: mmi3("pminh", i); break;
        case Pceqb: mmi3("pceqb", i); break;
        case Padduw: mmi3("padduw", i); break;
        case Psubuw: mmi3("psubuw", i); break;
        case Pextuw: mmi3("pextuw", i); break;
        case Padduh: mmi3("padduh", i); break;
        case Psubuh: mmi3("psubuh", i); break;
        case Pextuh: mmi3("pextuh", i); break;
        case Paddub: mmi3("paddub", i); break;
        case Psubub: mmi3("psubub", i); break;
        case Pextub: mmi3("pextub", i); break;
        case Qfsrv: if (rd) line(std::format("mmi::qfsrv({}, {}, {}, c.sa);", reg(rd), reg(rs), reg(rt))); break;
        case Psllvw: mmi3("psllvw", i); break;
        case Psrlvw: mmi3("psrlvw", i); break;
        case Psravw: mmi3("psravw", i); break;
        case Pmaddw: case Pmsubw:
            line(std::format("{{ Reg128 scratch; mmi::pmaddw(c, {}, {}, {}, {}); }}", dest_reg(i), reg(rs), reg(rt), i.op == Pmsubw ? "true" : "false"));
            break;
        case Pmadduw: line(std::format("{{ Reg128 scratch; mmi::pmadduw(c, {}, {}, {}); }}", dest_reg(i), reg(rs), reg(rt))); break;
        case Pmultw: line(std::format("{{ Reg128 scratch; mmi::pmultw(c, {}, {}, {}); }}", dest_reg(i), reg(rs), reg(rt))); break;
        case Pmultuw: line(std::format("{{ Reg128 scratch; mmi::pmultuw(c, {}, {}, {}); }}", dest_reg(i), reg(rs), reg(rt))); break;
        case Pmulth: case Pmaddh: case Pmsubh:
            line(std::format("{{ Reg128 scratch; mmi::pmulth(c, {}, {}, {}, {}); }}", dest_reg(i), reg(rs), reg(rt), i.op == Pmulth ? 0 : i.op == Pmaddh ? 1 : 2));
            break;
        case Phmadh: case Phmsbh:
            line(std::format("{{ Reg128 scratch; mmi::phmadh(c, {}, {}, {}, {}); }}", dest_reg(i), reg(rs), reg(rt), i.op == Phmsbh ? "true" : "false"));
            break;
        case Pdivw: line(std::format("mmi::pdivw(c, {}, {});", reg(rs), reg(rt))); break;
        case Pdivuw: line(std::format("mmi::pdivuw(c, {}, {});", reg(rs), reg(rt))); break;
        case Pdivbw: line(std::format("mmi::pdivbw(c, {}, {});", reg(rs), reg(rt))); break;
        case Pmfhi: if (rd) line(std::format("{} = c.hi;", reg(rd))); break;
        case Pmflo: if (rd) line(std::format("{} = c.lo;", reg(rd))); break;
        case Pmthi: line(std::format("c.hi = {};", reg(rs))); break;
        case Pmtlo: line(std::format("c.lo = {};", reg(rs))); break;
        case Pinth: mmi3("pinth", i); break;
        case Pinteh: mmi3("pinteh", i); break;
        case Pcpyld: mmi3("pcpyld", i); break;
        case Pcpyud: mmi3("pcpyud", i); break;
        case Pand: mmi3("pand", i); break;
        case Pxor: mmi3("pxor", i); break;
        case Por: mmi3("por", i); break;
        case Pnor: mmi3("pnor", i); break;
        case Pexeh: mmi2("pexeh", i); break;
        case Prevh: mmi2("prevh", i); break;
        case Pexew: mmi2("pexew", i); break;
        case Prot3w: mmi2("prot3w", i); break;
        case Pexch: mmi2("pexch", i); break;
        case Pcpyh: mmi2("pcpyh", i); break;
        case Pexcw: mmi2("pexcw", i); break;

        case Mfc0: set_w(rt, std::format("mfc0(c, {}u, {}u)", rd, i.word & 7u)); break;
        case Mtc0: line(std::format("mtc0(c, {}u, {});", rd, w(rt))); break;
        case Ei: line("set_interrupts(c, true);"); break;
        case Di: line("set_interrupts(c, false);"); break;
        case Tlbr: case Tlbwi: case Tlbwr: case Tlbp:
            line(std::format("stop(c, {}, \"{}\");", pc(i.pc), name(i.op)));
            break;

        case Mfc1: set_w(rt, f(i.fs())); break;
        case Mtc1: line(std::format("{} = {};", f(i.fs()), w(rt))); break;
        case Cfc1: set_w(rt, std::format("cfc1(c, {}u)", i.fs())); break;
        case Ctc1: line(std::format("ctc1(c, {}u, {});", i.fs(), w(rt))); break;
        case AddS: line(std::format("{} = fp::add({}, {});", f(i.fd()), f(i.fs()), f(i.ft()))); break;
        case SubS: line(std::format("{} = fp::sub({}, {});", f(i.fd()), f(i.fs()), f(i.ft()))); break;
        case MulS: line(std::format("{} = fp::mul({}, {});", f(i.fd()), f(i.fs()), f(i.ft()))); break;
        case DivS: line(std::format("fpu_div(c, {}u, {}, {});", i.fd(), f(i.fs()), f(i.ft()))); break;
        case SqrtS: line(std::format("{} = fp::sqrt({});", f(i.fd()), f(i.ft()))); break;
        case AbsS: line(std::format("{} = {} & fp::kAbs;", f(i.fd()), f(i.fs()))); break;
        case MovS: line(std::format("{} = {};", f(i.fd()), f(i.fs()))); break;
        case NegS: line(std::format("{} = {} ^ fp::kSign;", f(i.fd()), f(i.fs()))); break;
        case RsqrtS: line(std::format("fpu_rsqrt(c, {}u, {}, {});", i.fd(), f(i.fs()), f(i.ft()))); break;
        case AddaS: line(std::format("c.facc = fp::add({}, {});", f(i.fs()), f(i.ft()))); break;
        case SubaS: line(std::format("c.facc = fp::sub({}, {});", f(i.fs()), f(i.ft()))); break;
        case MulaS: line(std::format("c.facc = fp::mul({}, {});", f(i.fs()), f(i.ft()))); break;
        case MaddS: line(std::format("{} = fp::add(c.facc, fp::mul({}, {}));", f(i.fd()), f(i.fs()), f(i.ft()))); break;
        case MsubS: line(std::format("{} = fp::sub(c.facc, fp::mul({}, {}));", f(i.fd()), f(i.fs()), f(i.ft()))); break;
        case MaddaS: line(std::format("c.facc = fp::add(c.facc, fp::mul({}, {}));", f(i.fs()), f(i.ft()))); break;
        case MsubaS: line(std::format("c.facc = fp::sub(c.facc, fp::mul({}, {}));", f(i.fs()), f(i.ft()))); break;
        case CvtWS: line(std::format("{} = fp::cvt_w_s({});", f(i.fd()), f(i.fs()))); break;
        case CvtSW: line(std::format("{} = fp::cvt_s_w({});", f(i.fd()), f(i.fs()))); break;
        case MaxS: line(std::format("{} = fp::max({}, {});", f(i.fd()), f(i.fs()), f(i.ft()))); break;
        case MinS: line(std::format("{} = fp::min({}, {});", f(i.fd()), f(i.fs()), f(i.ft()))); break;
        case CF: line("fpu_compare(c, false);"); break;
        case CEq: line(std::format("fpu_compare(c, fp::key({}) == fp::key({}));", f(i.fs()), f(i.ft()))); break;
        case CLt: line(std::format("fpu_compare(c, fp::key({}) < fp::key({}));", f(i.fs()), f(i.ft()))); break;
        case CLe: line(std::format("fpu_compare(c, fp::key({}) <= fp::key({}));", f(i.fs()), f(i.ft()))); break;

        case Qmfc2:
            if (i.word & 1u) line("vu0_wait(c);");
            if (rt) line(std::format("{} = {};", reg(rt), vf(i.fs())));
            break;
        case Qmtc2:
            if (i.word & 1u) line("vu0_wait(c);");
            if (i.fs()) line(std::format("{} = {};", vf(i.fs()), reg(rt)));
            break;
        case Cfc2:
            if (i.fs() < 16) set_w(rt, vi(i.fs()));
            else set_w(rt, std::format("cfc2(c, {}u)", i.fs()));
            break;
        case Ctc2:
            if (i.fs() < 16) line(std::format("vu::set_vi(c.vu0, {}u, {});", i.fs(), w(rt)));
            else line(std::format("ctc2(c, {}u, {});", i.fs(), w(rt)));
            break;
        case Vaddbc: vecbc("addbc", i, bcv(i)); break;
        case Vsubbc: vecbc("subbc", i, bcv(i)); break;
        case Vmaddbc: vecbc("maddbc", i, bcv(i)); break;
        case Vmsubbc: vecbc("msubbc", i, bcv(i)); break;
        case Vmaxbc: vecbc("maxbc", i, bcv(i)); break;
        case Vminibc: vecbc("minibc", i, bcv(i)); break;
        case Vmulbc: vecbc("mulbc", i, bcv(i)); break;
        case Vmulq: vecbc("mulbc", i, "c.vu0.q"); break;
        case Vmaxi: vecbc("maxbc", i, "c.vu0.i"); break;
        case Vmuli: vecbc("mulbc", i, "c.vu0.i"); break;
        case Vminii: vecbc("minibc", i, "c.vu0.i"); break;
        case Vaddq: vecbc("addbc", i, "c.vu0.q"); break;
        case Vmaddq: vecbc("maddbc", i, "c.vu0.q"); break;
        case Vaddi: vecbc("addbc", i, "c.vu0.i"); break;
        case Vmaddi: vecbc("maddbc", i, "c.vu0.i"); break;
        case Vsubq: vecbc("subbc", i, "c.vu0.q"); break;
        case Vmsubq: vecbc("msubbc", i, "c.vu0.q"); break;
        case Vsubi: vecbc("subbc", i, "c.vu0.i"); break;
        case Vmsubi: vecbc("msubbc", i, "c.vu0.i"); break;
        case Vadd: vec("add", i); break;
        case Vmadd: vec("madd", i); break;
        case Vmul: vec("mul", i); break;
        case Vmax: vec("max", i); break;
        case Vsub: vec("sub", i); break;
        case Vmsub: vec("msub", i); break;
        case Vmini: vec("mini", i); break;
        case Vopmsub: line(std::format("vu::opmsub(c.vu0, {}u, {}u, {}u);", i.fd(), i.fs(), i.ft())); break;
        case Viadd: line(std::format("vu::set_vi(c.vu0, {}u, {} + {});", i.fd(), vi(i.fs()), vi(i.ft()))); break;
        case Visub: line(std::format("vu::set_vi(c.vu0, {}u, {} - {});", i.fd(), vi(i.fs()), vi(i.ft()))); break;
        case Viaddi: line(std::format("vu::set_vi(c.vu0, {}u, {} + {});", i.ft(), vi(i.fs()), i.imm5())); break;
        case Viand: line(std::format("vu::set_vi(c.vu0, {}u, {} & {});", i.fd(), vi(i.fs()), vi(i.ft()))); break;
        case Vior: line(std::format("vu::set_vi(c.vu0, {}u, {} | {});", i.fd(), vi(i.fs()), vi(i.ft()))); break;
        case Vcallms: line(std::format("vcallms(c, {}u);", i.imm15() * 8u)); break;
        case Vcallmsr: line("vcallms(c, c.vu0.cmsar0 * 8u);"); break;
        case Vaddabc: vaccbc("addabc", i, bcv(i)); break;
        case Vsubabc: vaccbc("subabc", i, bcv(i)); break;
        case Vmaddabc: vaccbc("maddabc", i, bcv(i)); break;
        case Vmsubabc: vaccbc("msubabc", i, bcv(i)); break;
        case Vmulabc: vaccbc("mulabc", i, bcv(i)); break;
        case Vmulaq: vaccbc("mulabc", i, "c.vu0.q"); break;
        case Vmulai: vaccbc("mulabc", i, "c.vu0.i"); break;
        case Vaddaq: vaccbc("addabc", i, "c.vu0.q"); break;
        case Vmaddaq: vaccbc("maddabc", i, "c.vu0.q"); break;
        case Vaddai: vaccbc("addabc", i, "c.vu0.i"); break;
        case Vmaddai: vaccbc("maddabc", i, "c.vu0.i"); break;
        case Vsubaq: vaccbc("subabc", i, "c.vu0.q"); break;
        case Vmsubaq: vaccbc("msubabc", i, "c.vu0.q"); break;
        case Vsubai: vaccbc("subabc", i, "c.vu0.i"); break;
        case Vmsubai: vaccbc("msubabc", i, "c.vu0.i"); break;
        case Vadda: vacc("adda", i); break;
        case Vmadda: vacc("madda", i); break;
        case Vmula: vacc("mula", i); break;
        case Vsuba: vacc("suba", i); break;
        case Vmsuba: vacc("msuba", i); break;
        case Vopmula: line(std::format("vu::opmula(c.vu0, {}u, {}u);", i.fs(), i.ft())); break;
        case Vitof0: case Vitof4: case Vitof12: case Vitof15: {
            const int frac = i.op == Vitof0 ? 0 : i.op == Vitof4 ? 4 : i.op == Vitof12 ? 12 : 15;
            line(std::format("vu::itof(c.vu0, {}u, {}u, {}u, {});", i.dest(), i.ft(), i.fs(), frac));
            break;
        }
        case Vftoi0: case Vftoi4: case Vftoi12: case Vftoi15: {
            const int frac = i.op == Vftoi0 ? 0 : i.op == Vftoi4 ? 4 : i.op == Vftoi12 ? 12 : 15;
            line(std::format("vu::ftoi(c.vu0, {}u, {}u, {}u, {});", i.dest(), i.ft(), i.fs(), frac));
            break;
        }
        case Vabs: line(std::format("vu::abs(c.vu0, {}u, {}u, {}u);", i.dest(), i.ft(), i.fs())); break;
        case Vmove: line(std::format("vu::move(c.vu0, {}u, {}u, {}u);", i.dest(), i.ft(), i.fs())); break;
        case Vmr32: line(std::format("vu::mr32(c.vu0, {}u, {}u, {}u);", i.dest(), i.ft(), i.fs())); break;
        case Vclip: line(std::format("vu::clip(c.vu0, {}u, {}u);", i.fs(), i.ft())); break;
        case Vnop: case Vwaitq: break;
        case Vlqi: line(std::format("lqc2_vu(c, {}u, {}u, {}); vu::set_vi(c.vu0, {}u, {} + 1u);", i.dest(), i.ft(), vi(i.fs()), i.fs(), vi(i.fs()))); break;
        case Vsqi: line(std::format("sqc2_vu(c, {}u, {}u, {}); vu::set_vi(c.vu0, {}u, {} + 1u);", i.dest(), i.fs(), vi(i.ft()), i.ft(), vi(i.ft()))); break;
        case Vlqd: line(std::format("vu::set_vi(c.vu0, {}u, {} - 1u); lqc2_vu(c, {}u, {}u, {});", i.fs(), vi(i.fs()), i.dest(), i.ft(), vi(i.fs()))); break;
        case Vsqd: line(std::format("vu::set_vi(c.vu0, {}u, {} - 1u); sqc2_vu(c, {}u, {}u, {});", i.ft(), vi(i.ft()), i.dest(), i.fs(), vi(i.ft()))); break;
        case Vdiv: line(std::format("vu::div(c.vu0, {}.w[{}], {}.w[{}]);", vf(i.fs()), fsf, vf(i.ft()), ftf)); break;
        case Vsqrt: line(std::format("vu::sqrt(c.vu0, {}.w[{}]);", vf(i.ft()), ftf)); break;
        case Vrsqrt: line(std::format("vu::rsqrt(c.vu0, {}.w[{}], {}.w[{}]);", vf(i.fs()), fsf, vf(i.ft()), ftf)); break;
        case Vmtir: line(std::format("vu::set_vi(c.vu0, {}u, {}.w[{}]);", i.ft(), vf(i.fs()), fsf)); break;
        case Vmfir: line(std::format("vu::mfir(c.vu0, {}u, {}u, {}u);", i.dest(), i.ft(), i.fs())); break;
        case Vilwr:
            line(std::format("{{ std::uint16_t v; __builtin_memcpy(&v, vu0_data() + vu0_addr({}) + 4u * {}u, 2); vu::set_vi(c.vu0, {}u, v); }}",
                             vi(i.fs()), std::countl_zero(i.dest() << 28), i.ft()));
            break;
        case Viswr:
            line(std::format("for (int k = 0; k < 4; ++k) if (vu::lane({}u, k)) {{ const std::uint32_t v = {}; __builtin_memcpy(vu0_data() + vu0_addr({}) + 4u * k, &v, 4); }}",
                             i.dest(), vi(i.ft()), vi(i.fs())));
            break;
        case Vrnext:
            line(std::format("c.vu0.r = vu::rand_next(c.vu0.r); vu::apply(c.vu0, {}u, {}u, [&](int) {{ return c.vu0.r | fp::kOne; }});", i.dest(), i.ft()));
            break;
        case Vrget: line(std::format("vu::apply(c.vu0, {}u, {}u, [&](int) {{ return c.vu0.r | fp::kOne; }});", i.dest(), i.ft())); break;
        case Vrinit: line(std::format("c.vu0.r = {}.w[{}] & 0x7FFFFFu;", vf(i.fs()), fsf)); break;
        case Vrxor: line(std::format("c.vu0.r = (c.vu0.r ^ {}.w[{}]) & 0x7FFFFFu;", vf(i.fs()), fsf)); break;

        default:
            line(std::format("stop(c, {}, \"{}\"); return;", pc(i.pc), name(i.op)));
            break;
        }
    }

    const Image &image_;
    Program &p_;
    const Links *links_;
    std::string prefix_;
    bool relocatable_;
    // A window image is proven unit by unit, so control leaving a unit goes
    // through dispatch, which proves the unit it enters.
    bool window_;
    bool template_;
    const CodegenOptions &options_;
    Layout layout_;
    std::vector<std::map<std::uint32_t, std::uint16_t>> ids_;
    std::vector<bool> has_code_;
    std::uint32_t base_{};
    std::set<std::uint32_t> labels_;
    std::string *capture_{};
    std::uint32_t pending_{};
    std::ostringstream out_;
};

}

void generate(const std::vector<Translation> &translations, const CodegenOptions &options) {
    fs::create_directories(options.output);
    std::ostringstream header, table, modules;
    header << "#pragma once\n\n#include \"ps2/generated.hpp\"\n\nnamespace gen {\n";
    table << "#include \"units.hpp\"\n\nnamespace gen {\n\n";
    std::size_t written = 0, main_units = 0, module_count = 0;
    std::vector<std::string> unit_names;
    struct PieceRow {
        std::uint64_t hash;
        std::size_t module;
        std::uint32_t offset, words;
    };
    std::vector<PieceRow> pieces;
    std::pair<std::uint32_t, std::uint32_t> area{UINT32_MAX, 0};
    for (const Translation &t : translations) {
        const std::vector<UnitRow> rows = Generator(t, options).run();
        written += rows.size();
        for (const UnitRow &r : rows) {
            unit_names.push_back(r.function);
            header << "void " << r.function << "(ps2::Cpu &c, std::uint32_t entry, std::uint32_t depth);\n";
            header << "extern const std::uint16_t " << r.table << "[];\n";
        }
        table << "const ps2::UnitInfo " << (t.relocatable || t.fixed_module ? t.prefix + "units" : "kUnits") << "[] = {\n";
        for (const UnitRow &r : rows)
            table << std::format("    {{{}, {}u, &{}, {}}},\n", hex(t.fixed_module ? r.base - t.fixed_base : r.base), r.size, r.function, r.table);
        table << "};\n\n";
        if (!t.relocatable && !t.fixed_module) {
            main_units = rows.size();
            continue;
        }
        table << "const std::uint32_t " << t.prefix << "checks[] = {";
        for (const auto &[at, value] : t.checks) table << std::format("{}, {}, ", hex(at), hex(value));
        table << "};\n\n";
        modules << std::format("    {{\"{}\", {}, {}, {}checks, {}u, {}units, {}u, {}u}},\n", t.name, hex(t.fixed_base), hex(t.size), t.prefix, t.checks.size(),
                               t.prefix, rows.size(), t.template_module ? 1 : 0);
        if (t.template_module) {
            for (const Translation::Piece &p : t.pieces) pieces.push_back({p.hash, module_count, p.offset, p.words});
            area = {std::min(area.first, t.area.first), std::max(area.second, t.area.second)};
        }
        ++module_count;
    }
    header << "}\n";
    if (module_count) table << "const ps2::ModuleInfo kModules[] = {\n" << modules.str() << "};\n\n";
    if (!pieces.empty()) {
        std::sort(pieces.begin(), pieces.end(), [](const PieceRow &a, const PieceRow &b) { return a.hash < b.hash; });
        table << "const ps2::TemplatePiece kTemplatePieces[] = {\n";
        for (const PieceRow &p : pieces) table << std::format("    {{0x{:016X}ull, {}u, {}, {}u}},\n", p.hash, p.module, hex(p.offset), p.words);
        table << "};\n\n";
    }
    table << "}\n\nvoid register_generated_code() { ps2::register_units(gen::kUnits, " << main_units << "u);";
    if (module_count) table << " ps2::register_modules(gen::kModules, " << module_count << "u);";
    if (!pieces.empty())
        table << std::format(" ps2::register_templates(gen::kTemplatePieces, {}u, {}, {});", pieces.size(), hex(area.first), hex(area.second));
    table << " }\n";
    write_if_changed(fs::path(options.output) / "units.hpp", header.str());
    write_if_changed(fs::path(options.output) / "table.cpp", table.str());
    // Units of an earlier translation that this one no longer has (a module
    // regrouped under a new name, a smaller image) would still be built.
    std::set<std::string> kept;
    for (const std::string &name : unit_names) kept.insert(name + ".cpp");
    static const std::regex unit_file("(m_.*_)?unit_[0-9A-F]{4}\\.cpp");
    for (const auto &entry : fs::directory_iterator(options.output)) {
        const std::string file = entry.path().filename().string();
        if (std::regex_match(file, unit_file) && !kept.contains(file)) fs::remove(entry.path());
    }
    std::printf("%zu units written to %s\n", written, options.output.c_str());
}

}
