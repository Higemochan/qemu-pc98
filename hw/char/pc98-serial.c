/*
 * NEC PC-9801/PC-9821 built-in RS-232C interface
 * Copyright (c) 2026 Awe Morris
 *
 * The interface is a uPD8251-compatible USART at I/O ports 0x30/0x32,
 * clocked by PC-98 PIT channel 2 and connected to IRQ 4.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "chardev/char-fe.h"
#include "chardev/char-serial.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/char/pc98-serial.h"
#include "hw/isa/isa.h"
#include "hw/misc/pc98-sys.h"
#include "hw/timer/i8254-pc98.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object.h"

OBJECT_DECLARE_SIMPLE_TYPE(Pc98SerialState, PC98_SERIAL)

#define PC98_SERIAL_PIT_FREQ 2457600

enum {
    USART_TXRDY   = 0x01,
    USART_RXRDY   = 0x02,
    USART_TXEMPTY = 0x04,
    USART_PARITY  = 0x08,
    USART_OVERRUN = 0x10,
    USART_FRAMING = 0x20,
    USART_BREAK   = 0x40,
    USART_DSR     = 0x80,
};

enum {
    CMD_TX_ENABLE = 0x01,
    CMD_DTR       = 0x02,
    CMD_RX_ENABLE = 0x04,
    CMD_BREAK     = 0x08,
    CMD_ERROR_RST = 0x10,
    CMD_RTS       = 0x20,
    CMD_RESET     = 0x40,
};

enum {
    IRQ_RXRDY = 0,
    IRQ_TXEMPTY,
    IRQ_TXRDY,
    IRQ_COUNT,
};

struct Pc98SerialState {
    ISADevice parent_obj;

    CharFrontend chr;
    MemoryRegion data_io;
    MemoryRegion control_io;
    qemu_irq irq;

    uint32_t iobase;
    uint32_t isairq;
    uint32_t pit_divisor;
    uint8_t irq_enable;

    uint8_t data;
    uint8_t status;
    uint8_t mode;
    uint8_t command;
    uint8_t config_phase;
    uint8_t dummy_commands;

    bool rx_valid;
    bool tx_pending;
    uint8_t tx_data;
    guint watch_tag;

    ISADevice *pit;
    Pc98SysState *sys;
};

static void pc98_serial_update_irq(Pc98SerialState *s)
{
    bool level;

    level = ((s->irq_enable & (1 << IRQ_RXRDY)) &&
             (s->status & USART_RXRDY)) ||
            ((s->irq_enable & (1 << IRQ_TXEMPTY)) &&
             (s->status & USART_TXEMPTY)) ||
            ((s->irq_enable & (1 << IRQ_TXRDY)) &&
             (s->status & USART_TXRDY));
    qemu_set_irq(s->irq, level);
}

static void pc98_serial_set_irq_enable(void *opaque, int n, int level)
{
    Pc98SerialState *s = opaque;

    if (level) {
        s->irq_enable |= 1 << n;
    } else {
        s->irq_enable &= ~(1 << n);
    }
    pc98_serial_update_irq(s);
}

static void pc98_serial_update_modem(Pc98SerialState *s)
{
    int flags;

    if (qemu_chr_fe_ioctl(&s->chr, CHR_IOCTL_SERIAL_GET_TIOCM,
                          &flags) == 0) {
        if (flags & CHR_TIOCM_DSR) {
            s->status |= USART_DSR;
        } else {
            s->status &= ~USART_DSR;
        }
    } else {
        /*
         * Non-serial backends have no modem-control lines.  Treat DSR as
         * asserted so ringbuf/socket/stdio backends remain useful.
         */
        s->status |= USART_DSR;
    }
}

static void pc98_serial_update_tiocm(Pc98SerialState *s)
{
    int flags = 0;

    qemu_chr_fe_ioctl(&s->chr, CHR_IOCTL_SERIAL_GET_TIOCM, &flags);
    if (s->command & CMD_DTR) {
        flags |= CHR_TIOCM_DTR;
    } else {
        flags &= ~CHR_TIOCM_DTR;
    }
    if (s->command & CMD_RTS) {
        flags |= CHR_TIOCM_RTS;
    } else {
        flags &= ~CHR_TIOCM_RTS;
    }
    qemu_chr_fe_ioctl(&s->chr, CHR_IOCTL_SERIAL_SET_TIOCM, &flags);
}

