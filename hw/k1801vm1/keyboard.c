/*
 * BK-0010 keyboard
 *
 * Copyright (c) 2019 Alexandr Ivanov
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qemu/bitmap.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "ui/input.h"
#include "bk-hw.h"

OBJECT_DECLARE_SIMPLE_TYPE(BkKeyboardState, BK_KEYBOARD)

#define IRQ_DISABLED_MASK   (1 << 6)
#define HAS_DATA_MASK       (1 << 7)

#define MOD_SHIFT   0x01
#define MOD_E0      0x02

/* Indexed by PC scancode set 1, +0x80 when Shift is on. */
static const uint8_t kbd_table[] = {
/*        0     1     2     3     4     5     6     7 */
/*        8     9     a     b     c     d     e     f */
/*00*/  0000, 0014, 0061, 0062, 0063, 0064, 0065, 0066,
        0067, 0070, 0071, 0060, 0055, 0075, 0030, 0000,
/*10*/  0161, 0167, 0145, 0162, 0164, 0171, 0165, 0151,
        0157, 0160, 0133, 0135, 0012, 0000, 0141, 0163,
/*20*/  0144, 0146, 0147, 0150, 0152, 0153, 0154, 0073,
        0047, 0000, 0000, 0134, 0172, 0170, 0143, 0166,
/*30*/  0142, 0156, 0155, 0054, 0056, 0057, 0000, 0000,
        0000, 0040, 0000, 0016, 0017, 0000, 0000, 0000,
/*40*/  0000, 0000, 0000, 0000, 0000, 0000, 0000, 0000,
        0032, 0000, 0000, 0010, 0000, 0031, 0000, 0000,
/*50*/  0033, 0000, 0000, 0000, 0000, 0000, 0000, 0000,
        0000, 0000, 0000, 0000, 0000, 0000, 0000, 0000,
/*60*/  0000, 0000, 0000, 0000, 0000, 0000, 0000, 0000,
        0000, 0000, 0000, 0000, 0000, 0000, 0000, 0000,
/*70*/  0000, 0000, 0000, 0000, 0000, 0000, 0000, 0000,
        0000, 0000, 0000, 0000, 0000, 0000, 0000, 0000,
/*80*/  0000, 0000, 0041, 0100, 0043, 0044, 0045, 0136,
        0046, 0052, 0050, 0051, 0137, 0053, 0000, 0000,
/*90*/  0121, 0127, 0105, 0122, 0124, 0131, 0125, 0111,
        0117, 0120, 0173, 0175, 0000, 0000, 0101, 0123,
/*a0*/  0104, 0106, 0107, 0110, 0112, 0113, 0114, 0072,
        0042, 0000, 0000, 0174, 0132, 0130, 0103, 0126,
/*b0*/  0102, 0116, 0115, 0074, 0076, 0077, 0000, 0000,
        0000, 0000, 0000, 0016, 0017, 0000, 0000, 0000,
};

struct BkKeyboardState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    QemuInputHandlerState *hs;
    qemu_irq irq;
    qemu_irq key_down;

    DECLARE_BITMAP(pressed, Q_KEY_CODE__MAX);
    uint16_t state_reg;
    uint16_t data_reg;
    uint16_t mod;
};

static void bk_keyboard_update_irq(BkKeyboardState *s)
{
    qemu_set_irq(s->irq, (s->state_reg & HAS_DATA_MASK) &&
                         !(s->state_reg & IRQ_DISABLED_MASK));
}

static uint64_t bk_keyboard_read(void *opaque, hwaddr addr, unsigned size)
{
    BkKeyboardState *s = opaque;
    uint16_t val;

    if (addr < 2) {
        val = s->state_reg;
    } else {
        val = s->data_reg;
        s->state_reg &= ~HAS_DATA_MASK;
        bk_keyboard_update_irq(s);
    }
    return (addr & 1) ? val >> 8 : val;
}

static void bk_keyboard_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    BkKeyboardState *s = opaque;

    if (addr == 0) {
        s->state_reg = (s->state_reg & ~IRQ_DISABLED_MASK) |
                       (value & IRQ_DISABLED_MASK);
        bk_keyboard_update_irq(s);
    } else if (addr >= 2) {
        qemu_log_mask(LOG_GUEST_ERROR, "bk-keyboard: write to data register\n");
    }
}

