/*
 * BK-0010 display
 *
 * Copyright (c) 2019 Alexandr Ivanov
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "ui/console.h"
#include "bk-hw.h"

OBJECT_DECLARE_SIMPLE_TYPE(BkDisplayState, BK_DISPLAY)

#define WIDTH           512
#define HEIGHT          256
#define BYTES_PER_LINE  (WIDTH / 8)

#define SCROLL_DEFAULT  0330
#define SCROLL_FULL     01000
#define SCROLL_MASK     (SCROLL_FULL | 0377)

struct BkDisplayState {
    SysBusDevice parent_obj;

    QemuConsole *con;
    MemoryRegion vram;
    MemoryRegion scroll_reg;

    uint16_t scroll;
};

static uint64_t bk_display_scroll_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    BkDisplayState *s = opaque;

    return (addr & 1) ? s->scroll >> 8 : s->scroll;
}

static void bk_display_scroll_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned size)
{
    BkDisplayState *s = opaque;

    if (size == 2) {
        s->scroll = value;
    } else if (addr & 1) {
        s->scroll = (s->scroll & 0x00ff) | ((value & 0xff) << 8);
    } else {
        s->scroll = (s->scroll & 0xff00) | (value & 0xff);
    }
    s->scroll &= SCROLL_MASK;
}

static const MemoryRegionOps bk_display_scroll_ops = {
    .read = bk_display_scroll_read,
    .write = bk_display_scroll_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
};

static bool bk_display_update(void *opaque)
{
    BkDisplayState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    const uint8_t *vram = memory_region_get_ram_ptr(&s->vram);
    int lines = (s->scroll & SCROLL_FULL) ? HEIGHT : HEIGHT / 4;
    int x, y, bit;

    for (y = 0; y < HEIGHT; y++) {
        uint32_t *d = (uint32_t *)(surface_data(surface) +
                                   y * surface_stride(surface));
        const uint8_t *src = vram +
            ((y + (s->scroll & 0xff) - SCROLL_DEFAULT) & 0xff) * BYTES_PER_LINE;

        for (x = 0; x < BYTES_PER_LINE; x++) {
            uint8_t pixels = y < lines ? src[x] : 0;

            for (bit = 0; bit < 8; bit++) {
                *d++ = (pixels & (1 << bit)) ? 0xffffffff : 0xff000000;
            }
        }
    }

    qemu_console_update(s->con, 0, 0, WIDTH, HEIGHT);
    return true;
}

static void bk_display_invalidate(void *opaque)
{
}

static const GraphicHwOps bk_display_gfx_ops = {
    .invalidate = bk_display_invalidate,
    .gfx_update = bk_display_update,
};

static void bk_display_reset_hold(Object *obj, ResetType type)
{
    BK_DISPLAY(obj)->scroll = SCROLL_FULL | SCROLL_DEFAULT;
}

static void bk_display_realize(DeviceState *dev, Error **errp)
{
    BkDisplayState *s = BK_DISPLAY(dev);

    if (!memory_region_init_ram(&s->vram, OBJECT(dev), "bk0010.vram",
                                BK_VIDEO_SIZE, errp)) {
        return;
    }
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->vram);

    memory_region_init_io(&s->scroll_reg, OBJECT(dev), &bk_display_scroll_ops,
                          s, "bk0010.scroll", BK_SCROLL_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->scroll_reg);

    s->con = qemu_graphic_console_create(dev, 0, &bk_display_gfx_ops, s);
    qemu_console_resize(s->con, WIDTH, HEIGHT);
}

static const VMStateDescription bk_display_vmstate = {
    .name = TYPE_BK_DISPLAY,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(scroll, BkDisplayState),
        VMSTATE_END_OF_LIST()
    }
};

static void bk_display_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = bk_display_realize;
    dc->vmsd = &bk_display_vmstate;
    rc->phases.hold = bk_display_reset_hold;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo bk_display_type_info = {
    .name          = TYPE_BK_DISPLAY,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BkDisplayState),
    .class_init    = bk_display_class_init,
};

static void bk_display_register_types(void)
{
    type_register_static(&bk_display_type_info);
}

type_init(bk_display_register_types)
