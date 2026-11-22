/*
 * hzrun: runs a HALTZ program image (.hzp) on the PC (docs/vm-v1.md section 10).
 *
 *   hzrun [options] program.hzp
 *     -d        disassemble the body and exit
 *     -s        print every instruction as it runs (pc, instruction, registers) on stderr
 *     -t        print every system call on stderr
 *     -n N      stop after N instructions (default 100000000)
 *     -k TEXT   keys for KEY_GET, one per call ("\n" in TEXT is Enter)
 *
 * The console calls (PUTC, PRINT, PRINT_NUM) write to stdout. Time is simulated (5 million
 * instructions per second, FRAME adds 16 ms), so runs are repeatable. Video, sound, LED and
 * the other calls answer OK and do nothing (-t shows them). The save area is 96 bytes in RAM.
 *
 * Exit status: the program's EXIT code (low 8 bits); 0 after HALT; 2 for BRK; 3 for a fault;
 * 4 when the instruction limit is reached; 1 when the file can't be loaded.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "haltz/cart.h"
#include "haltz/vm.h"

typedef struct {
    int trace_sys;
    const char *keys;
    size_t key_pos;
    uint32_t frames;
    uint32_t sleep_ms;
    uint32_t rng;
    uint8_t cart_type;
    uint8_t save[HALTZ_CART_SAVE_LEN];
} host_t;

static uint8_t g_mem[HALTZ_VM_MEM_SIZE];

/* HID usage for a typed character (keyboard page 0x07, US layout), 0xFF if there's none. */
static uint8_t usage_for(char c) {
    if (c >= 'a' && c <= 'z') return (uint8_t)(0x04 + (c - 'a'));
    if (c >= 'A' && c <= 'Z') return (uint8_t)(0x04 + (c - 'A'));
    if (c >= '1' && c <= '9') return (uint8_t)(0x1E + (c - '1'));
    if (c == '0') return 0x27;
    if (c == '\n') return 0x28;
    if (c == 27) return 0x29;
    if (c == '\b') return 0x2A;
    if (c == '\t') return 0x2B;
    if (c == ' ') return 0x2C;
    return 0xFF;
}

static uint32_t time_ms(const haltz_vm_t *vm, const host_t *h) {
    return vm->steps / 5000u + h->frames * 16u + h->sleep_ms;
}

