/* VM, CRC-16 and cartridge tests. Included by test_main.c (shares CHECK and the counters). */

static uint8_t g_vm_mem[HALTZ_VM_MEM_SIZE];

/* Register byte of the R/RR/RRO/... forms. */
#define RB(a, b) (uint8_t)(((a) << 4) | (b))
#define LO(v)    (uint8_t)((v) & 0xFF)
#define HI(v)    (uint8_t)(((v) >> 8) & 0xFF)

/* Loads prog and runs up to max instructions. */
static haltz_vm_stop_t vm_exec(haltz_vm_t *vm, const uint8_t *prog, size_t len, uint32_t max,
                               haltz_vm_sys_fn sys, void *user) {
    haltz_vm_init(vm, g_vm_mem, sys, user);
    CHECK(haltz_vm_load(vm, prog, len) == HALTZ_OK);
    return haltz_vm_run(vm, max);
}

static void test_crc16_vector(void) {
    const uint8_t check[] = "123456789";
    CHECK(haltz_crc16(check, 9) == HALTZ_CRC16_CHECK);
    CHECK(haltz_crc16(check, 9) == 0x29B1);
    CHECK(haltz_crc16(NULL, 0) == 0xFFFF);
    uint16_t crc = HALTZ_CRC16_INIT;
    for (int i = 0; i < 9; i++) {
        crc = haltz_crc16_update(crc, &check[i], 1);
    }
    CHECK(crc == 0x29B1);
}

static void test_vm_tables(void) {
    static const unsigned form_len[] = {0, 1, 2, 2, 2, 2, 3, 4, 3, 4, 2, 3, 3, 2};
    int opcodes = 0;
    for (unsigned op = 0; op < 256; op++) {
        haltz_vm_form_t f = haltz_vm_op_form((uint8_t)op);
        CHECK(haltz_vm_op_len((uint8_t)op) == form_len[f]);
        CHECK((f == HALTZ_VM_FORM_NONE) == (haltz_vm_op_name((uint8_t)op) == NULL));
        opcodes += f != HALTZ_VM_FORM_NONE;
    }
    CHECK(opcodes == 73); /* update when vm_opcodes.def grows */
    CHECK(HALTZ_OP_BRK == 0x00); /* empty memory traps */
    CHECK(haltz_vm_op_len(HALTZ_OP_MOVI) == 4 && haltz_vm_op_len(HALTZ_OP_SYS) == 2);
    CHECK(strcmp(haltz_vm_op_name(HALTZ_OP_DBNZ), "DBNZ") == 0);
    CHECK(haltz_vm_op_name(0x04) == NULL && haltz_vm_op_name(0xFF) == NULL);

    int calls = 0;
    for (unsigned n = 0; n < 256; n++) {
        const haltz_vm_sys_info_t *s = haltz_vm_sys_info((uint8_t)n);
        if (s != NULL) {
            calls++;
            CHECK(s->num == n && s->args <= 6 && s->results <= 3 && s->name != NULL);
        }
    }
    CHECK(calls == 43); /* update when vm_syscalls.def grows */
    CHECK(strcmp(haltz_vm_sys_info(HALTZ_SYS_PRINT)->name, "PRINT") == 0);
    CHECK(haltz_vm_sys_info(HALTZ_SYS_KEY_GET)->results == 3);
    CHECK(haltz_vm_sys_info(0x07) == NULL);
    /* Video and sound numbers mirror the link messages. */
    CHECK((int)HALTZ_SYS_SPRITE_SET == (int)HALTZ_MSG_GPU_SPRITE_SET);
    CHECK((int)HALTZ_SYS_BMP_COPY == (int)HALTZ_MSG_GPU_BMP_COPY);
    CHECK((int)HALTZ_SYS_TEXT_PUT == (int)HALTZ_MSG_GPU_TEXT_PUT);
    CHECK((int)HALTZ_SYS_GFX_INIT == (int)HALTZ_MSG_GPU_INIT);
    CHECK((int)HALTZ_SYS_LED == (int)HALTZ_MSG_IO_LED);
    CHECK((int)HALTZ_SYS_VOLUME == (int)HALTZ_MSG_IO_VOLUME);
}

/* ---- ALU against a reference written differently (signed and wide arithmetic) ---------- */

typedef struct {
    uint16_t v;
    uint8_t flags;
    int fault;
} alu_ref_t;

static uint8_t ref_zn(uint16_t v) {
    return (uint8_t)((v == 0 ? HALTZ_VM_FLAG_Z : 0) | (v >= 0x8000 ? HALTZ_VM_FLAG_N : 0));
}

