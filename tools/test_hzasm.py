#!/usr/bin/env python3
"""Tests for hzasm.py together with the C VM (through hzrun).

    python3 tools/test_hzasm.py [path/to/hzrun]     (default: build-host/hzrun)

1. Every opcode, assembled, disassembled by the C disassembler (hzrun -d) and assembled
   again, gives the same bytes. So the assembler, vm_opcodes.def and vm.c agree.
2. The example programs in tools/vm-examples run and print what they should.
3. Mistakes in a source file give an error with the line number.
Standard library only.
"""
import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import hzasm  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXAMPLES = ROOT / "tools" / "vm-examples"

fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print(f"  FAIL {what}")


def operands(mnem, form):
    """A few operand lists for one instruction."""
    if form == "N":
        return [""]
    if form == "R":
        return ["r0", "r7"]
    if form == "RR":
        return ["r1, r2", "r7, r0"]
    if form == "RQ":
        return ["r3, -8", "r3, 7", "r0, 0"]
    if form == "RS":
        return ["r4, 0", "r4, 15"]
    if form == "RI8":
        return ["r5, 0", "r5, 255"]
    if form == "RI16":
        return ["r6, 0x0000", "r6, 0xBEEF"]
    if form == "RRO":
        return ["r1, [r2]", "r1, [r2+127]", "r1, [r2-128]"]
    if form == "RA":
        return ["r2, [0x1234]", "r2, [0xFFFF]"]
    if form == "B8":
        return ["$", "$+2", "$-100", "$+120"]
    if form == "RB8":
        return ["r6, $", "r6, $+3"]
    if form == "A16":
        return ["0xABCD", "0x0000"]
    if form == "U8":
        if mnem == "sys":
            return ["PRINT", "EXIT", "0x07"]
        if mnem in ("pushm", "popm"):
            return ["r0", "r0, r3, r7", "0x00"]
        return ["0x12"]
    raise AssertionError(form)


def disasm(hzrun, body):
    with tempfile.TemporaryDirectory() as d:
        f = pathlib.Path(d) / "t.hzp"
        f.write_bytes(hzasm.make_header(0, "T", body) + body)
        out = subprocess.run([hzrun, "-d", str(f)], capture_output=True, text=True, check=True)
    lines = []
    for line in out.stdout.splitlines():
        if line.startswith(";"):
            continue
        # "ADDR  B0 B1 B2 B3   text": the text starts after the 4 byte columns
        lines.append(line[6 + 12 + 1:].strip())
    return lines


def test_roundtrip(hzrun):
    src = ["        .org 0x80"]
    for mnem, (_op, form) in sorted(hzasm.OPCODES.items(), key=lambda kv: kv[1][0]):
        for ops in operands(mnem, form):
            src.append(f"        {mnem} {ops}")
    a = hzasm.Assembler()
    body = a.run(src, "all.s")
    text = disasm(hzrun, body)
    # The .org padding disassembles as 128 BRKs; put them back as .org.
    text = [t for t in text[0x80:]]
    again = hzasm.Assembler().run([".org 0x80"] + text, "again.s")
    check(again == body, "every opcode: assemble -> hzrun -d -> assemble gives the same bytes")
    n = sum(len(operands(m, f)) for m, (_o, f) in hzasm.OPCODES.items())
    check(len(text) == n, f"disassembly has {n} instructions (got {len(text)})")


def run(hzrun, image, *args):
    return subprocess.run([hzrun, *args, str(image)], capture_output=True, text=True)


def test_examples(hzrun):
    with tempfile.TemporaryDirectory() as d:
        for name in ("hello", "echo"):
            check(hzasm.main([str(EXAMPLES / f"{name}.s"), "-o", f"{d}/{name}.hzp"]) == 0,
                  f"{name}.s assembles")
        r = run(hzrun, f"{d}/hello.hzp")
        check(r.returncode == 0, "hello exits with 0")
        check(r.stdout == "HELLO FROM HALTZ\n1 2 3 4 5 6 7 8 9 10 \nSUM 55\n7! = 5040\n",
              f"hello output: {r.stdout!r}")
        r = run(hzrun, f"{d}/echo.hzp", "-k", "hi there\n")
        check(r.returncode == 0 and r.stdout == "TYPE> hi there\nKEYS: 8\n",
              f"echo output: {r.stdout!r}")
        r = run(hzrun, f"{d}/echo.hzp")
        check(r.returncode == 1 and r.stdout.endswith("NO KEYS\n"), "echo times out")
        # The instruction limit stops a program.
        r = run(hzrun, f"{d}/echo.hzp", "-n", "50")
        check(r.returncode == 4, "-n stops the run (status 4)")
        # A damaged image is refused (CRC).
        img = bytearray(pathlib.Path(f"{d}/hello.hzp").read_bytes())
        img[40] ^= 1
        pathlib.Path(f"{d}/bad.hzp").write_bytes(img)
        check(run(hzrun, f"{d}/bad.hzp").returncode == 1, "hzrun refuses a bad CRC")


def test_errors():
    cases = {
        "far: bra far2\n .zero 200\nfar2: nop": "range -128..127",
        "add r1, r8": "expected a register",
        "frob r1": "unknown instruction",
        "addq r1, 8": "out of range",
        "ld r1, [r2+200]": "out of range",
        "x: nop\nx: nop": "defined twice",
        "movi r0, nowhere": "unknown symbol",
        ".title \"THIS TITLE IS TOO LONG\"": "title",
        ".org 4\n.org 2": "behind",
        "movi r0, r1": "register r1 where a value is needed",
    }
    for src, want in cases.items():
        try:
            hzasm.Assembler().run(src.splitlines(), "e.s")
            check(False, f"no error for {src!r}")
        except hzasm.AsmError as e:
            check(want in str(e) and "e.s:" in str(e), f"error for {src!r}: {e}")
    # Expressions, equates, characters, $.
    body = hzasm.Assembler().run(
        ["N = 3", ".equ M, N*2+1", "movi8 r0, M", "movi8 r1, 'A'+1", "here: .word $, here",
         ".byte -1, 0x7f, \"ok\\n\""], "x.s")
    check(body == bytes([0x38, 0x00, 7, 0x38, 0x10, 0x42, 6, 0, 6, 0, 0xFF, 0x7F]) + b"ok\n",
          f"expressions: {body.hex()}")


def main():
    hzrun = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "build-host" / "hzrun")
    if not pathlib.Path(hzrun).exists():
        print(f"no hzrun at {hzrun}: build it first "
              "(cmake -S shared/haltz_proto -B build-host -DHALTZ_BUILD_TESTS=ON)")
        return 1
    for name, fn in (("opcode round trip", lambda: test_roundtrip(hzrun)),
                     ("examples", lambda: test_examples(hzrun)),
                     ("errors and expressions", test_errors)):
        before = fails
        fn()
        print(f"{'PASS' if fails == before else 'FAIL'} {name}")
    print(f"\n{fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