static const MemoryRegionOps bk_keyboard_ops = {
    .read = bk_keyboard_read,
    .write = bk_keyboard_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
};

static void bk_keyboard_put_scancode(BkKeyboardState *s, int code)
{
    bool mod_e0 = false;

    switch (code) {
    case 0x2a:
    case 0x36:
        s->mod |= MOD_SHIFT;
        return;
    case 0xaa:
    case 0xb6:
        s->mod &= ~MOD_SHIFT;
        return;
    case 0xe0:
        s->mod |= MOD_E0;
        return;
    }

    if (s->mod & MOD_E0) {
        s->mod &= ~MOD_E0;
        mod_e0 = true;
    }

    if ((s->state_reg & HAS_DATA_MASK) || code >= 0x80) {
        return;
    }

    if ((s->mod & MOD_SHIFT) && !mod_e0) {
        code += 0x80;
    }

    if (code >= ARRAY_SIZE(kbd_table) || kbd_table[code] == 0) {
        qemu_log_mask(LOG_UNIMP, "bk-keyboard: unmapped scancode 0x%x\n",
                      code);
        return;
    }

    s->data_reg = kbd_table[code];
    s->state_reg |= HAS_DATA_MASK;
    bk_keyboard_update_irq(s);
}

static void bk_keyboard_event(DeviceState *dev, QemuConsole *src,
                              QemuInputEvent *evt)
{
    BkKeyboardState *s = BK_KEYBOARD(dev);
    int qcode = qemu_input_linux_to_qcode(evt->key.key);
    int codes[16];
    int i, n;

    switch (qcode) {
    case Q_KEY_CODE_SHIFT:
    case Q_KEY_CODE_SHIFT_R:
    case Q_KEY_CODE_CTRL:
    case Q_KEY_CODE_CTRL_R:
    case Q_KEY_CODE_ALT:
    case Q_KEY_CODE_ALT_R:
        break;
    default:
        if (evt->key.down) {
            set_bit(qcode, s->pressed);
        } else {
            clear_bit(qcode, s->pressed);
        }
        qemu_set_irq(s->key_down,
                     !bitmap_empty(s->pressed, Q_KEY_CODE__MAX));
    }

    n = qemu_input_linux_to_scancode(evt->key.key, evt->key.down, codes);
    for (i = 0; i < n; i++) {
        bk_keyboard_put_scancode(s, codes[i]);
    }
}

static const QemuInputHandler bk_keyboard_handler = {
    .name  = "BK-0010 keyboard",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = bk_keyboard_event,
};

static void bk_keyboard_reset_hold(Object *obj, ResetType type)
{
    BkKeyboardState *s = BK_KEYBOARD(obj);

    s->state_reg = 0;
    s->data_reg = 0;
    s->mod = 0;
    bitmap_zero(s->pressed, Q_KEY_CODE__MAX);
    qemu_set_irq(s->key_down, 0);
}

static void bk_keyboard_realize(DeviceState *dev, Error **errp)
{
    BkKeyboardState *s = BK_KEYBOARD(dev);

    s->hs = qemu_input_handler_register(dev, &bk_keyboard_handler);
}

static void bk_keyboard_init(Object *obj)
{
    BkKeyboardState *s = BK_KEYBOARD(obj);

    memory_region_init_io(&s->iomem, obj, &bk_keyboard_ops, s,
                          TYPE_BK_KEYBOARD, BK_KEYBOARD_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_out_named(DEVICE(obj), &s->key_down, "key-down", 1);
}

static const VMStateDescription bk_keyboard_vmstate = {
    .name = TYPE_BK_KEYBOARD,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(state_reg, BkKeyboardState),
        VMSTATE_UINT16(data_reg, BkKeyboardState),
        VMSTATE_UINT16(mod, BkKeyboardState),
        VMSTATE_END_OF_LIST()
    }
};

static void bk_keyboard_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &bk_keyboard_vmstate;
    dc->realize = bk_keyboard_realize;
    rc->phases.hold = bk_keyboard_reset_hold;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo bk_keyboard_info = {
    .name          = TYPE_BK_KEYBOARD,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BkKeyboardState),
    .instance_init = bk_keyboard_init,
    .class_init    = bk_keyboard_class_init,
};

static void bk_keyboard_register_types(void)
{
    type_register_static(&bk_keyboard_info);
}

type_init(bk_keyboard_register_types)
