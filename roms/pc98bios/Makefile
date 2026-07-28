# Minimal PC-9821 BIOS ROM banks.
#
# Each target is one 32 KiB bank image, linked flat and emitted as raw binary
# the same way pc-bios/optionrom builds its option ROMs.  Drop the results in
# the directory passed to -L, or install them into pc-bios/.

CC      ?= gcc
OBJCOPY ?= objcopy

SRC_DIR := $(dir $(lastword $(MAKEFILE_LIST)))

CFLAGS  := -m16 -march=i486 -ffreestanding -Wall -Werror -Os -fno-pie
LDFLAGS := -nostdlib -Wl,--build-id=none,-T,$(SRC_DIR)bank.lds,-m,elf_i386,-no-pie

BANKS := pc98itf.bin pc98ide.bin pc98pci.bin pc98bios.bin pc98basic.bin
UNIFONT_BDF := $(SRC_DIR)fonts/unifont_jp-17.0.04.bdf.gz

all: $(BANKS)

font: pc98font.bin

pc98font.bin: $(SRC_DIR)makefont.py $(UNIFONT_BDF)
	python3 $< $@

qemu-blobs: all font
	cp pc98itf.bin pc98ide.bin pc98pci.bin pc98bios.bin pc98basic.bin \
		pc98font.bin \
		$(SRC_DIR)../../pc-bios/

%.o: $(SRC_DIR)%.S
	$(CC) $(CFLAGS) -I$(SRC_DIR) -c -o $@ $<

%.img: %.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

# --pad-to fixes the image at exactly one bank; the loader rejects short banks.
pc98itf.bin: itf.img
	$(OBJCOPY) -O binary --pad-to 0x8000 --gap-fill 0xff $< $@

pc98ide.bin: ide.img
	$(OBJCOPY) -O binary --pad-to 0x2000 --gap-fill 0xff $< $@

pc98pci.bin: pci.img
	$(OBJCOPY) -O binary --pad-to 0x8000 --gap-fill 0xff $< $@

pc98bios.bin: bios.img
	$(OBJCOPY) -O binary --pad-to 0x18000 --gap-fill 0xff $< $@

pc98basic.bin: basic.img
	$(OBJCOPY) -O binary --pad-to 0x8000 --gap-fill 0xff $< $@

clean:
	rm -f *.o *.img pc98itf.bin pc98ide.bin pc98pci.bin pc98bios.bin
	rm -f pc98basic.bin
	rm -f pc98font.bin

.PHONY: all font qemu-blobs clean
.PRECIOUS: %.o %.img
