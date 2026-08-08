/*
 * QEMU NEC PC-9821 IDE interface
 *
 * PC-98 support
 *   Copyright (c) 2009 TAKEDA, toshiya
 *
 * PC-98 support modernisation
 *   Copyright (c) 2026 Awe Morris
 *
 * This device is derived from the PC-98 model in the QEMU/9821 fork
 * (GPL, by TAKEDA toshiya) and has been reimplemented and
 * restructured for modern QEMU.  Its register-level behaviour was
 * cross-checked against the Neko Project II and NP21W emulators.
 *
 * The PC-98 built-in IDE has two "banks" (each an ATA channel with a
 * master/slave pair) multiplexed onto one register block: port 0x432
 * selects the active bank, port 0x430 reports the connection layout,
 * and the command block lives at 0x640-0x64e with a 2-byte stride,
 * control/status at 0x74c/0x74e.
 * It reuses the shared IDE core (register handlers take an IDEBus
 * opaque) but wires the PC-98 port map itself, so the PC/AT ISA IDE
 * model stays untouched.  Both ATA hard disks and ATAPI CD-ROMs are
 * supported (the shared core provides the ATAPI PACKET machinery);
 * a CD-ROM is attached with, e.g., -drive if=ide,media=cdrom.
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
#include "hw/core/cpu.h"
#include "hw/core/hotplug.h"
#include "hw/core/irq.h"
#include "hw/ide/ide-bus.h"
#include "hw/ide/ide-dev.h"
#include "hw/ide/pc98-ide.h"
#include "hw/isa/isa.h"
#include "migration/vmstate.h"
#include "system/block-backend.h"
#include "system/ioport.h"
#include "qom/object.h"
#include "ide-internal.h"

/* For debug. */
#define pc98_ide_trace(tag, reg, val)
#if 0
static void pc98_ide_trace(const char *tag, int reg, uint32_t val)
{
    static int en = -1;
    static int pc_en = -1;

    if (en < 0) {
        en = getenv("PC98_IDE_TRACE") != NULL;
        pc_en = getenv("PC98_IDE_TRACE_PC") != NULL;
    }
    if (!en) {
        return;
    }
    fprintf(stderr, "IDE %-5s %-8s =0x%02x",
            tag, reg >= 0 ? pc98_ide_regname[reg & 7] : "-", val & 0xff);
    if (pc_en && current_cpu) {
        fprintf(stderr, " pc=0x%" PRIx64,
                (uint64_t)current_cpu->cc->get_pc(current_cpu));
    }
    fputc('\n', stderr);
    fflush(stderr);
}
#endif

#define PC98_IDE_NBUS 2
#define PC98_IDE_HEADS 8
#define PC98_IDE_SECTORS 17
#define PC98_IDE_BANK_SECONDARY 0x01
#define PC98_IDE_BANK_DWORD     0x08
#define PC98_IDE_BANK_WRITABLE  0x39
#define PC98_IDE_CONN_WRITABLE  0x71
#define PC98_IDE_SLAVE_CAPABLE  0x40

struct Pc98IdeState {
    ISADevice parent_obj;

    IDEBus bus[PC98_IDE_NBUS];
    IDEBus *cur_bus;
    uint8_t bank_reg[2];
    uint8_t irq_levels;
    qemu_irq *bus_irq;
    qemu_irq irq;
    PortioList portio_list;
};

/*
 * Port 0x432 chooses which of the two ATA channels is mapped into the shared
 * command block.  Bit 3 enables DWORD transfers through its data register;
 * bit 6 reports that this controller supports slave devices.  Port 0x430 is
 * a separate connection/configuration register.  NP21W keeps independent
 * latches for the two ports and only 0x432 affects the selected channel.
 * A write with bit 7 set is a no-op guard.
 *
 * The bit 3 and bit 6 semantics follow drachen6jp's analysis of NEC BIOSes
 * and the Windows 2000 IDE driver.
 */
static void pc98_ide_chsel_write(void *opaque, uint32_t addr, uint32_t val)
{
    Pc98IdeState *s = opaque;
    unsigned reg = (addr >> 1) & 1;

    pc98_ide_trace(reg ? "bank" : "conn", -1, val);
    if (val & 0x80) {
        return;
    }
    if (reg) {
        s->bank_reg[1] = val & PC98_IDE_BANK_WRITABLE;
        s->cur_bus = &s->bus[val & PC98_IDE_BANK_SECONDARY];
    } else {
        s->bank_reg[0] = val & PC98_IDE_CONN_WRITABLE;
    }
}

