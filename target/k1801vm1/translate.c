/*
 * K1801VM1 emulation for qemu: main translation routines.
 *
 * Copyright (c) 2019 Alexandr Ivanov
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "cpu.h"
#include "tcg/tcg-op.h"
#include "exec/translation-block.h"
#include "exec/translator.h"
#include "exec/target_page.h"
#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "exec/log.h"

#define HELPER_H "helper.h"
#include "exec/helper-info.c.inc"
#undef  HELPER_H

/* PC was updated dynamically; look up the next TB. */
#define DISAS_JUMP      DISAS_TARGET_0
/* PSW may have unmasked interrupts; return to the main loop. */
#define DISAS_EXIT      DISAS_TARGET_1
#define DISAS_EXIT_NEXT DISAS_TARGET_2

/* The longest instruction is an opcode plus two operand words. */
#define MAX_INSN_SIZE   6

enum Cc_cmd {
    CC_CMD_ANY,
    CC_CMD_ADD,
    CC_CMD_SUB,
    CC_CMD_ASL,
    CC_CMD_ASR
};

#define CC_PSW_SHIFT_C  0
#define CC_PSW_SHIFT_V  1
#define CC_PSW_SHIFT_Z  2
#define CC_PSW_SHIFT_N  3
#define CC_PSW_MASK_C   K1801VM1_PSW_C
#define CC_PSW_MASK_V   K1801VM1_PSW_V
#define CC_PSW_MASK_Z   K1801VM1_PSW_Z
#define CC_PSW_MASK_N   K1801VM1_PSW_N
#define CC_PSW_MASK_CV  (CC_PSW_MASK_C | CC_PSW_MASK_V)
#define CC_PSW_MASK_ZN  (CC_PSW_MASK_Z | CC_PSW_MASK_N)
#define CC_PSW_MASK_CVZN  (CC_PSW_MASK_CV | CC_PSW_MASK_ZN)
#define SIGN_SHIFT      15
#define SIGN_MASK       (1 << SIGN_SHIFT)
#define CARRY_SHIFT     16
#define CARRY_MASK      (1 << CARRY_SHIFT)
#define SIGN8_SHIFT     7
#define SIGN8_MASK      (1 << SIGN8_SHIFT)
#define CARRY8_SHIFT    8
#define CARRY8_MASK     (1 << CARRY8_SHIFT)

typedef struct {
    int addr;
    int addition;
    int code_size;
} OpArgument;

typedef struct DisasContext {
    DisasContextBase base;
    CPUK1801VM1State *env;
    uint16_t opcode;
    int cycles;
    uint32_t pc;
    OpArgument args[2];
    int byte;
    /* A byte result goes to the whole register, as MOVB does. */
    bool byte_full_store;
} DisasContext;

static TCGv cpu_regs[K1801VM1_REG_NUM];
static TCGv cpu_reg_psw;
static TCGv_i64 cpu_cycles;

#define cpu_reg_pc cpu_regs[K1801VM1_REG_PC]
#define cpu_reg_sp cpu_regs[K1801VM1_REG_SP]

void k1801vm1_translate_init(void)
{
    static const char reg_names[K1801VM1_REG_NUM][4] = {
        "r0", "r1", "r2", "r3", "r4", "r5", "sp", "pc"
    };
    int i;

    for (i = 0; i < K1801VM1_REG_NUM; i++) {
        cpu_regs[i] = tcg_global_mem_new_i32(tcg_env,
                                             offsetof(CPUK1801VM1State,
                                                      regs[i]),
                                             reg_names[i]);
    }
    cpu_reg_psw = tcg_global_mem_new_i32(tcg_env,
                                         offsetof(CPUK1801VM1State, psw),
                                         "psw");
    cpu_cycles = tcg_global_mem_new_i64(tcg_env,
                                        offsetof(CPUK1801VM1State, cycles),
                                        "cycles");
}

static uint16_t code_lduw(DisasContext *ctx, uint32_t addr)
{
    return translator_lduw_end(ctx->env, &ctx->base, addr & 0xffff, MO_LE);
}

static void gen_ld(TCGv val, TCGv addr, MemOp mop)
{
    tcg_gen_qemu_ld_i32(val, addr, 0, mop);
}

static void gen_st(TCGv val, TCGv addr, MemOp mop)
{
    tcg_gen_qemu_st_i32(val, addr, 0, mop);
}

static void gen_goto_tb(DisasContext *ctx, unsigned slot, uint32_t dest)
{
    dest &= 0xffff;
    if (translator_use_goto_tb(&ctx->base, dest)) {
        tcg_gen_goto_tb(slot);
        tcg_gen_movi_tl(cpu_reg_pc, dest);
        tcg_gen_exit_tb(ctx->base.tb, slot);
    } else {
        tcg_gen_movi_tl(cpu_reg_pc, dest);
        tcg_gen_lookup_and_goto_ptr();
    }
    ctx->base.is_jmp = DISAS_NORETURN;
}

static void gen_trap(DisasContext *ctx, int vector)
{
    tcg_gen_movi_tl(cpu_reg_pc, ctx->pc & 0xffff);
    gen_helper_raise_exception(tcg_env, tcg_constant_i32(vector));
    ctx->base.is_jmp = DISAS_NORETURN;
}

static void gen_reserved_insn(DisasContext *ctx)
{
    qemu_log_mask(LOG_UNIMP, "k1801vm1: unimplemented opcode %06o at %06o\n",
                  ctx->opcode, (ctx->pc - 2) & 0xffff);
    gen_trap(ctx, K1801VM1_VEC_RESERVED);
}

static void sign_ext8(TCGv reg)
{
    tcg_gen_ext8s_tl(reg, reg);
    tcg_gen_andi_tl(reg, reg, 0x8000ffff);
}

static void sign_ext16(TCGv reg)
{
    tcg_gen_ext16s_tl(reg, reg);
    tcg_gen_andi_tl(reg, reg, 0x8000ffff);
}

