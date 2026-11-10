/*
 * K1801VM1 emulation
 *
 * Copyright (c) 2019 Alexandr Ivanov
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef K1801VM1_CPU_H
#define K1801VM1_CPU_H

#include "cpu-qom.h"
#include "exec/cpu-common.h"
#include "exec/cpu-interrupt.h"

#ifdef CONFIG_USER_ONLY
#error "K1801VM1 does not support user mode"
#endif

#define CPU_RESOLVING_TYPE TYPE_K1801VM1_CPU

#define K1801VM1_REG_NUM        8
#define K1801VM1_REG_SP         6
#define K1801VM1_REG_PC         7

#define K1801VM1_PSW_C          (1 << 0)
#define K1801VM1_PSW_V          (1 << 1)
#define K1801VM1_PSW_Z          (1 << 2)
#define K1801VM1_PSW_N          (1 << 3)
#define K1801VM1_PSW_T          (1 << 4)
#define K1801VM1_PSW_P          (1 << 7)

#define K1801VM1_RESET_PC       0100000

#define K1801VM1_CLOCK_HZ       3000000

/* Trap and interrupt vectors; the exception index is the vector itself. */
#define K1801VM1_VEC_BUS_ERROR  0004
#define K1801VM1_VEC_RESERVED   0010
#define K1801VM1_VEC_IRQ        0060

typedef struct CPUArchState {
    uint32_t regs[K1801VM1_REG_NUM];
    uint32_t psw;

    /* Registers at the start of the instruction, to restart it after I/O. */
    uint32_t saved_regs[K1801VM1_REG_NUM];

    /* Fields after this point are preserved across CPU reset. */
    struct {} end_reset_fields;

    uint64_t cycles;
} CPUK1801VM1State;

struct ArchCPU {
    CPUState parent_obj;

    CPUK1801VM1State env;
};

struct K1801VM1CPUClass {
    CPUClass parent_class;

    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

extern const VMStateDescription vmstate_k1801vm1_cpu;

void k1801vm1_cpu_do_interrupt(CPUState *cs);
bool k1801vm1_cpu_exec_interrupt(CPUState *cs, int interrupt_request);
bool k1801vm1_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                           MMUAccessType access_type, int mmu_idx,
                           bool probe, uintptr_t retaddr);
bool k1801vm1_cpu_translate_for_debug(CPUState *cs, vaddr addr,
                                      TranslateForDebugResult *result);

void k1801vm1_translate_init(void);
void k1801vm1_translate_code(CPUState *cs, TranslationBlock *tb,
                             int *max_insns, vaddr pc, void *host_pc);

/* disas.c */
typedef uint16_t (*K1801VM1FetchFn)(void *opaque, uint16_t addr);
int k1801vm1_disasm(uint16_t pc, K1801VM1FetchFn fetch, void *opaque,
                    char *buf, size_t size);
int k1801vm1_print_insn(bfd_vma addr, disassemble_info *info);

#endif /* K1801VM1_CPU_H */
