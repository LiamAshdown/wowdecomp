"""Renders the login screen title (WebAccount.cpp) as a texture, styled like the client's zone text: Morpheus in a gold
gradient with a dark outline and a soft shadow. The client draws glyphs at 32 px at most, so a large title drawn as
text is blurry; a texture stays sharp. The font is Blizzard's (Fonts\\MORPHEUS.TTF in the game data), so the output
goes next to the game files on the server, never into the repository:

    python Tools/Bridge/make-logo.py MORPHEUS.TTF <game dir>/Interface/Glues/LasthavensLogo.tga
"""

import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

TEXT = "Lasthavens"
WIDTH, HEIGHT = 1024, 256   # 4:1, drawn at 360 x 90 UI units
SIZE = 168
OUTLINE = 7


def main(font_path, out_path):
    font = ImageFont.truetype(font_path, SIZE)
    left, top, right, bottom = font.getbbox(TEXT, stroke_width=OUTLINE)
    origin = ((WIDTH - (right - left)) // 2 - left, (HEIGHT - (bottom - top)) // 2 - top)

    def mask(stroke):
        m = Image.new("L", (WIDTH, HEIGHT), 0)
        ImageDraw.Draw(m).text(origin, TEXT, font=font, fill=255, stroke_width=stroke, stroke_fill=255)
        return m

    glyphs, outlined = mask(0), mask(OUTLINE)

    # Gold, light at the top and burnished at the bottom, as the zone text font's colour ramp.
    gradient = Image.new("RGBA", (WIDTH, HEIGHT))
    stops = [(0.0, (255, 244, 190)), (0.45, (255, 210, 64)), (1.0, (170, 104, 22))]
    for y in range(HEIGHT):
        t = (y - origin[1] - top) / max(1, bottom - top)
        t = min(max(t, 0.0), 1.0)
        for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
            if t <= t1:
                k = (t - t0) / (t1 - t0)
                color = tuple(round(a + (b - a) * k) for a, b in zip(c0, c1))
                break
        ImageDraw.Draw(gradient).line([(0, y), (WIDTH, y)], fill=color + (255,))

    shadow = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    shadow.paste((0, 0, 0, 170), (6, 7), outlined.crop((0, 0, WIDTH - 6, HEIGHT - 7)))
    shadow = shadow.filter(ImageFilter.GaussianBlur(5))

    image = Image.alpha_composite(Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0)), shadow)
    image = Image.alpha_composite(image, Image.composite(Image.new("RGBA", (WIDTH, HEIGHT), (28, 16, 6, 255)),
                                                         Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0)), outlined))
    image = Image.alpha_composite(image, Image.composite(gradient, Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0)), glyphs))

    # Fully transparent pixels black, so texture filtering at the edges doesn't bleed colour in.
    r, g, b, a = image.split()
    black = Image.new("L", (WIDTH, HEIGHT), 0)
    visible = a.point(lambda v: 255 if v else 0)
    image = Image.merge("RGBA", [Image.composite(c, black, visible) for c in (r, g, b)] + [a])
    image.save(out_path, compression="tga_rle")   # mostly transparent, so RLE (TGA type 10) is far smaller


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
