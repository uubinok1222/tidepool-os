#!/usr/bin/env python3
"""Sinh font.h: bitmap font có khử răng cưa (alpha 8-bit) cho nhân Tidepool OS.

Chỉ nhúng các ký tự thật sự xuất hiện trong chuỗi "..." của kernel.cpp (cộng ASCII 32..126),
nên file nhỏ gọn. Chạy lại mỗi khi bạn thêm chữ mới vào giao diện:

    python3 tools/gen_font.py            # cần: pip install pillow

Font nguồn: DejaVu Sans (giấy phép Bitstream Vera/DejaVu, cho phép nhúng và phân phối lại).
"""
import re, sys, pathlib
from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEJAVU = "/usr/share/fonts/truetype/dejavu/"
FACES = [  # (tên, file, cỡ px)
    ("SM",   "DejaVuSans.ttf",      13),
    ("REG",  "DejaVuSans.ttf",      14),
    ("BOLD", "DejaVuSans-Bold.ttf", 14),
    ("HEAD", "DejaVuSans-Bold.ttf", 26),
]

src = (ROOT / "kernel.cpp").read_text(encoding="utf-8")
chars = set(chr(c) for c in range(32, 127))
for lit in re.findall(r'"(?:[^"\\\n]|\\.)*"', src):
    chars.update(lit)
cps = sorted(ord(c) for c in chars if ord(c) >= 32)

out = ["// font.h - SINH TỰ ĐỘNG bởi tools/gen_font.py, đừng sửa tay.",
       "// Glyph = bitmap alpha 8-bit cắt sát nét chữ; font nguồn DejaVu Sans.",
       "#pragma once", "#include <stdint.h>", "",
       "struct Glyph { uint32_t cp; uint32_t off; uint8_t w, h; int8_t bx, by; uint8_t adv; };",
       "struct Face  { const Glyph* g; int n; const uint8_t* px; };", ""]

for name, fname, size in FACES:
    font = ImageFont.truetype(DEJAVU + fname, size)
    data, glyphs = [], []
    for cp in cps:
        ch = chr(cp)
        ox, oy = 16, 48
        img = Image.new("L", (96, 96), 0)
        ImageDraw.Draw(img).text((ox, oy), ch, font=font, fill=255, anchor="ls")
        bb = img.getbbox()
        adv = int(round(font.getlength(ch)))
        if bb is None:
            glyphs.append((cp, len(data), 0, 0, 0, 0, adv)); continue
        crop = img.crop(bb)
        w, h = crop.size
        glyphs.append((cp, len(data), w, h, bb[0] - ox, bb[1] - oy, adv))
        data.extend(crop.tobytes())
    out.append(f"static const uint8_t px_{name}[] = {{")
    for i in range(0, len(data), 32):
        out.append("  " + ",".join(str(v) for v in data[i:i+32]) + ",")
    if not data: out.append("  0")
    out.append("};")
    out.append(f"static const Glyph gl_{name}[] = {{")
    for g in glyphs:
        out.append("  {%d,%d,%d,%d,%d,%d,%d}," % g)
    out.append("};")
    out.append(f"static const Face FONT_{name} = {{gl_{name}, {len(glyphs)}, px_{name}}};")
    out.append("")

(ROOT / "font.h").write_text("\n".join(out), encoding="utf-8")
print(f"font.h: {len(cps)} ký tự x {len(FACES)} kiểu chữ")
