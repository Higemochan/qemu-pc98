/*
 * Minimal PC-9821 BIOS / Main BIOS banks.
 * Copyright (C) 2026 Awe Morris
 *
 * The combined image occupies banks 5-7 (E8000h-FFFFFh).  The ITF enters
 * FD80:0002 after paging bank 7 into the top window.  This first stage
 * installs the architectural interrupt vectors, exposes the free IDE and
 * PCI option-ROM services, and launches the fixed-disk IPL.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

.code16
.text

	/* File offset corresponding to segment FD80h offset zero. */
	.equ	FD80_FILE_BASE,	0x15800
	.equ	BIOS_SEG,	0xfd80

	.equ	PIC1_CMD,	0x0000
	.equ	PIC1_DATA,	0x0002
	.equ	PIT_CHANNEL0,	0x0071
	.equ	PIT_CONTROL,	0x0077
	.equ	KBD_DATA,	0x0041
	.equ	KBD_STATUS,	0x0043
	.equ	GDC_TEXT_PARAM,	0x0060
	.equ	GDC_TEXT_CMD,	0x0062
	.equ	GDC_MODE_1,	0x0068
	.equ	GDC_MODE_2,	0x006a
	.equ	GDC_GRAPH_PARAM, 0x00a0
	.equ	GDC_GRAPH_CMD,	0x00a2
	.equ	GDC_DISPLAY_BANK, 0x00a4
	.equ	GDC_PALETTE_0,	0x00a8
	.equ	GDC_PALETTE_1,	0x00aa
	.equ	GDC_PALETTE_2,	0x00ac
	.equ	GDC_PALETTE_3,	0x00ae
	.equ	PCI_ROM_SELECT,	0x063c
	.equ	CALENDAR_CMD,	0x0020
	.equ	CALENDAR_MODE,	0x0022
	.equ	SYSPPI_PORT_B,	0x0033
	.equ	FDC_MSR,	0x0090
	.equ	FDC_FIFO,	0x0092
	.equ	FDC_CTRL,	0x0094
	.equ	FDC_MODE,	0x00be
	.equ	FDC_MODE144,	0x04be
	.equ	WSS_CONFIG,	0x0f40
	.equ	WSS_INDEX,	0x0f44
	.equ	WSS_DATA,	0x0f45
	.equ	DMA_RESET,	0x001b
	.equ	DMA_PAGE_1,	0x0021
	.equ	DMA_PAGE_2,	0x0023
	.equ	DMA_PAGE_3,	0x0025
	.equ	DMA_PAGE_0,	0x0027
	.equ	DMA_BANK,	0x0029
	.equ	DMA_ACCESS,	0x0439
	.equ	DMA_PAGEH_0,	0x0e05
	.equ	BIOS_SHUTDOWN_PORT, 0x8900

	.equ	KB_BUF,		0x0502
	.equ	KB_BUF_END,	0x0522
	.equ	KB_HEAD,	0x0524
	.equ	KB_TAIL,	0x0526
	.equ	KB_COUNT,	0x0528
	.equ	KB_KEY_STATE,	0x052a
	.equ	KB_SHIFT,	0x053a
	.equ	CRT_RASTER,	0x053b
	.equ	CRT_MODE,	0x053c
	.equ	CRT_CURSOR,	0x053d
	.equ	CRT_GRAPH_MODE,	0x054c
	.equ	BOOT_DEVICE,	0x0584
	.equ	TIMER_COUNT,	0x058a
	.equ	BEEP_COUNT,	0x058c
	.equ	TIMER_ACTIVE,	0x058e
	.equ	SYSTEM_FLAGS,	0x0500

	.macro	set_vector number, handler
	movw	$(\handler - FD80_FILE_BASE), (4 * \number)
	movw	$BIOS_SEG, (4 * \number + 2)
	.endm

	/*
	 * Plug and Play BIOS 1.0A installation-check structure.
	 *
	 * NEC's Xa7 firmware publishes this at physical F51B0h.  Windows uses
	 * the bi-modal entry point while constructing its legacy system-device
	 * tree, so merely providing the INT 1Fh APM calls is not sufficient.
	 * Keep the entry in the permanently mapped FD80 bank instead of the
	 * pageable D800 option-ROM window used by the original firmware.
	 *
	 * The byte checksum over all 21h bytes is 05h for the fixed 1800h
	 * entry offset and FD800h protected-mode code base below.
	 */
	.org	0x0d1b0
pnp_installation_check:
	.ascii	"$PnP"
	.byte	0x10			/* specification version 1.0 */
	.byte	0x21			/* structure length */
	.word	0x0000			/* no event notification */
	.byte	0x05			/* eight-bit structure checksum */
	.long	0x00000000		/* no event-notification flag */
	.word	0x1800			/* real-mode entry offset */
	.word	BIOS_SEG		/* real-mode entry segment */
	.word	0x1800			/* protected-mode entry offset */
	.long	0x000fd800		/* protected-mode code-segment base */
	.ascii	"PC98"			/* OEM device identifier */
	.word	0x0000			/* real-mode BIOS data segment */
	.long	0x00000000		/* protected-mode BIOS data base */

	/*
	 * PC-9821 machine-capability table at physical F8E80h.
	 *
	 * Later NEC operating systems use this fixed table in addition to the
	 * ordinary BIOS work area when selecting their machine-specific IOS
	 * path.  NP21/W documents the relevant fields: byte 04h advertises
	 * PCI and other platform facilities, byte 10h is the IDE-device bitmap,
	 * byte 11h carries serial/CD-ROM capability flags, and byte 3fh is the
	 * PC-9821 model number.  The common 98h,21h header identifies the table
	 * as a PC-9821 descriptor.
	 *
	 * The values describe the Xa7-compatible machine implemented by
	 * -M pc9821.  Byte 10h is zero in ROM and is exposed through the
	 * hardware's writable F8E90h latch, which the ITF seeds from the actual
	 * IDE attachment bitmap.
	 */
	.org	0x10e80
	.byte	0x98, 0x21, 0x1f, 0x20
	.byte	0x2c, 0xb0, 0x00, 0x00
	.fill	8, 1, 0x00
	.byte	0x00, 0xb8
	.fill	45, 1, 0x00
	.byte	0x21

	.org	FD80_FILE_BASE
	.globl	_start
_start:
	jmp	cold_start
	jmp	cold_start		/* ITF post-initialisation entry, +0002h */

cold_start:
	cli
	cld
	/*
	 * Keep the temporary POST stack above the IVT and the BIOS work area.
	 * SS:SP=0060:0100 gives us physical 0700h growing down; the IPL installs
	 * its own stack before it needs this part of memory.
	 */
	movw	$0x0060, %ax
	movw	%ax, %ss
	movw	$0x0100, %sp
	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %fs
	movw	%ax, %gs

	/*
	 * Undo the temporary flat-segment and A20 state used by the free ITF's
	 * extended-memory probe.  The NEC Xa7 BIOS enters the IPL with A20
	 * masked, ordinary 64-KiB real-mode segment limits and CR0=00000032h.
	 * DOS uses the 1-MiB wraparound while detecting the processor and its
	 * initial memory layout, so leaking the probe state breaks kernel
	 * initialisation even though all disk reads themselves are correct.
	 */
	movl	$0x00000032, %eax
	movl	%eax, %cr0
	movb	$0x03, %al
	outb	%al, $0xf6

	call	install_vectors
	/*
	 * Select the uPD4990A-compatible 48-bit serial calendar format used by
	 * INT 1Ch below.  The system device deliberately resets with its mode
	 * latch high, which selects the uPD4993A extension and inserts an extra
	 * four bits ahead of the calendar stream.  DOS rarely notices, but
	 * GRUB/98 polls the returned seconds while timing disk retries and will
	 * wait forever if the fields remain nibble-shifted.
	 */
	xorb	%al, %al
	outb	%al, $CALENDAR_MODE
	/*
	 * The ITF's destructive memory probe leaves unowned low-memory bytes
	 * filled with FFh.  The main BIOS must initialise the IDE in-progress
	 * counter before the option ROM starts using INC/DEC around INT 1Bh.
	 * Starting at FFh would make every completed request appear busy.
	 */
	movb	$0x00, 0x0456
	/*
	 * Preserve the free ITF's memory-size and CPU-capability results at
	 * 0401h/0481h.  In particular, 0401h=70h describes the usable memory
	 * below the PC-9821 16-MiB system window.  Advertising 78h makes
	 * Windows allocate one extra MiB across that reserved window.
	 */
	movw	$0x0480, 0x0486
	/*
	 * BIOS_FLAG3 bit 6 advertises the extended PC-9821 keyboard command
	 * set; bit 1 is the companion Xa-class system capability flag.  Keep
	 * both bits for the NT keyboard/platform classification used during
	 * setup.  A separate Windows 2000 boot test with only bit 1 cleared
	 * remained stuck at the same point.
	 */
	orb	$0x42, 0x0481
	movb	$0x96, 0x0485		/* FDC interrupt-completion countdown */
	movb	$0x84, 0x053c
	/*
	 * Size of the writable native graphics raster in the BIOS work area.
	 * Xa-class firmware publishes 1900h (640 x 400 at 16 pixels per word).
	 * NEC's NT display hand-off uses this value when erasing the GDC boot
	 * surface; zero leaves the final "Windows is starting" frame visible
	 * even though the kernel and Core-Graph driver have already started.
	 */
	movw	$0x1900, 0x054a
	movw	$0x704f, 0x054c
	/*
	 * BIOS_FLAG0 bit 1 advertises the 1-MiB floppy interface (the native
	 * PC-98 1.2-MiB format).  The controller exists even when no medium is
	 * inserted.  NEC Xa7 firmware therefore publishes 03h here; leaving
	 * the bit clear makes NTDETECT classify the FDC inconsistently with
	 * the hardware it subsequently probes.
	 */
	orb	$0x02, SYSTEM_FLAGS
	movb	$0x01, 0x055c
	movb	$0xff, 0x0493
	movw	$0x0258, 0x0598
	movb	$0xfe, 0x05a4		/* no pending FDC recalibrate result */
	movb	$0x77, 0x05ae
	movb	$0xff, 0x05ca
	movb	$0x96, 0x05cb
	/*
	 * Xa7 drive-class summary observed after the IDE option ROM probe.
	 * The precise drive map remains in 0457h/05BAh/F8E90h.
	 */
	movb	$0x07, 0x05b0
	/*
	 * Xa7 POST clears the expansion-equipment summary after probing the
	 * integrated devices.  Keep the hand-off value observed by NTDETECT;
	 * legacy PC-9801-86 software probes the sound board directly through
	 * 0A460h/0188h.
	 */
	movb	$0x00, 0x05b3
	call	keyboard_init
	call	crt_init
	call	dma_init
	call	wss_init

	/*
	 * Select, register, and initialise the internal IDE option ROM.  Its
	 * install entry publishes the owning ROM segment in 04B0h/04B8h and
	 * fills the four-byte disk dispatch template at 04D8h.  DOS and IOS use
	 * those work-area cells when classifying the fixed-disk BIOS.
	 */
	movw	$PCI_ROM_SELECT, %dx
	xorb	%al, %al
	outb	%al, %dx
	movw	$0x04d8, %bx
	lcall	$0xd800, $0x000c
	lcall	$0xd800, $0x000f

	/*
	 * Keyboard and the slave cascade.  Keep IRQ0 masked here, matching the
	 * Xa7 firmware; clients that use the BIOS one-shot timer explicitly
	 * load TIMER_COUNT and unmask it.
	 */
	movb	$0x7d, %al
	outb	%al, $PIC1_DATA
	sti

	/*
	 * Give the free firmware a visible POST hand-off like the original
	 * PC-9821 firmware.  The ITF has already sized memory; do not touch the
	 * contents again here, merely report the installed amount and hold the
	 * message for one second.  INT 1Ch/AH=06h supplies both the short
	 * power-on tone and an interrupt-driven delay, so this does not depend
	 * on the host CPU's execution speed.
	 */
	call	post_memory_message
	movw	$10, %cx		/* 100-ms power-on tone */
	movw	$880, %dx
	movb	$0x06, %ah
	int	$0x1c
	pushw	%cs
	popw	%es
	movw	$(post_delay_done - FD80_FILE_BASE), %bx
	movw	$100, %cx		/* 100 x 10 ms = one second */
	movb	$0x02, %ah
	int	$0x1c
