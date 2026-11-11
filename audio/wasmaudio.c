/*
 * Browser audio output for the Emscripten build
 *
 * Samples go into a ring in the shared wasm heap that the page drains
 * from an AudioWorklet.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/module.h"
#include "qemu/audio.h"
#include "qom/object.h"

#include "audio_int.h"

#include <emscripten.h>

#define TYPE_AUDIO_WASM "audio-wasm"
OBJECT_DECLARE_SIMPLE_TYPE(AudioWasm, AUDIO_WASM)

#define WASM_AUDIO_RING_FRAMES 16384

struct AudioWasm {
    AudioMixengBackend parent_obj;
};

/* Layout is mirrored in web/src/qemu.js */
typedef struct WasmAudioShared {
    uint32_t freq;
    uint32_t channels;
    uint32_t size;
    uint32_t buf;
    uint32_t head;
    uint32_t tail;
} WasmAudioShared;

typedef struct WasmVoiceOut {
    HWVoiceOut hw;
    RateCtl rate;
} WasmVoiceOut;

static WasmAudioShared wasm_audio;
static int16_t *wasm_audio_ring;

static size_t wasm_write(HWVoiceOut *hw, void *buf, size_t len)
{
    WasmVoiceOut *wv = (WasmVoiceOut *)hw;
    size_t bytes = audio_rate_get_bytes(&wv->rate, &hw->info, len);
    unsigned bpf = hw->info.bytes_per_frame;
    uint32_t frames = bytes / bpf;
    uint32_t head = wasm_audio.head;
    uint32_t tail = qatomic_load_acquire(&wasm_audio.tail);
    uint32_t space = wasm_audio.size - (head - tail);
    const uint8_t *src = buf;

    /* A full ring means the page is not playing: drop, keep real time */
    frames = MIN(frames, space);
    while (frames) {
        uint32_t pos = head % wasm_audio.size;
        uint32_t n = MIN(frames, wasm_audio.size - pos);

        memcpy(wasm_audio_ring + pos * hw->info.nchannels, src, n * bpf);
        src += n * bpf;
        head += n;
        frames -= n;
    }
    qatomic_store_release(&wasm_audio.head, head);
    return bytes;
}

static int wasm_init_out(HWVoiceOut *hw, struct audsettings *as)
{
    WasmVoiceOut *wv = (WasmVoiceOut *)hw;

    as->fmt = AUDIO_FORMAT_S16;
    as->big_endian = false;
    audio_pcm_init_info(&hw->info, as);
    hw->samples = 1024;
    audio_rate_start(&wv->rate);

    wasm_audio_ring = g_new0(int16_t,
                             WASM_AUDIO_RING_FRAMES * hw->info.nchannels);
    wasm_audio.freq = hw->info.freq;
    wasm_audio.channels = hw->info.nchannels;
    wasm_audio.size = WASM_AUDIO_RING_FRAMES;
    wasm_audio.buf = (uint32_t)(uintptr_t)wasm_audio_ring;

    MAIN_THREAD_ASYNC_EM_ASM({
        if (Module.qemuAudioReady) {
            Module.qemuAudioReady(HEAPU8.buffer, $0);
        }
    }, (uint32_t)(uintptr_t)&wasm_audio);
    return 0;
}

static void wasm_fini_out(HWVoiceOut *hw)
{
    wasm_audio.size = 0;
    g_clear_pointer(&wasm_audio_ring, g_free);
}

static void wasm_enable_out(HWVoiceOut *hw, bool enable)
{
    WasmVoiceOut *wv = (WasmVoiceOut *)hw;

    if (enable) {
        audio_rate_start(&wv->rate);
    }
}

static void audio_wasm_class_init(ObjectClass *klass, const void *data)
{
    AudioMixengBackendClass *k = AUDIO_MIXENG_BACKEND_CLASS(klass);

    k->max_voices_out = 1;
    k->max_voices_in = 0;
    k->voice_size_out = sizeof(WasmVoiceOut);
    k->voice_size_in = 0;

    k->init_out = wasm_init_out;
    k->fini_out = wasm_fini_out;
    k->write = wasm_write;
    k->buffer_get_free = audio_generic_buffer_get_free;
    k->run_buffer_out = audio_generic_run_buffer_out;
    k->enable_out = wasm_enable_out;
}

static const TypeInfo audio_types[] = {
    {
        .name = TYPE_AUDIO_WASM,
        .parent = TYPE_AUDIO_MIXENG_BACKEND,
        .instance_size = sizeof(AudioWasm),
        .class_init = audio_wasm_class_init,
    },
};

DEFINE_TYPES(audio_types)
module_obj(TYPE_AUDIO_WASM);
