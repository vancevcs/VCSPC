"""Generate the VCS front-end art: the menu backdrop and the script-font page titles.

Both are baked to PNG rather than drawn at runtime, for the same reason the game bakes them:
the page titles are set in a brush script that PPSSPP's text renderer cannot reach (its FontStyle
picks a *family* - SansSerif or Fixed - with one face per family globally, so there is no way to
ask for a script face for one string and a sans face for the next), and the backdrop is a piece of
art, not a widget.

    python Tools/vcsmenuart.py

Writes into assets/vcs/, which ships with the build and is read through g_VFS at runtime.
"""

import math
import os

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(os.path.dirname(HERE), "assets", "vcs")

# The palette, sampled from the front end this is imitating.
BG_TOP = (70, 14, 88)
BG_BOTTOM = (48, 8, 60)
FROND = (43, 7, 54)
DISC = (54, 10, 68)

TITLE_PINK = (243, 154, 200)
TITLE_SHADOW = (26, 4, 33)

# Page titles. The key is what the C++ side asks for; keep the two in step.
TITLES = {
    "paused": "Paused",
    "mouse": "Mouse",
    "aiming": "Aiming",
    "mainmenu": "Main Menu",
    "game": "Game",
    "controls": "Controls",
    "settings": "Settings",
    "controller": "Controller",
    "bindings": "Bindings",
    "display": "Display",
    "graphics": "Graphics",
    "audio": "Audio",
    "quit": "Quit Game",
    "loadgame": "Load Game",
    "deletegame": "Delete Game",
    "onfoot": "On Foot",
    "invehicle": "In Vehicle",
    "aircraft": "Aircraft",
    "melee": "Melee Combat",
    "cheats": "Cheats",
    "player": "Player",
    "vehicles": "Vehicles",
    "pedestrians": "Pedestrians",
    "world": "World",
    "options": "Options",
    "gameplay": "Gameplay",
    "controllersetup": "Controller Setup",
    "mousesettings": "Mouse Settings",
    "keybindings": "Key Bindings",
    "audiosetup": "Audio Setup",
    "displaysetup": "Display Setup",
}

# Brush Script MT is the closest thing Windows ships to the logo script. Freestyle Script is the
# fallback; both are stock, so no font is redistributed with the build - only the rendered pixels.
FONT_CANDIDATES = [
    "C:/Windows/Fonts/BRUSHSCI.TTF",
    "C:/Windows/Fonts/FRSCRIPT.TTF",
    "C:/Windows/Fonts/segoesc.ttf",
]


def pick_font(size):
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            return ImageFont.truetype(path, size), os.path.basename(path)
    raise SystemExit("no script font found; looked for %s" % ", ".join(FONT_CANDIDATES))


def draw_frond(draw, x0, y0, angle_deg, length, curve, leaf_max, color, leaflets=64):
    """One palm frond: a curved spine with tapered leaflets swept back along it.

    The spine is a straight line plus a sine bulge on its perpendicular, which is enough of an
    arc to read as a frond. Leaflets are triangles angled back toward the base - swept forward
    they read as a fern instead.

    The leaflet base width is derived from the spacing rather than picked, because that is what
    decides whether this reads as a palm or as a fishbone. Narrower than the gap between
    leaflets and the frond becomes a row of separate spines with the background showing between
    them; at 0.8 of the spacing they overlap into one silhouette with a jagged edge, which is
    the shape the eye recognises.
    """
    a = math.radians(angle_deg)
    dx, dy = math.cos(a), math.sin(a)
    px, py = -dy, dx  # perpendicular

    def spine(t):
        bulge = curve * math.sin(math.pi * t)
        return (x0 + dx * length * t + px * bulge,
                y0 + dy * length * t + py * bulge)

    spacing = length / float(leaflets)
    half_base = spacing * 0.8

    for i in range(leaflets):
        t = (i + 0.5) / leaflets
        cx, cy = spine(t)

        # Longest in the middle, short at both ends.
        leaf = leaf_max * math.sin(math.pi * t) ** 0.55
        if leaf < 2.0:
            continue

        # Local spine direction, so leaflets follow the curve rather than the chord.
        t2 = min(1.0, t + 0.01)
        sx, sy = spine(t2)
        ldx, ldy = sx - cx, sy - cy
        norm = math.hypot(ldx, ldy) or 1.0
        ldx, ldy = ldx / norm, ldy / norm

        for side in (-1, 1):
            # Swept back ~62 degrees from the spine direction.
            sweep = math.radians(-62 * side)
            ex = ldx * math.cos(sweep) - ldy * math.sin(sweep)
            ey = ldx * math.sin(sweep) + ldy * math.cos(sweep)
            # Perpendicular of the leaflet, for its width at the base.
            wx, wy = -ey * half_base, ex * half_base
            tipx, tipy = cx + ex * leaf, cy + ey * leaf
            draw.polygon(
                [(cx - wx, cy - wy), (cx + wx, cy + wy), (tipx, tipy)],
                fill=color,
            )

    # The rib itself, thick enough to close the seam the two leaflet rows leave along it.
    pts = [spine(i / 60.0) for i in range(61)]
    draw.line(pts, fill=color, width=int(half_base * 1.5) + 2, joint="curve")


