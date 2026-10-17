"""Renders the login screen title (WebAccount.cpp) as a texture, styled like the client's zone text: Morpheus in a gold
gradient with a dark outline and a soft shadow. The client draws glyphs at 32 px at most, so a large title drawn as
text is blurry; a texture stays sharp. The font is Blizzard's (Fonts\\MORPHEUS.TTF in the game data), so the output
goes next to the game files on the server, never into the repository:

    python Tools/Bridge/make-logo.py MORPHEUS.TTF <game dir>/Interface/Glues/LasthavensLogo.tga

With a loading-screen painting (from extract-art.mjs) it also writes the 1200 x 630 link-preview image the page's
Open Graph tags point at (index.html), into the web directory:

    python Tools/Bridge/make-logo.py MORPHEUS.TTF <tga> <web dir>/loading-0.png <web dir>/og-image.jpg
"""

import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

TEXT = "Lasthavens"
TAGLINE = "A Wrath of the Lich King realm you play in your browser"
WIDTH, HEIGHT = 1024, 256   # 4:1, drawn at 360 x 90 UI units
SIZE = 168
OUTLINE = 7
OG_WIDTH, OG_HEIGHT = 1200, 630


def render_title(font_path):
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

    clear = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    image = Image.alpha_composite(clear, shadow)
    image = Image.alpha_composite(image, Image.composite(Image.new("RGBA", (WIDTH, HEIGHT), (28, 16, 6, 255)), clear, outlined))
    image = Image.alpha_composite(image, Image.composite(gradient, clear, glyphs))

    # Fully transparent pixels black, so texture filtering at the edges doesn't bleed colour in.
    r, g, b, a = image.split()
    black = Image.new("L", (WIDTH, HEIGHT), 0)
    visible = a.point(lambda v: 255 if v else 0)
    return Image.merge("RGBA", [Image.composite(c, black, visible) for c in (r, g, b)] + [a])


def render_preview(title, font_path, painting_path):
    # The painting (a 1024 x 1024 texture the client stretches to 16:10) stretched the same way, then cropped to 1.91:1
    # from the bottom: the paintings carry the WoW logo at the top, and the card has its own.
    painting = Image.open(painting_path).convert("RGB").resize((OG_WIDTH, OG_WIDTH * 10 // 16), Image.LANCZOS)
    top = painting.height - OG_HEIGHT
    card = painting.crop((0, top, OG_WIDTH, top + OG_HEIGHT)).convert("RGBA")

    # Darken towards the middle band so the title and tagline read on any painting.
    shade = Image.new("L", (OG_WIDTH, OG_HEIGHT), 0)
    draw = ImageDraw.Draw(shade)
    for y in range(OG_HEIGHT):
        distance = abs(y - OG_HEIGHT * 0.48) / (OG_HEIGHT * 0.5)
        fade = max(0.0, 1.0 - y / 90) * 215   # what is left of the WoW logo along the top edge
        draw.line([(0, y), (OG_WIDTH, y)], fill=min(255, round(150 * max(0.0, 1.0 - distance) + 40 + fade)))
    card = Image.alpha_composite(card, Image.merge("RGBA", [Image.new("L", card.size, 0)] * 3 + [shade]))

    logo = title.resize((1000, 250), Image.LANCZOS)
    card.alpha_composite(logo, ((OG_WIDTH - logo.width) // 2, 150))

    font = ImageFont.truetype(font_path, 40)
    left, _, right, _ = font.getbbox(TAGLINE)
    position = ((OG_WIDTH - (right - left)) // 2 - left, 400)
    draw = ImageDraw.Draw(card)
    draw.text((position[0] + 2, position[1] + 2), TAGLINE, font=font, fill=(0, 0, 0, 220))
    draw.text(position, TAGLINE, font=font, fill=(214, 228, 255, 255))
    return card.convert("RGB")


if __name__ == "__main__":
    if len(sys.argv) not in (3, 5):
        sys.exit(__doc__)
    title = render_title(sys.argv[1])
    title.save(sys.argv[2], compression="tga_rle")   # mostly transparent, so RLE (TGA type 10) is far smaller
    if len(sys.argv) == 5:
        render_preview(title, sys.argv[1], sys.argv[3]).save(sys.argv[4], quality=88, optimize=True)   # .jpg: a fraction of PNG
