/*
 * NEC PC-9821 Core-Graph bridge with an internal Cirrus GD5440
 *
 * Copyright (c) 2026 Awe Morris
 *
 * Core-Graph is the PCI-visible function (1033:0009).  The Cirrus chip sits
 * on the bridge's private bus: it has no PCI configuration function or BAR.
 * NEC exposes it through the fixed 0xFAA/0xFAB control interface, relocated
 * VGA ports, and a host aperture selected by control register 2.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/display/pc98-coregraph.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_ids.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "ui/console.h"
#include "cirrus_vga_internal.h"
#include "qom/object.h"
#include "trace.h"
#include "vga_regs.h"

#define COREGRAPH_ID              0x5b
#define COREGRAPH_LFB_SIZE        (1 * MiB)
#define COREGRAPH_LEGACY_SIZE     0x8000

typedef struct Pc98CoreGraphState {
    PCIDevice parent_obj;

    CirrusVGAState cirrus;

    MemoryRegion control_io;
    MemoryRegion wait_io;
    MemoryRegion io_ca0;
    MemoryRegion io_ba4;
    MemoryRegion io_baa;
    MemoryRegion io_da4;
    MemoryRegion io_daa;
    MemoryRegion linear_alias;
    MemoryRegion legacy_alias;

    uint8_t index;
    uint8_t regs[5];
    bool linear_mapped;
    bool legacy_mapped;
} Pc98CoreGraphState;

OBJECT_DECLARE_SIMPLE_TYPE(Pc98CoreGraphState, PC98_COREGRAPH)

static hwaddr coregraph_legacy_base(uint8_t value)
{
    switch (value) {
    case 0x10:
        return 0x000b0000;
    case 0xa0:
        return 0x00f00000;
    case 0x80:
        return 0x00f20000;
    case 0xc0:
        return 0x00f40000;
    case 0xe0:
        return 0x00f60000;
    default:
        return 0;
    }
}

static void coregraph_apply_mappings(Pc98CoreGraphState *s)
{
    MemoryRegion *system_memory = get_system_memory();
    bool enabled = (s->regs[3] & 0x01) != 0;
    hwaddr legacy_base = coregraph_legacy_base(s->regs[1]);
    hwaddr linear_base = (hwaddr)s->regs[2] << 24;

    memory_region_transaction_begin();

    /*
     * The relocated register block is always decoded.  In particular,
     * ACLMM.VXD calibrates itself from the vertical-retrace bit at 0xDAA
     * before it sets control register 3.  Hiding the I/O block until reg03
     * bit 0 was set made that calibration execute a 2^32-iteration LOOPNE
     * for every sample.  The access bit gates the host memory apertures, not
     * the register decode needed to turn the accelerator on.
     */
    memory_region_set_enabled(&s->io_ca0, true);
    memory_region_set_enabled(&s->io_ba4, true);
    memory_region_set_enabled(&s->io_baa, true);
    memory_region_set_enabled(&s->io_da4, true);
    memory_region_set_enabled(&s->io_daa, true);

    if (s->linear_mapped) {
        memory_region_del_subregion(system_memory, &s->linear_alias);
        s->linear_mapped = false;
    }
    if (enabled && linear_base) {
        memory_region_add_subregion_overlap(system_memory, linear_base,
                                            &s->linear_alias, 2);
        s->linear_mapped = true;
    }

    if (s->legacy_mapped) {
        memory_region_del_subregion(system_memory, &s->legacy_alias);
        s->legacy_mapped = false;
    }
    if (enabled && legacy_base) {
        memory_region_add_subregion_overlap(system_memory, legacy_base,
                                            &s->legacy_alias, 2);
        s->legacy_mapped = true;
    }

    memory_region_transaction_commit();
}

static uint64_t coregraph_control_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    Pc98CoreGraphState *s = opaque;

    if ((addr & 1) == 0) {
        trace_pc98_coregraph_control_read(s->index, s->index);
        return s->index;
    }
    if (s->index == 0) {
        trace_pc98_coregraph_control_read(s->index, COREGRAPH_ID);
        return COREGRAPH_ID;
    }
    if (s->index < ARRAY_SIZE(s->regs)) {
        trace_pc98_coregraph_control_read(s->index, s->regs[s->index]);
        return s->regs[s->index];
    }
    trace_pc98_coregraph_control_read(s->index, 0xff);
    return 0xff;
}