static uint32_t pc98_ide_chsel_read(void *opaque, uint32_t addr)
{
    Pc98IdeState *s = opaque;
    unsigned reg = (addr >> 1) & 1;
    uint32_t ret;

    if (reg) {
        ret = s->bank_reg[1] | PC98_IDE_SLAVE_CAPABLE;
    } else {
        bool compatibility_mode;
        IDEBus *bus = s->cur_bus;

        /*
         * NEC's connection register returns bit 0 set in the normal layout
         * and bit 6 when the selected channel has a slave.  The one special
         * layout (HDDs on channel 0, ATAPI CD-ROM as channel 1 master) clears
         * bit 0 to advertise compatibility mode.
         */
        compatibility_mode =
            s->bus[0].ifs[0].drive_kind != IDE_CD &&
            s->bus[0].ifs[1].drive_kind != IDE_CD &&
            s->bus[1].ifs[0].blk &&
            s->bus[1].ifs[0].drive_kind == IDE_CD &&
            s->bus[1].ifs[1].drive_kind != IDE_CD;

        ret = compatibility_mode ? 0 : 1;
        if (bus->ifs[1].blk) {
            ret |= 0x40;
        }
        s->bank_reg[0] = ret;
    }

    pc98_ide_trace(reg ? "bank?" : "conn?", -1, ret);
    return ret;
}

/*
 * IRQ-source companion register.  Bits 0 and 1 report the live interrupt
 * levels of the primary and secondary channels, respectively.  Reading an
 * ATA status register deasserts the corresponding IDE-core IRQ level.  The
 * Xa7 BIOS writes 0x01 here after reading status; accepting that write as a
 * no-op matches the observed hardware-facing sequence without inventing a
 * second interrupt latch.
 *
 * This interpretation follows drachen6jp's IDE analysis and was confirmed by
 * tracing the Xa7 BIOS IRQ handler.  Port 0x435 remains zero until its purpose
 * is established independently.
 */
static void pc98_ide_aux_write(void *opaque, uint32_t addr, uint32_t val)
{
    pc98_ide_trace("auxwr", -1, val);
}

static uint32_t pc98_ide_aux_read(void *opaque, uint32_t addr)
{
    Pc98IdeState *s = opaque;
    uint32_t ret = 0;

    if (addr == 0x433) {
        ret = s->irq_levels & 0x03;
    }
    pc98_ide_trace("auxrd", -1, ret);
    return ret;
}

/*
 * Both IDE channels share PC-98 IRQ 9.  Feeding both level outputs directly
 * into one PIC input is not an OR connection: a deassertion from one idle
 * channel can hide an interrupt still asserted by the other.  Keep each
 * source level and drive the PIC with their logical OR.
 */
static void pc98_ide_irq(void *opaque, int n, int level)
{
    Pc98IdeState *s = opaque;

    if (level) {
        s->irq_levels |= 1 << n;
    } else {
        s->irq_levels &= ~(1 << n);
    }
    pc98_ide_trace(level ? "irq+" : "irq-", n, s->irq_levels);
    qemu_set_irq(s->irq, s->irq_levels != 0);
}

/*
 * The PC-98 built-in IDE BIOS polls the status register for exactly
 * READY_STAT | SEEK_STAT (0x50) after commands such as IDLE IMMEDIATE
 * (0xe1).  A real IDE drive keeps DSC (SEEK_STAT) asserted whenever it is
 * not mid-seek, but the shared IDE core leaves it clear for several
 * "no-op" commands and returns 0x40, which makes the BIOS spin until it
 * times out.  Re-assert DSC on reads whenever the drive is ready and idle;
 * this turns 0x40 into 0x50 while leaving DRQ/BSY/error states untouched.
 * Kept here so the shared IDE core is not modified.
 *
 * This is only correct for ATA hard disks.  On an ATAPI (CD-ROM) device the
 * DSC bit is command-specific, so the fixup is restricted to a real IDE_HD on
 * the active unit; ATAPI status is passed through unchanged.
 */
