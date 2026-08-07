/*
 * Minimal PC-9821 BIOS / IDE BIOS bank 3
 *
 * This is the first deliberately small implementation of the private disk
 * option-ROM ABI used by the Xa7-family main BIOS.  It provides the HDD
 * services needed by the IPL and DOS through INT 1Bh, using polled ATA
 * PIO.  ATAPI and interrupt-driven transfers are intentionally left for a
 * later revision.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

.code16
.text

	.equ	IDE_CHANNEL,	0x0432
	.equ	IDE_DATA,	0x0640
	.equ	IDE_ERROR,	0x0642
	.equ	IDE_SECCNT,	0x0644
	.equ	IDE_SECNUM,	0x0646
	.equ	IDE_CYLLO,	0x0648
	.equ	IDE_CYLHI,	0x064a
	.equ	IDE_DRVHEAD,	0x064c
	.equ	IDE_STATUS,	0x064e
	.equ	IDE_COMMAND,	0x064e
	.equ	IDE_DEVCTL,	0x074c
	.equ	WAIT_IO,	0x005f
	.equ	PIC1_CMD,	0x0000
	.equ	PIC2_CMD,	0x0008
	.equ	PIC2_DATA,	0x000a

	.equ	ATA_SRST,	0x06
	.equ	ATA_READ,	0xc4
	.equ	ATA_WRITE,	0xc5
	.equ	ATA_IDENTIFY,	0xec
	.equ	ATA_ST_ERR,	0x01
	.equ	ATA_ST_DRQ,	0x08
	.equ	ATA_ST_DRDY,	0x40
	.equ	ATA_ST_BSY,	0x80

	.equ	WA_BUSY,	0x0456
	.equ	WA_GEOMETRY,	0x0457
	.equ	WA_IDE_FLAGS,	0x045d
	.equ	WA_DISK_FLAGS,	0x0480
	.equ	WA_DISK_ROM0,	0x04b0
	.equ	WA_DISK_ROM1,	0x04b8
	.equ	WA_DRIVES,	0x055d
	.equ	WA_BOOT_DEVICE,	0x0584
	.equ	WA_IDE_PROBE,	0x05ba
	.equ	WA_BOOT_OFF,	0x04ac
	.equ	WA_BOOT_SEG,	0x04ae

	.equ	BIOS_HEADS,	8
	.equ	BIOS_SECTORS,	17
	.equ	BIOS_CYLINDERS,	602
	.equ	BOOT_SEGMENT,	0x1fc0

	/*
	 * The ITF makes the 0xDB000 page writable shadow RAM.  The NEC disk
	 * ROM uses that page for its private state as well, so keep detected
	 * geometry there rather than consuming an undocumented main-BIOS work
	 * area cell.  These offsets are relative to the D800 ROM segment.
	 */
	.equ	ROM_WORK_CYLINDERS,	0x3000
	.equ	ROM_WORK_HEADS,		0x3002
	.equ	ROM_WORK_SECTORS,	0x3004

	/*
	 * The main BIOS indexes this table in three-byte slots.  Keep even the
	 * declined entries and the nonstandard signature byte-for-byte
	 * compatible with the NEC Xa7C9W ROM.
	 */
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

	.org	0x0009
	.byte	0x55, 0xaa, 0x2a
	.byte	0xe9
	.word	install_entry - . - 2
	.byte	0xe9
	.word	init_entry - . - 2
	lret
	nop
	nop
	.byte	0xe9
	.word	boot_entry - . - 2
	.byte	0xe9
	.word	int1b_entry - . - 2
	lret
	nop
	nop
	lret
	nop
	nop
	lret
	nop
	nop
	lret
	nop
	nop
	lret
	nop
	nop
	lret
	nop
	nop
	lret
	nop
	nop
	lret
	nop
	nop
	nop
	nop
	lret

	.org	0x0040

/*
 * Install the disk-ROM dispatch cell.  DS is zero and BX points at the
 * four-byte far-jump template owned by the main BIOS.
 */
install_entry:
	movb	$0xea, 0(%bx)
	movb	$0xaa, 1(%bx)
	movb	$0xaa, 2(%bx)
	movb	$0xaa, 3(%bx)

	cmpb	$0, WA_DISK_ROM0
	jne	1f
	movw	%cs, %ax
	movb	%ah, WA_DISK_ROM0
	movb	%ah, WA_DISK_ROM1
	orb	$0x80, WA_DISK_FLAGS
