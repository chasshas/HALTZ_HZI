/*
 * HALTZ VM: a 16-bit virtual CPU (docs/vm-v1.md). Portable C, no platform headers: the S3 runs
 * it on core 1, the PC tests and tools/hzrun run it on the desk.
 *
 * The caller owns the 64 KB of VM memory (on the S3: internal RAM) and implements the system
 * calls (one callback). haltz_vm_run() executes up to a given number of instructions and returns
 * why it stopped; between calls the machine is idle and its registers and memory may be read and
 * changed freely (that is the monitor).
 */
#ifndef HALTZ_VM_H
#define HALTZ_VM_H

#include <stddef.h>
#include <stdint.h>

#include "haltz/proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HALTZ_VM_MEM_SIZE 65536u
#define HALTZ_VM_NREGS    8u
#define HALTZ_VM_SLICE    1000u /* instructions per haltz_vm_run() call in the kernel loop */
#define HALTZ_VM_PRINT_MAX 1024u /* PRINT stops after this many characters */

/* Flag bits in haltz_vm_t.flags. */
#define HALTZ_VM_FLAG_C 0x01u /* carry out of ADD, borrow out of SUB/CMP, last bit shifted out */
#define HALTZ_VM_FLAG_Z 0x02u /* result is zero */
#define HALTZ_VM_FLAG_N 0x04u /* result bit 15 */
#define HALTZ_VM_FLAG_V 0x08u /* signed overflow */

/* Operand layouts (vm_opcodes.def explains each). */
typedef enum {
    HALTZ_VM_FORM_NONE = 0, /* not an opcode */
    HALTZ_VM_FORM_N,
    HALTZ_VM_FORM_R,
    HALTZ_VM_FORM_RR,
    HALTZ_VM_FORM_RQ,
    HALTZ_VM_FORM_RS,
    HALTZ_VM_FORM_RI8,
    HALTZ_VM_FORM_RI16,
    HALTZ_VM_FORM_RRO,
    HALTZ_VM_FORM_RA,
    HALTZ_VM_FORM_B8,
    HALTZ_VM_FORM_RB8,
    HALTZ_VM_FORM_A16,
    HALTZ_VM_FORM_U8,
} haltz_vm_form_t;

typedef enum {
#define X(name, op, form) HALTZ_OP_##name = (op),
#include "haltz/vm_opcodes.def"
#undef X
} haltz_vm_op_t;

typedef enum {
#define X(name, num, args, results) HALTZ_SYS_##name = (num),
#include "haltz/vm_syscalls.def"
#undef X
} haltz_vm_sys_t;

/* Why haltz_vm_run() returned. */
typedef enum {
    HALTZ_VM_BUDGET = 0, /* ran the instructions it was given; still runnable */
    HALTZ_VM_YIELD = 1,  /* a system call asked to wait (FRAME, SLEEP_MS, ...); pc is after SYS */
    HALTZ_VM_BREAK = 2,  /* BRK; pc is after it, so resuming continues */
    HALTZ_VM_HALTED = 3, /* HALT: the program stopped itself; pc is after it */
    HALTZ_VM_EXITED = 4, /* SYS EXIT: back to the launcher; exit code in r[0] */
    HALTZ_VM_FAULT = 5,  /* see haltz_vm_t.fault; pc is at the faulting instruction */
} haltz_vm_stop_t;

/* Fault codes. Shown by the monitor, so never renumber, only append. */
typedef enum {
    HALTZ_VM_FAULT_NONE = 0,
    HALTZ_VM_FAULT_ILLEGAL = 1,     /* undefined opcode, register 8-15, reserved bits set */
    HALTZ_VM_FAULT_DIV_ZERO = 2,    /* DIVU / MODU / DIVS / MODS by 0 */
    HALTZ_VM_FAULT_BAD_SYSCALL = 3, /* SYS with a number the system doesn't have */
    HALTZ_VM_FAULT_SYSCALL = 4,     /* the system call handler refused the call */
} haltz_vm_fault_t;

/* What the system call handler returns. */
typedef enum {
    HALTZ_VM_SYS_OK = 0,      /* done, continue */
    HALTZ_VM_SYS_YIELD = 1,   /* done, but stop the run loop: the caller has to wait first */
    HALTZ_VM_SYS_EXIT = 2,    /* the program ends (SYS EXIT) */
    HALTZ_VM_SYS_UNKNOWN = 3, /* no such call: fault BAD_SYSCALL */
    HALTZ_VM_SYS_FAULT = 4,   /* the call can't be done: fault SYSCALL */
} haltz_vm_sys_result_t;