static inline uint32_t pc98_ide_fixup_status(IDEBus *bus, uint32_t st)
{
    IDEState *active = &bus->ifs[bus->unit];

    if (active->blk && active->drive_kind == IDE_HD &&
        (st & (BUSY_STAT | READY_STAT)) == READY_STAT) {
        st |= SEEK_STAT;
    }
    return st;
}

/* command block: 0x640 + reg*2 -> IDE register 'reg' on the current bank */
static void pc98_ide_cmd_write(void *opaque, uint32_t addr, uint32_t val)
{
    Pc98IdeState *s = opaque;
    unsigned reg = (addr - 0x640) >> 1;

    pc98_ide_trace("wr", reg, val);
    ide_ioport_write(s->cur_bus, reg, val);
}

static uint32_t pc98_ide_cmd_read(void *opaque, uint32_t addr)
{
    Pc98IdeState *s = opaque;
    IDEBus *bus = s->cur_bus;
    unsigned reg = (addr - 0x640) >> 1;
    unsigned unit = bus->unit;
    uint32_t ret;

    if (!bus->ifs[unit].blk) {
        /*
         * Selected device is absent.  If its sibling is present, that device
         * answers for it (ATA "device 0 responds for an absent device 1"), so
         * the firmware sees the master's signature/status on both units and
         * concludes there is no separate slave instead of polling forever.
         * If both are absent the bus floats high (0xff).
         */
        if (!bus->ifs[unit ^ 1].blk) {
            pc98_ide_trace("rd?", reg, 0xff);
            return 0xff;
        }
        bus->unit = unit ^ 1;
    }
    ret = ide_ioport_read(bus, reg);
    if (reg == 7) {
        ret = pc98_ide_fixup_status(bus, ret);
    }
    bus->unit = unit;
    pc98_ide_trace("rd", reg, ret);
    return ret;
}

static void pc98_ide_data_writew(void *opaque, uint32_t addr, uint32_t val)
{
    Pc98IdeState *s = opaque;

    ide_data_writew(s->cur_bus, 0, val);
}

static uint32_t pc98_ide_data_readw(void *opaque, uint32_t addr)
{
    Pc98IdeState *s = opaque;

    return ide_data_readw(s->cur_bus, 0);
}

static void pc98_ide_data_writel(void *opaque, uint32_t addr, uint32_t val)
{
    Pc98IdeState *s = opaque;

    if (!(s->bank_reg[1] & PC98_IDE_BANK_DWORD)) {
        pc98_ide_trace("wrl?", 0, val);
        return;
    }
    ide_data_writel(s->cur_bus, 0, val);
}

static uint32_t pc98_ide_data_readl(void *opaque, uint32_t addr)
{
    Pc98IdeState *s = opaque;

    if (!(s->bank_reg[1] & PC98_IDE_BANK_DWORD)) {
        pc98_ide_trace("rdl?", 0, 0xffffffff);
        return 0xffffffff;
    }
    return ide_data_readl(s->cur_bus, 0);
}

/* control/alt-status at 0x74c, drive-address register at 0x74e */
static void pc98_ide_ctrl_write(void *opaque, uint32_t addr, uint32_t val)
{
    Pc98IdeState *s = opaque;

    pc98_ide_trace("ctl", -1, val);
    ide_ctrl_write(s->cur_bus, 0, val);
}

static uint32_t pc98_ide_status_read(void *opaque, uint32_t addr)
{
    Pc98IdeState *s = opaque;
    IDEBus *bus = s->cur_bus;
    unsigned unit = bus->unit;
    uint32_t ret;

    if (!bus->ifs[unit].blk) {
        if (!bus->ifs[unit ^ 1].blk) {
            pc98_ide_trace("alt?", 7, 0xff);
            return 0xff;
        }
        bus->unit = unit ^ 1;
    }
    ret = pc98_ide_fixup_status(bus, ide_status_read(bus, 0));
    bus->unit = unit;
    pc98_ide_trace("alt", 7, ret);
    return ret;
}

/*
 * Drive-address register: the two high bits float high, the head-select
 * nibble appears inverted in bits 5..2, and the low two bits flag which
 * device of the pair is active (bit 1 master, bit 0 slave).
 */
