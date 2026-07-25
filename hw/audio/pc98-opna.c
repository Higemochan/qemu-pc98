/*
 * QEMU NEC PC-9801-86 sound board (YM2608 OPNA + YM2149 SSG)
 *
 * Copyright (c) 2026 Awe Morris
 *
 * The PC-9801-86 ("86 board") is the de-facto standard PC-98 FM sound
 * board: a Yamaha YM2608 (OPNA) clocked at 7.9872 MHz, providing 6 FM
 * channels, an SSG (YM2149-compatible) section, a rhythm generator driven
 * by a 8 KiB internal ADPCM-A sample ROM, and an ADPCM-B channel backed by
 * 256 KiB of on-board DRAM.  Its register file is reached through the PC-98
 * I/O ports:
 *
 *   0x188  R/W  address port 1 (regs 0x00-0xFF)   / status read
 *   0x18A  R/W  data port 1                       / data read
 *   0x18C  R/W  address port 2 (regs 0x100-0x1FF)
 *   0x18E  R/W  data port 2                       / data read
 *
 * The board also answers the PC-98 "sound ID" register at I/O 0xA460, which
 * software reads to detect the board; the 86 board reports 0x40 ("4x").
 * The FM timers raise PC-98 IRQ 3 (INT0).
 *
 * The FM/ADPCM engine is the OPN core (hw/audio/fmopn.c, GPL-2.0+,
 * Jarek Burczynski & Tatsuyuki Satoh); the SSG is the emu2149 core
 * (hw/audio/emu2149.c, MIT, Mitsutaka Okazaki).  This file only wraps those
 * cores as a QOM ISA device: it maps the ports, mixes the two audio streams
 * into the QEMU mixer, arms the FM timers on the virtual clock, and forwards
 * the FM interrupt.  The rhythm sample ROM is loaded at runtime from the
 * firmware search path (-L); the copyrighted Yamaha data is never built in.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/host-utils.h"
#include "qemu/timer.h"
#include "qemu/audio.h"
#include "qemu/datadir.h"
#include "hw/core/loader.h"
#include "hw/core/irq.h"
#include "hw/audio/pc98-opna.h"
#include "hw/core/qdev-properties.h"
#include "hw/isa/isa.h"
#include "system/ioport.h"
#include "qom/object.h"
#include "trace.h"

#include "fmopn.h"
#include "emu2149.h"

/* PC-9801-86 OPNA master clock and derived SSG clock. */
#define PC98_OPNA_CLOCK         7987200
#define PC98_OPNA_SSG_CLOCK     (PC98_OPNA_CLOCK / 4)

/* FM/SSG register port block. */
#define PC98_OPNA_IOBASE        0x188

/* Sound ID register: read to detect the board.  0x40 == PC-9801-86 ("4x"). */
#define PC98_OPNA_ID_PORT       0xa460
#define PC98_OPNA_SOUND_ID      0x40

/* On-board ADPCM-B DRAM and rhythm ADPCM-A sample ROM. */
#define PC98_OPNA_ADPCM_RAM     (256 * 1024)
#define PC98_OPNA_RHYTHM_SIZE   0x2000
#define PC98_OPNA_RHYTHM_FILE   "2608_rhythm.rom"

/* Internal audio generation granularity (frames). */
#define PC98_OPNA_CHUNK         512

OBJECT_DECLARE_SIMPLE_TYPE(Pc98OpnaState, PC98_OPNA)

struct Pc98OpnaState {
    ISADevice parent_obj;

    AudioBackend *audio_be;
    uint32_t freq;

    void *opna;                 /* MAME YM2608 chip instance */
    PSG *ssg;                   /* emu2149 SSG instance      */
    uint8_t *rhythm_rom;        /* 0x2000-byte ADPCM-A ROM   */

    SWVoiceOut *voice;
    qemu_irq irq;               /* FM timer interrupt (IRQ 3) */
    QEMUTimer *timer[2];        /* OPNA timer A / timer B     */

    PortioList portio;          /* FM/SSG ports 0x188-0x18E   */
    PortioList portio_id;       /* sound ID port 0xA460       */
    uint8_t sound_id;           /* 0xA460 read value          */
    bool active;

    /* Scratch mixing buffers. */
    int32_t fm_l[PC98_OPNA_CHUNK];
    int32_t fm_r[PC98_OPNA_CHUNK];
    int16_t mix[PC98_OPNA_CHUNK * 2];
};

/* ---- SSG (emu2149) glue: the FM core routes SSG register access here ---- */

static void opna_ssg_set_clock(void *param, uint32_t clock)
{
    Pc98OpnaState *s = param;

    if (s->ssg) {
        PSG_set_clock(s->ssg, clock);
    }
}

static void opna_ssg_write(void *param, uint8_t address, uint8_t data)
{
    Pc98OpnaState *s = param;

    if (s->ssg) {
        PSG_writeIO(s->ssg, address, data);
    }
}

static uint8_t opna_ssg_read(void *param)
{
    Pc98OpnaState *s = param;

    return s->ssg ? PSG_readIO(s->ssg) : 0;
}

