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
#include "migration/vmstate.h"
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

/* PC-9801-86 PCM control block (even ports 0xA462-0xA46E). */
#define PC98_PCM_CLOCK_PORT     0xa466
#define PC98_PCM_FIFO_PORT      0xa468
#define PC98_PCM_DACTRL_PORT    0xa46a
#define PC98_PCM_DATA_PORT      0xa46c
#define PC98_PCM_BUFFER_SIZE    0x8000
#define PC98_PCM_BUFFER_MASK    (PC98_PCM_BUFFER_SIZE - 1)

/* Sample clock is eight times the actual PCM sample rate. */
static const uint32_t pc98_pcm_clock_rate[8] = {
    352800, 264600, 176400, 132300, 88200, 66150, 44010, 33075,
};

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
    uint32_t isairq;

    void *opna;                 /* MAME YM2608 chip instance */
    PSG *ssg;                   /* emu2149 SSG instance      */
    uint8_t *rhythm_rom;        /* 0x2000-byte ADPCM-A ROM   */

    SWVoiceOut *voice;
    qemu_irq irq;               /* FM timer interrupt (IRQ 3) */
    QEMUTimer *timer[2];        /* OPNA timer A / timer B     */

    PortioList portio;          /* FM/SSG ports 0x188-0x18E   */
    PortioList portio_id;       /* sound ID port 0xA460       */
    PortioList portio_pcm;      /* 86 PCM control/data ports  */
    uint8_t sound_id;           /* 0xA460 read value          */
    uint8_t pcm_fifo;           /* 0xA468 FIFO control        */
    uint8_t pcm_dactrl;         /* 0xA46A data format         */
    uint8_t pcm_volume;         /* 0xA466, inverted 4-bit     */
    uint8_t pcm_buffer[PC98_PCM_BUFFER_SIZE];
    uint16_t pcm_read_pos;
    uint16_t pcm_write_pos;
    uint32_t pcm_count;
    uint32_t pcm_threshold;
    uint64_t pcm_phase;
    int16_t pcm_last_l;
    int16_t pcm_last_r;
    bool pcm_irq_pending;
    bool pcm_irq_armed;
    bool active;
    uint8_t addr_latch[2];
    uint8_t reg_shadow[512];
    uint8_t reg_valid[64];
    uint8_t prescaler;
    bool irq_level;

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

    s->irq_level = !!level;
    trace_pc98_opna_irq(level);
    qemu_set_irq(s->irq, s->irq_level || s->pcm_irq_pending);
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

static unsigned opna_pcm_frame_bytes(Pc98OpnaState *s)
{
    static const uint8_t frame_bytes[8] = { 2, 2, 2, 4, 1, 1, 1, 2 };

    return frame_bytes[(s->pcm_dactrl >> 4) & 7];
}

static uint8_t opna_pcm_pop_byte(Pc98OpnaState *s)
{
    uint8_t val = s->pcm_buffer[s->pcm_read_pos];

    s->pcm_read_pos = (s->pcm_read_pos + 1) & PC98_PCM_BUFFER_MASK;
    s->pcm_count--;
    return val;
}

static int16_t opna_pcm_pop_16(Pc98OpnaState *s)
{
    unsigned hi = opna_pcm_pop_byte(s);
    unsigned lo = opna_pcm_pop_byte(s);

    return (int16_t)((hi << 8) | lo);
}

static int16_t opna_pcm_pop_8(Pc98OpnaState *s)
{
    return (int16_t)(int8_t)opna_pcm_pop_byte(s) << 8;
}

static void opna_pcm_update_irq(Pc98OpnaState *s, bool level)
{
    if (s->pcm_irq_pending == level) {
        return;
    }
    s->pcm_irq_pending = level;
    trace_pc98_opna_pcm_irq(level, s->pcm_count, s->pcm_threshold);
    qemu_set_irq(s->irq, s->irq_level || s->pcm_irq_pending);
}

