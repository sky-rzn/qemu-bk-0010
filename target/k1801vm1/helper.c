/*
 * K1801VM1 helpers
 *
 * Copyright (c) 2019 Alexandr Ivanov
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "accel/tcg/cpu-ldst.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/cputlb.h"
#include "exec/page-protection.h"
#include "exec/target_page.h"
#include "exec/helper-proto.h"
#include "qemu/plugin.h"

bool k1801vm1_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                           MMUAccessType access_type, int mmu_idx,
                           bool probe, uintptr_t retaddr)
{
    address &= TARGET_PAGE_MASK;
    tlb_set_page(cs, address, address, PAGE_READ | PAGE_WRITE | PAGE_EXEC,
                 mmu_idx, TARGET_PAGE_SIZE);
    return true;
}

bool k1801vm1_cpu_translate_for_debug(CPUState *cs, vaddr addr,
                                      TranslateForDebugResult *result)
{
    *result = (TranslateForDebugResult) {
        .physaddr = addr,
        .lg_page_size = TARGET_PAGE_BITS,
        .attrs.debug = 1,
    };
    return true;
}

static void push(CPUK1801VM1State *env, uint16_t val)
{
    env->regs[K1801VM1_REG_SP] = (env->regs[K1801VM1_REG_SP] - 2) & 0xffff;
    cpu_stw_le_mmuidx_ra(env, env->regs[K1801VM1_REG_SP], val, 0, 0);
}

void k1801vm1_cpu_do_interrupt(CPUState *cs)
{
    CPUK1801VM1State *env = cpu_env(cs);
    int vector = cs->exception_index;
    uint16_t pc = env->regs[K1801VM1_REG_PC];

    push(env, env->psw);
    push(env, pc);

    env->regs[K1801VM1_REG_PC] = cpu_lduw_le_mmuidx_ra(env, vector, 0, 0);
    env->psw = cpu_lduw_le_mmuidx_ra(env, vector + 2, 0, 0) & 0xff;

    cs->exception_index = -1;

    qemu_plugin_vcpu_interrupt_cb(cs, pc);
}

bool k1801vm1_cpu_exec_interrupt(CPUState *cs, int interrupt_request)
{
    CPUK1801VM1State *env = cpu_env(cs);

    if ((interrupt_request & CPU_INTERRUPT_HARD) &&
        !(env->psw & K1801VM1_PSW_P)) {
        /* The bus acknowledge clears the request on the device side. */
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
        cs->exception_index = K1801VM1_VEC_IRQ;
        k1801vm1_cpu_do_interrupt(cs);
        return true;
    }
    return false;
}

void HELPER(raise_exception)(CPUK1801VM1State *env, uint32_t vector)
{
    CPUState *cs = env_cpu(env);

    cs->exception_index = vector;
    cpu_loop_exit(cs);
}

void HELPER(wait)(CPUK1801VM1State *env)
{
    CPUState *cs = env_cpu(env);

    cs->halted = 1;
    cs->exception_index = EXCP_HLT;
    cpu_loop_exit(cs);
}