static alu_ref_t alu_ref(uint8_t op, uint16_t a, uint16_t b, int cin) {
    alu_ref_t r = {0, 0, 0};
    int32_t sa = (int16_t)a, sb = (int16_t)b;
    int64_t wide, swide;
    unsigned n = b & 15u;
    switch (op) {
    case HALTZ_OP_ADD:
    case HALTZ_OP_ADC:
        if (op == HALTZ_OP_ADD) cin = 0;
        wide = (int64_t)a + b + cin;
        swide = (int64_t)sa + sb + cin;
        r.v = (uint16_t)wide;
        r.flags = ref_zn(r.v) | (wide > 65535 ? HALTZ_VM_FLAG_C : 0) |
                  (swide > 32767 || swide < -32768 ? HALTZ_VM_FLAG_V : 0);
        break;
    case HALTZ_OP_SUB:
    case HALTZ_OP_SBC:
    case HALTZ_OP_CMP:
        if (op != HALTZ_OP_SBC) cin = 0;
        wide = (int64_t)a - b - cin;
        swide = (int64_t)sa - sb - cin;
        r.v = op == HALTZ_OP_CMP ? a : (uint16_t)wide;
        r.flags = ref_zn((uint16_t)wide) | (wide < 0 ? HALTZ_VM_FLAG_C : 0) |
                  (swide > 32767 || swide < -32768 ? HALTZ_VM_FLAG_V : 0);
        break;
    case HALTZ_OP_AND: r.v = a & b; r.flags = ref_zn(r.v); break;
    case HALTZ_OP_OR: r.v = a | b; r.flags = ref_zn(r.v); break;
    case HALTZ_OP_XOR: r.v = a ^ b; r.flags = ref_zn(r.v); break;
    case HALTZ_OP_TST: r.v = a; r.flags = ref_zn(a & b); break;
    case HALTZ_OP_SHL:
        wide = (int64_t)a << n;
        r.v = (uint16_t)wide;
        r.flags = ref_zn(r.v) | (((wide >> 16) & 1) ? HALTZ_VM_FLAG_C : 0);
        break;
    case HALTZ_OP_SHR:
        r.v = (uint16_t)(a >> n);
        r.flags = ref_zn(r.v) | (n && ((a >> (n - 1)) & 1) ? HALTZ_VM_FLAG_C : 0);
        break;
    case HALTZ_OP_SAR: {
        /* floor(sa / 2^n), without >> on a negative number */
        int32_t d = 1 << n, q = sa / d;
        if (sa % d != 0 && sa < 0) q--;
        int32_t d1 = n ? 1 << (n - 1) : 1, q1 = sa / d1;
        if (sa % d1 != 0 && sa < 0) q1--;
        r.v = (uint16_t)q;
        r.flags = ref_zn(r.v) | (n && (q1 & 1) ? HALTZ_VM_FLAG_C : 0);
        break;
    }
    case HALTZ_OP_MUL:
        wide = (int64_t)a * b;
        r.v = (uint16_t)wide;
        r.flags = ref_zn(r.v) | (wide > 65535 ? HALTZ_VM_FLAG_C : 0);
        break;
    case HALTZ_OP_DIVU:
    case HALTZ_OP_MODU:
        if (b == 0) { r.fault = 1; break; }
        r.v = (uint16_t)(op == HALTZ_OP_DIVU ? a / b : a % b);
        r.flags = ref_zn(r.v);
        break;
    case HALTZ_OP_DIVS:
    case HALTZ_OP_MODS:
        if (b == 0) { r.fault = 1; break; }
        if (sa == -32768 && sb == -1) {
            r.v = op == HALTZ_OP_DIVS ? 0x8000 : 0;
            r.flags = ref_zn(r.v) | HALTZ_VM_FLAG_V;
        } else {
            r.v = (uint16_t)(op == HALTZ_OP_DIVS ? sa / sb : sa % sb);
            r.flags = ref_zn(r.v);
        }
        break;
    default: break;
    }
    return r;
}

static const uint16_t k_vals[] = {0,      1,      2,      3,      7,      0x7FFF, 0x8000,
                                  0x8001, 0xFFFF, 0xFFFE, 0x1234, 0xFF00, 0x00FF, 0x5555};
#define NVALS (sizeof k_vals / sizeof k_vals[0])

static void test_vm_alu(void) {
    static const uint8_t ops[] = {HALTZ_OP_ADD, HALTZ_OP_SUB,  HALTZ_OP_ADC,  HALTZ_OP_SBC,
                                  HALTZ_OP_AND, HALTZ_OP_OR,   HALTZ_OP_XOR,  HALTZ_OP_CMP,
                                  HALTZ_OP_TST, HALTZ_OP_SHL,  HALTZ_OP_SHR,  HALTZ_OP_SAR,
                                  HALTZ_OP_MUL, HALTZ_OP_DIVU, HALTZ_OP_MODU, HALTZ_OP_DIVS,
                                  HALTZ_OP_MODS};
    haltz_vm_t vm;
    int bad = 0;
    for (size_t o = 0; o < sizeof ops; o++) {
        for (size_t i = 0; i < NVALS; i++) {
            for (size_t j = 0; j < NVALS; j++) {
                for (int cin = 0; cin < 2; cin++) {
                    uint16_t a = k_vals[i], b = k_vals[j];
                    /* r3 = 0xFFFF + 1 (C=1) or 0 + 0 (C=0) sets the carry going in. */
                    uint16_t c0 = cin ? 0xFFFF : 0, c1 = cin ? 1 : 0;
                    const uint8_t prog[] = {
                        HALTZ_OP_MOVI, RB(1, 0), LO(a),  HI(a),  HALTZ_OP_MOVI, RB(2, 0),
                        LO(b),         HI(b),    HALTZ_OP_MOVI,  RB(3, 0),      LO(c0),
                        HI(c0),        HALTZ_OP_ADDI, RB(3, 0), LO(c1),        HI(c1),
                        ops[o],        RB(1, 2), HALTZ_OP_HALT,
                    };
                    haltz_vm_stop_t st = vm_exec(&vm, prog, sizeof prog, 100, NULL, NULL);
                    alu_ref_t ref = alu_ref(ops[o], a, b, cin);
                    int ok;
                    if (ref.fault) {
                        ok = st == HALTZ_VM_FAULT && vm.fault == HALTZ_VM_FAULT_DIV_ZERO &&
                             vm.pc == 16 && vm.r[1] == a && vm.steps == 4;
                    } else {
                        ok = st == HALTZ_VM_HALTED && vm.r[1] == ref.v && vm.flags == ref.flags &&
                             vm.r[2] == b && vm.steps == 6;
                    }
                    if (!ok && bad++ < 10) {
                        printf("  ALU %s a=%04X b=%04X cin=%d: got %04X flags %X, want %04X "
                               "flags %X\n",
                               haltz_vm_op_name(ops[o]), a, b, cin, vm.r[1], vm.flags, ref.v,
                               ref.flags);
                    }
                    CHECK(ok);
                }
            }
        }
    }

    /* One-register ops. */
    for (size_t i = 0; i < NVALS; i++) {
        uint16_t a = k_vals[i];
        const uint8_t prog[] = {HALTZ_OP_MOVI, RB(1, 0), LO(a),  HI(a),          HALTZ_OP_MOV,
                                RB(2, 1),      HALTZ_OP_MOV,     RB(3, 1),       HALTZ_OP_MOV,
                                RB(4, 1),      HALTZ_OP_NOT,     RB(1, 0),       HALTZ_OP_SXB,
                                RB(3, 0),      HALTZ_OP_ZXB,     RB(4, 0),       HALTZ_OP_NEG,
                                RB(2, 0),      HALTZ_OP_HALT};
        CHECK(vm_exec(&vm, prog, sizeof prog, 100, NULL, NULL) == HALTZ_VM_HALTED);
        CHECK(vm.r[1] == (uint16_t)~a);
        CHECK(vm.r[3] == (uint16_t)(int16_t)(int8_t)(a & 0xFF));
        CHECK(vm.r[4] == (a & 0xFF));
        alu_ref_t neg = alu_ref(HALTZ_OP_SUB, 0, a, 0);
        CHECK(vm.r[2] == neg.v && vm.flags == neg.flags); /* SXB/ZXB leave the flags */
    }
}