1:
	cmpb	$0, TIMER_ACTIVE
	je	2f
	hlt
	jmp	1b
2:

	/*
	 * Prefer an inserted floppy, as the original firmware does.  fdd_boot
	 * distinguishes the native 1.2-MiB (1024-byte sectors) format from the
	 * later 1.44-MiB format using the N byte returned by the uPD765A.
	 */
	call	fdd_boot

	/* The IDE ROM reads two IPL sectors and far-calls 1FC0:0000. */
	lcall	$0xd800, $0x0015

boot_failed:
	cli
	hlt
	jmp	boot_failed

install_vectors:
	pushw	%ax
	pushw	%cx
	pushw	%di

	/*
	 * Start with the mostly-null IVT used by the Xa7 firmware.  NEC DOS
	 * classifies and chains the low exception vectors by their initial
	 * targets, so filling all 256 entries with one generic handler changes
	 * its wrapper allocation (notably INT 29h during Windows 95 startup).
	 */
	xorl	%eax, %eax
	xorw	%di, %di
	movw	$0x100, %cx
	rep stosl

	set_vector 0x00, default_int
	set_vector 0x01, default_int
	set_vector 0x02, default_nmi
	set_vector 0x03, default_int
	set_vector 0x04, default_int
	set_vector 0x05, default_int
	set_vector 0x06, default_int
	set_vector 0x07, default_int
	set_vector 0x08, timer_irq
	set_vector 0x09, keyboard_irq
	set_vector 0x0a, default_int_alt
	set_vector 0x0b, default_int_alt
	set_vector 0x0c, default_int_0c
	set_vector 0x0d, default_int_alt
	set_vector 0x0e, default_int_alt
	set_vector 0x0f, default_int_alt
	set_vector 0x10, default_int
	set_vector 0x11, default_int_11
	set_vector 0x12, default_int_alt
	set_vector 0x13, default_int_13
	set_vector 0x14, default_int_alt
	set_vector 0x15, default_int_alt
	set_vector 0x16, default_int_alt
	set_vector 0x17, default_int_17
	set_vector 0x18, int18
	set_vector 0x19, default_int_19
	set_vector 0x1a, int1a
	set_vector 0x1b, int1b
	set_vector 0x1c, int1c
	set_vector 0x1d, default_int
	set_vector 0x1e, default_int_1e
	set_vector 0x1f, int1f

	/*
	 * The early DOS bootstrap can issue INT 21h before installing its final
	 * dispatcher.  Keep 20h/21h harmless, but leave 22h and above null like
	 * the NEC firmware (INT 29h in particular must start at 0000:0000).
	 */
	set_vector 0x20, default_int
	set_vector 0x21, default_int

	popw	%di
	popw	%cx
	popw	%ax
	ret

keyboard_init:
	pushw	%ax
	pushw	%cx
	pushw	%di
	xorw	%ax, %ax
	movw	$KB_BUF, %di
	/* 502h..521h is the 16-entry key ring; 522h is a table pointer. */
	movw	$0x0020, %cx
	rep	stosb
	/* 528h count/retry, 52Ah..539h key-state groups, 53Ah shift. */
	movw	$KB_COUNT, %di
	movw	$0x0013, %cx
	rep	stosb
	movw	$KB_BUF, KB_HEAD
	movw	$KB_BUF, KB_TAIL
	movb	$0x3a, %al
	outb	%al, $KBD_STATUS
	movb	$0x32, %al
	outb	%al, $KBD_STATUS
	movb	$0x16, %al
	outb	%al, $KBD_STATUS
	popw	%di
	popw	%cx
	popw	%ax
	ret

crt_init:
	pushw	%ax
	/*
	 * Standard PC-98 packed digital palette: colours 0..7 map to black,
	 * blue, red, magenta, green, cyan, yellow, and white.  The display
	 * device resets these registers to zero, so omitting this makes all
	 * four GDC planes render as black even after AH=40h starts graphics.
	 */
	/*
	 * Each register describes two of the eight colours.  A8h owns white
	 * and magenta, while AEh owns green and black; the previous values for
	 * those two registers were reversed and consequently mapped colour 0
	 * (the normal text background) to magenta.
	 */
	movb	$0x37, %al
	outb	%al, $GDC_PALETTE_0
	movb	$0x15, %al
	outb	%al, $GDC_PALETTE_1
	movb	$0x26, %al
	outb	%al, $GDC_PALETTE_2
	movb	$0x04, %al
	outb	%al, $GDC_PALETTE_3
	movb	$0x0f, CRT_RASTER	/* 400-line, 25-row text */
	movb	$0x84, CRT_MODE
	movb	$0x00, CRT_CURSOR	/* blinking cursor */
	call	gdc_cursor_form		/* initially hidden until AH=11h */
	/*
	 * IPLs inherit a running text display from POST.  NEC DOS starts the
	 * text GDC itself, which had hidden this omission, while GRUB/98 only
	 * selects the CRT mode and clears VRAM before drawing its menu.
	 */
	movb	$0x0d, %al		/* START */
	outb	%al, $GDC_TEXT_CMD
	popw	%ax
	ret

/*
 * Draw the NEC-style "MEMORY 640KB + nKB OK" at the upper left.
 * PC-98 text VRAM stores a 16-bit character cell at A000:row*160 and an
 * attribute byte for that cell 2000h bytes later.  WA_EXTMEM (0594h) is the
 * number of whole megabytes above the reserved 16-MiB boundary.  The second
 * number is memory above the conventional first MiB, so a 64-MiB machine
 * reports 63 * 1024 = 64512 KiB.
 */
post_memory_message:
	pushw	%ax
	pushw	%bx
	pushw	%cx
	pushw	%dx
	pushw	%si
	pushw	%di
	pushw	%es

	movw	$0xa000, %ax
	movw	%ax, %es
	xorw	%di, %di
	movw	$(post_memory_prefix - FD80_FILE_BASE), %si
	call	post_puts

	movzwl	0x0594, %eax
	addl	$15, %eax
	shll	$10, %eax
	xorw	%cx, %cx
	movl	$10, %ebx
1:
	xorl	%edx, %edx
	divl	%ebx
	pushw	%dx
	incw	%cx
	testl	%eax, %eax
	jnz	1b
2:
	popw	%ax
	addb	$'0', %al
	call	post_putc
	loop	2b

	movw	$(post_memory_suffix - FD80_FILE_BASE), %si
	call	post_puts
	popw	%es
	popw	%di
	popw	%si
	popw	%dx
	popw	%cx
	popw	%bx
	popw	%ax
	ret

post_puts:
	movb	%cs:(%si), %al
	incw	%si
	testb	%al, %al
	jz	1f
	call	post_putc
	jmp	post_puts
1:
	ret

post_putc:
	xorb	%ah, %ah
	movw	%ax, %es:(%di)
	movb	$0xe1, %es:0x2000(%di)
	addw	$2, %di
	ret

post_memory_prefix:
	.asciz	"MEMORY 640KB + "
post_memory_suffix:
	.asciz	"KB OK"

post_delay_done:
	iret

/*
 * Match the Xa7C9W power-on WSS state.  The firmware selects IRQ3 but
 * deliberately leaves the DMA field invalid; a non-PnP DOS driver can then
 * retain IRQ3 and choose DMA1 by changing 08h to 0Ah.  The latch remains
 * writable, so Windows can still assign PnP resources.
 *
 * Initialising only that latch is insufficient.  The NEC POST also puts
 * the CS4231 into mode 2, disables playback/capture, and establishes its
 * mixer/format defaults.  Suika3 probes those indirect registers before
 * choosing its fallback DMA channel; against the bare device-reset state it
 * repeatedly writes the playback format without PMCE and stops in sound
 * initialisation before reaching the display driver.
 */
wss_init:
	pushw	%ax
	pushw	%cx
	pushw	%dx
	pushw	%si
	movw	$WSS_CONFIG, %dx
	movb	$0x08, %al
	outb	%al, %dx

	/* Wait for codec initialisation-in-progress to clear. */
	movw	$WSS_INDEX, %dx
	movw	$0xffff, %cx
1:
	inb	%dx, %al
	testb	$0x80, %al
	jz	2f
	outb	%al, $0x5f
	loop	1b

2:
	movw	$(wss_init_table - FD80_FILE_BASE), %si
	movw	$((wss_init_table_end - wss_init_table) / 2), %cx
3:
	movw	$WSS_INDEX, %dx
	movb	%cs:(%si), %al
	outb	%al, %dx
	incw	%si
	movw	$WSS_DATA, %dx
	movb	%cs:(%si), %al
	outb	%al, %dx
	incw	%si
	loop	3b

	popw	%si
	popw	%dx
	popw	%cx
	popw	%ax
	ret

wss_init_table:
	.byte	0x0c, 0xc0
	.byte	0x1a, 0xc0
	.byte	0x00, 0x00
	.byte	0x01, 0x00
	.byte	0x0d, 0x00
	.byte	0x10, 0x80
	.byte	0x49, 0x0c
	.byte	0x04, 0x88
	.byte	0x05, 0x88
	.byte	0x12, 0x08
	.byte	0x13, 0x08
	.byte	0x02, 0x04
	.byte	0x03, 0x04
	.byte	0x06, 0x00
	.byte	0x07, 0x00