static void opna_pcm_consume(Pc98OpnaState *s)
{
    unsigned format = (s->pcm_dactrl >> 4) & 7;
    unsigned frame_bytes = opna_pcm_frame_bytes(s);
    int16_t l = 0;
    int16_t r = 0;

    if (s->pcm_count >= frame_bytes) {
        switch (format) {
        case 1:                         /* 16-bit right */
            r = opna_pcm_pop_16(s);
            break;
        case 2:                         /* 16-bit left */
            l = opna_pcm_pop_16(s);
            break;
        case 3:                         /* 16-bit stereo */
            l = opna_pcm_pop_16(s);
            r = opna_pcm_pop_16(s);
            break;
        case 5:                         /* 8-bit right */
            r = opna_pcm_pop_8(s);
            break;
        case 6:                         /* 8-bit left */
            l = opna_pcm_pop_8(s);
            break;
        case 7:                         /* 8-bit stereo */
            l = opna_pcm_pop_8(s);
            r = opna_pcm_pop_8(s);
            break;
        default:                        /* output disabled */
            while (frame_bytes--) {
                opna_pcm_pop_byte(s);
            }
            break;
        }
    }

    s->pcm_last_l = l;
    s->pcm_last_r = r;
    if ((s->pcm_fifo & 0x20) && s->pcm_irq_armed &&
        s->pcm_count <= s->pcm_threshold) {
        s->pcm_irq_armed = false;
        opna_pcm_update_irq(s, true);
    }
}

