"""Renders the login screen title (WebAccount.cpp) as a texture, styled like the client's zone text: Morpheus in a gold
gradient with a dark outline and a soft shadow. The client draws glyphs at 32 px at most, so a large title drawn as
text is blurry; a texture stays sharp. The font is Blizzard's (Fonts\\MORPHEUS.TTF in the game data), so the output
goes next to the game files on the server, never into the repository:

    python Tools/Bridge/make-logo.py MORPHEUS.TTF <game dir>/Interface/Glues/LasthavensLogo.tga

With a loading-screen painting (from extract-art.mjs) it also writes the 1200 x 630 link-preview image the page's
Open Graph tags point at (index.html), into the web directory:

    python Tools/Bridge/make-logo.py MORPHEUS.TTF <tga> <web dir>/loading-0.png <web dir>/og-image.jpg

With Friz Quadrata (Fonts\\FRIZQT__.TTF) and an output directory it also writes the Discord server's 960 x 540 banner
and 512 x 512 icon (discord-banner.png, discord-icon.png) from the same painting, the Northrend one (loading-0.png).
They are uploaded to Discord by hand, so they go anywhere outside the repository:

    python Tools/Bridge/make-logo.py MORPHEUS.TTF <tga> <web dir>/loading-0.png <web dir>/og-image.jpg FRIZQT__.TTF <dir>
"""

import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

TEXT = "Lasthavens"
TAGLINE = "A Wrath of the Lich King realm you play in your browser"
WIDTH, HEIGHT = 1024, 256   # 4:1, drawn at 360 x 90 UI units
SIZE = 168
OUTLINE = 7
OG_WIDTH, OG_HEIGHT = 1200, 630
SUBTITLE = "Wrath of the Lich King"
BANNER_WIDTH, BANNER_HEIGHT = 960, 540
ICON_SIZE = 512


