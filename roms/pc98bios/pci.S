/*
 * Minimal PC-9821 BIOS / IDE BIOS Bank3
 * Copyright (C) 2026 Awe Morris
 *
 * The main BIOS maps this bank into the D8000 option window and enters +1Bh
 * for INT 1Ah functions AH=B1h/B4h.  The handler below implements the
 * published PCI BIOS calls directly through configuration mechanism #1.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

	.code16
	.text

	.equ	PCI_ADDR,	0x0cf8
	.equ	PCI_DATA,	0x0cfc
	.equ	PCI_SUCCESS,		0x00
	.equ	PCI_FUNC_UNSUPPORTED,	0x81
	.equ	PCI_BAD_VENDOR,		0x83
	.equ	PCI_DEVICE_NOT_FOUND,	0x86
	.equ	PCI_BAD_REGISTER,	0x87
	.equ	PCI_SET_FAILED,		0x88
	.equ	PCI_BUFFER_TOO_SMALL,	0x89

	.equ	PCI_IRQ,		5
	.equ	PCI_IRQ_MASK,		(1 << PCI_IRQ)
	.equ	IRQ_TABLE_SIZE,		(irq_table_end - irq_table)

	.globl	_start
_start:
	lret
	nop
	nop
	lret
	nop
	nop
	lret
	nop
	nop

	/* +09 is reserved in the stock PCI bank. */
	.byte	0x00, 0x00, 0x00

	/* +0C .. +18 */
	.rept	5
	lret
	nop
	nop
	.endr

	/* +1B: public INT 1Ah AH=B1h/B4h service entry. */
	.byte	0xe9
	.word	pci_int1a - . - 2

	/* +1E */
	lret
	nop
	nop

	/*
	 * +21: private POST helper.  The Xa7 ROM temporarily uses segment 4000h
	 * as scratch space here and clears it before returning.  The stock main
	 * BIOS accepts a declined entry, so no persistent service is needed.
	 */
	lret
	nop
	nop

	/* +24 and +27 */
	lret
	nop
	nop
	lret
	nop
	nop

	/* +2A .. +2F reserved; +30 and +35 are optional calls. */
	.byte	0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	.org	0x0030
	lret
	nop
	nop
	.byte	0x00, 0x00
	lret

	/* Published signature retained at the same bank offset as the Xa7 ROM. */
	.org	0x1102
	.ascii	"$PCI"

	.org	0x2000
pci_int1a:
	pushw	%bp
	movw	%sp, %bp
	pushw	%ax			/* preserve the B1xx subfunction in AL */
	pushw	%ds
	pushw	%es
	pushw	%si
	pushw	%di
	cld

	cmpb	$0xb1, %ah
	je	pci_bios
	cmpb	$0xb4, %ah
	je	pci_autoconfig
	jmp	pci_unsupported

pci_bios:
	cmpb	$0x01, %al
	je	pci_present
	cmpb	$0x02, %al
	je	pci_find_device
	cmpb	$0x03, %al
	je	pci_find_class
	cmpb	$0x08, %al
	je	pci_read_byte
	cmpb	$0x09, %al
	je	pci_read_word
	cmpb	$0x0a, %al
	je	pci_read_dword
	cmpb	$0x0b, %al
	je	pci_write_byte
	cmpb	$0x0c, %al
	je	pci_write_word
	cmpb	$0x0d, %al
	je	pci_write_dword
	cmpb	$0x0e, %al
	je	pci_get_irq_routing
	cmpb	$0x0f, %al
	je	pci_set_irq

pci_unsupported:
	movb	$PCI_FUNC_UNSUPPORTED, %ah
	jmp	pci_error

pci_autoconfig:
	testb	%al, %al
	jne	pci_autoconfig_unsupported
	xorl	%eax, %eax
	movw	$0x0208, %bx
	movw	$0x0002, %cx
	movl	$0x47464341, %edx	/* "ACFG" */
	movw	$0x001f, -8(%bp)	/* returned after the saved SI is popped */
	jmp	pci_success

pci_autoconfig_unsupported:
	movw	$0x0051, %ax		/* function not supported */
	jmp	pci_error

pci_present:
	movw	$0x0210, %bx		/* PCI BIOS version 2.10 */
	xorb	%cl, %cl		/* last PCI bus */
	movl	$0x20494350, %edx	/* "PCI " */
	movb	$PCI_SUCCESS, %ah
	jmp	pci_success

pci_find_device:
	cmpw	$0xffff, %dx
	je	pci_bad_vendor
	xorw	%bx, %bx		/* bus 0, devfn 0 */