static void get_addr(DisasContext *ctx, TCGv addr, int arg_ind,
                     int reg_changed)
{
    int mode, reg, addition, pc_addr = ctx->pc;
    OpArgument *arg = &ctx->args[arg_ind];

    mode = arg->addr & 070;
    reg = arg->addr & 007;

    addition = (ctx->byte && reg < K1801VM1_REG_SP) ? 1 : 2;

    if (arg_ind == 1) {
        pc_addr += ctx->args[0].code_size;
    }

    if (reg == 7) {
        switch (mode) {
        case 000:
            break;
        case 010:
            tcg_gen_movi_tl(addr, ctx->pc & 0xffff);
            break;
        case 030:
            tcg_gen_movi_tl(addr, code_lduw(ctx, pc_addr));
            break;
        case 060:
        case 070:
            pc_addr += (int16_t)code_lduw(ctx, pc_addr) + 2;
            pc_addr &= 0xffff;
            tcg_gen_movi_tl(addr, pc_addr);
            if (mode == 070) {
                gen_ld(addr, addr, MO_LEUW);
            }
            break;
        case 020:
            tcg_gen_movi_tl(addr, pc_addr & 0xffff);
            break;
        default:
            gen_reserved_insn(ctx);
            tcg_gen_movi_tl(addr, 0);
            return;
        }
        if (mode != 0 && mode != 010) {
            arg->code_size = 2;
        }
    } else {
        switch (mode) {
        case 000:
            break;
        case 010:
        case 020:
            tcg_gen_andi_tl(addr, cpu_regs[reg], 0xffff);
            if (mode == 020) {
                arg->addition += addition;
            }
            break;
        case 030:
            tcg_gen_andi_tl(addr, cpu_regs[reg], 0xffff);
            gen_ld(addr, addr, MO_LEUW);
            arg->addition += 2;
            break;
        case 040:
            if (reg_changed) {
                tcg_gen_andi_tl(addr, cpu_regs[reg], 0xffff);
            } else {
                tcg_gen_subi_tl(addr, cpu_regs[reg], addition);
                tcg_gen_andi_tl(addr, addr, 0xffff);
            }
            arg->addition += -addition;
            break;
        case 050:
            if (reg_changed) {
                tcg_gen_andi_tl(addr, cpu_regs[reg], 0xffff);
            } else {
                tcg_gen_subi_tl(addr, cpu_regs[reg], 2);
                tcg_gen_andi_tl(addr, addr, 0xffff);
            }
            gen_ld(addr, addr, MO_LEUW);
            arg->addition += -2;
            break;
        case 060:
        case 070:
            tcg_gen_addi_tl(addr, cpu_regs[reg], code_lduw(ctx, pc_addr));
            tcg_gen_andi_tl(addr, addr, 0xffff);
            if (mode == 070) {
                gen_ld(addr, addr, MO_LEUW);
            }
            arg->code_size = 2;
            break;
        }
    }
}

static void load_operand(DisasContext *ctx, TCGv t, int arg_ind,
                         int with_store)
{
    TCGv addr;
    int mode, reg;
    OpArgument *arg = &ctx->args[arg_ind];

    mode = arg->addr & 070;
    reg = arg->addr & 007;

    if (mode == 0) {
        if (reg == 7) {
            tcg_gen_movi_tl(t, ctx->pc & 0xffff);
        } else {
            tcg_gen_mov_tl(t, cpu_regs[reg]);
        }
        return;
    }

    addr = tcg_temp_new();
    get_addr(ctx, addr, arg_ind, 0);

    if (arg->addition < 0) {
        tcg_gen_addi_tl(cpu_regs[reg], cpu_regs[reg], arg->addition);
    }

    if (ctx->byte) {
        gen_ld(t, addr, MO_UB);
        sign_ext8(t);
    } else {
        gen_ld(t, addr, MO_LEUW);
        sign_ext16(t);
    }

    if (arg->addition > 0) {
        if (with_store) {
            arg->addition = 0;
        } else {
            tcg_gen_addi_tl(cpu_regs[reg], cpu_regs[reg], arg->addition);
        }
    }
}

static void load_src(DisasContext *ctx, TCGv src)
{
    load_operand(ctx, src, 0, 0);
}

static void load_srcdst(DisasContext *ctx, TCGv src, TCGv dst,
                        int with_store)
{
    load_operand(ctx, src, 0, 0);
    load_operand(ctx, dst, 1, with_store);
}

static void store_dst(DisasContext *ctx, TCGv dst, int save)
{
    int mode, reg, prev_addition;
    OpArgument *arg = &ctx->args[1];
    TCGv addr;

    mode = arg->addr & 070;
    reg = arg->addr & 007;

    if (mode == 0) {
        if (!save) {
            return;
        }
        if (reg == 7) {
            tcg_gen_andi_tl(cpu_reg_pc, dst, 0xffff);
            ctx->base.is_jmp = DISAS_JUMP;
        } else if (ctx->byte && !ctx->byte_full_store) {
            tcg_gen_deposit_tl(cpu_regs[reg], cpu_regs[reg], dst, 0, 8);
            sign_ext16(cpu_regs[reg]);
        } else {
            tcg_gen_andi_tl(cpu_regs[reg], dst, 0x8000ffff);
        }
        return;
    }

    addr = tcg_temp_new();

    prev_addition = arg->addition;

    get_addr(ctx, addr, 1, (prev_addition != 0));

    if (prev_addition == 0 && arg->addition < 0) {
        tcg_gen_addi_tl(cpu_regs[reg], cpu_regs[reg], arg->addition);
    }

    if (save) {
        gen_st(dst, addr, ctx->byte ? MO_UB : MO_LEUW);
    }

    if (prev_addition == 0 && arg->addition > 0) {
        tcg_gen_addi_tl(cpu_regs[reg], cpu_regs[reg], arg->addition);
    }
}

