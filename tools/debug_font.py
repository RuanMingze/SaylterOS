"""
debug_font.py —— 直接在 16x32 cell 里渲染，打印像素位置
搞清楚 PIL 的 textbbox 和 text 到底怎么放
"""
from PIL import Image, ImageDraw, ImageFont

TTF = r"C:\Windows\Fonts\CascadiaMono.ttf"
f = ImageFont.truetype(TTF, 30)

for label, test_str in [("A (大写)", "A"), ("g (下行)", "g"), ("a (小写)", "a"), ("S (SaylterOS)", "S")]:
    # 画到大画布，看绝对位置
    big = Image.new("L", (100, 100), 0)
    d = ImageDraw.Draw(big)
    d.text((10, 10), test_str, fill=255, font=f)  # 起点 (10, 10)
    bb = d.textbbox((10, 10), test_str, font=f)
    # 找出所有有像素的行的范围
    rows_with_pixels = []
    for y in range(100):
        for x in range(100):
            if big.getpixel((x, y)) > 0:
                rows_with_pixels.append(y)
                break
    cols_with_pixels = []
    for x in range(100):
        for y in range(100):
            if big.getpixel((x, y)) > 0:
                cols_with_pixels.append(x)
                break
    print(f"\n=== {label} '{test_str}' ===")
    print(f"  textbbox((10,10)) = {bb}")
    print(f"  像素行: {min(rows_with_pixels)} ~ {max(rows_with_pixels)}  (高度 {max(rows_with_pixels)-min(rows_with_pixels)+1})")
    print(f"  像素列: {min(cols_with_pixels)} ~ {max(cols_with_pixels)}  (宽度 {max(cols_with_pixels)-min(cols_with_pixels)+1})")

print("\n\n=== 测试：把 'S' 渲染到 16×32 cell 看效果 ===")
CELL_W, CELL_H = 16, 32
cell = Image.new("L", (CELL_W, CELL_H), 0)
d = ImageDraw.Draw(cell)
# 起点 (0, 0)
d.text((0, 0), "S", fill=255, font=f)
print("cell 里有像素的行 (top=0):")
for row in range(CELL_H):
    if any(cell.getpixel((col, row)) for col in range(CELL_W)):
        line = ''.join('#' if cell.getpixel((col, row)) else '.' for col in range(CELL_W))
        print(f"  row {row:2d}: {line}")
