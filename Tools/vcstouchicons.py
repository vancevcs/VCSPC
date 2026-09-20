"""Bake the phone controls' icons into assets/vcs/touch_*.png.

    python Tools/vcstouchicons.py

WHY THERE IS A RENDERER IN HERE

The icons are line art in a 24x24 box - a few of them lifted from Tabler's open icon set, the
rest drawn here in the same idiom so the whole overlay reads as one set.  Every one of them is
SVG path data, which is the only sane way to write "a circle with two chevrons in it" by hand.

Nothing on this machine rasterises SVG: cairosvg, rsvg-convert and reportlab's renderPM backend
are all absent, and pulling one in for twenty-eight small images is a dependency the build does
not otherwise need.  So the path data is flattened to polylines here and stroked with PIL, which
is about two hundred lines and needs nothing but Pillow.

Round caps and joins are what makes line art look drawn rather than assembled, and they come out
of the stroking loop for free: a segment is a thick line, and a disc of the same diameter at
every vertex is exactly a round join and a round cap.

Output is WHITE with the shape in the alpha channel, because that is what the overlay wants - a
control tints its icon by the same opacity as everything else it draws, so an icon carrying its
own colour would be the one thing on screen that did not fade.
"""

import math
import os
import re

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(os.path.dirname(HERE), "assets", "vcs")

# The box every path below is drawn in, the stroke it is drawn with, and the size that reaches
# the game.  128 is about twice the biggest an icon is ever drawn at (a 44dp button on a phone
# at 3x), which is the usual "enough that nothing upscales" rule.
VIEWBOX = 24.0
STROKE = 2.0
OUT_SIZE = 128
# Rendered this much bigger and then reduced, which is where the antialiasing comes from.
SUPERSAMPLE = 8


# --- The icons ----------------------------------------------------------------------------------
#
# A value is either a path string or a list of them, and a path may be a (data, options) pair.
# Options: "w" overrides the stroke width, "fill" fills the subpath instead of stroking it.
#
# The four marked TABLER are the ones the player supplied, unchanged apart from being lifted out
# of their <svg> wrapper.  Everything else is drawn here against the same 24-unit box and the
# same 2-unit stroke, so a hand-drawn one sits beside a borrowed one without looking it.

