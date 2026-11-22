# HALTZ VM v1 (provisional)

Status: draft, rev 1, 2026-11-22. Goes with architecture-v1.md (CPU and Cartridges sections), gpu-protocol-v1.md and io-protocol-v1.md.
Covers the HALTZ virtual CPU that runs programs on the S3: the machine, the instruction set and its encoding, the system calls, how a program is stopped and inspected, the program image and cartridge formats, and the assembler and PC tools.
The single sources of truth are in `shared/haltz_proto/include/haltz/`: `vm_opcodes.def` (instructions), `vm_syscalls.def` (system calls) and `cart.h` (image header, card layout). The interpreter is `shared/haltz_proto/src/vm.c`. This document explains them.
Points that add to architecture-v1: the cartridge header gets exact offsets and a `key_id` field (section 7), and CODE cards and KEY programs in flash use the same image format.

## Design goals
1. Small programs. A CODE card holds 624 bytes, so instructions are 1 to 4 bytes (most are 2 or 3), and everything a program needs from the system is one 2-byte `SYS`.
2. Easy to learn and to watch. Eight registers, a flat 64 KB memory, four flags, no hidden state. The monitor can show and change all of it between any two instructions.
3. Always stoppable. The interpreter runs a limited number of instructions per call and then returns, so the kernel gets control back at least every 1000 instructions, whatever the program does.
4. Strict. Undefined opcodes, register numbers 8-15 and reserved bits are faults, not "probably harmless". This keeps room for later instructions and finds bugs early.
5. Portable. `vm.c` uses no platform headers: the same file runs on the S3 and on the PC (tests, `hzrun`).

## 1. The machine
| Part | Size | Notes |
|---|---|---|
| r0-r7 | 8 x 16 bits | General registers, all alike. System calls take arguments in r0-r5 and return results in r0-r2 |
| pc | 16 bits | Address of the next instruction |
| sp | 16 bits | Stack pointer. The stack grows down; sp points at the last word pushed |
| flags | C, Z, N, V | Carry/borrow, zero, negative (bit 15), signed overflow (section 5) |
| memory | 64 KB | Bytes, addresses 0x0000-0xFFFF, wraps around. Words are little-endian and may be unaligned |
| steps | 32 bits | Instructions executed since reset (wraps). For the monitor and statistics |

- No memory-mapped I/O: every device is reached through `SYS`. No memory protection either: a program may overwrite itself (like a 1980s computer).
- Start state (after load): all memory 0, then the program body copied to 0x0000; registers, flags, pc and sp all 0. So execution starts at 0x0000, and the first push goes to 0xFFFE-0xFFFF.
- Opcode 0x00 is `BRK`, so a program that runs off into empty memory stops at once with a BREAK instead of sliding through it.
- On the S3 the 64 KB lives in internal RAM (architecture-v1 memory plan). Program images waiting to run live in PSRAM.

## 2. Running, stopping, the monitor
The kernel drives the VM with `haltz_vm_run(vm, n)`: it executes up to n instructions and returns why it stopped:

| Stop | Meaning | pc afterwards |
|---|---|---|
| BUDGET | ran n instructions; still runnable | next instruction |
| YIELD | a system call has to wait (FRAME, SLEEP_MS, SAVE_WRITE) | after the SYS |
| BREAK | `BRK` | after the BRK, so resume continues |
| HALTED | `HALT`: the program stopped itself | after the HALT |
| EXITED | `SYS EXIT`: back to the launcher, exit code in r0 | after the SYS |
| FAULT | see the fault code | at the faulting instruction, unchanged |

Fault codes (`haltz_vm_fault_t`, shown by the monitor, never renumbered): 1 ILLEGAL (undefined opcode, register 8-15, reserved bits set), 2 DIV_ZERO, 3 BAD_SYSCALL (a number not in `vm_syscalls.def`), 4 SYSCALL (the system refused the call). A faulting instruction changes nothing (except registers a refused system call may already have written) and isn't counted, so after fixing memory or registers in the monitor it can simply run again.

