#!/usr/bin/env python3
"""PIC18 disassembler (development tool; works on your own voice-CPU update file via pe_fw.py, ships nothing).

    pic18_dis.py VOICE.syx [FROM [TO]]        disassemble byte addresses FROM..TO (default everything)
    pic18_dis.py VOICE.syx --refs LO HI       list MOVLW / LFSR loads of constants in LO..HI (table pointers, SFR setup)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pe_fw  # noqa: E402

BR = {0xE0: "BZ", 0xE1: "BNZ", 0xE2: "BC", 0xE3: "BNC", 0xE4: "BOV", 0xE5: "BNOV", 0xE6: "BN", 0xE7: "BNN"}
LIT = {0x08: "SUBLW", 0x09: "IORLW", 0x0A: "XORLW", 0x0B: "ANDLW", 0x0C: "RETLW", 0x0D: "MULLW", 0x0E: "MOVLW", 0x0F: "ADDLW"}
FD = {0x04: "DECF", 0x14: "ANDWF", 0x10: "IORWF", 0x18: "XORWF", 0x1C: "COMF", 0x20: "ADDWFC", 0x24: "ADDWF", 0x28: "INCF",
      0x2C: "DECFSZ", 0x30: "RRCF", 0x34: "RLCF", 0x38: "SWAPF", 0x3C: "INCFSZ", 0x40: "RRNCF", 0x44: "RLNCF", 0x48: "INFSNZ",
      0x4C: "DCFSNZ", 0x50: "MOVF", 0x54: "SUBFWB", 0x58: "SUBWFB", 0x5C: "SUBWF"}
F1 = {0x60: "CPFSLT", 0x62: "CPFSEQ", 0x64: "CPFSGT", 0x66: "TSTFSZ", 0x68: "SETF", 0x6A: "CLRF", 0x6C: "NEGF", 0x6E: "MOVWF",
      0x02: "MULWF"}
BIT = {0x70: "BTG", 0x80: "BSF", 0x90: "BCF", 0xA0: "BTFSS", 0xB0: "BTFSC"}
TBL = {0x08: "TBLRD*", 0x09: "TBLRD*+", 0x0A: "TBLRD*-", 0x0B: "TBLRD+*", 0x0C: "TBLWT*", 0x0D: "TBLWT*+", 0x0E: "TBLWT*-",
       0x0F: "TBLWT+*"}
MISC = {0x0000: "NOP", 0x0003: "SLEEP", 0x0004: "CLRWDT", 0x0005: "PUSH", 0x0006: "POP", 0x0007: "DAW", 0x0010: "RETFIE",
        0x0011: "RETFIE FAST", 0x0012: "RETURN", 0x0013: "RETURN FAST", 0x00FF: "RESET"}


def dis(img, a):
    """-> (text, length in bytes)"""
    w = img[a] | (img[a + 1] << 8)
    hi, lo = w >> 8, w & 0xFF
    w2 = (img[a + 2] | (img[a + 3] << 8)) if a + 3 < len(img) else 0
    if w in MISC:
        return MISC[w], 2
    if hi == 0x00 and lo in TBL:
        return TBL[lo], 2
    if hi == 0x01:
        return "MOVLB 0x%x" % (lo & 15), 2
    if hi in LIT:
        return "%s 0x%02x" % (LIT[hi], lo), 2
    if hi in (0x02, 0x03):
        return "MULWF 0x%02x%s" % (lo, "" if hi & 1 == 0 else ", BANKED"), 2
    top = hi & 0xFC
    if top in FD:
        d = "F" if hi & 2 else "W"
        return "%s 0x%02x, %s%s" % (FD[top], lo, d, ", BANKED" if hi & 1 else ""), 2
    if (hi & 0xFE) in F1:
        return "%s 0x%02x%s" % (F1[hi & 0xFE], lo, ", BANKED" if hi & 1 else ""), 2
    if (hi & 0xF0) in BIT:
        return "%s 0x%02x, %d%s" % (BIT[hi & 0xF0], lo, (hi >> 1) & 7, ", BANKED" if hi & 1 else ""), 2
    if hi in BR:
        n = lo - 256 if lo & 0x80 else lo
        return "%s 0x%04x" % (BR[hi], a + 2 + 2 * n), 2
    if hi >> 3 == 0x1A:
        n = w & 0x7FF
        n = n - 0x800 if n & 0x400 else n
        return "BRA 0x%04x" % (a + 2 + 2 * n), 2
    if hi >> 3 == 0x1B:
        n = w & 0x7FF
        n = n - 0x800 if n & 0x400 else n
        return "RCALL 0x%04x" % (a + 2 + 2 * n), 2
    if hi in (0xEC, 0xED):
        return "CALL 0x%05x%s" % ((lo | ((w2 & 0xFFF) << 8)) * 2, ", FAST" if hi & 1 else ""), 4
    if hi == 0xEF:
        return "GOTO 0x%05x" % ((lo | ((w2 & 0xFFF) << 8)) * 2), 4
    if hi == 0xEE:
        return "LFSR %d, 0x%03x" % ((lo >> 4) & 3, ((lo & 15) << 8) | (w2 & 0xFF)), 4
    if hi >> 4 == 0xC:
        return "MOVFF 0x%03x, 0x%03x" % (w & 0xFFF, w2 & 0xFFF), 4
    if hi >> 4 == 0xF:
        return "NOP", 2
    return ".word 0x%04x" % w, 2


def listing(img, lo, hi):
    a = lo
    while a <= hi and a + 1 < len(img):
        t, n = dis(img, a)
        print("%04x  %s" % (a, t))
        a += n


def refs(img, lo, hi):
    a = 0
    while a + 1 < len(img):
        t, n = dis(img, a)
        if t.startswith("MOVLW"):
            v = int(t.split("0x")[1], 16)
            if lo <= v <= hi:
                print("%04x  %s" % (a, t))
        elif t.startswith("LFSR"):
            v = int(t.split("0x")[1], 16)
            if lo <= v <= hi:
                print("%04x  %s" % (a, t))
        a += n


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    t, img = pe_fw.unpack_fw(a[0])
    if t != 0x69:
        sys.exit("%s is not a voice-CPU update (target 0x%02x)" % (a[0], t))
    if len(a) > 1 and a[1] == "--refs":
        refs(img, int(a[2], 0), int(a[3], 0))
        return
    listing(img, int(a[1], 0) if len(a) > 1 else 0, int(a[2], 0) if len(a) > 2 else len(img) - 2)


if __name__ == "__main__":
    main()
