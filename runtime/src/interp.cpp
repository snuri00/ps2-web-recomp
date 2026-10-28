#include "ps2/generated.hpp"
#include "ps2/system.hpp"

#include "r5900.hpp"

#include <bit>
#include <cstdio>
#include <set>

// A fallback for code the translation has not seen: a game that loads code the
// exploring runs never met (another path through its menus, a window image
// with other relocations) goes on in this interpreter instead of halting. It
// mirrors what codegen.cpp emits instruction by instruction and hands control
// back to translated code at the first branch target that has some.
namespace ps2 {

bool translated(std::uint32_t pc);

namespace {

using recomp::Flow;
using recomp::Insn;
using enum recomp::Op;

std::uint32_t w(const Cpu &c, std::uint32_t r) { return r ? c.r[r].w[0] : 0u; }
std::uint64_t d(const Cpu &c, std::uint32_t r) { return r ? c.r[r].d[0] : 0ull; }
std::int64_t sd(const Cpu &c, std::uint32_t r) { return r ? c.r[r].sd[0] : 0ll; }
void set_w(Cpu &c, std::uint32_t r, std::uint32_t v) {
    if (r) set32(c.r[r], v);
}
void set_d(Cpu &c, std::uint32_t r, std::uint64_t v) {
    if (r) c.r[r].d[0] = v;
}
std::uint32_t addr(const Cpu &c, const Insn &i) { return w(c, i.rs()) + static_cast<std::uint32_t>(i.simm()); }

bool condition(const Cpu &c, const Insn &i) {
    switch (i.op) {
    case Beq: case Beql: return d(c, i.rs()) == d(c, i.rt());
    case Bne: case Bnel: return d(c, i.rs()) != d(c, i.rt());
    case Blez: case Blezl: return sd(c, i.rs()) <= 0;
    case Bgtz: case Bgtzl: return sd(c, i.rs()) > 0;
    case Bltz: case Bltzl: case Bltzal: case Bltzall: return sd(c, i.rs()) < 0;
    case Bgez: case Bgezl: case Bgezal: case Bgezall: return sd(c, i.rs()) >= 0;
    case Bc0f: case Bc0fl: return !cpcond0(const_cast<Cpu &>(c));
    case Bc0t: case Bc0tl: return cpcond0(const_cast<Cpu &>(c));
    case Bc1f: case Bc1fl: return !(c.fcr31 & kFpuCondition);
    case Bc1t: case Bc1tl: return (c.fcr31 & kFpuCondition) != 0;
    case Bc2f: case Bc2fl: return !vu0_busy(const_cast<Cpu &>(c));
    case Bc2t: case Bc2tl: return vu0_busy(const_cast<Cpu &>(c));
    default: return false;
    }
}

// Executes one instruction without control flow. False when execution left
// the interpreter's hands (a syscall that switched threads, a stop).
bool simple(Cpu &c, const Insn &i) {
    const std::uint32_t rs = i.rs(), rt = i.rt(), rd = i.rd(), sa = i.sa();
    const std::uint32_t fsf = (i.word >> 21) & 3u, ftf = (i.word >> 23) & 3u;
    Reg128 scratch;
    Reg128 &dest = rd ? c.r[rd] : scratch;
    auto &v = c.vu0;
    const std::uint32_t bc = c.vu0.vf[i.ft()].w[i.bc()];
    switch (i.op) {
    case Sll: set_w(c, rd, w(c, rt) << sa); break;
    case Srl: set_w(c, rd, w(c, rt) >> sa); break;
    case Sra: set_w(c, rd, static_cast<std::uint32_t>(static_cast<std::int32_t>(w(c, rt)) >> sa)); break;
    case Sllv: set_w(c, rd, w(c, rt) << (w(c, rs) & 31u)); break;
    case Srlv: set_w(c, rd, w(c, rt) >> (w(c, rs) & 31u)); break;
    case Srav: set_w(c, rd, static_cast<std::uint32_t>(static_cast<std::int32_t>(w(c, rt)) >> (w(c, rs) & 31u))); break;
    case Movz: if (d(c, rt) == 0) set_d(c, rd, d(c, rs)); break;
    case Movn: if (d(c, rt) != 0) set_d(c, rd, d(c, rs)); break;
    case Syscall: {
        const std::uint32_t next = i.pc + 4u;
        c.pc = next;
        syscall(c, i.pc);
        return c.pc == next;
    }
    case Break: breakpoint(c, i.pc); break;
    case Sync: case Cache: case Pref: break;
    case Mfhi: set_d(c, rd, c.hi.d[0]); break;
    case Mflo: set_d(c, rd, c.lo.d[0]); break;
    case Mfhi1: set_d(c, rd, c.hi.d[1]); break;
    case Mflo1: set_d(c, rd, c.lo.d[1]); break;
    case Mthi: c.hi.d[0] = d(c, rs); break;
    case Mtlo: c.lo.d[0] = d(c, rs); break;
    case Mthi1: c.hi.d[1] = d(c, rs); break;
    case Mtlo1: c.lo.d[1] = d(c, rs); break;
    case Dsllv: set_d(c, rd, d(c, rt) << (w(c, rs) & 63u)); break;
    case Dsrlv: set_d(c, rd, d(c, rt) >> (w(c, rs) & 63u)); break;
    case Dsrav: set_d(c, rd, static_cast<std::uint64_t>(sd(c, rt) >> (w(c, rs) & 63u))); break;
    case Mult: mmi::mult(c, 0, w(c, rs), w(c, rt)); set_d(c, rd, c.lo.d[0]); break;
    case Multu: mmi::multu(c, 0, w(c, rs), w(c, rt)); set_d(c, rd, c.lo.d[0]); break;
    case Madd: mmi::madd(c, 0, w(c, rs), w(c, rt)); set_d(c, rd, c.lo.d[0]); break;
    case Maddu: mmi::maddu(c, 0, w(c, rs), w(c, rt)); set_d(c, rd, c.lo.d[0]); break;
    case Mult1: mmi::mult(c, 1, w(c, rs), w(c, rt)); set_d(c, rd, c.lo.d[1]); break;
    case Multu1: mmi::multu(c, 1, w(c, rs), w(c, rt)); set_d(c, rd, c.lo.d[1]); break;
    case Madd1: mmi::madd(c, 1, w(c, rs), w(c, rt)); set_d(c, rd, c.lo.d[1]); break;
    case Maddu1: mmi::maddu(c, 1, w(c, rs), w(c, rt)); set_d(c, rd, c.lo.d[1]); break;
    case Div: mmi::div(c, 0, w(c, rs), w(c, rt)); break;
    case Divu: mmi::divu(c, 0, w(c, rs), w(c, rt)); break;
    case Div1: mmi::div(c, 1, w(c, rs), w(c, rt)); break;
    case Divu1: mmi::divu(c, 1, w(c, rs), w(c, rt)); break;
    case Add: case Addu: set_w(c, rd, w(c, rs) + w(c, rt)); break;
    case Sub: case Subu: set_w(c, rd, w(c, rs) - w(c, rt)); break;
    case And: set_d(c, rd, d(c, rs) & d(c, rt)); break;
    case Or: set_d(c, rd, d(c, rs) | d(c, rt)); break;
    case Xor: set_d(c, rd, d(c, rs) ^ d(c, rt)); break;
    case Nor: set_d(c, rd, ~(d(c, rs) | d(c, rt))); break;
    case Mfsa: set_d(c, rd, c.sa); break;
    case Mtsa: c.sa = w(c, rs) & 15u; break;
    case Mtsab: c.sa = (w(c, rs) ^ i.uimm()) & 15u; break;
    case Mtsah: c.sa = ((w(c, rs) ^ i.uimm()) & 7u) * 2u; break;
    case Slt: set_d(c, rd, sd(c, rs) < sd(c, rt) ? 1ull : 0ull); break;
    case Sltu: set_d(c, rd, d(c, rs) < d(c, rt) ? 1ull : 0ull); break;
    case Dadd: case Daddu: set_d(c, rd, d(c, rs) + d(c, rt)); break;
    case Dsub: case Dsubu: set_d(c, rd, d(c, rs) - d(c, rt)); break;
    case Tge: if (sd(c, rs) >= sd(c, rt)) trap(c, i.pc); break;
    case Tgeu: if (d(c, rs) >= d(c, rt)) trap(c, i.pc); break;
    case Tlt: if (sd(c, rs) < sd(c, rt)) trap(c, i.pc); break;
    case Tltu: if (d(c, rs) < d(c, rt)) trap(c, i.pc); break;
    case Teq: if (d(c, rs) == d(c, rt)) trap(c, i.pc); break;
    case Tne: if (d(c, rs) != d(c, rt)) trap(c, i.pc); break;
    case Tgei: if (sd(c, rs) >= i.simm()) trap(c, i.pc); break;
    case Tgeiu: if (d(c, rs) >= static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm()))) trap(c, i.pc); break;
    case Tlti: if (sd(c, rs) < i.simm()) trap(c, i.pc); break;
    case Tltiu: if (d(c, rs) < static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm()))) trap(c, i.pc); break;
    case Teqi: if (sd(c, rs) == i.simm()) trap(c, i.pc); break;
    case Tnei: if (sd(c, rs) != i.simm()) trap(c, i.pc); break;
    case Dsll: set_d(c, rd, d(c, rt) << sa); break;
    case Dsrl: set_d(c, rd, d(c, rt) >> sa); break;
    case Dsra: set_d(c, rd, static_cast<std::uint64_t>(sd(c, rt) >> sa)); break;
    case Dsll32: set_d(c, rd, d(c, rt) << (sa + 32)); break;
    case Dsrl32: set_d(c, rd, d(c, rt) >> (sa + 32)); break;
    case Dsra32: set_d(c, rd, static_cast<std::uint64_t>(sd(c, rt) >> (sa + 32))); break;
    case Addi: case Addiu: set_w(c, rt, w(c, rs) + static_cast<std::uint32_t>(i.simm())); break;
    case Slti: set_d(c, rt, sd(c, rs) < i.simm() ? 1ull : 0ull); break;
    case Sltiu: set_d(c, rt, d(c, rs) < static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm())) ? 1ull : 0ull); break;
    case Andi: set_d(c, rt, d(c, rs) & i.uimm()); break;
    case Ori: set_d(c, rt, d(c, rs) | i.uimm()); break;
    case Xori: set_d(c, rt, d(c, rs) ^ i.uimm()); break;
    case Lui: if (rt) c.r[rt].sd[0] = static_cast<std::int32_t>(i.uimm() << 16); break;
    case Daddi: case Daddiu: set_d(c, rt, d(c, rs) + static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm()))); break;
    case Ldl: if (rt) ldl(c.r[rt], addr(c, i)); break;
    case Ldr: if (rt) ldr(c.r[rt], addr(c, i)); break;
    case Lwl: if (rt) lwl(c.r[rt], addr(c, i)); break;
    case Lwr: if (rt) lwr(c.r[rt], addr(c, i)); break;
    case Sdl: sdl(c.r[rt], addr(c, i)); break;
    case Sdr: sdr(c.r[rt], addr(c, i)); break;
    case Swl: swl(c.r[rt], addr(c, i)); break;
    case Swr: swr(c.r[rt], addr(c, i)); break;
    case Lq: ld128(addr(c, i) & ~15u, rt ? c.r[rt] : scratch); break;
    case Sq: st128(addr(c, i) & ~15u, c.r[rt]); break;
    case Lb: { const auto x = ld8(addr(c, i)); if (rt) c.r[rt].sd[0] = static_cast<std::int8_t>(x); break; }
    case Lbu: { const auto x = ld8(addr(c, i)); if (rt) c.r[rt].d[0] = x; break; }
    case Lh: { const auto x = ld16(addr(c, i)); if (rt) c.r[rt].sd[0] = static_cast<std::int16_t>(x); break; }
    case Lhu: { const auto x = ld16(addr(c, i)); if (rt) c.r[rt].d[0] = x; break; }
    case Lw: { const auto x = ld32(addr(c, i)); if (rt) set32(c.r[rt], x); break; }
    case Lwu: { const auto x = ld32(addr(c, i)); if (rt) c.r[rt].d[0] = x; break; }
    case Ld: { const auto x = ld64(addr(c, i)); if (rt) c.r[rt].d[0] = x; break; }
    case Sb: st8(addr(c, i), static_cast<std::uint8_t>(w(c, rt))); break;
    case Sh: st16(addr(c, i), static_cast<std::uint16_t>(w(c, rt))); break;
    case Sw: st32(addr(c, i), w(c, rt)); break;
    case Sd: st64(addr(c, i), d(c, rt)); break;
    case Lwc1: c.f[i.ft()] = ld32(addr(c, i)); break;
    case Swc1: st32(addr(c, i), c.f[i.ft()]); break;
    case Lqc2: ld128(addr(c, i) & ~15u, i.ft() ? c.vu0.vf[i.ft()] : scratch); break;
    case Sqc2: st128(addr(c, i) & ~15u, c.vu0.vf[i.ft()]); break;

    case Plzcw: if (rd) mmi::plzcw(c.r[rd], c.r[rs]); break;
    case Pmfhl: if (rd) mmi::pmfhl(c, c.r[rd], sa); break;
    case Pmthl: mmi::pmthl(c, c.r[rs]); break;
    case Psllh: if (rd) mmi::psllh(c.r[rd], c.r[rt], sa); break;
    case Psrlh: if (rd) mmi::psrlh(c.r[rd], c.r[rt], sa); break;
    case Psrah: if (rd) mmi::psrah(c.r[rd], c.r[rt], sa); break;
    case Psllw: if (rd) mmi::psllw(c.r[rd], c.r[rt], sa); break;
    case Psrlw: if (rd) mmi::psrlw(c.r[rd], c.r[rt], sa); break;
    case Psraw: if (rd) mmi::psraw(c.r[rd], c.r[rt], sa); break;
