#!/usr/bin/env python3
"""Minimal dsPIC30F/33F disassembler (the common integer, branch, move, multiply/divide and table instructions).

    dspic_dis.py IMAGE.bin START END        program addresses (even), image = 3 bytes per 24-bit word

Unknown encodings print as .word (in DSI images most are data: strings and tables stored as 16-bit words in program memory,
read through the PSV window at 0x8000 + program address). Written from the 16-bit MCU/DSC programmer's reference encodings for the
Tempest voice CPU (mpc-vst-tpv) and used here on the Poly Evolver main CPU (docs/FIRMWARE.md section 12); good enough to follow
compiled C, not a validated tool.
"""
import sys

SRC = ["W{s}", "[W{s}]", "[W{s}--]", "[W{s}++]", "[--W{s}]", "[++W{s}]", "[W{s}+W{w}]", "[W{s}+W{w}]"]
COND = {0x30: "OV", 0x31: "C", 0x32: "Z", 0x33: "N", 0x34: "LE", 0x35: "LT", 0x36: "LEU", 0x37: "", 0x38: "NOV",
        0x39: "NC", 0x3A: "NZ", 0x3B: "NN", 0x3C: "GT", 0x3D: "GE", 0x3E: "GTU"}


def mode(m, r, w=0):
    return SRC[m].format(s=r, w=w)


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def dis(W, i):
    """-> (text, words used)"""
    x = W[i]
    pa = 2 * i
    op = x >> 16
    s, d = x & 15, (x >> 7) & 15
    p, q = (x >> 4) & 7, (x >> 11) & 7
    wb = (x >> 15) & 15
    B = ".B" if x & 0x4000 else ""
    if x == 0:
        return "NOP", 1
    if x == 0xFFFFFF:
        return "NOPR", 1
    if op in (0x02, 0x04):
        y = W[i + 1] if i + 1 < len(W) else 0
        tgt = (x & 0xFFFE) | ((y & 0x7F) << 16)
        return ("CALL" if op == 2 else "GOTO") + " 0x%05x" % tgt, 2
    if op == 0x01:
        sub = (x >> 4) & 0xFFF
        n = x & 15
        names = {0x000: "CALL W%d", 0x004: "GOTO W%d", 0x002: "RCALL W%d", 0x006: "BRA W%d"}
        for k, v in names.items():
            if (x & 0xFFF0) >> 4 == k << 0 and False:
                pass
        if (x & 0xFFFF0) == 0x10000: return "CALL W%d" % n, 1
        if (x & 0xFFFF0) == 0x14000: return "GOTO W%d" % n, 1
        if (x & 0xFFFF0) == 0x12000: return "RCALL W%d" % n, 1
        if (x & 0xFFFF0) == 0x16000: return "BRA W%d" % n, 1
        return ".word 0x%06x" % x, 1
    if op == 0x05:
        return "RETLW%s #%d, W%d" % (B, (x >> 4) & 0x3FF, s), 1
    if x == 0x060000:
        return "RETURN", 1
    if x == 0x064000:
        return "RETFIE", 1
    if op == 0x07:
        return "RCALL 0x%05x" % (pa + 2 + 2 * sx(x & 0xFFFF, 16)), 1
    if op == 0x09:
        if x & 0xC000 == 0:
            return "REPEAT #%d" % (x & 0x3FFF), 1
        return "REPEAT W%d" % s, 1
    if op == 0x08:
        return "DO #%d, 0x%05x" % (x & 0x3FFF, pa + 2 * (W[i + 1] & 0xFFFF) + 2), 2
    if 0x30 <= op <= 0x3E:
        return ("BRA %s, " % COND[op] if COND[op] else "BRA ") + "0x%05x" % (pa + 2 + 2 * sx(x & 0xFFFF, 16)), 1
    if 0x20 <= op <= 0x2F:
        return "MOV #0x%04x, W%d" % ((x >> 4) & 0xFFFF, s), 1
    if 0x40 <= op <= 0x7F or 0x10 <= op <= 0x1F:
        names = {0x10: "SUBR", 0x18: "SUBBR", 0x40: "ADD", 0x48: "ADDC", 0x50: "SUB", 0x58: "SUBB", 0x60: "AND", 0x68: "XOR",
                 0x70: "IOR", 0x78: "MOV"}
        base = op & 0xF8
        nm = names[base]
        if base == 0x78:
            g, h = p, q
            if h >= 6 or g >= 6:
                return "MOV%s %s, %s" % (B, mode(g, s, wb), mode(h, d, wb)), 1
            return "MOV%s %s, %s" % (B, mode(g, s), mode(h, d)), 1
        if (x >> 5) & 3 == 3:
            return "%s%s W%d, #%d, %s" % (nm, B, wb, x & 31, mode(q, d)), 1
        return "%s%s W%d, %s, %s" % (nm, B, wb, mode(p, s), mode(q, d)), 1
    if 0x80 <= op <= 0x87:
        return "MOV 0x%04x, W%d" % (((x >> 4) & 0x7FFF) << 1, s), 1
    if 0x88 <= op <= 0x8F:
        return "MOV W%d, 0x%04x" % (s, ((x >> 4) & 0x7FFF) << 1), 1
    if 0x90 <= op <= 0x97:
        k = ((x >> 15) & 0xF) << 6 | ((x >> 11) & 7) << 3 | ((x >> 4) & 7)
        return "MOV%s [W%d%+d], W%d" % (B, s, sx(k, 10) * (1 if B else 2), d), 1
    if 0x98 <= op <= 0x9F:
        k = ((x >> 15) & 0xF) << 6 | ((x >> 11) & 7) << 3 | ((x >> 4) & 7)
        return "MOV%s W%d, [W%d%+d]" % (B, s, d, sx(k, 10) * (1 if B else 2)), 1
    if 0xA0 <= op <= 0xA7:
        nm = ["BSET", "BCLR", "BTG", "BTST", "BTSTS", "BTST.Z?", "BTSS", "BTSC"][op & 7]
        return "%s %s, #%d" % (nm, mode(p, s), (x >> 12) & 15), 1
    if 0xA8 <= op <= 0xAF:
        nm = ["BSET", "BCLR", "BTG", "BTST", "BTSTS", "BSW", "BTSS", "BTSC"][op & 7]
        f = x & 0x1FFE
        b = ((x >> 13) & 7) | ((x & 1) << 3)
        return "%s.%s 0x%04x, #%d" % (nm, "B" if False else "W", f, b), 1
    if 0xB0 <= op <= 0xB3:
        names = {0xB00: "ADD", 0xB08: "ADDC", 0xB10: "SUB", 0xB18: "SUBB", 0xB20: "AND", 0xB28: "XOR", 0xB30: "IOR"}
        k3 = x >> 12
        if (x >> 12) == 0xB3C:
            return "MOV.B #%d, W%d" % ((x >> 4) & 0xFF, s), 1
        nm = names.get(k3 & 0xFF8)
        if nm:
            return "%s%s #%d, W%d" % (nm, B, (x >> 4) & 0x3FF, s), 1
    if 0xB4 <= op <= 0xB7:
        names = {0xB40: "ADD", 0xB48: "ADDC", 0xB50: "SUB", 0xB58: "SUBB", 0xB60: "AND", 0xB68: "XOR", 0xB70: "IOR", 0xB78: "MOV"}
        nm = names.get((x >> 12) & 0xFF8)
        if nm:
            f = x & 0x1FFF
            dst = "WREG" if not (x & 0x2000) else "f"
            return "%s%s 0x%04x%s" % (nm, B, f, ", WREG" if dst == "WREG" else ""), 1
    if op in (0xB8, 0xB9):
        nm = {0xB80: "MUL.UU", 0xB88: "MUL.US", 0xB90: "MUL.SU", 0xB98: "MUL.SS"}[(x >> 12) & 0xFF8]
        dd = (x >> 7) & 15
        if (x >> 5) & 3 == 3:
            return "%s W%d, #%d, W%d" % (nm, wb, x & 31, dd), 1
        return "%s W%d, %s, W%d" % (nm, wb, mode(p, s), dd), 1
    if op in (0xBA, 0xBB):
        nm = {0xBA0: "TBLRDH", 0xBA8: "TBLRDL", 0xBB0: "TBLWTH", 0xBB8: "TBLWTL"}[(x >> 12) & 0xFF8]
        return "%s%s %s, %s" % (nm, B, mode(p, s), mode(q, d)), 1
    if op == 0xBC:
        return "MUL%s 0x%04x" % (B, x & 0x1FFF), 1
    if op == 0xBE:
        if x & 0x8000 == 0:
            return "MOV.D %s, W%d" % (mode(p, s), d), 1
        return "MOV.D W%d, %s" % (s, mode(q, d)), 1
    if op == 0xBF:
        return "MOV%s 0x%04x, WREG" % (B, x & 0x1FFF), 1
    if 0xC0 <= op <= 0xC7:
        return "MAC/MPY-class 0x%06x" % x, 1
    if op in (0xC8, 0xC9):
        return "SFTAC/LAC-class 0x%06x" % x, 1
    if op == 0xCA:
        acc = "AB"[(x >> 15) & 1]
        return "LAC %s, #%d, %s" % (mode(p, s), sx((x >> 7) & 15, 4), acc), 1
    if op == 0xCC:
        acc = "AB"[(x >> 15) & 1]
        return "SAC %s, #%d, %s" % (acc, sx((x >> 7) & 15, 4), mode(q, d)), 1
    if op == 0xCD:
        acc = "AB"[(x >> 15) & 1]
        return "SAC.R %s, #%d, %s" % (acc, sx((x >> 7) & 15, 4), mode(q, d)), 1
    if op == 0xCB:
        return "ADD/NEG/CLR acc 0x%06x" % x, 1
    if op in (0xD0, 0xD1, 0xD2, 0xD3):
        nm = {0xD00: "SL", 0xD10: "LSR", 0xD18: "ASR", 0xD20: "RLNC", 0xD28: "RLC", 0xD30: "RRNC", 0xD38: "RRC"}.get((x >> 12) & 0xFF8)
        if nm:
            return "%s%s %s, %s" % (nm, B, mode(p, s), mode(q, d)), 1
    if op in (0xD4, 0xD5, 0xD6, 0xD7):
        nm = {0xD40: "SL", 0xD50: "LSR", 0xD58: "ASR", 0xD60: "RLNC", 0xD68: "RLC", 0xD70: "RRNC", 0xD78: "RRC"}.get((x >> 12) & 0xFF8)
        if nm:
            return "%s%s 0x%04x%s" % (nm, B, x & 0x1FFF, "" if x & 0x2000 else ", WREG"), 1
    if op == 0xD8:
        nm = "DIV.U" if x & 0x8000 else "DIV.S"
        return "%s%s W%d, W%d" % (nm, ".D" if x & 0x40 else "", (x >> 11) & 15, s), 1
    if op == 0xD9:
        return "DIVF W%d, W%d" % ((x >> 11) & 15, s), 1
    if op == 0xDD:
        if x & 0x40:
            return "SL W%d, #%d, W%d" % ((x >> 11) & 15, x & 15, d), 1
        return "SL W%d, W%d, W%d" % ((x >> 11) & 15, s, d), 1
    if op == 0xDE:
        nm = "ASR" if x & 0x8000 else "LSR"
        if x & 0x40:
            return "%s W%d, #%d, W%d" % (nm, (x >> 11) & 15, x & 15, d), 1
        return "%s W%d, W%d, W%d" % (nm, (x >> 11) & 15, s, d), 1
    if op == 0xDF:
        return "FBCL/FF1 0x%06x" % x, 1
    if op == 0xE0:
        return "CP0%s %s" % (B, mode(p, s)), 1
    if op == 0xE1:
        nm = "CPB" if x & 0x8000 else "CP"
        if (x >> 5) & 3 == 3:
            return "%s%s W%d, #%d" % (nm, B, (x >> 11) & 15, x & 31), 1
        return "%s%s W%d, %s" % (nm, B, (x >> 11) & 15, mode(p, s)), 1
    if op == 0xE2:
        return "CP0%s 0x%04x" % (B, x & 0x1FFF), 1
    if op == 0xE3:
        return "%s%s 0x%04x" % ("CPB" if x & 0x8000 else "CP", B, x & 0x1FFF), 1
    if op in (0xE6, 0xE7):
        nm = {0xE60: "CPSGT", 0xE68: "CPSLT", 0xE70: "CPSNE", 0xE78: "CPSEQ"}.get((x >> 12) & 0xFF8, "CPS?")
        return "%s%s W%d, W%d" % (nm, B, (x >> 11) & 15, s), 1
    if 0xE8 <= op <= 0xEB:
        nm = {0xE80: "INC", 0xE88: "INC2", 0xE90: "DEC", 0xE98: "DEC2", 0xEA0: "NEG", 0xEA8: "COM", 0xEB0: "CLR", 0xEB8: "SETM"}[(x >> 12) & 0xFF8]
        if nm in ("CLR", "SETM"):
            return "%s%s %s" % (nm, B, mode(q, d)), 1
        return "%s%s %s, %s" % (nm, B, mode(p, s), mode(q, d)), 1
    if 0xEC <= op <= 0xEF:
        nm = {0xEC0: "INC", 0xEC8: "INC2", 0xED0: "DEC", 0xED8: "DEC2", 0xEE0: "NEG", 0xEE8: "COM", 0xEF0: "CLR", 0xEF8: "SETM"}[(x >> 12) & 0xFF8]
        return "%s%s 0x%04x%s" % (nm, B, x & 0x1FFF, "" if x & 0x2000 else ", WREG"), 1
    if op == 0xF8:
        return "PUSH 0x%04x" % (x & 0xFFFE), 1
    if op == 0xF9:
        return "POP 0x%04x" % (x & 0xFFFE), 1
    if op == 0xFA:
        if x & 0x8000:
            return "ULNK", 1
        return "LNK #%d" % (x & 0x3FFF), 1
    if op == 0xFB:
        if x & 0x8000:
            return "ZE %s, W%d" % (mode(p, s), d), 1
        return "SE %s, W%d" % (mode(p, s), d), 1
    if op == 0xFC:
        return "DISI #%d" % (x & 0x3FFF), 1
    if op == 0xFD:
        if (x >> 12) & 0xF == 0xF8 >> 4 and False:
            pass
        if x & 0xFF80 == 0x8000:
            return "SWAP W%d" % s, 1
        return "EXCH/DAW 0x%06x" % x, 1
    if op == 0xFE:
        return {0xFE0000: "RESET", 0xFE4000: "PWRSAV #0", 0xFE4001: "PWRSAV #1", 0xFE6000: "CLRWDT", 0xFE8000: "POP.S", 0xFEA000: "PUSH.S"}.get(x, ".word 0x%06x" % x), 1
    return ".word 0x%06x" % x, 1


def listing(W, start, end):
    i = start // 2
    while i < end // 2:
        t, n = dis(W, i)
        print("%05x: %06x  %s" % (2 * i, W[i], t))
        i += n


if __name__ == "__main__":
    D = open(sys.argv[1], "rb").read()
    W = [D[k] | D[k + 1] << 8 | D[k + 2] << 16 for k in range(0, len(D) - 2, 3)]
    listing(W, int(sys.argv[2], 16), int(sys.argv[3], 16))