/* ---- branches ---------------------------------------------------------------------------- */

static void test_vm_branches(void) {
    static const uint8_t bops[] = {HALTZ_OP_BEQ, HALTZ_OP_BNE, HALTZ_OP_BCS, HALTZ_OP_BCC,
                                   HALTZ_OP_BMI, HALTZ_OP_BPL, HALTZ_OP_BVS, HALTZ_OP_BVC,
                                   HALTZ_OP_BHI, HALTZ_OP_BLS, HALTZ_OP_BLT, HALTZ_OP_BGE,
                                   HALTZ_OP_BGT, HALTZ_OP_BLE, HALTZ_OP_BRA};
    haltz_vm_t vm;
    for (size_t i = 0; i < NVALS; i++) {
        for (size_t j = 0; j < NVALS; j++) {
            uint16_t a = k_vals[i], b = k_vals[j];
            int32_t sa = (int16_t)a, sb = (int16_t)b, sd = sa - sb;
            int n = (uint16_t)(a - b) >= 0x8000, v = sd > 32767 || sd < -32768;
            for (size_t k = 0; k < sizeof bops; k++) {
                int want;
                switch (bops[k]) {
                case HALTZ_OP_BEQ: want = a == b; break;
                case HALTZ_OP_BNE: want = a != b; break;
                case HALTZ_OP_BCS: want = a < b; break;
                case HALTZ_OP_BCC: want = a >= b; break;
                case HALTZ_OP_BMI: want = n; break;
                case HALTZ_OP_BPL: want = !n; break;
                case HALTZ_OP_BVS: want = v; break;
                case HALTZ_OP_BVC: want = !v; break;
                case HALTZ_OP_BHI: want = a > b; break;
                case HALTZ_OP_BLS: want = a <= b; break;
                case HALTZ_OP_BLT: want = sa < sb; break;
                case HALTZ_OP_BGE: want = sa >= sb; break;
                case HALTZ_OP_BGT: want = sa > sb; break;
                case HALTZ_OP_BLE: want = sa <= sb; break;
                default: want = 1; break;
                }
                /* r0 = 1 only if the branch over "movi8 r0, 1" is not taken. */
                const uint8_t prog[] = {HALTZ_OP_MOVI, RB(1, 0), LO(a),         HI(a),
                                        HALTZ_OP_MOVI, RB(2, 0), LO(b),         HI(b),
                                        HALTZ_OP_CMP,  RB(1, 2), bops[k],       3,
                                        HALTZ_OP_MOVI8, RB(0, 0), 1,            HALTZ_OP_HALT};
                CHECK(vm_exec(&vm, prog, sizeof prog, 100, NULL, NULL) == HALTZ_VM_HALTED);
                if ((vm.r[0] == 0) != want) {
                    printf("  %s after CMP %04X, %04X: taken=%d, want %d\n",
                           haltz_vm_op_name(bops[k]), a, b, vm.r[0] == 0, want);
                }
                CHECK((vm.r[0] == 0) == want);
            }
        }
    }
    /* Backward branch: offset is from the next instruction. */
    const uint8_t back[] = {HALTZ_OP_MOVI8, RB(1, 0), 5,   HALTZ_OP_ADDQ, RB(2, 1),
                            HALTZ_OP_ADDQ,  RB(1, 0xF), HALTZ_OP_BNE,  (uint8_t)-6, HALTZ_OP_HALT};
    CHECK(vm_exec(&vm, back, sizeof back, 100, NULL, NULL) == HALTZ_VM_HALTED);
    CHECK(vm.r[1] == 0 && vm.r[2] == 5);
}

/* ---- immediates --------------------------------------------------------------------------- */

