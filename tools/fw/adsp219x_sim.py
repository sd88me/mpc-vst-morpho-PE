#!/usr/bin/env python3
"""ADSP-219x interpreter for DSP 3.5 routines (development tool; works on your own DSP update file, ships nothing).

A reference rig, not an emulator of the chip: it runs a routine of the user's DSP image on a memory state the caller sets up
(parameters, accumulators), so the firmware's own arithmetic can be measured instead of guessed. No peripherals, interrupts or
secondary register set; the I/O and system registers read as zero. Instruction decoding follows adsp219x_dis.py.

    from adsp219x_sim import Sim
    sim = Sim.from_syx("Poly_Evolver_DSP_3.5.syx")
    sim.dm[0xf813] = 0xf900              # set up memory
    sim.run(0x15cb)                      # run until the routine returns
    print(sim.dm.get(0xf70c))
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pe_fw  # noqa: E402
from adsp219x_dis import (COND, G0, G1, G2, G3, GRP, IREG, SF, XOP_ALU, XOP_MAC, XOP_SH, YOP_ALU, YOP_MAC, k4, sx)  # noqa: E402

M16 = 0xFFFF
M40 = (1 << 40) - 1


def s16(x):
    x &= M16
    return x - 0x10000 if x & 0x8000 else x


def s40(x):
    x &= M40
    return x - (1 << 40) if x >> 39 else x


class Sim:
    def __init__(self, mem):
        self.pm = dict(mem)             # 24-bit words by address (program memory and the 24-bit table block)
        self.dm = {}                    # 16-bit data memory (addresses not in pm)
        self.r = {n: 0 for n in G0 + G1 + G2 + G3 if n != "?"}
        self.r.update({"AF": 0})
        self.mr = 0
        self.sr = 0
        self.base = {}                  # circular buffer base per I register
        self.flags = {"AZ": 0, "AN": 0, "AV": 0, "AC": 0, "MV": 0}
        self.sat = False
        self.integer = False
        self.pcstack = []
        self.loops = []                 # [start, end, count]
        self.steps = 0

    @classmethod
    def from_syx(cls, path):
        t, img = pe_fw.unpack_fw(path)
        return cls(pe_fw.dsp_load(img)[1])

    # ---- memory ----
    def dmr(self, a):
        a &= M16
        if a in self.dm:
            return self.dm[a]
        return (self.pm[a] >> 8) & M16 if a in self.pm else 0

    def dmw(self, a, v):
        self.dm[a & M16] = v & M16

    def pmr(self, a):
        w = self.pm.get(a & 0xFFFFFF, 0)
        self.r["PX"] = w & 0xFF
        return (w >> 8) & M16

    def pmw(self, a, v):
        self.pm[a & 0xFFFFFF] = ((v & M16) << 8) | (self.r["PX"] & 0xFF)

    # ---- registers ----
    def rd(self, n):
        if n in ("MR0", "MR1", "MR2", "SR0", "SR1", "SR2"):
            v = self.mr if n[0] == "M" else self.sr
            sh = {"0": 0, "1": 16, "2": 32}[n[2]]
            x = (v >> sh) & (M16 if n[2] != "2" else 0xFF)
            return x if n[2] != "2" else (x | 0xFF00 if x & 0x80 else x)
        if n == "0":
            return 0
        return self.r[n]

    def wr(self, n, v):
        v &= M16
        if n in ("MR0", "MR1", "MR2", "SR0", "SR1", "SR2"):
            sh = {"0": 0, "1": 16, "2": 32}[n[2]]
            m = (M16 if n[2] != "2" else 0xFF) << sh
            cur = self.mr if n[0] == "M" else self.sr
            cur = (cur & ~m) | ((v & (m >> sh)) << sh)
            if n[2] == "1" or n[2] == "0":
                pass
            if n[0] == "M":
                self.mr = cur & M40
            else:
                self.sr = cur & M40
            return
        if n[0] == "I" and n[1:].isdigit():
            self.base[n] = v
        self.r[n] = v

    def reg(self, grp, i):
        return GRP[grp][i]

    # ---- flags / conditions ----
    def cond(self, c):
        f = self.flags
        an, av, az = f["AN"], f["AV"], f["AZ"]
        return {0: az, 1: not az, 2: not ((an ^ av) or az), 3: (an ^ av) or az, 4: an ^ av, 5: not (an ^ av), 6: av, 7: not av,
                8: f["AC"], 9: not f["AC"], 10: 0, 11: 1, 12: f["MV"], 13: not f["MV"], 14: bool(self.loops and self.loops[-1][2] > 1),
                15: 1}[c]

    # ---- ALU ----
    def alu(self, amf, x, y):
        x &= M16
        y &= M16
        cin = self.flags["AC"]
        f = self.flags
        add = lambda a, b, c: self._addf(a, b, c)
        if amf == 0x10: r = y; f["AC"] = f["AV"] = 0
        elif amf == 0x11: r = add(y, 0, 1)
        elif amf == 0x12: r = add(x, y, cin)
        elif amf == 0x13: r = add(x, y, 0)
        elif amf == 0x14: r = ~y & M16; f["AC"] = f["AV"] = 0
        elif amf == 0x15: r = add(0, ~y & M16, 1)
        elif amf == 0x16: r = add(x, ~y & M16, cin)
        elif amf == 0x17: r = add(x, ~y & M16, 1)
        elif amf == 0x18: r = add(y, M16, 0)
        elif amf == 0x19: r = add(y, ~x & M16, 1)
        elif amf == 0x1A: r = add(y, ~x & M16, cin)
        elif amf == 0x1B: r = ~x & M16; f["AC"] = f["AV"] = 0
        elif amf == 0x1C: r = x & y; f["AC"] = f["AV"] = 0
        elif amf == 0x1D: r = x | y; f["AC"] = f["AV"] = 0
        elif amf == 0x1E: r = x ^ y; f["AC"] = f["AV"] = 0
        else:
            r = add(0, ~x & M16, 1) if x & 0x8000 else x
            f["AC"] = 0
            f["AV"] = 1 if x == 0x8000 else 0
        if f["AV"] and self.sat and amf not in (0x14, 0x1B, 0x1C, 0x1D, 0x1E):
            r = 0x7FFF if r & 0x8000 else 0x8000
        f["AZ"] = 1 if r == 0 else 0
        f["AN"] = 1 if r & 0x8000 else 0
        return r

    def _addf(self, a, b, c):
        t = a + b + c
        self.flags["AC"] = 1 if t > M16 else 0
        r = t & M16
        self.flags["AV"] = 1 if ((a ^ r) & (b ^ r) & 0x8000) else 0
        return r

    # ---- MAC ----
    def mac(self, amf, z, x, y):
        if amf == 0:
            return
        mode = (0, 0, 0, 0, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3)[amf]      # 0 SS, 1 SU, 2 US, 3 UU
        acc = 0 if amf in (1, 4, 5, 6, 7) else (self.sr if z else self.mr)
        sub = amf in (3, 12, 13, 14, 15)
        xs = s16(x) if mode in (0, 1) else x & M16
        ys = s16(y) if mode in (0, 2) else y & M16
        p = xs * ys
        if not self.integer:
            p <<= 1
        v = s40(acc) + (-p if sub else p) if amf not in (1, 4, 5, 6, 7) else p
        if amf in (1, 2, 3):
            v += 0x8000
        self.flags["MV"] = 1 if not (-(1 << 31) <= s40(v) < (1 << 31)) else 0
        if z:
            self.sr = v & M40
        else:
            self.mr = v & M40

    def sat_reg(self, which):
        v = s40(self.sr if which else self.mr)
        if not (-(1 << 31) <= v < (1 << 31)):
            v = (1 << 31) - 1 if v > 0 else -(1 << 31)
            if which:
                self.sr = v & M40
            else:
                self.mr = v & M40

    # ---- shifter ----
    def shift(self, sf, x, amt):
        x &= M16
        name = SF[sf]
        hi = " HI" in name
        orr = name.endswith("OR")
        if name.startswith("LSHIFT"):
            v = (x << 16) if hi else x
            v = (v << amt) if amt >= 0 else (v >> -amt)
            v &= 0xFFFFFFFF
        elif name.startswith("ASHIFT"):
            v = s16(x) << 16 if hi else s16(x)
            v = (v << amt) if amt >= 0 else (v >> -amt)
            v &= M40
        else:
            raise NotImplementedError(name)
        self.sr = ((self.sr | v) if orr else v) & M40

    # ---- DAG ----
    def mod(self, ireg, mreg, post=True, imm=None):
        i = self.r[ireg]
        m = s16(self.r[mreg]) if imm is None else imm
        n = ireg[1:]
        ln = self.r["L" + n]
        if ln:
            base = self.base.get(ireg, 0)
            new = i + m
            if new >= base + ln:
                new -= ln
            elif new < base:
                new += ln
            return new & M16
        return (i + m) & M16

    def mem_access(self, ireg, mreg, kind, write, reg, post, imm=None):
        i = self.r[ireg]
        newi = self.mod(ireg, mreg, True, imm)
        addr = i if post else newi
        if kind == "DM":
            if write:
                self.dmw(addr, self.rd(reg))
            else:
                self.wr(reg, self.dmr(addr))
        else:
            if write:
                self.pmw(addr, self.rd(reg))
            else:
                self.wr(reg, self.pmr(addr))
        if post:
            self.r[ireg] = newi

    # ---- execution ----
    def run(self, pc, maxsteps=2000000):
        self.pc = pc
        self.pcstack.append(None)
        self.delay = None
        depth = len(self.pcstack)
        while self.steps < maxsteps:
            self.steps += 1
            if not self.step():
                return
        raise RuntimeError("step limit")

    def step(self):
        pc = self.pc
        w = self.pm.get(pc, 0)
        n = 1
        jump = None
        db = False
        call = False
        ret = False
        b = lambda hi, lo=None: (w >> (lo if lo is not None else hi)) & ((1 << ((hi - lo + 1) if lo is not None else 1)) - 1)
        top = w >> 16

        def xyops(amf, xop, yop):
            mac = amf < 16
            return (XOP_MAC if mac else XOP_ALU)[xop], (YOP_MAC if mac else YOP_ALU)[yop]

        def compute(amf, z, xn, yn, store=True):
            if amf == 0:
                return
            if amf < 16:
                self.mac(amf, z, self.rd(xn), self.rd(yn))
            else:
                r = self.alu(amf, self.rd(xn), self.rd(yn))
                if store:
                    self.wr("AF" if z else "AR", r)

        if w >> 22 == 3:                                                  # type 1
            amf = b(17, 13)
            xn, yn = xyops(amf, b(10, 8), b(12, 11))
            c_args = (amf, 0, xn, yn)
            dd = ["AX0", "AX1", "MX0", "MX1"][b(19, 18)]
            pd = ["AY0", "AY1", "MY0", "MY1"][b(21, 20)]
            compute(*c_args)
            self.mem_access("I%d" % b(3, 2), "M%d" % b(1, 0), "DM", 0, dd, True)
            self.mem_access("I%d" % (4 + b(7, 6)), "M%d" % (4 + b(5, 4)), "PM", 0, pd, True)
        elif w >> 21 in (4, 5):                                           # type 3
            t3 = w >> 21
            r = G0[b(3, 0)] if t3 == 4 else IREG[b(3, 0)]
            a = b(19, 4)
            if b(20):
                self.dmw(a, self.rd(r))
            else:
                self.wr(r, self.dmr(a))
        elif w >> 21 == 3:                                                # type 4
            amf = b(17, 13)
            xn, yn = xyops(amf, b(10, 8), b(12, 11))
            g = 4 * b(20)
            reg = G0[b(7, 4)]
            if b(19):                                                      # write: the compute happens first, memory reads old reg
                self.mem_access("I%d" % (g + b(3, 2)), "M%d" % (g + b(1, 0)), "DM", 1, reg, True)
                compute(amf, b(18), xn, yn)
            else:
                compute(amf, b(18), xn, yn)
                self.mem_access("I%d" % (g + b(3, 2)), "M%d" % (g + b(1, 0)), "DM", 0, reg, True)
        elif w >> 20 == 4:
            self.wr(G0[b(3, 0)], b(19, 4))
        elif w >> 20 == 5:
            self.wr(G1[b(3, 0)], b(19, 4))
        elif w >> 20 == 3:
            self.wr(G2[b(3, 0)], b(19, 4))
        elif w >> 19 == 5:                                                # type 8
            amf = b(17, 13)
            xn, yn = xyops(amf, b(10, 8), b(12, 11))
            if (w & 0xFF) == 0xAA:
                compute(amf, b(18), xn, yn, False)                           # status only
            else:
                src = self.rd(G0[b(3, 0)])
                compute(amf, b(18), xn, yn)
                self.wr(G0[b(7, 4)], src)
        elif w >> 19 == 4:                                                # types 9, 9a
            amf, z = b(17, 13), b(18)
            if b(5, 4) == 2:
                xn = G0[b(11, 8)]
                yn = G0[b(3, 0)] if not b(12) else "0"
                compute(amf, z, xn, yn)
            else:
                mac = amf < 16
                xn = (XOP_MAC if mac else XOP_ALU)[b(10, 8)]
                if self.cond(b(3, 0)):
                    if b(7, 4) == 0:
                        yn = (YOP_MAC if mac else YOP_ALU)[b(12, 11)]
                        compute(amf, z, xn, yn)
                    elif b(7, 4) == 1 and mac:
                        self.mac(amf, z, self.rd(xn), self.rd(xn))
                    else:
                        k = k4((b(12, 11) << 2) | b(7, 6), b(5, 4)) & M16
                        if amf < 16:
                            self.mac(amf, z, self.rd(xn), k)
                        else:
                            self.wr("AF" if z else "AR", self.alu(amf, self.rd(xn), k))
        elif top == 0x10:
            self.wr(G3[b(3, 0)], b(15, 4))
        elif w >> 17 == 0x09:                                             # type 12
            sf = b(15, 12)
            g = 4 * b(16)
            xn = XOP_SH[b(10, 8)]
            amt = s16(self.r["SE"] << 8) >> 8 if False else sx(self.r["SE"] & 0xFF, 8)
            self.shift(sf, self.rd(xn), amt)
            self.mem_access("I%d" % (g + b(3, 2)), "M%d" % (g + b(1, 0)), "DM", b(11), G0[b(7, 4)], True)
        elif top == 0x14:
            amt = sx(self.r["SE"] & 0xFF, 8)
            x = self.rd(G0[b(11, 8)])
            self.shift(b(15, 12), x, amt)
            self.wr(G0[b(7, 4)], self.rd(G0[b(3, 0)]))
        elif top == 0x15:
            g = 4 * b(13)
            if b(12, 11) == 3:
                raise NotImplementedError("type 32a")
            kind = "PM" if b(15) else "DM"
            self.mem_access("I%d" % (g + b(3, 2)), "M%d" % (g + b(1, 0)), kind, b(12), GRP[b(9, 8)][b(7, 4)], bool(b(14)))
        elif top == 0x16:                                                 # DO UNTIL
            end = (pc + sx(b(15, 4), 12)) & M16
            self.loops.append([pc + 1, end, self.r["CNTR"] if b(3, 0) == 14 else 1 << 30])
        elif w >> 18 == 0x06:                                             # type 10
            if self.cond(b(3, 0)):
                jump = (pc + sx(b(16, 4), 13)) & M16
                db = bool(b(17))
        elif w >> 18 == 0x07:                                             # type 10a
            a = sx((b(1, 0) << 14) | b(17, 4), 16)
            jump = (pc + a) & M16
            call = bool(b(2))
            db = bool(b(3))
        elif top == 0x0F:
            self.shift(b(15, 12), self.rd(G0[b(11, 8)]), sx(b(7, 0), 8))
        elif top == 0x0E:
            if self.cond(b(3, 0)):
                self.shift(b(15, 12), self.rd(G0[b(11, 8)]), sx(self.r["SE"] & 0xFF, 8))
        elif top == 0x0D:
            self.wr(GRP[b(11, 10)][b(7, 4)], self.rd(GRP[b(9, 8)][b(3, 0)]))
        elif top == 0x0C:
            for name, sh in (("sat", 10), ("integer", 12)):
                v = b(sh + 1, sh)
                if v == 3:
                    setattr(self, name, True)
                elif v == 2:
                    setattr(self, name, False)
        elif top == 0x0B:
            if self.cond(b(7, 4)):
                jump = self.r["I%d" % (4 * b(13) + b(3, 2))]
                call = bool(b(14))
                db = bool(b(15))
        elif top == 0x0A:
            if self.cond(b(7, 4)):
                ret = True
                db = bool(b(15))
        elif w >> 17 == 0x04 and top in (0x08, 0x09):                     # type 29
            g = 4 * b(13)
            reg = G0[(b(15, 14) << 2) | b(1, 0)]
            self.mem_access("I%d" % (g + b(3, 2)), None, "DM", b(12), reg, bool(b(16)), imm=sx(b(11, 4), 8))
        elif top == 0x07:
            if b(15):
                g = 4 * b(13)
                w2 = self.pm.get(pc + 1, 0)
                ir, mr = "I%d" % (g + b(3, 2)), "M%d" % (g + b(1, 0))
                if b(14):
                    val = ((w2 >> 12 & 0xFF) << 16) | (b(11, 4) << 8) | (w2 >> 4 & 0xFF)
                    self.pm[self.r[ir]] = val
                else:
                    self.dmw(self.r[ir], ((w2 >> 12 & 0xFF) << 8) | b(11, 4))
                self.r[ir] = self.mod(ir, mr)
                n = 2
        elif top == 0x06:
            pass                                                            # IO / REG access: reads give zero
        elif top == 0x05:
            w2 = self.pm.get(pc + 1, 0)
            n = 2
            if self.cond(b(3, 0)):
                jump = (b(11, 4) << 16) | (w2 >> 4 & 0xFFFF)
                call = bool(b(12))
        elif top == 0x03:
            if b(15) == 0:
                self.sat_reg(b(12))
            else:
                raise NotImplementedError("DIVS/DIVQ")
        elif top == 0x01:
            g = 4 * b(13)
            if b(15):
                ir, mr = "I%d" % (g + b(3, 2)), "M%d" % (g + b(1, 0))
                self.r[ir] = self.mod(ir, mr)
            else:
                ir = "I%d" % (g + b(3, 2))
                self.r[ir] = self.mod(ir, None, imm=sx(b(11, 4), 8))
        elif top in (0x00, 0x02, 0x04):
            pass
        else:
            raise NotImplementedError("opcode %06x at %04x" % (w, pc))

        nxt = pc + n
        end_of_loop = bool(self.loops) and nxt - 1 == self.loops[-1][1] and not (jump is not None and not db)
        if self.delay:
            self.delay[1] -= 1
            if self.delay[1] == 0:
                tgt, _, iscall, rets = self.delay
                self.delay = None
                if tgt is None:
                    a = self.pcstack.pop()
                    if a is None:
                        return False
                    self.pc = a
                    return True
                if iscall:
                    self.pcstack.append(rets)
                self.pc = tgt
                return True
        if jump is not None and db:
            self.delay = [jump, 2, call, nxt + 2]
            self.pc = nxt
            return True
        if ret:
            if db:
                self.delay = [None, 2, False, 0]
            else:
                a = self.pcstack.pop()
                if a is None:
                    return False
                self.pc = a
                return True
        if jump is not None:
            if call:
                self.pcstack.append(nxt)
            self.pc = jump
            return True
        if end_of_loop:
            lp = self.loops[-1]
            lp[2] -= 1
            if lp[2] > 0:
                self.pc = lp[0]
                return True
            self.loops.pop()
        self.pc = nxt
        return True


if __name__ == "__main__":
    sys.exit(__doc__)