1:
	pushw	%cx
	pushw	%dx
	xorw	%di, %di		/* register 0 */
	call	cfg_read_dword
	popw	%dx
	popw	%cx
	cmpw	%dx, %ax
	jne	2f
	shrl	$16, %eax
	cmpw	%cx, %ax
	jne	2f
	testw	%si, %si
	je	pci_found
	decw	%si
2:
	incb	%bl
	jnz	1b
	jmp	pci_not_found

pci_find_class:
	andl	$0x00ffffff, %ecx
	xorw	%bx, %bx
	pushw	%dx
1:
	pushl	%ecx
	movw	$0x0008, %di
	call	cfg_read_dword
	shrl	$8, %eax
	popl	%ecx
	andl	$0x00ffffff, %eax
	cmpl	%ecx, %eax
	jne	2f
	testw	%si, %si
	je	pci_class_found
	decw	%si
2:
	incb	%bl
	jnz	1b
	popw	%dx
	jmp	pci_not_found

pci_class_found:
	popw	%dx
pci_found:
	movb	$PCI_SUCCESS, %ah
	jmp	pci_success

pci_bad_vendor:
	movb	$PCI_BAD_VENDOR, %ah
	jmp	pci_error

pci_not_found:
	movb	$PCI_DEVICE_NOT_FOUND, %ah
	jmp	pci_error

pci_read_byte:
	cmpw	$0x00ff, %di
	ja	pci_bad_register
	pushw	%dx
	call	cfg_address
	movw	%di, %ax
	andw	$3, %ax
	addw	$PCI_DATA, %ax
	movw	%ax, %dx
	inb	%dx, %al
	movb	%al, %cl
	call	cfg_finish
	popw	%dx
	movb	$PCI_SUCCESS, %ah
	jmp	pci_success

pci_read_word:
	cmpw	$0x00fe, %di
	ja	pci_bad_register
	testw	$1, %di
	jnz	pci_bad_register
	pushw	%dx
	call	cfg_address
	movw	%di, %ax
	andw	$2, %ax
	addw	$PCI_DATA, %ax
	movw	%ax, %dx
	inw	%dx, %ax
	movw	%ax, %cx
	call	cfg_finish
	popw	%dx
	movb	$PCI_SUCCESS, %ah
	jmp	pci_success

pci_read_dword:
	cmpw	$0x00fc, %di
	ja	pci_bad_register
	testw	$3, %di
	jnz	pci_bad_register
	pushw	%dx
	call	cfg_read_dword
	movl	%eax, %ecx
	popw	%dx
	movb	$PCI_SUCCESS, %ah
	jmp	pci_success

pci_write_byte:
	cmpw	$0x00ff, %di
	ja	pci_bad_register
	pushw	%dx
	call	cfg_address
	movw	%di, %ax
	andw	$3, %ax
	addw	$PCI_DATA, %ax
	movw	%ax, %dx
	movb	%cl, %al
	outb	%al, %dx
	call	cfg_finish
	popw	%dx
	movb	$PCI_SUCCESS, %ah
	jmp	pci_success

pci_write_word:
	cmpw	$0x00fe, %di
	ja	pci_bad_register
	testw	$1, %di
	jnz	pci_bad_register
	pushw	%dx
	call	cfg_address
	movw	%di, %ax
	andw	$2, %ax
	addw	$PCI_DATA, %ax
	movw	%ax, %dx
	movw	%cx, %ax
	outw	%ax, %dx
	call	cfg_finish
	popw	%dx
	movb	$PCI_SUCCESS, %ah
	jmp	pci_success

pci_write_dword:
	cmpw	$0x00fc, %di
	ja	pci_bad_register
	testw	$3, %di
	jnz	pci_bad_register
	pushw	%dx
	pushl	%ecx
	call	cfg_address
	popl	%eax
	movw	$PCI_DATA, %dx
	outl	%eax, %dx
	call	cfg_finish
	popw	%dx
	movb	$PCI_SUCCESS, %ah
	jmp	pci_success

/*
 * PCI BIOS B10Eh.  The 16-bit interface points ES:DI at:
 *     word buffer_size; dword buffer_far_pointer
 * The Xa7 exposes three 16-byte routes for its onboard/physical slots.
 * This machine wires every PCI INTx pin to PC-98 IRQ 5, so advertise that
 * fixed route instead of the mutable routing registers used by real boards.
 */