static void test_vm_immediates(void) {
    haltz_vm_t vm;
    /* ADDQ over the whole -8..7 range: r1 = 100 + q. */
    for (int q = -8; q <= 7; q++) {
        const uint8_t prog[] = {HALTZ_OP_MOVI8, RB(1, 0), 100, HALTZ_OP_ADDQ,
                                (uint8_t)(0x10 | (q & 15)), HALTZ_OP_HALT};
        CHECK(vm_exec(&vm, prog, sizeof prog, 10, NULL, NULL) == HALTZ_VM_HALTED);
        CHECK(vm.r[1] == (uint16_t)(100 + q));
        /* Same flags as ADD of the 16-bit value: adding a negative number sets C. */
        CHECK(vm.flags == alu_ref(HALTZ_OP_ADD, 100, (uint16_t)q, 0).flags);
    }
    const uint8_t prog[] = {
        HALTZ_OP_MOVI8, RB(1, 0), 0xF0,                 /* r1 = 0x00F0 (zero-extended) */
        HALTZ_OP_MOVI,  RB(2, 0), 0x34, 0x12,           /* r2 = 0x1234 */
        HALTZ_OP_MOV,   RB(3, 2),
        HALTZ_OP_SHLQ,  RB(3, 4),                       /* r3 = 0x2340 */
        HALTZ_OP_MOV,   RB(4, 2),
        HALTZ_OP_SHRQ,  RB(4, 8),                       /* r4 = 0x0012 */
        HALTZ_OP_MOVI,  RB(5, 0), 0x00, 0x80,
        HALTZ_OP_SARQ,  RB(5, 15),                      /* r5 = 0xFFFF */
        HALTZ_OP_MOV,   RB(6, 2),
        HALTZ_OP_ANDI,  RB(6, 0), 0xF0, 0x0F,           /* r6 = 0x0230 */
        HALTZ_OP_ORI,   RB(6, 0), 0x01, 0x00,           /* r6 = 0x0231 */
        HALTZ_OP_XORI,  RB(6, 0), 0xFF, 0xFF,           /* r6 = 0xFDCE, N */
        HALTZ_OP_MOV,   RB(7, 2),
        HALTZ_OP_ADDI,  RB(7, 0), 0xCC, 0xED,           /* r7 = 0x1234 + 0xEDCC = 0x0000, Z C */
        HALTZ_OP_HALT,
    };
    CHECK(vm_exec(&vm, prog, sizeof prog, 100, NULL, NULL) == HALTZ_VM_HALTED);
    CHECK(vm.r[1] == 0x00F0 && vm.r[2] == 0x1234 && vm.r[3] == 0x2340 && vm.r[4] == 0x0012);
    CHECK(vm.r[5] == 0xFFFF && vm.r[6] == 0xFDCE && vm.r[7] == 0);
    CHECK(vm.flags == (HALTZ_VM_FLAG_Z | HALTZ_VM_FLAG_C));

    /* CMPQ / CMPI8 / CMPI compare without changing the register. */
    const uint8_t cmp[] = {HALTZ_OP_MOVI8, RB(1, 0), 'A', HALTZ_OP_CMPI8, RB(1, 0), 'A',
                           HALTZ_OP_BNE,   4,        HALTZ_OP_CMPQ, RB(1, 0xF), /* 'A' vs -1 */
                           HALTZ_OP_BCS,   0,        HALTZ_OP_CMPI, RB(1, 0), 0x41, 0x00,
                           HALTZ_OP_HALT};
    CHECK(vm_exec(&vm, cmp, sizeof cmp, 100, NULL, NULL) == HALTZ_VM_HALTED);
    CHECK(vm.r[1] == 'A' && vm.flags == HALTZ_VM_FLAG_Z);
}

/* ---- memory ------------------------------------------------------------------------------- */

static void test_vm_memory(void) {
    haltz_vm_t vm;
    const uint8_t prog[] = {
        HALTZ_OP_MOVI,  RB(1, 0), 0x00, 0x20,     /* r1 = 0x2000 (base) */
        HALTZ_OP_MOVI,  RB(2, 0), 0xCD, 0xAB,     /* r2 = 0xABCD */
        HALTZ_OP_ST,    RB(2, 1), 4,              /* [0x2004] = CD AB */
        HALTZ_OP_STB,   RB(2, 1), (uint8_t)-1,    /* [0x1FFF] = CD */
        HALTZ_OP_LD,    RB(3, 1), 4,              /* r3 = 0xABCD */
        HALTZ_OP_LDB,   RB(4, 1), 5,              /* r4 = 0x00AB */
        HALTZ_OP_LD,    RB(5, 1), (uint8_t)-1,    /* r5 = [0x1FFF..0x2000] = 0x00CD */
        HALTZ_OP_STA,   RB(2, 0), 0xFF, 0xFF,     /* [0xFFFF] = CD, [0x0000] = AB: wraps */
        HALTZ_OP_LDA,   RB(6, 0), 0xFF, 0xFF,     /* r6 = 0xABCD */
        HALTZ_OP_STBA,  RB(2, 0), 0x00, 0x30,     /* [0x3000] = CD */
        HALTZ_OP_LDBA,  RB(7, 0), 0x00, 0x30,     /* r7 = 0x00CD */
        HALTZ_OP_HALT,
    };
    CHECK(vm_exec(&vm, prog, sizeof prog, 100, NULL, NULL) == HALTZ_VM_HALTED);
    CHECK(g_vm_mem[0x2004] == 0xCD && g_vm_mem[0x2005] == 0xAB && g_vm_mem[0x1FFF] == 0xCD);
    CHECK(vm.r[3] == 0xABCD && vm.r[4] == 0x00AB && vm.r[5] == 0x00CD);
    CHECK(g_vm_mem[0xFFFF] == 0xCD && g_vm_mem[0x0000] == 0xAB); /* overwrote MOVI's opcode */
    CHECK(vm.r[6] == 0xABCD && vm.r[7] == 0x00CD && g_vm_mem[0x3000] == 0xCD);
    CHECK(vm.flags == 0); /* loads and stores leave the flags */

    /* Base + offset wraps at 64 KB too. */
    const uint8_t wrap[] = {HALTZ_OP_MOVI, RB(1, 0), 0xFE, 0xFF, HALTZ_OP_LDB, RB(2, 1), 3,
                            HALTZ_OP_HALT};
    CHECK(vm_exec(&vm, wrap, sizeof wrap, 10, NULL, NULL) == HALTZ_VM_HALTED);
    CHECK(vm.r[2] == wrap[1]); /* 0xFFFE + 3 = 0x0001 */

    /* load() clears memory and rejects too much. */
    memset(g_vm_mem, 0x77, sizeof g_vm_mem);
    haltz_vm_init(&vm, g_vm_mem, NULL, NULL);
    CHECK(haltz_vm_load(&vm, wrap, sizeof wrap) == HALTZ_OK);
    CHECK(g_vm_mem[sizeof wrap] == 0 && g_vm_mem[0xFFFF] == 0);
    CHECK(haltz_vm_load(&vm, g_vm_mem, HALTZ_VM_MEM_SIZE + 1) == HALTZ_ERR_ARG);
    CHECK(haltz_vm_buf_ok(0xFF00, 0x100) && !haltz_vm_buf_ok(0xFF00, 0x101));
}