wss_init_table_end:

/*
 * Initialise the uPD71037-compatible DMA controller before handing control
 * to DOS.  In particular, 0439h=0B4h enables the PC-9821 low-memory access
 * mode used by legacy DMA clients.  Leaving the device-reset value zero
 * lets a WSS driver program channel 1 but prevents the transfer from ever
 * advancing, so the application waits forever for its first interrupt.
 */
dma_init:
	pushw	%ax
	pushw	%cx
	pushw	%dx

	movw	$DMA_ACCESS, %dx
	movb	$0xb4, %al
	outb	%al, %dx

	movw	$DMA_RESET, %dx
	xorb	%al, %al
	outb	%al, %dx

	movw	$DMA_PAGE_0, %dx
	outb	%al, %dx
	movw	$DMA_PAGE_1, %dx
	outb	%al, %dx
	movw	$DMA_PAGE_2, %dx
	outb	%al, %dx
	movw	$DMA_PAGE_3, %dx
	outb	%al, %dx

	movw	$DMA_PAGEH_0, %dx
	movw	$4, %cx
1:
	outb	%al, %dx
	addw	$2, %dx
	loop	1b

	movw	$DMA_BANK, %dx
	xorb	%al, %al
	movw	$4, %cx
2:
	outb	%al, %dx
	incb	%al
	loop	2b

	popw	%dx
	popw	%cx
	popw	%ax
	ret

default_int:
	iret
default_int_alt:
	iret
default_nmi:
	iret
default_int_0c:
	iret
default_int_11:
	iret
default_int_13:
	iret
default_int_17:
	iret
default_int_19:
	iret
default_int_1e:
	iret
timer_irq:
	/*
	 * Preserve the Xa7 low-resolution handler's interrupt window exactly.
	 * In particular, the handler enables interrupts before touching the
	 * countdown word and disables them only while acknowledging the PIC.
	 * Windows 95 uses this one-shot while calibrating IOS timing.
	 */
	sti
	pushw	%ax
	pushw	%ds
	xorw	%ax, %ax
	movw	%ax, %ds

	cmpw	$0, BEEP_COUNT
	je	1f
	decw	BEEP_COUNT
	jne	1f
	movb	$0x07, %al		/* system-PPI port C bit 3 set: off */
	outb	%al, $0x37
1:
	cmpb	$0, TIMER_ACTIVE
	je	timer_no_single
	decw	TIMER_COUNT
	je	timer_expired
timer_no_single:
	cmpb	$0, TIMER_ACTIVE
	jne	timer_continue
	cmpw	$0, BEEP_COUNT
	jne	timer_continue

	/*
	 * No BIOS-owned timer source remains.  Do not mask IRQ0 here: DOS
	 * applications commonly replace vector 08h, program PIT channel 0 as
	 * a periodic clock, and chain to the old BIOS handler.  Masking the
	 * line from the chained handler lets exactly one application tick
	 * through and then freezes all time-based rendering (Suika3's message
	 * window was the visible symptom).  Explicit BIOS one-shots still mask
	 * the line in timer_expired before invoking their callback.
	 */
	popw	%ds
	cli
	movb	$0x20, %al
	outb	%al, $PIC1_CMD
	outb	%al, $0x5f
	sti
	popw	%ax
	iret

timer_continue:
	popw	%ds
	cli
	movb	$0x20, %al
	outb	%al, $PIC1_CMD
	outb	%al, $0x5f
	sti
	movb	$0x03, %ah
	int	$0x1c
	popw	%ax
	iret

timer_expired:
	/*
	 * The low-resolution single-event service installs its callback as
	 * INT 07h.  Mask the completed one-shot before invoking it, exactly as
	 * the NEC FD80 BIOS does.  Leave the interrupted general registers
	 * live: DOS's Windows loader deliberately returns DX=1 from this
	 * callback to terminate its keyboard-sense wait loop.
	 */
	movb	$0, TIMER_ACTIVE
	cmpw	$0, BEEP_COUNT
	jne	1f
	inb	$PIC1_DATA, %al
	orb	$0x01, %al
	outb	%al, $0x5f
	outb	%al, $PIC1_DATA
1:
	movb	$0x20, %al
	outb	%al, $PIC1_CMD
	outb	%al, $0x5f
	sti
	popw	%ds
	popw	%ax
	int	$0x07
	iret

/*
 * Continue the 10-ms interval by loading channel 0 with 24576
 * (2.4576 MHz / 100), and unmask IRQ0.  The hardware IRQ deliberately
 * reaches this helper through AH=03h, INT 1Ch so that an OS hook on the
 * timer BIOS vector remains in the chain.
 */
timer_program_10ms:
	pushw	%ax
	xorb	%al, %al		/* 24576 = 6000h */
	outb	%al, $PIT_CHANNEL0
	movb	$0x60, %al
	outb	%al, $PIT_CHANNEL0
	inb	$PIC1_DATA, %al
	andb	$0xfe, %al
	outb	%al, $0x5f
	outb	%al, $PIC1_DATA
	popw	%ax
	ret

/*
 * PC-98 keyboard IRQ (master IRQ1/vector 09h).  Store the raw scan code in
 * AH and a conservative ASCII value in AL.  The complete national-layout
 * translation tables belong to the next main-BIOS increment; DOS can still
 * use scan codes and the common alphanumeric subset below.
 */
keyboard_irq:
	pushw	%ax
	pushw	%bx
	pushw	%cx
	pushw	%ds
	xorw	%ax, %ax
	movw	%ax, %ds
	inb	$KBD_DATA, %al

	/*
	 * Maintain the 16-byte direct key-state table used by INT 18h/AH=04h.
	 * A PC-98 break code is the make code with bit 7 set.
	 */
	movb	%al, %ch		/* raw make/break code */
	movb	%al, %bl
	andb	$0x7f, %bl
	movb	%bl, %cl
	andb	$0x07, %cl
	movb	$0x01, %ah
	shlb	%cl, %ah
	shrb	$3, %bl
	xorb	%bh, %bh
	testb	$0x80, %ch
	jnz	keyboard_break
	orb	%ah, KB_KEY_STATE(%bx)

	/* Update the shift-state byte exposed by AH=02h. */
	cmpb	$0x70, %ch
	je	keyboard_shift_make
	cmpb	$0x7d, %ch
	je	keyboard_shift_make
	cmpb	$0x71, %ch
	jb	keyboard_buffer_make
	cmpb	$0x75, %ch
	jae	keyboard_buffer_make
	orb	%ah, KB_SHIFT
	jmp	keyboard_buffer_make
keyboard_shift_make:
	orb	$0x01, KB_SHIFT

keyboard_buffer_make:
	cmpb	$16, KB_COUNT
	jae	keyboard_done

	movb	%ch, %ah
	xorb	%al, %al
	cmpb	$(ascii_table_end - ascii_table), %ah
	jae	1f
	movzbl	%ah, %ebx
	movb	%cs:(ascii_table - FD80_FILE_BASE)(%bx), %al
1:
	movw	KB_TAIL, %bx
	movw	%ax, (%bx)
	addw	$2, %bx
	cmpw	$KB_BUF_END, %bx
	jb	3f
	movw	$KB_BUF, %bx
3:
	movw	%bx, KB_TAIL
	incb	KB_COUNT
	jmp	keyboard_done

keyboard_break:
	notb	%ah
	andb	%ah, KB_KEY_STATE(%bx)
	cmpb	$0xf0, %ch
	je	keyboard_shift_break
	cmpb	$0xfd, %ch
	je	keyboard_shift_break
	cmpb	$0xf1, %ch
	jb	keyboard_done
	cmpb	$0xf5, %ch
	jae	keyboard_done
	movb	%ch, %cl
	andb	$0x07, %cl
	movb	$0x01, %al
	shlb	%cl, %al
	notb	%al
	andb	%al, KB_SHIFT
	jmp	keyboard_done
keyboard_shift_break:
	andb	$0xfe, KB_SHIFT

keyboard_done:
	movb	$0x20, %al
	outb	%al, $PIC1_CMD
	popw	%ds
	popw	%cx
	popw	%bx
	popw	%ax
	iret

/*
 * INT 18h: keyboard and text-display BIOS.  These are the core calls used
 * by DOS; unsupported graphics calls intentionally return unchanged.
 */
int18:
	pushw	%ds
	pushw	%ax
	xorw	%ax, %ax
	movw	%ax, %ds
	popw	%ax
	cmpb	$0x00, %ah
	je	int18_key_wait
	cmpb	$0x01, %ah
	je	int18_key_peek
	cmpb	$0x02, %ah
	je	int18_shift
	cmpb	$0x03, %ah
	je	int18_kbd_reset
	cmpb	$0x04, %ah
	je	int18_key_state
	cmpb	$0x05, %ah
	je	int18_key_poll
	cmpb	$0x06, %ah
	je	int18_buffer_reset
	cmpb	$0x0a, %ah
	je	int18_mode_set
	cmpb	$0x0b, %ah
	je	int18_mode_get
	cmpb	$0x0c, %ah
	je	int18_text_start
	cmpb	$0x0d, %ah
	je	int18_text_stop
	cmpb	$0x0e, %ah
	je	int18_text_area
	cmpb	$0x10, %ah
	je	int18_cursor_type
	cmpb	$0x11, %ah
	je	int18_cursor_start
	cmpb	$0x12, %ah
	je	int18_cursor_stop
	cmpb	$0x13, %ah
	je	int18_cursor_position
	cmpb	$0x16, %ah
	je	int18_text_clear
	cmpb	$0x31, %ah
	je	int18_hires_status
	cmpb	$0x40, %ah
	je	int18_graph_start
	cmpb	$0x41, %ah
	je	int18_graph_stop
	cmpb	$0x42, %ah
	je	int18_graph_area
	cmpb	$0x43, %ah
	je	int18_graph_palette
	jmp	int18_return

int18_key_wait:
	sti
1:
	cmpb	$0, KB_COUNT
	jne	2f
	hlt
	jmp	1b
2:
	call	key_dequeue
	jmp	int18_return

int18_key_peek:
	pushw	%si
	xorb	%bh, %bh
	cmpb	$0, KB_COUNT
	je	1f
	movw	KB_HEAD, %si
	movw	(%si), %ax
	movb	$1, %bh
1:
	popw	%si
	jmp	int18_return

int18_shift:
	movb	KB_SHIFT, %al
	jmp	int18_return

int18_kbd_reset:
	pushw	%es
	pushw	%ax
	xorw	%ax, %ax
	movw	%ax, %es
	call	keyboard_init
	popw	%ax
	popw	%es
	jmp	int18_return

