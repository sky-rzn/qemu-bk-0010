/*
 * QEMU K1801VM1 CPU
 *
 * Copyright (c) 2019 Alexandr Ivanov
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "migration/vmstate.h"

const VMStateDescription vmstate_k1801vm1_cpu = {
    .name = "cpu",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(env.regs, K1801VM1CPU, K1801VM1_REG_NUM),
        VMSTATE_UINT32(env.psw, K1801VM1CPU),
        VMSTATE_END_OF_LIST()
    }
};
