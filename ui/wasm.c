/*
 * Browser display for the Emscripten build
 *
 * Frames and key events are exchanged with the page through a block of
 * the shared wasm heap, so the page never calls into QEMU's thread.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/module.h"
#include "ui/console.h"
#include "ui/input.h"

#include <emscripten.h>

#define WASM_UI_REFRESH_MS  20
#define WASM_UI_KEY_RING    256
#define WASM_UI_MAX_PIXELS  (1024 * 1024)

/* Layout is mirrored in web/src/qemu.js */
typedef struct WasmUiShared {
    uint32_t frame_seq;
    uint32_t frame_buf;
    uint32_t width;
    uint32_t height;
    uint32_t frame[2];
    uint32_t key_head;
    uint32_t key_tail;
    uint32_t key_ring[WASM_UI_KEY_RING];
} WasmUiShared;

#define WASM_UI_KEY_DOWN    (1u << 31)

static WasmUiShared wasm_ui;
static uint32_t *wasm_ui_frames[2];
static DisplayChangeListener wasm_dcl;

static void wasm_ui_poll_keys(DisplayChangeListener *dcl)
{
    uint32_t tail = wasm_ui.key_tail;
    uint32_t head = qatomic_load_acquire(&wasm_ui.key_head);

    while (tail != head) {
        uint32_t ev = wasm_ui.key_ring[tail % WASM_UI_KEY_RING];

        qemu_input_event_send_key_linux(dcl->con, ev & ~WASM_UI_KEY_DOWN,
                                        ev & WASM_UI_KEY_DOWN);
        tail++;
    }
    qatomic_store_release(&wasm_ui.key_tail, tail);
}

static void wasm_ui_publish(DisplaySurface *surface)
{
    int w = surface_width(surface);
    int h = surface_height(surface);
    int buf = !qatomic_read(&wasm_ui.frame_buf);
    uint32_t *dst = wasm_ui_frames[buf];
    int x, y;

    if (w * h > WASM_UI_MAX_PIXELS ||
        surface_format(surface) != PIXMAN_x8r8g8b8) {
        return;
    }

    for (y = 0; y < h; y++) {
        const uint32_t *src = (const uint32_t *)(surface_data(surface) +
                                                 y * surface_stride(surface));
        for (x = 0; x < w; x++) {
            uint32_t p = *src++;
            /* xRGB -> RGBA bytes as ImageData wants them */
            *dst++ = 0xff000000 | ((p & 0xff) << 16) | (p & 0xff00) |
                     ((p >> 16) & 0xff);
        }
    }
    wasm_ui.width = w;
    wasm_ui.height = h;
    qatomic_store_release(&wasm_ui.frame_buf, buf);
    qatomic_inc(&wasm_ui.frame_seq);
}

static void wasm_ui_refresh(DisplayChangeListener *dcl)
{
    wasm_ui_poll_keys(dcl);
    qemu_console_hw_update(dcl->con);
}

static void wasm_ui_gfx_update(DisplayChangeListener *dcl,
                               int x, int y, int w, int h)
{
    if (dcl->con && qemu_console_surface(dcl->con)) {
        wasm_ui_publish(qemu_console_surface(dcl->con));
    }
}

static void wasm_ui_gfx_switch(DisplayChangeListener *dcl,
                               DisplaySurface *surface)
{
    if (surface) {
        wasm_ui_publish(surface);
    }
}

static const DisplayChangeListenerOps wasm_ui_ops = {
    .dpy_name = "wasm",
    .dpy_refresh = wasm_ui_refresh,
    .dpy_gfx_update = wasm_ui_gfx_update,
    .dpy_gfx_switch = wasm_ui_gfx_switch,
};

static void wasm_ui_init(DisplayState *ds, DisplayOptions *opts)
{
    QemuConsole *con = qemu_console_lookup_by_index(0);

    if (!con || !qemu_console_is_graphic(con)) {
        return;
    }

    for (int i = 0; i < 2; i++) {
        wasm_ui_frames[i] = g_new0(uint32_t, WASM_UI_MAX_PIXELS);
        wasm_ui.frame[i] = (uint32_t)(uintptr_t)wasm_ui_frames[i];
    }

    qemu_console_register_listener(con, &wasm_dcl, &wasm_ui_ops);
    qemu_console_listener_set_refresh(&wasm_dcl, WASM_UI_REFRESH_MS);

    MAIN_THREAD_ASYNC_EM_ASM({
        if (Module.qemuDisplayReady) {
            Module.qemuDisplayReady(HEAPU8.buffer, $0);
        }
    }, (uint32_t)(uintptr_t)&wasm_ui);
}

static QemuDisplay qemu_display_wasm = {
    .type = DISPLAY_TYPE_WASM,
    .init = wasm_ui_init,
};

static void register_wasm_ui(void)
{
    qemu_display_register(&qemu_display_wasm);
}

type_init(register_wasm_ui);