static void gen_jmp_addr(DisasContext *ctx, TCGv dest, int addr)
{
    int mode = addr & 070;
    int reg = addr & 007;
    TCGv tmp;

    if (reg == 7) {
        switch (mode) {
        case 030:
            tcg_gen_movi_tl(dest, code_lduw(ctx, ctx->pc));
            ctx->pc += 2;
            return;
        case 060:
            tcg_gen_movi_tl(dest, (ctx->pc + (int16_t)code_lduw(ctx, ctx->pc)
                                   + 2) & 0xffff);
            ctx->pc += 2;
            return;
        case 070:
            tmp = tcg_temp_new();
            tcg_gen_movi_tl(tmp, (ctx->pc + (int16_t)code_lduw(ctx, ctx->pc)
                                  + 2) & 0xffff);
            gen_ld(dest, tmp, MO_LEUW);
            ctx->pc += 2;
            return;
        }
    } else {
        switch (mode) {
        case 010:
            tcg_gen_andi_tl(dest, cpu_regs[reg], 0xffff);
            return;
        case 020:
            tcg_gen_andi_tl(dest, cpu_regs[reg], 0xffff);
            tcg_gen_addi_tl(cpu_regs[reg], dest, 2);
            return;
        case 040:
        case 050:
            tcg_gen_subi_tl(cpu_regs[reg], cpu_regs[reg], 2);
            tcg_gen_andi_tl(cpu_regs[reg], cpu_regs[reg], 0xffff);
            if (mode == 050) {
                gen_ld(dest, cpu_regs[reg], MO_LEUW);
            } else {
                tcg_gen_mov_tl(dest, cpu_regs[reg]);
            }
            return;
        case 030:
            tmp = tcg_temp_new();
            tcg_gen_andi_tl(tmp, cpu_regs[reg], 0xffff);
            gen_ld(dest, tmp, MO_LEUW);
            tcg_gen_addi_tl(cpu_regs[reg], cpu_regs[reg], 2);
            tcg_gen_andi_tl(cpu_regs[reg], cpu_regs[reg], 0xffff);
            return;
        case 060:
        case 070:
            tmp = tcg_temp_new();
            tcg_gen_addi_tl(tmp, cpu_regs[reg], code_lduw(ctx, ctx->pc));
            tcg_gen_andi_tl(tmp, tmp, 0xffff);
            if (mode == 070) {
                gen_ld(dest, tmp, MO_LEUW);
            } else {
                tcg_gen_mov_tl(dest, tmp);
            }
            ctx->pc += 2;
            return;
        }
    }

    qemu_log_mask(LOG_UNIMP, "k1801vm1: unsupported jump mode %o at %06o\n",
                  addr, (ctx->pc - 2) & 0xffff);
    gen_trap(ctx, K1801VM1_VEC_RESERVED);
    tcg_gen_movi_tl(dest, 0);
}

static void push(TCGv t)
{
    tcg_gen_subi_tl(cpu_reg_sp, cpu_reg_sp, 2);
    tcg_gen_andi_tl(cpu_reg_sp, cpu_reg_sp, 0xffff);
    gen_st(t, cpu_reg_sp, MO_LEUW);
}

static void pop(TCGv t)
{
    TCGv addr = tcg_temp_new();

    tcg_gen_andi_tl(addr, cpu_reg_sp, 0xffff);
    gen_ld(t, addr, MO_LEUW);
    tcg_gen_addi_tl(cpu_reg_sp, addr, 2);
}

static void cc_clr_c(void)
{
    tcg_gen_andi_tl(cpu_reg_psw, cpu_reg_psw, ~CC_PSW_MASK_C);
}

static void cc_set_c(void)
{
    tcg_gen_ori_tl(cpu_reg_psw, cpu_reg_psw, CC_PSW_MASK_C);
}

static void cc_clr_v(void)
{
    tcg_gen_andi_tl(cpu_reg_psw, cpu_reg_psw, ~CC_PSW_MASK_V);
}

static void cc_save_psw(DisasContext *ctx, TCGv arg1, TCGv arg2, TCGv res,
                        int mask, enum Cc_cmd cmd)
{
    uint32_t sign_mask, sign_shift;
    uint32_t carry_mask, carry_shift;
    TCGv zero = tcg_constant_tl(0);

    tcg_gen_andi_tl(cpu_reg_psw, cpu_reg_psw, ~mask);

    sign_mask = ctx->byte ? SIGN8_MASK : SIGN_MASK;
    sign_shift = ctx->byte ? SIGN8_SHIFT : SIGN_SHIFT;
    carry_mask = ctx->byte ? CARRY8_MASK : CARRY_MASK;
    carry_shift = ctx->byte ? CARRY8_SHIFT : CARRY_SHIFT;

    if (mask & CC_PSW_MASK_Z) {
        TCGv t1 = tcg_temp_new();
        TCGv t2 = tcg_temp_new();
        tcg_gen_ori_tl(t1, cpu_reg_psw, CC_PSW_MASK_Z);
        tcg_gen_andi_tl(t2, res, ctx->byte ? 0xff : 0xffff);
        tcg_gen_movcond_tl(TCG_COND_EQ, cpu_reg_psw, t2, zero, t1,
                           cpu_reg_psw);
    }
    if (mask & CC_PSW_MASK_N) {
        TCGv t1 = tcg_temp_new();
        tcg_gen_andi_tl(t1, res, sign_mask);
        tcg_gen_shri_tl(t1, t1, sign_shift - CC_PSW_SHIFT_N);
        tcg_gen_or_tl(cpu_reg_psw, cpu_reg_psw, t1);
    }
    if (mask & CC_PSW_MASK_C) {
        TCGv t1 = tcg_temp_new();
        if (cmd == CC_CMD_ASR) {
            tcg_gen_andi_tl(t1, arg1, 1);
            tcg_gen_shli_tl(t1, t1, carry_shift);
        } else if (cmd == CC_CMD_SUB) {
            TCGv t3 = tcg_temp_new();
            TCGv t4 = tcg_temp_new();

            tcg_gen_andi_tl(t3, arg1, ctx->byte ? 0xff : 0xffff);
            tcg_gen_andi_tl(t4, arg2, ctx->byte ? 0xff : 0xffff);
            tcg_gen_movcond_tl(TCG_COND_GT, t1, t3, t4,
                               tcg_constant_tl(carry_mask), zero);
        } else {
            tcg_gen_andi_tl(t1, res, carry_mask);
        }
        tcg_gen_shri_tl(t1, t1, carry_shift - CC_PSW_SHIFT_C);
        tcg_gen_or_tl(cpu_reg_psw, cpu_reg_psw, t1);
    }
    if (mask & CC_PSW_MASK_V) {
        TCGv t1 = tcg_temp_new();
        if (cmd == CC_CMD_ADD) {
            TCGv t2 = tcg_temp_new();
            tcg_gen_xor_tl(t1, arg1, res);
            tcg_gen_xor_tl(t2, arg1, arg2);
            tcg_gen_andc_tl(t1, t1, t2);
        } else if (cmd == CC_CMD_SUB) {
            TCGv t2 = tcg_temp_new();
            tcg_gen_xor_tl(t1, arg2, res);
            tcg_gen_xor_tl(t2, arg1, arg2);
            tcg_gen_and_tl(t1, t1, t2);
        } else if (cmd == CC_CMD_ASL) {
            tcg_gen_xor_tl(t1, arg1, res);
        } else if (cmd == CC_CMD_ASR) {
            TCGv t2 = tcg_temp_new();
            tcg_gen_andi_tl(t1, cpu_reg_psw, CC_PSW_MASK_N);
            tcg_gen_shri_tl(t1, t1, CC_PSW_SHIFT_N - CC_PSW_SHIFT_V);
            tcg_gen_andi_tl(t2, cpu_reg_psw, CC_PSW_MASK_C);
            tcg_gen_shli_tl(t2, t2, CC_PSW_SHIFT_V - CC_PSW_SHIFT_C);
            tcg_gen_xor_tl(t1, t1, t2);
            tcg_gen_or_tl(cpu_reg_psw, cpu_reg_psw, t1);
        } else {
            tcg_gen_movi_tl(t1, 0);
        }
        tcg_gen_andi_tl(t1, t1, sign_mask);
        tcg_gen_shri_tl(t1, t1, sign_shift - CC_PSW_SHIFT_V);
        tcg_gen_or_tl(cpu_reg_psw, cpu_reg_psw, t1);
    }
}

