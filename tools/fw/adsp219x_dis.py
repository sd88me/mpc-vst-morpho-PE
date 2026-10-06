#!/usr/bin/env python3
"""ADSP-219x disassembler (development tool; works on your own DSP update file, ships nothing).

    adsp219x_dis.py DSP.syx [FROM [TO]]        disassemble program memory FROM..TO (word addresses, default the whole image)
    adsp219x_dis.py DSP.syx --refs LO HI       list the instructions that load a constant in LO..HI (table references)

The loader is pe_fw.py's. Encodings follow the ADSP-219x DSP Instruction Set Reference, chapter 8 (instruction types 1-37, AMF,
condition, constant and register tables). Unknown words print as ".word". Not decoded in detail: types 18, 26, 31 (mode change,
stack, idle) print their raw fields.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pe_fw  # noqa: E402

G0 = "AX0 AX1 MX0 MX1 AY0 AY1 MY0 MY1 MR2 SR2 AR SI MR1 SR1 MR0 SR0".split()
G1 = "I0 I1 I2 I3 M0 M1 M2 M3 L0 L1 L2 L3 IMASK IRPTL ICNTL STACKA".split()
G2 = "I4 I5 I6 I7 M4 M5 M6 M7 L4 L5 L6 L7 ? ? CNTR LPSTACKA".split()
G3 = "ASTAT MSTAT SSTAT LPSTACKP CCODE SE SB PX DMPG1 DMPG2 IOPG IJPG ? ? ? STACKP".split()
GRP = [G0, G1, G2, G3]
IREG = ["I%d" % i for i in range(8)] + ["M%d" % i for i in range(8)]
XOP_ALU = "AX0 AX1 AR MR0 MR1 MR2 SR0 SR1".split()
XOP_MAC = "MX0 MX1 AR MR0 MR1 MR2 SR0 SR1".split()
XOP_SH = "SI SR2 AR MR0 MR1 MR2 SR0 SR1".split()
YOP_ALU = "AY0 AY1 AF 0".split()
YOP_MAC = "MY0 MY1 SR1 0".split()
COND = "EQ NE GT LE LT GE AV NOT_AV AC NOT_AC SWCOND NOT_SWCOND MV NOT_MV NOT_CE TRUE".split()
SF = ["LSHIFT HI", "LSHIFT HI OR", "LSHIFT LO", "LSHIFT LO OR", "ASHIFT HI", "ASHIFT HI OR", "ASHIFT LO", "ASHIFT LO OR",
      "NORM HI", "NORM HI OR", "NORM LO", "NORM LO OR", "EXP HI", "EXP HIX", "EXP LO", "EXPADJ"]
MACF = ["NOP", "X*Y (RND)", "MR+X*Y (RND)", "MR-X*Y (RND)", "X*Y (SS)", "X*Y (SU)", "X*Y (US)", "X*Y (UU)",
        "MR+X*Y (SS)", "MR+X*Y (SU)", "MR+X*Y (US)", "MR+X*Y (UU)", "MR-X*Y (SS)", "MR-X*Y (SU)", "MR-X*Y (US)", "MR-X*Y (UU)"]


def k4(v, bo):
    """Table 8-4 constants: v = YY:CC (0-15), bo = 1 positive (2^v), 3 negative (-(2^v)-1); 15 with BO=3 is +32767, with 1 is -32768."""
    if v == 15:
        return -32768 if bo == 1 else 32767
    return (1 << v) if bo == 1 else -(1 << v) - 1


def alu(amf, x, y, c):
    return {0x10: y, 0x11: "%s+1" % y, 0x12: "%s+%s+C" % (x, y), 0x13: "%s+%s" % (x, y), 0x14: "NOT %s" % y, 0x15: "-%s" % y,
            0x16: "%s-%s+C-1" % (x, y), 0x17: "%s-%s" % (x, y), 0x18: "%s-1" % y, 0x19: "%s-%s" % (y, x),
            0x1a: "%s-%s+C-1" % (y, x), 0x1b: "NOT %s" % x, 0x1c: "%s AND %s" % (x, y), 0x1d: "%s OR %s" % (x, y),
            0x1e: "%s XOR %s" % (x, y), 0x1f: "ABS %s" % x}[amf]


def compute(amf, z, x, y):
    """Text for the ALU/MAC operation. x, y: operand names."""
    if amf == 0:
        return ""
    if amf < 16:
        d = "SR" if z else "MR"
        return "%s = %s" % (d, MACF[amf].replace("X", x).replace("Y", y).replace("MR", d))
    return "%s = %s" % ("AF" if z else "AR", alu(amf, x, y, 0))


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def dis(mem, pc):
    """-> (text, length) for the instruction at pc."""
    w = mem.get(pc, 0)
    b = lambda hi, lo=None: (w >> (lo if lo is not None else hi)) & ((1 << ((hi - lo + 1) if lo is not None else 1)) - 1)
    top = w >> 16
    if w >> 22 == 3:                                     # type 1
        amf, yop, xop = b(17, 13), b(12, 11), b(10, 8)
        mac = amf < 16
        xn = (XOP_MAC if mac else XOP_ALU)[xop]
        yn = (YOP_MAC if mac else YOP_ALU)[yop]
        dd = ["AX0", "AX1", "MX0", "MX1"][b(19, 18)]
        pd = ["AY0", "AY1", "MY0", "MY1"][b(21, 20)]
        c = compute(amf, 0, xn, yn)
        return "%s%s = DM(I%d += M%d), %s = PM(I%d += M%d);" % (c + ", " if c else "", dd, b(3, 2), b(1, 0), pd, 4 + b(7, 6), 4 + b(5, 4)), 1
    t3 = w >> 21
    if t3 in (4, 5):                                     # type 3
        a, reg, wr = b(19, 4), b(3, 0), b(20)
        r = GRP[0][reg] if t3 == 4 else IREG[reg]
        return ("DM(0x%04x) = %s;" if wr else "%s = DM(0x%04x);") % ((a, r) if wr else (r, a)), 1
    if t3 == 3:                                          # type 4
        amf, yop, xop = b(17, 13), b(12, 11), b(10, 8)
        mac = amf < 16
        xn = (XOP_MAC if mac else XOP_ALU)[xop]
        yn = (YOP_MAC if mac else YOP_ALU)[yop]
        c = compute(amf, b(18), xn, yn)
        g = 4 * b(20)
        m = "DM(I%d += M%d)" % (g + b(3, 2), g + b(1, 0))
        r = G0[b(7, 4)]
        mv = "%s = %s;" % (m, r) if b(19) else "%s = %s;" % (r, m)
        return "%s%s" % (c + ", " if c else "", mv), 1
    t4 = w >> 20
    if t4 == 4:
        return "%s = 0x%04x;" % (G0[b(3, 0)], b(19, 4)), 1
    if t4 == 5:
        return "%s = 0x%04x;" % (G1[b(3, 0)], b(19, 4)), 1
    if t4 == 3:
        return "%s = 0x%04x;" % (G2[b(3, 0)], b(19, 4)), 1
    if w >> 19 == 5:                                     # type 8
        amf, yop, xop = b(17, 13), b(12, 11), b(10, 8)
        mac = amf < 16
        xn = (XOP_MAC if mac else XOP_ALU)[xop]
        yn = (YOP_MAC if mac else YOP_ALU)[yop]
        c = compute(amf, b(18), xn, yn)
        if (w & 0xFF) == 0xAA:                           # status-only form (bits 7-0 = 10101010)
            return "NONE = %s;" % c, 1
        return "%s, %s = %s;" % (c, G0[b(7, 4)], G0[b(3, 0)]) if c else "%s = %s;" % (G0[b(7, 4)], G0[b(3, 0)]), 1
    if w >> 19 == 4:                                     # types 9, 9a
        amf, z = b(17, 13), b(18)
        mac = amf < 16
        if b(5, 4) == 2:                                 # 9a: register file
            xn, yn = G0[b(11, 8)], (G0[b(3, 0)] if not b(12) else "0")
            return compute(amf, z, xn, yn) + ";" if amf else "NOP;", 1
        cond = COND[b(3, 0)]
        pre = "" if cond == "TRUE" else "IF %s " % cond
        xn = (XOP_MAC if mac else XOP_ALU)[b(10, 8)]
        if b(7, 4) == 0:
            yn = (YOP_MAC if mac else YOP_ALU)[b(12, 11)]
        elif b(7, 4) == 1 and mac:
            yn = xn
        else:
            yn = "%d" % k4((b(12, 11) << 2) | b(7, 6), b(5, 4))
        return pre + (compute(amf, z, xn, yn) if amf else "NOP") + ";", 1
    t8 = w >> 16
    if t8 == 0x10:
        return "%s = 0x%03x;" % (GRP[3][b(3, 0)], b(15, 4)), 1
    if w >> 17 == 0x09:                                  # type 12 0001001x
        sf = SF[b(15, 12)]
        xn = XOP_SH[b(10, 8)]
        g = 4 * b(16)
        m = "DM(I%d += M%d)" % (g + b(3, 2), g + b(1, 0))
        r = G0[b(7, 4)]
        mv = "%s = %s;" % (m, r) if b(11) else "%s = %s;" % (r, m)
        return "SHIFT %s of %s, %s" % (sf, xn, mv), 1
    if t8 == 0x14:
        return "SHIFT %s of %s, %s = %s;" % (SF[b(15, 12)], G0[b(11, 8)], G0[b(7, 4)], G0[b(3, 0)]), 1
    if t8 == 0x15:
        mm = "PM" if b(15) else "DM"
        g = 4 * b(13)
        if b(12, 11) == 3:
            return "DM(I%d += M%d) = %s" % ((g + b(3, 2)), g + b(1, 0), GRP[b(9, 8)][b(7, 4)]) + " (32a)", 1
        m = "%s(I%d %s M%d)" % (mm, g + b(3, 2), "+=" if b(14) else "+", g + b(1, 0))
        r = GRP[b(9, 8)][b(7, 4)]
        return ("%s = %s;" % (m, r) if b(12) else "%s = %s;" % (r, m)), 1
    if t8 == 0x16:
        te = b(3, 0)
        return "DO 0x%04x UNTIL %s;" % ((pc + sx(b(15, 4), 12)) & 0xFFFF, "CE" if te == 14 else "FOREVER"), 1
    if w >> 18 == 0x06:                                  # type 10, 000110
        cond = COND[b(3, 0)]
        return "%sJUMP 0x%04x%s;" % ("" if cond == "TRUE" else "IF %s " % cond, (pc + sx(b(16, 4), 13)) & 0xFFFF, " (DB)" if b(17) else ""), 1
    if w >> 18 == 0x07:                                  # type 10a
        a = sx((b(1, 0) << 14) | b(17, 4), 16)
        return "%s 0x%04x%s;" % ("CALL" if b(2) else "JUMP", (pc + a) & 0xFFFF, " (DB)" if b(3) else ""), 1
    if t8 == 0x0F:
        return "SHIFT %s BY %d of %s;" % (SF[b(15, 12)], sx(b(7, 0), 8), G0[b(11, 8)]), 1
    if t8 == 0x0E:
        cond = COND[b(3, 0)]
        return "%sSHIFT %s of %s;" % ("" if cond == "TRUE" else "IF %s " % cond, SF[b(15, 12)], G0[b(11, 8)]), 1
    if t8 == 0x0D:
        return "%s = %s;" % (GRP[b(11, 10)][b(7, 4)], GRP[b(9, 8)][b(3, 0)]), 1
    if t8 == 0x0C:
        return "MODE fields 0x%03x;" % b(15, 0), 1
    if t8 == 0x0B:
        cond = COND[b(7, 4)]
        return "%s%s I%d%s;" % ("" if cond == "TRUE" else "IF %s " % cond, "CALL" if b(14) else "JUMP", (4 * b(13)) + b(3, 2), " (DB)" if b(15) else ""), 1
    if t8 == 0x0A:
        cond = COND[b(7, 4)]
        return "%s%s%s;" % ("" if cond == "TRUE" else "IF %s " % cond, "RTI" if b(14) else "RTS", " (DB)" if b(15) else ""), 1
    if w >> 17 == 0x04 and (w >> 16) in (0x08, 0x09):    # type 29: 0000100x
        g = 4 * b(13)
        reg = G0[(b(15, 14) << 2) | b(1, 0)]
        imm = sx(b(11, 4), 8)
        ir = "I%d" % (g + b(3, 2))
        if b(16):
            m = "DM(%s += %d)" % (ir, imm)
        else:
            m = "DM(%s + %d)" % (ir, imm)
        return ("%s = %s;" % (m, reg) if b(12) else "%s = %s;" % (reg, m)), 1
    if t8 == 0x07:
        if b(15):
            g = 4 * b(13)
            w2 = mem.get(pc + 1, 0)
            if b(14):
                return "PM(I%d += M%d) = 0x%06x;" % (g + b(3, 2), g + b(1, 0), ((w2 >> 12 & 0xFF) << 16) | (b(11, 4) << 8) | (w2 >> 4 & 0xFF)), 2
            return "DM(I%d += M%d) = 0x%04x;" % (g + b(3, 2), g + b(1, 0), ((w2 >> 12 & 0xFF) << 8) | b(11, 4)), 2
        return "%sINT %d;" % ("CLR" if b(5) else "SET", b(3, 0)), 1
    if t8 == 0x06:
        if b(15):
            return "IO(0x%03x) %s %s;" % ((b(14, 13) << 8) | b(11, 4), "<-" if b(12) else "->", G0[b(3, 0)]), 1
        return "REG(0x%02x) %s %s;" % (b(11, 4), "<-" if b(12) else "->", G0[b(3, 0)]), 1
    if t8 == 0x05:
        w2 = mem.get(pc + 1, 0)
        cond = COND[b(3, 0)]
        a = (b(11, 4) << 16) | (w2 >> 4 & 0xFFFF)
        return "%s%s 0x%06x;" % ("" if cond == "TRUE" else "IF %s " % cond, "LCALL" if b(12) else "LJUMP", a), 2
    if t8 == 0x04:
        return "STACK 0x%03x;" % b(15, 0), 1
    if t8 == 0x03:
        if b(15) == 0:
            return "SAT %s;" % ("SR" if b(12) else "MR"), 1
        if b(15, 12) == 0xD:
            return "DIVQ %s;" % XOP_ALU[b(10, 8)], 1
        return "DIVS %s, %s;" % (YOP_ALU[b(12, 11)], XOP_ALU[b(10, 8)]), 1
    if t8 == 0x02:
        return "IDLE;", 1
    if t8 == 0x01:
        g = 4 * b(13)
        if b(15):
            return "MODIFY (I%d += M%d), (I%d += M%d);" % (g + b(3, 2), g + b(1, 0), g + b(3, 2), g + b(1, 0)), 1
        return "MODIFY (I%d += %d);" % (g + b(3, 2), sx(b(11, 4), 8)), 1
    if w == 0:
        return "NOP;", 1
    return ".word 0x%06x" % w, 1


def listing(mem, lo, hi):
    pc = lo
    while pc <= hi:
        if pc not in mem:
            pc += 1
            continue
        t, n = dis(mem, pc)
        print("%04x  %06x  %s" % (pc, mem[pc], t))
        pc += n


def refs(mem, lo, hi):
    pc = 0
    while pc <= 0x17FF:
        if pc not in mem:
            pc += 1
            continue
        t, n = dis(mem, pc)
        if "= 0x" in t and not t.startswith("DM") and not t.startswith("PM"):
            v = int(t.split("= 0x")[1].split(";")[0], 16)
            if lo <= v <= hi:
                print("%04x  %06x  %s" % (pc, mem[pc], t))
        pc += n


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    t, img = pe_fw.unpack_fw(a[0])
    if t != 0x78:
        sys.exit("%s is not a DSP update" % a[0])
    _, mem = pe_fw.dsp_load(img)
    if len(a) > 1 and a[1] == "--refs":
        refs(mem, int(a[2], 0), int(a[3], 0))
        return
    lo = int(a[1], 0) if len(a) > 1 else 0
    hi = int(a[2], 0) if len(a) > 2 else 0x17FF
    listing(mem, lo, hi)


if __name__ == "__main__":
    main()
