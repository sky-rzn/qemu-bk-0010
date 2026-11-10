/*
 * BK-0010 system registers
 *
 * Copyright (c) 2019 Alexandr Ivanov
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "bk-hw.h"

OBJECT_DECLARE_SIMPLE_TYPE(BkSysregsState, BK_SYSREGS)

#define REG_IO_PORT     0114
#define REG_SYSTEM_PORT 0116

struct BkSysregsState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
};

static uint64_t bk_sysregs_read(void *opaque, hwaddr addr, unsigned size)
{
    switch (addr & ~1) {
    case REG_IO_PORT:
        return 0;
    case REG_SYSTEM_PORT:
        return size == 1 ? 0x80 : 0x8080;
    }
    qemu_log_mask(LOG_UNIMP, "bk-sysregs: read of %06o (size %u)\n",
                  (unsigned)(BK_SYSREGS_BASE + addr), size);
    return 0;
}

static void bk_sysregs_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    switch (addr & ~1) {
    case REG_IO_PORT:
    case REG_SYSTEM_PORT:
        return;
    }
    qemu_log_mask(LOG_UNIMP, "bk-sysregs: write of %06o to %06o (size %u)\n",
                  (unsigned)value, (unsigned)(BK_SYSREGS_BASE + addr), size);
}

static const MemoryRegionOps bk_sysregs_ops = {
    .read = bk_sysregs_read,
    .write = bk_sysregs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
};

static void bk_sysregs_init(Object *obj)
{
    BkSysregsState *s = BK_SYSREGS(obj);

    memory_region_init_io(&s->iomem, obj, &bk_sysregs_ops, s,
                          TYPE_BK_SYSREGS, BK_SYSREGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void bk_sysregs_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo bk_sysregs_info = {
    .name          = TYPE_BK_SYSREGS,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BkSysregsState),
    .instance_init = bk_sysregs_init,
    .class_init    = bk_sysregs_class_init,
};

static void bk_sysregs_register_types(void)
{
    type_register_static(&bk_sysregs_info);
}

type_init(bk_sysregs_register_types)