static void cc_set_v_xor_cn(void)
{
    TCGv t1 = tcg_temp_new();
    TCGv t2 = tcg_temp_new();
    tcg_gen_rotli_tl(t1, cpu_reg_psw, CC_PSW_SHIFT_V - CC_PSW_SHIFT_C);
    tcg_gen_rotri_tl(t2, cpu_reg_psw, CC_PSW_SHIFT_N - CC_PSW_SHIFT_V);
    tcg_gen_xor_tl(t1, t1, t2);
    tcg_gen_andi_tl(t1, t1, CC_PSW_MASK_V);
    tcg_gen_andi_tl(cpu_reg_psw, cpu_reg_psw, ~CC_PSW_MASK_V);
    tcg_gen_or_tl(cpu_reg_psw, cpu_reg_psw, t1);
}

static bool decode_dop(DisasContext *ctx, int op)
{
    int oppart;

    oppart = op & 0070000;
    if (oppart == 0) {
        return false;
    }

    ctx->byte = op & 0100000;
    ctx->args[1].addr = op & 0000077;

    if (oppart != 0070000) {
        ctx->args[0].addr = (op & 0007700) >> 6;

        switch (oppart) {
        case 0010000: /* MOV(B) */
            {
                /* reproduce CPU bug - https://habr.com/ru/post/471020/ */
                TCGv t1 = tcg_temp_new();
                load_src(ctx, t1);
                cc_save_psw(ctx, t1, t1, t1, CC_PSW_MASK_ZN, CC_CMD_ANY);
                cc_clr_v();
                if (ctx->byte && (ctx->args[1].addr & 070) == 0) {
                    sign_ext8(t1);
                }
                ctx->byte_full_store = true;
                store_dst(ctx, t1, 1);
            }
            break;
        case 0020000: /* CMP(B) */
            {
                TCGv t1 = tcg_temp_new();
                TCGv t2 = tcg_temp_new();
                TCGv t3 = tcg_temp_new();
                load_srcdst(ctx, t1, t2, 0);
                tcg_gen_andi_tl(t1, t1, ctx->byte ? 0xff : 0xffff);
                tcg_gen_andi_tl(t2, t2, ctx->byte ? 0xff : 0xffff);
                tcg_gen_sub_tl(t3, t1, t2);
                cc_save_psw(ctx, t2, t1, t3, CC_PSW_MASK_CVZN, CC_CMD_SUB);
            }
            break;
        case 0030000: /* BIT(B) */
            {
                TCGv t1 = tcg_temp_new();
                TCGv t2 = tcg_temp_new();
                load_srcdst(ctx, t1, t2, 0);
                tcg_gen_and_tl(t2, t2, t1);
                cc_save_psw(ctx, t1, t2, t2, CC_PSW_MASK_ZN, CC_CMD_ANY);
                cc_clr_v();
            }
            break;
        case 0040000: /* BIC(B) */
            {
                TCGv t1 = tcg_temp_new();
                TCGv t2 = tcg_temp_new();
                load_srcdst(ctx, t1, t2, 1);
                tcg_gen_andc_tl(t2, t2, t1);
                cc_save_psw(ctx, t1, t2, t2, CC_PSW_MASK_ZN, CC_CMD_ANY);
                cc_clr_v();
                sign_ext16(t2);
                store_dst(ctx, t2, 1);
            }
            break;
        case 0050000: /* BIS(B) */
            {
                TCGv t1 = tcg_temp_new();
                TCGv t2 = tcg_temp_new();
                load_srcdst(ctx, t1, t2, 1);
                if (ctx->byte) {
                    tcg_gen_andi_tl(t1, t1, 0xff);
                }
                tcg_gen_or_tl(t2, t2, t1);
                cc_save_psw(ctx, t1, t2, t2, CC_PSW_MASK_ZN, CC_CMD_ANY);
                cc_clr_v();
                store_dst(ctx, t2, 1);
            }
            break;
        case 0060000: /* ADD, SUB */
            {
                int sub = ctx->byte;
                TCGv t1 = tcg_temp_new();
                TCGv t2 = tcg_temp_new();
                TCGv t3 = tcg_temp_new();
                ctx->byte = 0;
                load_srcdst(ctx, t1, t2, 1);
                if (sub) {
                    tcg_gen_sub_tl(t3, t2, t1);
                } else {
                    tcg_gen_add_tl(t3, t1, t2);
                }
                cc_save_psw(ctx, t1, t2, t3, CC_PSW_MASK_CVZN,
                            sub ? CC_CMD_SUB : CC_CMD_ADD);
                sign_ext16(t3);
                store_dst(ctx, t3, 1);
            }
            break;
        default:
            return false;
        }
    } else {
        if (ctx->byte) {
            return false;
        }
        oppart = op & 0007000;
        ctx->args[0].addr = (op & 0000700) >> 6;
        switch (oppart) {
        case 0004000: /* XOR */
            {
                TCGv t1 = tcg_temp_new();
                TCGv t2 = tcg_temp_new();
                load_srcdst(ctx, t1, t2, 1);
                tcg_gen_xor_tl(t2, t1, t2);
                cc_save_psw(ctx, t1, t2, t2, CC_PSW_MASK_ZN, CC_CMD_ANY);
                cc_clr_v();
                store_dst(ctx, t2, 1);
            }
            break;
        case 0007000: /* SOB */
            {
                int src = ctx->args[0].addr;
                int dst = ctx->args[1].addr;
                TCGLabel *l = gen_new_label();

                tcg_gen_subi_tl(cpu_regs[src], cpu_regs[src], 1);
                sign_ext16(cpu_regs[src]);
                tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_regs[src], 0, l);
                gen_goto_tb(ctx, 0, ctx->pc - (dst << 1));
                gen_set_label(l);
                gen_goto_tb(ctx, 1, ctx->pc);
            }
            break;
        default:
            return false;
        }
    }
    return true;
}

