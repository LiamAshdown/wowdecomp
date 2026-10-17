"""Draws the touch controls' art (docs/mobile-plan.md, T3) in the style of the client's own interface: beveled gold and
dark iron on painted backgrounds. Everything is drawn here at 4x and scaled down, so the repository holds no images. The
output goes next to the game files, where the bridge serves loose Interface files (as with make-logo.py):

    python Tools/Bridge/make-touch-art.py <game dir>/Interface/TouchUI [preview.png]

Writes StickRing (256), StickKnob (128), the 128 x 128 button icons (framed by the action-button border, so they fill
their square like spell icons), Rotate (512 x 256, for the portrait card) and TrayHandle (128 x 64, the tab that slides up
the second row), as RLE TGAs. With a second argument it also writes a contact sheet of all of them on a mid-grey
background. The arc's backing plate, ArcPlate, is bent from the stock action bar by make-arc-plate.mjs.
"""

import math
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageOps

S = 4   # supersampling: shapes are drawn at 4x the output size

GOLD = ((38, 20, 4), (190, 138, 42), (255, 242, 186))
IRON = ((8, 8, 10), (72, 68, 64), (196, 186, 170))
FIRE = ((120, 20, 0), (255, 140, 20), (255, 248, 200))


def blank(w, h, mode="L"):
    return Image.new(mode, (w, h), 0)


def disc(img, cx, cy, r, fill=255):
    ImageDraw.Draw(img).ellipse((cx - r, cy - r, cx + r, cy + r), fill=fill)


def ring_mask(w, h, cx, cy, outer, inner):
    m = blank(w, h)
    disc(m, cx, cy, outer)
    disc(m, cx, cy, inner, 0)
    return m


def shade(mask, bevel, amp=6.0, grit=0.12):
    """Light from the top left over the mask's blurred edges, as a grey level (128 = flat)."""
    height = mask.filter(ImageFilter.GaussianBlur(bevel))
    # Height falling towards the top left means a face turned to the light.
    kernel = ImageFilter.Kernel((3, 3), [-1, -1, 0, -1, 0, 1, 0, 1, 1], scale=1 / amp, offset=128)
    lit = height.filter(kernel)
    if grit:
        noise = Image.effect_noise(mask.size, 64).filter(ImageFilter.GaussianBlur(S * 0.6))
        lit = Image.blend(lit, noise, grit)
    return lit


def metal(mask, bevel, ramp, amp=6.0, grit=0.12, lit=None):
    lit = lit if lit is not None else shade(mask, bevel, amp, grit)
    rgb = ImageOps.colorize(lit, black=ramp[0], mid=ramp[1], white=ramp[2])
    rgb.putalpha(mask)
    return rgb


