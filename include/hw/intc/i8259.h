#ifndef HW_I8259_H
#define HW_I8259_H

/* i8259.c */

#define TYPE_I8259 "isa-i8259"

typedef struct PICCommonState PICCommonState;

extern PICCommonState *isa_pic;
extern PICCommonState *slave_pic;

/*
 * i8259_init()
 *
 * Create a i8259 device on an ISA @bus,
 * connect its output to @parent_irq_in,
 * return an (allocated) array of 16 input IRQs.
 */
qemu_irq *i8259_init(ISABus *bus, qemu_irq parent_irq_in);
qemu_irq *kvm_i8259_init(ISABus *bus);
int pic_get_output(PICCommonState *s);
int pic_read_irq(PICCommonState *s);

/* PC-98: drop a stale master IRQ (clear both IRR and ISR).  Used to reap a
 * vsync interrupt a game armed but abandoned without an EOI, which would
 * otherwise stay in-service and block every lower-priority interrupt.
 * Returns true if any state for @irq was cleared. */
bool pc98_pic_reap_master_irq(int irq);

#endif