static bool decode_sop(DisasContext *ctx, int op)
{
    int oppart, src_size, save = 1;
    TCGv t;

    ctx->args[0].addr = op & 0000077;
    ctx->args[1].addr = ctx->args[0].addr;
    ctx->byte = op & 0100000;
    oppart = op & 0077700;

    if (oppart == 000300) {
        if (ctx->byte) {
            return false;
        }
    } else if (oppart < 005000 || oppart > 006700) {
        return false;
    }

    if (oppart == 006500 || oppart == 006600 ||
        (oppart == 006400 && !ctx->byte)) {
        /* MARK, MFPI, MFPD, MTPI, MTPD */
        gen_reserved_insn(ctx);
        return true;
    }

    t = tcg_temp_new();

    if (oppart == 006700) {
        if (ctx->byte) { /* MFPS */
            tcg_gen_andi_tl(t, cpu_reg_psw, 0xff);
            sign_ext8(t);
            cc_save_psw(ctx, t, t, t, CC_PSW_MASK_ZN, CC_CMD_ANY);
            ctx->byte_full_store = true;
        } else { /* SXT */
            TCGv t1 = tcg_temp_new();
            tcg_gen_andi_tl(t1, cpu_reg_psw, CC_PSW_MASK_N);
            tcg_gen_movcond_tl(TCG_COND_NE, t, t1, tcg_constant_tl(0),
                               tcg_constant_tl(0x8000ffff),
                               tcg_constant_tl(0));
            cc_save_psw(ctx, t, t, t, CC_PSW_MASK_Z, CC_CMD_ANY);
        }
        cc_clr_v();
        store_dst(ctx, t, 1);
        return true;
    }

    load_operand(ctx, t, 0, (oppart != 006400));
    if (ctx->byte) {
        tcg_gen_andi_tl(t, t, 0xff);
    }

    src_size = ctx->args[0].code_size;
    ctx->args[0].code_size = 0;
    ctx->args[1].addition = ctx->args[0].addition;

    switch (oppart) {
    case 000300: /* SWAB */
        {
            TCGv t1 = tcg_temp_new();
            tcg_gen_bswap16_tl(t, t, 0);
            tcg_gen_andi_tl(t1, t, 0xff);
            sign_ext8(t1);
            cc_save_psw(ctx, t1, t1, t1, CC_PSW_MASK_ZN, CC_CMD_ANY);
            cc_clr_c();
            cc_clr_v();
        }
        break;
    case 005000: /* CLR(B) */
        tcg_gen_andi_tl(t, t, ctx->byte ? 0xff00 : 0x0000);
        tcg_gen_andi_tl(cpu_reg_psw, cpu_reg_psw, ~CC_PSW_MASK_CVZN);
        tcg_gen_ori_tl(cpu_reg_psw, cpu_reg_psw, CC_PSW_MASK_Z);
        break;
    case 005100: /* COM(B) */
        tcg_gen_not_tl(t, t);
        cc_save_psw(ctx, t, t, t, CC_PSW_MASK_ZN, CC_CMD_ANY);
        cc_set_c();
        cc_clr_v();
        break;
    case 005200: /* INC(B) */
        {
            TCGv t1 = tcg_temp_new();
            TCGv t2 = tcg_constant_tl(1);
            tcg_gen_mov_tl(t1, t);
            tcg_gen_add_tl(t, t1, t2);
            cc_save_psw(ctx, t1, t2, t, CC_PSW_MASK_V | CC_PSW_MASK_ZN,
                        CC_CMD_ADD);
        }
        break;
    case 005300: /* DEC(B) */
        {
            TCGv t1 = tcg_temp_new();
            TCGv t2 = tcg_constant_tl(1);
            tcg_gen_mov_tl(t1, t);
            tcg_gen_sub_tl(t, t1, t2);
            cc_save_psw(ctx, t2, t1, t, CC_PSW_MASK_V | CC_PSW_MASK_ZN,
                        CC_CMD_SUB);
        }
        break;
    case 005400: /* NEG(B) */
        {
            TCGv t1 = tcg_temp_new();
            tcg_gen_mov_tl(t1, t);
            tcg_gen_neg_tl(t, t);
            cc_save_psw(ctx, t1, tcg_constant_tl(0), t, CC_PSW_MASK_CVZN,
                        CC_CMD_SUB);
        }
        break;
    case 005500: /* ADC(B) */
        {
            TCGv t1 = tcg_temp_new();
            TCGv t2 = tcg_temp_new();
            tcg_gen_mov_tl(t1, t);
            tcg_gen_andi_tl(t2, cpu_reg_psw, CC_PSW_MASK_C);
            tcg_gen_add_tl(t, t1, t2);
            cc_save_psw(ctx, t1, t2, t, CC_PSW_MASK_CVZN, CC_CMD_ADD);
        }
        break;
    case 005600: /* SBC(B) */
        {
            TCGv t1 = tcg_temp_new();
            TCGv t2 = tcg_temp_new();
            tcg_gen_mov_tl(t2, t);
            tcg_gen_andi_tl(t1, cpu_reg_psw, CC_PSW_MASK_C);
            tcg_gen_sub_tl(t, t2, t1);
            cc_save_psw(ctx, t1, t2, t, CC_PSW_MASK_CVZN, CC_CMD_SUB);
        }
        break;
    case 005700: /* TST(B) */
        save = 0;
        cc_save_psw(ctx, t, t, t, CC_PSW_MASK_ZN, CC_CMD_ANY);
        cc_clr_c();
        cc_clr_v();
        break;
    case 006000: /* ROR(B) */
        {
            uint32_t shift = ctx->byte ? SIGN8_SHIFT : SIGN_SHIFT;
            uint32_t mask = ctx->byte ? SIGN8_MASK : SIGN_MASK;
            TCGv t1 = tcg_temp_new();
            TCGv t2 = tcg_temp_new();

            tcg_gen_andi_tl(t1, t, 1);
            tcg_gen_shri_tl(t, t, 1);

            tcg_gen_rotli_tl(t2, cpu_reg_psw, shift - CC_PSW_SHIFT_C);
            tcg_gen_andi_tl(t2, t2, mask);
            tcg_gen_andi_tl(t, t, ~mask);
            tcg_gen_or_tl(t, t, t2);

            tcg_gen_andi_tl(cpu_reg_psw, cpu_reg_psw, ~CC_PSW_MASK_C);
            tcg_gen_or_tl(cpu_reg_psw, cpu_reg_psw, t1);

            cc_save_psw(ctx, t, t, t, CC_PSW_MASK_ZN, CC_CMD_ANY);
            cc_set_v_xor_cn();
        }
        break;
    case 006100: /* ROL(B) */
        {
            uint32_t shift = ctx->byte ? SIGN8_SHIFT : SIGN_SHIFT;
            uint32_t mask = ctx->byte ? SIGN8_MASK : SIGN_MASK;
            TCGv t1 = tcg_temp_new();
            TCGv t2 = tcg_temp_new();

            tcg_gen_andi_tl(t1, t, mask);

            tcg_gen_shli_tl(t, t, 1);

            tcg_gen_andi_tl(t2, cpu_reg_psw, CC_PSW_MASK_C);
            tcg_gen_andi_tl(t, t, ~1);
            tcg_gen_or_tl(t, t, t2);

            tcg_gen_shri_tl(t1, t1, shift - CC_PSW_SHIFT_C);
            tcg_gen_andi_tl(cpu_reg_psw, cpu_reg_psw, ~CC_PSW_MASK_C);
            tcg_gen_or_tl(cpu_reg_psw, cpu_reg_psw, t1);

            cc_save_psw(ctx, t, t, t, CC_PSW_MASK_ZN, CC_CMD_ANY);
            cc_set_v_xor_cn();
        }
        break;
    case 006200: /* ASR(B) */
        {
            TCGv t1 = tcg_temp_new();
            tcg_gen_mov_tl(t1, t);
            if (ctx->byte) {
                tcg_gen_ext8s_tl(t, t);
            } else {
                tcg_gen_ext16s_tl(t, t);
            }
            tcg_gen_sari_tl(t, t, 1);
            cc_save_psw(ctx, t1, t1, t, CC_PSW_MASK_CVZN, CC_CMD_ASR);
        }
        break;
    case 006300: /* ASL(B) */
        {
            TCGv t1 = tcg_temp_new();
            tcg_gen_mov_tl(t1, t);
            tcg_gen_shli_tl(t, t, 1);
            cc_save_psw(ctx, t1, t1, t, CC_PSW_MASK_CVZN, CC_CMD_ASL);
        }
        break;
    case 006400: /* MTPS */
        tcg_gen_andi_tl(t, t, 0xff & ~K1801VM1_PSW_T);
        tcg_gen_andi_tl(cpu_reg_psw, cpu_reg_psw, K1801VM1_PSW_T);
        tcg_gen_or_tl(cpu_reg_psw, cpu_reg_psw, t);
        ctx->args[0].code_size = src_size;
        ctx->base.is_jmp = DISAS_EXIT_NEXT;
        return true;
    default:
        g_assert_not_reached();
    }
    store_dst(ctx, t, save);
    return true;
}