int18_key_state:
	pushw	%bx
	xorb	%bh, %bh
	movb	%al, %bl
	andb	$0x0f, %bl
	movb	KB_KEY_STATE(%bx), %ah
	popw	%bx
	jmp	int18_return

int18_key_poll:
	xorb	%bh, %bh
	cmpb	$0, KB_COUNT
	je	1f
	call	key_dequeue
	movb	$1, %bh
1:
	jmp	int18_return

int18_buffer_reset:
	movb	$0, KB_COUNT
	movw	$KB_BUF, KB_HEAD
	movw	$KB_BUF, KB_TAIL
	jmp	int18_return

int18_mode_set:
	/*
	 * Bit 7 is the Xa-class 31-kHz capability latch, not part of the mode
	 * value supplied by callers.  NEC Windows writes AL=00h while taking
	 * ownership of the display; replacing the whole byte makes NTDETECT
	 * later see a pre-Xa CRT.  NEC firmware preserves the bit (84h
	 * becomes 80h).
	 */
	andb	$0x7f, %al
	andb	$0x80, CRT_MODE
	orb	%al, CRT_MODE
	jmp	int18_return

int18_mode_get:
	movb	CRT_MODE, %al
	jmp	int18_return

int18_text_start:
	pushw	%ax
	movb	$0x0d, %al
	outb	%al, $GDC_TEXT_CMD
	popw	%ax
	jmp	int18_return

int18_text_stop:
	pushw	%ax
	movb	$0x0c, %al
	outb	%al, $GDC_TEXT_CMD
	popw	%ax
	jmp	int18_return

/*
 * AH=0Eh -- set the first text display area.
 *
 * DX is a byte offset in text VRAM; the master GDC consumes a word address.
 * Its SCROLL descriptor encodes the line count shifted left by four.  The
 * Xa-class 31-kHz mode selected by CRT_MODE bit 7 has 400 raster lines,
 * otherwise the legacy mode has 200.  Clear the remaining three descriptors
 * exactly as the firmware service does, so stale split-screen areas cannot
 * survive a mode change.  Windows 2000's real-mode loader calls this service
 * with DX=0 before clearing the startup display.
 */
int18_text_area:
	pushw	%ax
	pushw	%cx
	pushw	%dx

	movw	%dx, %ax
	shrw	$1, %ax
	movw	%ax, 0x0548
	movw	$0x0c80, %cx		/* 200 << 4 */
	testb	$0x80, CRT_MODE
	jz	1f
	movw	$0x1900, %cx		/* 400 << 4 */
1:
	movw	%cx, 0x054a

	movw	%ax, %dx
	movb	$0x70, %al		/* SCROLL */
	outb	%al, $GDC_TEXT_CMD
	movb	%dl, %al
	outb	%al, $GDC_TEXT_PARAM
	movb	%dh, %al
	outb	%al, $GDC_TEXT_PARAM
	movb	%cl, %al
	outb	%al, $GDC_TEXT_PARAM
	movb	%ch, %al
	outb	%al, $GDC_TEXT_PARAM
	xorb	%al, %al
	movw	$12, %cx
2:
	outb	%al, $GDC_TEXT_PARAM
	loop	2b

	popw	%dx
	popw	%cx
	popw	%ax
	jmp	int18_return

int18_cursor_type:
	pushw	%ax
	andb	$0x01, %al
	shlb	$5, %al
	movb	%al, CRT_CURSOR
	andb	$0xbf, CRT_MODE
	call	gdc_cursor_form
	popw	%ax
	jmp	int18_return

int18_cursor_start:
	orb	$0x40, CRT_MODE
	call	gdc_cursor_form
	jmp	int18_return

int18_cursor_stop:
	andb	$0xbf, CRT_MODE
	call	gdc_cursor_form
	jmp	int18_return

int18_cursor_position:
	pushw	%ax
	pushw	%dx
	movw	%dx, %ax
	shrw	$1, %ax
	movw	$GDC_TEXT_CMD, %dx
	pushw	%ax
	movb	$0x49, %al		/* CSRW */
	outb	%al, %dx
	popw	%ax
	movw	$GDC_TEXT_PARAM, %dx
	outb	%al, %dx
	movb	%ah, %al
	outb	%al, %dx
	xorb	%al, %al
	outb	%al, %dx
	popw	%dx
	popw	%ax
	jmp	int18_return

int18_text_clear:
	pushw	%ax
	pushw	%cx
	pushw	%di
	pushw	%es
	movw	$0xa000, %ax
	movw	%ax, %es
	xorw	%di, %di
	movb	%dl, %al
	xorb	%ah, %ah
	movw	$0x1000, %cx
	rep	stosw
	movw	$0xa200, %ax
	movw	%ax, %es
	xorw	%di, %di
	movb	%dh, %al
	movw	$0x0ff0, %cx
1:
	stosb
	incw	%di
	loop	1b
	popw	%es
	popw	%di
	popw	%cx
	popw	%ax
	jmp	int18_return

int18_hires_status:
	/*
	 * Xa7-class machines expose the 31-kHz display status extension.
	 * AL bits 3/2 report the available/current horizontal rate; BH encodes
	 * the current screen selection.  The power-on 640x400 mode is 31 kHz,
	 * screen zero, 25 text rows.
	 */
	movb	$0x0c, %al
	movb	$0x01, %bh
	jmp	int18_return

/*
 * The graphics display has its own (slave) uPD7220.  Windows 95 draws its
 * startup bitmap directly into the planar VRAM, then uses these BIOS calls
 * to make that layer visible.  Leaving AH=40h as a no-op makes a successful
 * boot look like a black-screen hang.
 */
int18_graph_start:
	pushw	%ax
	movb	$0x0d, %al		/* START */
	outb	%al, $GDC_GRAPH_CMD
	orb	$0x80, CRT_GRAPH_MODE
	popw	%ax
	jmp	int18_return

int18_graph_stop:
	pushw	%ax
	movb	$0x0c, %al		/* STOP */
	outb	%al, $GDC_GRAPH_CMD
	andb	$0x7f, CRT_GRAPH_MODE
	popw	%ax
	jmp	int18_return

/*
 * AH=42h -- select the display bank, colour mode and GDC display area.
 *
 * CH bits 7:6 encode NONE/UPPER/LOWER/ALL as documented by the 1992 BIOS
 * reference.  The uPD7220 SCROLL descriptor stores a word address in bits
 * 0..17 and the line count in bits 20..29.  A 640x200 half-screen therefore
 * uses 0C800000h (or starts at word 1F40h for UPPER), while the native
 * 640x400 ALL screen uses 19000000h.  Programming this descriptor is
 * essential for older games: they draw through VRAM directly and rely on
 * the BIOS call to make that memory the displayed surface.
 */
int18_graph_area:
	pushw	%ax
	pushw	%dx

	/* Select display VRAM bank from CH bit 4. */
	movb	%ch, %al
	shrb	$4, %al
	andb	$0x01, %al
	outb	%al, $GDC_DISPLAY_BANK

	/* CH bit 5 selects monochrome (1) versus colour (0). */
	testb	$0x20, %ch
	jnz	1f
	movb	$0x02, %al
	outb	%al, $GDC_MODE_1
	movb	$0x04, %al
	outb	%al, $GDC_MODE_2
	jmp	2f
1:
	movb	$0x03, %al
	outb	%al, $GDC_MODE_1
	movb	$0x05, %al
	outb	%al, $GDC_MODE_2

2:
	/*
	 * Switching from a 200-line half screen to the Xa7 native 400-line
	 * ALL screen changes the slave GDC clock, SYNC stream and pitch.  The
	 * NEC BIOS also records that transition in PRXDUPD (054Dh).  Merely
	 * changing SCROLL leaves the GDC on the old 40-word pitch and produces
	 * a black screen in YU-NO after its display-selection prompt.
	 */
	movb	%ch, %al
	andb	$0xc0, %al
	cmpb	$0xc0, %al
	jne	6f

	/*
	 * Xa7's BIOS first stops the slave sequencer and loads the 400-line
	 * timing while it is still in the old clock mode.  It then changes
	 * the clock flip-flops, selects SLAVE again, and reloads the timing.
	 * Some direct-GDC software depends on the complete transition rather
	 * than merely on the final SYNC and PITCH register contents.
	 */
	movb	$0x05, %al
	outb	%al, $GDC_GRAPH_CMD

	movb	$0x0e, %al		/* SYNC */
	outb	%al, $GDC_GRAPH_CMD
	movw	$(gdc_slave_sync_400 - FD80_FILE_BASE), %dx
	pushw	%si
	movw	%dx, %si
	movw	$8, %dx
7:
	movb	%cs:(%si), %al
	outb	%al, $GDC_GRAPH_PARAM
	incw	%si
	decw	%dx
	jnz	7b
	popw	%si

	movb	$0x83, %al
	outb	%al, $GDC_MODE_2
	movb	$0x85, %al
	outb	%al, $GDC_MODE_2

	movb	$0x6e, %al		/* SLAVE */
	outb	%al, $GDC_GRAPH_CMD
	movb	$0x0e, %al		/* SYNC */
	outb	%al, $GDC_GRAPH_CMD
	movw	$(gdc_slave_sync_400 - FD80_FILE_BASE), %dx
	pushw	%si
	movw	%dx, %si
	movw	$8, %dx
11:
	movb	%cs:(%si), %al
	outb	%al, $GDC_GRAPH_PARAM
	incw	%si
	decw	%dx
	jnz	11b
	popw	%si

	movb	$0x47, %al		/* PITCH */
	outb	%al, $GDC_GRAPH_CMD
	movb	$80, %al
	outb	%al, $GDC_GRAPH_PARAM

	movb	$0x46, %al		/* ZOOM */
	outb	%al, $GDC_GRAPH_CMD
	xorb	%al, %al
	outb	%al, $GDC_GRAPH_PARAM

	/*
	 * Clear the 200-line compatibility mode.  This is the critical part
	 * of selecting ALL: without it only the upper half of Suika3's
	 * 640x400 surface is scanned out, hiding its message window.
	 */
	movb	$0x08, %al
	outb	%al, $GDC_MODE_1

	movb	$0x4b, %al		/* CSRFORM */
	outb	%al, $GDC_GRAPH_CMD
	xorb	%al, %al
	outb	%al, $GDC_GRAPH_PARAM

	/*
	 * The documented 200-to-400-line transition changes 70h to 7Ch.
	 * Do not XOR bit 2 again on a repeated AH=42h call: games are free to
	 * reselect ALL while already in the 400-line mode.
	 */
	movb	0x054d, %al
	andb	$0x24, %al
	cmpb	$0x20, %al
	jne	10f
	xorb	$0x04, 0x054d
10:
	orb	$0x08, 0x054d