/* ---- stack and calls ---------------------------------------------------------------------- */

static void test_vm_stack_calls(void) {
    haltz_vm_t vm;
    const uint8_t prog[] = {
        /* 0x00 */ HALTZ_OP_MOVI8, RB(0, 0), 10,
        /* 0x03 */ HALTZ_OP_MOVI8, RB(1, 0), 11,
        /* 0x06 */ HALTZ_OP_MOVI8, RB(4, 0), 44,
        /* 0x09 */ HALTZ_OP_PUSHM, 0x13,              /* pushes r4, r1, r0: sp = 0xFFFA */
        /* 0x0B */ HALTZ_OP_GETSP, RB(5, 0),          /* r5 = 0xFFFA */
        /* 0x0D */ HALTZ_OP_MOVI8, RB(0, 0), 0,
        /* 0x10 */ HALTZ_OP_MOVI8, RB(1, 0), 0,
        /* 0x13 */ HALTZ_OP_MOVI8, RB(4, 0), 0,
        /* 0x16 */ HALTZ_OP_POPM, 0x13,               /* back: r0 10, r1 11, r4 44 */
        /* 0x18 */ HALTZ_OP_CALL, 0x38, 0x00,         /* sub: r2 += 1 */
        /* 0x1B */ HALTZ_OP_BSR, 0x38 - 0x1D,         /* sub again */
        /* 0x1D */ HALTZ_OP_MOVI, RB(3, 0), 0x38, 0x00,
        /* 0x21 */ HALTZ_OP_CALLR, RB(3, 0),          /* third time: r2 = 3 */
        /* 0x23 */ HALTZ_OP_MOVI8, RB(6, 0), 4,
        /* 0x26 */ HALTZ_OP_ADDQ, RB(7, 1),           /* r7 counts DBNZ rounds */
        /* 0x28 */ HALTZ_OP_DBNZ, RB(6, 0), (uint8_t)(0x26 - 0x2B),
        /* 0x2B */ HALTZ_OP_MOVI, RB(3, 0), 0x34, 0x00,
        /* 0x2F */ HALTZ_OP_JMPR, RB(3, 0),           /* to 0x34 */
        /* 0x31 */ 0, 0, 0,                           /* BRK, never reached */
        /* 0x34 */ HALTZ_OP_HALT, 0, 0, 0,
        /* 0x38 */ HALTZ_OP_ADDQ, RB(2, 1),           /* sub */
        /* 0x3A */ HALTZ_OP_RET,
    };
    CHECK(sizeof prog == 0x3B);
    CHECK(vm_exec(&vm, prog, sizeof prog, 200, NULL, NULL) == HALTZ_VM_HALTED);
    CHECK(vm.pc == 0x35);
    CHECK(vm.r[0] == 10 && vm.r[1] == 11 && vm.r[4] == 44);
    CHECK(vm.r[5] == 0xFFFA && vm.sp == 0);
    CHECK(vm.r[2] == 3);
    CHECK(vm.r[6] == 0 && vm.r[7] == 4);
    /* The calls reused the stack words: the last one left CALLR's return address. */
    CHECK(haltz_vm_rd16(&vm, 0xFFFE) == 0x0023);
    /* PUSHM put the highest register first, so r0 is at the lowest address. */
    CHECK(haltz_vm_rd16(&vm, 0xFFFA) == 10 && haltz_vm_rd16(&vm, 0xFFFC) == 11);

    /* SETSP moves the stack; DBNZ from 1 falls through at once. */
    const uint8_t sp[] = {HALTZ_OP_MOVI,  RB(1, 0), 0x00, 0x80, HALTZ_OP_SETSP, RB(1, 0),
                          HALTZ_OP_PUSHM, 0x02,     HALTZ_OP_MOVI8, RB(2, 0), 1,
                          HALTZ_OP_DBNZ,  RB(2, 0), (uint8_t)-128,   HALTZ_OP_HALT};
    CHECK(vm_exec(&vm, sp, sizeof sp, 20, NULL, NULL) == HALTZ_VM_HALTED);
    CHECK(vm.sp == 0x7FFE && haltz_vm_rd16(&vm, 0x7FFE) == 0x8000 && vm.r[2] == 0);
}

