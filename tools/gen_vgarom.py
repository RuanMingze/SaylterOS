#!/usr/bin/env python3
"""Convert FreeBSD vgarom-16x32.hex to font16x32[256][32][2] C array.

vgarom-16x32.hex format:
  # Height: 32
  # Width: 16
  XXXX:HHHHHH...   (XXXX = char code, HHHH... = 128 hex chars = 64 bytes = 32 rows x 2 bytes)

Output format matches src/kernel/vbe.c vbe_draw_char():
  glyph[row][0] = left 8 pixels, glyph[row][1] = right 8 pixels
  MSB = leftmost pixel within each byte
"""

import sys
import re
import os

HEX_FILE = os.path.join(os.path.dirname(__file__), '..', 'fonts', 'vgarom-16x32.hex')
OUT_FILE = os.path.join(os.path.dirname(__file__), '..', 'src', 'kernel', 'font16x32.c')

def main():
    # Init all 256 chars to zero
    font = [[[0, 0] for _ in range(32)] for _ in range(256)]

    with open(HEX_FILE, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            m = re.match(r'^([0-9A-Fa-f]{4}):([0-9A-Fa-f]+)$', line)
            if not m:
                continue
            char_code = int(m.group(1), 16)
            hex_data = m.group(2)
            if char_code > 255:
                continue
            # Should be 128 hex chars = 64 bytes
            if len(hex_data) != 128:
                print(f"WARNING: char 0x{char_code:02X} has {len(hex_data)} hex chars (expected 128)", file=sys.stderr)
            # Parse: each 4 hex chars = 2 bytes = one row
            # hex_data[0:2] = byte0, hex_data[2:4] = byte1
            # glyph[row][0] = byte0 (left 8 px), glyph[row][1] = byte1 (right 8 px)
            for row in range(32):
                off = row * 4
                if off + 4 <= len(hex_data):
                    b0 = int(hex_data[off:off+2], 16)
                    b1 = int(hex_data[off+2:off+4], 16)
                    font[char_code][row][0] = b0
                    font[char_code][row][1] = b1

    # Write C file
    with open(OUT_FILE, 'w') as f:
        f.write("/* Auto-generated from FreeBSD vgarom-16x32.hex */\n")
        f.write("/* Public domain (IBM PC VGA ROM font) */\n")
        f.write("/* MSB = leftmost pixel, row order top=0 */\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write("const uint8_t font16x32[256][32][2] = {\n")
        for ci in range(256):
            glyph = font[ci]
            # Build row data string
            row_bytes = []
            for row in range(32):
                row_bytes.append(f"0x{glyph[row][0]:02X},0x{glyph[row][1]:02X}")
            comment = f"'{chr(ci)}'" if 32 <= ci <= 126 else '.'
            f.write(f"    /* 0x{ci:02X} {comment} */ {{ {','.join(row_bytes)} }},\n")
        f.write("};\n")

    # Stats
    non_zero = sum(1 for ci in range(256) if any(any(p for p in row) for row in font[ci]))
    print(f"Generated {OUT_FILE}")
    print(f"  Non-zero glyphs: {non_zero}/256")
    print(f"  File size: {os.path.getsize(OUT_FILE)} bytes")

if __name__ == '__main__':
    main()