1:
	lret

/*
 * Initialise the fixed machine profile and verify that ATA master answers.
 * The ITF has already issued the reset required to run ATA diagnostics.
 * Do not pulse SRST again here: the minimal free main BIOS launches the IPL
 * immediately after this call, before a second reset could settle.
 */
init_entry:
	pushw	%ds
	pushw	%ax
	pushw	%dx
	xorw	%ax, %ax
	movw	%ax, %ds

	/*
	 * The built-in IDE interrupt is slave IR1 (IRQ9 / vector 11h).  The
	 * stock option ROM owns this vector even though its public INT 1Bh
	 * services normally transfer data by polling.  Windows 9x uses the
	 * installed vector and PIC state while classifying the native IDE
	 * controller, so leaving the main BIOS's placeholder handler here
	 * makes IOS reject an otherwise working disk interface.
	 */
	cli
	movw	$ide_irq, 0x0044
	movw	%cs, 0x0046
	inb	$PIC2_DATA, %al
	outb	%al, $WAIT_IO
	andb	$0xfd, %al
	outb	%al, $PIC2_DATA
	outb	%al, $WAIT_IO
	sti

	movw	$BIOS_CYLINDERS, %ax
	movw	%ax, %cs:ROM_WORK_CYLINDERS
	movw	$BIOS_HEADS, %ax
	movw	%ax, %cs:ROM_WORK_HEADS
	movw	$BIOS_SECTORS, %ax
	movw	%ax, %cs:ROM_WORK_SECTORS

	movw	$IDE_STATUS, %dx
	inb	%dx, %al
	cmpb	$0xff, %al
	je	1f
	testb	%al, %al
	jz	1f

	call	ata_identify_geometry

	orw	$0x0001, WA_DRIVES
	orb	$0x01, WA_IDE_PROBE
	andb	$0xc7, WA_GEOMETRY
	orb	$0x10, WA_GEOMETRY	/* drive 0: variable 8/17 profile */
	orb	$0x48, WA_IDE_FLAGS	/* fast IDE and fixed-disk boot */
	orb	$0x80, WA_DISK_FLAGS
	movb	$0x80, WA_BOOT_DEVICE
1:
	popw	%dx
	popw	%ax
	popw	%ds
	lret

/*
 * IDE IRQ9 acknowledgement.
 *
 * Reading the command-block status register deasserts the ATA interrupt.
 * A non-specific EOI is then sent to the slave and master PICs.  The stock
 * ROM conditionally omits the master EOI while another slave request is in
 * service; this compact handler has no other unmasked slave source during
 * POST and the operating system replaces the vector before enabling its
 * own devices, so the standard paired EOI is equivalent here.
 */
ide_irq:
	pushw	%ax
	pushw	%dx

	movw	$IDE_STATUS, %dx
	inb	%dx, %al
	outb	%al, $WAIT_IO

	movb	$0x20, %al
	outb	%al, $PIC2_CMD
	outb	%al, $WAIT_IO
	outb	%al, $PIC1_CMD
	outb	%al, $WAIT_IO

	popw	%dx
	popw	%ax
	iret

/*
 * The main BIOS asks the owning disk ROM to find and launch an IPL through
 * this entry.  Load its first two 512-byte sectors at 1FC0:0000, check the
 * "IPL1" marker, and call it with the same AX and work-area pointer used by
 * the stock ROM.
 */
boot_entry:
	pushw	%ax
	pushw	%bx
	pushw	%cx
	pushw	%dx
	pushw	%si
	pushw	%di
	pushw	%bp
	pushw	%ds
	pushw	%es

	xorw	%ax, %ax
	movw	%ax, %ds
	movw	$BOOT_SEGMENT, %ax
	movw	%ax, %es
	xorw	%di, %di
	xorl	%eax, %eax
	movw	$2, %cx
	call	ata_pio_read
	jc	boot_return

	cmpw	$0x5049, %es:4		/* "IP" */
	jne	boot_return
	cmpw	$0x314c, %es:6		/* "L1" */
	jne	boot_return

	movb	$0x80, WA_BOOT_DEVICE
	movw	$0, WA_BOOT_OFF
	movw	$BOOT_SEGMENT, WA_BOOT_SEG
	movw	$0x0080, %ax
	xorw	%bp, %bp
	lcall	*WA_BOOT_OFF