/* The branch is taken unless t satisfies cond against zero. */
static void gen_branch(DisasContext *ctx, int addr, TCGv t, TCGCond cond)
{
    TCGLabel *l = gen_new_label();

    tcg_gen_brcondi_tl(cond, t, 0, l);
    gen_goto_tb(ctx, 0, ctx->pc + addr);
    gen_set_label(l);
    gen_goto_tb(ctx, 1, ctx->pc);
}

static void gen_branch_mask(DisasContext *ctx, int addr, int mask,
                            TCGCond cond)
{
    TCGv t = tcg_temp_new();

    tcg_gen_andi_tl(t, cpu_reg_psw, mask);
    gen_branch(ctx, addr, t, cond);
}

static void gen_branch_nv(DisasContext *ctx, int addr, TCGCond cond)
{
    TCGv t = tcg_temp_new();

    tcg_gen_shri_tl(t, cpu_reg_psw, CC_PSW_SHIFT_N - CC_PSW_SHIFT_V);
    tcg_gen_xor_tl(t, t, cpu_reg_psw);
    tcg_gen_andi_tl(t, t, CC_PSW_MASK_V);
    gen_branch(ctx, addr, t, cond);
}

static void gen_branch_znv(DisasContext *ctx, int addr, TCGCond cond)
{
    TCGv t1 = tcg_temp_new();
    TCGv t2 = tcg_temp_new();

    tcg_gen_shri_tl(t1, cpu_reg_psw, CC_PSW_SHIFT_N - CC_PSW_SHIFT_Z);
    tcg_gen_shli_tl(t2, cpu_reg_psw, CC_PSW_SHIFT_Z - CC_PSW_SHIFT_V);
    tcg_gen_xor_tl(t1, t1, t2);
    tcg_gen_or_tl(t1, t1, cpu_reg_psw);
    tcg_gen_andi_tl(t1, t1, CC_PSW_MASK_Z);
    gen_branch(ctx, addr, t1, cond);
}