def flat(mask, color, alpha=255):
    img = Image.new("RGBA", mask.size, color + (0,))
    img.putalpha(mask.point(lambda v: v * alpha // 255))
    return img


def shifted(mask, offset):
    """The mask moved by offset, without wrapping round the edges (art cut off by its frame would come back)."""
    m = blank(*mask.size)
    m.paste(mask, offset)
    return m


def blurred(mask, color, radius, alpha=255, offset=(0, 0)):
    m = shifted(mask, offset).filter(ImageFilter.GaussianBlur(radius))
    return flat(m, color, alpha)


def grow(mask, px):
    return mask.filter(ImageFilter.MaxFilter(px * 2 + 1)) if px else mask


def stack(size, layers):
    out = Image.new("RGBA", size, (0, 0, 0, 0))
    for layer in layers:
        out.alpha_composite(layer)
    return out


def emblem(mask, ramp=GOLD, bevel=10, outline=6):
    """A raised metal symbol: drop shadow, dark outline, beveled face."""
    return [
        blurred(grow(mask, outline), (0, 0, 0), 14, 200, (10, 14)),
        flat(grow(mask, outline), (12, 8, 2)),
        metal(mask, bevel, ramp),
    ]


def finish(img, w, h):
    return img.resize((w, h), Image.LANCZOS)


# Icons: a painted square in the icon's colour, darker towards the corners, with a raised gold emblem.

N = 128 * S   # icon design size
C = N // 2


def icon_background(color):
    radial = Image.radial_gradient("L").resize((N, N), Image.BICUBIC)   # 0 in the middle, 255 at the edge midpoints
    light = ImageOps.colorize(radial, black=tuple(min(255, int(c * 1.6)) for c in color), white=tuple(c // 2 for c in color))
    brush = Image.effect_noise((N // 32, N // 32), 48).resize((N, N), Image.BICUBIC)   # broad painted strokes
    fine = Image.effect_noise((N // 8, N // 8), 16).resize((N, N), Image.BICUBIC)
    texture = ImageChops.add(ImageOps.colorize(brush, (0, 0, 0), (255, 255, 255)), ImageOps.colorize(fine, (0, 0, 0), (255, 255, 255)), 2, -128)
    bg = ImageChops.overlay(light, texture)
    # Corner vignette.
    vignette = Image.radial_gradient("L").resize((N, N), Image.BICUBIC).point(lambda v: max(0, v - 120) * 2)
    bg = Image.composite(Image.new("RGB", (N, N), (0, 0, 0)), bg, vignette.point(lambda v: min(255, int(v * 0.45))))
    return bg.convert("RGBA")


def arrowhead(cx, cy, angle, size):
    """A triangle pointing along angle (radians), tip at (cx, cy)."""
    back = angle + math.pi
    return [
        (cx, cy),
        (cx + size * math.cos(back + 0.5), cy + size * math.sin(back + 0.5)),
        (cx + size * math.cos(back - 0.5), cy + size * math.sin(back - 0.5)),
    ]


def arc_arrow(m, cx, cy, r, start, end, width, head):
    """A thick arc from start to end (degrees, clockwise on screen) with a head at the end."""
    d = ImageDraw.Draw(m)
    mid = r - width / 2   # PIL draws the width inwards from the box
    d.arc((cx - r, cy - r, cx + r, cy + r), start, end - math.degrees(head * 0.7 / mid), fill=255, width=width)
    a = math.radians(end)
    d.polygon(arrowhead(cx + mid * math.cos(a), cy + mid * math.sin(a), a + math.pi / 2 - head * 0.4 / mid, head), fill=255)


def icon(color, mask, extra=()):
    return finish(stack((N, N), [icon_background(color)] + emblem(mask) + list(extra)), 128, 128)


def icon_jump():
    m = blank(N, N)
    d = ImageDraw.Draw(m)
    # An upward arrow over a curved patch of ground, with two motion streaks.
    d.polygon([(C, 70), (C + 120, 210), (C + 48, 210), (C + 48, 330), (C - 48, 330), (C - 48, 210), (C - 120, 210)], fill=255)
    d.arc((C - 190, 360, C + 190, 520), 200, 340, fill=255, width=26)
    return icon((50, 110, 200), m)


def icon_target():
    m = blank(N, N)
    d = ImageDraw.Draw(m)
    d.ellipse((C - 120, C - 120, C + 120, C + 120), outline=255, width=28)
    for a in range(4):
        x, y = math.cos(a * math.pi / 2), math.sin(a * math.pi / 2)
        d.line((C + x * 60, C + y * 60, C + x * 160, C + y * 160), fill=255, width=26)
    disc(m, C, C, 22)
    arc_arrow(m, C, C, 215, 200, 300, 26, 70)
    arc_arrow(m, C, C, 215, 20, 120, 26, 70)
    return icon((170, 24, 20), m)


def icon_interact():
    m = blank(N, N)
    d = ImageDraw.Draw(m)
    # The gossip speech bubble, with three dots cut through to the background.
    d.rounded_rectangle((70, 100, N - 70, 360), radius=90, fill=255)
    d.polygon([(150, 330), (250, 340), (110, 450)], fill=255)
    for dx in (-100, 0, 100):
        disc(m, C + dx, 230, 32, 0)
    return icon((150, 96, 30), m)


def icon_autorun():
    m = blank(N, N)
    d = ImageDraw.Draw(m)
    arc_arrow(m, C, C, 190, 120, 400, 34, 96)
    for y in (C - 30, C + 60):
        d.line((C - 80, y + 50, C, y - 30, C + 80, y + 50), fill=255, width=40, joint="curve")
    return icon((40, 130, 50), m)


def icon_menu():
    m = blank(N, N)
    d = ImageDraw.Draw(m)
    teeth, outer, inner = 8, 205, 160
    points = []
    for i in range(teeth * 4):
        a = (i / (teeth * 4)) * 2 * math.pi
        r = outer if i % 4 in (1, 2) else inner
        points.append((C + r * math.cos(a), C + r * math.sin(a)))
    d.polygon(points, fill=255)
    disc(m, C, C, 70, 0)
    return icon((96, 84, 140), m)


def icon_page():
    m = blank(N, N)
    d = ImageDraw.Draw(m)
    d.rounded_rectangle((80, 150, 330, 230), radius=26, fill=255)
    d.rounded_rectangle((80, 282, 330, 362), radius=26, fill=255)
    d.polygon([(410, 110), (480, 220), (340, 220)], fill=255)
    d.polygon([(410, 402), (480, 292), (340, 292)], fill=255)
    return icon((30, 120, 130), m)


def icon_sit():
    m = blank(N, N)
    d = ImageDraw.Draw(m)
    # Two crossed logs under a flame.
    for sign in (-1, 1):
        log = blank(N, N)
        ImageDraw.Draw(log).rounded_rectangle((C - 190, 380, C + 190, 440), radius=30, fill=255)
        m = ImageChops.lighter(m, log.rotate(sign * 16, center=(C, 410), resample=Image.BICUBIC))
    def flame_shape(base_y, width, height, lean):
        f = blank(N, N)
        pts = []
        for i in range(61):   # the round bottom
            a = math.pi * i / 60
            pts.append((C + width * math.cos(a), base_y + width * 0.8 * math.sin(a)))
        for i in range(61):   # up the left side to the tip and down the right, narrowing like a candle flame
            t = i / 60
            side = -1 if t < 0.5 else 1
            k = t * 2 if t < 0.5 else (1 - t) * 2   # 0 at the base, 1 at the tip
            half = width * (1 - k) ** 1.4
            pts.append((C + side * half + lean * k * k + 18 * math.sin(k * 7), base_y - height * k))
        ImageDraw.Draw(f).polygon(pts, fill=255)
        return f

    flame = flame_shape(330, 105, 270, 40)
    core = flame_shape(345, 55, 150, 20)
    fire = [
        blurred(flame, (255, 120, 20), 44, 230),
        metal(flame, 16, FIRE, amp=4, grit=0.05),
        blurred(core, (255, 250, 200), 10, 230),
    ]
    return icon((150, 60, 20), m, fire)


def stick_ring():
    w = 256 * S
    c = w // 2
    body = ring_mask(w, w, c, c, 470, 320)
    iron = metal(body, 14, IRON, amp=5, grit=0.18)
    lips = ImageChops.lighter(ring_mask(w, w, c, c, 474, 446), ring_mask(w, w, c, c, 346, 316))
    studs = blank(w, w)
    for i in range(8):
        a = (i + 0.5) * math.pi / 4
        disc(studs, c + 396 * math.cos(a), c + 396 * math.sin(a), 18)
    notches = blank(w, w)
    nd = ImageDraw.Draw(notches)
    for i in range(4):
        a = i * math.pi / 2
        nd.polygon(arrowhead(c + 432 * math.cos(a), c + 432 * math.sin(a), a, 64), fill=255)
    gold = ImageChops.lighter(ImageChops.lighter(lips, studs), notches)
    well = blank(w, w)
    disc(well, c, c, 320, 255)
    layers = [
        blurred(grow(body, 6), (0, 0, 0), 20, 180, (8, 12)),
        flat(well, (0, 0, 0), 70),   # a faint dark well so the stick reads over bright ground
        blurred(ring_mask(w, w, c, c, 330, 290), (0, 0, 0), 18, 160),   # inner lip shadow
        iron,
    ] + emblem(gold, bevel=6, outline=4)
    return finish(stack((w, w), layers), 256, 256)


def stick_knob():
    w = 128 * S
    c = w // 2
    # The dome: a height of sqrt(1 - r^2) gives the light a round falloff.
    dome = Image.new("L", (w, w))
    px = dome.load()
    R = 182
    for y in range(w):
        for x in range(w):
            r = math.hypot(x - c, y - c) / R
            px[x, y] = int(255 * math.sqrt(1 - r * r)) if r < 1 else 0
    cap = blank(w, w)
    disc(cap, c, c, R)
    lit = dome.filter(ImageFilter.Kernel((3, 3), [-1, -1, 0, -1, 0, 1, 0, 1, 1], scale=1 / 18, offset=128))
    lit = Image.blend(lit, Image.effect_noise((w, w), 64).filter(ImageFilter.GaussianBlur(3)), 0.08)
    rim = ring_mask(w, w, c, c, 212, 178)
    grip = ring_mask(w, w, c, c, 70, 52)
    layers = [
        blurred(rim, (0, 0, 0), 22, 200, (10, 16)),
        metal(cap, 0, IRON, lit=lit),
        flat(ring_mask(w, w, c, c, 184, 154), (0, 0, 0), 90),   # the dome sits under the rim
    ] + emblem(rim, bevel=8, outline=4) + emblem(grip, bevel=6, outline=3)
    return finish(stack((w, w), layers), 128, 128)


def rotate_card():
    w, h = 512 * S, 256 * S
    m = blank(w, h)
    d = ImageDraw.Draw(m)
    # A phone held upright (left), turning to landscape (right), with the turn arrow over them.
    d.rounded_rectangle((260, 330, 520, 860), radius=50, outline=255, width=30)
    d.rounded_rectangle((350, 370, 430, 384), radius=7, fill=255)
    d.rounded_rectangle((1300, 470, 1830, 730), radius=50, outline=255, width=30)
    d.rounded_rectangle((1340, 560, 1354, 640), radius=7, fill=255)
    arc_arrow(m, 1024, 900, 640, 222, 312, 34, 110)
    return finish(stack((w, h), emblem(m, bevel=9, outline=6)), 512, 256)


BRONZE = ((24, 14, 4), (128, 88, 38), (226, 190, 120))
STONE = ((20, 18, 16), (62, 57, 50), (118, 110, 96))


def stone(mask, bevel):
    """Dark weathered stone like the main action bar: broad painted blotches in two tints, grain and pitting, a soft
    bevel and a wash of light from the top left."""
    w, h = mask.size
    def noise(cell, sigma):
        return Image.effect_noise((max(1, w // cell), max(1, h // cell)), sigma).resize((w, h), Image.BICUBIC)
    grain = Image.blend(Image.blend(noise(96, 50), noise(24, 40), 0.35), noise(4, 30), 0.2)
    grain = Image.blend(grain, noise(1, 90).filter(ImageFilter.GaussianBlur(1.5)), 0.12)   # pitting
    ramp = Image.linear_gradient("L")
    wash = ImageOps.invert(ImageChops.add(ramp, ramp.rotate(90), 2)).resize((w, h), Image.BICUBIC)   # bright at the top left
    height = Image.blend(grain, wash, 0.4)
    lit = shade(mask, bevel, amp=5, grit=0.1)
    tone = ImageChops.overlay(height.convert("RGB"), lit.convert("RGB")).convert("L")
    grey = ImageOps.colorize(tone, black=STONE[0], mid=STONE[1], white=STONE[2])
    brown = ImageOps.colorize(tone, black=(22, 16, 10), mid=(66, 52, 38), white=(122, 102, 78))
    rgb = Image.composite(brown, grey, noise(160, 70))
    rgb.putalpha(mask)
    return rgb


def chevron(m, cx, cy, half, rise, width):
    ImageDraw.Draw(m).line((cx - half, cy + rise / 2, cx, cy - rise / 2, cx + half, cy + rise / 2), fill=255, width=width, joint="curve")


def tray_handle():
    w, h = 128 * S, 64 * S
    body = blank(w, h)
    ImageDraw.Draw(body).rounded_rectangle((18, 18, w - 18, h - 14), radius=70, fill=255)
    face = blank(w, h)
    ImageDraw.Draw(face).rounded_rectangle((44, 44, w - 44, h - 40), radius=46, fill=255)
    rim = ImageChops.subtract(body, face)
    mark = blank(w, h)
    chevron(mark, w // 2, h // 2 + 6, 100, 74, 30)
    layers = [
        blurred(grow(body, 4), (0, 0, 0), 14, 190, (6, 10)),
        stone(body, 8),
        blurred(rim, (0, 0, 0), 10, 150, (4, 6)),
        flat(grow(rim, 4), (14, 9, 3)),
        metal(rim, 7, BRONZE, amp=5, grit=0.16),
    ] + emblem(mark, bevel=6, outline=4)
    return finish(stack((w, h), layers), 128, 64)


ART = {
    "StickRing": stick_ring,
    "StickKnob": stick_knob,
    "Jump": icon_jump,
    "Target": icon_target,
    "Interact": icon_interact,
    "AutoRun": icon_autorun,
    "Menu": icon_menu,
    "Page": icon_page,
    "Sit": icon_sit,
    "Rotate": rotate_card,
    "TrayHandle": tray_handle,
}


def contact_sheet(images):
    sheet = Image.new("RGBA", (1100, 1100), (96, 96, 96, 255))
    x, y, row = 20, 20, 0
    for name, img in images.items():
        if x + img.width > sheet.width - 20:
            x, y, row = 20, y + row + 20, 0
        sheet.alpha_composite(img, (x, y))
        x, row = x + img.width + 20, max(row, img.height)
    return sheet


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    os.makedirs(sys.argv[1], exist_ok=True)
    images = {name: draw() for name, draw in ART.items()}
    for name, img in images.items():
        img.save(os.path.join(sys.argv[1], f"{name}.tga"), compression="tga_rle")
    if len(sys.argv) == 3:
        contact_sheet(images).save(sys.argv[2])
