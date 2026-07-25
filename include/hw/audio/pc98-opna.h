/*
 * QEMU NEC PC-9801-86 sound board (YM2608 OPNA + YM2149 SSG)
 * Copyright (c) 2026 Awe Morris
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_AUDIO_PC98_OPNA_H
#define HW_AUDIO_PC98_OPNA_H

#include "hw/isa/isa.h"

#define TYPE_PC98_OPNA "pc98-opna"

/*
 * Create the PC-9801-86 sound board: a Yamaha YM2608 (OPNA) whose four
 * FM/SSG registers are mapped onto the PC-98 port block at 0x188-0x18E, with
 * the FM timer interrupt wired to PC-98 IRQ 3 (INT0).  The SSG section is
 * provided by the emu2149 YM2149 core.
 */
void pc98_opna_init(ISABus *bus, qemu_irq irq);

#endif