static void opna_ssg_reset(void *param)
{
    Pc98OpnaState *s = param;

    if (s->ssg) {
        PSG_reset(s->ssg);
    }
}

static const ssg_callbacks opna_ssg_intf = {
    opna_ssg_set_clock,
    opna_ssg_write,
    opna_ssg_read,
    opna_ssg_reset,
};

/* ---- FM timer / interrupt callbacks (driven by the MAME core) ---- */

static void opna_irq_handler(void *param, uint8_t level)
{
    Pc98OpnaState *s = param;

    trace_pc98_opna_irq(level);
    qemu_set_irq(s->irq, level ? 1 : 0);
}

/*
 * The core calls this to (re)start (cnt > 0) or stop (cnt == 0) an OPNA
 * timer.  The period is cnt counts of the timer base clock, i.e.
 * cnt / clock seconds.
 */
static void opna_timer_handler(void *param, uint8_t c, int32_t cnt,
                               uint32_t clock)
{
    Pc98OpnaState *s = param;
    unsigned n = c & 1;

    if (cnt == 0 || clock == 0) {
        trace_pc98_opna_timer(n, 0);
        timer_del(s->timer[n]);
        return;
    }

    trace_pc98_opna_timer(n, muldiv64(cnt, 1000000, clock));
    timer_mod(s->timer[n],
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
              + muldiv64(cnt, NANOSECONDS_PER_SECOND, clock));
}

static void opna_timer_a(void *opaque)
{
    Pc98OpnaState *s = opaque;

    ym2608_timer_over(s->opna, 0);
}

static void opna_timer_b(void *opaque)
{
    Pc98OpnaState *s = opaque;

    ym2608_timer_over(s->opna, 1);
}

/* ---- Audio rendering ---- */

static inline int16_t opna_clip(int32_t v)
{
    if (v > 32767) {
        return 32767;
    }
    if (v < -32768) {
        return -32768;
    }
    return v;
}

static void opna_callback(void *opaque, int free_bytes)
{
    Pc98OpnaState *s = opaque;
    int frames = free_bytes / (int)sizeof(int16_t) / 2;

    if (!s->active || frames <= 0) {
        return;
    }

    while (frames > 0) {
        int chunk = MIN(frames, PC98_OPNA_CHUNK);
        int32_t *bufs[2] = { s->fm_l, s->fm_r };
        int i;

        ym2608_update_one(s->opna, chunk, bufs);

        for (i = 0; i < chunk; i++) {
            int32_t l = s->fm_l[i];
            int32_t r = s->fm_r[i];

            if (s->ssg) {
                int32_t p = (int32_t)PSG_calc(s->ssg) << 1;
                l += p;
                r += p;
            }
            s->mix[2 * i]     = opna_clip(l);
            s->mix[2 * i + 1] = opna_clip(r);
        }

        audio_be_write(s->audio_be, s->voice, s->mix,
                       chunk * 2 * sizeof(int16_t));
        frames -= chunk;
    }
}

/* ---- I/O port handlers ---- */

static void opna_write(void *opaque, uint32_t addr, uint32_t val)
{
    Pc98OpnaState *s = opaque;
    int a = (addr - PC98_OPNA_IOBASE) >> 1;     /* 0x188/8A/8C/8E -> 0..3 */

    if (!s->active) {
        s->active = true;
        audio_be_set_active_out(s->audio_be, s->voice, 1);
    }
    ym2608_write(s->opna, a, val & 0xff);
}

static uint32_t opna_read(void *opaque, uint32_t addr)
{
    Pc98OpnaState *s = opaque;
    int a = (addr - PC98_OPNA_IOBASE) >> 1;

    return ym2608_read(s->opna, a);
}

static const MemoryRegionPortio pc98_opna_portio[] = {
    { 0x188, 1, 1, .read = opna_read, .write = opna_write },
    { 0x18a, 1, 1, .read = opna_read, .write = opna_write },
    { 0x18c, 1, 1, .read = opna_read, .write = opna_write },
    { 0x18e, 1, 1, .read = opna_read, .write = opna_write },
    PORTIO_END_OF_LIST(),
};

/* ---- Sound ID register (0xA460) ---- */

static uint32_t opna_id_read(void *opaque, uint32_t addr)
{
    Pc98OpnaState *s = opaque;

    trace_pc98_opna_id_read(s->sound_id);
    return s->sound_id;
}

static void opna_id_write(void *opaque, uint32_t addr, uint32_t val)
{
    Pc98OpnaState *s = opaque;

    /* Only bit 0 is a writable latch; the board-ID bits are read-only. */
    s->sound_id = (s->sound_id & 0xfe) | (val & 1);
}

static const MemoryRegionPortio pc98_opna_id_portio[] = {
    { PC98_OPNA_ID_PORT, 1, 1, .read = opna_id_read, .write = opna_id_write },
    PORTIO_END_OF_LIST(),
};

/* ---- Rhythm ADPCM-A ROM: loaded from the -L firmware path ---- */

