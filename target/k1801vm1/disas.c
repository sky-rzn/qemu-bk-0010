/*
 * K1801VM1 disassembler
 *
 * Copyright (c) 2019 Alexandr Ivanov
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "disas/dis-asm.h"
#include "cpu.h"

typedef struct {
    K1801VM1FetchFn fetch;
    void *opaque;
    uint16_t pc;
} DisasState;

static uint16_t next_word(DisasState *s)
{
    uint16_t val = s->fetch(s->opaque, s->pc);

    s->pc += 2;
    return val;
}

static const char *regname(int reg)
{
    static const char *const names[] = {
        "r0", "r1", "r2", "r3", "r4", "r5", "sp", "pc"
    };

    return names[reg & 7];
}

static void operand(DisasState *s, int addr, char *res, size_t size)
{
    int mode = (addr & 070) >> 3;
    int reg = addr & 007;
    uint16_t val;

    switch (mode) {
    case 0:
        snprintf(res, size, "%s", regname(reg));
        break;
    case 1:
        snprintf(res, size, "(%s)", regname(reg));
        break;
    case 2:
        if (reg == 7) {
            snprintf(res, size, "#%o", next_word(s));
        } else {
            snprintf(res, size, "(%s)+", regname(reg));
        }
        break;
    case 3:
        if (reg == 7) {
            snprintf(res, size, "@#%o", next_word(s));
        } else {
            snprintf(res, size, "@(%s)+", regname(reg));
        }
        break;
    case 4:
        snprintf(res, size, "-(%s)", regname(reg));
        break;
    case 5:
        snprintf(res, size, "@-(%s)", regname(reg));
        break;
    case 6:
    case 7:
        val = next_word(s);
        if (reg == 7) {
            snprintf(res, size, "%s%o", mode == 7 ? "@" : "",
                     (uint16_t)(s->pc + val));
        } else {
            snprintf(res, size, "%s%o(%s)", mode == 7 ? "@" : "",
                     val, regname(reg));
        }
        break;
    }
}

static int dop(DisasState *s, int opcode, char *buf, size_t size)
{
    int oppart, src, dst, byte;
    char srcbuf[32], dstbuf[32];
    const char *op = NULL;

    oppart = opcode & 0070000;
    if (oppart == 0) {
        return 0;
    }

    byte = opcode & 0100000;
    dst = opcode & 0000077;

    if (oppart != 0070000) {
        src = (opcode & 0007700) >> 6;

        switch (oppart) {
        case 0010000:
            op = byte ? "movb" : "mov";
            break;
        case 0020000:
            op = byte ? "cmpb" : "cmp";
            break;
        case 0030000:
            op = byte ? "bitb" : "bit";
            break;
        case 0040000:
            op = byte ? "bicb" : "bic";
            break;
        case 0050000:
            op = byte ? "bisb" : "bis";
            break;
        case 0060000:
            op = byte ? "sub" : "add";
            break;
        }
        operand(s, src, srcbuf, sizeof(srcbuf));
    } else {
        if (byte) {
            return 0;
        }
        oppart = opcode & 0007000;
        src = (opcode & 0000700) >> 6;
        switch (oppart) {
        case 0004000:
            op = "xor";
            break;
        case 0007000:
            return snprintf(buf, size, "sob %s, %o", regname(src),
                            (uint16_t)(s->pc - dst * 2));
        default:
            return 0;
        }
        snprintf(srcbuf, sizeof(srcbuf), "%s", regname(src));
    }

    operand(s, dst, dstbuf, sizeof(dstbuf));
    return snprintf(buf, size, "%s %s, %s", op, srcbuf, dstbuf);
}

static int sop(DisasState *s, int opcode, char *buf, size_t size)
{
    int byte = opcode & 0100000;
    int oppart = opcode & 0077700;
    const char *op = NULL;
    char dstbuf[32];

    switch (oppart) {
    case 000300:
        op = byte ? NULL : "swab";
        break;
    case 005000:
        op = byte ? "clrb" : "clr";
        break;
    case 005100:
        op = byte ? "comb" : "com";
        break;
    case 005200:
        op = byte ? "incb" : "inc";
        break;
    case 005300:
        op = byte ? "decb" : "dec";
        break;
    case 005400:
        op = byte ? "negb" : "neg";
        break;
    case 005500:
        op = byte ? "adcb" : "adc";
        break;
    case 005600:
        op = byte ? "sbcb" : "sbc";
        break;
    case 005700:
        op = byte ? "tstb" : "tst";
        break;
    case 006000:
        op = byte ? "rorb" : "ror";
        break;
    case 006100:
        op = byte ? "rolb" : "rol";
        break;
    case 006200:
        op = byte ? "asrb" : "asr";
        break;
    case 006300:
        op = byte ? "aslb" : "asl";
        break;
    case 006400:
        op = byte ? "mtps" : "mark";
        break;
    case 006500:
        op = byte ? "mfpd" : "mfpi";
        break;
    case 006600:
        op = byte ? "mtpd" : "mtpi";
        break;
    case 006700:
        op = byte ? "mfps" : "sxt";
        break;
    }
    if (!op) {
        return 0;
    }

    operand(s, opcode & 077, dstbuf, sizeof(dstbuf));
    return snprintf(buf, size, "%s %s", op, dstbuf);
}

static int branch(DisasState *s, int opcode, char *buf, size_t size)
{
    char dstbuf[32];
    const char *op;
    int addr;

    if ((opcode & 0777000) == 0004000) {
        operand(s, opcode & 077, dstbuf, sizeof(dstbuf));
        return snprintf(buf, size, "jsr %s, %s",
                        regname((opcode & 0700) >> 6), dstbuf);
    }
    if ((opcode & 0777000) == 0104000) {
        op = (opcode & 0000400) ? "trap" : "emt";
        return snprintf(buf, size, "%s %o", op, opcode & 0377);
    }
    if (opcode == 0000002) {
        return snprintf(buf, size, "rti");
    }
    if ((opcode & 0777700) == 0100) {
        operand(s, opcode & 077, dstbuf, sizeof(dstbuf));
        return snprintf(buf, size, "jmp %s", dstbuf);
    }
    if ((opcode & 0777770) == 0200) {
        return snprintf(buf, size, "rts %s", regname(opcode & 07));
    }

    switch (opcode & 0777400) {
    case 0000400:
        op = "br";
        break;
    case 0001000:
        op = "bne";
        break;
    case 0001400:
        op = "beq";
        break;
    case 0002000:
        op = "bge";
        break;
    case 0002400:
        op = "blt";
        break;
    case 0003000:
        op = "bgt";
        break;
    case 0003400:
        op = "ble";
        break;
    case 0100000:
        op = "bpl";
        break;
    case 0100400:
        op = "bmi";
        break;
    case 0101000:
        op = "bhi";
        break;
    case 0101400:
        op = "blos";
        break;
    case 0102000:
        op = "bvc";
        break;
    case 0102400:
        op = "bvs";
        break;
    case 0103000:
        op = "bcc";
        break;
    case 0103400:
        op = "bcs";
        break;
    default:
        return 0;
    }
    addr = (int8_t)(opcode & 0377) * 2;
    return snprintf(buf, size, "%s %o", op, (uint16_t)(s->pc + addr));
}

static int nop(int opcode, char *buf, size_t size)
{
    const char *op;

    switch (opcode) {
    case 0000000:
        op = "halt";
        break;
    case 0000001:
        op = "wait";
        break;
    case 0000003:
        op = "bpt";
        break;
    case 0000004:
        op = "iot";
        break;
    case 0000005:
        op = "reset";
        break;
    case 0000006:
        op = "rtt";
        break;
    case 0240:
    case 0260:
        op = "nop";
        break;
    case 0241:
        op = "clc";
        break;
    case 0242:
        op = "clv";
        break;
    case 0244:
        op = "clz";
        break;
    case 0250:
        op = "cln";
        break;
    case 0257:
        op = "ccc";
        break;
    case 0261:
        op = "sec";
        break;
    case 0262:
        op = "sev";
        break;
    case 0264:
        op = "sez";
        break;
    case 0270:
        op = "sen";
        break;
    case 0277:
        op = "scc";
        break;
    default:
        if ((opcode & 0777740) == 0240) {
            return snprintf(buf, size, "%s %o",
                            (opcode & 020) ? "se" : "cl", opcode & 017);
        }
        return 0;
    }
    return snprintf(buf, size, "%s", op);
}

int k1801vm1_disasm(uint16_t pc, K1801VM1FetchFn fetch, void *opaque,
                    char *buf, size_t size)
{
    DisasState s = { .fetch = fetch, .opaque = opaque, .pc = pc };
    int opcode = next_word(&s);

    if (dop(&s, opcode, buf, size) > 0 ||
        sop(&s, opcode, buf, size) > 0 ||
        branch(&s, opcode, buf, size) > 0 ||
        nop(opcode, buf, size) > 0) {
        return (uint16_t)(s.pc - pc);
    }
    snprintf(buf, size, ".word %06o", opcode);
    return 2;
}

static uint16_t print_insn_fetch(void *opaque, uint16_t addr)
{
    disassemble_info *info = opaque;
    bfd_byte buf[2];

    if (info->read_memory_func(addr, buf, 2, info) != 0) {
        return 0;
    }
    return bfd_getl16(buf);
}

int k1801vm1_print_insn(bfd_vma addr, disassemble_info *info)
{
    char buf[64];
    int len;

    len = k1801vm1_disasm(addr, print_insn_fetch, info, buf, sizeof(buf));
    info->fprintf_func(info->stream, "%s", buf);
    return len;
}