ICONS = {
    # --- Supplied ---------------------------------------------------------------------------
    # TABLER run: sprint, and on this overlay the combined jump-and-sprint button.
    "run": [
        "M11.007 5a2 2 0 1 0 4 0a2 2 0 1 0 -4 0",
        "M4 17l5 1l.75 -1.5",
        "M15 21v-4l-4 -3l1 -6",
        "M7 12v-3l5 -1l3 3l3 1",
    ],
    # TABLER car: enter and exit a vehicle.
    "car": [
        "M5 17a2 2 0 1 0 4 0a2 2 0 1 0 -4 0",
        "M15 17a2 2 0 1 0 4 0a2 2 0 1 0 -4 0",
        "M5 17h-2v-6l2 -5h9l4 5h1a2 2 0 0 1 2 2v4h-2m-4 0h-6m-6 -6h15m-6 0v-5",
    ],
    # TABLER speakerphone: the horn.
    "horn": [
        "M18 8a3 3 0 0 1 0 6",
        "M10 8v11a1 1 0 0 1 -1 1h-1a1 1 0 0 1 -1 -1v-5",
        "M12 8l4.524 -3.77a.9 .9 0 0 1 1.476 .692v12.156a.9 .9 0 0 1 -1.476 .692l-4.524 -3.77h-8"
        "a1 1 0 0 1 -1 -1v-4a1 1 0 0 1 1 -1h8",
    ],
    # TABLER arrow-badge-left / -right: steering, and cycling weapons and targets.
    "arrow_left": "M11 17h6l-4 -5l4 -5h-6l-4 5l4 5",
    "arrow_right": "M13 7h-6l4 5l-4 5h6l4 -5l-4 -5",
    # TABLER hospital-circle: not a control - kept because it was supplied with the others, and
    # a health marker is the obvious next thing this overlay will want.
    "health": [
        "M10 16v-8",
        "M3 12a9 9 0 1 0 18 0a9 9 0 0 0 -18 0",
        "M14 16v-8",
        "M10 12h4",
    ],

    # --- Fighting ------------------------------------------------------------------------------
    # A fist, as a boxing glove: the blunt shape, the thumb on the side, and the cuff under it.
    # Knuckles drawn as bumps along the top were the first attempt and read as a bread roll -
    # what says "fist" at 40dp is the outline of the whole hand, not the fingers in it.
    "fist": [
        "M6.5 9.5a5 5 0 0 1 5 -5h.5a5 5 0 0 1 5 5v3a4.5 4.5 0 0 1 -4.5 4.5h-1.5a4.5 4.5 0 0 1"
        " -4.5 -4.5z",
        "M6.5 11h-1.5a1.75 1.75 0 0 0 0 3.5h1.5",
        "M9 16.8v1.7a1.5 1.5 0 0 0 1.5 1.5h3a1.5 1.5 0 0 0 1.5 -1.5v-1.7",
    ],
    # The same fist, moved over to make room for the lines a heavier blow drags behind it.
    "heavy": [
        "M9 9.5a5 5 0 0 1 5 -5h.5a5 5 0 0 1 5 5v3a4.5 4.5 0 0 1 -4.5 4.5h-1.5a4.5 4.5 0 0 1"
        " -4.5 -4.5z",
        "M9 11h-1.5a1.75 1.75 0 0 0 0 3.5h1.5",
        "M11.5 16.8v1.7a1.5 1.5 0 0 0 1.5 1.5h3a1.5 1.5 0 0 0 1.5 -1.5v-1.7",
        "M5 7l-2.5 -2",
        "M4.5 11.5h-2.5",
        "M5 16l-2.5 2",
    ],
    # A grab: an open hand, reaching.
    "grab": [
        "M8 11v-4.5a1.5 1.5 0 0 1 3 0v4.5",
        "M11 10.5v-5a1.5 1.5 0 0 1 3 0v5",
        "M14 11v-3.5a1.5 1.5 0 0 1 3 0v6.5a5 5 0 0 1 -5 5h-1.5a4.5 4.5 0 0 1 -3.2 -1.35l-2.8"
        " -2.85a1.5 1.5 0 0 1 2.1 -2.15l1.4 1.35",
    ],
    # A shield: block.
    "shield": "M12 3l7 3v5.5c0 4 -2.9 7.6 -7 8.5c-4.1 -.9 -7 -4.5 -7 -8.5v-5.5z",

    # --- Shooting --------------------------------------------------------------------------------
    # A pistol in profile, muzzle to the right, and the one icon here that is FILLED rather than
    # stroked.  A gun is read by its silhouette - slide, receiver, raked grip - and an outline of
    # that silhouette at 40dp is a tangle of parallel lines.  Three solid pieces, no trigger
    # guard: the hole would close up at this size anyway.
    "gun": [
        ("M2.5 7h17.5v3.6h-17.5z", {"fill": True}),
        ("M2.5 10.4h9v2.6h-9z", {"fill": True}),
        ("M4.2 12.8h5.3l-2 6.9h-4.6z", {"fill": True}),
    ],
    # The same pistol, firing: the two drive-by buttons, which shoot to their own side. Drawn
    # once and mirrored, so the pair cannot drift apart.
    "driveby_right": [
        ("M1.5 8h14.5v3.2h-14.5z", {"fill": True}),
        ("M1.5 10.9h7.5v2.3h-7.5z", {"fill": True}),
        ("M3 13h4.6l-1.8 6h-4z", {"fill": True}),
        "M17.8 6.6l2.6 -2",
        "M18.2 9.6h3.6",
        "M17.8 12.6l2.6 2",
    ],
    "driveby_left": ("mirror", "driveby_right"),
    # The target toggle.  A ring with a gap at each axis, which is what a reticle looks like and
    # what tells it apart from the plain circles everything else here is drawn inside.
    "crosshair": [
        "M12 5a7 7 0 1 0 0 14a7 7 0 0 0 0 -14",
        "M12 2v3",
        "M12 19v3",
        "M2 12h3",
        "M19 12h3",
        "M12 10.5a1.5 1.5 0 1 0 0 3a1.5 1.5 0 0 0 0 -3",
    ],
    # Down a scope: the same ring, with the magnifier's plus and minus.
    "zoom_in": [
        "M11 4a7 7 0 1 0 0 14a7 7 0 0 0 0 -14",
        "M16 16l4.5 4.5",
        "M11 8v6",
        "M8 11h6",
    ],
    "zoom_out": [
        "M11 4a7 7 0 1 0 0 14a7 7 0 0 0 0 -14",
        "M16 16l4.5 4.5",
        "M8 11h6",
    ],

    # --- Driving ---------------------------------------------------------------------------------
    # The two pedals, tilted the way a pedal is actually seen from the driver's seat, standing on
    # the floor.  They differ in SHAPE as well as in the grip pattern: the accelerator is the
    # narrow upright one and the brake is the wide one, which is the difference a foot feels for
    # and the one the reference layout uses.
    #
    # An untilted rounded rectangle with dots in it was the first attempt, and it read as a
    # television remote.  The tilt and the floor line are what make it a pedal.
    "pedal_gas": [
        "M9.8 2.6l5.4 1.5l-3.2 12.6l-5.4 -1.5z",
        ("M9.3 5.6a.8 .8 0 1 0 0 1.6a.8 .8 0 0 0 0 -1.6", {"fill": True}),
        ("M12.5 6.5a.8 .8 0 1 0 0 1.6a.8 .8 0 0 0 0 -1.6", {"fill": True}),
        ("M8.5 9a.8 .8 0 1 0 0 1.6a.8 .8 0 0 0 0 -1.6", {"fill": True}),
        ("M11.7 9.9a.8 .8 0 1 0 0 1.6a.8 .8 0 0 0 0 -1.6", {"fill": True}),
        "M8.2 16.4l-1.2 4",
        "M4.5 20.8h8",
    ],
    "pedal_brake": [
        "M3.4 6.9l14.8 -2.2l1 6.6l-14.8 2.2z",
        ("M6.6 8.1a.8 .8 0 1 0 0 1.6a.8 .8 0 0 0 0 -1.6", {"fill": True}),
        ("M10.6 7.5a.8 .8 0 1 0 0 1.6a.8 .8 0 0 0 0 -1.6", {"fill": True}),
        ("M14.6 6.9a.8 .8 0 1 0 0 1.6a.8 .8 0 0 0 0 -1.6", {"fill": True}),
        ("M7.2 11.3a.8 .8 0 1 0 0 1.6a.8 .8 0 0 0 0 -1.6", {"fill": True}),
        ("M11.2 10.7a.8 .8 0 1 0 0 1.6a.8 .8 0 0 0 0 -1.6", {"fill": True}),
        ("M15.2 10.1a.8 .8 0 1 0 0 1.6a.8 .8 0 0 0 0 -1.6", {"fill": True}),
        "M11 13.4v4.4",
        "M6.5 18.2h9",
    ],
    # The handbrake, as the lever it is: base, pivot, rod, and the grip at the top of it.
    #
    # It was a tyre with tread, which is what the reference screenshot uses - and beside the
    # reticle on the same overlay, a wheel with two axes through it read as a second crosshair.
    # The lever cannot be confused with anything else here.
    "handbrake": [
        "M5 19.5h7",
        "M8.5 19.5v-1.2",
        ("M8.5 18.3l6 -9.6", {"w": 2.4}),
        ("M12.6 6.2l4.4 2.7l-2.1 3.4l-4.4 -2.7z", {"fill": True}),
    ],
    # Change camera: a film camera in profile.
    "camera": [
        "M3 8h11a1 1 0 0 1 1 1v6a1 1 0 0 1 -1 1h-11a1 1 0 0 1 -1 -1v-6a1 1 0 0 1 1 -1z",
        "M15 11.5l5 -2.5v6l-5 -2.5",
        "M5.5 4a1.75 1.75 0 1 0 0 3.5a1.75 1.75 0 0 0 0 -3.5",
        "M10.5 4.5a1.75 1.75 0 1 0 0 3.5a1.75 1.75 0 0 0 0 -3.5",
    ],
    # The radio, forwards and back: one note, and the direction clear of it. Two note heads and a
    # chevron is three round things in a 24-unit box, and at button size they run together.
    "radio_next": [
        "M8 18a3 3 0 1 0 0 -6a3 3 0 0 0 0 6",
        "M11 15.5v-11l5 2",
        "M19 8l3 4l-3 4",
    ],
    "radio_prev": [
        "M13 18a3 3 0 1 0 0 -6a3 3 0 0 0 0 6",
        "M16 15.5v-11l5 2",
        "M5 8l-3 4l3 4",
    ],

    # --- Flying ----------------------------------------------------------------------------------
    # Climb and descend: two chevrons, which is what altitude looks like on every flight control
    # ever drawn, and nothing like the arrows that steer.
    "climb": [
        "M5 13l7 -6l7 6",
        "M5 18l7 -6l7 6",
    ],
    "descend": [
        "M5 6l7 6l7 -6",
        "M5 11l7 6l7 -6",
    ],
    # Yaw: the nose swinging round, drawn as the arc-and-corner every rotate control uses. Two
    # bare strokes for the head read as a letter rather than as an arrow, which is what the first
    # attempt produced.
    "yaw_right": [
        "M19.95 11a8 8 0 1 0 -.5 4",
        "M19.45 20v-5h-5",
    ],
    "yaw_left": ("mirror", "yaw_right"),

    # --- Everything else ---------------------------------------------------------------------------
    # The sub-mission button: the game's own vigilante, taxi, paramedic and recruit control.
    "submission": [
        "M6 21v-17l13 3l-13 3",
        "M6 4a1 1 0 1 0 0 -2a1 1 0 0 0 0 2",
    ],
    # Take a weapon off the ground.
    "pickup": [
        "M12 3v9",
        "M8.5 8.5l3.5 3.5l3.5 -3.5",
        "M4 15v3a2 2 0 0 0 2 2h12a2 2 0 0 0 2 -2v-3",
    ],
    # The menu, the pause, and the way past a cutscene.
    "menu": [
        "M4 7h16",
        "M4 12h16",
        "M4 17h16",
    ],
    "pause": [
        ("M8 5h2.5v14h-2.5z", {"fill": True}),
        ("M13.5 5h2.5v14h-2.5z", {"fill": True}),
    ],
    "skip": [
        ("M4 6l8 6l-8 6z", {"fill": True}),
        ("M12 6l8 6l-8 6z", {"fill": True}),
    ],
    # The game's own pages: confirm, go back, drop a marker.
    "select": "M5 12.5l5 5l9 -11",
    "back": [
        "M4 12h15a1 1 0 0 1 0 6h-3",
        "M9 7l-5 5l5 5",
    ],
    "marker": [
        "M12 3a6 6 0 0 1 6 6c0 4 -6 12 -6 12s-6 -8 -6 -12a6 6 0 0 1 6 -6",
        "M12 7a2 2 0 1 0 0 4a2 2 0 0 0 0 -4",
    ],
    "up": "M6 15l6 -6l6 6",
    "down": "M6 9l6 6l6 -6",
    "left": "M15 6l-6 6l6 6",
    "right": "M9 6l6 6l-6 6",
}

