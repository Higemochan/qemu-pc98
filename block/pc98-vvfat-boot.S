/*
 * Free boot loaders for QEMU's PC-98 directory-backed FAT disk.
 * Copyright (C) 2026 Awe Morris
 *
 * This source is assembled independently. It does not contain code copied
 * from an NEC or Microsoft boot sector.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

.code16
.section .text

/*
 * Sector 0 is loaded with the partition table (sector 1) immediately after
 * it at 1fc0:0000.  The first partition is the one vvfat98 creates.
 */
        .globl  pc98_vvfat_ipl
pc98_vvfat_ipl:
        jmp     ipl_start
        nop
        nop
        .ascii  "IPL1"
        .byte   0, 0, 0
        .byte   0x1e

ipl_start:
        cli
        xorw    %ax, %ax
        movw    %ax, %ss
        movw    $0x0400, %sp
        sti
        pushw   %cs
        popw    %ds

	/*
	 * Convert the first partition's IPL CHS tuple to the IDE BIOS's
	 * relative 24-bit LBA form.  vvfat98 has 8 heads and 17 sectors.
	 */
        movzwl  0x0206, %eax
        imull   $136, %eax, %eax
        movzbl  0x0205, %ecx
        imull   $17, %ecx, %ecx
        addl    %ecx, %eax
        movzbl  0x0204, %ecx
        addl    %ecx, %eax
        movw    %ax, %cx
        shrl    $16, %eax
        movb    %al, %dl
        movw    $0x1f80, %ax
        movw    %ax, %es
        movw    %ax, %si
        xorw    %bp, %bp
        movw    $512, %bx
        movb    %ss:0x0584, %al
        andb    $0x7f, %al
        movb    $0x06, %ah
        int     $0x1b
        jc      boot_failure
        ljmp    $0x1f80, $0

boot_failure:
        movb    $0x06, %al
        outb    %al, $0x37
1:
        hlt
        jmp     1b

        .org    0x01f4
        .word   9
        .word   0xaa55
        .org    0x01fc
        .word   9
        .word   0xaa55

/*
 * The partition boot record begins with a FAT12/16 BPB.  vvfat98 fills the
 * BPB fields at runtime and overlays this code beginning at offset 0x48.
 * Bytes 0x3e..0x45 are the PC-98 DOS fixed-disk BPB extension.
 */
        .org    0x0200
        .globl  pc98_vvfat_pbr
pc98_vvfat_pbr:
        jmp     pbr_start
        nop
        .ascii  "NEC  5.0"
        .fill   0x48 - (. - pc98_vvfat_pbr), 1, 0

pbr_start:
        cli
        xorw    %ax, %ax
        movw    %ax, %ss
        movw    $0x0400, %sp
        sti
        cld
        pushw   %si
        pushw   %cs
        popw    %ds

        /*
         * root_lba = hidden + reserved + fats * sectors_per_fat
         * All vvfat98 metadata is near the start of the disk, but use 32-bit
         * arithmetic so the PBR remains correct if the geometry grows.
         */
        movzwl  0x0b, %edi
        shrl    $9, %edi
        movzwl  0x0e, %eax
        imull   %edi, %eax
        movzbl  0x10, %ecx
        movzwl  0x16, %edx
        imull   %edx, %ecx
        imull   %edi, %ecx
        addl    %ecx, %eax
        addl    0x1c, %eax
        movl    %eax, root_lba - pc98_vvfat_pbr

        /* root_bytes = root_entries * 32 */
        movzwl  0x11, %ebx
        shll    $5, %ebx
        movw    %bx, root_bytes - pc98_vvfat_pbr

        xorw    %dx, %dx
        movw    %dx, %es
        movw    $0x0600, %bp
        call    read_disk
        jc      pbr_failure

        /* Search all ordinary root entries for the short name IO.SYS. */
        movw    $0x0600, %di
        movw    0x11, %cx
find_io:
        cmpb    $0, %es:(%di)
        je      pbr_failure
        cmpb    $0xe5, %es:(%di)
        je      next_entry
        cmpb    $0x0f, %es:0x0b(%di)
        je      next_entry
        pushw   %cx
        pushw   %di
        movw    $(io_name - pc98_vvfat_pbr), %si
        movw    $11, %cx
        repe cmpsb
        popw    %di
        popw    %cx
        je      io_found
next_entry:
        addw    $32, %di
        loop    find_io
        jmp     pbr_failure

io_found:
        cmpl    $0x00010000, %es:0x1c(%di)
        jb      pbr_failure
        movzwl  %es:0x1a(%di), %eax
        cmpw    $2, %ax
        jb      pbr_failure
        subl    $2, %eax
        movzbl  0x0d, %ecx
        imull   %ecx, %eax
        movzwl  0x0b, %ecx
        shrl    $9, %ecx
        imull   %ecx, %eax
        movl    %eax, io_lba - pc98_vvfat_pbr

        /*
         * data_lba = root_lba + ceil(root_bytes / bytes_per_sector)
         *            + (first_cluster - 2) * sectors_per_cluster
         */
        movzwl  root_bytes - pc98_vvfat_pbr, %ebx
        movl    $512, %ecx
        leal    -1(%ebx, %ecx), %eax
        xorl    %edx, %edx
        divl    %ecx
        addl    root_lba - pc98_vvfat_pbr, %eax
        addl    io_lba - pc98_vvfat_pbr, %eax
        movl    %eax, io_lba - pc98_vvfat_pbr

        /*
         * PC-98 MS-DOS IO.SYS is a 64 KiB loader image.  Load its first
         * 0xf800 bytes at 0060:0000 and its final 0x0800 bytes immediately
         * above that at 1000:0000.
         */
        movl    io_lba - pc98_vvfat_pbr, %eax
        movw    $0x0060, %dx
        movw    %dx, %es
        xorw    %bp, %bp
        movw    $0xf800, %bx
        call    read_disk
        jc      pbr_failure

        addl    $124, io_lba - pc98_vvfat_pbr
        movl    io_lba - pc98_vvfat_pbr, %eax
        movw    $0x1000, %dx
        movw    %dx, %es
        xorw    %bp, %bp
        movw    $0x0800, %bx
        call    read_disk
        jc      pbr_failure

        /*
         * Join the final 2 KiB to the first transfer, producing a contiguous
         * 64 KiB image at 0060:0000.  Restore SI to the segment containing
         * this PBR: PC-98 MS-DOS uses it to locate the BPB after entry.
         */
        movw    $0x0fe0, %dx
        movw    %dx, %ds
        movw    %dx, %es
        movw    $0x0200, %si
        xorw    %di, %di
        movw    $0x0800, %cx
        rep movsb
        movw    $0x0060, %dx
        movw    %dx, %es
        popw    %si
        ljmp    $0x0060, $0

pbr_failure:
        movb    $0x06, %al
        outb    %al, $0x37
1:
        hlt
        jmp     1b

/*
 * Read BX bytes from the 24-bit LBA in EAX to ES:BP through PC-98 INT 1Bh.
 * The IDE BIOS uses CX for the low 16 LBA bits and DL for bits 16..23.
 */
read_disk:
        pushl   %eax
        movw    %ax, %cx
        shrl    $16, %eax
        movb    %al, %dl
        movb    %ss:0x0584, %al
        andb    $0x7f, %al
        movb    $0x06, %ah
        int     $0x1b
        popl    %eax
        ret

io_name:
        .ascii  "IO      SYS"
root_lba:
        .long   0
io_lba:
        .long   0
root_bytes:
        .word   0

        .org    0x03fe
        .word   0xaa55