#define MMI3(op, fn) case op: if (rd) mmi::fn(c.r[rd], c.r[rs], c.r[rt]); break;
#define MMI2(op, fn) case op: if (rd) mmi::fn(c.r[rd], c.r[rt]); break;
    MMI3(Paddw, paddw) MMI3(Psubw, psubw) MMI3(Pcgtw, pcgtw) MMI3(Pmaxw, pmaxw) MMI3(Paddh, paddh) MMI3(Psubh, psubh)
    MMI3(Pcgth, pcgth) MMI3(Pmaxh, pmaxh) MMI3(Paddb, paddb) MMI3(Psubb, psubb) MMI3(Pcgtb, pcgtb) MMI3(Paddsw, paddsw)
    MMI3(Psubsw, psubsw) MMI3(Pextlw, pextlw) MMI3(Ppacw, ppacw) MMI3(Paddsh, paddsh) MMI3(Psubsh, psubsh) MMI3(Pextlh, pextlh)
    MMI3(Ppach, ppach) MMI3(Paddsb, paddsb) MMI3(Psubsb, psubsb) MMI3(Pextlb, pextlb) MMI3(Ppacb, ppacb)
    MMI2(Pext5, pext5) MMI2(Ppac5, ppac5) MMI2(Pabsw, pabsw) MMI3(Pceqw, pceqw) MMI3(Pminw, pminw) MMI3(Padsbh, padsbh)
    MMI2(Pabsh, pabsh) MMI3(Pceqh, pceqh) MMI3(Pminh, pminh) MMI3(Pceqb, pceqb) MMI3(Padduw, padduw) MMI3(Psubuw, psubuw)
    MMI3(Pextuw, pextuw) MMI3(Padduh, padduh) MMI3(Psubuh, psubuh) MMI3(Pextuh, pextuh) MMI3(Paddub, paddub)
    MMI3(Psubub, psubub) MMI3(Pextub, pextub) MMI3(Psllvw, psllvw) MMI3(Psrlvw, psrlvw) MMI3(Psravw, psravw)
    MMI3(Pinth, pinth) MMI3(Pinteh, pinteh) MMI3(Pcpyld, pcpyld) MMI3(Pcpyud, pcpyud) MMI3(Pand, pand) MMI3(Pxor, pxor)
    MMI3(Por, por) MMI3(Pnor, pnor) MMI2(Pexeh, pexeh) MMI2(Prevh, prevh) MMI2(Pexew, pexew) MMI2(Prot3w, prot3w)
    MMI2(Pexch, pexch) MMI2(Pcpyh, pcpyh) MMI2(Pexcw, pexcw)