typedef struct haltz_vm haltz_vm_t;

/* System call handler. num is the SYS operand; arguments are in vm->r[0...], results go there
 * too. Runs with pc already past the SYS instruction. */
typedef haltz_vm_sys_result_t (*haltz_vm_sys_fn)(haltz_vm_t *vm, uint8_t num, void *user);

struct haltz_vm {
    uint16_t r[HALTZ_VM_NREGS];
    uint16_t pc;
    uint16_t sp;
    uint8_t flags;     /* HALTZ_VM_FLAG_* */
    uint8_t fault;     /* haltz_vm_fault_t of the last FAULT, else NONE */
    uint32_t steps;    /* instructions executed since reset (wraps) */
    uint8_t *mem;      /* HALTZ_VM_MEM_SIZE bytes, owned by the caller */
    haltz_vm_sys_fn sys; /* may be NULL: every SYS then faults BAD_SYSCALL */
    void *user;
};

/* Static facts about one system call (from vm_syscalls.def). */
typedef struct {
    const char *name;
    uint8_t num;
    uint8_t args;
    uint8_t results;
} haltz_vm_sys_info_t;

/* Sets up vm around mem (not cleared) and resets it. */
void haltz_vm_init(haltz_vm_t *vm, uint8_t *mem, haltz_vm_sys_fn sys, void *user);

/* Registers, pc, sp, flags, fault and steps to 0. Memory is not touched. */
void haltz_vm_reset(haltz_vm_t *vm);

/* Clears all memory, copies the program to address 0 and resets. len <= HALTZ_VM_MEM_SIZE. */
haltz_err_t haltz_vm_load(haltz_vm_t *vm, const uint8_t *prog, size_t len);

/* Executes up to max_steps instructions (0 = none, returns BUDGET at once). */
haltz_vm_stop_t haltz_vm_run(haltz_vm_t *vm, uint32_t max_steps);

/* Memory access for system calls and the monitor. Addresses wrap at 64 KB, words are
 * little-endian and may be unaligned. */
static inline uint8_t haltz_vm_rd8(const haltz_vm_t *vm, uint16_t a) { return vm->mem[a]; }
static inline void haltz_vm_wr8(haltz_vm_t *vm, uint16_t a, uint8_t v) { vm->mem[a] = v; }
static inline uint16_t haltz_vm_rd16(const haltz_vm_t *vm, uint16_t a) {
    return (uint16_t)(vm->mem[a] | (vm->mem[(uint16_t)(a + 1u)] << 8));
}
static inline void haltz_vm_wr16(haltz_vm_t *vm, uint16_t a, uint16_t v) {
    vm->mem[a] = (uint8_t)v;
    vm->mem[(uint16_t)(a + 1u)] = (uint8_t)(v >> 8);
}

/* True if the buffer [addr, addr + len) lies inside memory without wrapping. A system call
 * given a buffer that wraps answers HALTZ_ERR_BAD_VALUE. */
static inline int haltz_vm_buf_ok(uint16_t addr, uint32_t len) {
    return (uint32_t)addr + len <= HALTZ_VM_MEM_SIZE;
}

/* Instruction facts. form/len of an undefined opcode: NONE / 0. */
haltz_vm_form_t haltz_vm_op_form(uint8_t opcode);
unsigned haltz_vm_op_len(uint8_t opcode);
const char *haltz_vm_op_name(uint8_t opcode); /* NAME from the .def ("ADD"), NULL if undefined */
const char *haltz_vm_fault_name(uint8_t fault);
const char *haltz_vm_stop_name(haltz_vm_stop_t stop);

/* NULL if the number is not a system call. */
const haltz_vm_sys_info_t *haltz_vm_sys_info(uint8_t num);

/* Disassembles the instruction at addr into out (always terminated), in the syntax
 * tools/hzasm.py reads. Returns its length in bytes (1 for a byte that isn't a valid
 * instruction, shown as ".byte 0xNN"). */
unsigned haltz_vm_disasm(const uint8_t *mem, uint16_t addr, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* HALTZ_VM_H */
