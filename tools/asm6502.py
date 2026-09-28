#!/usr/bin/env python3
"""
asm6502.py - minimal two-pass 6502 assembler for AmiAtari2600 test ROMs.

Syntax (DASM-like subset):
    NAME = expr          ; constant
    label:               ; label (colon optional if at column 0)
    org $F000            ; set location counter
    rorg $F000           ; set logical address, keep file position
    .byte 1,2,$03        ; data (also: byte, .db)
    .word label          ; little endian words (also: word, .dw)
    .fill count,value    ; repeat a byte (also: ds count,value)
    lda #<label          ; < low byte, > high byte
    sta WSYNC
    lda (ptr),y
Expressions: decimal, $hex, %binary, 'c', labels, + - * / & | ^ << >> ().

Usage: asm6502.py input.asm output.bin [--size 4096] [--fill 0xFF]
The output image covers [first org, first org + size).
"""
import re
import sys

IMP, ACC, IMM, ZP, ZPX, ZPY, ABS, ABX, ABY, IND, IZX, IZY, REL = range(13)
SIZE = {IMP: 1, ACC: 1, IMM: 2, ZP: 2, ZPX: 2, ZPY: 2, ABS: 3, ABX: 3, ABY: 3,
        IND: 3, IZX: 2, IZY: 2, REL: 2}

# mnemonic -> {mode: opcode}
OPS = {}


def _add(mn, table):
    OPS[mn] = table


_alu = {"ora": 0x00, "and": 0x20, "eor": 0x40, "adc": 0x60, "sta": 0x80,
        "lda": 0xA0, "cmp": 0xC0, "sbc": 0xE0}
for mn, b in _alu.items():
    t = {ZP: b | 0x05, ZPX: b | 0x15, ABS: b | 0x0D, ABX: b | 0x1D,
         ABY: b | 0x19, IZX: b | 0x01, IZY: b | 0x11}
    if mn != "sta":
        t[IMM] = b | 0x09
    _add(mn, t)
_sh = {"asl": 0x00, "rol": 0x20, "lsr": 0x40, "ror": 0x60}
for mn, b in _sh.items():
    _add(mn, {ACC: b | 0x0A, ZP: b | 0x06, ZPX: b | 0x16, ABS: b | 0x0E, ABX: b | 0x1E})
_add("inc", {ZP: 0xE6, ZPX: 0xF6, ABS: 0xEE, ABX: 0xFE})
_add("dec", {ZP: 0xC6, ZPX: 0xD6, ABS: 0xCE, ABX: 0xDE})
_add("ldx", {IMM: 0xA2, ZP: 0xA6, ZPY: 0xB6, ABS: 0xAE, ABY: 0xBE})
_add("ldy", {IMM: 0xA0, ZP: 0xA4, ZPX: 0xB4, ABS: 0xAC, ABX: 0xBC})
_add("stx", {ZP: 0x86, ZPY: 0x96, ABS: 0x8E})
_add("sty", {ZP: 0x84, ZPX: 0x94, ABS: 0x8C})
_add("cpx", {IMM: 0xE0, ZP: 0xE4, ABS: 0xEC})
_add("cpy", {IMM: 0xC0, ZP: 0xC4, ABS: 0xCC})
_add("bit", {ZP: 0x24, ABS: 0x2C})
_add("jmp", {ABS: 0x4C, IND: 0x6C})
_add("jsr", {ABS: 0x20})
for mn, op in {"bpl": 0x10, "bmi": 0x30, "bvc": 0x50, "bvs": 0x70, "bcc": 0x90,
               "bcs": 0xB0, "bne": 0xD0, "beq": 0xF0}.items():
    _add(mn, {REL: op})
