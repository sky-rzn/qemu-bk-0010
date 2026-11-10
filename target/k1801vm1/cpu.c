/*
 * QEMU K1801VM1 CPU
 *
 * Copyright (c) 2019 Alexandr Ivanov
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/qemu-print.h"
#include "cpu.h"
#include "disas/dis-asm.h"
#include "tcg/debug-assert.h"
#include "accel/tcg/cpu-ops.h"
#include "exec/translation-block.h"
#include "hw/core/sysemu-cpu-ops.h"

static void k1801vm1_cpu_set_pc(CPUState *cs, vaddr value)
{
    cpu_env(cs)->regs[K1801VM1_REG_PC] = value & 0xffff;
}

static vaddr k1801vm1_cpu_get_pc(CPUState *cs)
{
    return cpu_env(cs)->regs[K1801VM1_REG_PC] & 0xffff;
}

static bool k1801vm1_cpu_has_work(CPUState *cs)
{
    return cpu_test_interrupt(cs, CPU_INTERRUPT_HARD);
}

static int k1801vm1_cpu_mmu_index(CPUState *cs, bool ifetch)
{
    return 0;
}

static TCGTBCPUState k1801vm1_get_tb_cpu_state(CPUState *cs)
{
    return (TCGTBCPUState){
        .pc = cpu_env(cs)->regs[K1801VM1_REG_PC] & 0xffff,
    };
}

static void k1801vm1_cpu_synchronize_from_tb(CPUState *cs,
                                             const TranslationBlock *tb)
{
    tcg_debug_assert(!tcg_cflags_has(cs, CF_PCREL));
    cpu_env(cs)->regs[K1801VM1_REG_PC] = tb->pc;
}

static void k1801vm1_restore_state_to_opc(CPUState *cs,
                                          const TranslationBlock *tb,
                                          const uint64_t *data)
{
    CPUK1801VM1State *env = cpu_env(cs);
    int i;

    for (i = 0; i < K1801VM1_REG_NUM; i++) {
        if (data[2] & (1 << i)) {
            env->regs[i] = env->saved_regs[i];
        }
    }
    env->regs[K1801VM1_REG_PC] = data[0];
    /* The instruction will run again and charge its cycles again. */
    env->cycles -= data[1];
}

static vaddr k1801vm1_pointer_wrap(CPUState *cs, int mmu_idx,
                                   vaddr result, vaddr base)
{
    return (uint16_t)result;
}

static void k1801vm1_cpu_reset_hold(Object *obj, ResetType type)
{
    CPUState *cs = CPU(obj);
    K1801VM1CPUClass *mcc = K1801VM1_CPU_GET_CLASS(obj);
    CPUK1801VM1State *env = cpu_env(cs);

    if (mcc->parent_phases.hold) {
        mcc->parent_phases.hold(obj, type);
    }

    memset(env, 0, offsetof(CPUK1801VM1State, end_reset_fields));
    env->regs[K1801VM1_REG_PC] = K1801VM1_RESET_PC;
}

static void k1801vm1_cpu_disas_set_info(const CPUState *cs,
                                        disassemble_info *info)
{
    info->endian = BFD_ENDIAN_LITTLE;
    info->mach = bfd_arch_k1801vm1;
    info->print_insn = k1801vm1_print_insn;
}

static void k1801vm1_cpu_realizefn(DeviceState *dev, Error **errp)
{
    CPUState *cs = CPU(dev);
    K1801VM1CPUClass *mcc = K1801VM1_CPU_GET_CLASS(dev);
    Error *local_err = NULL;

    cpu_common_realize(cs, &local_err);
    if (local_err != NULL) {
        error_propagate(errp, local_err);
        return;
    }

    qemu_init_vcpu(cs);
    cpu_reset(cs);

    mcc->parent_realize(dev, errp);
}

static void k1801vm1_cpu_set_irq(void *opaque, int irq, int level)
{
    CPUState *cs = CPU(opaque);

    if (level) {
        cpu_interrupt(cs, CPU_INTERRUPT_HARD);
    } else {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
    }
}

static void k1801vm1_cpu_initfn(Object *obj)
{
    qdev_init_gpio_in(DEVICE(obj), k1801vm1_cpu_set_irq, 1);
}

