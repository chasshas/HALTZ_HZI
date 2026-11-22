#!/usr/bin/env python3
"""hzasm: assembler for the HALTZ VM (docs/vm-v1.md section 9).

Reads the instruction set from shared/haltz_proto/include/haltz/vm_opcodes.def and the system
calls from vm_syscalls.def, so it can't drift from the C interpreter. Writes a program image
(.hzp: the 32-byte header of shared/haltz_proto/include/haltz/cart.h, then the body).

    python3 tools/hzasm.py hello.s                  -> hello.hzp
    python3 tools/hzasm.py hello.s -o out.hzp -l -  (listing on stdout)

Standard library only.
"""
import argparse
import ast
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
HALTZ_INC = ROOT / "shared" / "haltz_proto" / "include" / "haltz"

FORM_LEN = {"N": 1, "R": 2, "RR": 2, "RQ": 2, "RS": 2, "RI8": 3, "RI16": 4, "RRO": 3,
            "RA": 4, "B8": 2, "RB8": 3, "A16": 3, "U8": 2}

CART_TYPES = {"code": 0, "key": 1, "data": 2}
HEADER_LEN = 32
TITLE_LEN = 16
CART_BODY_MAX = 624
IMAGE_BODY_MAX = 65535

# Short forms that become a real instruction: name -> (mnemonic, extra operand appended)
ALIASES = {"inc": ("addq", "1"), "dec": ("addq", "-1"), "push": ("pushm", None),
           "pop": ("popm", None)}


def read_def(path, nfields):
    """Rows of X(...) lines from a .def file, as lists of strings."""
    rows = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line.startswith("X("):
            continue
        m = re.fullmatch(r"X\((.*)\)", line)
        fields = [f.strip() for f in m.group(1).split(",")]
        if len(fields) != nfields:
            raise SystemExit(f"{path}: bad line: {line}")
        rows.append(fields)
    return rows


OPCODES = {name.lower(): (int(op, 16), form)
           for name, op, form in read_def(HALTZ_INC / "vm_opcodes.def", 3)}
SYSCALLS = {name.upper(): int(num, 16)
            for name, num, _args, _res in read_def(HALTZ_INC / "vm_syscalls.def", 4)}
REG_RE = re.compile(r"r([0-7])", re.IGNORECASE)


class AsmError(Exception):
    pass


def crc16(data, crc=0xFFFF):
    """CRC-16/CCITT-FALSE, same as shared/haltz_proto/src/crc16.c."""
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def make_header(cart_type, title, body, key_id=0):
    t = title.encode("ascii")
    hdr = bytearray(HEADER_LEN)
    hdr[0:2] = b"HZ"
    hdr[2] = 1  # HALTZ_CART_FORMAT
    hdr[3] = cart_type
    hdr[4:4 + len(t)] = t
    hdr[20:22] = len(body).to_bytes(2, "little")
    hdr[24:28] = key_id.to_bytes(4, "little")
    hdr[30:32] = crc16(body, crc16(hdr[:30])).to_bytes(2, "little")
    return bytes(hdr)


def split_operands(text):
    """Split at commas that are not inside quotes or brackets."""
    out, cur, depth, quote, i = [], "", 0, None, 0
    while i < len(text):
        c = text[i]
        if quote:
            cur += c
            if c == "\\" and i + 1 < len(text):
                cur += text[i + 1]
                i += 1
            elif c == quote:
                quote = None
        elif c in "\"'":
            quote = c
            cur += c
        elif c in "[(":
            depth += 1
            cur += c
        elif c in "])":
            depth -= 1
            cur += c
        elif c == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += c
        i += 1
    if cur.strip():
        out.append(cur.strip())
    return out


def strip_comment(line):
    quote = None
    i = 0
    while i < len(line):
        c = line[i]
        if quote:
            if c == "\\":
                i += 1
            elif c == quote:
                quote = None
        elif c in "\"'":
            quote = c
        elif c == ";":
            return line[:i]
        i += 1
    return line


