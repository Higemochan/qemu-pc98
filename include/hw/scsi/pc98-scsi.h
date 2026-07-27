/*
 * NEC PC-9801-92 compatible SCSI host adapter
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_SCSI_PC98_SCSI_H
#define HW_SCSI_PC98_SCSI_H

#include "hw/isa/isa.h"

typedef struct Pc98MemState Pc98MemState;

#define TYPE_PC98_SCSI "pc98-scsi"

ISADevice *pc98_scsi_init(ISABus *bus, Pc98MemState *mem);

#endif