static uint32_t pc98_ide_drive_addr_read(void *opaque, uint32_t addr)
{
    Pc98IdeState *s = opaque;
    IDEBus *bus = s->cur_bus;
    IDEState *active = &bus->ifs[bus->unit];
    uint32_t value = 0xc0;

    value |= (~active->select & 0x0f) << 2;
    value |= bus->unit ? 0x01 : 0x02;
    return value;
}

static const MemoryRegionPortio pc98_ide_portio[] = {
    { 0x430, 1, 1, .read = pc98_ide_chsel_read, .write = pc98_ide_chsel_write },
    { 0x432, 1, 1, .read = pc98_ide_chsel_read, .write = pc98_ide_chsel_write },
    { 0x433, 1, 1, .read = pc98_ide_aux_read, .write = pc98_ide_aux_write },
    { 0x435, 1, 1, .read = pc98_ide_aux_read, .write = pc98_ide_aux_write },
    { 0x640, 8, 2, .read = pc98_ide_data_readw, .write = pc98_ide_data_writew },
    { 0x640, 1, 4, .read = pc98_ide_data_readl, .write = pc98_ide_data_writel },
    { 0x640, 1, 1, .read = pc98_ide_cmd_read, .write = pc98_ide_cmd_write },
    { 0x642, 1, 1, .read = pc98_ide_cmd_read, .write = pc98_ide_cmd_write },
    { 0x644, 1, 1, .read = pc98_ide_cmd_read, .write = pc98_ide_cmd_write },
    { 0x646, 1, 1, .read = pc98_ide_cmd_read, .write = pc98_ide_cmd_write },
    { 0x648, 1, 1, .read = pc98_ide_cmd_read, .write = pc98_ide_cmd_write },
    { 0x64a, 1, 1, .read = pc98_ide_cmd_read, .write = pc98_ide_cmd_write },
    { 0x64c, 1, 1, .read = pc98_ide_cmd_read, .write = pc98_ide_cmd_write },
    { 0x64e, 1, 1, .read = pc98_ide_cmd_read, .write = pc98_ide_cmd_write },
    { 0x74c, 1, 1, .read = pc98_ide_status_read, .write = pc98_ide_ctrl_write },
    { 0x74e, 1, 1, .read = pc98_ide_drive_addr_read },
    PORTIO_END_OF_LIST(),
};

uint8_t pc98_ide_connected(Pc98IdeState *s)
{
    uint8_t ret = 0;
    int b, u;

    for (b = 0; b < PC98_IDE_NBUS; b++) {
        for (u = 0; u < 2; u++) {
            IDEState *ide = &s->bus[b].ifs[u];
            if (ide->blk && ide->drive_kind == IDE_HD) {
                ret |= 1 << (b * 2 + u);
            }
        }
    }
    return ret;
}

static void pc98_ide_reset(DeviceState *dev)
{
    Pc98IdeState *s = PC98_IDE(dev);
    int b;

    s->bank_reg[0] = 0;
    s->bank_reg[1] = 0;
    s->irq_levels = 0;
    qemu_set_irq(s->irq, 0);
    for (b = 0; b < PC98_IDE_NBUS; b++) {
        ide_bus_reset(&s->bus[b]);
    }
    s->cur_bus = &s->bus[0];
}

static int pc98_ide_post_load(void *opaque, int version_id)
{
    Pc98IdeState *s = opaque;

    s->cur_bus = &s->bus[s->bank_reg[1] & PC98_IDE_BANK_SECONDARY];
    qemu_set_irq(s->irq, s->irq_levels != 0);
    return 0;
}