# --- The front end's tiles ------------------------------------------------------------------------
#
# The phone's menu is a grid of tiles rather than a list of words, so each destination needs a
# face. Same 24-unit box and the same stroke, drawn a little heavier because they are seen at
# twice the size of a control and with a word underneath rather than instead of one.
#
# Written as a second table only so the two sets can be told apart at the call site - they go
# through the same renderer and land in the same folder under a different prefix.
TILES = {
    # Back to the game.
    "resume": [("M8 5l11 7l-11 7z", {"fill": True})],
    # A fresh start. A plus rather than anything more literal: every other reading of "new game"
    # is a picture of the game, which is what all the other tiles already are.
    "newgame": [
        "M12 3a9 9 0 1 0 0 18a9 9 0 0 0 0 -18",
        "M12 8v8",
        "M8 12h8",
    ],
    # The city.
    "map": [
        "M9 4l-6 2v14l6 -2l6 2l6 -2v-14l-6 2l-6 -2z",
        "M9 4v14",
        "M15 6v14",
    ],
    # Everything the game has been counting.
    "stats": [
        "M4 20v-6",
        "M9.3 20v-11",
        "M14.7 20v-8",
        "M20 20v-14",
    ],
    # What you were last told to do.
    "brief": [
        "M6 3h8l4 4v14a1 1 0 0 1 -1 1h-11a1 1 0 0 1 -1 -1v-17a1 1 0 0 1 1 -1z",
        "M14 3v4h4",
        "M8.5 13h7",
        "M8.5 17h5",
    ],
    # The combinations. A skull, because it is the one glyph that says "this is not how the game
    # is meant to be played" without a word under it - which the tile has anyway.
    "cheats": [
        "M12 3a7 7 0 0 1 7 7v3.5l-1.2 1.6v2.4a1.5 1.5 0 0 1 -1.5 1.5h-8.6a1.5 1.5 0 0 1 -1.5 -1.5"
        "v-2.4l-1.2 -1.6v-3.5a7 7 0 0 1 7 -7z",
        ("M9.2 9.2a1.9 1.9 0 1 0 0 3.8a1.9 1.9 0 0 0 0 -3.8", {"fill": True}),
        ("M14.8 9.2a1.9 1.9 0 1 0 0 3.8a1.9 1.9 0 0 0 0 -3.8", {"fill": True}),
        "M12 14.5v1.5",
        "M9.5 19v2",
        "M12 19v2",
        "M14.5 19v2",
    ],
    # Everything this port added, and everything it kept.
    "options": [
        "M12 8.5a3.5 3.5 0 1 0 0 7a3.5 3.5 0 0 0 0 -7",
        "M19.4 14.6a1.6 1.6 0 0 0 .3 1.8l.1 .1a2 2 0 1 1 -2.8 2.8l-.1 -.1a1.6 1.6 0 0 0 -1.8 -.3"
        "a1.6 1.6 0 0 0 -1 1.5v.2a2 2 0 1 1 -4 0v-.1a1.6 1.6 0 0 0 -1.1 -1.5a1.6 1.6 0 0 0 -1.8 .3"
        "l-.1 .1a2 2 0 1 1 -2.8 -2.8l.1 -.1a1.6 1.6 0 0 0 .3 -1.8a1.6 1.6 0 0 0 -1.5 -1h-.2"
        "a2 2 0 1 1 0 -4h.1a1.6 1.6 0 0 0 1.5 -1.1a1.6 1.6 0 0 0 -.3 -1.8l-.1 -.1a2 2 0 1 1 2.8 -2.8"
        "l.1 .1a1.6 1.6 0 0 0 1.8 .3h.1a1.6 1.6 0 0 0 1 -1.5v-.2a2 2 0 1 1 4 0v.1a1.6 1.6 0 0 0 1 1.5"
        "a1.6 1.6 0 0 0 1.8 -.3l.1 -.1a2 2 0 1 1 2.8 2.8l-.1 .1a1.6 1.6 0 0 0 -.3 1.8v.1a1.6 1.6 0 0 0 1.5 1h.2"
        "a2 2 0 1 1 0 4h-.1a1.6 1.6 0 0 0 -1.5 1z",
    ],
    # Out.
    "quit": [
        "M12 3v9",
        "M6.6 6.6a9 9 0 1 0 10.8 0",
    ],
    # The save list.
    "load": [
        "M5 3h11l3 3v14a1 1 0 0 1 -1 1h-13a1 1 0 0 1 -1 -1v-16a1 1 0 0 1 1 -1z",
        "M8 3v6h8v-6",
        "M8 21v-6h8v6",
    ],
}


