/*
 * QEMU Crystal CS4231A audio codec
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef HW_AUDIO_CS4231A_H
#define HW_AUDIO_CS4231A_H

#include "hw/isa/isa.h"

#define TYPE_CS4231A "cs4231a"

/*
 * Change the ISA resources selected by a board-level configuration latch.
 * The caller must supply valid ISA IRQ and DMA channel numbers.
 */
void cs4231a_set_resources(ISADevice *dev, uint32_t irq, uint32_t dma);

#endif
