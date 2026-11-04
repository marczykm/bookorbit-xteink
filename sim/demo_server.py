"""A small stand-in for the BookOrbit server, for the simulator (sim/README.md): login demo/demo, a library of a few books
(three of them being read), the dashboard widgets, covers, EPUB downloads, reading progress and sessions.
Only what src/bookorbit.cpp calls. State lives in memory and starts afresh with each run.

    python3 sim/demo_server.py [--port 3000]
"""
import argparse
import datetime
import io
import json
import re
import secrets
import struct
import time
import zipfile
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

# title, author, series (name, index) or None, reading progress or None
TITLES = [
    ("The Lighthouse Keeper's Daughter", "Maren Holt", ("Northern Lights", 1), 47.0),
    ("Salt and Cedar", "Jonas Ek", None, 12.0),
    ("A Brief History of Small Rivers", "Ada Lindqvist", None, 80.0),
    ("Winter Harbour", "Maren Holt", ("Northern Lights", 2), None),
    ("The Clockmaker", "Jonas Ek", None, None),
    ("Paper Birds", "Ada Lindqvist", None, None),
    ("Quiet Fields", "Teodor Nowak", None, None),
    ("The Last Ferry", "Teodor Nowak", None, None),
]
BOOKS = {}
SERIES = {}
for i, (title, author, series, pct) in enumerate(TITLES, 1):
    if series:
        SERIES.setdefault(series[0], []).append(i)
    BOOKS[i] = dict(id=i, title=title, authors=[author], seriesName=series[0] if series else None,
                    seriesIndex=series[1] if series else None, files=[dict(id=100 + i, format="epub", role="primary")],
                    readingProgress=pct, hasCover=True, addedAt=f"2026-0{9 - i % 9}-01T00:00:00Z")
# When each book was last read: orders the continue-reading shelf (most recent first).
LAST_READ = {1: time.time() - 3600, 2: time.time() - 86400, 3: time.time() - 3 * 86400}
AUTHORS = sorted({b["authors"][0] for b in BOOKS.values()})
SCOPES = {1: ("Unfinished", [1, 2, 3]), 2: ("By Maren Holt", [1, 4]), 3: ("Empty scope", [])}
ACCESS, REFRESH = set(), set()


def make_epub(book):
    """An EPUB of three long chapters with the book's title, so the reader has pages to turn."""
    words = ("The tide came in slowly over the flat grey sand, and the gulls followed it, calling to each other in the "
             "wind. Nobody on the island remembered a spring so patient. ")
    chapters = []
    for n in range(1, 4):
        paras = "".join(f"<p>{words * 3}({book['title']}, chapter {n}, paragraph {i}.)</p>" for i in range(1, 40))
        chapters.append(f'<?xml version="1.0" encoding="utf-8"?><html xmlns="http://www.w3.org/1999/xhtml"><head>'
                        f"<title>Chapter {n}</title></head><body><h1>Chapter {n}</h1>{paras}</body></html>")
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", '<?xml version="1.0"?><container version="1.0" '
                   'xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile '
                   'full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>')
        items = "".join(f'<item id="c{n}" href="c{n}.xhtml" media-type="application/xhtml+xml"/>' for n in range(1, 4))
        spine = "".join(f'<itemref idref="c{n}"/>' for n in range(1, 4))
        z.writestr("OEBPS/content.opf", f'<?xml version="1.0" encoding="utf-8"?><package xmlns="http://www.idpf.org/2007/opf" '
                   f'version="3.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
                   f'<dc:identifier id="id">demo-{book["id"]}</dc:identifier><dc:title>{book["title"]}</dc:title>'
                   f'<dc:creator>{book["authors"][0]}</dc:creator><dc:language>en</dc:language>'
                   f'<meta property="dcterms:modified">2026-01-01T00:00:00Z</meta></metadata><manifest>'
                   f'<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>{items}'
                   f"</manifest><spine>{spine}</spine></package>")
        toc = "".join(f'<li><a href="c{n}.xhtml">Chapter {n}</a></li>' for n in range(1, 4))
        z.writestr("OEBPS/nav.xhtml", '<?xml version="1.0" encoding="utf-8"?><html xmlns="http://www.w3.org/1999/xhtml" '
                   'xmlns:epub="http://www.idpf.org/2007/ops"><head><title>Contents</title></head><body>'
                   f'<nav epub:type="toc"><ol>{toc}</ol></nav></body></html>')
        for n, html in enumerate(chapters, 1):
            z.writestr(f"OEBPS/c{n}.xhtml", html)
    return buf.getvalue()


def cover(i, w=240, h=360):
    """A PNG cover: a light-to-dark gradient with stripes, different for each book."""
    shade, step = 40 + (i * 37) % 160, 8 + i * 3
    rows = []
    for y in range(h):
        v = min(255, shade + y * 90 // h)
        stripe = (y // step) % 2 == 0 and h // 3 < y < 2 * h // 3
        rows.append(b"\x00" + bytes([255 - v if stripe else v] * 3) * w)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b""))


EPUBS = {100 + i: make_epub(b) for i, b in BOOKS.items()}


def paged(items, page, size):
    return dict(items=items[page * size:(page + 1) * size], total=len(items), page=page, size=size)


def continue_reading():
    ids = sorted((i for i, b in BOOKS.items() if b["readingProgress"] is not None and b["readingProgress"] < 100),
                 key=lambda i: -LAST_READ.get(i, 0))
    return [BOOKS[i] for i in ids]


