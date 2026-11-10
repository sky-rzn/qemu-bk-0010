/*
 * BK-0010 speaker
 *
 * Copyright (c) 2026 Alexandr Ivanov
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/audio.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "target/k1801vm1/cpu.h"
#include "bk-hw.h"

OBJECT_DECLARE_SIMPLE_TYPE(BkSpeakerState, BK_SPEAKER)

#define SAMPLE_RATE     44100
#define AMPLITUDE       8192
#define FIFO_LEN        (SAMPLE_RATE / 5)
#define MAX_GAP_CYCLES  (K1801VM1_CLOCK_HZ / 5)

struct BkSpeakerState {
    SysBusDevice parent_obj;

    K1801VM1CPU *cpu;
    AudioBackend *audio_be;
    SWVoiceOut *voice;

    bool level;
    /* The output is rendered up to this CPU cycle. */
    uint64_t cycles;
    /* Both are in cycles * SAMPLE_RATE, so sample boundaries stay exact. */
    uint32_t phase;
    int32_t acc;

    int16_t fifo[FIFO_LEN];
    unsigned int head;
    unsigned int count;
};

static void bk_speaker_push(BkSpeakerState *s, int16_t sample)
{
    if (s->count == FIFO_LEN) {
        s->head = (s->head + 1) % FIFO_LEN;
        s->count--;
    }
    s->fifo[(s->head + s->count) % FIFO_LEN] = sample;
    s->count++;
}

static void bk_speaker_render(BkSpeakerState *s, uint64_t now)
{
    int sign = s->level ? 1 : -1;
    uint64_t units;

    if (now <= s->cycles) {
        return;
    }
    if (now - s->cycles > MAX_GAP_CYCLES) {
        s->cycles = now;
        s->phase = 0;
        s->acc = 0;
        return;
    }

    units = (now - s->cycles) * SAMPLE_RATE;
    s->cycles = now;

    while (s->phase + units >= K1801VM1_CLOCK_HZ) {
        uint32_t n = K1801VM1_CLOCK_HZ - s->phase;

        s->acc += sign * (int32_t)n;
        bk_speaker_push(s, (int64_t)s->acc * AMPLITUDE / K1801VM1_CLOCK_HZ);
        units -= n;
        s->phase = 0;
        s->acc = 0;
    }
    s->acc += sign * (int32_t)units;
    s->phase += units;
}

static void bk_speaker_set_level(void *opaque, int irq, int level)
{
    BkSpeakerState *s = opaque;

    bk_speaker_render(s, s->cpu->env.cycles);
    s->level = level;
}

static void bk_speaker_callback(void *opaque, int free)
{
    BkSpeakerState *s = opaque;

    bk_speaker_render(s, qatomic_read(&s->cpu->env.cycles));

    while (s->count && free >= sizeof(int16_t)) {
        unsigned int n = MIN(s->count, FIFO_LEN - s->head);
        size_t done;

        n = MIN(n, free / sizeof(int16_t));
        done = audio_be_write(s->audio_be, s->voice, &s->fifo[s->head],
                              n * sizeof(int16_t)) / sizeof(int16_t);
        if (!done) {
            break;
        }
        s->head = (s->head + done) % FIFO_LEN;
        s->count -= done;
        free -= done * sizeof(int16_t);
    }
}

static void bk_speaker_realize(DeviceState *dev, Error **errp)
{
    BkSpeakerState *s = BK_SPEAKER(dev);
    struct audsettings as = {
        .freq = SAMPLE_RATE,
        .nchannels = 1,
        .fmt = AUDIO_FORMAT_S16,
        .big_endian = HOST_BIG_ENDIAN,
    };

    if (!s->cpu) {
        error_setg(errp, "bk-speaker: 'cpu' link not set");
        return;
    }
    if (!audio_be_check(&s->audio_be, errp)) {
        return;
    }

    s->voice = audio_be_open_out(s->audio_be, s->voice, TYPE_BK_SPEAKER, s,
                                 bk_speaker_callback, &as);
    if (!s->voice) {
        error_setg(errp, "bk-speaker: could not open voice");
        return;
    }
    audio_be_set_active_out(s->audio_be, s->voice, true);
}

static void bk_speaker_init(Object *obj)
{
    qdev_init_gpio_in(DEVICE(obj), bk_speaker_set_level, 1);
}

static void bk_speaker_reset_hold(Object *obj, ResetType type)
{
    BkSpeakerState *s = BK_SPEAKER(obj);

    s->level = false;
    s->cycles = s->cpu ? s->cpu->env.cycles : 0;
    s->phase = 0;
    s->acc = 0;
    s->head = 0;
    s->count = 0;
}

static const VMStateDescription vmstate_bk_speaker = {
    .name = TYPE_BK_SPEAKER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(level, BkSpeakerState),
        VMSTATE_UINT64(cycles, BkSpeakerState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property bk_speaker_properties[] = {
    DEFINE_AUDIO_PROPERTIES(BkSpeakerState, audio_be),
    DEFINE_PROP_LINK("cpu", BkSpeakerState, cpu, TYPE_K1801VM1_CPU,
                     K1801VM1CPU *),
};

static void bk_speaker_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = bk_speaker_realize;
    dc->vmsd = &vmstate_bk_speaker;
    rc->phases.hold = bk_speaker_reset_hold;
    device_class_set_props(dc, bk_speaker_properties);
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
}

static const TypeInfo bk_speaker_info = {
    .name          = TYPE_BK_SPEAKER,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BkSpeakerState),
    .instance_init = bk_speaker_init,
    .class_init    = bk_speaker_class_init,
};

static void bk_speaker_register_types(void)
{
    type_register_static(&bk_speaker_info);
}

type_init(bk_speaker_register_types)