class Assembler:
    def __init__(self):
        self.symbols = {}
        self.title = None
        self.pc = 0
        self.out = bytearray()
        self.final = False  # pass 2: unknown symbols are errors
        self.listing = []

    # ---- expressions ----
    def value(self, text, need=True):
        """Evaluate an expression. In pass 1 an unknown symbol gives None (if need is False)."""
        text = text.strip()
        if text.startswith("#"):
            text = text[1:].strip()
        if text == "$":
            return self.pc
        try:
            tree = ast.parse(text.replace("$", "_pc_"), mode="eval")
        except SyntaxError:
            raise AsmError(f"bad expression: {text}")
        try:
            return self._eval(tree.body)
        except KeyError as e:
            if self.final or need:
                raise AsmError(f"unknown symbol: {e.args[0]}")
            return None

    def _eval(self, n):
        if isinstance(n, ast.Constant):
            if isinstance(n.value, bool):
                raise AsmError("bad constant")
            if isinstance(n.value, int):
                return n.value
            if isinstance(n.value, str) and len(n.value) == 1 and ord(n.value) < 256:
                return ord(n.value)
            raise AsmError(f"bad constant: {n.value!r}")
        if isinstance(n, ast.Name):
            if n.id == "_pc_":
                return self.pc
            if REG_RE.fullmatch(n.id):
                raise AsmError(f"register {n.id} where a value is needed")
            return self.symbols[n.id]
        if isinstance(n, ast.UnaryOp) and isinstance(n.op, (ast.USub, ast.UAdd, ast.Invert)):
            v = self._eval(n.operand)
            return -v if isinstance(n.op, ast.USub) else (~v if isinstance(n.op, ast.Invert) else v)
        if isinstance(n, ast.BinOp):
            a, b = self._eval(n.left), self._eval(n.right)
            ops = {ast.Add: lambda: a + b, ast.Sub: lambda: a - b, ast.Mult: lambda: a * b,
                   ast.FloorDiv: lambda: a // b, ast.Mod: lambda: a % b,
                   ast.LShift: lambda: a << b, ast.RShift: lambda: a >> b,
                   ast.BitAnd: lambda: a & b, ast.BitOr: lambda: a | b,
                   ast.BitXor: lambda: a ^ b}
            for t, f in ops.items():
                if isinstance(n.op, t):
                    return f()
        raise AsmError("expression uses something other than numbers, symbols and + - * // % << >> & | ^ ~")

    def ranged(self, text, lo, hi, what):
        v = self.value(text, need=False)
        if v is None:
            return 0
        if not lo <= v <= hi:
            raise AsmError(f"{what} {v} out of range {lo}..{hi}")
        return v

    @staticmethod
    def reg(text):
        m = REG_RE.fullmatch(text.strip())
        if not m:
            raise AsmError(f"expected a register r0-r7, got '{text}'")
        return int(m.group(1))

    # ---- instructions ----
    def encode(self, mnem, ops):
        op, form = OPCODES[mnem]
        n = FORM_LEN[form]

        def need(k):
            if len(ops) != k:
                raise AsmError(f"{mnem} takes {k} operand(s), got {len(ops)}")

        def rel(target_text, end):
            t = self.value(target_text, need=False)
            if t is None:
                return 0
            d = t - end
            if not -128 <= d <= 127:
                raise AsmError(f"branch target {t:#06x} is {d} bytes away (range -128..127); "
                               "use jmp, or a branch around a jmp")
            return d & 0xFF

        if form == "N":
            need(0)
            return bytes([op])
        if form == "R":
            need(1)
            return bytes([op, self.reg(ops[0]) << 4])
        if form == "RR":
            need(2)
            return bytes([op, self.reg(ops[0]) << 4 | self.reg(ops[1])])
        if form == "RQ":
            need(2)
            return bytes([op, self.reg(ops[0]) << 4 | (self.ranged(ops[1], -8, 7, "value") & 15)])
        if form == "RS":
            need(2)
            return bytes([op, self.reg(ops[0]) << 4 | self.ranged(ops[1], 0, 15, "shift")])
        if form == "RI8":
            need(2)
            return bytes([op, self.reg(ops[0]) << 4, self.ranged(ops[1], 0, 255, "value")])
        if form == "RI16":
            need(2)
            v = self.ranged(ops[1], -32768, 65535, "value") & 0xFFFF
            return bytes([op, self.reg(ops[0]) << 4]) + v.to_bytes(2, "little")
        if form == "RRO":
            need(2)
            m = re.fullmatch(r"\[\s*(r[0-7])\s*(?:([+-])(.*))?\]", ops[1].strip(), re.IGNORECASE)
            if not m:
                raise AsmError(f"expected [rN], [rN+offset] or [rN-offset], got '{ops[1]}'")
            off = 0
            if m.group(2):
                off = self.ranged(m.group(3), -255, 255, "offset")
                off = -off if m.group(2) == "-" else off
                if not -128 <= off <= 127:
                    raise AsmError(f"offset {off} out of range -128..127")
            return bytes([op, self.reg(ops[0]) << 4 | self.reg(m.group(1)), off & 0xFF])
        if form == "RA":
            need(2)
            a = ops[1].strip()
            if a.startswith("[") and a.endswith("]"):
                a = a[1:-1]
            v = self.ranged(a, 0, 0xFFFF, "address")
            return bytes([op, self.reg(ops[0]) << 4]) + v.to_bytes(2, "little")
        if form == "B8":
            need(1)
            return bytes([op, rel(ops[0], self.pc + n)])
        if form == "RB8":
            need(2)
            return bytes([op, self.reg(ops[0]) << 4, rel(ops[1], self.pc + n)])
        if form == "A16":
            need(1)
            return bytes([op]) + self.ranged(ops[0], 0, 0xFFFF, "address").to_bytes(2, "little")
        if form == "U8":
            if mnem == "sys":
                need(1)
                name = ops[0].strip().upper()
                if name in SYSCALLS:
                    return bytes([op, SYSCALLS[name]])
                return bytes([op, self.ranged(ops[0], 0, 255, "system call")])
            if mnem in ("pushm", "popm"):
                if len(ops) == 1 and not REG_RE.match(ops[0].strip()):
                    return bytes([op, self.ranged(ops[0], 0, 255, "register mask")])
                mask = 0
                for o in ops:
                    m = re.fullmatch(r"r([0-7])\s*-\s*r([0-7])", o.strip(), re.IGNORECASE)
                    if m:
                        for r in range(int(m.group(1)), int(m.group(2)) + 1):
                            mask |= 1 << r
                    else:
                        mask |= 1 << self.reg(o)
                return bytes([op, mask])
            need(1)
            return bytes([op, self.ranged(ops[0], 0, 255, "value")])
        raise AsmError(f"form {form} not handled")

    # ---- directives ----
    def data_bytes(self, ops, width):
        out = bytearray()
        for o in ops:
            o = o.strip()
            if o[:1] == '"':
                if width != 1:
                    raise AsmError("strings only in .byte / .ascii / .asciz")
                out += self.string(o)
                continue
            lo = -(1 << (8 * width - 1))
            hi = (1 << (8 * width)) - 1
            v = self.ranged(o, lo, hi, "value") & hi
            out += v.to_bytes(width, "little")
        return bytes(out)

    @staticmethod
    def string(text):
        try:
            s = ast.literal_eval(text)
        except (SyntaxError, ValueError):
            raise AsmError(f"bad string: {text}")
        if not isinstance(s, str):
            raise AsmError(f"bad string: {text}")
        try:
            return s.encode("latin-1")
        except UnicodeEncodeError:
            raise AsmError("strings hold byte values 0-255 only")

    def directive(self, name, ops):
        if name in (".byte", ".db"):
            return self.data_bytes(ops, 1)
        if name in (".word", ".dw"):
            return self.data_bytes(ops, 2)
        if name in (".ascii", ".asciz"):
            data = b"".join(self.string(o) for o in ops)
            return data + (b"\0" if name == ".asciz" else b"")
        if name in (".zero", ".ds"):
            if len(ops) != 1:
                raise AsmError(f"{name} takes one size")
            return bytes(self.ranged(ops[0], 0, 0x10000, "size"))
        if name == ".org":
            if len(ops) != 1:
                raise AsmError(".org takes one address")
            a = self.value(ops[0])
            if a < self.pc:
                raise AsmError(f".org {a:#06x} is behind the current address {self.pc:#06x}")
            return bytes(a - self.pc)
        if name == ".title":
            if len(ops) != 1:
                raise AsmError(".title takes one string")
            t = self.string(ops[0]).decode("latin-1")
            if len(t) > TITLE_LEN or any(not 0x20 <= ord(c) <= 0x7E for c in t):
                raise AsmError(f"title: up to {TITLE_LEN} printable ASCII characters")
            self.title = t
            return b""
        if name == ".equ":
            if len(ops) != 2:
                raise AsmError(".equ takes a name and a value")
            self.define(ops[0].strip(), self.value(ops[1]))
            return b""
        raise AsmError(f"unknown directive {name}")

    def define(self, name, v):
        if not re.fullmatch(r"[A-Za-z_][\w.]*", name) or REG_RE.fullmatch(name):
            raise AsmError(f"bad symbol name '{name}'")
        if name.lower() in OPCODES or name.lower() in ALIASES:
            raise AsmError(f"'{name}' is an instruction name")
        if not self.final and name in self.symbols and self.symbols[name] != v:
            raise AsmError(f"symbol '{name}' defined twice")
        self.symbols[name] = v

    # ---- passes ----
    def line(self, text):
        text = strip_comment(text).strip()
        while True:
            m = re.match(r"([A-Za-z_][\w.]*)\s*:", text)
            if not m:
                break
            self.define(m.group(1), self.pc)
            text = text[m.end():].strip()
        if not text:
            return b""
        m = re.fullmatch(r"([A-Za-z_][\w.]*)\s*=\s*(.+)", text)
        if m:
            self.define(m.group(1), self.value(m.group(2)))
            return b""
        parts = text.split(None, 1)
        word = parts[0].lower()
        ops = split_operands(parts[1]) if len(parts) > 1 else []
        if word.startswith("."):
            return self.directive(word, ops)
        if word in ALIASES:
            word, extra = ALIASES[word]
            if extra is not None:
                ops = ops + [extra]
        if word not in OPCODES:
            raise AsmError(f"unknown instruction '{parts[0]}'")
        return self.encode(word, ops)

    def run(self, lines, filename):
        for final in (False, True):
            self.final = final
            self.pc = 0
            self.out = bytearray()
            self.listing = []
            for no, text in enumerate(lines, 1):
                try:
                    data = self.line(text)
                except AsmError as e:
                    raise AsmError(f"{filename}:{no}: error: {e}")
                if self.final:
                    self.listing.append((self.pc, data, text.rstrip()))
                self.out += data
                self.pc += len(data)
                if self.pc > IMAGE_BODY_MAX:
                    raise AsmError(f"{filename}:{no}: error: program is larger than "
                                   f"{IMAGE_BODY_MAX} bytes")
        return bytes(self.out)