**The run loop** (VM task on S3 core 1, planned): `haltz_vm_run(vm, HALTZ_VM_SLICE)` with a slice of 1000 instructions, then check the halt request, feed the task watchdog, and loop. On YIELD the task blocks until the wait is over (the next FRAME pulse, the sleep time, the card write), which also gives core 1 idle time.

**Halting.** Ways to stop a running program (architecture-v1): the S3 BOOT button, a keyboard combo (still TBD), pulling the cartridge, a fault, `BRK`/`HALT` in the program. All of them end up as "the kernel does not call `haltz_vm_run` again". Delay: at most one slice, well under a millisecond (estimated: see section 11). A program blocked in a yielding call is stopped when the wait ends; SLEEP_MS is cut short.

**The monitor.** Between two `haltz_vm_run` calls the VM is idle and every field of `haltz_vm_t` (registers, pc, sp, flags, memory) may be read and changed. Single step = `haltz_vm_run(vm, 1)`. Resume = back to the run loop. `haltz_vm_disasm()` shows the instruction at any address in the assembler's syntax.

**Speed control.** Because the kernel decides how many instructions each call gets, it can slow a program down to a few instructions per second (one candidate meaning of the knob, architecture-v1 open question 1) without the VM knowing.

## 3. Instruction encoding
Byte 0 is the opcode. The opcode alone decides the length and the layout of the rest (its *form*):

| Form | Bytes | Layout | Used for |
|---|---|---|---|
| N | 1 | op | BRK, NOP, HALT, RET |
| R | 2 | op, rd<<4 | one register |
| RR | 2 | op, rd<<4 \| rs | register to register |
| RQ | 2 | op, rd<<4 \| q | q = signed -8..7 (ADDQ, CMPQ) |
| RS | 2 | op, rd<<4 \| n | n = 0..15 (shift by constant) |
| RI8 | 3 | op, rd<<4, u8 | 8-bit constant |
| RI16 | 4 | op, rd<<4, lo, hi | 16-bit constant |
| RRO | 3 | op, rd<<4 \| rb, off | memory at rb + off, off = signed -128..127 |
| RA | 4 | op, rd<<4, lo, hi | memory at an absolute address |
| B8 | 2 | op, rel | branch, rel = signed, counted from the next instruction |
| RB8 | 3 | op, rd<<4, rel | DBNZ |
| A16 | 3 | op, lo, hi | JMP, CALL |
| U8 | 2 | op, u8 | SYS number, PUSHM/POPM register mask |

- Register nibbles must be 0-7. In the forms with only one register (R, RI8, RI16, RA, RB8) the low nibble must be 0. Anything else is fault ILLEGAL.
- Instructions may cross 0xFFFF; addresses wrap.

## 4. Instruction set
73 instructions. "Flags" says which flags the instruction sets: `ZN` = Z and N from the result, C and V cleared; `all` = all four (section 5); `-` = unchanged.