static void pc98_serial_update_parameters(Pc98SerialState *s)
{
    static const int clock_factor[4] = { 1, 1, 16, 64 };
    QEMUSerialSetParams params;
    uint32_t divisor = s->pit_divisor ? s->pit_divisor : 0x10000;
    unsigned int stop_code = s->mode >> 6;

    params.speed = PC98_SERIAL_PIT_FREQ /
                   clock_factor[s->mode & 3] / divisor;
    if (params.speed <= 0) {
        params.speed = 50;
    }
    params.data_bits = 5 + ((s->mode >> 2) & 3);
    if (s->mode & 0x10) {
        params.parity = (s->mode & 0x20) ? 'E' : 'O';
    } else {
        params.parity = 'N';
    }
    /* The host API has no representation for the 8251's 1.5 stop bits. */
    params.stop_bits = stop_code >= 2 ? 2 : 1;
    qemu_chr_fe_ioctl(&s->chr, CHR_IOCTL_SERIAL_SET_PARAMS, &params);
}

static void pc98_serial_clock_changed(void *opaque, uint32_t divisor)
{
    Pc98SerialState *s = opaque;

    s->pit_divisor = divisor;
    pc98_serial_update_parameters(s);
}

static void pc98_serial_tx_complete(Pc98SerialState *s)
{
    s->tx_pending = false;
    s->status |= USART_TXRDY | USART_TXEMPTY;
    pc98_serial_update_irq(s);
}

static void pc98_serial_try_tx(Pc98SerialState *s);

static gboolean pc98_serial_watch_cb(void *do_not_use, GIOCondition cond,
                                    void *opaque)
{
    Pc98SerialState *s = opaque;

    s->watch_tag = 0;
    pc98_serial_try_tx(s);
    return G_SOURCE_REMOVE;
}

static void pc98_serial_try_tx(Pc98SerialState *s)
{
    int ret;

    if (!s->tx_pending) {
        return;
    }
    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        pc98_serial_tx_complete(s);
        return;
    }

    ret = qemu_chr_fe_write(&s->chr, &s->tx_data, 1);
    if (ret == 1 || ret < 0) {
        pc98_serial_tx_complete(s);
    } else if (!s->watch_tag) {
        s->watch_tag = qemu_chr_fe_add_watch(&s->chr,
                                             G_IO_OUT | G_IO_HUP,
                                             pc98_serial_watch_cb, s);
    }
}

static int pc98_serial_can_receive(void *opaque)
{
    Pc98SerialState *s = opaque;

    return (s->command & CMD_RX_ENABLE) && !s->rx_valid;
}

static void pc98_serial_receive(void *opaque, const uint8_t *buf, int size)
{
    Pc98SerialState *s = opaque;

    if (size <= 0) {
        return;
    }
    if (s->rx_valid) {
        s->status |= USART_OVERRUN;
    } else {
        s->data = buf[0];
        s->rx_valid = true;
        s->status |= USART_RXRDY;
    }
    pc98_serial_update_irq(s);
}

static void pc98_serial_event(void *opaque, QEMUChrEvent event)
{
    Pc98SerialState *s = opaque;

    if (event == CHR_EVENT_BREAK) {
        s->status |= USART_BREAK;
        pc98_serial_update_irq(s);
    }
}

static int pc98_serial_backend_changed(void *opaque)
{
    Pc98SerialState *s = opaque;

    qemu_chr_fe_set_handlers(&s->chr, pc98_serial_can_receive,
                             pc98_serial_receive, pc98_serial_event,
                             pc98_serial_backend_changed, s, NULL, true);
    pc98_serial_update_parameters(s);
    pc98_serial_update_tiocm(s);
    pc98_serial_update_modem(s);
    if (s->tx_pending) {
        pc98_serial_try_tx(s);
    }
    return 0;
}

static uint64_t pc98_serial_data_read(void *opaque, hwaddr addr,
                                     unsigned size)
{
    Pc98SerialState *s = opaque;
    uint8_t value = s->data;

    if (s->rx_valid) {
        s->rx_valid = false;
        s->status &= ~USART_RXRDY;
        qemu_chr_fe_accept_input(&s->chr);
    }
    pc98_serial_update_irq(s);
    return value;
}