# --- A very small SVG path renderer --------------------------------------------------------------

TOKEN = re.compile(r"[MmLlHhVvCcSsQqTtAaZz]|-?\d*\.?\d+(?:[eE][-+]?\d+)?")


def tokenize(data):
    return TOKEN.findall(data)


def flatten_cubic(p0, p1, p2, p3, steps=24):
    out = []
    for i in range(1, steps + 1):
        t = i / steps
        u = 1.0 - t
        x = u * u * u * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t * t * t * p3[0]
        y = u * u * u * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t * t * t * p3[1]
        out.append((x, y))
    return out


def flatten_quad(p0, p1, p2, steps=18):
    out = []
    for i in range(1, steps + 1):
        t = i / steps
        u = 1.0 - t
        x = u * u * p0[0] + 2 * u * t * p1[0] + t * t * p2[0]
        y = u * u * p0[1] + 2 * u * t * p1[1] + t * t * p2[1]
        out.append((x, y))
    return out


def flatten_arc(p0, rx, ry, rotation, large_arc, sweep, p1, steps=48):
    """SVG's endpoint arc, turned into a polyline. The conversion is F.6.5 of the spec."""
    if rx == 0 or ry == 0 or (abs(p0[0] - p1[0]) < 1e-9 and abs(p0[1] - p1[1]) < 1e-9):
        return [p1]
    rx, ry = abs(rx), abs(ry)
    phi = math.radians(rotation)
    cos_p, sin_p = math.cos(phi), math.sin(phi)

    dx2 = (p0[0] - p1[0]) / 2.0
    dy2 = (p0[1] - p1[1]) / 2.0
    x1p = cos_p * dx2 + sin_p * dy2
    y1p = -sin_p * dx2 + cos_p * dy2

    # An arc whose radii are too small for the two endpoints is scaled up until it fits, which
    # is what the spec asks for rather than an error.
    lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry)
    if lam > 1.0:
        scale = math.sqrt(lam)
        rx *= scale
        ry *= scale

    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    factor = math.sqrt(max(0.0, num / den)) if den else 0.0
    if large_arc == sweep:
        factor = -factor
    cxp = factor * rx * y1p / ry
    cyp = -factor * ry * x1p / rx

    cx = cos_p * cxp - sin_p * cyp + (p0[0] + p1[0]) / 2.0
    cy = sin_p * cxp + cos_p * cyp + (p0[1] + p1[1]) / 2.0

    def angle(ux, uy, vx, vy):
        dot = ux * vx + uy * vy
        len_u = math.hypot(ux, uy)
        len_v = math.hypot(vx, vy)
        if len_u == 0 or len_v == 0:
            return 0.0
        a = math.acos(max(-1.0, min(1.0, dot / (len_u * len_v))))
        return -a if ux * vy - uy * vx < 0 else a

    theta = angle(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    delta = angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sweep and delta > 0:
        delta -= 2 * math.pi
    elif sweep and delta < 0:
        delta += 2 * math.pi

    out = []
    n = max(4, int(steps * abs(delta) / (2 * math.pi)) + 2)
    for i in range(1, n + 1):
        t = theta + delta * i / n
        x = cos_p * rx * math.cos(t) - sin_p * ry * math.sin(t) + cx
        y = sin_p * rx * math.cos(t) + cos_p * ry * math.sin(t) + cy
        out.append((x, y))
    return out


def parse_path(data):
    """Path data to a list of (points, closed) subpaths."""
    tokens = tokenize(data)
    i = 0
    subpaths = []
    points = []
    current = (0.0, 0.0)
    start = (0.0, 0.0)
    command = None
    last_control = None

    def number():
        nonlocal i
        value = float(tokens[i])
        i += 1
        return value

    def flush(closed=False):
        nonlocal points
        if len(points) > 1:
            subpaths.append((points, closed))
        points = []

    while i < len(tokens):
        token = tokens[i]
        if token.isalpha():
            command = token
            i += 1
        elif command is None:
            raise ValueError("path data starts with a number: " + data[:32])
        # A repeated coordinate set after M is an implicit L, which several of the icons use.
        elif command == "M":
            command = "L"
        elif command == "m":
            command = "l"

        upper = command.upper()
        relative = command.islower()
        ox, oy = current if relative else (0.0, 0.0)

        if upper == "M":
            flush()
            current = (number() + ox, number() + oy)
            start = current
            points = [current]
            last_control = None
        elif upper == "L":
            current = (number() + ox, number() + oy)
            points.append(current)
            last_control = None
        elif upper == "H":
            current = (number() + ox, current[1])
            points.append(current)
            last_control = None
        elif upper == "V":
            current = (current[0], number() + oy)
            points.append(current)
            last_control = None
        elif upper in ("C", "S"):
            if upper == "C":
                c1 = (number() + ox, number() + oy)
            else:
                c1 = current if last_control is None else (
                    2 * current[0] - last_control[0], 2 * current[1] - last_control[1])
            c2 = (number() + ox, number() + oy)
            end = (number() + ox, number() + oy)
            points.extend(flatten_cubic(current, c1, c2, end))
            current, last_control = end, c2
        elif upper in ("Q", "T"):
            if upper == "Q":
                c1 = (number() + ox, number() + oy)
            else:
                c1 = current if last_control is None else (
                    2 * current[0] - last_control[0], 2 * current[1] - last_control[1])
            end = (number() + ox, number() + oy)
            points.extend(flatten_quad(current, c1, end))
            current, last_control = end, c1
        elif upper == "A":
            rx, ry, rot = number(), number(), number()
            large, sweep = number(), number()
            end = (number() + ox, number() + oy)
            points.extend(flatten_arc(current, rx, ry, rot, large >= 0.5, sweep >= 0.5, end))
            current, last_control = end, None
        elif upper == "Z":
            if points:
                points.append(start)
                flush(closed=True)
            current = start
            points = [current]
            last_control = None
        else:
            raise ValueError("unsupported path command " + command)

    flush()
    return subpaths


def stroke(draw, subpaths, width):
    """Stroke with round caps and joins: a thick line per segment, a disc at every vertex."""
    radius = width / 2.0
    for points, _ in subpaths:
        draw.line(points, fill=255, width=int(round(width)))
        for x, y in points:
            draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=255)