**Control**
| Opcode | Instruction | Does | Flags |
|---|---|---|---|
| 0x00 | `brk` | stop with BREAK (the monitor's breakpoint) | - |
| 0x01 | `nop` | nothing | - |
| 0x02 | `halt` | stop with HALTED | - |
| 0x03 | `ret` | pc = pop | - |

**Register to register** (`op rd, rs`: rd = rd op rs)
| Opcode | Instruction | Does | Flags |
|---|---|---|---|
| 0x10 | `mov rd, rs` | rd = rs | - |
| 0x11 | `add rd, rs` | rd = rd + rs | all |
| 0x12 | `sub rd, rs` | rd = rd - rs | all |
| 0x13 | `adc rd, rs` | rd = rd + rs + C | all |
| 0x14 | `sbc rd, rs` | rd = rd - rs - C | all |
| 0x15 | `and rd, rs` | rd = rd & rs | ZN |
| 0x16 | `or rd, rs` | rd = rd \| rs | ZN |
| 0x17 | `xor rd, rs` | rd = rd ^ rs | ZN |
| 0x18 | `cmp rd, rs` | flags of rd - rs, rd unchanged | all |
| 0x19 | `tst rd, rs` | flags of rd & rs, rd unchanged | ZN |
| 0x1A | `shl rd, rs` | rd = rd << (rs & 15) | ZN, C = last bit out |
| 0x1B | `shr rd, rs` | rd = rd >> (rs & 15), zeros in | ZN, C = last bit out |
| 0x1C | `sar rd, rs` | rd = rd >> (rs & 15), sign bit copied in | ZN, C = last bit out |
| 0x1D | `mul rd, rs` | rd = low 16 bits of rd x rs | ZN, C = result didn't fit |
| 0x1E | `divu rd, rs` | rd = rd / rs, unsigned | ZN; rs = 0: fault DIV_ZERO |
| 0x1F | `modu rd, rs` | rd = rd % rs, unsigned | ZN; DIV_ZERO |
| 0x20 | `divs rd, rs` | rd = rd / rs, signed, rounds toward 0 | ZN; DIV_ZERO; -32768 / -1 = -32768 with V |
| 0x21 | `mods rd, rs` | rd = rd % rs, signed, sign of rd | ZN; DIV_ZERO; -32768 % -1 = 0 with V |

**One register**
| Opcode | Instruction | Does | Flags |
|---|---|---|---|
| 0x28 | `not rd` | rd = ~rd | ZN |
| 0x29 | `neg rd` | rd = 0 - rd | all (as SUB) |
| 0x2A | `sxb rd` | rd = low byte, sign-extended | - |
| 0x2B | `zxb rd` | rd = low byte (rd & 0xFF) | - |
| 0x2C | `getsp rd` | rd = sp | - |
| 0x2D | `setsp rd` | sp = rd | - |

**Small constants**
| Opcode | Instruction | Does | Flags |
|---|---|---|---|
| 0x30 | `addq rd, q` | rd = rd + q, q = -8..7 (assembler: `inc rd`, `dec rd`) | all, as ADD of the 16-bit value of q |
| 0x31 | `cmpq rd, q` | flags of rd - q | all |
| 0x32 | `shlq rd, n` | rd = rd << n, n = 0..15 | ZN, C |
| 0x33 | `shrq rd, n` | rd = rd >> n | ZN, C |
| 0x34 | `sarq rd, n` | rd = rd >> n, signed | ZN, C |
| 0x38 | `movi8 rd, u8` | rd = u8 (0-255, high byte 0) | - |
| 0x39 | `cmpi8 rd, u8` | flags of rd - u8 | all |

**16-bit constants**
| Opcode | Instruction | Does | Flags |
|---|---|---|---|
| 0x40 | `movi rd, k` | rd = k | - |
| 0x41 | `addi rd, k` | rd = rd + k (also for subtracting: k = -n) | all |
| 0x42 | `cmpi rd, k` | flags of rd - k | all |
| 0x43 | `andi rd, k` | rd = rd & k | ZN |
| 0x44 | `ori rd, k` | rd = rd \| k | ZN |
| 0x45 | `xori rd, k` | rd = rd ^ k | ZN |

**Memory** (loads and stores never change the flags)
| Opcode | Instruction | Does |
|---|---|---|
| 0x48 | `ld rd, [rb+off]` | rd = word at rb + off |
| 0x49 | `ldb rd, [rb+off]` | rd = byte at rb + off (high byte 0) |
| 0x4A | `st rs, [rb+off]` | word at rb + off = rs |
| 0x4B | `stb rs, [rb+off]` | byte at rb + off = low byte of rs |
| 0x4C | `lda rd, [addr]` | rd = word at addr |
| 0x4D | `ldba rd, [addr]` | rd = byte at addr |
| 0x4E | `sta rs, [addr]` | word at addr = rs |
| 0x4F | `stba rs, [addr]` | byte at addr = low byte of rs |

**Branches** (`bxx target`: if the condition holds, pc = target; target within -128..+127 bytes of the next instruction; flags unchanged)
| Opcode | Instruction | Condition | Meaning after `cmp a, b` |
|---|---|---|---|
| 0x50 | `bra` | always | |
| 0x51 | `beq` | Z | a == b |
| 0x52 | `bne` | !Z | a != b |
| 0x53 | `bcs` | C | a < b, unsigned ("lower") |
| 0x54 | `bcc` | !C | a >= b, unsigned |
| 0x55 | `bmi` | N | result negative |
| 0x56 | `bpl` | !N | result positive or 0 |
| 0x57 | `bvs` | V | signed overflow |
| 0x58 | `bvc` | !V | no signed overflow |
| 0x59 | `bhi` | !C and !Z | a > b, unsigned |
| 0x5A | `bls` | C or Z | a <= b, unsigned |
| 0x5B | `blt` | N != V | a < b, signed |
| 0x5C | `bge` | N == V | a >= b, signed |
| 0x5D | `bgt` | !Z and N == V | a > b, signed |
| 0x5E | `ble` | Z or N != V | a <= b, signed |
| 0x5F | `bsr` | always, push the return address first | (short call) |

**Jumps, calls, stack, system**
| Opcode | Instruction | Does |
|---|---|---|
| 0x60 | `jmp addr` | pc = addr |
| 0x61 | `call addr` | push the address of the next instruction, pc = addr |
| 0x62 | `jmpr rd` | pc = rd (jump tables, computed jumps) |
| 0x63 | `callr rd` | push the next address, pc = rd |
| 0x64 | `dbnz rd, target` | rd = rd - 1; if rd != 0 branch (loops; flags unchanged) |
| 0x68 | `pushm list` | push the listed registers, r7 first ... r0 last (assembler: `push`) |
| 0x69 | `popm list` | pop into the listed registers, r0 first ... r7 last (assembler: `pop`) |
| 0x6A | `sys name` | system call (section 6) |

Push = sp - 2, then store the word at sp. Pop = load the word at sp, then sp + 2. Because PUSHM and POPM go in opposite register orders, `push r1, r4` ... `pop r1, r4` restores both, whatever order the list is written in.

Free opcodes (0x04-0x0F, 0x22-0x27, 0x2E-0x2F, 0x35-0x37, 0x3A-0x3F, 0x46-0x47, 0x65-0x67, 0x6B-0xFF) fault ILLEGAL; later instructions take them.

## 5. Flags
- **ADD, ADC, ADDQ, ADDI**: C = the unsigned result didn't fit in 16 bits (carry out). V = the signed result didn't fit (both inputs had the same sign and the result has the other).
- **SUB, SBC, CMP, CMPQ, CMPI8, CMPI, NEG**: C = borrow: a < b (+ C for SBC) as unsigned numbers. This is the 6800/x86 convention, not the 6502/ARM one: after `cmp a, b`, C set means "a is lower". V = the signed result didn't fit.
- ADDQ / CMPQ use the 16-bit value of q, so `addq r1, -1` is r1 + 0xFFFF and sets C unless r1 was 0 (like any two's-complement add).
- A 32-bit add: `add r0, r2` then `adc r1, r3` (low words first). A 32-bit compare or subtract works the same way with `sub` / `sbc`.
- **Logic** (AND, OR, XOR, TST, NOT, ANDI, ORI, XORI), **DIVU/MODU**: Z and N, C = V = 0.
- **Shifts**: Z and N; C = the last bit shifted out (0 for a shift by 0); V = 0.
- **MUL**: Z and N of the low 16 bits; C = the full product was bigger than 0xFFFF; V = 0.
- **DIVS/MODS**: Z, N, C = 0; V = 1 only for -32768 / -1.
- Everything else (moves, loads, stores, MOVI, SXB, ZXB, stack, jumps, DBNZ, SYS) leaves the flags alone. A system call never changes them.

## 6. System calls
`sys NAME` (2 bytes): arguments in r0, r1, ... in the order listed, results in r0, r1, r2. **A call changes only its result registers** (and memory it was asked to fill). Commands return r0 = `haltz_err_t`: 0 OK, 7 BUSY (device not there now, e.g. no card), 8 BAD_VALUE (a value out of range, or a buffer that runs past 0xFFFF: nothing is done).
Conventions: "colours" = fg | bg << 8 (palette indices). Signed values (x, y, rows) are 16-bit two's complement. Buffers are `addr, n` with n counted in elements (characters, words, bytes, notes), and must not wrap past 0xFFFF.
Numbers: video calls use the GPU message IDs and sound/LED calls the I/O message IDs, so `sys 0x28` and GPU_SPRITE_SET are the same thing.

**System**
| No | Call | Arguments | Results | Does |
|---|---|---|---|---|
| 0x00 | EXIT | code | - | ends the program, back to the launcher. 0 = OK |
| 0x01 | FRAME | - | frame count | shows everything drawn since the last FRAME (GPU_PRESENT) and waits until it is on the panel (vsync). A game's main loop calls it once per frame. Yields |
| 0x02 | TIME_MS | - | low, high | ms since the program started, 32 bits |
| 0x03 | RANDOM | - | value | 16 random bits (S3 hardware RNG) |
| 0x04 | SLEEP_MS | ms | - | waits. Yields |

**Console** (the text layer used as a 40 x 30 terminal, the kernel's console engine `con_term.c`)
| No | Call | Arguments | Does |
|---|---|---|---|
| 0x08 | PUTC | char | writes one character at the cursor; `\n` `\r` `\b` `\t` work, the screen scrolls |
| 0x09 | PRINT | addr | a zero-terminated string (at most 1024 characters) |
| 0x0A | PRINT_NUM | value, format | format 0 unsigned decimal, 1 signed decimal, 2 hex (4 digits); any other format prints like 0 |
| 0x0B | CLS | - | clears the screen, cursor to the top left |
| 0x0C | LOCATE | col, row | moves the cursor |
| 0x0D | COLOR | colours | colours for the following text |

The console and the TEXT_* calls write to the same text layer; mixing them is allowed (the console only keeps its cursor and colours).

**Video** (one call per GPU message; same values and rules as gpu-protocol-v1 section 4, checked by the same `haltz_gpu_cmd_pack()`; the picture changes at the next FRAME; all return r0 = error)
| No | Call | Arguments |
|---|---|---|
| 0x10 | GFX_INIT | - (picture back to the defaults; also resets the console) |
| 0x13 | BACKLIGHT | level |
| 0x14 | LAYERS | mask |
| 0x15 | PALETTE | first, addr, n (n RGB565 words) |
| 0x18 | TEXT_PUT | col, row, colours, addr, n (n characters) |
| 0x19 | TEXT_FILL | col, row, w, h, char, colours |
| 0x1A | TEXT_SCROLL | top, height, rows, char, colours |
| 0x1B | TEXT_CURSOR | col, row, style |
| 0x1C | FONT_LOAD | first, addr, n (n glyphs of 8 bytes) |
| 0x20 | PATTERN_LOAD | index, addr (128 bytes) |
| 0x21 | TILE_PUT | x, y, addr, n (n map entries, words) |
| 0x22 | TILE_FILL | x, y, w, h, entry |
| 0x23 | TILE_SCROLL | sx, sy |
| 0x28 | SPRITE_SET | id, x, y, pattern, attr |
| 0x30 | BMP_PIXELS | x, y, addr, n (n colour bytes) |
| 0x31 | BMP_FILL | x, y, w, h, colour |
| 0x32 | BMP_LINE | x0, y0, x1, y1, colour |
| 0x33 | BMP_COPY | sx, sy, w, h, dx, dy |

Long buffers are split into several GPU messages by the S3 driver, as for the kernel's own drawing. Sprite 63 is the pointer by kernel convention; a program may take it.

**Input**
| No | Call | Arguments | Results | Does |
|---|---|---|---|---|
| 0x40 | KEY_GET | - | char, usage, modifiers | the oldest key press from a queue of 16 (presses and key repeats only). usage 0 = no key waiting. char is 0 for keys without a character (arrows, F keys) |
| 0x41 | STICK | - | x, y, buttons | thumbstick now: -1000..1000, right and up positive (io-protocol-v1 section 3) |
| 0x42 | KNOB | - | value | 0-255 |
| 0x44 | KEY_HELD | usage | 1 or 0 | whether that key is held now (games) |
| 0x45 | POINTER | - | x, y, buttons | pointer position in screen pixels; buttons bit 0 left, bit 1 right. Until the pointer logic exists (architecture-v1 step 8) it is the screen centre and the stick buttons |

**Sound and LED** (one call per I/O message, same values and rules as io-protocol-v1 section 6; all return r0 = error, BUSY while the I/O link is down)
| No | Call | Arguments |
|---|---|---|
| 0x50 | LED | mode, r, g, b, period_ms, count |
| 0x51 | TONE | voice, hz, ms |
| 0x52 | NOTES | voice, flags, tick_ms, addr, n (n note/ticks byte pairs) |
| 0x53 | SFX | id, voice |
| 0x54 | STOP | voices |
| 0x55 | VOLUME | voices, level |

When the program ends, the kernel puts the system LED pattern and volumes back.

**Cartridge save area** (96 bytes on the card, section 8)
| No | Call | Arguments | Results | Does |
|---|---|---|---|---|
| 0x60 | SAVE_READ | offset, addr, n | error | copies n bytes of the save area from offset into memory |
| 0x61 | SAVE_WRITE | offset, addr, n | error | writes n bytes from memory to the save area on the card. Slow (each touched 16-byte block is a card write) and the card wears (about 100,000 writes per block), so save on purpose, never every frame. Yields. BUSY: no card |
| 0x62 | CART_INFO | - | type, save size | type of the card that started the program (section 7); save size 96, or 0 if there is no card |

Numbers 0x80-0xFF are kept for system ROM routines (architecture-v1: shared routines in S3 flash that cards can call). Not designed yet.

**Suggested register use** (not enforced; only system calls have fixed rules): subroutines take arguments in r0-r3 and return in r0, may change r0-r3, and keep r4-r7. The examples follow this.

## 7. Program images (.hzp)
One format for everything: a 32-byte header, then the body. A CODE card stores it across its sectors (section 8); a KEY program in S3 flash and a file on the PC (`.hzp`) store it as header + body.

| Offset | Field | Type | Rule |
|---|---|---|---|
| 0 | magic | 2 bytes | `H` `Z` |
| 2 | format | u8 | 1 (`HALTZ_CART_FORMAT`); another value: HALTZ_ERR_VERSION |
| 3 | type | u8 | 0 CODE, 1 KEY, 2 DATA |
| 4 | title | 16 bytes | printable ASCII 0x20-0x7E, padded with 0 (16 characters = no 0) |
| 20 | length | u16 | body bytes. CODE card: up to 624; file in flash: up to 65535 |
| 22 | flags | u8 | 0 (reserved) |
| 23 | reserved | u8 | 0 |
| 24 | key_id | u32 | KEY: the program's ID in flash, not 0. CODE and DATA: 0 |
| 28 | reserved | u16 | 0 |
| 30 | crc | u16 | CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF; `crc16.h`) of bytes 0-29, then of the body |

- Little-endian, like everything else. Header + body are checked together by `haltz_cart_check()`: magic, format, field rules, length, CRC.
- CODE: the body is the program, loaded at 0x0000 and started there.
- KEY: the card only names a program in flash (`key_id`); its body may be empty. The flash program is itself a CODE image (planned path: `/progs/<key_id as 8 hex digits>.hzp` on the LittleFS partition, architecture-v1 step 7). For anything bigger than 624 bytes.
- DATA: saves, settings, a profile. Not runnable; the launcher shows it as data.

## 8. Cartridge layout (MIFARE Classic 1K)
1 KB = 16 sectors x 4 blocks x 16 bytes. Block 0 is the manufacturer block (UID, read-only); block 3 of every sector is its trailer (keys and access bits). That leaves 752 bytes:

| Area | Blocks | Bytes |
|---|---|---|
| Header | sector 0, blocks 1-2 | 32 |
| Body | sectors 1-13, blocks 0-2 of each (block numbers 4-6, 8-10, ... 52-54) | 624 |
| Save area | sectors 14-15, blocks 0-2 (56-58, 60-62) | 96 |

`haltz_cart_body_block(offset)` and `haltz_cart_save_block(offset)` turn a byte offset into the block number (the byte is at offset % 16 in it), so the reader skips the trailers.
Reading a card: header first, then only the body blocks the length needs (a short program reads fast), then the CRC check. The card's UID is its serial number (shown in the monitor; not part of the image).
Sector keys and access bits (so a card can't be overwritten by accident; not real security) are set by the burner and decided with the RC522 driver (open question 3).

## 9. The assembler (tools/hzasm.py)
```
python3 tools/hzasm.py prog.s                       # writes prog.hzp
python3 tools/hzasm.py prog.s -o out.hzp -l -       # listing on stdout
python3 tools/hzasm.py data.s --type data           # DATA image
python3 tools/hzasm.py menu.s --type key --key-id 0x1001
```
Reads `vm_opcodes.def` and `vm_syscalls.def` itself, so a new instruction or call needs no assembler change. Standard library only.

Syntax, one statement per line:
```
; comment to the end of the line
        .title  "HELLO"          ; title in the header (else the file name)
WIDTH = 40                        ; constant (also .equ WIDTH, 40)
start:  movi    r0, msg           ; label: its address
        sys     PRINT             ; system call by name (or number)
        ld      r1, [r2+4]        ; memory: [rN], [rN+k], [rN-k]
        lda     r1, [counter]     ; absolute: [addr] or addr
        push    r4, r5            ; pushm/popm take a list (r0-r3 ranges too)
        bne     start             ; branch target: a label or an address
        halt
msg:    .asciz  "HI\n"            ; string with a 0 at the end (.ascii: without)
table:  .word   start, 0x1234     ; 16-bit words, little-endian
bytes:  .byte   1, 2, 'A', "text" ; bytes and strings
buf:    .zero   32                ; 32 zero bytes (.ds)
        .org    0x0400            ; pad with zeros up to an address
```
- Mnemonics, registers and call names ignore case; labels don't. Immediates may be written with or without `#`.
- Expressions: numbers (`42`, `0x2A`, `0b101010`, `'*'`), labels, constants, `$` (the address of this statement) and `+ - * // % << >> & | ^ ~` with brackets.
- Short forms: `inc rd` = `addq rd, 1`, `dec rd` = `addq rd, -1`, `push` = `pushm`, `pop` = `popm`.
- The assembler never picks an instruction for you: `movi8` vs `movi`, `bxx` vs `jmp` are your choice (short forms save card space). A branch that is too far is an error that says so.
- Two passes; errors give `file:line: error: ...`. At the end it prints the size and whether it fits a CODE card.

## 10. PC tools and tests
- `hzrun` (built with the PC tests: `cmake -S shared/haltz_proto -B build-host -DHALTZ_BUILD_TESTS=ON`, then `cmake --build build-host`): runs a `.hzp` with the real interpreter. Console calls go to stdout; time is simulated (5 million instructions per second, FRAME adds 16 ms), so runs repeat exactly; video, sound and LED calls answer OK and do nothing (`-t` lists every call). `-d` disassembles, `-s` traces every instruction with the registers, `-n N` limits the run, `-k TEXT` types keys. Exit status: the EXIT code; 0 after HALT; 2 BRK; 3 fault; 4 limit; 1 bad file.
- `build-host/haltz_proto_tests`: the ALU and every flag against a reference model written differently (14 values x 14 values x carry in x 17 operations), every branch condition after CMP, immediates, memory and wrap-around, stack and calls, every stop and fault, system call plumbing, the disassembler, the CRC-16 check value, the image header rules and the card block map.
- `tools/test_hzasm.py`: every instruction assembled, disassembled by the C disassembler and assembled again must give the same bytes (assembler, .def files and `vm.c` agree); the examples run with the expected output; assembler errors.
- Examples: `tools/vm-examples/hello.s` (122 bytes: text, a loop, a recursive factorial) and `echo.s` (keys, FRAME).

## 11. S3 side (planned, architecture-v1 step 7)
Not written yet. The plan:
- A `vm` task pinned to core 1 runs the loop of section 2. VM memory: 64 KB from internal RAM (`heap_caps_malloc(..., MALLOC_CAP_INTERNAL)`), allocated once at boot.
- System calls run on core 1 inside the VM task and call the existing drivers: `gpu_*()` (thread-safe, `gpu.h`), `io_*()` (`io.h`), the console engine, `io_get_input()` and a key queue fed from `io_get_event()`. FRAME = `gpu_present()` + `gpu_vsync()`.
- Before the VM starts, the kernel takes the text layer from the boot console (the console window becomes the program's screen) and gives it back afterwards.
- Loader: card -> image in PSRAM -> `haltz_cart_check()` -> `haltz_vm_load()`. Then the launcher, the monitor screen, LittleFS for KEY programs, and the power sequence.
- Speed: not measured yet. A switch-based interpreter like `vm.c` usually needs a few tens of cycles per instruction, so roughly 5-10 million instructions per second at 240 MHz (to be measured on the board; `hzrun`'s 5 MIPS clock is a guess on purpose).

## 12. Bring-up order
1. PC (done 2026-11-22): `vm.c`, `cart.c`, `hzasm.py`, `hzrun`, tests and examples.
2. S3, VM task alone: a built-in test program (an image compiled into the firmware) runs on core 1 and prints on the TFT console; the BOOT button halts it; a register dump on the console.
3. System calls: console, then video (a sprite demo in assembly), then input and sound.
4. RC522 driver (finishes architecture-v1 bring-up step 2): read a card, check the image, run it; pull the card -> halt. Burner mode: write a `.hzp` from the PC to a card.
5. Monitor screen, launcher, LittleFS + KEY programs, power sequence.

## Open questions
1. Halt keyboard combo and the MENU key (architecture-v1, still TBD). Suggestion: Ctrl+Alt+Backspace halts, Esc in the monitor goes to the launcher.
2. Should a system call get a cycle cost, so the instruction budget (and the knob's slow motion) treats a big BMP_PIXELS like many instructions?
3. Card keys and access bits: one HALTZ key for all cards, or per card? Decide with the RC522 driver.
4. Should `hzrun` grow a picture mode (render the GPU calls to PNG with the GPU's own renderer, like `firmware/cpu-s3/test/test_demo.c`) so graphics programs can be tested on the PC?
5. System ROM routines (0x80-0xFF): what goes in first (number printing, line input, sprite helpers, a tiny tune player)?