/* ---- stops and faults --------------------------------------------------------------------- */

static void test_vm_stops(void) {
    haltz_vm_t vm;
    /* Empty memory is BRK: running off into it traps. */
    const uint8_t nop[] = {HALTZ_OP_NOP};
    CHECK(vm_exec(&vm, nop, sizeof nop, 10, NULL, NULL) == HALTZ_VM_BREAK);
    CHECK(vm.pc == 2 && vm.steps == 2);
    /* Resuming after BRK continues with the next byte (another BRK). */
    CHECK(haltz_vm_run(&vm, 10) == HALTZ_VM_BREAK && vm.pc == 3 && vm.steps == 3);

    /* Budget: an endless loop stops after exactly max instructions. */
    const uint8_t loop[] = {HALTZ_OP_ADDQ, RB(1, 1), HALTZ_OP_BRA, (uint8_t)-4};
    CHECK(vm_exec(&vm, loop, sizeof loop, 1000, NULL, NULL) == HALTZ_VM_BUDGET);
    CHECK(vm.steps == 1000 && vm.r[1] == 500 && vm.pc == 0);
    CHECK(haltz_vm_run(&vm, 0) == HALTZ_VM_BUDGET && vm.steps == 1000);
    CHECK(haltz_vm_run(&vm, 1) == HALTZ_VM_BUDGET && vm.pc == 2 && vm.r[1] == 501);

    /* Illegal encodings: the pc stays on them, nothing changes, no step counted. */
    static const uint8_t bad[][4] = {
        {0x04, 0, 0, 0},                       /* undefined opcode */
        {0xFF, 0, 0, 0},                       /* undefined opcode */
        {HALTZ_OP_MOV, 0x81, 0, 0},            /* rd = 8 */
        {HALTZ_OP_ADD, 0x18, 0, 0},            /* rs = 8 */
        {HALTZ_OP_NOT, 0x11, 0, 0},            /* R form, low nibble set */
        {HALTZ_OP_MOVI, 0x12, 0, 0},           /* RI16, low nibble set */
        {HALTZ_OP_LD, 0x19, 0, 0},             /* base r9 */
        {HALTZ_OP_ADDQ, 0x91, 0, 0},           /* rd = 9 */
        {HALTZ_OP_DBNZ, 0x01, 0, 0},           /* RB8, low nibble set */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        uint8_t prog[6] = {HALTZ_OP_NOP, HALTZ_OP_NOP};
        memcpy(&prog[2], bad[i], 4);
        CHECK(vm_exec(&vm, prog, sizeof prog, 10, NULL, NULL) == HALTZ_VM_FAULT);
        CHECK(vm.fault == HALTZ_VM_FAULT_ILLEGAL && vm.pc == 2 && vm.steps == 2);
    }
    /* reset() clears the fault and the counters. */
    haltz_vm_reset(&vm);
    CHECK(vm.fault == HALTZ_VM_FAULT_NONE && vm.pc == 0 && vm.steps == 0 && vm.sp == 0);

    /* DIVS overflow is not a fault. */
    const uint8_t ovf[] = {HALTZ_OP_MOVI, RB(1, 0), 0x00, 0x80, HALTZ_OP_MOVI, RB(2, 0),
                           0xFF,          0xFF,     HALTZ_OP_DIVS, RB(1, 2), HALTZ_OP_HALT};
    CHECK(vm_exec(&vm, ovf, sizeof ovf, 10, NULL, NULL) == HALTZ_VM_HALTED);
    CHECK(vm.r[1] == 0x8000 && (vm.flags & HALTZ_VM_FLAG_V));

    CHECK(strcmp(haltz_vm_fault_name(HALTZ_VM_FAULT_DIV_ZERO), "DIV_ZERO") == 0);
    CHECK(strcmp(haltz_vm_stop_name(HALTZ_VM_YIELD), "YIELD") == 0);
}

/* ---- system calls ------------------------------------------------------------------------- */

typedef struct {
    int calls;
    uint8_t last_num;
    uint16_t args[6];
    haltz_vm_sys_result_t answer;
} sys_probe_t;

static haltz_vm_sys_result_t sys_probe(haltz_vm_t *vm, uint8_t num, void *user) {
    sys_probe_t *p = user;
    p->calls++;
    p->last_num = num;
    memcpy(p->args, vm->r, sizeof p->args);
    vm->r[0] = 0x4242;
    return p->answer;
}