static const VMStateDescription vmstate_pc98_ide = {
    .name = "pc98-ide",
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = pc98_ide_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_IDE_BUS(bus[0], Pc98IdeState),
        VMSTATE_IDE_DRIVES(bus[0].ifs, Pc98IdeState),
        VMSTATE_IDE_BUS(bus[1], Pc98IdeState),
        VMSTATE_IDE_DRIVES(bus[1].ifs, Pc98IdeState),
        VMSTATE_UINT8_ARRAY_V(bank_reg, Pc98IdeState, 2, 2),
        VMSTATE_UINT8_V(irq_levels, Pc98IdeState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void pc98_ide_pre_plug(HotplugHandler *hotplug_dev,
                              DeviceState *dev, Error **errp)
{
    IDEDevice *ide;
    int64_t bytes;
//    uint64_t sectors, cylinders;
//    uint32_t cyls;

    if (!object_dynamic_cast(OBJECT(dev), "ide-hd")) {
        return;
    }

    ide = IDE_DEVICE(dev);
    if (ide->conf.cyls || ide->conf.heads || ide->conf.secs ||
        !ide->conf.blk) {
        return;
    }

    bytes = blk_getlength(ide->conf.blk);
    if (bytes < 0) {
        error_setg_errno(errp, -bytes,
                         "Could not determine PC-98 IDE disk size");
        return;
    }
#if 0
    sectors = bytes / BDRV_SECTOR_SIZE;
    cylinders = sectors / (PC98_IDE_HEADS * PC98_IDE_SECTORS);
    if (cylinders < 1) {
        cyls = 1;
    } else if (cylinders > 65535) {
        cyls = 65535;
    } else {
        cyls = cylinders;
    }

    /*
     * Generic IDE otherwise advertises 16 heads and 63 sectors.  NEC DOS
     * uses IDENTIFY geometry to interpret the PC-98 partition table, whose
     * fixed-disk convention is 8 heads by 17 sectors.
     */
    ide->conf.cyls = cyls;
    ide->conf.heads = PC98_IDE_HEADS;
    ide->conf.secs = PC98_IDE_SECTORS;
#endif
}

static void pc98_ide_realize(DeviceState *dev, Error **errp)
{
    ISADevice *isadev = ISA_DEVICE(dev);
    Pc98IdeState *s = PC98_IDE(dev);
    int b;

    s->bus_irq = qemu_allocate_irqs(pc98_ide_irq, s, PC98_IDE_NBUS);
    for (b = 0; b < PC98_IDE_NBUS; b++) {
        ide_bus_init(&s->bus[b], sizeof(s->bus[b]), dev, b, 2);
        qbus_set_hotplug_handler(BUS(&s->bus[b]), OBJECT(dev));
        ide_bus_init_output_irq(&s->bus[b], s->bus_irq[b]);
        ide_bus_register_restart_cb(&s->bus[b]);
    }
    s->cur_bus = &s->bus[0];

    isa_register_portio_list(isadev, &s->portio_list, 0,
                             pc98_ide_portio, s, "pc98-ide");
}

static void pc98_ide_finalize(Object *obj)
{
    Pc98IdeState *s = PC98_IDE(obj);

    if (s->bus_irq) {
        qemu_free_irqs(s->bus_irq, PC98_IDE_NBUS);
    }
}

static void pc98_ide_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    HotplugHandlerClass *hc = HOTPLUG_HANDLER_CLASS(klass);

    dc->realize = pc98_ide_realize;
    device_class_set_legacy_reset(dc, pc98_ide_reset);
    dc->vmsd = &vmstate_pc98_ide;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    dc->user_creatable = false;
    hc->pre_plug = pc98_ide_pre_plug;
}

static const TypeInfo pc98_ide_info = {
    .name          = TYPE_PC98_IDE,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(Pc98IdeState),
    .instance_finalize = pc98_ide_finalize,
    .class_init    = pc98_ide_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_HOTPLUG_HANDLER },
        { }
    },
};

static void pc98_ide_register_types(void)
{
    type_register_static(&pc98_ide_info);
}

type_init(pc98_ide_register_types)

ISADevice *pc98_ide_init(ISABus *bus, DriveInfo **hd_table, qemu_irq irq)
{
    DeviceState *dev;
    ISADevice *isadev;
    Pc98IdeState *s;
    int i;

    isadev = isa_new(TYPE_PC98_IDE);
    dev = DEVICE(isadev);
    s = PC98_IDE(dev);
    s->irq = irq;
    isa_realize_and_unref(isadev, bus, &error_fatal);

    /* bank1 -> drives 0,1 ; bank2 -> drives 2,3 */
    for (i = 0; i < 4; i++) {
        if (hd_table[i]) {
            ide_bus_create_drive(&s->bus[i / 2], i % 2, hd_table[i]);
        }
    }
    for (i = 0; i < PC98_IDE_NBUS; i++) {
        ide_bus_reset(&s->bus[i]);
    }

    return isadev;
}
