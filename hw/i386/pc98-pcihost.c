/*
 * QEMU NEC PC-9821 PCI host bridge
 *
 * Copyright (c) 2026 Awe Morris
 *
 * A minimal PCI host bridge for the PCI-equipped PC-9821 machines
 * (e.g. Xa7/C9W).  PC-98 uses PCI Configuration Mechanism #1 with the
 * same I/O ports as the PC/AT: CONFIG_ADDRESS at 0xCF8 and CONFIG_DATA
 * at 0xCFC.  The host bridge (device 0) is presented as an Intel
 * 82441FX-compatible PMC (8086:1237), matching what NP21W exposes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qapi/error.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_host.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_ids.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/i386/pc98.h"
#include "system/address-spaces.h"

/* -------------------------------------------------------------------- */
/* Host bridge PCI function (device 0): Intel 82441FX-compatible PMC    */
/* -------------------------------------------------------------------- */

#define TYPE_PC98_PMC "pc98-pmc"

struct Pc98PmcState {
    PCIDevice parent_obj;
};

OBJECT_DECLARE_SIMPLE_TYPE(Pc98PmcState, PC98_PMC)

/*
 * Target for the D000-segment shadow control (config register 0x64).  There
 * is a single PC-98 machine instance, so a file-scope pointer to the memory
 * controller state is enough; the machine wires it up after both objects
 * exist via pc98_pci_set_d000_mem().
 */
static void *pc98_d000_mem;

void pc98_pci_set_d000_mem(void *mem)
{
    pc98_d000_mem = mem;
}

PCIBus *pc98_pci_get_bus(DeviceState *host)
{
    return PCI_HOST_BRIDGE(host)->bus;
}

static void pc98_pmc_realize(PCIDevice *dev, Error **errp)
{
    /*
     * The default configuration-space header (vendor/device/class from the
     * class_init below) identifies the host bridge during PCI enumeration.
     * Register-0x64 D000-window handling is done in the config-write hook.
     */
}

static void pc98_pmc_write_config(PCIDevice *dev, uint32_t addr,
                                  uint32_t val, int len)
{
    uint8_t old_bios_probe_gate = dev->config[0x69];

    pci_default_write_config(dev, addr, val, len);

    /*
     * The Xa7 ITF briefly sets config byte 0x69 bit 4, writes its IDE probe
     * mask from work area 0x5ba to physical 0xf8e90, then clears the bit.
     * The location behaves as a writable latch over the banked BIOS window;
     * forward the gate so the memory controller can expose it only then.
     */
    if (pc98_d000_mem && addr <= 0x69 && addr + len > 0x69 &&
        old_bios_probe_gate != dev->config[0x69]) {
        pc98_mem_set_bios_probe_write(pc98_d000_mem,
                                      (dev->config[0x69] & 0x10) != 0);
    }

    /*
     * Config register 0x64, top byte (0x67), is the D000-segment shadow
     * control (matching NP21W's setRAM_D000).  Forward it whenever a write
     * covers that byte.
     */
    if (pc98_d000_mem && addr <= 0x67 && addr + len > 0x67) {
        pc98_mem_set_d000_shadow(pc98_d000_mem, dev->config[0x67]);
    }
}

static void pc98_pmc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = pc98_pmc_realize;
    k->config_write = pc98_pmc_write_config;
    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = PCI_DEVICE_ID_INTEL_82441;
    k->revision = 0x02;
    k->class_id = PCI_CLASS_BRIDGE_HOST;
    dc->desc = "PC-98 host bridge (PMC)";
    /* Part of the host bridge; cannot be created or removed on its own. */
    dc->user_creatable = false;
    dc->hotpluggable = false;
}