static void test_vm_syscalls(void) {
    haltz_vm_t vm;
    sys_probe_t p;
    const uint8_t prog[] = {HALTZ_OP_MOVI8, RB(0, 0), 1,  HALTZ_OP_MOVI8, RB(5, 0), 6,
                            HALTZ_OP_SYS,   HALTZ_SYS_SPRITE_SET,           HALTZ_OP_HALT};
    static const struct {
        haltz_vm_sys_result_t answer;
        haltz_vm_stop_t stop;
        uint16_t pc;
        uint8_t fault;
    } cases[] = {
        {HALTZ_VM_SYS_OK, HALTZ_VM_HALTED, 9, HALTZ_VM_FAULT_NONE},
        {HALTZ_VM_SYS_YIELD, HALTZ_VM_YIELD, 8, HALTZ_VM_FAULT_NONE},
        {HALTZ_VM_SYS_EXIT, HALTZ_VM_EXITED, 8, HALTZ_VM_FAULT_NONE},
        {HALTZ_VM_SYS_UNKNOWN, HALTZ_VM_FAULT, 6, HALTZ_VM_FAULT_BAD_SYSCALL},
        {HALTZ_VM_SYS_FAULT, HALTZ_VM_FAULT, 6, HALTZ_VM_FAULT_SYSCALL},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        memset(&p, 0, sizeof p);
        p.answer = cases[i].answer;
        CHECK(vm_exec(&vm, prog, sizeof prog, 10, sys_probe, &p) == cases[i].stop);
        CHECK(vm.pc == cases[i].pc && vm.fault == cases[i].fault);
        CHECK(p.calls == 1 && p.last_num == HALTZ_SYS_SPRITE_SET);
        CHECK(p.args[0] == 1 && p.args[5] == 6 && vm.r[0] == 0x4242);
    }
    /* After a YIELD the program goes on. */
    memset(&p, 0, sizeof p);
    p.answer = HALTZ_VM_SYS_YIELD;
    CHECK(vm_exec(&vm, prog, sizeof prog, 10, sys_probe, &p) == HALTZ_VM_YIELD);
    CHECK(haltz_vm_run(&vm, 10) == HALTZ_VM_HALTED && vm.steps == 4);

    /* A number that isn't in vm_syscalls.def never reaches the handler. */
    const uint8_t undef[] = {HALTZ_OP_SYS, 0x07, HALTZ_OP_HALT};
    memset(&p, 0, sizeof p);
    CHECK(vm_exec(&vm, undef, sizeof undef, 10, sys_probe, &p) == HALTZ_VM_FAULT);
    CHECK(vm.fault == HALTZ_VM_FAULT_BAD_SYSCALL && vm.pc == 0 && p.calls == 0);
    /* No handler at all: every SYS faults. */
    const uint8_t any[] = {HALTZ_OP_SYS, HALTZ_SYS_PUTC};
    CHECK(vm_exec(&vm, any, sizeof any, 10, NULL, NULL) == HALTZ_VM_FAULT);
    CHECK(vm.fault == HALTZ_VM_FAULT_BAD_SYSCALL);
}

/* ---- disassembler ------------------------------------------------------------------------- */