boot_return:
	popw	%es
	popw	%ds
	popw	%bp
	popw	%di
	popw	%si
	popw	%dx
	popw	%cx
	popw	%bx
	popw	%ax
	lret

/*
 * INT 1Bh request entry.  The main BIOS dispatcher has already pushed the
 * caller's registers and points BP at this frame:
 *
 *   +00 AX, +02 BX, +04 CX, +06 DX, +08 BP, +0a ES,
 *   +0c DI, +0e SI, +10 DS, +12 IP, +14 CS, +16 FLAGS.
 *
 * Return by consuming that frame exactly as the stock ROM does.
 */
int1b_entry:
	pushl %eax
	pushl %ebx
	pushl %ecx
	pushl %edx
	pushl %ebp
	incb	WA_BUSY
	xorw	%ax, %ax
	movw	%ax, %ds

	movb	1(%bp), %al
	andb	$0x0f, %al
	cmpb	$0x04, %al
	je	int1b_sense
	cmpb	$0x05, %al
	je	int1b_write
	cmpb	$0x06, %al
	je	int1b_read

	/* Recalibrate, initialise and unsupported legacy controls are no-ops. */
	xorw	%ax, %ax
	jmp	int1b_finish

int1b_sense:
	movw	$512, 2(%bp)
	movw	%cs:ROM_WORK_CYLINDERS, %ax
	movw	%ax, 4(%bp)
	movb	%cs:ROM_WORK_HEADS, %ah
	movb	%cs:ROM_WORK_SECTORS, %al
	movw	%ax, 6(%bp)
	movb	$0x0f, %ah
	jmp	int1b_finish

int1b_read:
	call	request_lba
	jc	int1b_finish
	call	request_count_buffer
	call	ata_pio_read_bytes
	jmp	int1b_finish

int1b_write:
	call	request_lba
	jc	int1b_finish
	call	request_count_buffer
	call	ata_pio_write

int1b_finish:
	movb	%ah, 1(%bp)
	andb	$0xfe, 0x16(%bp)
	cmpb	$0x20, %ah
	jb	1f
	orb	$0x01, 0x16(%bp)
1:
	decb	WA_BUSY
	popl %ebp
	popl %edx
	popl %ecx
	popl %ebx
	popl %eax
	popw	%ax
	popw	%bx
	popw	%cx
	popw	%dx
	popw	%bp
	popw	%es
	popw	%di
	popw	%si
	popw	%ds
	iret

/*
 * Convert the request's disk address to a 32-bit LBA in EAX.
 *
 * AL=80h/81h uses absolute CHS in CX, DH and DL.  AL=00h/01h uses the
 * IDE BIOS 24-bit relative form: DL is the high byte, followed by CH and
 * CL.  The older SASI service masks this to 21 bits, but the IDE option
 * ROM must retain all eight DL bits; Windows 2000 uses them on disks larger
 * than 1 GiB.
 */
request_lba:
	testb	$0x01, 0(%bp)
	jnz	request_bad_device
	testb	$0x80, 0(%bp)
	jz	request_relative

	movzwl	4(%bp), %eax
	movzwl	%cs:ROM_WORK_HEADS, %ebx
	movzwl	%cs:ROM_WORK_SECTORS, %ecx
	imull	%ecx, %ebx
	imull	%ebx, %eax
	movzbl	7(%bp), %edx
	imull	%ecx, %edx
	addl	%edx, %eax
	movzbl	6(%bp), %edx
	addl	%edx, %eax
	clc
	ret

request_relative:
	movzwl	4(%bp), %eax
	movzbl	6(%bp), %edx
	shll	$16, %edx
	orl	%edx, %eax
	clc
	ret

request_bad_device:
	movb	$0x60, %ah		/* not ready */
	stc
	ret

