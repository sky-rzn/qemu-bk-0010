/*
 * BK-0010 devices
 *
 * Copyright (c) 2019 Alexandr Ivanov
 * SPDX-License-Identifier: MIT
 */

#ifndef HW_K1801VM1_BK_HW_H
#define HW_K1801VM1_BK_HW_H

#define TYPE_BK_DISPLAY         "bk-display"
#define TYPE_BK_KEYBOARD        "bk-keyboard"
#define TYPE_BK_SPEAKER         "bk-speaker"
#define TYPE_BK_SYSREGS         "bk-sysregs"

#define BK_RAM_BASE             0000000
#define BK_RAM_SIZE             0040000

#define BK_VIDEO_BASE           0040000
#define BK_VIDEO_SIZE           0040000

#define BK_MONITOR_ROM_BASE     0100000
#define BK_MONITOR_ROM_SIZE     0020000

#define BK_BASIC_ROM_BASE       0120000
#define BK_BASIC_ROM_SIZE       0060000

#define BK_SYSREGS_BASE         0177600
#define BK_SYSREGS_SIZE         0000200

#define BK_KEYBOARD_BASE        0177660
#define BK_KEYBOARD_SIZE        4

#define BK_SCROLL_BASE          0177664
#define BK_SCROLL_SIZE          2

#endif
