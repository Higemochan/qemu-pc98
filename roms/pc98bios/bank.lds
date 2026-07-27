/*
 * Link script for Minial PC-9821 free BIOS.
 *
 * Each bank is 32 KiB and is paged into the 0xF8000 window, so the bank's
 * own offsets are what the code sees at 0xF8000 + offset.  Everything is
 * linked flat at 0 and padded to the bank size by the objcopy step.
 */
SECTIONS
{
    . = 0;
    .text : { *(.text) *(.text.*) *(.rodata) *(.rodata.*) }
}
ENTRY(_start)
