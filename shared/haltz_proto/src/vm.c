#include "haltz/vm.h"

#include <stdio.h>
#include <string.h>

/* ---- tables generated from vm_opcodes.def / vm_syscalls.def ---------------------------- */

#define X(name, op, form)                                                                      \
    _Static_assert((op) >= 0x00 && (op) <= 0xFF, "opcode " #name ": must be 0x00..0xFF");
#include "haltz/vm_opcodes.def"
#undef X

#define X(name, num, args, results)                                                            \
    _Static_assert((num) >= 0x00 && (num) <= 0xFF, "syscall " #name ": must be 0x00..0xFF");  \
    _Static_assert((args) >= 0 && (args) <= 6, "syscall " #name ": at most 6 arguments");     \
    _Static_assert((results) >= 0 && (results) <= 3, "syscall " #name ": at most 3 results");
#include "haltz/vm_syscalls.def"
#undef X

static const uint8_t k_form[256] = {
#define X(name, op, form) [(op)] = HALTZ_VM_FORM_##form,
#include "haltz/vm_opcodes.def"
#undef X
};

static const uint8_t k_form_len[] = {
    [HALTZ_VM_FORM_NONE] = 0, [HALTZ_VM_FORM_N] = 1,    [HALTZ_VM_FORM_R] = 2,
    [HALTZ_VM_FORM_RR] = 2,   [HALTZ_VM_FORM_RQ] = 2,   [HALTZ_VM_FORM_RS] = 2,
    [HALTZ_VM_FORM_RI8] = 3,  [HALTZ_VM_FORM_RI16] = 4, [HALTZ_VM_FORM_RRO] = 3,
    [HALTZ_VM_FORM_RA] = 4,   [HALTZ_VM_FORM_B8] = 2,   [HALTZ_VM_FORM_RB8] = 3,
    [HALTZ_VM_FORM_A16] = 3,  [HALTZ_VM_FORM_U8] = 2,
};

/* A duplicate opcode in vm_opcodes.def becomes a duplicate case label: a compile error. */
const char *haltz_vm_op_name(uint8_t opcode) {
    switch (opcode) {
#define X(name, op, form)                                                                      \
    case (op): return #name;
#include "haltz/vm_opcodes.def"
#undef X
    default: return NULL;
    }
}

static const haltz_vm_sys_info_t k_sys[] = {
#define X(name, num, args, results) {#name, (num), (args), (results)},
#include "haltz/vm_syscalls.def"
#undef X
};

enum {
#define X(name, num, args, results) SYS_INDEX_##name,
#include "haltz/vm_syscalls.def"
#undef X
};

/* Same trick for duplicate syscall numbers. */
const haltz_vm_sys_info_t *haltz_vm_sys_info(uint8_t num) {
    switch (num) {
#define X(name, n, args, results)                                                              \
    case (n): return &k_sys[SYS_INDEX_##name];
#include "haltz/vm_syscalls.def"
#undef X
    default: return NULL;
    }
}

haltz_vm_form_t haltz_vm_op_form(uint8_t opcode) { return (haltz_vm_form_t)k_form[opcode]; }

unsigned haltz_vm_op_len(uint8_t opcode) { return k_form_len[k_form[opcode]]; }

const char *haltz_vm_fault_name(uint8_t fault) {
    switch (fault) {
    case HALTZ_VM_FAULT_NONE: return "NONE";
    case HALTZ_VM_FAULT_ILLEGAL: return "ILLEGAL";
    case HALTZ_VM_FAULT_DIV_ZERO: return "DIV_ZERO";
    case HALTZ_VM_FAULT_BAD_SYSCALL: return "BAD_SYSCALL";
    case HALTZ_VM_FAULT_SYSCALL: return "SYSCALL";
    default: return "?";
    }
}

const char *haltz_vm_stop_name(haltz_vm_stop_t stop) {
    switch (stop) {
    case HALTZ_VM_BUDGET: return "BUDGET";
    case HALTZ_VM_YIELD: return "YIELD";
    case HALTZ_VM_BREAK: return "BREAK";
    case HALTZ_VM_HALTED: return "HALTED";
    case HALTZ_VM_EXITED: return "EXITED";
    case HALTZ_VM_FAULT: return "FAULT";
    default: return "?";
    }
}

/* True if the register byte b1 is valid for the form (vm-v1 section 3). */
static int operands_ok(haltz_vm_form_t form, uint8_t b1) {
    unsigned hi = b1 >> 4, lo = b1 & 0x0Fu;
    switch (form) {
    case HALTZ_VM_FORM_R:
    case HALTZ_VM_FORM_RI8:
    case HALTZ_VM_FORM_RI16:
    case HALTZ_VM_FORM_RA:
    case HALTZ_VM_FORM_RB8: return hi < 8u && lo == 0u;
    case HALTZ_VM_FORM_RR:
    case HALTZ_VM_FORM_RRO: return hi < 8u && lo < 8u;
    case HALTZ_VM_FORM_RQ:
    case HALTZ_VM_FORM_RS: return hi < 8u;
    case HALTZ_VM_FORM_NONE: return 0;
    default: return 1;
    }
}

/* ---- machine ----------------------------------------------------------------------------- */

void haltz_vm_reset(haltz_vm_t *vm) {
    memset(vm->r, 0, sizeof vm->r);
    vm->pc = 0;
    vm->sp = 0;
    vm->flags = 0;
    vm->fault = HALTZ_VM_FAULT_NONE;
    vm->steps = 0;
}

void haltz_vm_init(haltz_vm_t *vm, uint8_t *mem, haltz_vm_sys_fn sys, void *user) {
    vm->mem = mem;
    vm->sys = sys;
    vm->user = user;
    haltz_vm_reset(vm);
}

haltz_err_t haltz_vm_load(haltz_vm_t *vm, const uint8_t *prog, size_t len) {
    if (vm == NULL || vm->mem == NULL || (prog == NULL && len > 0) || len > HALTZ_VM_MEM_SIZE) {
        return HALTZ_ERR_ARG;
    }
    memset(vm->mem, 0, HALTZ_VM_MEM_SIZE);
    if (len > 0) {
        memcpy(vm->mem, prog, len);
    }
    haltz_vm_reset(vm);
    return HALTZ_OK;
}

#define F_C HALTZ_VM_FLAG_C
#define F_Z HALTZ_VM_FLAG_Z
#define F_N HALTZ_VM_FLAG_N
#define F_V HALTZ_VM_FLAG_V

static inline uint8_t zn(uint16_t v) {
    return (uint8_t)((v == 0 ? F_Z : 0u) | ((v & 0x8000u) ? F_N : 0u));
}

/* Logic result: Z and N from the value, C and V cleared. */
static inline uint16_t logic(haltz_vm_t *vm, uint16_t v) {
    vm->flags = zn(v);
    return v;
}

static inline uint16_t add(haltz_vm_t *vm, uint16_t a, uint16_t b, unsigned cin) {
    uint32_t r = (uint32_t)a + b + cin;
    uint16_t v = (uint16_t)r;
    vm->flags = (uint8_t)(zn(v) | (r > 0xFFFFu ? F_C : 0u) |
                          (((a ^ v) & (b ^ v) & 0x8000u) ? F_V : 0u));
    return v;
}

/* C = borrow: set when a < b + bin as unsigned numbers. */
static inline uint16_t sub(haltz_vm_t *vm, uint16_t a, uint16_t b, unsigned bin) {
    uint16_t v = (uint16_t)((uint32_t)a - b - bin);
    vm->flags = (uint8_t)(zn(v) | ((uint32_t)a < (uint32_t)b + bin ? F_C : 0u) |
                          (((a ^ b) & (a ^ v) & 0x8000u) ? F_V : 0u));
    return v;
}

static inline uint16_t shl(haltz_vm_t *vm, uint16_t a, unsigned n) {
    unsigned c = n ? (a >> (16u - n)) & 1u : 0u;
    uint16_t v = (uint16_t)(a << n);
    vm->flags = (uint8_t)(zn(v) | (c ? F_C : 0u));
    return v;
}

static inline uint16_t shr(haltz_vm_t *vm, uint16_t a, unsigned n) {
    unsigned c = n ? (a >> (n - 1u)) & 1u : 0u;
    uint16_t v = (uint16_t)(a >> n);
    vm->flags = (uint8_t)(zn(v) | (c ? F_C : 0u));
    return v;
}

static inline uint16_t sar(haltz_vm_t *vm, uint16_t a, unsigned n) {
    /* Spelled out so it doesn't rely on >> of a negative int. */
    uint32_t ext = (a & 0x8000u) ? (0xFFFF0000u | a) : a;
    unsigned c = n ? (ext >> (n - 1u)) & 1u : 0u;
    uint16_t v = (uint16_t)(ext >> n);
    vm->flags = (uint8_t)(zn(v) | (c ? F_C : 0u));
    return v;
}

static inline void push(haltz_vm_t *vm, uint16_t v) {
    vm->sp = (uint16_t)(vm->sp - 2u);
    haltz_vm_wr16(vm, vm->sp, v);
}

static inline uint16_t pop(haltz_vm_t *vm) {
    uint16_t v = haltz_vm_rd16(vm, vm->sp);
    vm->sp = (uint16_t)(vm->sp + 2u);
    return v;
}

static inline int cond(uint8_t op, uint8_t f) {
    int c = (f & F_C) != 0, z = (f & F_Z) != 0, n = (f & F_N) != 0, v = (f & F_V) != 0;
    switch (op) {
    case HALTZ_OP_BEQ: return z;
    case HALTZ_OP_BNE: return !z;
    case HALTZ_OP_BCS: return c;
    case HALTZ_OP_BCC: return !c;
    case HALTZ_OP_BMI: return n;
    case HALTZ_OP_BPL: return !n;
    case HALTZ_OP_BVS: return v;
    case HALTZ_OP_BVC: return !v;
    case HALTZ_OP_BHI: return !c && !z;
    case HALTZ_OP_BLS: return c || z;
    case HALTZ_OP_BLT: return n != v;
    case HALTZ_OP_BGE: return n == v;
    case HALTZ_OP_BGT: return !z && n == v;
    case HALTZ_OP_BLE: return z || n != v;
    default: return 1; /* BRA, BSR */
    }
}

haltz_vm_stop_t haltz_vm_run(haltz_vm_t *vm, uint32_t max_steps) {
    uint8_t *m = vm->mem;
    for (; max_steps > 0; max_steps--) {
        const uint16_t pc = vm->pc;
        const uint8_t op = m[pc];
        const haltz_vm_form_t form = (haltz_vm_form_t)k_form[op];
        const uint8_t b1 = m[(uint16_t)(pc + 1u)];
        const uint8_t b2 = m[(uint16_t)(pc + 2u)];
        const uint8_t b3 = m[(uint16_t)(pc + 3u)];
        const unsigned hi = b1 >> 4, lo = b1 & 0x0Fu;
        uint8_t fault = HALTZ_VM_FAULT_NONE;

        if (form == HALTZ_VM_FORM_NONE || !operands_ok(form, b1)) {
            fault = HALTZ_VM_FAULT_ILLEGAL;
            goto do_fault;
        }
        const uint16_t next = (uint16_t)(pc + k_form_len[form]);
        vm->pc = next;
        uint16_t *rd = &vm->r[hi & 7u];
        const uint16_t rs = vm->r[lo & 7u];
        const uint16_t imm16 = (uint16_t)(b2 | (b3 << 8));
        const int q = (int)((lo ^ 8u)) - 8; /* RQ: signed 4 bits */

        switch (op) {
        /* control */
        case HALTZ_OP_BRK: vm->steps++; return HALTZ_VM_BREAK;
        case HALTZ_OP_NOP: break;
        case HALTZ_OP_HALT: vm->steps++; return HALTZ_VM_HALTED;
        case HALTZ_OP_RET: vm->pc = pop(vm); break;

        /* register to register */
        case HALTZ_OP_MOV: *rd = rs; break;
        case HALTZ_OP_ADD: *rd = add(vm, *rd, rs, 0); break;
        case HALTZ_OP_SUB: *rd = sub(vm, *rd, rs, 0); break;
        case HALTZ_OP_ADC: *rd = add(vm, *rd, rs, (vm->flags & F_C) ? 1u : 0u); break;
        case HALTZ_OP_SBC: *rd = sub(vm, *rd, rs, (vm->flags & F_C) ? 1u : 0u); break;
        case HALTZ_OP_AND: *rd = logic(vm, (uint16_t)(*rd & rs)); break;
        case HALTZ_OP_OR: *rd = logic(vm, (uint16_t)(*rd | rs)); break;
        case HALTZ_OP_XOR: *rd = logic(vm, (uint16_t)(*rd ^ rs)); break;
        case HALTZ_OP_CMP: (void)sub(vm, *rd, rs, 0); break;
        case HALTZ_OP_TST: (void)logic(vm, (uint16_t)(*rd & rs)); break;
        case HALTZ_OP_SHL: *rd = shl(vm, *rd, rs & 15u); break;
        case HALTZ_OP_SHR: *rd = shr(vm, *rd, rs & 15u); break;
        case HALTZ_OP_SAR: *rd = sar(vm, *rd, rs & 15u); break;
        case HALTZ_OP_MUL: {
            uint32_t p = (uint32_t)*rd * rs;
            *rd = (uint16_t)p;
            vm->flags = (uint8_t)(zn(*rd) | (p > 0xFFFFu ? F_C : 0u));
            break;
        }
        case HALTZ_OP_DIVU:
        case HALTZ_OP_MODU:
            if (rs == 0) {
                fault = HALTZ_VM_FAULT_DIV_ZERO;
                goto do_fault;
            }
            *rd = logic(vm, (uint16_t)(op == HALTZ_OP_DIVU ? *rd / rs : *rd % rs));
            break;
        case HALTZ_OP_DIVS:
        case HALTZ_OP_MODS: {
            if (rs == 0) {
                fault = HALTZ_VM_FAULT_DIV_ZERO;
                goto do_fault;
            }
            int32_t a = (int16_t)*rd, b = (int16_t)rs;
            if (a == -32768 && b == -1) { /* the one quotient that doesn't fit */
                *rd = logic(vm, op == HALTZ_OP_DIVS ? 0x8000u : 0u);
                vm->flags |= F_V;
            } else {
                *rd = logic(vm, (uint16_t)(op == HALTZ_OP_DIVS ? a / b : a % b));
            }
            break;
        }

        /* one register */
        case HALTZ_OP_NOT: *rd = logic(vm, (uint16_t)~*rd); break;
        case HALTZ_OP_NEG: *rd = sub(vm, 0, *rd, 0); break;
        case HALTZ_OP_SXB: *rd = (uint16_t)(int16_t)(int8_t)(uint8_t)*rd; break;
        case HALTZ_OP_ZXB: *rd = (uint16_t)(*rd & 0xFFu); break;
        case HALTZ_OP_GETSP: *rd = vm->sp; break;
        case HALTZ_OP_SETSP: vm->sp = *rd; break;

        /* small immediates */
        case HALTZ_OP_ADDQ: *rd = add(vm, *rd, (uint16_t)q, 0); break;
        case HALTZ_OP_CMPQ: (void)sub(vm, *rd, (uint16_t)q, 0); break;
        case HALTZ_OP_SHLQ: *rd = shl(vm, *rd, lo); break;
        case HALTZ_OP_SHRQ: *rd = shr(vm, *rd, lo); break;
        case HALTZ_OP_SARQ: *rd = sar(vm, *rd, lo); break;
        case HALTZ_OP_MOVI8: *rd = b2; break;
        case HALTZ_OP_CMPI8: (void)sub(vm, *rd, b2, 0); break;

        /* 16-bit immediates */
        case HALTZ_OP_MOVI: *rd = imm16; break;
        case HALTZ_OP_ADDI: *rd = add(vm, *rd, imm16, 0); break;
        case HALTZ_OP_CMPI: (void)sub(vm, *rd, imm16, 0); break;
        case HALTZ_OP_ANDI: *rd = logic(vm, (uint16_t)(*rd & imm16)); break;
        case HALTZ_OP_ORI: *rd = logic(vm, (uint16_t)(*rd | imm16)); break;
        case HALTZ_OP_XORI: *rd = logic(vm, (uint16_t)(*rd ^ imm16)); break;

        /* memory: base + signed offset, or absolute */
        case HALTZ_OP_LD: *rd = haltz_vm_rd16(vm, (uint16_t)(rs + (int8_t)b2)); break;
        case HALTZ_OP_LDB: *rd = haltz_vm_rd8(vm, (uint16_t)(rs + (int8_t)b2)); break;
        case HALTZ_OP_ST: haltz_vm_wr16(vm, (uint16_t)(rs + (int8_t)b2), *rd); break;
        case HALTZ_OP_STB: haltz_vm_wr8(vm, (uint16_t)(rs + (int8_t)b2), (uint8_t)*rd); break;
        case HALTZ_OP_LDA: *rd = haltz_vm_rd16(vm, imm16); break;
        case HALTZ_OP_LDBA: *rd = haltz_vm_rd8(vm, imm16); break;
        case HALTZ_OP_STA: haltz_vm_wr16(vm, imm16, *rd); break;
        case HALTZ_OP_STBA: haltz_vm_wr8(vm, imm16, (uint8_t)*rd); break;

        /* branches */
        case HALTZ_OP_BSR: push(vm, next); /* fall through */
        case HALTZ_OP_BRA:
        case HALTZ_OP_BEQ:
        case HALTZ_OP_BNE:
        case HALTZ_OP_BCS:
        case HALTZ_OP_BCC:
        case HALTZ_OP_BMI:
        case HALTZ_OP_BPL:
        case HALTZ_OP_BVS:
        case HALTZ_OP_BVC:
        case HALTZ_OP_BHI:
        case HALTZ_OP_BLS:
        case HALTZ_OP_BLT:
        case HALTZ_OP_BGE:
        case HALTZ_OP_BGT:
        case HALTZ_OP_BLE:
            if (cond(op, vm->flags)) {
                vm->pc = (uint16_t)(next + (int8_t)b1);
            }
            break;

        /* jumps, calls, stack, system */
        case HALTZ_OP_CALL: push(vm, next); /* fall through */
        case HALTZ_OP_JMP: vm->pc = (uint16_t)(b1 | (b2 << 8)); break;
        case HALTZ_OP_CALLR: push(vm, next); /* fall through */
        case HALTZ_OP_JMPR: vm->pc = *rd; break;
        case HALTZ_OP_DBNZ:
            *rd = (uint16_t)(*rd - 1u);
            if (*rd != 0) {
                vm->pc = (uint16_t)(next + (int8_t)b2);
            }
            break;
        case HALTZ_OP_PUSHM:
            for (int i = 7; i >= 0; i--) {
                if (b1 & (1u << i)) {
                    push(vm, vm->r[i]);
                }
            }
            break;
        case HALTZ_OP_POPM:
            for (int i = 0; i < 8; i++) {
                if (b1 & (1u << i)) {
                    vm->r[i] = pop(vm);
                }
            }
            break;
        case HALTZ_OP_SYS: {
            if (vm->sys == NULL || haltz_vm_sys_info(b1) == NULL) {
                fault = HALTZ_VM_FAULT_BAD_SYSCALL;
                goto do_fault;
            }
            haltz_vm_sys_result_t res = vm->sys(vm, b1, vm->user);
            switch (res) {
            case HALTZ_VM_SYS_OK: break;
            case HALTZ_VM_SYS_YIELD: vm->steps++; return HALTZ_VM_YIELD;
            case HALTZ_VM_SYS_EXIT: vm->steps++; return HALTZ_VM_EXITED;
            case HALTZ_VM_SYS_UNKNOWN: fault = HALTZ_VM_FAULT_BAD_SYSCALL; goto do_fault;
            default: fault = HALTZ_VM_FAULT_SYSCALL; goto do_fault;
            }
            break;
        }
        default: /* in the form table but not handled here: a bug, treat as illegal */
            fault = HALTZ_VM_FAULT_ILLEGAL;
            goto do_fault;
        }
        vm->steps++;
        continue;

    do_fault:
        vm->pc = pc;
        vm->fault = fault;
        return HALTZ_VM_FAULT;
    }
    return HALTZ_VM_BUDGET;
}

/* ---- disassembler ------------------------------------------------------------------------ */

unsigned haltz_vm_disasm(const uint8_t *mem, uint16_t addr, char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return 1;
    }
    const uint8_t op = mem[addr];
    const haltz_vm_form_t form = (haltz_vm_form_t)k_form[op];
    const uint8_t b1 = mem[(uint16_t)(addr + 1u)];
    const uint8_t b2 = mem[(uint16_t)(addr + 2u)];
    const uint8_t b3 = mem[(uint16_t)(addr + 3u)];
    const unsigned hi = b1 >> 4, lo = b1 & 0x0Fu;

    if (form == HALTZ_VM_FORM_NONE || !operands_ok(form, b1)) {
        snprintf(out, out_size, ".byte 0x%02X", op);
        return 1;
    }
    const unsigned len = k_form_len[form];
    const uint16_t next = (uint16_t)(addr + len);

    char mn[8];
    const char *name = haltz_vm_op_name(op);
    size_t i = 0;
    for (; name[i] != '\0' && i < sizeof mn - 1; i++) {
        char c = name[i];
        mn[i] = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    mn[i] = '\0';

    switch (form) {
    case HALTZ_VM_FORM_N: snprintf(out, out_size, "%s", mn); break;
    case HALTZ_VM_FORM_R: snprintf(out, out_size, "%s r%u", mn, hi); break;
    case HALTZ_VM_FORM_RR: snprintf(out, out_size, "%s r%u, r%u", mn, hi, lo); break;
    case HALTZ_VM_FORM_RQ: snprintf(out, out_size, "%s r%u, %d", mn, hi, (int)(lo ^ 8u) - 8); break;
    case HALTZ_VM_FORM_RS: snprintf(out, out_size, "%s r%u, %u", mn, hi, lo); break;
    case HALTZ_VM_FORM_RI8: snprintf(out, out_size, "%s r%u, %u", mn, hi, b2); break;
    case HALTZ_VM_FORM_RI16:
        snprintf(out, out_size, "%s r%u, 0x%04X", mn, hi, (unsigned)(b2 | (b3 << 8)));
        break;
    case HALTZ_VM_FORM_RRO: {
        int off = (int8_t)b2;
        if (off == 0) {
            snprintf(out, out_size, "%s r%u, [r%u]", mn, hi, lo);
        } else {
            snprintf(out, out_size, "%s r%u, [r%u%+d]", mn, hi, lo, off);
        }
        break;
    }
    case HALTZ_VM_FORM_RA:
        snprintf(out, out_size, "%s r%u, [0x%04X]", mn, hi, (unsigned)(b2 | (b3 << 8)));
        break;
    case HALTZ_VM_FORM_B8:
        snprintf(out, out_size, "%s 0x%04X", mn, (unsigned)(uint16_t)(next + (int8_t)b1));
        break;
    case HALTZ_VM_FORM_RB8:
        snprintf(out, out_size, "%s r%u, 0x%04X", mn, hi,
                 (unsigned)(uint16_t)(next + (int8_t)b2));
        break;
    case HALTZ_VM_FORM_A16:
        snprintf(out, out_size, "%s 0x%04X", mn, (unsigned)(b1 | (b2 << 8)));
        break;
    case HALTZ_VM_FORM_U8:
        if (op == HALTZ_OP_SYS) {
            const haltz_vm_sys_info_t *s = haltz_vm_sys_info(b1);
            if (s != NULL) {
                snprintf(out, out_size, "%s %s", mn, s->name);
            } else {
                snprintf(out, out_size, "%s 0x%02X", mn, b1);
            }
        } else if ((op == HALTZ_OP_PUSHM || op == HALTZ_OP_POPM) && b1 != 0) {
            char list[40]; /* "r0, r1, ... r7" is 30 characters */
            size_t n = 0;
            for (unsigned r = 0; r < 8; r++) {
                if (b1 & (1u << r)) {
                    n += (size_t)snprintf(list + n, sizeof list - n, "%sr%u", n ? ", " : "", r);
                }
            }
            snprintf(out, out_size, "%s %s", mn, list);
        } else {
            snprintf(out, out_size, "%s 0x%02X", mn, b1);
        }
        break;
    default: snprintf(out, out_size, ".byte 0x%02X", op); return 1;
    }
    return len;
}