6:
	movb	$0x70, %al		/* SCROLL */
	outb	%al, $GDC_GRAPH_CMD
	movb	%ch, %al
	andb	$0xc0, %al
	cmpb	$0x40, %al
	je	3f			/* UPPER: word 1F40h, 200 lines */
	cmpb	$0xc0, %al
	je	4f			/* ALL: word 0000h, 400 lines */

	/* LOWER (and the reserved 00b encoding): word 0000h, 200 lines. */
	xorb	%al, %al
	outb	%al, $GDC_GRAPH_PARAM
	outb	%al, $GDC_GRAPH_PARAM
	movb	$0x80, %al
	outb	%al, $GDC_GRAPH_PARAM
	movb	$0x0c, %al
	outb	%al, $GDC_GRAPH_PARAM
	jmp	5f

3:
	movb	$0x40, %al
	outb	%al, $GDC_GRAPH_PARAM
	movb	$0x1f, %al
	outb	%al, $GDC_GRAPH_PARAM
	movb	$0x80, %al
	outb	%al, $GDC_GRAPH_PARAM
	movb	$0x0c, %al
	outb	%al, $GDC_GRAPH_PARAM
	jmp	5f

4:
	xorb	%al, %al
	outb	%al, $GDC_GRAPH_PARAM
	outb	%al, $GDC_GRAPH_PARAM
	outb	%al, $GDC_GRAPH_PARAM
	movb	$0x59, %al
	outb	%al, $GDC_GRAPH_PARAM

	/*
	 * Complete the Xa7 BIOS's ALL-screen transition.  In particular,
	 * clear the second display partition through TEXTW before START.
	 * Suika3 assumes this postcondition and otherwise leaves its lower
	 * message plane outside the active GDC partition.
	 */
	movb	$0x20, %al		/* WRITE, replace mode */
	outb	%al, $GDC_GRAPH_CMD
	movb	$0x78, %al		/* TEXTW: clear partition 2 address */
	outb	%al, $GDC_GRAPH_CMD
	xorb	%al, %al
	outb	%al, $GDC_GRAPH_PARAM
	outb	%al, $GDC_GRAPH_PARAM
	movb	$0x0d, %al		/* START */
	outb	%al, $GDC_GRAPH_CMD
	movb	$0x05, %al
	outb	%al, $GDC_GRAPH_CMD
	movb	$0x0e, %al		/* restore the leading SYNC mode byte */
	outb	%al, $GDC_GRAPH_CMD
	movb	$0x02, %al
	outb	%al, $GDC_GRAPH_PARAM
	movb	$0x01, %al
	outb	%al, $GDC_MODE_2

5:
	/* Publish the selected LOWER/UPPER/ALL class in CRT_BIOS bits 1:0. */
	andb	$0xfc, 0x0597
	movb	%ch, %al
	shrb	$6, %al
	cmpb	$3, %al
	jne	8f
	movb	$2, %al
	jmp	9f
8:
	cmpb	$2, %al
	jne	9f
	xorb	%al, %al
9:
	orb	%al, 0x0597

	/*
	 * For half-screen modes the cursor-form command is still needed.
	 * ALL already programmed the NEC BIOS's single zero parameter
	 * before SCROLL above.
	 */
	movb	%ch, %al
	andb	$0xc0, %al
	cmpb	$0xc0, %al
	je	12f
	movb	$0x4b, %al		/* CSRFORM */
	outb	%al, $GDC_GRAPH_CMD
	xorb	%al, %al
	outb	%al, $GDC_GRAPH_PARAM
	outb	%al, $GDC_GRAPH_PARAM
	outb	%al, $GDC_GRAPH_PARAM
12:
	popw	%dx
	popw	%ax
	jmp	int18_return

gdc_slave_sync_400:
	.byte	0x02, 0x4e, 0x07, 0x25, 0x87, 0x07, 0x90, 0x65

/*
 * AH=43h -- load the eight packed digital-palette entries from UCW+4.
 * The four bytes map directly to the A8h/AAh/ACh/AEh hardware registers.
 */
int18_graph_palette:
	pushw	%ax
	pushw	%bx
	pushw	%dx
	addw	$4, %bx
	movw	$GDC_PALETTE_0, %dx
	movb	(%bx), %al
	outb	%al, %dx
	addw	$2, %dx
	movb	1(%bx), %al
	outb	%al, %dx
	addw	$2, %dx
	movb	2(%bx), %al
	outb	%al, %dx
	addw	$2, %dx
	movb	3(%bx), %al
	outb	%al, %dx
	popw	%dx
	popw	%bx
	popw	%ax
	jmp	int18_return

/*
 * Program the text GDC cursor form.  CRT_MODE bit 6 mirrors the BIOS cursor
 * visibility state; CRT_CURSOR bit 5 selects steady (1) versus blinking (0).
 */
gdc_cursor_form:
	pushw	%ax
	pushw	%bx
	pushw	%dx
	movw	$GDC_TEXT_CMD, %dx
	movb	$0x4b, %al		/* CSRFORM */
	outb	%al, %dx
	movw	$GDC_TEXT_PARAM, %dx
	movb	CRT_RASTER, %al
	testb	$0x40, CRT_MODE
	jz	1f
	orb	$0x80, %al
1:
	outb	%al, %dx
	movb	CRT_CURSOR, %al
	outb	%al, %dx
	movb	CRT_RASTER, %bl
	shlb	$3, %bl
	addb	$3, %bl
	movb	%bl, %al
	outb	%al, %dx
	popw	%dx
	popw	%bx
	popw	%ax
	ret

int18_return:
	popw	%ds
	iret

key_dequeue:
	pushw	%bx
	movw	KB_HEAD, %bx
	movw	(%bx), %ax
	addw	$2, %bx
	cmpw	$KB_BUF_END, %bx
	jb	1f
	movw	$KB_BUF, %bx
1:
	movw	%bx, KB_HEAD
	decb	KB_COUNT
	popw	%bx
	ret

/*
 * INT 1Ah public entry.
 *
 * The legacy BIOS ABI exposes a second entry exactly 19h bytes beyond the
 * vector target.  MS-DOS temporarily hooks INT 1Ah and chains AH=10h by
 * jumping directly to that address, after making the same two-word save
 * frame as the original handler.  Keep that fixed layout even though the
 * first free implementation treats the legacy service as a no-op.
 */
int1a:
	cmpb	$0xb1, %ah
	je	int1a_pci
	cmpb	$0xb4, %ah
	je	int1a_pci
	pushw	%ds
	pushw	%dx
	jmp	int1a_legacy_entry

	.fill	0x19 - (. - int1a), 1, 0x90
int1a_legacy_entry:
	/*
	 * The NEC dispatcher accepts AH=00h..1Fh here and returns through
	 * POP DX, POP DS, IRET.  NEC DOS's temporary INT 1Ah wrapper enters the
	 * fixed +19h entry after PUSH DS, PUSH DX; restoring them in the wrong
	 * order leaves DS equal to the caller's DX and redirects its following
	 * IVT writes away from low memory.  AH=10h is sufficient for bootstrap;
	 * preserve that calling convention for all unimplemented functions.
	 */
	cmpb	$0x10, %ah
	jne	1f
	xorb	%ah, %ah
1:
	popw	%dx
	popw	%ds
	iret

int1a_pci:
	pushw	%ax
	pushw	%dx
	movw	$PCI_ROM_SELECT, %dx
	movb	$0x01, %al
	outb	%al, %dx
	popw	%dx
	popw	%ax
	ljmp	$0xd800, $0x001b

/*
 * INT 1Bh disk dispatcher.
 *
 * AL=10h/30h/70h/90h names a floppy interface.  Fixed-disk requests retain
 * the private option-ROM frame documented in ide.S.  The first floppy
 * increment implements INITIALIZE (AH low nibble 3), SENSE (4), and READ
 * (6), including both the PC-98 1.2-MiB and 1.44-MiB formats.
 */
int1b:
	/*
	 * Software INT clears IF on entry, but the NEC disk dispatcher
	 * immediately reenables interrupts before saving its frame.  Windows
	 * IOS performs many BIOS reads while calibrating the timer; keeping IF
	 * clear throughout every polled PIO transfer starves IRQ0 and leads to
	 * the IOS protection-error path even though the sectors are correct.
	 * The dispatcher also promises forward string direction to option ROMs.
	 */
	sti
	cld
	pushw	%ax
	andb	$0xf0, %al
	cmpb	$0x10, %al
	je	1f
	cmpb	$0x30, %al
	je	1f
	cmpb	$0x70, %al
	je	1f
	cmpb	$0x90, %al
	je	1f
	popw	%ax
	pushw	%ds
	pushw	%si
	pushw	%di
	pushw	%es
	pushw	%bp
	pushw	%dx
	pushw	%cx
	pushw	%bx
	pushw	%ax
	movw	%sp, %bp
	/*
	 * The option-ROM ABI enters with DS=0000h.  Besides giving the disk
	 * BIOS access to the work area, the NEC dispatcher publishes the
	 * active ROM entry in 04ACh before every transfer.  IOS inspects this
	 * cell when it decides whether the native IDE path can replace the
	 * real-mode mapper.  boot_entry temporarily uses the same cell for the
	 * IPL address, so it must not be left pointing at 1FC0:0000.
	 */
	xorw	%bx, %bx
	movw	%bx, %ds
	movw	$0x0018, 0x04ac
	movw	$0xd800, 0x04ae
	movw	$PCI_ROM_SELECT, %dx
	xorb	%al, %al
	outb	%al, %dx
	ljmp	$0xd800, $0x0018
1:
	popw	%ax
	jmp	fdd_int1b

/*
 * Floppy INT 1Bh frame.  The caller's FLAGS word is at BP+6 after saving BP;
 * update it explicitly because IRET otherwise restores the incoming carry.
 * Saved AX and BX are patched for the documented AH status and SENSE result.
 */
fdd_int1b:
	pushw	%bp
	movw	%sp, %bp
	pushw	%ax			/* BP-2 */
	pushw	%bx			/* BP-4 */
	pushw	%cx
	pushw	%dx
	pushw	%si
	pushw	%di
	pushw	%ds
	pushw	%es

	movb	%ah, %al
	andb	$0x0f, %al
	cmpb	$0x03, %al
	je	fdd_int1b_init
	cmpb	$0x04, %al
	je	fdd_int1b_sense
	cmpb	$0x06, %al
	je	fdd_int1b_read
	jmp	fdd_int1b_bad_command

fdd_int1b_init:
	movb	-2(%bp), %al
	call	fdd_initialize
	jc	fdd_int1b_not_ready
	jmp	fdd_int1b_success

fdd_int1b_sense:
	movw	$FDC_CTRL, %dx
	inb	%dx, %al
	testb	$0x10, %al
	jz	fdd_int1b_not_ready
	movw	$512, -4(%bp)
	movb	-2(%bp), %al
	andb	$0xf0, %al
	cmpb	$0x30, %al
	je	fdd_int1b_success
	movw	$1024, -4(%bp)
	jmp	fdd_int1b_success