for mn, op in {"brk": 0x00, "php": 0x08, "clc": 0x18, "plp": 0x28, "sec": 0x38,
               "rti": 0x40, "pha": 0x48, "cli": 0x58, "rts": 0x60, "pla": 0x68,
               "sei": 0x78, "dey": 0x88, "txa": 0x8A, "tya": 0x98, "txs": 0x9A,
               "tay": 0xA8, "tax": 0xAA, "clv": 0xB8, "tsx": 0xBA, "iny": 0xC8,
               "dex": 0xCA, "cld": 0xD8, "inx": 0xE8, "nop": 0xEA, "sed": 0xF8}.items():
    _add(mn, {IMP: op})


DIRECTIVES = {"org", "rorg", ".byte", "byte", ".db", "dc.b", ".word", "word", ".dw",
              "dc.w", ".fill", "ds", ".ds", ".align", "align", "include"}


class AsmError(Exception):
    pass


class Assembler:
    def __init__(self):
        self.symbols = {}
        self.pass_no = 1

    # ---- expressions ----
    def value(self, expr, line):
        expr = expr.strip()
        if expr.startswith("<"):
            return self.value(expr[1:], line) & 0xFF
        if expr.startswith(">"):
            return (self.value(expr[1:], line) >> 8) & 0xFF

        def num(m):
            t = m.group(0)
            if t.startswith("$"):
                return str(int(t[1:], 16))
            if t.startswith("%"):
                return str(int(t[1:], 2))
            if t.startswith("'"):
                return str(ord(t[1]))
            if t == "*":
                return str(self.pc)
            if re.match(r"[A-Za-z_.]", t):
                if t in self.symbols:
                    return str(self.symbols[t])
                if self.pass_no == 1:
                    self.unresolved = True
                    return "0"
                raise AsmError("undefined symbol '%s' in line %d" % (t, line))
            return t
        self.unresolved = False
        py = re.sub(r"\$[0-9A-Fa-f]+|%[01]+|'.'|[A-Za-z_.][A-Za-z0-9_.]*|\d+|(?<![\w)])\*(?![\w(])",
                    num, expr)
        if not re.fullmatch(r"[0-9+\-*/&|^<>() ~]*", py):
            raise AsmError("bad expression '%s' in line %d" % (expr, line))
        return int(eval(py.replace("/", "//"), {"__builtins__": {}}))

    # ---- operand parsing ----
    def operand(self, mn, arg, line):
        arg = arg.strip()
        modes = OPS[mn]
        if not arg:
            return (ACC if ACC in modes else IMP), None
        if arg.lower() == "a" and ACC in modes:
            return ACC, None
        if REL in modes:
            return REL, arg
        if arg.startswith("#"):
            return IMM, arg[1:]
        m = re.fullmatch(r"\((.+)\)\s*,\s*[yY]", arg)
        if m:
            return IZY, m.group(1)
        m = re.fullmatch(r"\((.+)\s*,\s*[xX]\)", arg)
        if m:
            return IZX, m.group(1)
        m = re.fullmatch(r"\((.+)\)", arg)
        if m and IND in modes:
            return IND, m.group(1)
        m = re.fullmatch(r"(.+?)\s*,\s*([xXyY])", arg)
        if m:
            expr, reg = m.group(1), m.group(2).lower()
            v = self.value(expr, line)
            zp_ok = not self.unresolved and 0 <= v < 256
            if reg == "x":
                return (ZPX if zp_ok and ZPX in modes else ABX), expr
            return (ZPY if zp_ok and ZPY in modes else ABY), expr
        v = self.value(arg, line)
        zp_ok = not self.unresolved and 0 <= v < 256
        return (ZP if zp_ok and ZP in modes else ABS), arg

    def run_pass(self, lines):
        self.pc = 0
        self.out = {}
        self.origin = None
        self.fpos = 0          # physical offset from origin
        for no, raw in enumerate(lines, 1):
            line = raw.split(";")[0].rstrip()
            if not line.strip():
                continue
            m = re.match(r"^([A-Za-z_.][A-Za-z0-9_.]*)\s*(=|equ\b)\s*(.+)$", line.strip(), re.I)
            if m:
                self.symbols[m.group(1)] = self.value(m.group(3), no)
                continue
            m = re.match(r"^([A-Za-z_.][A-Za-z0-9_.]*):?(\s+|$)(.*)$", line) if not line[0].isspace() else None
            if m and m.group(1).lower() not in OPS and m.group(1).lower() not in DIRECTIVES:
                self.symbols[m.group(1)] = self.pc
                line = " " + m.group(3)
                if not line.strip():
                    continue
            parts = line.strip().split(None, 1)
            op = parts[0].lower()
            arg = parts[1] if len(parts) > 1 else ""
            if op in ("org",):
                v = self.value(arg, no)
                if self.origin is None:
                    self.origin = v
                self.fpos = v - self.origin
                self.pc = v
            elif op in ("rorg",):
                self.pc = self.value(arg, no)
            elif op in (".byte", "byte", ".db", "dc.b"):
                for item in self.split_args(arg):
                    self.emit([self.value(item, no) & 0xFF])
            elif op in (".word", "word", ".dw", "dc.w"):
                for item in self.split_args(arg):
                    v = self.value(item, no)
                    self.emit([v & 0xFF, (v >> 8) & 0xFF])
            elif op in (".fill", "ds", ".ds"):
                a = self.split_args(arg)
                cnt = self.value(a[0], no)
                val = self.value(a[1], no) if len(a) > 1 else 0
                self.emit([val & 0xFF] * cnt)
            elif op in (".align", "align"):
                n = self.value(arg, no)
                pad = (-self.pc) % n
                self.emit([0] * pad)
            elif op in OPS:
                mode, expr = self.operand(op, arg, no)
                if mode not in OPS[op]:
                    raise AsmError("addressing mode not supported for %s in line %d" % (op, no))
                code = [OPS[op][mode]]
                if mode == REL:
                    target = self.value(expr, no)
                    off = target - (self.pc + 2)
                    if self.pass_no == 2 and not -128 <= off <= 127:
                        raise AsmError("branch out of range in line %d" % no)
                    code.append(off & 0xFF)
                elif SIZE[mode] == 2:
                    code.append(self.value(expr, no) & 0xFF)
                elif SIZE[mode] == 3:
                    v = self.value(expr, no)
                    code += [v & 0xFF, (v >> 8) & 0xFF]
                self.emit(code)
            else:
                raise AsmError("unknown instruction '%s' in line %d" % (op, no))

    @staticmethod
    def split_args(arg):
        return [a.strip() for a in arg.split(",") if a.strip()]

    def emit(self, data):
        if self.origin is None:
            raise AsmError("code before org")
        for b in data:
            self.out[self.fpos] = b
            self.fpos += 1
            self.pc += 1

    @staticmethod
    def expand_includes(text, base):
        import os
        out = []
        for line in text.splitlines():
            m = re.match(r'^\s+include\s+"?([^"\s]+)"?', line, re.I)
            if m:
                path = os.path.join(base, m.group(1))
                out += Assembler.expand_includes(open(path).read(), os.path.dirname(path))
            else:
                out.append(line)
        return out

    def assemble(self, text, base="."):
        lines = self.expand_includes(text, base)
        self.pass_no = 1
        self.run_pass(lines)
        self.pass_no = 2
        self.run_pass(lines)
        return self.out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    size = 4096
    fill = 0xFF
    args = sys.argv[3:]
    for i, a in enumerate(args):
        if a == "--size":
            size = int(args[i + 1], 0)
        if a == "--fill":
            fill = int(args[i + 1], 0)
    asm = Assembler()
    try:
        import os
        out = asm.assemble(open(sys.argv[1]).read(), os.path.dirname(sys.argv[1]) or ".")
    except AsmError as e:
        print("%s: %s" % (sys.argv[1], e), file=sys.stderr)
        return 1
    img = bytearray([fill] * size)
    for pos, b in out.items():
        if not 0 <= pos < size:
            print("%s: data outside image at offset %d" % (sys.argv[1], pos), file=sys.stderr)
            return 1
        img[pos] = b
    open(sys.argv[2], "wb").write(img)
    return 0


if __name__ == "__main__":
    sys.exit(main())