static const TypeInfo pc98_pmc_info = {
    .name          = TYPE_PC98_PMC,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(Pc98PmcState),
    .class_init    = pc98_pmc_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

/* -------------------------------------------------------------------- */
/* NEC PCI-to-C-bus bridge (device 6)                                   */
/* -------------------------------------------------------------------- */

#define TYPE_PC98_CBUS_BRIDGE "pc98-cbus-bridge"

struct Pc98CbusBridgeState {
    PCIDevice parent_obj;
};

OBJECT_DECLARE_SIMPLE_TYPE(Pc98CbusBridgeState, PC98_CBUS_BRIDGE)

static void pc98_cbus_bridge_realize(PCIDevice *dev, Error **errp)
{
    static const uint8_t bridge_regs_40_63[] = {
        0x10, 0x00, 0xef, 0x00, 0xfa, 0xff, 0xfb, 0xff,
        0xfe, 0xff, 0xfe, 0xff, 0xff, 0xff, 0x00, 0x00,
        0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x09, 0x0a, 0x0b, 0x00, 0x0d, 0x00, 0x00,
        0x03, 0x05, 0x06, 0x0c,
    };

    /*
     * These reset values match the on-board NEC PCI-to-C-bus bridge
     * exposed by NP21/W.  In particular, Windows NT-family PC-98 kernels
     * use this function to discover the legacy C-bus/ISA side of the
     * machine.  Without it, IoReportResourceForDetection() rejects every
     * ISA I/O resource and the native ATAPI driver cannot start.
     */
    pci_set_word(dev->config + PCI_COMMAND, 0x010f);
    pci_set_word(dev->config + PCI_STATUS, 0x0200);
    dev->config[PCI_INTERRUPT_PIN] = 1;
    memcpy(dev->config + 0x40, bridge_regs_40_63,
           sizeof(bridge_regs_40_63));
}

static void pc98_cbus_bridge_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = pc98_cbus_bridge_realize;
    k->vendor_id = PCI_VENDOR_ID_NEC;
    k->device_id = 0x0001;
    k->revision = 0x01;
    k->class_id = PCI_CLASS_BRIDGE_OTHER;
    dc->desc = "NEC PCI-to-C-bus bridge";
    dc->user_creatable = false;
    dc->hotpluggable = false;
}