static ObjectClass *k1801vm1_cpu_class_by_name(const char *cpu_model)
{
    return object_class_by_name(TYPE_K1801VM1_CPU);
}

static uint16_t k1801vm1_debug_fetch(void *opaque, uint16_t addr)
{
    uint8_t buf[2] = { 0, 0 };

    cpu_memory_rw_debug(CPU(opaque), addr, buf, sizeof(buf), false);
    return lduw_le_p(buf);
}

static void k1801vm1_cpu_dump_state(CPUState *cs, FILE *f, int flags)
{
    CPUK1801VM1State *env = cpu_env(cs);
    uint16_t pc = env->regs[K1801VM1_REG_PC];
    uint16_t sp = env->regs[K1801VM1_REG_SP];
    char insn[64];
    int i;

    k1801vm1_disasm(pc, k1801vm1_debug_fetch, cs, insn, sizeof(insn));

    qemu_fprintf(f, "%c%c%c%c  %04x: [%06o] ",
                 (env->psw & K1801VM1_PSW_N) ? 'N' : '_',
                 (env->psw & K1801VM1_PSW_Z) ? 'Z' : '_',
                 (env->psw & K1801VM1_PSW_V) ? 'V' : '_',
                 (env->psw & K1801VM1_PSW_C) ? 'C' : '_',
                 pc, k1801vm1_debug_fetch(cs, pc));

    for (i = 0; i < K1801VM1_REG_PC; i++) {
        qemu_fprintf(f, "r%d=%04x ", i, env->regs[i] & 0xffff);
    }

    qemu_fprintf(f, "r6@=%04x  %s\n", k1801vm1_debug_fetch(cs, sp), insn);
}

static const struct SysemuCPUOps k1801vm1_sysemu_ops = {
    .has_work = k1801vm1_cpu_has_work,
    .translate_for_debug = k1801vm1_cpu_translate_for_debug,
};

static const TCGCPUOps k1801vm1_tcg_ops = {
    .guest_default_memory_order = 0,
    .mttcg_supported = false,
    .initialize = k1801vm1_translate_init,
    .translate_code = k1801vm1_translate_code,
    .get_tb_cpu_state = k1801vm1_get_tb_cpu_state,
    .synchronize_from_tb = k1801vm1_cpu_synchronize_from_tb,
    .restore_state_to_opc = k1801vm1_restore_state_to_opc,
    .mmu_index = k1801vm1_cpu_mmu_index,
    .cpu_exec_interrupt = k1801vm1_cpu_exec_interrupt,
    .cpu_exec_reset = cpu_reset,
    .tlb_fill = k1801vm1_cpu_tlb_fill,
    .do_interrupt = k1801vm1_cpu_do_interrupt,
    .pointer_wrap = k1801vm1_pointer_wrap,
};

static void k1801vm1_cpu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    CPUClass *cc = CPU_CLASS(oc);
    K1801VM1CPUClass *mcc = K1801VM1_CPU_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    device_class_set_parent_realize(dc, k1801vm1_cpu_realizefn,
                                    &mcc->parent_realize);
    resettable_class_set_parent_phases(rc, NULL, k1801vm1_cpu_reset_hold, NULL,
                                       &mcc->parent_phases);

    cc->class_by_name = k1801vm1_cpu_class_by_name;
    cc->dump_state = k1801vm1_cpu_dump_state;
    cc->set_pc = k1801vm1_cpu_set_pc;
    cc->get_pc = k1801vm1_cpu_get_pc;
    dc->vmsd = &vmstate_k1801vm1_cpu;
    cc->sysemu_ops = &k1801vm1_sysemu_ops;
    cc->disas_set_info = k1801vm1_cpu_disas_set_info;
    cc->tcg_ops = &k1801vm1_tcg_ops;
}

static const TypeInfo k1801vm1_cpu_type_infos[] = {
    {
        .name = TYPE_K1801VM1_CPU,
        .parent = TYPE_CPU,
        .instance_size = sizeof(K1801VM1CPU),
        .instance_align = __alignof(K1801VM1CPU),
        .instance_init = k1801vm1_cpu_initfn,
        .class_size = sizeof(K1801VM1CPUClass),
        .class_init = k1801vm1_cpu_class_init,
    },
};

DEFINE_TYPES(k1801vm1_cpu_type_infos)