fdd_int1b_read:
	movb	-2(%bp), %al
	call	fdd_initialize
	jc	fdd_int1b_not_ready

	/* ES:BP is the public buffer; BX is its byte count. */
	movw	0(%bp), %di
	movw	-4(%bp), %si
	movb	-5(%bp), %bh		/* original CH: sector-size code N */
	movb	-6(%bp), %bl		/* original CL: cylinder */
	movw	-8(%bp), %dx		/* original DH=head, DL=sector */
	movw	$512, %cx
	cmpb	$2, %bh
	jne	2f
	movb	$15, %ah
	movb	-2(%bp), %al
	andb	$0xf0, %al
	cmpb	$0x30, %al
	jne	1f
	movb	$18, %ah
	jmp	1f
2:
	movw	$1024, %cx
	movb	$8, %ah
	cmpb	$3, %bh
	jne	fdd_int1b_bad_parameter
1:
	testw	%si, %si
	jz	fdd_int1b_success
	cmpw	%cx, %si
	jb	fdd_int1b_bad_parameter
	call	fdd_read_one
	jc	fdd_int1b_io_error
	subw	%cx, %si
	jz	fdd_int1b_success

	/* Advance CHS according to the selected physical format. */
	incb	%dl
	cmpb	%ah, %dl
	jbe	1b
	movb	$1, %dl
	xorb	$1, %dh
	testb	%dh, %dh
	jnz	1b
	incb	%bl
	jmp	1b

fdd_int1b_success:
	movb	$0x00, -1(%bp)
	andw	$0xfffe, 6(%bp)
	jmp	fdd_int1b_return

fdd_int1b_bad_command:
	movb	$0x40, -1(%bp)
	jmp	fdd_int1b_error
fdd_int1b_bad_parameter:
	movb	$0x20, -1(%bp)
	jmp	fdd_int1b_error
fdd_int1b_not_ready:
	movb	$0x60, -1(%bp)
	jmp	fdd_int1b_error
fdd_int1b_io_error:
	movb	$0xd0, -1(%bp)
fdd_int1b_error:
	orw	$0x0001, 6(%bp)

fdd_int1b_return:
	popw	%es
	popw	%ds
	popw	%di
	popw	%si
	popw	%dx
	popw	%cx
	popw	%bx
	popw	%ax
	popw	%bp
	iret

/*
 * Configure the 1-MiB interface, select the requested drive's 1.44-MiB
 * personality when AL=3xh, spin the motor, and select non-DMA operation
 * through SPECIFY.  Do not pulse reset for every request: QEMU correctly
 * reports the resulting ready-change state, which must be sensed before a
 * command can proceed.  PIO keeps this free BIOS independent of the
 * still-unimplemented BIOS DMA setup.
 */
fdd_initialize:
	pushw	%ax
	movb	%al, %ah
	andb	$0xf0, %ah
	pushw	%dx
	movw	$FDC_MODE, %dx
	movb	$0x07, %al		/* 1-MiB ports, FDD order, motor control */
	outb	%al, %dx

	movw	$FDC_MODE144, %dx
	movb	$0x10, %al		/* enable programming, drive 0, 1.2 MiB */
	cmpb	$0x30, %ah
	jne	1f
	orb	$0x01, %al		/* drive 0, 1.44 MiB */
1:
	outb	%al, %dx

	movw	$FDC_CTRL, %dx
	movb	$0xb8, %al		/* nRESET, IRQ/DMA gates, motor */
	outb	%al, %dx

	movb	$0x03, %al		/* SPECIFY */
	call	fdd_fifo_write
	jc	2f
	movb	$0xdf, %al
	call	fdd_fifo_write
	jc	2f
	movb	$0x03, %al		/* HLT=1, ND=1 */
	call	fdd_fifo_write
2:
	popw	%dx
	popw	%ax
	ret

/*
 * Read one physical sector.
 *
 * Inputs: BL=C, BH=N, DH=H, DL=R, AH=EOT, CX=byte count, ES:DI=buffer.
 * Output: AL=N from the command result, CF status; DI advances by CX.
 */
fdd_read_one:
	pushw	%bp
	movw	%sp, %bp
	pushw	%ax			/* preserve EOT at BP-1 */
	pushw	%bx
	pushw	%cx
	pushw	%dx

	movb	$0x46, %al		/* READ DATA, MFM, single track */
	call	fdd_fifo_write
	jc	fdd_read_one_done
	movb	%dh, %al
	shlb	$2, %al		/* head plus drive 0 */
	call	fdd_fifo_write
	jc	fdd_read_one_done
	movb	%bl, %al
	call	fdd_fifo_write
	jc	fdd_read_one_done
	movb	%dh, %al
	call	fdd_fifo_write
	jc	fdd_read_one_done
	movb	%dl, %al
	call	fdd_fifo_write
	jc	fdd_read_one_done
	movb	%bh, %al
	call	fdd_fifo_write
	jc	fdd_read_one_done
	movb	-1(%bp), %al
	call	fdd_fifo_write
	jc	fdd_read_one_done
	movb	$0x1b, %al		/* GPL */
	call	fdd_fifo_write
	jc	fdd_read_one_done
	movb	$0xff, %al		/* DTL (unused for N != 0) */
	call	fdd_fifo_write
	jc	fdd_read_one_done

	/*
	 * An invalid CHS enters result phase immediately (DIO=1, NONDMA=0).
	 * Drain that result instead of mistaking its seven bytes for payload.
	 */
	movw	$FDC_MSR, %dx
	inb	%dx, %al
	testb	$0x20, %al
	jnz	1f
	call	fdd_fifo_read
	jc	fdd_read_one_done
	movb	%al, %bl
	movw	$6, %cx
4:
	call	fdd_fifo_read
	jc	fdd_read_one_done
	loop	4b
	stc
	jmp	fdd_read_one_done

1:
	call	fdd_fifo_read
	jc	fdd_read_one_done
	stosb
	loop	1b

	/* Seven result bytes: remember ST0 and return the final N byte. */
	call	fdd_fifo_read
	jc	fdd_read_one_done
	movb	%al, %bl
	movw	$5, %cx
2:
	call	fdd_fifo_read
	jc	fdd_read_one_done
	loop	2b
	call	fdd_fifo_read
	jc	fdd_read_one_done
	testb	$0xc0, %bl
	jz	3f
	stc
	jmp	fdd_read_one_done
3:
	clc
fdd_read_one_done:
	popw	%dx
	popw	%cx
	popw	%bx
	movb	-1(%bp), %ah
	movw	%bp, %sp
	popw	%bp
	ret

/* Wait for command-direction RQM and write AL to the uPD765A FIFO. */
fdd_fifo_write:
	pushw	%cx
	pushw	%dx
	movb	%al, %ah
	movw	$FDC_MSR, %dx
	movw	$0xffff, %cx
1:
	inb	%dx, %al
	andb	$0xc0, %al
	cmpb	$0x80, %al
	je	2f
	loop	1b
	stc
	jmp	3f
2:
	movw	$FDC_FIFO, %dx
	movb	%ah, %al
	outb	%al, %dx
	clc
3:
	popw	%dx
	popw	%cx
	ret

/* Wait for result/data-direction RQM and return the FIFO byte in AL. */
fdd_fifo_read:
	pushw	%cx
	pushw	%dx
	movw	$FDC_MSR, %dx
	movw	$0xffff, %cx
1:
	inb	%dx, %al
	andb	$0xc0, %al
	cmpb	$0xc0, %al
	je	2f
	loop	1b
	stc
	jmp	3f
2:
	movw	$FDC_FIFO, %dx
	inb	%dx, %al
	clc
3:
	popw	%dx
	popw	%cx
	ret

/*
 * Floppy IPL.  Sector 18 exists on a 1.44-MiB disk, but not on either the
 * native 8x1024-byte PC-98 format or the 15x512-byte 1.2-MiB format.  Probe
 * it first, then use result N to distinguish the two AL=90h sector sizes.
 */
fdd_boot:
	movw	$FDC_CTRL, %dx
	inb	%dx, %al
	testb	$0x10, %al
	jz	fdd_boot_failed

	movb	$0x30, %al
	call	fdd_initialize
	jc	fdd_boot_failed
	movw	$0x07c0, %ax
	movw	%ax, %es
	xorw	%di, %di
	xorw	%bx, %bx
	movb	$2, %bh
	movw	$0x0012, %dx		/* probe C=0, H=0, R=18 */
	movw	$512, %cx
	movb	$18, %ah
	call	fdd_read_one
	jnc	fdd_boot_144m

	/* AL=90h covers both 8x1024 and 15x512 1-MiB-interface media. */
	movb	$0x90, %al
	call	fdd_initialize
	jc	fdd_boot_failed
	movw	$0x1fc0, %ax
	movw	%ax, %es
	xorw	%di, %di
	xorw	%bx, %bx
	movb	$2, %bh
	movw	$0x0001, %dx
	movw	$512, %cx
	movb	$15, %ah
	call	fdd_read_one
	jc	fdd_boot_failed
	cmpb	$3, %al
	jne	fdd_boot_12m_loaded

	/* Native 1.2 MiB: reload the complete 1024-byte physical sector. */
	movb	$0x90, %al
	call	fdd_initialize
	jc	fdd_boot_failed
	movw	$0x1fc0, %ax
	movw	%ax, %es
	xorw	%di, %di
	xorw	%bx, %bx
	movb	$3, %bh
	movw	$0x0001, %dx
	movw	$1024, %cx
	movb	$8, %ah
	call	fdd_read_one
	jc	fdd_boot_failed

fdd_boot_12m_loaded:
	cmpw	$0xaa55, %es:510
	jne	fdd_boot_failed
	xorw	%ax, %ax
	movw	%ax, %ds
	movb	$0x90, BOOT_DEVICE
	movw	$0x1fc0, %ax
	movw	%ax, %ds
	movw	$0x0090, %ax
	cli
	ljmp	$0x1fc0, $0x0000

fdd_boot_144m:
	movb	$0x30, %al
	call	fdd_initialize
	jc	fdd_boot_failed
	movw	$0x07c0, %ax
	movw	%ax, %es
	xorw	%di, %di
	xorw	%bx, %bx
	movb	$2, %bh
	movw	$0x0001, %dx
	movw	$512, %cx
	movb	$18, %ah
	call	fdd_read_one
	jc	fdd_boot_failed
	cmpw	$0xaa55, %es:510
	jne	fdd_boot_failed
	xorw	%ax, %ax
	movw	%ax, %ds
	movb	$0x30, BOOT_DEVICE
	movw	$0x0030, %ax
	cli
	ljmp	$0x07c0, $0x0000