static void opna_load_rhythm_rom(Pc98OpnaState *s)
{
    char *path;

    s->rhythm_rom = g_malloc0(PC98_OPNA_RHYTHM_SIZE);

    path = qemu_find_file(QEMU_FILE_TYPE_BIOS, PC98_OPNA_RHYTHM_FILE);
    if (path) {
        if (load_image_size(path, s->rhythm_rom, PC98_OPNA_RHYTHM_SIZE)
            != PC98_OPNA_RHYTHM_SIZE) {
            warn_report("pc98-opna: %s is not %d bytes; rhythm disabled",
                        PC98_OPNA_RHYTHM_FILE, PC98_OPNA_RHYTHM_SIZE);
            memset(s->rhythm_rom, 0, PC98_OPNA_RHYTHM_SIZE);
        } else {
            trace_pc98_opna_rhythm_loaded(path);
        }
        g_free(path);
    } else {
        warn_report("pc98-opna: %s not found on the -L path; rhythm disabled",
                    PC98_OPNA_RHYTHM_FILE);
    }
    ym2608_set_rhythm_rom(s->rhythm_rom);
}

/* ---- Realize / lifecycle ---- */

static void pc98_opna_realize(DeviceState *dev, Error **errp)
{
    ISADevice *isadev = ISA_DEVICE(dev);
    Pc98OpnaState *s = PC98_OPNA(dev);
    struct audsettings as;

    if (!audio_be_check(&s->audio_be, errp)) {
        return;
    }

    qdev_init_gpio_out(dev, &s->irq, 1);
    s->sound_id = PC98_OPNA_SOUND_ID;

    /* SSG first: the FM core's reset path calls back into it. */
    s->ssg = PSG_new(PC98_OPNA_SSG_CLOCK, s->freq);
    if (!s->ssg) {
        error_setg(errp, "pc98-opna: PSG_new failed");
        return;
    }
    PSG_setVolumeMode(s->ssg, 1);       /* YM2149 volume law */

    opna_load_rhythm_rom(s);

    s->timer[0] = timer_new_ns(QEMU_CLOCK_VIRTUAL, opna_timer_a, s);
    s->timer[1] = timer_new_ns(QEMU_CLOCK_VIRTUAL, opna_timer_b, s);

    s->opna = ym2608_init(s, PC98_OPNA_CLOCK, s->freq,
                          opna_timer_handler, opna_irq_handler);
    if (!s->opna) {
        error_setg(errp, "pc98-opna: ym2608_init failed");
        return;
    }
    ym2608_link_ssg(s->opna, &opna_ssg_intf, s);
    ym2608_alloc_pcmromb(s->opna, PC98_OPNA_ADPCM_RAM);
    ym2608_reset_chip(s->opna);

    as.freq = s->freq;
    as.nchannels = 2;
    as.fmt = AUDIO_FORMAT_S16;
    as.big_endian = HOST_BIG_ENDIAN;

    s->voice = audio_be_open_out(s->audio_be, s->voice, "pc98-opna",
                                 s, opna_callback, &as);
    if (!s->voice) {
        error_setg(errp, "pc98-opna: audio_be_open_out failed");
        return;
    }

    isa_register_portio_list(isadev, &s->portio, 0, pc98_opna_portio, s,
                             "pc98-opna");
    isa_register_portio_list(isadev, &s->portio_id, 0, pc98_opna_id_portio, s,
                             "pc98-opna-id");
}

static void pc98_opna_reset(DeviceState *dev)
{
    Pc98OpnaState *s = PC98_OPNA(dev);

    trace_pc98_opna_reset();

    /*
     * Stop the FM timers and drop the interrupt line before resetting the
     * chip: ym2608_reset_chip() clears the status word directly and would not
     * otherwise call back to deassert IRQ 3.
     */
    timer_del(s->timer[0]);
    timer_del(s->timer[1]);
    qemu_set_irq(s->irq, 0);

    s->sound_id = PC98_OPNA_SOUND_ID;
    s->active = false;
    ym2608_reset_chip(s->opna);         /* also resets the SSG via callback */
}

static const Property pc98_opna_properties[] = {
    DEFINE_AUDIO_PROPERTIES(Pc98OpnaState, audio_be),
    DEFINE_PROP_UINT32("freq", Pc98OpnaState, freq, 55466),
};

static void pc98_opna_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pc98_opna_realize;
    device_class_set_legacy_reset(dc, pc98_opna_reset);
    device_class_set_props(dc, pc98_opna_properties);
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "NEC PC-9801-86 sound board (YM2608 OPNA)";
    dc->user_creatable = false;
}

static const TypeInfo pc98_opna_info = {
    .name          = TYPE_PC98_OPNA,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(Pc98OpnaState),
    .class_init    = pc98_opna_class_init,
};

static void pc98_opna_register_types(void)
{
    type_register_static(&pc98_opna_info);
}

type_init(pc98_opna_register_types)

void pc98_opna_init(ISABus *bus, qemu_irq irq)
{
    ISADevice *isadev = isa_new(TYPE_PC98_OPNA);
    DeviceState *dev = DEVICE(isadev);

    isa_realize_and_unref(isadev, bus, &error_fatal);
    qdev_connect_gpio_out(dev, 0, irq);
}