static bool decode_branch(DisasContext *ctx, int op)
{
    int reg, addr;

    if ((op & 0777000) == 0004000) { /* JSR */
        TCGv dest = tcg_temp_new();

        reg = (op & 0000700) >> 6;
        gen_jmp_addr(ctx, dest, op & 0000077);
        if (ctx->base.is_jmp == DISAS_NORETURN) {
            return true;
        }
        if (reg != 7) {
            push(cpu_regs[reg]);
            tcg_gen_movi_tl(cpu_regs[reg], ctx->pc & 0xffff);
        } else {
            push(tcg_constant_tl(ctx->pc & 0xffff));
        }
        tcg_gen_mov_tl(cpu_reg_pc, dest);
    } else if ((op & 0777000) == 0104000) { /* EMT, TRAP */
        gen_trap(ctx, (op & 0000400) ? 034 : 030);
        return true;
    } else if (op == 0000003) { /* BPT */
        gen_trap(ctx, 014);
        return true;
    } else if (op == 0000004) { /* IOT */
        gen_trap(ctx, 020);
        return true;
    } else if (op == 0000001) { /* WAIT */
        tcg_gen_movi_tl(cpu_reg_pc, ctx->pc & 0xffff);
        gen_helper_wait(tcg_env);
        ctx->base.is_jmp = DISAS_NORETURN;
        return true;
    } else if (op == 0000002 || op == 0000006) { /* RTI, RTT */
        TCGv t = tcg_temp_new();
        pop(cpu_reg_pc);
        pop(t);
        tcg_gen_andi_tl(cpu_reg_psw, t, 0xff);
        ctx->base.is_jmp = DISAS_EXIT;
        return true;
    } else if ((op & 0777700) == 0000100) { /* JMP */
        gen_jmp_addr(ctx, cpu_reg_pc, op & 0000077);
    } else if ((op & 0777770) == 0000200) { /* RTS */
        reg = op & 0000007;
        if (reg != 7) {
            tcg_gen_andi_tl(cpu_reg_pc, cpu_regs[reg], 0xffff);
            pop(cpu_regs[reg]);
        } else {
            pop(cpu_reg_pc);
        }
    } else {
        addr = (int8_t)(op & 0000377) * 2;
        switch (op & 0777400) {
        case 0000400: /* BR */
            gen_goto_tb(ctx, 0, ctx->pc + addr);
            break;
        case 0001000: /* BNE */
            gen_branch_mask(ctx, addr, CC_PSW_MASK_Z, TCG_COND_NE);
            break;
        case 0001400: /* BEQ */
            gen_branch_mask(ctx, addr, CC_PSW_MASK_Z, TCG_COND_EQ);
            break;
        case 0002000: /* BGE */
            gen_branch_nv(ctx, addr, TCG_COND_NE);
            break;
        case 0002400: /* BLT */
            gen_branch_nv(ctx, addr, TCG_COND_EQ);
            break;
        case 0003000: /* BGT */
            gen_branch_znv(ctx, addr, TCG_COND_NE);
            break;
        case 0003400: /* BLE */
            gen_branch_znv(ctx, addr, TCG_COND_EQ);
            break;
        case 0100000: /* BPL */
            gen_branch_mask(ctx, addr, CC_PSW_MASK_N, TCG_COND_NE);
            break;
        case 0100400: /* BMI */
            gen_branch_mask(ctx, addr, CC_PSW_MASK_N, TCG_COND_EQ);
            break;
        case 0101000: /* BHI */
            gen_branch_mask(ctx, addr, CC_PSW_MASK_C | CC_PSW_MASK_Z,
                            TCG_COND_NE);
            break;
        case 0101400: /* BLOS */
            gen_branch_mask(ctx, addr, CC_PSW_MASK_C | CC_PSW_MASK_Z,
                            TCG_COND_EQ);
            break;
        case 0102000: /* BVC */
            gen_branch_mask(ctx, addr, CC_PSW_MASK_V, TCG_COND_NE);
            break;
        case 0102400: /* BVS */
            gen_branch_mask(ctx, addr, CC_PSW_MASK_V, TCG_COND_EQ);
            break;
        case 0103000: /* BCC */
            gen_branch_mask(ctx, addr, CC_PSW_MASK_C, TCG_COND_NE);
            break;
        case 0103400: /* BCS */
            gen_branch_mask(ctx, addr, CC_PSW_MASK_C, TCG_COND_EQ);
            break;
        default:
            return false;
        }
        return true;
    }

    if (ctx->base.is_jmp == DISAS_NEXT) {
        ctx->base.is_jmp = DISAS_JUMP;
    }
    return true;
}

static bool decode_nop(DisasContext *ctx, int op)
{
    if (op == 0000240 || op == 0000260) {
        return true;
    } else if ((op & 0777760) == 0000240) {
        tcg_gen_andi_tl(cpu_reg_psw, cpu_reg_psw, ~(op & 017));
    } else if ((op & 0777760) == 0000260) {
        tcg_gen_ori_tl(cpu_reg_psw, cpu_reg_psw, (op & 017));
    } else if (op == 0000005) { /* RESET */
        tcg_gen_andi_tl(cpu_reg_psw, cpu_reg_psw, ~CC_PSW_MASK_Z);
    } else {
        return false;
    }
    return true;
}

/*
 * Instruction times in clock cycles, after Yu. A. Zal'tsman, "Personal'nyj
 * komp'yuter BK" No. 1, 1995, as refined against real hardware by BKBTL.
 * They include the wait states the display adds to memory accesses.
 */
#define CYCLES_BASE     12
#define CYCLES_BRANCH   16
#define CYCLES_SOB      20
#define CYCLES_RTS      32
#define CYCLES_RTI      40
#define CYCLES_TRAP     68
#define CYCLES_ILLEGAL  144
#define CYCLES_WAIT     1140