static void coregraph_control_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned size)
{
    Pc98CoreGraphState *s = opaque;

    if ((addr & 1) == 0) {
        s->index = value;
        trace_pc98_coregraph_index_write(s->index);
        return;
    }
    if (s->index > 0 && s->index < ARRAY_SIZE(s->regs)) {
        s->regs[s->index] = value;
        trace_pc98_coregraph_control_write(s->index, value);
        coregraph_apply_mappings(s);
    }
}

static const MemoryRegionOps coregraph_control_ops = {
    .read = coregraph_control_read,
    .write = coregraph_control_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static uint64_t coregraph_wait_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0xff;
}

static void coregraph_wait_write(void *opaque, hwaddr addr,
                                 uint64_t value, unsigned size)
{
}

static const MemoryRegionOps coregraph_wait_ops = {
    .read = coregraph_wait_read,
    .write = coregraph_wait_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void coregraph_reset(DeviceState *dev)
{
    Pc98CoreGraphState *s = PC98_COREGRAPH(dev);

    s->index = 0;
    memset(s->regs, 0, sizeof(s->regs));

    /*
     * The motherboard firmware leaves a valid banked window selected.
     * ACLMM reads the value while classifying the fixed-interface path.
     * 0xe0 selects the 0xf60000 window used as the generic built-in GD54xx
     * reset value by NP21/W.
     */
    s->regs[1] = 0xe0;
    coregraph_apply_mappings(s);
}

static int coregraph_post_load(void *opaque, int version_id)
{
    coregraph_apply_mappings(opaque);
    return 0;
}

static const VMStateDescription vmstate_pc98_coregraph = {
    .name = "pc98-coregraph",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = coregraph_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, Pc98CoreGraphState),
        VMSTATE_STRUCT(cirrus, Pc98CoreGraphState, 0,
                       vmstate_cirrus_vga, CirrusVGAState),
        VMSTATE_UINT8(index, Pc98CoreGraphState),
        VMSTATE_UINT8_ARRAY(regs, Pc98CoreGraphState, 5),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * The Cirrus RAM block is private to Core-Graph and is reached through an
 * I/O aperture rather than a direct RAM alias.  Force a full scanout update:
 * dirty-log listeners otherwise do not see every write to that private RAM
 * block when it is not directly present in the system address space.
 */
static bool coregraph_gfx_update(void *opaque)
{
    Pc98CoreGraphState *s = opaque;
    VGACommonState *vga = &s->cirrus.vga;
    uint8_t gr5;
    bool result;

    /*
     * NEC's Windows 95 Core-Graph driver leaves GR05's VGA shift field
     * cleared after selecting an SR07 packed-pixel mode.  The board still
     * scans the aperture linearly; QEMU's generic VGA renderer otherwise
     * interprets it as planar.  Apply the board-side scanout semantics
     * without changing the guest-visible Cirrus register.
     */
    gr5 = vga->gr[VGA_GFX_MODE];
    if ((s->regs[3] & 0x02) && (vga->sr[0x07] & 0x01)) {
        vga->gr[VGA_GFX_MODE] = (gr5 & ~0x60) | 0x40;
    }
    result = vga->hw_ops->gfx_update(vga);
    vga->gr[VGA_GFX_MODE] = gr5;
    return result;
}

static void coregraph_invalidate(void *opaque)
{
    Pc98CoreGraphState *s = opaque;
    VGACommonState *vga = &s->cirrus.vga;

    vga->hw_ops->invalidate(vga);
}

static const GraphicHwOps coregraph_hw_ops = {
    .invalidate = coregraph_invalidate,
    .gfx_update = coregraph_gfx_update,
};

static void coregraph_init_io_alias(MemoryRegion *alias, Object *owner,
                                    const char *name, MemoryRegion *source,
                                    hwaddr offset, hwaddr size,
                                    hwaddr target)
{
    memory_region_init_alias(alias, owner, name, source, offset, size);
    memory_region_set_enabled(alias, false);
    memory_region_add_subregion(get_system_io(), target, alias);
}

static void coregraph_realize(PCIDevice *dev, Error **errp)
{
    Pc98CoreGraphState *s = PC98_COREGRAPH(dev);
    CirrusVGAState *c = &s->cirrus;
    Object *owner = OBJECT(dev);

    c->vga.vram_size_mb = 2;
    c->enable_blitter = true;
    if (!vga_common_init(&c->vga, owner, errp)) {
        return;
    }

    /*
     * CR27=A0 identifies the GD5440 on verified Core-Graph machines.  QEMU's
     * GD5430 core uses that same chip ID and supplies the required Alpine
     * register set and BitBLT engine.
     */
    cirrus_init_common(c, owner, CIRRUS_ID_CLGD5430, 0,
                       get_system_memory(), get_system_io());

    /*
     * Undo the PC/AT-facing mappings made by the reusable core.  Core-Graph
     * exposes neither standard VGA I/O nor A0000 VGA memory directly.
     */
    memory_region_del_subregion(get_system_io(), &c->cirrus_vga_io);
    memory_region_del_subregion(get_system_memory(), &c->low_mem_container);

    /*
     * On the verified 1 MiB host window, SR17 enables MMIO in its final
     * 256 bytes even though the Cirrus itself has 2 MiB of VRAM.
     */
    c->linear_mmio_mask = COREGRAPH_LFB_SIZE - 256;

    coregraph_init_io_alias(&s->io_ca0, owner, "coregraph-io-ca0",
                            &c->cirrus_vga_io, 0x10, 0x10, 0x0ca0);
    coregraph_init_io_alias(&s->io_ba4, owner, "coregraph-io-ba4",
                            &c->cirrus_vga_io, 0x04, 2, 0x0ba4);
    coregraph_init_io_alias(&s->io_baa, owner, "coregraph-io-baa",
                            &c->cirrus_vga_io, 0x0a, 1, 0x0baa);
    coregraph_init_io_alias(&s->io_da4, owner, "coregraph-io-da4",
                            &c->cirrus_vga_io, 0x24, 2, 0x0da4);
    coregraph_init_io_alias(&s->io_daa, owner, "coregraph-io-daa",
                            &c->cirrus_vga_io, 0x2a, 1, 0x0daa);

    memory_region_init_io(&s->control_io, owner, &coregraph_control_ops, s,
                          "coregraph-control", 2);
    memory_region_add_subregion(get_system_io(), 0x0faa, &s->control_io);
    memory_region_init_io(&s->wait_io, owner, &coregraph_wait_ops, s,
                          "coregraph-wait", 1);
    memory_region_add_subregion(get_system_io(), 0x005f, &s->wait_io);

    memory_region_init_alias(&s->linear_alias, owner, "coregraph-linear",
                             &c->cirrus_linear_io, 0, COREGRAPH_LFB_SIZE);
    memory_region_init_alias(&s->legacy_alias, owner, "coregraph-legacy",
                             &c->low_mem_container, 0,
                             COREGRAPH_LEGACY_SIZE);

    c->vga.con = qemu_graphic_console_create(DEVICE(dev), 0,
                                              &coregraph_hw_ops, s);
    coregraph_reset(DEVICE(dev));
}

static void coregraph_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);

    pc->realize = coregraph_realize;
    pc->vendor_id = PCI_VENDOR_ID_NEC;
    pc->device_id = 0x0009;
    pc->revision = 0x01;
    pc->class_id = PCI_CLASS_DISPLAY_OTHER;

    dc->desc = "NEC Core-Graph bridge with internal Cirrus GD5440";
    dc->vmsd = &vmstate_pc98_coregraph;
    device_class_set_legacy_reset(dc, coregraph_reset);
    dc->user_creatable = false;
    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo coregraph_info = {
    .name = TYPE_PC98_COREGRAPH,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(Pc98CoreGraphState),
    .class_init = coregraph_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void coregraph_register_types(void)
{
    type_register_static(&coregraph_info);
}

type_init(coregraph_register_types)