def render_title(font_path, text=TEXT, width=WIDTH, height=HEIGHT, size=SIZE, outline=OUTLINE):
    font = ImageFont.truetype(font_path, size)
    left, top, right, bottom = font.getbbox(text, stroke_width=outline)
    origin = ((width - (right - left)) // 2 - left, (height - (bottom - top)) // 2 - top)

    def mask(stroke):
        m = Image.new("L", (width, height), 0)
        ImageDraw.Draw(m).text(origin, text, font=font, fill=255, stroke_width=stroke, stroke_fill=255)
        return m

    glyphs, outlined = mask(0), mask(outline)

    # Gold, light at the top and burnished at the bottom, as the zone text font's colour ramp.
    gradient = Image.new("RGBA", (width, height))
    stops = [(0.0, (255, 244, 190)), (0.45, (255, 210, 64)), (1.0, (170, 104, 22))]
    for y in range(height):
        t = (y - origin[1] - top) / max(1, bottom - top)
        t = min(max(t, 0.0), 1.0)
        for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
            if t <= t1:
                k = (t - t0) / (t1 - t0)
                color = tuple(round(a + (b - a) * k) for a, b in zip(c0, c1))
                break
        ImageDraw.Draw(gradient).line([(0, y), (width, y)], fill=color + (255,))

    shadow = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    shadow.paste((0, 0, 0, 170), (6, 7), outlined.crop((0, 0, width - 6, height - 7)))
    shadow = shadow.filter(ImageFilter.GaussianBlur(5))

    clear = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    image = Image.alpha_composite(clear, shadow)
    image = Image.alpha_composite(image, Image.composite(Image.new("RGBA", (width, height), (28, 16, 6, 255)), clear, outlined))
    image = Image.alpha_composite(image, Image.composite(gradient, clear, glyphs))

    # Fully transparent pixels black, so texture filtering at the edges doesn't bleed colour in.
    r, g, b, a = image.split()
    black = Image.new("L", (width, height), 0)
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


def render_discord(title, font_path, subtitle_font_path, painting_path):
    # The Northrend painting stretched to 16:10 like the client, cropped to 16:9 from the top: Arthas on the left,
    # Frostmourne on the right, and the WoW logo at the top centre, filled in from the pixels around it (a blur that
    # ignores the logo's own pixels; coarse first so the middle of the hole, far from any of them, still gets a colour).
    painting = Image.open(painting_path).convert("RGB")
    banner = painting.resize((BANNER_WIDTH, BANNER_WIDTH * 10 // 16), Image.LANCZOS)
    banner = banner.crop((0, 0, BANNER_WIDTH, BANNER_HEIGHT)).convert("RGBA")
    hole = Image.new("L", banner.size, 0)
    ImageDraw.Draw(hole).rounded_rectangle((336, 0, 624, 142), 30, fill=255)
    known = banner.copy()
    known.putalpha(Image.eval(hole, lambda v: 255 - v))
    fill = Image.new("RGBA", banner.size, (0, 0, 0, 255))
    for radius in (120, 50, 20):
        fill = Image.alpha_composite(fill, known.convert("RGBa").filter(ImageFilter.GaussianBlur(radius)).convert("RGBA"))
    banner = Image.composite(fill, banner, hole.filter(ImageFilter.GaussianBlur(14)).point(lambda v: min(255, v * 2)))

    # Darker along the top (where Discord draws the server name) and behind the title.
    middle = BANNER_HEIGHT * 0.62
    shade = Image.new("L", banner.size, 0)
    draw = ImageDraw.Draw(shade)
    for y in range(BANNER_HEIGHT):
        top = max(0.0, 1.0 - y / 190) * 200
        band = max(0.0, 1.0 - abs(y - middle) / (BANNER_HEIGHT * 0.3)) * 150
        draw.line([(0, y), (BANNER_WIDTH, y)], fill=min(255, round(top + band + 25)))
    banner = Image.alpha_composite(banner, Image.merge("RGBA", [Image.new("L", banner.size, 0)] * 3 + [shade]))

    # The title over a line of Friz Quadrata in the login screen's pale blue, as on the login screen.
    logo = title.resize((800, 200), Image.LANCZOS)
    banner.alpha_composite(logo, ((BANNER_WIDTH - logo.width) // 2, round(middle) - 120))
    font = ImageFont.truetype(subtitle_font_path, 30)
    left, _, right, _ = font.getbbox(SUBTITLE, stroke_width=2)
    x, y = (BANNER_WIDTH - (right - left)) // 2 - left, round(middle) + 52
    draw = ImageDraw.Draw(banner)
    draw.text((x + 2, y + 3), SUBTITLE, font=font, fill=(0, 0, 0, 200), stroke_width=2, stroke_fill=(0, 0, 0, 200))
    draw.text((x, y), SUBTITLE, font=font, fill=(199, 224, 255, 255), stroke_width=2, stroke_fill=(10, 14, 24, 255))

    # The icon: the title's "L" over Frostmourne, darkened towards the edge (Discord shows it as a circle).
    wide = painting.resize((1600, 1000), Image.LANCZOS)
    icon = wide.crop((900, 250, 1500, 850)).resize((ICON_SIZE, ICON_SIZE), Image.LANCZOS).convert("RGBA")
    black = Image.new("RGBA", icon.size, (0, 0, 0, 255))
    icon = Image.blend(icon, black, 0.45)
    lit = Image.new("L", icon.size, 0)
    ImageDraw.Draw(lit).ellipse((40, 40, ICON_SIZE - 40, ICON_SIZE - 40), fill=255)
    icon = Image.composite(icon, Image.blend(icon, black, 0.6), lit.filter(ImageFilter.GaussianBlur(70)))
    icon.alpha_composite(render_title(font_path, TEXT[0], ICON_SIZE, ICON_SIZE, 400, 14), (0, 10))
    return banner.convert("RGB"), icon.convert("RGB")


if __name__ == "__main__":
    if len(sys.argv) not in (3, 5, 7):
        sys.exit(__doc__)
    title = render_title(sys.argv[1])
    title.save(sys.argv[2], compression="tga_rle")   # mostly transparent, so RLE (TGA type 10) is far smaller
    if len(sys.argv) >= 5:
        render_preview(title, sys.argv[1], sys.argv[3]).save(sys.argv[4], quality=88, optimize=True)   # .jpg: a fraction of PNG
    if len(sys.argv) == 7:
        banner, icon = render_discord(title, sys.argv[1], sys.argv[5], sys.argv[3])
        banner.save(f"{sys.argv[6]}/discord-banner.png", optimize=True)
        icon.save(f"{sys.argv[6]}/discord-icon.png", optimize=True)