static const uint8_t cycles_src[8]     = { 0, 12, 12, 24, 16, 20, 20, 28 };
static const uint8_t cycles_dst[8]     = { 0, 20, 20, 32, 24, 36, 32, 40 };
static const uint8_t cycles_mem_dst[8] = { 4, 16, 20, 24, 16, 24, 24, 36 };
static const uint8_t cycles_test[8]    = { 0, 20, 20, 32, 20, 28, 32, 36 };
static const uint8_t cycles_jsr[8]     = { 0, 32, 32, 40, 32, 40, 40, 48 };

static int insn_cycles(uint16_t op)
{
    int ms = (op >> 9) & 7;
    int md = (op >> 3) & 7;

    switch (op >> 12) {
    case 001: case 004: case 005: case 006:
    case 011: case 014: case 015: case 016: /* MOV, BIC, BIS, ADD, SUB */
        return CYCLES_BASE + cycles_src[ms] +
               (ms ? cycles_mem_dst[md] : cycles_dst[md]);
    case 002: case 003: case 012: case 013: /* CMP, BIT */
        return CYCLES_BASE + cycles_src[ms] +
               (ms ? cycles_src[md] : cycles_test[md]);
    }

    switch (op & 0177000) {
    case 0004000: /* JSR */
        return md ? cycles_jsr[md] : CYCLES_TRAP;
    case 0074000: /* XOR */
        return CYCLES_BASE + cycles_test[md];
    case 0077000:
        return CYCLES_SOB;
    case 0104000: /* EMT, TRAP */
        return CYCLES_TRAP;
    }

    if ((op & 0074000) == 0 && (op & 0103400) != 0) {
        return CYCLES_BRANCH;
    }

    switch (op & 0077700) {
    case 0005700: /* TST(B) */
        return CYCLES_BASE + cycles_src[md];
    case 0006400: /* MTPS */
        if (!(op & 0100000)) {
            break;
        }
        /* fall through */
    case 0005000 ... 0005600:
    case 0006000 ... 0006300:
    case 0006700:
        return CYCLES_BASE + cycles_dst[md];
    }

    switch (op) {
    case 0000001: /* WAIT */
    case 0000005: /* RESET */
        return CYCLES_WAIT;
    case 0000002: /* RTI */
    case 0000006: /* RTT */
        return CYCLES_RTI;
    case 0000003: /* BPT */
    case 0000004: /* IOT */
        return CYCLES_TRAP;
    case 0000100 ... 0000177: /* JMP */
        return md ? cycles_test[md] : CYCLES_TRAP;
    case 0000200 ... 0000207: /* RTS */
        return CYCLES_RTS;
    case 0000240 ... 0000277: /* NOP, condition codes */
        return CYCLES_BASE;
    case 0000300 ... 0000377: /* SWAB */
        return CYCLES_BASE + cycles_dst[md];
    }
    return CYCLES_ILLEGAL;
}

static void decode_opc(DisasContext *ctx)
{
    int op, i;

    for (i = 0; i < 2; i++) {
        ctx->args[i].addition = 0;
        ctx->args[i].addr = -1;
        ctx->args[i].code_size = 0;
    }
    ctx->byte_full_store = false;

    op = ctx->opcode;
    ctx->pc += 2;

    if (!decode_dop(ctx, op) &&
        !decode_sop(ctx, op) &&
        !decode_branch(ctx, op) &&
        !decode_nop(ctx, op)) {
        gen_reserved_insn(ctx);
        return;
    }

    ctx->pc += ctx->args[0].code_size;
    ctx->pc += ctx->args[1].code_size;
}

static void k1801vm1_tr_init_disas_context(DisasContextBase *dcbase,
                                           CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);

    ctx->env = cpu_env(cs);
    ctx->pc = ctx->base.pc_first;
}

static void k1801vm1_tr_tb_start(DisasContextBase *dcbase, CPUState *cs)
{
    gen_helper_sync_clock(tcg_env);
}

static void k1801vm1_tr_insn_start(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);

    ctx->opcode = code_lduw(ctx, ctx->pc);
    ctx->cycles = insn_cycles(ctx->opcode);
    tcg_gen_insn_start(ctx->pc, ctx->cycles, 0);
}

static void k1801vm1_tr_translate_insn(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);

    tcg_gen_addi_i64(cpu_cycles, cpu_cycles, ctx->cycles);
    decode_opc(ctx);
    ctx->base.pc_next = ctx->pc;

    if (ctx->base.is_jmp == DISAS_NEXT) {
        vaddr page_first = ctx->base.pc_first & TARGET_PAGE_MASK;

        if (ctx->pc - page_first >= TARGET_PAGE_SIZE - MAX_INSN_SIZE ||
            ctx->pc > 0xffff - MAX_INSN_SIZE) {
            ctx->base.is_jmp = DISAS_TOO_MANY;
        }
    }
}

static void k1801vm1_tr_tb_stop(DisasContextBase *dcbase, CPUState *cs)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);

    switch (ctx->base.is_jmp) {
    case DISAS_NORETURN:
        break;
    case DISAS_NEXT:
    case DISAS_TOO_MANY:
        gen_goto_tb(ctx, 0, ctx->pc);
        break;
    case DISAS_JUMP:
        tcg_gen_lookup_and_goto_ptr();
        break;
    case DISAS_EXIT_NEXT:
        tcg_gen_movi_tl(cpu_reg_pc, ctx->pc & 0xffff);
        /* fall through */
    case DISAS_EXIT:
        tcg_gen_exit_tb(NULL, 0);
        break;
    default:
        g_assert_not_reached();
    }
}

static const TranslatorOps k1801vm1_tr_ops = {
    .init_disas_context = k1801vm1_tr_init_disas_context,
    .tb_start           = k1801vm1_tr_tb_start,
    .insn_start         = k1801vm1_tr_insn_start,
    .translate_insn     = k1801vm1_tr_translate_insn,
    .tb_stop            = k1801vm1_tr_tb_stop,
};

void k1801vm1_translate_code(CPUState *cs, TranslationBlock *tb,
                             int *max_insns, vaddr pc, void *host_pc)
{
    DisasContext ctx = { };

    translator_loop(cs, tb, max_insns, pc, host_pc, &k1801vm1_tr_ops,
                    &ctx.base, TCG_TYPE_VA);
}