#undef MMI3
#undef MMI2
    case Qfsrv: if (rd) mmi::qfsrv(c.r[rd], c.r[rs], c.r[rt], c.sa); break;
    case Pmaddw: case Pmsubw: mmi::pmaddw(c, dest, c.r[rs], c.r[rt], i.op == Pmsubw); break;
    case Pmadduw: mmi::pmadduw(c, dest, c.r[rs], c.r[rt]); break;
    case Pmultw: mmi::pmultw(c, dest, c.r[rs], c.r[rt]); break;
    case Pmultuw: mmi::pmultuw(c, dest, c.r[rs], c.r[rt]); break;
    case Pmulth: case Pmaddh: case Pmsubh: mmi::pmulth(c, dest, c.r[rs], c.r[rt], i.op == Pmulth ? 0 : i.op == Pmaddh ? 1 : 2); break;
    case Phmadh: case Phmsbh: mmi::phmadh(c, dest, c.r[rs], c.r[rt], i.op == Phmsbh); break;
    case Pdivw: mmi::pdivw(c, c.r[rs], c.r[rt]); break;
    case Pdivuw: mmi::pdivuw(c, c.r[rs], c.r[rt]); break;
    case Pdivbw: mmi::pdivbw(c, c.r[rs], c.r[rt]); break;
    case Pmfhi: if (rd) c.r[rd] = c.hi; break;
    case Pmflo: if (rd) c.r[rd] = c.lo; break;
    case Pmthi: c.hi = c.r[rs]; break;
    case Pmtlo: c.lo = c.r[rs]; break;

    case Mfc0: set_w(c, rt, mfc0(c, rd, i.word & 7u)); break;
    case Mtc0: mtc0(c, rd, w(c, rt)); break;
    case Ei: set_interrupts(c, true); break;
    case Di: set_interrupts(c, false); break;

    case Mfc1: set_w(c, rt, c.f[i.fs()]); break;
    case Mtc1: c.f[i.fs()] = w(c, rt); break;
    case Cfc1: set_w(c, rt, cfc1(c, i.fs())); break;
    case Ctc1: ctc1(c, i.fs(), w(c, rt)); break;
    case AddS: c.f[i.fd()] = fp::add(c.f[i.fs()], c.f[i.ft()]); break;
    case SubS: c.f[i.fd()] = fp::sub(c.f[i.fs()], c.f[i.ft()]); break;
    case MulS: c.f[i.fd()] = fp::mul(c.f[i.fs()], c.f[i.ft()]); break;
    case DivS: fpu_div(c, i.fd(), c.f[i.fs()], c.f[i.ft()]); break;
    case SqrtS: c.f[i.fd()] = fp::sqrt(c.f[i.ft()]); break;
    case AbsS: c.f[i.fd()] = c.f[i.fs()] & fp::kAbs; break;
    case MovS: c.f[i.fd()] = c.f[i.fs()]; break;
    case NegS: c.f[i.fd()] = c.f[i.fs()] ^ fp::kSign; break;
    case RsqrtS: fpu_rsqrt(c, i.fd(), c.f[i.fs()], c.f[i.ft()]); break;
    case AddaS: c.facc = fp::add(c.f[i.fs()], c.f[i.ft()]); break;
    case SubaS: c.facc = fp::sub(c.f[i.fs()], c.f[i.ft()]); break;
    case MulaS: c.facc = fp::mul(c.f[i.fs()], c.f[i.ft()]); break;
    case MaddS: c.f[i.fd()] = fp::add(c.facc, fp::mul(c.f[i.fs()], c.f[i.ft()])); break;
    case MsubS: c.f[i.fd()] = fp::sub(c.facc, fp::mul(c.f[i.fs()], c.f[i.ft()])); break;
    case MaddaS: c.facc = fp::add(c.facc, fp::mul(c.f[i.fs()], c.f[i.ft()])); break;
    case MsubaS: c.facc = fp::sub(c.facc, fp::mul(c.f[i.fs()], c.f[i.ft()])); break;
    case CvtWS: c.f[i.fd()] = fp::cvt_w_s(c.f[i.fs()]); break;
    case CvtSW: c.f[i.fd()] = fp::cvt_s_w(c.f[i.fs()]); break;
    case MaxS: c.f[i.fd()] = fp::max(c.f[i.fs()], c.f[i.ft()]); break;
    case MinS: c.f[i.fd()] = fp::min(c.f[i.fs()], c.f[i.ft()]); break;
    case CF: fpu_compare(c, false); break;
    case CEq: fpu_compare(c, fp::key(c.f[i.fs()]) == fp::key(c.f[i.ft()])); break;
    case CLt: fpu_compare(c, fp::key(c.f[i.fs()]) < fp::key(c.f[i.ft()])); break;
    case CLe: fpu_compare(c, fp::key(c.f[i.fs()]) <= fp::key(c.f[i.ft()])); break;

    case Qmfc2:
        if (i.word & 1u) vu0_wait(c);
        if (rt) c.r[rt] = v.vf[i.fs()];
        break;
    case Qmtc2:
        if (i.word & 1u) vu0_wait(c);
        if (i.fs()) v.vf[i.fs()] = c.r[rt];
        break;
    case Cfc2: set_w(c, rt, i.fs() < 16 ? v.vi[i.fs()] : cfc2(c, i.fs())); break;
    case Ctc2:
        if (i.fs() < 16) vu::set_vi(v, i.fs(), w(c, rt));
        else ctc2(c, i.fs(), w(c, rt));
        break;
    case Vaddbc: vu::addbc(v, i.dest(), i.fd(), i.fs(), bc); break;
    case Vsubbc: vu::subbc(v, i.dest(), i.fd(), i.fs(), bc); break;
    case Vmaddbc: vu::maddbc(v, i.dest(), i.fd(), i.fs(), bc); break;
    case Vmsubbc: vu::msubbc(v, i.dest(), i.fd(), i.fs(), bc); break;
    case Vmaxbc: vu::maxbc(v, i.dest(), i.fd(), i.fs(), bc); break;
    case Vminibc: vu::minibc(v, i.dest(), i.fd(), i.fs(), bc); break;
    case Vmulbc: vu::mulbc(v, i.dest(), i.fd(), i.fs(), bc); break;
    case Vmulq: vu::mulbc(v, i.dest(), i.fd(), i.fs(), v.q); break;
    case Vmaxi: vu::maxbc(v, i.dest(), i.fd(), i.fs(), v.i); break;
    case Vmuli: vu::mulbc(v, i.dest(), i.fd(), i.fs(), v.i); break;
    case Vminii: vu::minibc(v, i.dest(), i.fd(), i.fs(), v.i); break;
    case Vaddq: vu::addbc(v, i.dest(), i.fd(), i.fs(), v.q); break;
    case Vmaddq: vu::maddbc(v, i.dest(), i.fd(), i.fs(), v.q); break;
    case Vaddi: vu::addbc(v, i.dest(), i.fd(), i.fs(), v.i); break;
    case Vmaddi: vu::maddbc(v, i.dest(), i.fd(), i.fs(), v.i); break;
    case Vsubq: vu::subbc(v, i.dest(), i.fd(), i.fs(), v.q); break;
    case Vmsubq: vu::msubbc(v, i.dest(), i.fd(), i.fs(), v.q); break;
    case Vsubi: vu::subbc(v, i.dest(), i.fd(), i.fs(), v.i); break;
    case Vmsubi: vu::msubbc(v, i.dest(), i.fd(), i.fs(), v.i); break;
    case Vadd: vu::add(v, i.dest(), i.fd(), i.fs(), i.ft()); break;
    case Vmadd: vu::madd(v, i.dest(), i.fd(), i.fs(), i.ft()); break;
    case Vmul: vu::mul(v, i.dest(), i.fd(), i.fs(), i.ft()); break;
    case Vmax: vu::max(v, i.dest(), i.fd(), i.fs(), i.ft()); break;
    case Vsub: vu::sub(v, i.dest(), i.fd(), i.fs(), i.ft()); break;
    case Vmsub: vu::msub(v, i.dest(), i.fd(), i.fs(), i.ft()); break;
    case Vmini: vu::mini(v, i.dest(), i.fd(), i.fs(), i.ft()); break;
    case Vopmsub: vu::opmsub(v, i.fd(), i.fs(), i.ft()); break;
    case Viadd: vu::set_vi(v, i.fd(), v.vi[i.fs()] + v.vi[i.ft()]); break;
    case Visub: vu::set_vi(v, i.fd(), v.vi[i.fs()] - v.vi[i.ft()]); break;
    case Viaddi: vu::set_vi(v, i.ft(), v.vi[i.fs()] + static_cast<std::uint32_t>(i.imm5())); break;
    case Viand: vu::set_vi(v, i.fd(), v.vi[i.fs()] & v.vi[i.ft()]); break;
    case Vior: vu::set_vi(v, i.fd(), v.vi[i.fs()] | v.vi[i.ft()]); break;
    case Vcallms: vcallms(c, i.imm15() * 8u); break;
    case Vcallmsr: vcallms(c, v.cmsar0 * 8u); break;
    case Vaddabc: vu::addabc(v, i.dest(), i.fs(), bc); break;
    case Vsubabc: vu::subabc(v, i.dest(), i.fs(), bc); break;
    case Vmaddabc: vu::maddabc(v, i.dest(), i.fs(), bc); break;
    case Vmsubabc: vu::msubabc(v, i.dest(), i.fs(), bc); break;
    case Vmulabc: vu::mulabc(v, i.dest(), i.fs(), bc); break;
    case Vmulaq: vu::mulabc(v, i.dest(), i.fs(), v.q); break;
    case Vmulai: vu::mulabc(v, i.dest(), i.fs(), v.i); break;
    case Vaddaq: vu::addabc(v, i.dest(), i.fs(), v.q); break;
    case Vmaddaq: vu::maddabc(v, i.dest(), i.fs(), v.q); break;
    case Vaddai: vu::addabc(v, i.dest(), i.fs(), v.i); break;
    case Vmaddai: vu::maddabc(v, i.dest(), i.fs(), v.i); break;
    case Vsubaq: vu::subabc(v, i.dest(), i.fs(), v.q); break;
    case Vmsubaq: vu::msubabc(v, i.dest(), i.fs(), v.q); break;
    case Vsubai: vu::subabc(v, i.dest(), i.fs(), v.i); break;
    case Vmsubai: vu::msubabc(v, i.dest(), i.fs(), v.i); break;
    case Vadda: vu::adda(v, i.dest(), i.fs(), i.ft()); break;
    case Vmadda: vu::madda(v, i.dest(), i.fs(), i.ft()); break;
    case Vmula: vu::mula(v, i.dest(), i.fs(), i.ft()); break;
    case Vsuba: vu::suba(v, i.dest(), i.fs(), i.ft()); break;
    case Vmsuba: vu::msuba(v, i.dest(), i.fs(), i.ft()); break;
    case Vopmula: vu::opmula(v, i.fs(), i.ft()); break;
    case Vitof0: vu::itof(v, i.dest(), i.ft(), i.fs(), 0); break;
    case Vitof4: vu::itof(v, i.dest(), i.ft(), i.fs(), 4); break;
    case Vitof12: vu::itof(v, i.dest(), i.ft(), i.fs(), 12); break;
    case Vitof15: vu::itof(v, i.dest(), i.ft(), i.fs(), 15); break;
    case Vftoi0: vu::ftoi(v, i.dest(), i.ft(), i.fs(), 0); break;
    case Vftoi4: vu::ftoi(v, i.dest(), i.ft(), i.fs(), 4); break;
    case Vftoi12: vu::ftoi(v, i.dest(), i.ft(), i.fs(), 12); break;
    case Vftoi15: vu::ftoi(v, i.dest(), i.ft(), i.fs(), 15); break;
    case Vabs: vu::abs(v, i.dest(), i.ft(), i.fs()); break;
    case Vmove: vu::move(v, i.dest(), i.ft(), i.fs()); break;
    case Vmr32: vu::mr32(v, i.dest(), i.ft(), i.fs()); break;
    case Vclip: vu::clip(v, i.fs(), i.ft()); break;
    case Vnop: case Vwaitq: break;
    case Vlqi: lqc2_vu(c, i.dest(), i.ft(), v.vi[i.fs()]); vu::set_vi(v, i.fs(), v.vi[i.fs()] + 1u); break;
    case Vsqi: sqc2_vu(c, i.dest(), i.fs(), v.vi[i.ft()]); vu::set_vi(v, i.ft(), v.vi[i.ft()] + 1u); break;
    case Vlqd: vu::set_vi(v, i.fs(), v.vi[i.fs()] - 1u); lqc2_vu(c, i.dest(), i.ft(), v.vi[i.fs()]); break;
    case Vsqd: vu::set_vi(v, i.ft(), v.vi[i.ft()] - 1u); sqc2_vu(c, i.dest(), i.fs(), v.vi[i.ft()]); break;
    case Vdiv: vu::div(v, v.vf[i.fs()].w[fsf], v.vf[i.ft()].w[ftf]); break;
    case Vsqrt: vu::sqrt(v, v.vf[i.ft()].w[ftf]); break;
    case Vrsqrt: vu::rsqrt(v, v.vf[i.fs()].w[fsf], v.vf[i.ft()].w[ftf]); break;
    case Vmtir: vu::set_vi(v, i.ft(), v.vf[i.fs()].w[fsf]); break;
    case Vmfir: vu::mfir(v, i.dest(), i.ft(), i.fs()); break;
    case Vilwr: {
        std::uint16_t x;
        __builtin_memcpy(&x, vu0_data() + vu0_addr(v.vi[i.fs()]) + 4u * static_cast<std::uint32_t>(std::countl_zero(i.dest() << 28)), 2);
        vu::set_vi(v, i.ft(), x);
        break;
    }
    case Viswr:
        for (int k = 0; k < 4; ++k)
            if (vu::lane(i.dest(), k)) {
                const std::uint32_t x = v.vi[i.ft()];
                __builtin_memcpy(vu0_data() + vu0_addr(v.vi[i.fs()]) + 4u * k, &x, 4);
            }
        break;
    case Vrnext:
        v.r = vu::rand_next(v.r);
        vu::apply(v, i.dest(), i.ft(), [&](int) { return v.r | fp::kOne; });
        break;
    case Vrget: vu::apply(v, i.dest(), i.ft(), [&](int) { return v.r | fp::kOne; }); break;
    case Vrinit: v.r = v.vf[i.fs()].w[fsf] & 0x7FFFFFu; break;
    case Vrxor: v.r = (v.r ^ v.vf[i.fs()].w[fsf]) & 0x7FFFFFu; break;

    default:
        stop(c, i.pc, recomp::name(i.op));
        return false;
    }
    return true;
}