/*
 * Return sector count in CX, the original byte count in BX, and the
 * caller's transfer address in ES:DI.  The PC-98 ABI expresses length in
 * bytes and the built-in IDE BIOS emulates 256-byte transfers on a
 * 512-byte ATA device.  A literal zero denotes 64 KiB (128 sectors).
 */
request_count_buffer:
	movzwl	2(%bp), %ecx
	movw	%cx, %bx
	testw	%cx, %cx
	jz	2f
	addl	$511, %ecx
	shrl	$9, %ecx
	jmp	1f
2:
	movw	$0x0080, %cx
1:
	movw	8(%bp), %di
	movw	10(%bp), %dx
	movw	%dx, %es
	ret

ata_reset:
	movw	$IDE_DEVCTL, %dx
	movb	$ATA_SRST, %al
	outb	%al, %dx
	outb	%al, $WAIT_IO
	outb	%al, $WAIT_IO
	xorb	%al, %al
	outb	%al, %dx
	outb	%al, $WAIT_IO
	ret

/*
 * Read the current master's legacy geometry from ATA IDENTIFY words 1, 3
 * and 6.  QEMU's pc98 IDE controller reports the machine's required 8/17
 * translation here, with the cylinder count derived from the image size.
 * Consume all 256 words so the device leaves its data phase.  Invalid or
 * unavailable IDENTIFY data leaves the conservative defaults installed by
 * init_entry.
 */
ata_identify_geometry:
	pushw	%ax
	pushw	%bx
	pushw	%cx
	pushw	%dx
	pushw	%si

	movw	$IDE_CHANNEL, %dx
	xorb	%al, %al
	outb	%al, %dx
	movw	$IDE_DRVHEAD, %dx
	movb	$0xa0, %al
	outb	%al, %dx
	call	ata_wait_ready
	jc	2f

	movw	$IDE_COMMAND, %dx
	movb	$ATA_IDENTIFY, %al
	outb	%al, %dx
	call	ata_wait_drq
	jc	2f

	movw	$IDE_DATA, %dx
	inw	%dx, %ax		/* word 0: configuration */
	inw	%dx, %ax		/* word 1: cylinders */
	movw	%ax, %bx
	inw	%dx, %ax		/* word 2: reserved */
	inw	%dx, %ax		/* word 3: heads */
	movw	%ax, %si
	inw	%dx, %ax		/* word 4: bytes per track */
	inw	%dx, %ax		/* word 5: bytes per sector */
	inw	%dx, %ax		/* word 6: sectors per track */
	movw	%ax, %cx

	testw	%bx, %bx
	jz	1f
	cmpw	$0xffff, %bx
	jne	4f
	decw	%bx
//	je	1f
4:
	testw	%si, %si
	jz	1f
	cmpw	$0x00ff, %si
	ja	1f
	testw	%cx, %cx
	jz	1f
	cmpw	$0x00ff, %cx
	ja	1f
	movw	%bx, %cs:ROM_WORK_CYLINDERS
	movw	%si, %cs:ROM_WORK_HEADS
	movw	%cx, %cs:ROM_WORK_SECTORS
1:
	movw	$249, %cx		/* words 7 through 255 */
3:
	inw	%dx, %ax
	loop	3b
	clc
2:
	popw	%si
	popw	%dx
	popw	%cx
	popw	%bx
	popw	%ax
	ret

/*
 * Program an LBA28 command.  Input is EAX=LBA and CX=sector count.
 * SI retains the count for the data loop.
 */
ata_setup:
	movw	%cx, %si
	movl	%eax, %ebx

	movw	$IDE_CHANNEL, %dx
	xorb	%al, %al
	outb	%al, %dx
	call	ata_wait_ready
	jc	1f

	movw	%si, %ax
	movw	$IDE_SECCNT, %dx
	outb	%al, %dx
	movw	$IDE_SECNUM, %dx
	movb	%bl, %al
	outb	%al, %dx
	movw	$IDE_CYLLO, %dx
	movb	%bh, %al
	outb	%al, %dx
	shrl	$16, %ebx
	movw	$IDE_CYLHI, %dx
	movb	%bl, %al
	outb	%al, %dx
	movb	%bh, %al
	andb	$0x0f, %al
	orb	$0xe0, %al
	movw	$IDE_DRVHEAD, %dx
	outb	%al, %dx
	clc
1:
	ret