def make_background(width=1280, height=720):
    img = Image.new("RGB", (width, height), BG_TOP)
    draw = ImageDraw.Draw(img)

    for y in range(height):
        t = y / float(height - 1)
        draw.line(
            [(0, y), (width, y)],
            fill=tuple(int(BG_TOP[i] + (BG_BOTTOM[i] - BG_TOP[i]) * t) for i in range(3)),
        )

    # The disc sitting behind the fronds on the right.
    cx, cy, r = width * 0.78, height * 0.42, height * 0.62
    draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=DISC)

    # Fronds, roughly the arrangement of the original: two big ones sweeping in from the right
    # edge, and smaller ones climbing out of the bottom left corner.
    draw_frond(draw, width * 1.02, height * 0.16, 172, width * 0.72, -height * 0.20, 118, FROND)
    draw_frond(draw, width * 1.02, height * 0.62, 190, width * 0.70, height * 0.20, 112, FROND)
    draw_frond(draw, width * 0.98, height * 0.95, 205, width * 0.52, height * 0.10, 84, FROND)
    draw_frond(draw, -width * 0.04, height * 0.82, -18, width * 0.34, -height * 0.10, 74, FROND)
    draw_frond(draw, -width * 0.02, height * 1.02, -32, width * 0.30, -height * 0.08, 66, FROND)

    path = os.path.join(OUT_DIR, "menu_bg.png")
    img.save(path)
    return path, img.size


def make_title(key, text, size=96):
    font, font_name = pick_font(size)

    # Measure first, then pad for the drop shadow and the script's overhang.
    probe = Image.new("RGBA", (16, 16))
    box = ImageDraw.Draw(probe).textbbox((0, 0), text, font=font)
    pad = size // 3
    w = box[2] - box[0] + pad * 2
    h = box[3] - box[1] + pad * 2

    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    ox, oy = pad - box[0], pad - box[1]
    off = max(3, size // 24)
    draw.text((ox + off, oy + off), text, font=font, fill=TITLE_SHADOW + (255,))
    draw.text((ox, oy), text, font=font, fill=TITLE_PINK + (255,))

    path = os.path.join(OUT_DIR, "title_%s.png" % key)
    img.save(path)
    return path, img.size, font_name


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    # make_background() is deliberately not called: the menu draws a flat fill in the backdrop's
    # own base colour instead of loading an image. The generator is kept because it is the only
    # record of how that backdrop was built - see the leaflet-width note in CLAUDE.md - and
    # calling it here again is all it takes to go back to the patterned version.

    used_font = None
    for key, text in sorted(TITLES.items()):
        path, size, used_font = make_title(key, text)
        print("%-34s %dx%d  %r" % (os.path.relpath(path, os.path.dirname(HERE)),
                                   size[0], size[1], text))
    print("\ntitles rendered with %s" % used_font)


if __name__ == "__main__":
    main()