std::set<std::uint32_t> g_reported;

}

void interpret(Cpu &c, std::uint32_t, std::uint32_t depth) {
    std::uint32_t pc = c.pc;
    if (g_reported.insert(pc).second) std::fprintf(stderr, "[ee] interpreting untranslated code at 0x%08X (ra 0x%08X)\n", pc, c.r[31].w[0]);
    for (bool check = false;; check = true) {
        // back to translated code at the first branch target that has some
        if (check && translated(pc)) {
            c.pc = pc;
            [[clang::musttail]] return dispatch(c, 0u, depth);
        }
        for (;;) {
            const Insn i = recomp::decode(ld32(pc), pc);
            const Flow fl = recomp::flow(i.op);
            --c.budget;
            if (fl == Flow::None) {
                if (!simple(c, i)) return;
                pc += 4u;
                continue;
            }
            if (i.op == Eret) {
                c.pc = eret(c);
                [[clang::musttail]] return dispatch(c, 0u, depth);
            }
            if (fl == Flow::Stop) {
                stop(c, pc, recomp::name(i.op));
                return;
            }
            const Insn ds = recomp::decode(ld32(pc + 4u), pc + 4u);
            if (ds.op == Invalid || recomp::has_delay_slot(ds.op) || ds.op == Syscall) {
                stop(c, pc + 4u, "unsupported delay slot");
                return;
            }
            --c.budget;
            const std::uint32_t ret = pc + 8u, t = i.branch_target_of();
            auto slot = [&] { return simple(c, ds); };
            auto link = [&](std::uint32_t r) { if (r) c.r[r].sd[0] = static_cast<std::int32_t>(ret); };
            auto taken = [&](std::uint32_t target) {
                // the preemption point translated code has on backward branches
                if (target <= i.pc && c.budget < 0 && preempt(c)) {
                    c.pc = target | kUnwind;
                    return false;
                }
                pc = target;
                return true;
            };
            switch (i.op) {
            case Jal:
                link(31);
                if (!slot()) return;
                call(c, t, depth + 1u);
                if (c.pc != ret) return;
                pc = ret;
                break;
            case Bltzal: case Bgezal: {
                const bool go = condition(c, i);
                link(31);
                if (!slot()) return;
                if (go) {
                    call(c, t, depth + 1u);
                    if (c.pc != ret) return;
                }
                pc = ret;
                break;
            }
            case Bltzall: case Bgezall: {
                const bool go = condition(c, i);
                link(31);
                if (go) {
                    if (!slot()) return;
                    call(c, t, depth + 1u);
                    if (c.pc != ret) return;
                }
                pc = ret;
                break;
            }
            case Jalr: {
                const std::uint32_t target = w(c, i.rs());
                link(i.rd());
                if (!slot()) return;
                c.pc = target;
                call(c, target, depth + 1u);
                if (c.pc != ret) return;
                pc = ret;
                break;
            }
            case Jr: {
                const std::uint32_t target = w(c, i.rs());
                if (!slot()) return;
                c.pc = target;
                if (i.rs() == 31) return;
                [[clang::musttail]] return dispatch(c, 0u, depth);
            }
            case J:
                if (!slot()) return;
                if (!taken(t)) return;
                break;
            default:
                if (fl == Flow::BranchLikely) {
                    if (condition(c, i)) {
                        if (!slot()) return;
                        if (!taken(t)) return;
                    } else {
                        pc = ret;
                    }
                } else {
                    const bool go = condition(c, i);
                    if (!slot()) return;
                    if (go) {
                        if (!taken(t)) return;
                    } else {
                        pc = ret;
                    }
                }
                break;
            }
            break;
        }
    }
}

}
