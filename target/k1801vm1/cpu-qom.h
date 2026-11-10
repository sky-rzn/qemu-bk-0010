/*
 * QEMU K1801VM1 CPU QOM header (target agnostic)
 *
 * Copyright (c) 2019 Alexandr Ivanov
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef TARGET_K1801VM1_CPU_QOM_H
#define TARGET_K1801VM1_CPU_QOM_H

#include "hw/core/cpu.h"

#define TYPE_K1801VM1_CPU "k1801vm1-cpu"

OBJECT_DECLARE_CPU_TYPE(K1801VM1CPU, K1801VM1CPUClass, K1801VM1_CPU)

#endif