def widgets():
    today = datetime.date.today()
    days = [600, 0, 1800, 2400, 900, 1200, 1500]
    return {
        "reading-streak": {"currentStreak": 5, "longestStreak": 12, "lastSevenDays": [True, True, False, True, True, True, True]},
        "reading-goal": {"goalBooks": 24, "completedBooks": 9, "year": today.year},
        "reading-rhythm": {"days": [{"date": str(today - datetime.timedelta(days=6 - k)), "readingSeconds": s} for k, s in enumerate(days)],
                           "consistencyPercent": 86, "avgSecondsPerDay": 1200},
        "year-projection": {"projectedBooks": 18, "booksCompletedYtd": 9, "trend": "up"},
        "monthly-challenge": {"title": "Read 3 short books", "progress": 1, "target": 3, "completed": False},
        "library-overview": {"totalBooks": len(BOOKS), "totalAuthors": len(AUTHORS), "totalSeries": len(SERIES),
                             "booksAddedThisYear": len(BOOKS)},
    }


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, body=None, ctype="application/json"):
        data = b"" if body is None else body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def tokens(self):
        access, refresh = "acc-" + secrets.token_hex(8), secrets.token_hex(32)
        ACCESS.add(access)
        REFRESH.add(refresh)
        return dict(accessToken=access, accessTokenExpiresAt="2030-01-01T00:00:00Z", refreshToken=refresh,
                    refreshTokenExpiresAt="2030-01-01T00:00:00Z", sessionId=1)

    def handle_any(self, method):
        url = urlparse(self.path)
        qs = parse_qs(url.query)
        page, size = int(qs.get("page", ["0"])[0]), int(qs.get("size", ["50"])[0])
        n = int(self.headers.get("Content-Length") or 0)
        try:
            body = json.loads(self.rfile.read(n)) if n else None
        except ValueError:
            body = None
        print(f"[demo] {method} {self.path}", flush=True)
        if not url.path.startswith("/api/v1/"):
            return self.send(404, {"message": "not found"})
        p = url.path[len("/api/v1"):]

        if p == "/auth/login" and method == "POST":
            if (body or {}).get("username") != "demo" or body.get("password") != "demo":
                return self.send(401, {"message": "Invalid username or password"})
            return self.send(201, self.tokens())
        if p == "/auth/refresh" and method == "POST":
            token = (body or {}).get("refreshToken")
            if token not in REFRESH:
                return self.send(401, {"message": "Invalid refresh token"})
            REFRESH.discard(token)
            return self.send(200, self.tokens())
        if (self.headers.get("Authorization") or "").removeprefix("Bearer ") not in ACCESS:
            return self.send(401, {"message": "Unauthorized"})

        if p == "/books/query" and method == "POST":
            pg = (body or {}).get("pagination") or {}
            books = sorted(BOOKS.values(), key=lambda b: b["addedAt"], reverse=True)
            return self.send(200, paged(books, pg.get("page", 0), pg.get("size", 50)))
        if p == "/authors":
            items = [dict(id=k, name=a, bookCount=sum(b["authors"][0] == a for b in BOOKS.values())) for k, a in enumerate(AUTHORS, 1)]
            return self.send(200, paged(items, page, size))
        if m := re.fullmatch(r"/authors/(\d+)/books", p):
            name = AUTHORS[int(m[1]) - 1]
            return self.send(200, paged([b for b in BOOKS.values() if b["authors"][0] == name], page, size))
        if p == "/series":
            items = [dict(id=k, name=s, bookCount=len(ids)) for k, (s, ids) in enumerate(SERIES.items(), 1)]
            return self.send(200, paged(items, page, size))
        if m := re.fullmatch(r"/series/(\d+)/books", p):
            ids = list(SERIES.values())[int(m[1]) - 1]
            return self.send(200, paged([BOOKS[i] for i in ids], page, size))
        if p == "/smart-scopes":
            return self.send(200, [dict(id=k, name=name, bookCount=len(ids)) for k, (name, ids) in SCOPES.items()])
        if m := re.fullmatch(r"/smart-scopes/(\d+)/books", p):
            return self.send(200, paged([BOOKS[i] for i in SCOPES[int(m[1])][1]], page, size))

        if p == "/dashboard/scrollers/batch" and method == "POST":
            out = []
            for item in (body or {}).get("items", []):
                books = continue_reading() if item.get("type") == "continue-reading" else []
                out.append({"id": item.get("id"), "books": books[:item.get("limit", 20)], "failed": False})
            return self.send(200, {"items": out})
        if p == "/dashboard/widgets/batch" and method == "POST":
            data = widgets()
            return self.send(200, {"items": [{"type": t, "data": data.get(t), "failed": t not in data} for t in body["widgets"]]})

        if m := re.fullmatch(r"/books/(\d+)/thumbnail", p):
            return self.send(200, cover(int(m[1])), "image/png") if int(m[1]) in BOOKS else self.send(404, {"message": "no cover"})
        if m := re.fullmatch(r"/books/files/(\d+)/serve", p):
            epub = EPUBS.get(int(m[1]))
            return self.send(200, epub, "application/epub+zip") if epub else self.send(404, {"message": "no file"})
        if m := re.fullmatch(r"/books/files/(\d+)/progress", p):
            book = BOOKS.get(int(m[1]) - 100)
            if not book:
                return self.send(404, {"message": "no file"})
            if method == "POST":
                book["readingProgress"] = float(body["percentage"])
                LAST_READ[book["id"]] = time.time()
                return self.send(201)
            return self.send(200, {"percentage": book["readingProgress"] or 0, "cfi": None})
        if re.fullmatch(r"/books/files/(\d+)/sessions", p) and method == "POST":
            return self.send(204)
        return self.send(404, {"message": "not handled: " + p})

    def do_GET(self):
        self.handle_any("GET")

    def do_POST(self):
        self.handle_any("POST")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=3000)
    args = ap.parse_args()
    print(f"[demo] BookOrbit demo server on http://127.0.0.1:{args.port} (demo/demo)", flush=True)
    ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()
