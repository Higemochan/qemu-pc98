/*
 * NEC PC-9821 Core-Graph bridge
 *
 * Copyright (c) 2026 Awe Morris
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DISPLAY_PC98_COREGRAPH_H
#define HW_DISPLAY_PC98_COREGRAPH_H

#include "qemu/typedefs.h"
#include "hw/display/pc98-vga.h"

#define TYPE_PC98_COREGRAPH "pc98-coregraph"

void pc98_coregraph_set_primary_vga(PCIDevice *dev, Pc98VgaState *vga);

#endif
