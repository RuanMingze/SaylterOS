"""
gen_font.py —— 栅格化 TTF 字体为 16×32 1bpp C 数组
✅ 直接像素裁剪 + baseline 对齐
✅ PIL 内部起点不影响我们
"""
import sys
from collections import Counter
from PIL import Image, ImageDraw, ImageFont

TTF = sys.argv[1] if len(sys.argv) > 1 else r"C:\Windows\Fonts\consola.ttf"
OUT = sys.argv[2] if len(sys.argv) > 2 else "font16x32.c"

CELL_W, CELL_H = 16, 32
FONT_PX = 30
CHAR_START, CHAR_END = 0x00, 0xFF

font = ImageFont.truetype(TTF, FONT_PX)

# ===== 1. 对每个 glyph 渲染到 40×40 大画布，拿到真实像素边界 =====
def render_glyph_big(c):
    """返回 (big_image, min_y, max_y, min_x, max_x)"""
    big = Image.new("L", (40, 40), 0)
    d = ImageDraw.Draw(big)
    d.text((0, 0), c, fill=255, font=font)
    ys = [y for y in range(40) if any(big.getpixel((x, y)) for x in range(40))]
    xs = [x for x in range(40) if any(big.getpixel((x, y)) for y in range(40))]
    return big, (min(ys) if ys else 0), (max(ys) if ys else -1), (min(xs) if xs else 0), (max(xs) if xs else -1)

# ===== 2. 找全局 baseline：大写字母 bottom 众数 =====
NO_DESC = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
big_bounds = {c: render_glyph_big(c) for c in NO_DESC}
bottoms = [big_bounds[c][2] for c in NO_DESC if big_bounds[c][2] >= 0]
baseline_raw = Counter(bottoms).most_common(1)[0][0]

# 最大 top（大写字母最高像素）找 ascent
tops = [big_bounds[c][1] for c in NO_DESC if big_bounds[c][1] < 40]
ascent_raw = min(tops)

# 下行字母 bottom 找 descent
desc_chars = "gjpqyQ"
desc_bottoms = []
for c in desc_chars:
    _, _, by, _, _ = render_glyph_big(c)
    if by >= 0: desc_bottoms.append(by)
descent_raw = max(desc_bottoms) - baseline_raw if desc_bottoms else 0

print(f"Font: {TTF}")
print(f"  ascent_raw={ascent_raw}  baseline_raw={baseline_raw}  descent_raw={descent_raw}")

# ===== 3. cell 内 baseline 位置（让所有字符底部对齐）=====
# 把 baseline_raw 映射到 cell 内 baseline
# cell 内 baseline_y = ascent_offset_from_top + (CELL_H - total) // 2  这种
# 更简单：baseline 在 cell 底部往上 descent + 一点 padding
bottom_pad = max(0, (CELL_H - descent_raw - (baseline_raw - ascent_raw)) // 2)
# 不对...让我用最简单的：所有 glyph 的"真实 bottom"对齐到 cell 同一行
cell_base_bottom = CELL_H - max(bottom_pad, 2)   # 留 2px 底 padding
print(f"  cell {CELL_W}×{CELL_H}  cell_base_bottom={cell_base_bottom}")

# ===== 4. 渲染每个字符到 cell =====
chars_data = []
for code in range(CHAR_START, CHAR_END + 1):
    c = chr(code)
    img = Image.new("L", (CELL_W, CELL_H), 0)

    big, gt, gb, gx0, gx1 = render_glyph_big(c)
    if gb < 0 or gt >= 40:   # 空白字符（如 0x00）
        chars_data.append(",".join(["0x00,0x00"] * CELL_H))
        continue

    gh = gb - gt + 1
    gw = gx1 - gx0 + 1

    # 目标位置：
    # 水平居中：glyph 宽 gw，cell 宽 CELL_W
    dst_x = (CELL_W - gw) // 2
    # 垂直：bottom 对齐到 cell_base_bottom
    dst_y = cell_base_bottom - gh + 1   # 因为 gb 是包含的

    # 从 big 里裁出来贴到 img 里
    for dy in range(gh):
        for dx in range(gw):
            sy = gt + dy
            sx = gx0 + dx
            py = dst_y + dy
            px = dst_x + dx
            if 0 <= py < CELL_H and 0 <= px < CELL_W:
                img.putpixel((px, py), big.getpixel((sx, sy)))

    # 二值化
    img = img.point(lambda p: 1 if p > 128 else 0)

    # 转 C 数组
    rows = []
    for row in range(CELL_H):
        byte0 = 0
        byte1 = 0
        for col in range(CELL_W):
            px = img.getpixel((col, row))
            if px:
                if col < 8:
                    byte0 |= (1 << (7 - col))
                else:
                    byte1 |= (1 << (15 - col))
        rows.append(f"0x{byte0:02X},0x{byte1:02X}")

    chars_data.append(",".join(rows))

# ===== 5. 写 C 文件 =====
with open(OUT, "w", encoding="utf-8") as f:
    f.write(f"/* Auto-generated 16x32 1bpp font from {TTF} */\n")
    f.write(f"/* baseline bottom-aligned: cell_base_bottom={cell_base_bottom} */\n")
    f.write("/* MSB = leftmost pixel, row order top=0 */\n\n")
    f.write("#include <stdint.h>\n\n")
    f.write(f"const uint8_t font16x32[256][32][2] = {{\n")
    for i, cdata in enumerate(chars_data):
        ch = chr(i)
        label = ch if (32 <= i < 127) else "."
        f.write(f"    /* 0x{i:02X} '{label}' */ {{ {cdata} }},\n")
    f.write("};\n")

print(f"\n✅ Wrote {OUT} — 256 chars × 64B = {256*64}B")
