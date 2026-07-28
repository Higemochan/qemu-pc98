#!/usr/bin/env python3
#
# Generate block/pc98-vvfat-boot.h from the 1024-byte flat binary assembled
# from block/pc98-vvfat-boot.S.
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: GPL-2.0-or-later

import pathlib
import sys


def emit_array(name: str, data: bytes) -> str:
    lines = [f"static const uint8_t {name}[512] = {{"]
    for offset in range(0, len(data), 12):
        chunk = ", ".join(f"0x{byte:02x}" for byte in data[offset:offset + 12])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines)


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} INPUT.bin OUTPUT.h", file=sys.stderr)
        return 2

    source = pathlib.Path(sys.argv[1])
    output = pathlib.Path(sys.argv[2])
    image = source.read_bytes()
    if len(image) != 1024:
        print(f"{source}: expected 1024 bytes, got {len(image)}",
              file=sys.stderr)
        return 1

    text = """\
/*
 * Generated from block/pc98-vvfat-boot.S; do not edit.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef BLOCK_PC98_VVFAT_BOOT_H
#define BLOCK_PC98_VVFAT_BOOT_H

"""
    text += emit_array("pc98_vvfat_ipl", image[:512])
    text += "\n\n"
    text += emit_array("pc98_vvfat_pbr", image[512:])
    text += "\n\n#endif /* BLOCK_PC98_VVFAT_BOOT_H */\n"
    output.write_text(text, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
