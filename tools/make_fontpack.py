"""Packs fonts/*.ttf into build/fontpack.bin: 'FPK1', u32 count, then per font u32 style (1 bold, 2 italic), u32 offset, u32 length."""
import os, struct
root = os.path.join(os.path.dirname(__file__), "..")
faces = [("NotoSerif-Regular.ttf", 0), ("NotoSerif-Bold.ttf", 1), ("NotoSerif-Italic.ttf", 2), ("NotoSerif-BoldItalic.ttf", 3)]
blobs = [open(os.path.join(root, "fonts", f), "rb").read() for f, _ in faces]
offset = 8 + 12 * len(faces)
header = b"FPK1" + struct.pack("<I", len(faces))
body = b""
for (name, style), blob in zip(faces, blobs):
    offset = (offset + 3) & ~3
    body += b"\0" * (offset - 8 - 12 * len(faces) - len(body))
    header += struct.pack("<III", style, offset, len(blob))
    body += blob
    offset += len(blob)
out = os.path.join(root, "build", "fontpack.bin")
os.makedirs(os.path.dirname(out), exist_ok=True)
open(out, "wb").write(header + body)
print(out, len(header + body), "bytes")