static void test_vm_disasm(void) {
    static const struct {
        uint8_t bytes[4];
        uint16_t at;
        unsigned len;
        const char *text;
    } cases[] = {
        {{HALTZ_OP_BRK}, 0, 1, "brk"},
        {{HALTZ_OP_RET}, 0, 1, "ret"},
        {{HALTZ_OP_ADD, RB(1, 2)}, 0, 2, "add r1, r2"},
        {{HALTZ_OP_NOT, RB(7, 0)}, 0, 2, "not r7"},
        {{HALTZ_OP_ADDQ, RB(3, 0xF)}, 0, 2, "addq r3, -1"},
        {{HALTZ_OP_SHRQ, RB(3, 0xF)}, 0, 2, "shrq r3, 15"},
        {{HALTZ_OP_MOVI8, RB(0, 0), 65}, 0, 3, "movi8 r0, 65"},
        {{HALTZ_OP_MOVI, RB(2, 0), 0x34, 0x12}, 0, 4, "movi r2, 0x1234"},
        {{HALTZ_OP_LD, RB(1, 2), 0}, 0, 3, "ld r1, [r2]"},
        {{HALTZ_OP_ST, RB(1, 2), 4}, 0, 3, "st r1, [r2+4]"},
        {{HALTZ_OP_LDB, RB(1, 2), 0xFC}, 0, 3, "ldb r1, [r2-4]"},
        {{HALTZ_OP_STA, RB(5, 0), 0x00, 0x40}, 0, 4, "sta r5, [0x4000]"},
        {{HALTZ_OP_BNE, 0xFE}, 0x100, 2, "bne 0x0100"},
        {{HALTZ_OP_BRA, 0x10}, 0x100, 2, "bra 0x0112"},
        {{HALTZ_OP_DBNZ, RB(6, 0), 0xFD}, 0x20, 3, "dbnz r6, 0x0020"},
        {{HALTZ_OP_CALL, 0xCD, 0xAB}, 0, 3, "call 0xABCD"},
        {{HALTZ_OP_SYS, HALTZ_SYS_PRINT}, 0, 2, "sys PRINT"},
        {{HALTZ_OP_SYS, 0x07}, 0, 2, "sys 0x07"},
        {{HALTZ_OP_PUSHM, 0x83}, 0, 2, "pushm r0, r1, r7"},
        {{HALTZ_OP_POPM, 0x00}, 0, 2, "popm 0x00"},
        {{0x04}, 0, 1, ".byte 0x04"},
        {{HALTZ_OP_MOV, 0x81}, 0, 1, ".byte 0x10"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        memset(g_vm_mem, 0, sizeof g_vm_mem);
        memcpy(&g_vm_mem[cases[i].at], cases[i].bytes, 4);
        char out[48];
        unsigned n = haltz_vm_disasm(g_vm_mem, cases[i].at, out, sizeof out);
        if (n != cases[i].len || strcmp(out, cases[i].text) != 0) {
            printf("  disasm: got \"%s\" (%u), want \"%s\" (%u)\n", out, n, cases[i].text,
                   cases[i].len);
        }
        CHECK(n == cases[i].len && strcmp(out, cases[i].text) == 0);
    }
    /* A tiny buffer is cut, but terminated. */
    char tiny[4];
    g_vm_mem[0] = HALTZ_OP_MOVI;
    g_vm_mem[1] = RB(2, 0);
    CHECK(haltz_vm_disasm(g_vm_mem, 0, tiny, sizeof tiny) == 4 && strlen(tiny) == 3);
}

/* ---- cartridge ---------------------------------------------------------------------------- */

static void test_cart(void) {
    uint8_t body[HALTZ_CART_BODY_MAX];
    for (size_t i = 0; i < sizeof body; i++) {
        body[i] = (uint8_t)(i * 7 + 3);
    }
    haltz_cart_header_t h = {.type = HALTZ_CART_CODE, .title = "SPACE ROCKS", .length = 300};
    uint8_t hdr[HALTZ_CART_HEADER_LEN];
    CHECK(haltz_cart_header_pack(&h, body, hdr) == HALTZ_OK);
    CHECK(hdr[0] == 'H' && hdr[1] == 'Z' && hdr[2] == HALTZ_CART_FORMAT && hdr[3] == 0);
    CHECK(memcmp(&hdr[4], "SPACE ROCKS\0\0\0\0\0", 16) == 0);
    CHECK(hdr[20] == 0x2C && hdr[21] == 0x01);
    uint16_t crc = haltz_crc16_update(haltz_crc16(hdr, 30), body, 300);
    CHECK(hdr[30] == LO(crc) && hdr[31] == HI(crc));

    haltz_cart_header_t back;
    CHECK(haltz_cart_check(hdr, body, sizeof body, &back) == HALTZ_OK);
    CHECK(back.type == HALTZ_CART_CODE && back.length == 300 && back.crc == crc);
    CHECK(strcmp(back.title, "SPACE ROCKS") == 0 && back.key_id == 0);
    CHECK(haltz_cart_check(hdr, body, 299, NULL) == HALTZ_ERR_BAD_LEN);
    body[299] ^= 1;
    CHECK(haltz_cart_check(hdr, body, sizeof body, NULL) == HALTZ_ERR_BAD_CRC);
    body[299] ^= 1;
    body[300] ^= 1; /* past the length: not covered */
    CHECK(haltz_cart_check(hdr, body, sizeof body, NULL) == HALTZ_OK);

    /* A 16-character title fills the field with no 0. */
    haltz_cart_header_t k = {.type = HALTZ_CART_KEY, .title = "ABCDEFGHIJKLMNOP",
                             .key_id = 0xDEADBEEF};
    CHECK(haltz_cart_header_pack(&k, NULL, hdr) == HALTZ_OK);
    CHECK(haltz_cart_check(hdr, NULL, 0, &back) == HALTZ_OK);
    CHECK(back.key_id == 0xDEADBEEF && strcmp(back.title, "ABCDEFGHIJKLMNOP") == 0);

    /* Pack refuses broken fields. */
    haltz_cart_header_t b = h;
    b.type = 3;
    CHECK(haltz_cart_header_pack(&b, body, hdr) == HALTZ_ERR_BAD_VALUE);
    b = h;
    b.flags = 1;
    CHECK(haltz_cart_header_pack(&b, body, hdr) == HALTZ_ERR_BAD_VALUE);
    b = h;
    b.key_id = 5; /* CODE with a key */
    CHECK(haltz_cart_header_pack(&b, body, hdr) == HALTZ_ERR_BAD_VALUE);
    b = k;
    b.key_id = 0; /* KEY without one */
    CHECK(haltz_cart_header_pack(&b, NULL, hdr) == HALTZ_ERR_BAD_VALUE);
    b = h;
    b.title[2] = '\n';
    CHECK(haltz_cart_header_pack(&b, body, hdr) == HALTZ_ERR_BAD_VALUE);
    CHECK(haltz_cart_header_pack(&h, NULL, hdr) == HALTZ_ERR_ARG);

    /* Unpack refuses broken headers. */
    CHECK(haltz_cart_header_pack(&h, body, hdr) == HALTZ_OK);
    static const struct {
        unsigned at;
        uint8_t value;
        haltz_err_t err;
    } bad[] = {
        {0, 'X', HALTZ_ERR_BAD_VALUE}, {2, 2, HALTZ_ERR_VERSION},  {3, 7, HALTZ_ERR_BAD_VALUE},
        {9, 0x01, HALTZ_ERR_BAD_VALUE}, {18, 'Z', HALTZ_ERR_BAD_VALUE}, /* text after the 0 */
        {22, 1, HALTZ_ERR_BAD_VALUE},  {23, 1, HALTZ_ERR_BAD_VALUE}, {24, 1, HALTZ_ERR_BAD_VALUE},
        {29, 1, HALTZ_ERR_BAD_VALUE},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        uint8_t copy[HALTZ_CART_HEADER_LEN];
        memcpy(copy, hdr, sizeof copy);
        copy[bad[i].at] = bad[i].value;
        CHECK(haltz_cart_header_unpack(copy, &back) == bad[i].err);
    }

    /* Block map: sector trailers (block 3 of each sector) are skipped. */
    CHECK(haltz_cart_body_block(0) == 4 && haltz_cart_body_block(15) == 4);
    CHECK(haltz_cart_body_block(16) == 5 && haltz_cart_body_block(47) == 6);
    CHECK(haltz_cart_body_block(48) == 8 && haltz_cart_body_block(623) == 54);
    CHECK(haltz_cart_body_block(624) == -1);
    CHECK(haltz_cart_save_block(0) == 56 && haltz_cart_save_block(48) == 60);
    CHECK(haltz_cart_save_block(95) == 62 && haltz_cart_save_block(96) == -1);
    for (unsigned off = 0; off < HALTZ_CART_BODY_MAX; off++) {
        int blk = haltz_cart_body_block(off);
        CHECK(blk % 4 != 3 && blk > 3 && blk < 56);
    }
}