static haltz_vm_sys_result_t host_sys(haltz_vm_t *vm, uint8_t num, void *user) {
    host_t *h = user;
    const haltz_vm_sys_info_t *info = haltz_vm_sys_info(num);
    uint16_t *r = vm->r;
    if (h->trace_sys) {
        fprintf(stderr, "[sys %-12s", info->name);
        for (unsigned i = 0; i < info->args; i++) {
            fprintf(stderr, " r%u=0x%04X", i, r[i]);
        }
        fprintf(stderr, "]\n");
    }
    switch (num) {
    case HALTZ_SYS_EXIT: return HALTZ_VM_SYS_EXIT;
    case HALTZ_SYS_FRAME:
        h->frames++;
        r[0] = (uint16_t)h->frames;
        return HALTZ_VM_SYS_YIELD;
    case HALTZ_SYS_TIME_MS: {
        uint32_t t = time_ms(vm, h);
        r[0] = (uint16_t)t;
        r[1] = (uint16_t)(t >> 16);
        return HALTZ_VM_SYS_OK;
    }
    case HALTZ_SYS_RANDOM:
        h->rng ^= h->rng << 13;
        h->rng ^= h->rng >> 17;
        h->rng ^= h->rng << 5;
        r[0] = (uint16_t)(h->rng >> 16);
        return HALTZ_VM_SYS_OK;
    case HALTZ_SYS_SLEEP_MS: h->sleep_ms += r[0]; return HALTZ_VM_SYS_YIELD;
    case HALTZ_SYS_PUTC: putchar(r[0] & 0xFF); return HALTZ_VM_SYS_OK;
    case HALTZ_SYS_PRINT:
        for (uint32_t a = r[0], n = 0; a < HALTZ_VM_MEM_SIZE && n < HALTZ_VM_PRINT_MAX; a++, n++) {
            uint8_t c = vm->mem[a];
            if (c == 0) {
                break;
            }
            putchar(c);
        }
        return HALTZ_VM_SYS_OK;
    case HALTZ_SYS_PRINT_NUM:
        if (r[1] == 1) {
            printf("%d", (int)(int16_t)r[0]);
        } else if (r[1] == 2) {
            printf("%04X", r[0]);
        } else {
            printf("%u", r[0]);
        }
        return HALTZ_VM_SYS_OK;
    case HALTZ_SYS_KEY_GET:
        if (h->keys != NULL && h->keys[h->key_pos] != '\0') {
            char c = h->keys[h->key_pos++];
            r[0] = (uint8_t)c;
            r[1] = usage_for(c);
        } else {
            r[0] = 0;
            r[1] = 0;
        }
        r[2] = 0;
        return HALTZ_VM_SYS_OK;
    case HALTZ_SYS_STICK: r[0] = r[1] = r[2] = 0; return HALTZ_VM_SYS_OK;
    case HALTZ_SYS_POINTER:
        r[0] = 160;
        r[1] = 120;
        r[2] = 0;
        return HALTZ_VM_SYS_OK;
    case HALTZ_SYS_KNOB: r[0] = 128; return HALTZ_VM_SYS_OK;
    case HALTZ_SYS_KEY_HELD: r[0] = 0; return HALTZ_VM_SYS_OK;
    case HALTZ_SYS_SAVE_READ:
    case HALTZ_SYS_SAVE_WRITE: {
        uint16_t off = r[0], addr = r[1], n = r[2];
        if ((uint32_t)off + n > HALTZ_CART_SAVE_LEN || !haltz_vm_buf_ok(addr, n)) {
            r[0] = HALTZ_ERR_BAD_VALUE;
            return HALTZ_VM_SYS_OK;
        }
        if (num == HALTZ_SYS_SAVE_READ) {
            memcpy(&vm->mem[addr], &h->save[off], n);
        } else {
            memcpy(&h->save[off], &vm->mem[addr], n);
        }
        r[0] = HALTZ_OK;
        return HALTZ_VM_SYS_OK;
    }
    case HALTZ_SYS_CART_INFO:
        r[0] = h->cart_type;
        r[1] = HALTZ_CART_SAVE_LEN;
        return HALTZ_VM_SYS_OK;
    case HALTZ_SYS_CLS:
    case HALTZ_SYS_LOCATE:
    case HALTZ_SYS_COLOR: return HALTZ_VM_SYS_OK;
    default:
        /* video, sound, LED: accepted, nothing happens on the PC */
        if (info->results > 0) {
            r[0] = HALTZ_OK;
        }
        return HALTZ_VM_SYS_OK;
    }
}

static void print_regs(const haltz_vm_t *vm, FILE *f) {
    for (unsigned i = 0; i < HALTZ_VM_NREGS; i++) {
        fprintf(f, "r%u=%04X ", i, vm->r[i]);
    }
    fprintf(f, "sp=%04X %c%c%c%c", vm->sp, (vm->flags & HALTZ_VM_FLAG_N) ? 'N' : '-',
            (vm->flags & HALTZ_VM_FLAG_Z) ? 'Z' : '-', (vm->flags & HALTZ_VM_FLAG_C) ? 'C' : '-',
            (vm->flags & HALTZ_VM_FLAG_V) ? 'V' : '-');
}

static int load_file(const char *path, uint8_t **data, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    size_t cap = 4096, n = 0;
    uint8_t *buf = malloc(cap);
    for (size_t got; buf != NULL && (got = fread(buf + n, 1, cap - n, f)) > 0;) {
        n += got;
        if (n == cap) {
            cap *= 2;
            uint8_t *nb = realloc(buf, cap);
            if (nb == NULL) {
                free(buf);
                buf = NULL;
            }
            buf = nb;
        }
    }
    fclose(f);
    if (buf == NULL) {
        return -1;
    }
    *data = buf;
    *len = n;
    return 0;
}

static void usage(void) {
    fprintf(stderr, "usage: hzrun [-d] [-s] [-t] [-n MAX] [-k KEYS] program.hzp\n");
}

