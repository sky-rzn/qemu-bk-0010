/*
 * QEMU/bk-0010 emulation
 *
 * Copyright (c) 2019 Alexandr Ivanov
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qemu/cutils.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/sysbus.h"
#include "system/address-spaces.h"
#include "target/k1801vm1/cpu.h"
#include "bk-hw.h"

#define MONITOR_ROM_FILENAME    "bk0010-monit10.rom"
#define BASIC_ROM_FILENAME      "bk0010-basic10.rom"

static void bk0010_load_rom(const char *name, hwaddr base, uint64_t size)
{
    g_autofree char *filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, name);

    if (!filename) {
        error_report("Could not find ROM image '%s'", name);
        exit(1);
    }
    if (load_image_targphys(filename, base, size, NULL) < 0) {
        error_report("Could not load ROM image '%s'", filename);
        exit(1);
    }
}

static void bk0010_init(MachineState *machine)
{
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *monitor_rom = g_new(MemoryRegion, 1);
    MemoryRegion *basic_rom = g_new(MemoryRegion, 1);
    K1801VM1CPU *cpu;
    DeviceState *dev;

    if (machine->ram_size != mc->default_ram_size) {
        g_autofree char *sz = size_to_str(mc->default_ram_size);

        error_report("Invalid RAM size, should be %s", sz);
        exit(1);
    }

    cpu = K1801VM1_CPU(cpu_create(machine->cpu_type));

    memory_region_add_subregion(sysmem, BK_RAM_BASE, machine->ram);

    memory_region_init_rom(monitor_rom, NULL, "bk0010.monitor-rom",
                           BK_MONITOR_ROM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, BK_MONITOR_ROM_BASE, monitor_rom);

    memory_region_init_rom(basic_rom, NULL, "bk0010.basic-rom",
                           BK_BASIC_ROM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, BK_BASIC_ROM_BASE, basic_rom);

    dev = qdev_new(TYPE_BK_SYSREGS);
    object_property_set_link(OBJECT(dev), "cpu", OBJECT(cpu), &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(dev), 0, BK_SYSREGS_BASE, 1);

    dev = qdev_new(TYPE_BK_DISPLAY);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, BK_VIDEO_BASE);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(dev), 1, BK_SCROLL_BASE, 2);

    dev = qdev_new(TYPE_BK_KEYBOARD);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(dev), 0, BK_KEYBOARD_BASE, 2);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in(DEVICE(cpu), 0));

    bk0010_load_rom(machine->firmware ?: MONITOR_ROM_FILENAME,
                    BK_MONITOR_ROM_BASE, BK_MONITOR_ROM_SIZE);
    bk0010_load_rom(BASIC_ROM_FILENAME, BK_BASIC_ROM_BASE, BK_BASIC_ROM_SIZE);
}

static void bk0010_machine_init(MachineClass *mc)
{
    mc->desc = "BK-0010 home computer";
    mc->init = bk0010_init;
    mc->is_default = true;
    mc->default_cpu_type = TYPE_K1801VM1_CPU;
    mc->default_ram_size = BK_RAM_SIZE;
    mc->default_ram_id = "bk0010.ram";
}

DEFINE_MACHINE("bk0010", bk0010_machine_init)