def write_listing(asm, f):
    for addr, data, text in asm.listing:
        if not data:
            f.write(f"{'':18} {text}\n")
            continue
        for k in range(0, min(len(data), 16), 4):
            hexs = " ".join(f"{b:02X}" for b in data[k:k + 4])
            f.write(f"{addr + k:04X}  {hexs:<12} {text if k == 0 else ''}\n")
        if len(data) > 16:
            f.write(f"{'':6}... {len(data)} bytes in all\n")
    syms = sorted(asm.symbols.items(), key=lambda kv: kv[1])
    if syms:
        f.write("\n; symbols\n")
        for k, v in syms:
            f.write(f"; {v & 0xFFFF:04X}  {k}\n")


def main(argv=None):
    p = argparse.ArgumentParser(description="HALTZ VM assembler (docs/vm-v1.md)")
    p.add_argument("source")
    p.add_argument("-o", "--output", help="image file (default: source with .hzp)")
    p.add_argument("-l", "--listing", help="write a listing ('-' = stdout)")
    p.add_argument("--title", help="title (default: .title, else the file name)")
    p.add_argument("--type", choices=sorted(CART_TYPES), default="code")
    p.add_argument("--key-id", type=lambda s: int(s, 0), default=0, help="for --type key")
    p.add_argument("--raw", action="store_true", help="write only the body, no header")
    a = p.parse_args(argv)

    src = pathlib.Path(a.source)
    asm = Assembler()
    try:
        body = asm.run(src.read_text().splitlines(), src.name)
    except AsmError as e:
        print(e, file=sys.stderr)
        return 1

    title = a.title or asm.title or src.stem.upper()[:TITLE_LEN]
    if a.type == "key" and a.key_id == 0:
        print("hzasm: --type key needs --key-id (not 0)", file=sys.stderr)
        return 1
    if a.type != "key" and a.key_id:
        print("hzasm: --key-id only goes with --type key", file=sys.stderr)
        return 1
    out = pathlib.Path(a.output) if a.output else src.with_suffix(".hzp")
    data = body if a.raw else make_header(CART_TYPES[a.type], title, body, a.key_id) + body
    out.write_bytes(data)

    if a.listing == "-":
        write_listing(asm, sys.stdout)
    elif a.listing:
        with open(a.listing, "w") as f:
            write_listing(asm, f)
    fits = "fits a CODE card" if len(body) <= CART_BODY_MAX else \
        f"too big for a CODE card ({CART_BODY_MAX} bytes): use it as a KEY program in flash"
    print(f"hzasm: {out}: \"{title}\", {len(body)} bytes, {fits}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