static void pc98_serial_data_write(void *opaque, hwaddr addr, uint64_t value,
                                   unsigned size)
{
    Pc98SerialState *s = opaque;

    if (!(s->command & CMD_TX_ENABLE)) {
        return;
    }
    if (s->tx_pending) {
        return;
    }

    s->tx_data = value;
    s->tx_pending = true;
    s->status &= ~(USART_TXRDY | USART_TXEMPTY);
    pc98_serial_update_irq(s);
    pc98_serial_try_tx(s);
}

static uint64_t pc98_serial_status_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    Pc98SerialState *s = opaque;

    pc98_serial_update_modem(s);
    return s->status;
}

static void pc98_serial_command_write(void *opaque, hwaddr addr,
                                      uint64_t value, unsigned size)
{
    Pc98SerialState *s = opaque;
    uint8_t command = value;
    int break_enable;

    /*
     * PC-98 firmware commonly resets the 8251 with three dummy zero writes
     * followed by 0x40.  Recognise that sequence from any programming phase.
     */
    if (!(command & 0xfd)) {
        s->dummy_commands++;
    } else {
        if (s->dummy_commands >= 3 && command == CMD_RESET) {
            s->config_phase = 0;
        }
        s->dummy_commands = 0;
    }

    switch (s->config_phase) {
    case 0:
        s->status &= ~(USART_PARITY | USART_OVERRUN |
                       USART_FRAMING | USART_BREAK);
        s->config_phase = 1;
        break;
    case 1:
        s->mode = command;
        s->config_phase = 2;
        pc98_serial_update_parameters(s);
        break;
    default:
        if (command & CMD_RESET) {
            s->config_phase = 1;
            s->status &= ~(USART_PARITY | USART_OVERRUN |
                           USART_FRAMING | USART_BREAK);
        }
        if (command & CMD_ERROR_RST) {
            s->status &= ~(USART_PARITY | USART_OVERRUN |
                           USART_FRAMING | USART_BREAK);
        }
        s->command = command;
        break_enable = !!(command & CMD_BREAK);
        qemu_chr_fe_ioctl(&s->chr, CHR_IOCTL_SERIAL_SET_BREAK,
                          &break_enable);
        pc98_serial_update_tiocm(s);
        qemu_chr_fe_accept_input(&s->chr);
        break;
    }
    pc98_serial_update_irq(s);
}