def render(paths, size):
    canvas = size * SUPERSAMPLE
    scale = canvas / VIEWBOX
    mask = Image.new("L", (canvas, canvas), 0)
    draw = ImageDraw.Draw(mask)

    for entry in paths:
        data, options = entry if isinstance(entry, tuple) else (entry, {})
        subpaths = [([(x * scale, y * scale) for x, y in pts], closed)
                    for pts, closed in parse_path(data)]
        if options.get("fill"):
            for pts, _ in subpaths:
                draw.polygon(pts, fill=255)
        else:
            stroke(draw, subpaths, options.get("w", STROKE) * scale)

    mask = mask.resize((size, size), Image.LANCZOS)
    white = Image.new("L", (size, size), 255)
    return Image.merge("RGBA", (white, white, white, mask))


def resolve(name, seen=(), table=None):
    """The paths for an icon, following a ("mirror", other) entry to the one it copies."""
    if name in seen:
        raise ValueError("mirror loop at " + name)
    table = ICONS if table is None else table
    entry = table[name]
    if isinstance(entry, tuple) and len(entry) == 2 and entry[0] == "mirror":
        paths, flip = resolve(entry[1], seen + (name,), table)
        return paths, not flip
    if isinstance(entry, str):
        return [entry], False
    return entry, False


def bake(table, prefix):
    for name in sorted(table):
        paths, flip = resolve(name, table=table)
        image = render(paths, OUT_SIZE)
        if flip:
            image = image.transpose(Image.FLIP_LEFT_RIGHT)
        path = os.path.join(OUT_DIR, "%s_%s.png" % (prefix, name))
        image.save(path)
        print("wrote", os.path.relpath(path, os.path.dirname(HERE)))


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    bake(ICONS, "touch")
    # The tiles are NOT baked. The phone menu they were drawn for was a grid of them and it was
    # reverted to the desktop front end, so shipping the art would be shipping nine images
    # nothing loads. The table above is kept as the record of how they were drawn, exactly as
    # Tools/vcsmenuart.py keeps make_background() after the patterned backdrop came out - one
    # line here brings them back.
    #
    #   bake(TILES, "tile")
    print("%d controls" % len(ICONS))


if __name__ == "__main__":
    main()