fdd_boot_failed:
	xorw	%ax, %ax
	movw	%ax, %ds
	stc
	ret

/*
 * Calendar/timer service.
 *
 * AH=00h returns the six-byte format documented by the PC-9800 BIOS
 * reference:
 *
 *   year, (month << 4 | weekday), day, hour, minute, second
 *
 * All numeric fields except weekday are packed BCD.  The uPD4990A shifts
 * the reverse byte order, least-significant bit first.
 */
int1c:
	cmpb	$0x00, %ah
	je	int1c_read_calendar
	cmpb	$0x01, %ah
	je	int1c_write_calendar
	cmpb	$0x02, %ah
	je	int1c_set_single_timer
	cmpb	$0x03, %ah
	je	int1c_continue_timer
	cmpb	$0x06, %ah
	je	int1c_beep
	cmpb	$0x80, %ah
	je	int1c_read_hrtimer
	cmpb	$0x81, %ah
	je	int1c_write_hrtimer
	iret

int1c_read_calendar:
	pushw	%ax
	pushw	%cx
	pushw	%dx
	pushw	%di
	movw	%bx, %di
	addw	$5, %di

	/* Latch a coherent time sample on the falling edge of STB. */
	movb	$0x0b, %al		/* TIMEREAD command | STB */
	outb	%al, $CALENDAR_CMD
	movb	$0x03, %al		/* TIMEREAD command */
	outb	%al, $CALENDAR_CMD

	movw	$6, %cx
2:
	pushw	%cx
	movw	$8, %cx
	xorb	%ah, %ah
3:
	inb	$SYSPPI_PORT_B, %al
	shrb	$1, %al
	rcrb	$1, %ah
	movb	$0x13, %al		/* TIMEREAD | CLK */
	outb	%al, $CALENDAR_CMD
	movb	$0x03, %al		/* falling CLK shifts the next bit */
	outb	%al, $CALENDAR_CMD
	loop	3b
	movb	%ah, %es:(%di)
	decw	%di
	popw	%cx
	loop	2b

	popw	%di
	popw	%dx
	popw	%cx
	popw	%ax
	iret

int1c_write_calendar:
	/*
	 * Shift the public six-byte buffer into the uPD4990A, least-significant
	 * bit first in the hardware order second..year, then issue TIMESET.
	 * The month/weekday byte is passed through unchanged.
	 */
	pushw	%ax
	pushw	%cx
	pushw	%dx
	pushw	%si
	movw	%bx, %si
	addw	$5, %si

	movb	$0x09, %al		/* SHIFT command | STB */
	outb	%al, $CALENDAR_CMD
	movb	$0x01, %al		/* capture SHIFT, reset serial input */
	outb	%al, $CALENDAR_CMD

	movw	$6, %cx
1:
	pushw	%cx
	movb	%es:(%si), %ah
	movw	$8, %cx
2:
	movb	$0x01, %al
	testb	$0x01, %ah
	jz	3f
	orb	$0x20, %al		/* DATA */
3:
	orb	$0x10, %al		/* rising CLK */
	outb	%al, $CALENDAR_CMD
	andb	$0xef, %al		/* falling CLK shifts the bit */
	outb	%al, $CALENDAR_CMD
	shrb	$1, %ah
	loop	2b
	decw	%si
	popw	%cx
	loop	1b

	movb	$0x0a, %al		/* TIMESET command | STB */
	outb	%al, $CALENDAR_CMD
	movb	$0x02, %al
	outb	%al, $CALENDAR_CMD

	popw	%si
	popw	%dx
	popw	%cx
	popw	%ax
	iret

int1c_set_single_timer:
	/*
	 * CX is a count of 10-ms ticks (zero denotes 65536); ES:BX is an IRET
	 * callback.  The low-resolution PC-98 BIOS reserves INT 07h for that
	 * callback, then the IRQ0 handler continues the interval through
	 * AH=03h, INT 1Ch after every non-final tick.
	 */
	pushw	%ds
	pushw	%dx
	xorw	%dx, %dx
	movw	%dx, %ds
	movw	%cx, TIMER_COUNT
	movb	$1, TIMER_ACTIVE
	movw	%bx, (4 * 0x07)
	movw	%es, (4 * 0x07 + 2)
	movb	$0x36, %al		/* channel 0, LSB/MSB, mode 3 */
	outb	%al, $PIT_CONTROL
	call	timer_program_10ms
	popw	%dx
	popw	%ds
	xorb	%ah, %ah
	iret

int1c_continue_timer:
	/*
	 * AH=03h is the NEC BIOS's private low-resolution "continue" entry.
	 * High-resolution machines expose the same number as timer cancel, but
	 * this Xa7-compatible low-resolution path uses it only from IRQ0.
	 */
	call	timer_program_10ms
	iret

int1c_beep:
	/*
	 * CX is the duration in 10-ms ticks and DX is the requested frequency
	 * in hertz.  PIT channel 1 runs from 2.4576 MHz; active-low system-PPI
	 * port-C bit 3 connects it to the speaker.
	 */
	pushw	%ax
	pushw	%bx
	pushw	%cx
	pushw	%dx
	pushw	%si
	pushw	%ds
	movw	%cx, %si
	movzwl	%dx, %ecx
	testl	%ecx, %ecx
	jnz	1f
	movl	$32, %ecx
1:
	movl	$2457600, %eax
	xorl	%edx, %edx
	divl	%ecx
	testl	%eax, %eax
	jnz	2f
	incl	%eax
2:
	cmpl	$0xffff, %eax
	jbe	3f
	movl	$0xffff, %eax
3:
	movw	%ax, %bx
	movb	$0x76, %al		/* channel 1, LSB/MSB, mode 3 */
	outb	%al, $PIT_CONTROL
	movw	%bx, %ax
	outb	%al, $0x73
	movb	%ah, %al
	outb	%al, $0x73

	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%si, BEEP_COUNT
	movb	$0x06, %al		/* port C bit 3 reset: speaker on */
	outb	%al, $0x37
	call	timer_program_10ms

	popw	%ds
	popw	%si
	popw	%dx
	popw	%cx
	popw	%bx
	popw	%ax
	iret

/*
 * PC-9821 high-resolution clock.
 *
 * The chipset maintains a 24-bit value at 0000:04F1h.  Bits 21:0 are
 * 1/32-second ticks since midnight and bits 23:22 are the modulo-four
 * elapsed-day field.  AH=80h exposes the clock as AL/CX/DX; AH=81h writes
 * the same representation.  Windows 9x probes these calls while bringing
 * up IOS, so silently treating them as unsupported changes its timing path.
 */
int1c_read_hrtimer:
	pushw	%ds
	xorw	%dx, %dx
	movw	%dx, %ds
	movl	0x04f1, %edx
	movl	%edx, %eax
	shrl	$16, %eax
	andw	$0x003f, %ax
	movw	%ax, %cx
	movb	0x04f3, %al
	shrb	$6, %al
	decb	%al
	andb	$0x03, %al
	popw	%ds
	iret

int1c_write_hrtimer:
	pushw	%ds
	pushw	%bx
	movb	%al, %bl
	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%dx, 0x04f1
	movb	%cl, %al
	andb	$0x3f, %al
	andb	$0x03, %bl
	shlb	$6, %bl
	orb	%bl, %al
	movb	%al, 0x04f3
	popw	%bx
	popw	%ds
	iret

/*
 * Extended-memory and internal-event BIOS.
 *
 * AH=90h moves CX bytes between two 286-style descriptors at ES:BX+10h
 * and ES:BX+18h.  SI and DI are the respective 16-bit offsets.  A zero
 * count denotes 65536 bytes.  Windows 95 uses this service while preparing
 * the page directory that it will shortly load into CR3; treating INT 1Fh
 * as a no-op leaves those pages empty and causes an immediate triple fault
 * when paging is enabled.
 *
 * AH=80h..8fh are internal-event hooks.  With no resident hook installed,
 * the NEC BIOS acknowledges them by returning with carry clear.
 *
 * AH=9Ah is the PC-98 form of the APM BIOS interface.  Its AL function
 * numbers and register contract match the standard AX=53xx interface.  Xa
 * firmware reports APM 1.1 or later through AX=9A00h; Windows 9x uses that
 * service to power the machine off, and Windows NT can use it through the
 * NT APM/Legacy Support driver.
 */
int1f:
	cmpb	$0x9a, %ah
	je	int1f_apm
	testb	$0x80, %ah
	jz	int1f_unchanged
	cmpb	$0x90, %ah
	je	int1f_move
	testb	$0x10, %ah
	jnz	int1f_unchanged
	pushw	%bp
	movw	%sp, %bp
	andw	$0xfffe, %ss:6(%bp)
	popw	%bp
	iret

int1f_unchanged:
	iret

int1f_apm:
	cmpb	$0x00, %al
	je	int1f_apm_installation
	cmpb	$0x07, %al
	je	int1f_apm_set_power
	cmpb	$0x0a, %al
	je	int1f_apm_power_status
	cmpb	$0x0b, %al
	je	int1f_apm_no_event
	cmpb	$0x0c, %al
	je	int1f_apm_get_state
	cmpb	$0x0e, %al
	je	int1f_apm_driver_version
	cmpb	$0x10, %al
	je	int1f_apm_capabilities

	/*
	 * Connect/disconnect, CPU busy/idle notification, restore defaults,
	 * power-management enable and engage/disengage are stateless in this
	 * firmware.  Accept them so an APM driver can complete its handshake.
	 */
	cmpb	$0x01, %al
	je	int1f_apm_success
	cmpb	$0x04, %al
	je	int1f_apm_success
	cmpb	$0x05, %al
	je	int1f_apm_idle
	cmpb	$0x06, %al
	je	int1f_apm_success
	cmpb	$0x08, %al
	je	int1f_apm_success
	cmpb	$0x09, %al
	je	int1f_apm_success
	cmpb	$0x0d, %al
	je	int1f_apm_success
	cmpb	$0x0f, %al
	je	int1f_apm_success
	movb	$0x09, %ah		/* unrecognised device/function */
	jmp	int1f_apm_error

int1f_apm_installation:
	movw	$0x0112, %ax		/* APM BIOS version 1.12 */
	movw	$0x504d, %bx		/* "PM" installation signature */
	xorw	%cx, %cx		/* no 16/32-bit protected-mode entry */
	jmp	int1f_apm_success

int1f_apm_idle:
	sti
	hlt
	jmp	int1f_apm_success

int1f_apm_set_power:
	cmpw	$0x0003, %cx		/* off */
	jne	int1f_apm_success
	cmpw	$0x0001, %bx		/* all BIOS-managed devices */
	je	1f
	cmpw	$0xffff, %bx
	jne	int1f_apm_bad_device