static const MemoryRegionOps pc98_serial_data_ops = {
    .read = pc98_serial_data_read,
    .write = pc98_serial_data_write,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps pc98_serial_control_ops = {
    .read = pc98_serial_status_read,
    .write = pc98_serial_command_write,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void pc98_serial_reset(DeviceState *dev)
{
    Pc98SerialState *s = PC98_SERIAL(dev);

    g_clear_handle_id(&s->watch_tag, g_source_remove);
    s->data = 0xff;
    s->status = USART_TXRDY | USART_TXEMPTY | USART_DSR;
    s->mode = 0;
    s->command = CMD_TX_ENABLE | CMD_DTR | CMD_RX_ENABLE | CMD_RTS;
    s->config_phase = 0;
    s->dummy_commands = 0;
    s->rx_valid = false;
    s->tx_pending = false;
    s->tx_data = 0;
    pc98_serial_update_parameters(s);
    pc98_serial_update_tiocm(s);
    pc98_serial_update_irq(s);
}

static int pc98_serial_post_load(void *opaque, int version_id)
{
    Pc98SerialState *s = opaque;

    s->pit_divisor = pc98_pit_get_channel_count(s->pit, 2);
    pc98_serial_update_parameters(s);
    pc98_serial_update_tiocm(s);
    pc98_serial_update_irq(s);
    if (s->tx_pending) {
        pc98_serial_try_tx(s);
    }
    return 0;
}

static const VMStateDescription vmstate_pc98_serial = {
    .name = "pc98-serial",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = pc98_serial_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(pit_divisor, Pc98SerialState),
        VMSTATE_UINT8(irq_enable, Pc98SerialState),
        VMSTATE_UINT8(data, Pc98SerialState),
        VMSTATE_UINT8(status, Pc98SerialState),
        VMSTATE_UINT8(mode, Pc98SerialState),
        VMSTATE_UINT8(command, Pc98SerialState),
        VMSTATE_UINT8(config_phase, Pc98SerialState),
        VMSTATE_UINT8(dummy_commands, Pc98SerialState),
        VMSTATE_BOOL(rx_valid, Pc98SerialState),
        VMSTATE_BOOL(tx_pending, Pc98SerialState),
        VMSTATE_UINT8(tx_data, Pc98SerialState),
        VMSTATE_END_OF_LIST()
    },
};

static void pc98_serial_realize(DeviceState *dev, Error **errp)
{
    Pc98SerialState *s = PC98_SERIAL(dev);
    ISADevice *isadev = ISA_DEVICE(dev);
    Object *obj;
    int i;

    obj = object_resolve_type_unambiguous(TYPE_PC98_PIT, errp);
    if (!obj) {
        if (!*errp) {
            error_setg(errp, "pc98-serial requires a PC-98 machine");
        }
        return;
    }
    s->pit = ISA_DEVICE(obj);

    obj = object_resolve_type_unambiguous(TYPE_PC98_SYS, errp);
    if (!obj) {
        if (!*errp) {
            error_setg(errp, "pc98-serial requires the PC-98 system port");
        }
        return;
    }
    s->sys = PC98_SYS(obj);

    s->irq = isa_get_irq(isadev, s->isairq);
    memory_region_init_io(&s->data_io, OBJECT(s), &pc98_serial_data_ops,
                          s, "pc98-serial-data", 1);
    memory_region_init_io(&s->control_io, OBJECT(s),
                          &pc98_serial_control_ops, s,
                          "pc98-serial-control", 1);
    isa_register_ioport(isadev, &s->data_io, s->iobase);
    isa_register_ioport(isadev, &s->control_io, s->iobase + 2);
    qdev_set_legacy_instance_id(dev, s->iobase, 2);

    qdev_init_gpio_in_named(dev, pc98_serial_set_irq_enable,
                            "irq-enable", IRQ_COUNT);
    s->irq_enable = pc98_sys_get_portc(s->sys) & 7;
    for (i = 0; i < IRQ_COUNT; i++) {
        qdev_connect_gpio_out_named(
            DEVICE(s->sys), "serial-irq-enable", i,
            qdev_get_gpio_in_named(dev, "irq-enable", i));
    }

    qemu_chr_fe_set_handlers(&s->chr, pc98_serial_can_receive,
                             pc98_serial_receive, pc98_serial_event,
                             pc98_serial_backend_changed, s, NULL, true);
    pc98_pit_set_serial_clock_notifier(s->pit,
                                       pc98_serial_clock_changed, s);
}

static void pc98_serial_unrealize(DeviceState *dev)
{
    Pc98SerialState *s = PC98_SERIAL(dev);

    pc98_pit_set_serial_clock_notifier(s->pit, NULL, NULL);
    g_clear_handle_id(&s->watch_tag, g_source_remove);
    qemu_chr_fe_deinit(&s->chr, false);
}

static const Property pc98_serial_properties[] = {
    DEFINE_PROP_CHR("chardev", Pc98SerialState, chr),
    DEFINE_PROP_UINT32("iobase", Pc98SerialState, iobase, 0x30),
    DEFINE_PROP_UINT32("irq", Pc98SerialState, isairq, 4),
};

static void pc98_serial_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pc98_serial_realize;
    dc->unrealize = pc98_serial_unrealize;
    dc->vmsd = &vmstate_pc98_serial;
    dc->hotpluggable = false;
    dc->user_creatable = false;
    device_class_set_legacy_reset(dc, pc98_serial_reset);
    device_class_set_props(dc, pc98_serial_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo pc98_serial_info = {
    .name = TYPE_PC98_SERIAL,
    .parent = TYPE_ISA_DEVICE,
    .instance_size = sizeof(Pc98SerialState),
    .class_init = pc98_serial_class_init,
};

static void pc98_serial_register_types(void)
{
    type_register_static(&pc98_serial_info);
}

type_init(pc98_serial_register_types)