static void opna_pcm_render(Pc98OpnaState *s, int32_t *l, int32_t *r)
{
    uint32_t rate;

    if (!(s->pcm_fifo & 0x80)) {
        return;
    }

    rate = pc98_pcm_clock_rate[s->pcm_fifo & 7] / 8;
    s->pcm_phase += rate;
    while (s->pcm_phase >= s->freq) {
        s->pcm_phase -= s->freq;
        opna_pcm_consume(s);
    }
    *l += s->pcm_last_l * s->pcm_volume / 15;
    *r += s->pcm_last_r * s->pcm_volume / 15;
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
            opna_pcm_render(s, &l, &r);
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
    unsigned bank = (a >> 1) & 1;
    unsigned reg;

    if (!s->active) {
        s->active = true;
        audio_be_set_active_out(s->audio_be, s->voice, 1);
    }
    val &= 0xff;
    if (!(a & 1)) {
        s->addr_latch[bank] = val;
        if (!bank && val >= 0x2d && val <= 0x2f) {
            s->prescaler = val;
        }
    } else {
        reg = bank * 256 + s->addr_latch[bank];
        s->reg_shadow[reg] = val;
        s->reg_valid[reg >> 3] |= 1u << (reg & 7);
    }
    ym2608_write(s->opna, a, val);
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

/*
 * The 86 board's PCM clock is observable as bit 0 at 0xA466.  NEC's
 * Windows 95 NEC73PCM.DRV calibrates the device by waiting for consecutive
 * low/high transitions here before it initializes the multimedia stack.
 * Returning an open bus therefore hangs Windows even when no sound is being
 * played.
 *
 * The sample-rate selector is expressed as eight times the actual sample
 * rate.  It is also the clock observed by software at bit 0 of 0xA466.
 */
static uint32_t opna_pcm_read(void *opaque, uint32_t addr)
{
    Pc98OpnaState *s = opaque;

    switch (addr) {
    case PC98_PCM_CLOCK_PORT: {
        uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        uint32_t rate = pc98_pcm_clock_rate[s->pcm_fifo & 7];
        uint8_t phase = muldiv64(now, rate * 2,
                                 NANOSECONDS_PER_SECOND) & 1;
        unsigned frame_bytes = opna_pcm_frame_bytes(s);

        if (s->pcm_count >= PC98_PCM_BUFFER_SIZE) {
            phase |= 0x80;
        } else if (s->pcm_count < frame_bytes) {
            phase |= 0x40;
        }
        return phase;
    }
    case PC98_PCM_FIFO_PORT:
        return (s->pcm_fifo & ~0x10) |
               (s->pcm_irq_pending ? 0x10 : 0);
    case PC98_PCM_DACTRL_PORT:
        return s->pcm_dactrl;
    default:
        return 0;
    }
}

static void opna_pcm_write(void *opaque, uint32_t addr, uint32_t val)
{
    Pc98OpnaState *s = opaque;

    val &= 0xff;
    switch (addr) {
    case PC98_PCM_CLOCK_PORT:
        if ((val & 0xe0) == 0xa0) {
            s->pcm_volume = (~val) & 0x0f;
        }
        break;
    case PC98_PCM_FIFO_PORT:
        if ((val & 0x08) && !(s->pcm_fifo & 0x08)) {
            s->pcm_read_pos = 0;
            s->pcm_write_pos = 0;
            s->pcm_count = 0;
            s->pcm_phase = 0;
            s->pcm_last_l = 0;
            s->pcm_last_r = 0;
            s->pcm_irq_armed = false;
        }
        if (!(val & 0x10)) {
            opna_pcm_update_irq(s, false);
        }
        s->pcm_fifo = val & ~0x10;
        if (val & 0x80) {
            s->active = true;
            audio_be_set_active_out(s->audio_be, s->voice, 1);
        }
        break;
    case PC98_PCM_DACTRL_PORT:
        if (s->pcm_fifo & 0x20) {
            s->pcm_threshold = val == 0xff ? 0x7ffc : (val + 1) << 7;
        } else if ((val & 0x0f) != 0x0f) {
            s->pcm_dactrl = val;
        }
        break;
    case PC98_PCM_DATA_PORT:
        if (s->pcm_count < PC98_PCM_BUFFER_SIZE) {
            s->pcm_buffer[s->pcm_write_pos] = val;
            s->pcm_write_pos =
                (s->pcm_write_pos + 1) & PC98_PCM_BUFFER_MASK;
            s->pcm_count++;
            s->pcm_irq_armed = true;
            s->active = true;
            audio_be_set_active_out(s->audio_be, s->voice, 1);
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionPortio pc98_opna_pcm_portio[] = {
    { 0xa462, 1, 1, .read = opna_pcm_read, .write = opna_pcm_write },
    { 0xa464, 1, 1, .read = opna_pcm_read, .write = opna_pcm_write },
    { 0xa466, 1, 1, .read = opna_pcm_read, .write = opna_pcm_write },
    { 0xa468, 1, 1, .read = opna_pcm_read, .write = opna_pcm_write },
    { 0xa46a, 1, 1, .read = opna_pcm_read, .write = opna_pcm_write },
    { 0xa46c, 1, 1, .read = opna_pcm_read, .write = opna_pcm_write },
    { 0xa46e, 1, 1, .read = opna_pcm_read, .write = opna_pcm_write },
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

    s->irq = isa_get_irq(isadev, s->isairq);
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
    isa_register_portio_list(isadev, &s->portio_pcm, 0,
                             pc98_opna_pcm_portio, s, "pc98-opna-pcm");
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
    s->pcm_fifo = 0;
    s->pcm_dactrl = 0x32;
    s->pcm_volume = 15;
    memset(s->pcm_buffer, 0, sizeof(s->pcm_buffer));
    s->pcm_read_pos = 0;
    s->pcm_write_pos = 0;
    s->pcm_count = 0;
    s->pcm_threshold = 0x80;
    s->pcm_phase = 0;
    s->pcm_last_l = 0;
    s->pcm_last_r = 0;
    s->pcm_irq_pending = false;
    s->pcm_irq_armed = false;
    s->active = false;
    memset(s->addr_latch, 0, sizeof(s->addr_latch));
    memset(s->reg_shadow, 0, sizeof(s->reg_shadow));
    memset(s->reg_valid, 0, sizeof(s->reg_valid));
    s->prescaler = 0xff;
    s->irq_level = false;
    ym2608_reset_chip(s->opna);         /* also resets the SSG via callback */
}

/*
 * The imported YM2608/PSG engines do not expose a VMState layout.  Preserve
 * their architectural register files in the wrapper and rebuild the cores by
 * replaying register writes after load.  This retains guest programming and
 * timer deadlines without serializing host pointers or mixer scratch state.
 */
static int pc98_opna_post_load(void *opaque, int version_id)
{
    Pc98OpnaState *s = opaque;
    uint8_t addr_latch[2] = { s->addr_latch[0], s->addr_latch[1] };
    bool irq_level = s->irq_level;
    bool pending[2];
    uint64_t expires[2];
    unsigned reg;
    int i;

    if (version_id < 2) {
        s->pcm_volume = 15;
        s->pcm_threshold = 0x80;
    }
    for (i = 0; i < 2; i++) {
        pending[i] = timer_pending(s->timer[i]);
        expires[i] = timer_expire_time_ns(s->timer[i]);
        timer_del(s->timer[i]);
    }
    ym2608_reset_chip(s->opna);
    if (s->prescaler >= 0x2d && s->prescaler <= 0x2f) {
        ym2608_write(s->opna, 0, s->prescaler);
    }
    for (reg = 0; reg < ARRAY_SIZE(s->reg_shadow); reg++) {
        if (s->reg_valid[reg >> 3] & (1u << (reg & 7))) {
            unsigned bank = reg >> 8;

            ym2608_write(s->opna, bank ? 2 : 0, reg & 0xff);
            ym2608_write(s->opna, bank ? 3 : 1, s->reg_shadow[reg]);
        }
    }
    ym2608_write(s->opna, 0, addr_latch[0]);
    ym2608_write(s->opna, 2, addr_latch[1]);
    for (i = 0; i < 2; i++) {
        if (pending[i]) {
            timer_mod(s->timer[i], expires[i]);
        } else {
            timer_del(s->timer[i]);
        }
    }
    s->irq_level = irq_level;
    qemu_set_irq(s->irq, irq_level || s->pcm_irq_pending);
    audio_be_set_active_out(s->audio_be, s->voice, s->active);
    return 0;
}

static const VMStateDescription vmstate_pc98_opna = {
    .name = "pc98-opna",
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = pc98_opna_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(sound_id, Pc98OpnaState),
        VMSTATE_UINT8(pcm_fifo, Pc98OpnaState),
        VMSTATE_UINT8(pcm_dactrl, Pc98OpnaState),
        VMSTATE_UINT8_V(pcm_volume, Pc98OpnaState, 2),
        VMSTATE_UINT8_ARRAY_V(pcm_buffer, Pc98OpnaState,
                              PC98_PCM_BUFFER_SIZE, 2),
        VMSTATE_UINT16_V(pcm_read_pos, Pc98OpnaState, 2),
        VMSTATE_UINT16_V(pcm_write_pos, Pc98OpnaState, 2),
        VMSTATE_UINT32_V(pcm_count, Pc98OpnaState, 2),
        VMSTATE_UINT32_V(pcm_threshold, Pc98OpnaState, 2),
        VMSTATE_UINT64_V(pcm_phase, Pc98OpnaState, 2),
        VMSTATE_INT16_V(pcm_last_l, Pc98OpnaState, 2),
        VMSTATE_INT16_V(pcm_last_r, Pc98OpnaState, 2),
        VMSTATE_BOOL_V(pcm_irq_pending, Pc98OpnaState, 2),
        VMSTATE_BOOL_V(pcm_irq_armed, Pc98OpnaState, 2),
        VMSTATE_BOOL(active, Pc98OpnaState),
        VMSTATE_UINT8_ARRAY(addr_latch, Pc98OpnaState, 2),
        VMSTATE_UINT8_ARRAY(reg_shadow, Pc98OpnaState, 512),
        VMSTATE_UINT8_ARRAY(reg_valid, Pc98OpnaState, 64),
        VMSTATE_UINT8(prescaler, Pc98OpnaState),
        VMSTATE_BOOL(irq_level, Pc98OpnaState),
        VMSTATE_TIMER_PTR(timer[0], Pc98OpnaState),
        VMSTATE_TIMER_PTR(timer[1], Pc98OpnaState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property pc98_opna_properties[] = {
    DEFINE_AUDIO_PROPERTIES(Pc98OpnaState, audio_be),
    DEFINE_PROP_UINT32("freq", Pc98OpnaState, freq, 55466),
    DEFINE_PROP_UINT32("irq", Pc98OpnaState, isairq, 12),
};

static void pc98_opna_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pc98_opna_realize;
    dc->vmsd = &vmstate_pc98_opna;
    device_class_set_legacy_reset(dc, pc98_opna_reset);
    device_class_set_props(dc, pc98_opna_properties);
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "NEC PC-9801-86 sound board (YM2608 OPNA)";
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