pci_get_irq_routing:
	testw	%bx, %bx
	jnz	pci_unsupported
	cmpw	$IRQ_TABLE_SIZE, %es:(%di)
	jb	1f
	movw	$IRQ_TABLE_SIZE, %es:(%di)
	movw	%es:2(%di), %bx
	movw	%es:4(%di), %ax
	movw	%ax, %es
	movw	%cs, %ax
	movw	%ax, %ds
	movw	$irq_table, %si
	movw	%bx, %di
	movw	$IRQ_TABLE_SIZE, %cx
	rep	movsb
	movw	$PCI_IRQ_MASK, %bx
	movb	$PCI_SUCCESS, %ah
	jmp	pci_success
1:
	movw	$IRQ_TABLE_SIZE, %es:(%di)
	movb	$PCI_BUFFER_TOO_SMALL, %ah
	jmp	pci_error

/*
 * PCI BIOS B10Fh.  QEMU's PC-9821 host bridge has a fixed INTx route, so a
 * request succeeds only for IRQ 5.  Also mirror it to Interrupt Line (3Ch)
 * for software that subsequently reads configuration space.
 */
pci_set_irq:
	cmpb	$0x0a, %cl
	jb	1f
	cmpb	$0x0d, %cl
	ja	1f
	cmpb	$PCI_IRQ, %ch
	jne	1f
	movw	$0x0000, %di
	pushw	%dx
	call	cfg_read_dword
	cmpw	$0xffff, %ax
	je	2f
	movw	$0x003c, %di
	call	cfg_address
	movw	$PCI_DATA, %dx
	movb	%ch, %al
	outb	%al, %dx
	call	cfg_finish
	popw	%dx
	movb	$PCI_SUCCESS, %ah
	jmp	pci_success
2:
	popw	%dx
1:
	movb	$PCI_SET_FAILED, %ah
	jmp	pci_error

pci_bad_register:
	movb	$PCI_BAD_REGISTER, %ah

pci_error:
	orb	$0x01, 6(%bp)
	jmp	pci_return

pci_success:
	andb	$0xfe, 6(%bp)

pci_return:
	/*
	 * The PCI BIOS status is returned in AH while AL continues to identify
	 * the B1xx subfunction.  The NEC ROM preserves it even when config
	 * reads use EAX internally.  B4xx has its own full-AX return values.
	 */
	cmpb	$0xb1, -1(%bp)
	jne	1f
	movb	-2(%bp), %al
1:
	popw	%di
	popw	%si
	popw	%es
	popw	%ds
	addw	$2, %sp
	popw	%bp
	iret

/*
 * Select BX (BH=bus, BL=devfn), DI register in configuration mechanism #1.
 */
cfg_address:
	movl	$0x80000000, %eax
	movzbl	%bh, %edx
	shll	$16, %edx
	orl	%edx, %eax
	movzbl	%bl, %edx
	shll	$8, %edx
	orl	%edx, %eax
	movzwl	%di, %edx
	andl	$0xfc, %edx
	orl	%edx, %eax
	movw	$PCI_ADDR, %dx
	outl	%eax, %dx
	ret

cfg_read_dword:
	call	cfg_address
	movw	$PCI_DATA, %dx
	inl	%dx, %eax
	call	cfg_finish
	ret

/*
 * End a mechanism-1 cycle by clearing the enable bit in CF8h.  The Xa7
 * PCI BIOS does this after every access; leaving it asserted changes how
 * later firmware/OS probes see the configuration latch.
 */
cfg_finish:
	pushl	%eax
	pushw	%dx
	movw	$PCI_ADDR, %dx
	inl	%dx, %eax
	andl	$0x7fffffff, %eax
	outl	%eax, %dx
	popw	%dx
	popl	%eax
	ret

	/* bus, devfn, four (link, IRQ bitmap) pairs, slot, reserved */
irq_table:
	.byte	0x00, 0x40
	.byte	0x01
	.word	PCI_IRQ_MASK
	.byte	0x00
	.word	0x0000
	.byte	0x00
	.word	0x0000
	.byte	0x00
	.word	0x0000
	.byte	0x00, 0x00

	.byte	0x00, 0x68
	.byte	0x01
	.word	PCI_IRQ_MASK
	.byte	0x02
	.word	PCI_IRQ_MASK
	.byte	0x03
	.word	PCI_IRQ_MASK
	.byte	0x04
	.word	PCI_IRQ_MASK
	.byte	0x01, 0x00

	.byte	0x00, 0x70
	.byte	0x02
	.word	PCI_IRQ_MASK
	.byte	0x03
	.word	PCI_IRQ_MASK
	.byte	0x04
	.word	PCI_IRQ_MASK
	.byte	0x01
	.word	PCI_IRQ_MASK
	.byte	0x02, 0x00
irq_table_end:

	.org	0x8000
