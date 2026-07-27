#!/usr/bin/env python3
"""Build a freely redistributable PC-98 character-generator image.

The output layout is the compact 0x46800-byte format consumed by
hw/display/pc98-vga.c:

  00000-007ff  256 ANK glyphs, 8x8
  00800-017ff  256 ANK glyphs, 8x16
  01800-467ff  JIS X 0208 rows 21h-7Ch, 16x16

The default faces are resolved by fontconfig through Cairo.  The generated
binary is checked in, so end users do not need Cairo or the source fonts.
Regeneration requires an Apache-2.0 Droid Sans Fallback installation and a
freely redistributable monospace Latin face.
"""

from __future__ import annotations

import argparse
import pathlib

import cairo


FONT_SIZE = 0x46800
KANJI_BASE = 0x1800
KANJI_ROW_BYTES = 0x60 * 32


def decode_ank(code: int) -> str | None:
    if code < 0x20 or code == 0x7F:
        return None
    try:
        return bytes((code,)).decode("cp932")
    except UnicodeDecodeError:
        return None


def decode_jis(row: int, cell: int) -> str | None:
    if not (0x21 <= row <= 0x7E and 0x21 <= cell <= 0x7E):
        return None
    encoded = bytes((0x1B, 0x24, 0x42, row, cell, 0x1B, 0x28, 0x42))
    try:
        return encoded.decode("iso2022_jp")
    except UnicodeDecodeError:
        return None


def rasterize(
    text: str | None,
    width: int,
    height: int,
    family: str,
    point_size: float,
) -> list[int]:
    if not text:
        return [0] * height

    surface = cairo.ImageSurface(cairo.FORMAT_A8, width, height)
    context = cairo.Context(surface)
    options = cairo.FontOptions()
    options.set_antialias(cairo.ANTIALIAS_NONE)
    options.set_hint_style(cairo.HINT_STYLE_FULL)
    options.set_hint_metrics(cairo.HINT_METRICS_ON)
    context.set_font_options(options)
    context.select_font_face(
        family, cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_NORMAL
    )
    context.set_font_size(point_size)

    extents = context.text_extents(text)
    x = (width - extents.width) / 2.0 - extents.x_bearing
    y = (height - extents.height) / 2.0 - extents.y_bearing
    context.move_to(round(x), round(y))
    context.set_source_rgba(1, 1, 1, 1)
    context.show_text(text)
    surface.flush()

    stride = surface.get_stride()
    data = memoryview(surface.get_data())
    rows: list[int] = []
    for y_pos in range(height):
        bits = 0
        for x_pos in range(width):
            if data[y_pos * stride + x_pos]:
                bits |= 1 << (width - 1 - x_pos)
        rows.append(bits)
    return rows


def build_font(ank_family: str, kanji_family: str) -> bytearray:
    output = bytearray(FONT_SIZE)

    for code in range(256):
        text = decode_ank(code)
        # DejaVu Mono does not contain the CP932 half-width katakana block,
        # and Cairo's toy API does not perform per-glyph fallback.  Use the
        # Japanese face for the upper ANK half while retaining a true
        # monospace Latin face for ASCII.
        family = ank_family if code < 0x80 else kanji_family
        for row, bits in enumerate(
            rasterize(text, 8, 8, family, 7.5)
        ):
            output[code * 8 + row] = bits
        for row, bits in enumerate(
            rasterize(text, 8, 16, family, 14.0)
        ):
            output[0x800 + code * 16 + row] = bits

    # The file reserves 60h glyph slots per JIS row, including the blank
    # 20h and 7Fh boundary cells expected by kanji_copy().
    for row_index in range(0x5C):
        jis_row = row_index + 0x21
        row_base = KANJI_BASE + row_index * KANJI_ROW_BYTES
        for slot in range(0x60):
            jis_cell = slot + 0x20
            text = decode_jis(jis_row, jis_cell)
            glyph = rasterize(text, 16, 16, kanji_family, 15.0)
            glyph_base = row_base + slot * 32
            for y_pos, bits in enumerate(glyph):
                output[glyph_base + y_pos] = (bits >> 8) & 0xFF
                output[glyph_base + 16 + y_pos] = bits & 0xFF

    return output


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--ank-family",
        default="DejaVu Sans Mono",
        help="fontconfig family used for 8-pixel ANK glyphs",
    )
    parser.add_argument(
        "--kanji-family",
        default="Droid Sans Fallback",
        help="fontconfig family used for 16-pixel JIS glyphs",
    )
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()

    output = build_font(args.ank_family, args.kanji_family)
    args.output.write_bytes(output)


if __name__ == "__main__":
    main()