static const TypeInfo pc98_cbus_bridge_info = {
    .name          = TYPE_PC98_CBUS_BRIDGE,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(Pc98CbusBridgeState),
    .class_init    = pc98_cbus_bridge_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

/* -------------------------------------------------------------------- */
/* Host bridge                                                          */
/* -------------------------------------------------------------------- */

struct Pc98PciHostState {
    PCIHostState parent_obj;

    MemoryRegion pci_mem;   /* PCI MMIO address space (no BAR users yet) */
    qemu_irq intx[PCI_NUM_PINS];
};

OBJECT_DECLARE_SIMPLE_TYPE(Pc98PciHostState, PC98_PCI_HOST)

/*
 * INTx routing.  No built-in PCI function asserts an interrupt in the
 * current configuration, so this is a placeholder: all four PCI
 * interrupt pins are collapsed onto a single host output line that the
 * machine wires to a free PC-98 IRQ.  Real PC-9821 routing tables will
 * replace this when an interrupt-driven PCI device is added.
 */
static int pc98_pci_map_irq(PCIDevice *pci_dev, int pin)
{
    return pin;
}

static void pc98_pci_set_irq(void *opaque, int irq_num, int level)
{
    qemu_irq *pins = opaque;

    qemu_set_irq(pins[irq_num], level);
}

/*
 * Optional configuration-cycle trace, gated on the PC98_PCI_TRACE
 * environment variable.  This answers "does the firmware touch a given
 * IDSEL during POST" without a debugger.  The address register (0xCF8)
 * uses the stock handler; only the data window (0xCFC) is wrapped.
 */
static bool pc98_pci_trace_enabled(void)
{
    static int cached = -1;

    if (cached < 0) {
        cached = getenv("PC98_PCI_TRACE") != NULL;
    }
    return cached;
}

static void pc98_pci_data_write(void *opaque, hwaddr addr,
                                uint64_t val, unsigned len)
{
    PCIHostState *s = opaque;
    uint32_t cfg;

    if (!(s->config_reg & (1u << 31))) {
        return;
    }
    cfg = s->config_reg | (addr & 3);
    if (pc98_pci_trace_enabled()) {
        fprintf(stderr, "PC98PCI wr bus=%u dev=%u fn=%u reg=0x%02x "
                "len=%u val=0x%08x\n",
                extract32(cfg, 16, 8), extract32(cfg, 11, 5),
                extract32(cfg, 8, 3), cfg & 0xff, len, (uint32_t)val);
        fflush(stderr);
    }
    pci_data_write(s->bus, cfg, val, len);
}

static uint64_t pc98_pci_data_read(void *opaque, hwaddr addr, unsigned len)
{
    PCIHostState *s = opaque;
    uint32_t cfg;
    uint32_t val;

    if (!(s->config_reg & (1u << 31))) {
        return 0xffffffff;
    }
    cfg = s->config_reg | (addr & 3);
    val = pci_data_read(s->bus, cfg, len);
    if (pc98_pci_trace_enabled()) {
        fprintf(stderr, "PC98PCI rd bus=%u dev=%u fn=%u reg=0x%02x "
                "len=%u -> 0x%08x\n",
                extract32(cfg, 16, 8), extract32(cfg, 11, 5),
                extract32(cfg, 8, 3), cfg & 0xff, len, val);
        fflush(stderr);
    }
    return val;
}

static const MemoryRegionOps pc98_pci_data_ops = {
    .read = pc98_pci_data_read,
    .write = pc98_pci_data_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void pc98_pcihost_realize(DeviceState *dev, Error **errp)
{
    Pc98PciHostState *s = PC98_PCI_HOST(dev);
    PCIHostState *phb = PCI_HOST_BRIDGE(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    PCIBus *b;
    int i;

    for (i = 0; i < PCI_NUM_PINS; i++) {
        sysbus_init_irq(sbd, &s->intx[i]);
    }

    /* CONFIG_ADDRESS (0xCF8, dword) and CONFIG_DATA (0xCFC) */
    memory_region_init_io(&phb->conf_mem, OBJECT(dev), &pci_host_conf_le_ops,
                          phb, "pc98-pci-conf-idx", 4);
    memory_region_init_io(&phb->data_mem, OBJECT(dev), &pc98_pci_data_ops,
                          phb, "pc98-pci-conf-data", 4);
    memory_region_add_subregion(get_system_io(), 0xcf8, &phb->conf_mem);
    sysbus_init_ioports(sbd, 0xcf8, 4);
    memory_region_add_subregion(get_system_io(), 0xcfc, &phb->data_mem);
    sysbus_init_ioports(sbd, 0xcfc, 4);

    /* PCI MMIO space.  Standalone for now: no built-in function has a BAR. */
    memory_region_init(&s->pci_mem, OBJECT(dev), "pc98-pci-mem", UINT64_MAX);

    b = pci_register_root_bus(dev, "pci.0", pc98_pci_set_irq,
                              pc98_pci_map_irq, s->intx,
                              &s->pci_mem, get_system_io(),
                              0, PCI_NUM_PINS, TYPE_PCI_BUS);
    phb->bus = b;

    /* device 0: host bridge / PMC */
    pci_create_simple(b, PCI_DEVFN(0, 0), TYPE_PC98_PMC);

    /* device 6: on-board bridge from PCI to the legacy C-bus */
    pci_create_simple(b, PCI_DEVFN(6, 0), TYPE_PC98_CBUS_BRIDGE);
}

static void pc98_pcihost_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pc98_pcihost_realize;
    dc->fw_name = "pci";
    /* Instantiated by the pc98-pci machine, not by the user. */
    dc->user_creatable = false;
}

static const TypeInfo pc98_pcihost_info = {
    .name          = TYPE_PC98_PCI_HOST,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(Pc98PciHostState),
    .class_init    = pc98_pcihost_class_init,
};

static void pc98_pcihost_register_types(void)
{
    type_register_static(&pc98_pmc_info);
    type_register_static(&pc98_cbus_bridge_info);
    type_register_static(&pc98_pcihost_info);
}
type_init(pc98_pcihost_register_types)
