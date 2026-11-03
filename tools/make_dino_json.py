"""Draws Chrome's offline T-rex as a companion in the same JSON format as the BookOrbit pet sprites (64x64, palette + six poses)
and writes tools/dino.json, which make_pets.py picks up. It is drawn on a 32x32 grid and doubled."""
import json, os

G = 32
PALETTE = {"#": "535353", "o": "ffffff"}  # grey body, white features

def blank():
    return [["."] * G for _ in range(G)]

def rect(g, x0, y0, x1, y1, ch="#"):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if 0 <= x < G and 0 <= y < G: g[y][x] = ch

def dino(left_up=False):
    g = blank()
    rect(g, 14, 3, 29, 11)               # head
    rect(g, 14, 12, 21, 16)              # neck
    rect(g, 5, 15, 22, 25)               # body
    rect(g, 1, 16, 4, 19); rect(g, 0, 15, 1, 16)   # tail
    rect(g, 21, 17, 24, 18); rect(g, 24, 18, 25, 19)  # arm
    rect(g, 8, 26, 11, 29 if not left_up else 27); rect(g, 8, 30 if not left_up else 28, 13, 31 if not left_up else 29)  # left leg
    rect(g, 15, 26, 18, 29); rect(g, 15, 30, 20, 31)  # right leg
    return g

def eye_open(g):
    rect(g, 22, 5, 23, 6, "o"); rect(g, 23, 6, 23, 6)       # a white eye with a pupil

def eye_closed(g):
    rect(g, 21, 6, 24, 6, "o")

def mouth_closed(g):
    rect(g, 25, 9, 29, 9, "o")

def sprite(pose):
    if pose == "wait":
        g = dino(); eye_open(g); mouth_closed(g)
    elif pose == "sleep":
        g = dino(); eye_closed(g); mouth_closed(g)
        for (x, y) in ((26, 0), (29, 0)):    # Zzz
            rect(g, x, y, x + 2, y); rect(g, x, y + 2, x + 2, y + 2); rect(g, x + 1, y + 1, x + 1, y + 1)
    elif pose == "ribbon":
        g = dino(); eye_open(g); mouth_closed(g)
        rect(g, 15, 12, 18, 15, "o"); rect(g, 20, 12, 23, 15, "o"); rect(g, 19, 13, 19, 14, "o")  # a white bow at the neck
    elif pose == "party":
        g = dino(left_up=True); eye_open(g); mouth_closed(g)
        rect(g, 20, 2, 25, 2); rect(g, 21, 1, 24, 1); rect(g, 22, 0, 23, 0)  # party hat on the head
        for (x, y) in ((4, 4), (10, 8), (28, 14), (30, 22), (2, 10)):  # confetti
            rect(g, x, y, x + 1, y + 1)
    elif pose == "yawn":
        g = dino(); eye_closed(g)
        rect(g, 22, 8, 29, 10, "o")           # open mouth
    elif pose == "read":
        g = dino(); eye_open(g); mouth_closed(g)
        rect(g, 12, 17, 25, 25, "o"); rect(g, 18, 17, 19, 25)   # a book held in front: pages and spine
        rect(g, 12, 17, 25, 17); rect(g, 12, 25, 25, 25); rect(g, 12, 17, 12, 25); rect(g, 25, 17, 25, 25)
    # double it to 64x64
    rows = []
    for r in g:
        line = "".join(c * 2 for c in r)
        rows += [line, line]
    return rows

out = {"palette": PALETTE, "poses": {p: sprite(p) for p in ("wait", "sleep", "ribbon", "party", "yawn", "read")}}
json.dump(out, open(os.path.join(os.path.dirname(__file__), "dino.json"), "w"))