1:
	pushw	%ds
	pushw	%si
	pushw	%dx
	pushw	%cx
	pushw	%ax
	pushw	%cs
	popw	%ds
	movw	$(apm_shutdown_signature - FD80_FILE_BASE), %si
	movw	$BIOS_SHUTDOWN_PORT, %dx
	movw	$8, %cx
	cld
2:
	lodsb
	outb	%al, %dx
	loop	2b
	popw	%ax
	popw	%cx
	popw	%dx
	popw	%si
	popw	%ds
	/*
	 * QEMU exits asynchronously after the final byte.  On hardware without
	 * this emulated back end, park the CPU instead of returning to an OS
	 * which has already torn itself down.
	 */
	sti
3:
	hlt
	jmp	3b

int1f_apm_bad_device:
	movb	$0x09, %ah
	jmp	int1f_apm_error

int1f_apm_power_status:
	movw	$0xffff, %bx		/* AC and battery status unknown */
	movw	$0xffff, %cx
	movw	$0xffff, %dx
	jmp	int1f_apm_success

int1f_apm_no_event:
	movb	$0x80, %ah		/* no power-management event pending */
	jmp	int1f_apm_error

int1f_apm_get_state:
	xorw	%cx, %cx		/* ready/on */
	jmp	int1f_apm_success

int1f_apm_driver_version:
	movw	$0x0112, %ax
	jmp	int1f_apm_success

int1f_apm_capabilities:
	xorw	%bx, %bx		/* no batteries */
	xorw	%cx, %cx		/* no suspend/standby timer */

int1f_apm_success:
	pushw	%bp
	movw	%sp, %bp
	andw	$0xfffe, %ss:6(%bp)
	popw	%bp
	iret

int1f_apm_error:
	pushw	%bp
	movw	%sp, %bp
	orw	$0x0001, %ss:6(%bp)
	popw	%bp
	iret

apm_shutdown_signature:
	.ascii	"Shutdown"

int1f_move:
	pushw	%ds
	pushw	%es
	pushal

	/* Validate both initial offsets against their descriptor limits. */
	movzwl	%si, %eax
	movzwl	%es:0x10(%bx), %edx
	cmpl	%edx, %eax
	ja	int1f_move_error
	movzwl	%di, %eax
	movzwl	%es:0x18(%bx), %edx
	cmpl	%edx, %eax
	ja	int1f_move_error

	/*
	 * Build a temporary GDT on the caller's stack.  Giving DS and ES the
	 * actual source and destination descriptor bases lets the ordinary
	 * 16-bit MOVSB instruction reproduce the BIOS ABI's SI/DI wrap at the
	 * 64-KiB boundary.  This matters especially for CX=0, which means a
	 * complete 65536-byte transfer rather than no transfer.
	 *
	 * Stack scratch layout:
	 *   +00 old GDTR, +06 new GDTR, +12 null, +20 source, +28 destination,
	 *   +36 BIOS real-mode data cache
	 */
	movb	$0x02, %al
	outb	%al, $0xf6		/* release A20 */
	pushf
	cli
	subw	$44, %sp
	movw	%sp, %bp
	sgdt	%ss:0(%bp)

	movl	$0, %ss:12(%bp)
	movl	$0, %ss:16(%bp)

	movw	%es:0x10(%bx), %ax
	movw	%ax, %ss:20(%bp)
	movw	%es:0x12(%bx), %ax
	movw	%ax, %ss:22(%bp)
	movb	%es:0x14(%bx), %al
	movb	%al, %ss:24(%bp)
	movb	$0x92, %ss:25(%bp)
	movw	$0, %ss:26(%bp)

	movw	%es:0x18(%bx), %ax
	movw	%ax, %ss:28(%bp)
	movw	%es:0x1a(%bx), %ax
	movw	%ax, %ss:30(%bp)
	movb	%es:0x1c(%bx), %al
	movb	%al, %ss:32(%bp)
	movb	$0x92, %ss:33(%bp)
	movw	$0, %ss:34(%bp)

	/*
	 * The NEC transition helper leaves FS and GS loaded from a 64-KiB
	 * data descriptor based at 10000h.  They are 386 additions to this
	 * 286-era service and are intentionally not part of its saved frame.
	 */
	movw	$0xffff, %ss:36(%bp)
	movw	$0x0000, %ss:38(%bp)
	movb	$0x01, %ss:40(%bp)
	movb	$0x92, %ss:41(%bp)
	movw	$0x0000, %ss:42(%bp)

	movw	%ss, %ax
	movzwl	%ax, %eax
	shll	$4, %eax
	movzwl	%bp, %edx
	addl	%edx, %eax
	addl	$12, %eax
	movw	$31, %ss:6(%bp)
	movl	%eax, %ss:8(%bp)
	lgdt	%ss:6(%bp)

	movl	%cr0, %eax
	orl	$0x00000001, %eax
	movl	%eax, %cr0
	movw	$0x0008, %ax
	movw	%ax, %ds
	movw	$0x0010, %ax
	movw	%ax, %es
	movw	$0x0018, %ax
	movw	%ax, %fs
	movw	%ax, %gs
	movl	%cr0, %eax
	andl	$0xfffffffe, %eax
	movl	%eax, %cr0

	/*
	 * Reload CS after clearing PE before restoring the caller's segment
	 * registers.  Without this serialising far jump, QEMU continues the
	 * current protected-mode translation block and POP DS/ES below changes
	 * their visible selectors without restoring real-mode 64-KiB limits.
	 * Windows then returns from INT 1Fh with (for example) DS.limit=1000h
	 * and faults as soon as the loader touches the rest of its data segment.
	 *
	 * Keep the source and destination caches in DS/ES across the jump: the
	 * copy below deliberately uses their protected-mode bases.  They are
	 * normalised only by the saved POP ES/POP DS in int1f_move_return.
	 */
	ljmp	$BIOS_SEG, $(int1f_move_real - FD80_FILE_BASE)

int1f_move_real:
	lgdt	%ss:0(%bp)
	addw	$44, %sp

	cld
	testw	%cx, %cx
	jnz	1f
	movw	$0xffff, %cx
	rep movsb
	movsb
	jmp	2f
1:
	rep movsb
2:

	/* Match the PC-98 BIOS contract: the move leaves A20 masked. */
	movb	$0x03, %al
	outb	%al, $0xf6
	popf
	clc
	jmp	int1f_move_return

int1f_move_error:
	stc

int1f_move_return:
	popal
	popw	%es
	popw	%ds
	jc	1f
	pushw	%bp
	movw	%sp, %bp
	andw	$0xfffe, %ss:6(%bp)
	popw	%bp
	iret
1:
	pushw	%bp
	movw	%sp, %bp
	orw	$0x0001, %ss:6(%bp)
	popw	%bp
	iret

/*
 * Unshifted common-key subset indexed by the PC-98 scan code delivered by
 * the emulated keyboard.  Zero means "scan code only".
 */
ascii_table:
	.byte	0x1b
	.ascii	"1234567890-^"
	.byte	'\\', 0x08, 0x09
	.ascii	"qwertyuiop@["
	.byte	0x0d
	.ascii	"asdfghjkl;:]"
	.ascii	"zxcvbnm,./"
	.byte	0x00, 0x20
ascii_table_end:

	/*
	 * Plug and Play BIOS bi-modal runtime entry at FD80:1800h.
	 *
	 * The interface follows the 1.0A 16-bit C stack convention.  Both real
	 * mode and protected-mode callers pass 16-bit arguments; far pointers
	 * contain either real-mode segments or selectors.  All registers and
	 * FLAGS except AX are preserved as required by the specification.
	 */
	.org	0x17000
pnp_bios_entry:
	pushw	%bp
	movw	%sp, %bp
	pushf
	pushw	%bx
	pushw	%cx
	pushw	%dx
	pushw	%si
	pushw	%di
	pushw	%ds
	pushw	%es

	movw	6(%bp), %ax		/* Function */
	testw	%ax, %ax
	jz	pnp_get_num_nodes
	cmpw	$0x0001, %ax
	je	pnp_get_device_node
	movw	$0x0082, %ax		/* FUNCTION_NOT_SUPPORTED */
	jmp	pnp_return

pnp_get_num_nodes:
	/*
	 * Function 00h:
	 *   far *NumNodes at +08h, far *NodeSize at +0Ch.
	 */
	ldsw	8(%bp), %bx
	movb	$1, (%bx)
	ldsw	12(%bp), %bx
	movw	$(pnp_apm_node_end - pnp_apm_node), (%bx)
	xorw	%ax, %ax		/* SUCCESS */
	jmp	pnp_return

pnp_get_device_node:
	/*
	 * Function 01h:
	 *   far *Node at +08h, far *buffer at +0Ch, Control at +10h.
	 * Only current (1) or next-boot (2) configuration is valid.
	 */
	movw	16(%bp), %ax
	cmpw	$0x0001, %ax
	je	1f
	cmpw	$0x0002, %ax
	je	1f
	movw	$0x0084, %ax		/* BAD_PARAMETER */
	jmp	pnp_return
1:
	ldsw	8(%bp), %bx
	cmpb	$0, (%bx)
	je	2f
	movw	$0x0083, %ax		/* INVALID_HANDLE */
	jmp	pnp_return
2:
	lesw	12(%bp), %di
	pushw	%cs
	popw	%ds
	movw	$(pnp_apm_node - FD80_FILE_BASE), %si
	movw	$(pnp_apm_node_end - pnp_apm_node), %cx
	cld
	rep	movsb

	/* Tell the caller that this was the final node. */
	ldsw	8(%bp), %bx
	movb	$0xff, (%bx)
	xorw	%ax, %ax		/* SUCCESS */

pnp_return:
	popw	%es
	popw	%ds
	popw	%di
	popw	%si
	popw	%dx
	popw	%cx
	popw	%bx
	popf
	popw	%bp
	lret

	/*
	 * Static APM BIOS system device, PNP0C05.  The three resource blocks
	 * contain only End Tags because the interface itself owns no I/O, IRQ,
	 * DMA or memory resource.  Attribute bits 1:0 mark it non-configurable
	 * and non-disableable.
	 */
pnp_apm_node:
	.word	pnp_apm_node_end - pnp_apm_node
	.byte	0x00			/* node handle */
	.byte	0x41, 0xd0, 0x0c, 0x05	/* compressed PNP0C05 */
	.byte	0x08, 0x00, 0x00	/* system peripheral */
	.word	0x0003			/* static, required device */
	.byte	0x79, 0x00		/* allocated-resource End Tag */
	.byte	0x79, 0x00		/* possible-resource End Tag */
	.byte	0x79, 0x00		/* compatible-ID End Tag */
pnp_apm_node_end:

	/* CPU reset while the main bank is already selected. */
	.org	0x17ff0
	ljmp	$BIOS_SEG, $0x0000

	.org	0x18000
