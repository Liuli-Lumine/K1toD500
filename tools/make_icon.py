#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成程序图标 res/app.ico (多尺寸, 无需外部素材)。

用法:  python tools/make_icon.py
"""
from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
OUT = HERE.parent / "res" / "app.ico"

NAVY = (35, 42, 63, 255)
BODY = (237, 241, 248, 255)
DARK = (42, 50, 70, 255)
ORANGE = (255, 159, 69, 255)
LIGHT = (199, 206, 220, 255)


def draw(size: int = 1024) -> Image.Image:
    s = size / 256.0
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    def R(x0, y0, x1, y1):
        return [x0 * s, y0 * s, x1 * s, y1 * s]

    # 底板
    d.rounded_rectangle(R(4, 4, 252, 252), radius=56 * s, fill=NAVY)
    # 天线
    d.line([(150 * s, 96 * s), (176 * s, 34 * s)], fill=ORANGE, width=int(13 * s))
    d.ellipse(R(170, 26, 186, 42), fill=ORANGE)
    # 机身
    d.rounded_rectangle(R(66, 86, 190, 216), radius=18 * s, fill=BODY)
    # 扬声器栅格
    for i, y in enumerate((108, 126, 144)):
        d.rounded_rectangle(R(84, y, 120, y + 9), radius=4 * s, fill=DARK)
    # 屏幕
    d.rounded_rectangle(R(130, 104, 174, 152), radius=6 * s, fill=DARK)
    d.rounded_rectangle(R(137, 112, 167, 122), radius=3 * s, fill=ORANGE)
    d.rounded_rectangle(R(137, 130, 158, 138), radius=3 * s, fill=LIGHT)
    # 按键
    for x in (92, 122, 152):
        d.ellipse(R(x, 178, x + 22, 200), fill=LIGHT)
    return img


def main() -> int:
    big = draw(1024)
    base = big.resize((256, 256), Image.LANCZOS)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    base.save(OUT, format="ICO", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64),
                                        (128, 128), (256, 256)])
    print("wrote", OUT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
