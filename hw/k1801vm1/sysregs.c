/*
 * BK-0010 system registers
 *
 * Copyright (c) 2019 Alexandr Ivanov
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "target/k1801vm1/cpu.h"
#include "bk-hw.h"

OBJECT_DECLARE_SIMPLE_TYPE(BkSysregsState, BK_SYSREGS)

#define REG_IO_PORT     0114
#define REG_SYSTEM_PORT 0116
#define REG_TIMER_LOAD  0106
#define REG_TIMER_COUNT 0110
#define REG_TIMER_CTRL  0112

#define TIMER_CTRL_STOP     (1 << 0)
#define TIMER_CTRL_WRAP     (1 << 1)
#define TIMER_CTRL_EXPEN    (1 << 2)
#define TIMER_CTRL_ONESHOT  (1 << 3)
#define TIMER_CTRL_RUN      (1 << 4)
#define TIMER_CTRL_DIV_SHIFT 5
#define TIMER_CTRL_EXPIRED  (1 << 7)
#define TIMER_CTRL_UNUSED   0177400

#define SYSTEM_PORT_SOUND   (1 << 6)

/* The timer counts CPU clock cycles divided by 128. */
#define TIMER_CLOCK_SHIFT   7

struct BkSysregsState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    K1801VM1CPU *cpu;
    qemu_irq sound;

    uint16_t timer_load;
    uint16_t timer_count;
    uint16_t timer_ctrl;
    uint16_t timer_div;
    uint64_t timer_cycles;
};

static void bk_timer_advance(BkSysregsState *s, uint64_t ticks)
{
    static const unsigned prescaler[4] = { 1, 16, 4, 64 };
    unsigned div = prescaler[(s->timer_ctrl >> TIMER_CTRL_DIV_SHIFT) & 3];
    uint64_t total, dec, period;

    if (s->timer_ctrl & TIMER_CTRL_STOP) {
        s->timer_count = s->timer_load;
        return;
    }
    if (!(s->timer_ctrl & TIMER_CTRL_RUN)) {
        return;
    }

    total = s->timer_div + ticks;
    dec = total / div;
    s->timer_div = total % div;

    if (s->timer_ctrl & TIMER_CTRL_WRAP) {
        s->timer_count -= dec;
        return;
    }

    /* The counter reloads on reaching zero, so a count of zero is 0x10000. */
    period = s->timer_count ?: 0x10000;
    if (dec < period) {
        s->timer_count -= dec;
        return;
    }
    dec -= period;
    s->timer_count = s->timer_load;
    if (s->timer_ctrl & TIMER_CTRL_EXPEN) {
        s->timer_ctrl |= TIMER_CTRL_EXPIRED;
    }
    if (s->timer_ctrl & TIMER_CTRL_ONESHOT) {
        s->timer_ctrl &= ~TIMER_CTRL_RUN;
        return;
    }

    s->timer_count -= dec % (s->timer_load ?: 0x10000);
}

static void bk_timer_update(BkSysregsState *s)
{
    uint64_t ticks = (s->cpu->env.cycles - s->timer_cycles) >>
                     TIMER_CLOCK_SHIFT;

    s->timer_cycles += ticks << TIMER_CLOCK_SHIFT;
    if (ticks) {
        bk_timer_advance(s, ticks);
    }
}

static uint16_t merge_byte(uint16_t old, hwaddr addr, uint64_t value,
                           unsigned size)
{
    if (size == 2) {
        return value;
    }
    if (addr & 1) {
        return (old & 0x00ff) | ((value & 0xff) << 8);
    }
    return (old & 0xff00) | (value & 0xff);
}

static uint64_t bk_sysregs_read(void *opaque, hwaddr addr, unsigned size)
{
    BkSysregsState *s = opaque;
    uint16_t val;

    switch (addr & ~1) {
    case REG_TIMER_LOAD:
        val = s->timer_load;
        break;
    case REG_TIMER_COUNT:
        bk_timer_update(s);
        val = s->timer_count;
        break;
    case REG_TIMER_CTRL:
        bk_timer_update(s);
        val = s->timer_ctrl;
        break;
    case REG_IO_PORT:
        return 0;
    case REG_SYSTEM_PORT:
        return size == 1 ? 0x80 : 0x8080;
    default:
        qemu_log_mask(LOG_UNIMP, "bk-sysregs: read of %06o (size %u)\n",
                      (unsigned)(BK_SYSREGS_BASE + addr), size);
        return 0;
    }
    return size == 1 ? (val >> ((addr & 1) * 8)) & 0xff : val;
}

static void bk_sysregs_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    BkSysregsState *s = opaque;

    switch (addr & ~1) {
    case REG_TIMER_LOAD:
        bk_timer_update(s);
        s->timer_load = merge_byte(s->timer_load, addr, value, size);
        return;
    case REG_TIMER_COUNT:
        return;
    case REG_TIMER_CTRL:
        bk_timer_update(s);
        s->timer_ctrl = TIMER_CTRL_UNUSED |
                        merge_byte(s->timer_ctrl, addr, value, size);
        s->timer_count = s->timer_load;
        s->timer_div = 0;
        return;
    case REG_SYSTEM_PORT:
        if (size == 1 && (addr & 1)) {
            return;
        }
        qemu_set_irq(s->sound, !!(value & SYSTEM_PORT_SOUND));
        return;
    case REG_IO_PORT:
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
    qdev_init_gpio_out_named(DEVICE(obj), &s->sound, "sound", 1);
}

static void bk_sysregs_reset_hold(Object *obj, ResetType type)
{
    BkSysregsState *s = BK_SYSREGS(obj);

    s->timer_load = 011000;
    s->timer_count = 0177777;
    s->timer_ctrl = TIMER_CTRL_UNUSED;
    s->timer_div = 0;
    s->timer_cycles = s->cpu ? s->cpu->env.cycles : 0;
}

static const VMStateDescription vmstate_bk_sysregs = {
    .name = TYPE_BK_SYSREGS,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(timer_load, BkSysregsState),
        VMSTATE_UINT16(timer_count, BkSysregsState),
        VMSTATE_UINT16(timer_ctrl, BkSysregsState),
        VMSTATE_UINT16(timer_div, BkSysregsState),
        VMSTATE_UINT64(timer_cycles, BkSysregsState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property bk_sysregs_properties[] = {
    DEFINE_PROP_LINK("cpu", BkSysregsState, cpu, TYPE_K1801VM1_CPU,
                     K1801VM1CPU *),
};

static void bk_sysregs_realize(DeviceState *dev, Error **errp)
{
    BkSysregsState *s = BK_SYSREGS(dev);

    if (!s->cpu) {
        error_setg(errp, "bk-sysregs: 'cpu' link not set");
    }
}

static void bk_sysregs_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = bk_sysregs_realize;
    dc->vmsd = &vmstate_bk_sysregs;
    rc->phases.hold = bk_sysregs_reset_hold;
    device_class_set_props(dc, bk_sysregs_properties);
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
