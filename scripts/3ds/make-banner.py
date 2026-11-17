#!/usr/bin/env python3
"""Generate the CIA's HOME Menu banner assets for the 3DS port (run with python3 -I).

Writes ports/3ds/res/banner.png (256x128, bannertool's banner image size) and
ports/3ds/res/banner.wav (1 s of silence, 16-bit mono 22050 Hz: bannertool needs a sound).
The picture is the port's own 48x48 icon (ports/3ds/res/icon.png) scaled up beside the title.
Needs Pillow. Both outputs are committed; re-run only to change the banner.
"""
import os
import sys
import wave

from PIL import Image, ImageDraw, ImageFont

RES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "ports", "3ds", "res")
FONTS = ["/System/Library/Fonts/Supplemental/Arial Bold.ttf", "/Library/Fonts/Arial Bold.ttf"]


def font(size):
    for path in FONTS:
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def main():
    img = Image.new("RGBA", (256, 128), (12, 14, 18, 255))
    draw = ImageDraw.Draw(img)
    # a dark gradient band so the banner reads on the HOME Menu's light background
    for y in range(128):
        shade = 18 + y // 6
        draw.line([(0, y), (255, y)], fill=(shade, shade + 4, shade + 2, 255))
    icon = Image.open(os.path.join(RES, "icon.png")).convert("RGBA").resize((88, 88), Image.NEAREST)
    img.alpha_composite(icon, (12, 20))
    draw.text((110, 34), "Aleph One", font=font(26), fill=(235, 240, 235, 255))
    draw.text((111, 70), "Marathon engine", font=font(14), fill=(150, 200, 150, 255))
    draw.text((111, 88), "Nintendo 3DS port", font=font(14), fill=(150, 200, 150, 255))
    img.convert("RGB").save(os.path.join(RES, "banner.png"))

    with wave.open(os.path.join(RES, "banner.wav"), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(22050)
        w.writeframes(b"\0\0" * 22050)
    print("wrote banner.png and banner.wav in", os.path.normpath(RES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
