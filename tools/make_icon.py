"""Generate res/app.ico (requires Pillow).

    python tools/make_icon.py
"""
from pathlib import Path

from PIL import Image, ImageDraw

SIZE = 256


def draw() -> Image.Image:
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # background: rounded square
    d.rounded_rectangle((8, 8, SIZE - 8, SIZE - 8), radius=52, fill=(37, 99, 235, 255))
    d.rounded_rectangle((8, 8, SIZE - 8, SIZE // 2), radius=52, fill=(59, 130, 246, 255))
    d.rectangle((8, SIZE // 2 - 52, SIZE - 8, SIZE // 2), fill=(59, 130, 246, 255))
    d.rectangle((8, SIZE // 2, SIZE - 8, SIZE // 2 + 1), fill=(37, 99, 235, 255))

    # click rays
    cx, cy = 92, 84
    rays = [((cx - 58, cy), (cx - 30, cy)), ((cx, cy - 58), (cx, cy - 30)),
            ((cx - 42, cy - 42), (cx - 22, cy - 22)), ((cx + 42, cy - 42), (cx + 22, cy - 22)),
            ((cx - 42, cy + 42), (cx - 22, cy + 22))]
    for a, b in rays:
        d.line((a, b), fill=(255, 255, 255, 255), width=14)
        for p in (a, b):
            d.ellipse((p[0] - 7, p[1] - 7, p[0] + 7, p[1] + 7), fill=(255, 255, 255, 255))

    # mouse pointer arrow
    tip = (cx + 8, cy + 8)
    arrow = [tip, (tip[0], tip[1] + 140), (tip[0] + 36, tip[1] + 106), (tip[0] + 62, tip[1] + 160),
             (tip[0] + 88, tip[1] + 148), (tip[0] + 62, tip[1] + 96), (tip[0] + 108, tip[1] + 96)]
    d.polygon(arrow, fill=(255, 255, 255, 255), outline=(15, 23, 42, 255))
    d.line(arrow + [tip], fill=(15, 23, 42, 255), width=8, joint="curve")
    return img


def main() -> None:
    out = Path(__file__).resolve().parent.parent / "res" / "app.ico"
    img = draw()
    img.save(out, sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (128, 128), (256, 256)])
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
