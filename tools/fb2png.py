"""Turns a 'FB-BEGIN ... FB-END' hex dump from the serial log into a portrait PNG (needs Pillow)."""
import sys
from PIL import Image
lines = open(sys.argv[1], errors="replace").read().split("FB-BEGIN")[-1].split("FB-END")[0].split()
raw = bytes.fromhex("".join(lines))
pw, ph = 800, 480
img = Image.new("1", (pw, ph), 1)
px = img.load()
for y in range(ph):
    for x in range(pw):
        px[x, y] = (raw[y * (pw // 8) + x // 8] >> (7 - x % 8)) & 1
# portrait = panel rotated 90 degrees clockwise: logical (x, y) -> panel (y, ph-1-x); undo with a 90 degree turn
img.transpose(Image.Transpose.ROTATE_270).save(sys.argv[2])