int main(int argc, char **argv) {
    int disasm = 0, step_trace = 0;
    unsigned long long max = 100000000ull;
    host_t host = {.rng = 0x2545F491u};
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            disasm = 1;
        } else if (strcmp(argv[i], "-s") == 0) {
            step_trace = 1;
        } else if (strcmp(argv[i], "-t") == 0) {
            host.trace_sys = 1;
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            max = strtoull(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
            host.keys = argv[++i];
        } else if (argv[i][0] == '-' || path != NULL) {
            usage();
            return 1;
        } else {
            path = argv[i];
        }
    }
    if (path == NULL) {
        usage();
        return 1;
    }

    uint8_t *file;
    size_t file_len;
    if (load_file(path, &file, &file_len) != 0) {
        fprintf(stderr, "hzrun: can't read %s\n", path);
        return 1;
    }
    haltz_cart_header_t hdr;
    haltz_err_t err = file_len < HALTZ_CART_HEADER_LEN
                          ? HALTZ_ERR_BAD_LEN
                          : haltz_cart_check(file, file + HALTZ_CART_HEADER_LEN,
                                             file_len - HALTZ_CART_HEADER_LEN, &hdr);
    if (err != HALTZ_OK) {
        fprintf(stderr, "hzrun: %s is not a valid HALTZ image (error %d)\n", path, (int)err);
        return 1;
    }
    if (hdr.type == HALTZ_CART_DATA) {
        fprintf(stderr, "hzrun: %s is a DATA image, nothing to run\n", path);
        return 1;
    }
    const uint8_t *body = file + HALTZ_CART_HEADER_LEN;
    host.cart_type = hdr.type;

    haltz_vm_t vm;
    haltz_vm_init(&vm, g_mem, host_sys, &host);
    haltz_vm_load(&vm, body, hdr.length);

    if (disasm) {
        printf("; %s: \"%s\", %u bytes\n", path, hdr.title, hdr.length);
        char line[64];
        for (uint32_t a = 0; a < hdr.length;) {
            unsigned n = haltz_vm_disasm(g_mem, (uint16_t)a, line, sizeof line);
            printf("%04X  ", a);
            for (unsigned i = 0; i < 4; i++) {
                if (i < n) {
                    printf("%02X ", g_mem[(uint16_t)(a + i)]);
                } else {
                    printf("   ");
                }
            }
            printf(" %s\n", line);
            a += n;
        }
        free(file);
        return 0;
    }

    haltz_vm_stop_t stop = HALTZ_VM_BUDGET;
    unsigned long long done = 0;
    while (done < max) {
        uint32_t slice = (uint32_t)((max - done) < HALTZ_VM_SLICE ? (max - done) : HALTZ_VM_SLICE);
        if (step_trace) {
            char line[64];
            haltz_vm_disasm(g_mem, vm.pc, line, sizeof line);
            fprintf(stderr, "%04X  %-22s ", vm.pc, line);
            print_regs(&vm, stderr);
            fprintf(stderr, "\n");
            slice = 1;
        }
        uint32_t before = vm.steps;
        stop = haltz_vm_run(&vm, slice);
        done += (uint32_t)(vm.steps - before);
        if (stop != HALTZ_VM_BUDGET && stop != HALTZ_VM_YIELD) {
            break;
        }
        stop = HALTZ_VM_BUDGET;
    }
    fflush(stdout);

    int status;
    switch (stop) {
    case HALTZ_VM_EXITED:
        status = vm.r[0] & 0xFF;
        fprintf(stderr, "hzrun: EXIT %u after %u instructions\n", vm.r[0], vm.steps);
        break;
    case HALTZ_VM_HALTED:
        status = 0;
        fprintf(stderr, "hzrun: HALT at %04X after %u instructions\n",
                (uint16_t)(vm.pc - 1u), vm.steps);
        break;
    case HALTZ_VM_BREAK:
        status = 2;
        fprintf(stderr, "hzrun: BRK at %04X\n", (uint16_t)(vm.pc - 1u));
        break;
    case HALTZ_VM_FAULT: {
        status = 3;
        char line[64];
        haltz_vm_disasm(g_mem, vm.pc, line, sizeof line);
        fprintf(stderr, "hzrun: FAULT %s at %04X (%s)\n", haltz_vm_fault_name(vm.fault), vm.pc,
                line);
        break;
    }
    default:
        status = 4;
        fprintf(stderr, "hzrun: stopped after %llu instructions (limit), pc %04X\n", done,
                vm.pc);
        break;
    }
    if (stop != HALTZ_VM_EXITED && stop != HALTZ_VM_HALTED) {
        fprintf(stderr, "       ");
        print_regs(&vm, stderr);
        fprintf(stderr, "\n");
    }
    free(file);
    return status;
}
