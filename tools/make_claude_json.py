"""Draws the Claude companion (a blocky orange creature with arms and four legs) in the same JSON format as the BookOrbit pet
sprites (64x64, palette + six poses) and writes tools/claude.json, which make_pets.py picks up."""
import json, os

W = 64
C = 4  # one drawn "pixel" is 4x4 sprite pixels
BODY, EYE, PAPER, INK = "B", "0", "W", "K"  # orange body, white eyes/mouth, book pages, dark detail
PALETTE = {"B": "b5532f", "0": "ffffff", "W": "fff6e6", "K": "3a2418", "Y": "f5c623"}

def blank():
    return [["."] * W for _ in range(W)]

def cell(g, c0, r0, c1, r1, ch):
    """Fills cells c0..c1, r0..r1 (inclusive, in 4 px cells)."""
    for y in range(r0 * C, (r1 + 1) * C):
        for x in range(c0 * C, (c1 + 1) * C):
            if 0 <= x < W and 0 <= y < W: g[y][x] = ch

def px(g, x0, y0, x1, y1, ch):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if 0 <= x < W and 0 <= y < W: g[y][x] = ch

def base(arms_up=False):
    g = blank()
    cell(g, 3, 8, 12, 13, BODY)            # body
    ay = 5 if arms_up else 10               # arms: raised or at the sides
    cell(g, 1, ay, 2, ay + 1, BODY)
    cell(g, 13, ay, 14, ay + 1, BODY)
    if arms_up:                             # connect the raised arms to the body
        cell(g, 2, ay + 2, 2, 9, BODY)
        cell(g, 13, ay + 2, 13, 9, BODY)
    for c in (4, 6, 9, 11):                 # legs
        cell(g, c, 14, c, 15, BODY)
    return g

def eyes_open(g, dy=0):
    if dy == 0:
        cell(g, 5, 9, 5, 10, EYE)
        cell(g, 10, 9, 10, 10, EYE)
    else:  # looking down
        px(g, 20, 40, 23, 47, EYE)
        px(g, 40, 40, 43, 47, EYE)

def eyes_closed(g):
    px(g, 19, 40, 24, 42, EYE)
    px(g, 39, 40, 44, 42, EYE)

def eyes_happy(g):  # little arches: ^ ^
    for x0 in (19, 39):
        px(g, x0 + 1, 36, x0 + 4, 37, EYE)
        px(g, x0, 38, x0 + 2, 40, EYE)
        px(g, x0 + 3, 38, x0 + 5, 40, EYE)

def sprite(pose):
    if pose == "wait":
        g = base(); eyes_open(g)
    elif pose == "sleep":
        g = base(); eyes_closed(g)
        for (x, y, s) in ((46, 6, 5), (53, 1, 4)):  # floating Z letters
            px(g, x, y, x + s, y + 1, INK); px(g, x, y + s - 1, x + s, y + s, INK)
            for k in range(s): px(g, x + s - k - 1, y + k, x + s - k, y + k, INK)
    elif pose == "ribbon":
        g = base(); eyes_open(g)
        px(g, 20, 22, 29, 31, INK); px(g, 34, 22, 43, 31, INK)   # a bow on top
        px(g, 30, 25, 33, 31, INK)
        px(g, 23, 25, 27, 28, EYE); px(g, 36, 25, 40, 28, EYE)
    elif pose == "party":
        g = base(arms_up=True); eyes_happy(g)
        for (x, y) in ((6, 6), (56, 8), (10, 20), (52, 22), (30, 2)):  # confetti
            px(g, x, y, x + 2, y + 2, INK)
    elif pose == "yawn":
        g = base(); eyes_closed(g)
        cell(g, 7, 11, 8, 12, EYE)          # open mouth
        px(g, 28, 44, 35, 51, EYE)
    elif pose == "read":
        g = base(); eyes_open(g, dy=1)
        px(g, 16, 46, 47, 59, PAPER); px(g, 31, 46, 32, 59, INK)  # a book held in front
        px(g, 16, 46, 47, 46, INK); px(g, 16, 59, 47, 59, INK)
        px(g, 16, 46, 16, 59, INK); px(g, 47, 46, 47, 59, INK)
    return ["".join(r) for r in g]

out = {"palette": PALETTE, "poses": {p: sprite(p) for p in ("wait", "sleep", "ribbon", "party", "yawn", "read")}}
json.dump(out, open(os.path.join(os.path.dirname(__file__), "claude.json"), "w"))