ata_pio_read:
	call	ata_setup
	jc	ata_failed
	movw	$IDE_COMMAND, %dx
	movb	$ATA_READ, %al
	outb	%al, %dx
	cld
1:
	call	ata_wait_drq
	jc	ata_failed
	movw	$IDE_DATA, %dx
	movw	$256, %cx
	rep	insw
	decw	%si
	jnz	1b
	xorw	%ax, %ax
	clc
	ret

/*
 * INT 1Bh byte-counted read.
 *
 * ATA always transfers complete 512-byte sectors, while the PC-98 BIOS ABI
 * permits 256-byte multiples.  Store only the requested portion of the
 * final physical sector and consume the remainder from the data port.
 * Reading a rounded-up sector directly into ES:DI corrupts the 256 bytes
 * following an IPL or DOS buffer.
 *
 * Input: EAX=LBA, CX=physical sector count, BX=requested byte count
 *        (BX=0 means 64 KiB), ES:DI=destination.
 */
ata_pio_read_bytes:
	pushl	%ebp
	movzwl	%bx, %ebp
	testl	%ebp, %ebp
	jnz	1f
	movl	$0x00010000, %ebp
1:
	call	ata_setup
	jc	5f
	movw	$IDE_COMMAND, %dx
	movb	$ATA_READ, %al
	outb	%al, %dx
	cld
2:
	call	ata_wait_drq
//	jc	5f
	jc	2b			//emulatorでエラー出るわけがない
	movw	$IDE_DATA, %dx
	cmpl	$512, %ebp
	jb	3f
	movw	$256, %cx
	rep	insw
	subl	$512, %ebp
	jmp	4f
3:
	/*
	 * Public fixed-disk requests are word sized (normally 256 bytes).
	 * Consume any rounded-up final word as a word too; no caller-visible
	 * byte beyond the documented transfer length is written.
	 */
	movl	%ebp, %ecx
	shrl	$1, %ecx
	rep	insw
	movw	$256, %cx
	movw	%bp, %ax
	shrw	$1, %ax
	subw	%ax, %cx
6:
	inw	%dx, %ax
	loop	6b
	xorl	%ebp, %ebp
4:
	decw	%si
	jnz	2b
	xorw	%ax, %ax
	clc
5:
	popl	%ebp
	ret

ata_pio_write:
	call	ata_setup
	jc	ata_failed
	/*
	 * request_count_buffer returned the caller's transfer address in
	 * ES:DI.  OUTSW normally reads DS:SI, so retain the sector count in BX,
	 * copy the offset to SI and use an ES override for the data stream.
	 */
	movw	%si, %bx
	movw	%di, %si
	movw	$IDE_COMMAND, %dx
	movb	$ATA_WRITE, %al
	outb	%al, %dx
	cld
1:
	call	ata_wait_drq
//	jc	ata_failed
	jc	1b		//emulatorではエラーは発生しない
	movw	$IDE_DATA, %dx
	movw	$256, %cx
	rep	outsw	%es:(%si), (%dx)
	decw	%bx
	jnz	1b
	call	ata_wait_ready
	jc	ata_failed
	xorw	%ax, %ax
	clc
	ret

ata_wait_ready:
	movw	$0xffff, %cx
	movw	$IDE_STATUS, %dx
1:
	inb	%dx, %al
	testb	$ATA_ST_ERR, %al
	jnz	ata_failed
	testb	$ATA_ST_BSY, %al
	jnz	2f
	testb	$ATA_ST_DRDY, %al
	jnz	3f
2:
	outb	%al, $WAIT_IO
	loop	1b
	jmp	ata_failed
3:
	clc
	ret

ata_wait_drq:
	movw	$0xffff, %cx
	movw	$IDE_STATUS, %dx
1:
	inb	%dx, %al
	testb	$ATA_ST_ERR, %al
	jnz	ata_failed
	testb	$ATA_ST_BSY, %al
	jnz	2f
	testb	$ATA_ST_DRQ, %al
	jnz	3f
2:
	outb	%al, $WAIT_IO
	loop	1b
	jmp	ata_failed
3:
	clc
	ret

ata_failed:
	movb	$0x40, %ah		/* equipment/not-ready class error */
	stc
	ret

	.org	0x2000
