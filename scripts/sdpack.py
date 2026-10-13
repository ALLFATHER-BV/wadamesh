#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Put books and documents on a wadamesh SD card for the on-device Reader.

The Reader opens sd:/home.htm when it starts, follows links between pages on the
card without Wi-Fi, and pages through large .txt / .md files on its own. This
tool fills a card for it:

    python3 scripts/sdpack.py /Volumes/SDCARD book.epub notes.txt manual.md

- An EPUB becomes /library/<name>/ : one small page per chapter (split further when
  a chapter is long, each part linked to the next) plus index.htm, its contents.
- A .txt or .md file is copied to /library/ as it is: the Reader pages through it.
- /home.htm is (re)written to list everything in /library, so the Reader opens on it.

Only the Python standard library. The card must be FAT32 (the firmware does not
read exFAT). Pages stay well under the Reader's limits (it reads up to 48 KB of a
page on the smallest boards and shows up to 280 links).
"""
import html, os, posixpath, re, shutil, sys, zipfile
from html.parser import HTMLParser

PART_MAX = 30 * 1024        # bytes of text per page: under the Reader's smallest buffer
LIB = "library"


def slug(name):
    s = re.sub(r"[^A-Za-z0-9]+", "-", name).strip("-").lower()
    return (s or "book")[:40]


class _Text(HTMLParser):
    """XHTML chapter -> blocks of (kind, text): headings and paragraphs, nothing else."""
    BLOCK = {"p", "div", "li", "br", "tr", "blockquote", "pre"}
    HEAD = {"h1", "h2", "h3", "h4", "h5", "h6"}
    SKIP = {"script", "style", "head", "title"}

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.blocks, self.cur, self.kind, self.skip = [], [], "p", 0

    def _flush(self):
        t = re.sub(r"\s+", " ", "".join(self.cur)).strip()
        if t:
            self.blocks.append((self.kind, t))
        self.cur, self.kind = [], "p"

    def handle_starttag(self, tag, attrs):
        if tag in self.SKIP:
            self.skip += 1
        elif tag in self.HEAD:
            self._flush(); self.kind = "h"
        elif tag in self.BLOCK:
            self._flush()

    def handle_endtag(self, tag):
        if tag in self.SKIP:
            self.skip = max(0, self.skip - 1)
        elif tag in self.HEAD or tag in self.BLOCK:
            self._flush()

    def handle_data(self, data):
        if not self.skip:
            self.cur.append(data)

    def close(self):
        super().close(); self._flush()
        return self.blocks


def _page(title, body, nav):
    return ("<!doctype html><html><head><meta charset=\"utf-8\"><title>%s</title></head><body>\n"
            "<h1>%s</h1>\n%s\n<p>%s</p>\n</body></html>\n") % (
        html.escape(title), html.escape(title), body, " | ".join(nav))


def _opf(z):
    container = z.read("META-INF/container.xml").decode("utf-8", "replace")
    opf_path = re.search(r'full-path="([^"]+)"', container).group(1)
    opf = z.read(opf_path).decode("utf-8", "replace")
    base = posixpath.dirname(opf_path)
    title = re.search(r"<dc:title[^>]*>(.*?)</dc:title>", opf, re.S)
    items = {m.group(1): m.group(2) for m in
             re.finditer(r'<item\b[^>]*?id="([^"]+)"[^>]*?href="([^"]+)"', opf)}
    items.update({m.group(2): m.group(1) for m in
                  re.finditer(r'<item\b[^>]*?href="([^"]+)"[^>]*?id="([^"]+)"', opf)})
    spine = re.findall(r'<itemref\b[^>]*?idref="([^"]+)"', opf)
    files = [posixpath.normpath(posixpath.join(base, items[i])) for i in spine if i in items]
    return (html.unescape(title.group(1).strip()) if title else None), files


def pack_epub(src, card):
    z = zipfile.ZipFile(src)
    title, files = _opf(z)
    title = title or os.path.splitext(os.path.basename(src))[0]
    d = os.path.join(card, LIB, slug(title))
    os.makedirs(d, exist_ok=True)
    pages = []          # (file name, page title)
    for idx, f in enumerate(files):
        try:
            raw = z.read(f).decode("utf-8", "replace")
        except KeyError:
            continue
        p = _Text(); p.feed(raw); blocks = p.close()
        if not blocks:
            continue
        head = next((t for k, t in blocks if k == "h"), "Part %d" % (idx + 1))
        part, size, n = [], 0, 0
        for k, t in blocks + [("end", "")]:
            if k != "end":
                line = ("<h2>%s</h2>" if k == "h" else "<p>%s</p>") % html.escape(t)
                part.append(line); size += len(line.encode("utf-8"))
            if (size >= PART_MAX or k == "end") and part:
                n += 1
                pages.append(("c%03d-%d.htm" % (idx, n), head if n == 1 else "%s (%d)" % (head, n), part))
                part, size = [], 0
    for i, (fn, ptitle, part) in enumerate(pages):
        nav = ['<a href="index.htm">Contents</a>']
        if i > 0:
            nav.insert(0, '<a href="%s">Previous</a>' % pages[i - 1][0])
        if i + 1 < len(pages):
            nav.append('<a href="%s">Next</a>' % pages[i + 1][0])
        open(os.path.join(d, fn), "w", encoding="utf-8").write(_page(ptitle, "\n".join(part), nav))
    toc = "\n".join('<p><a href="%s">%s</a></p>' % (fn, html.escape(t)) for fn, t, _ in pages)
    open(os.path.join(d, "index.htm"), "w", encoding="utf-8").write(
        _page(title, toc, ['<a href="/home.htm">Library</a>']))
    return title, "/%s/%s/index.htm" % (LIB, slug(title)), len(pages)


def pack_text(src, card):
    d = os.path.join(card, LIB)
    os.makedirs(d, exist_ok=True)
    name = os.path.basename(src)
    shutil.copyfile(src, os.path.join(d, name))
    return os.path.splitext(name)[0], "/%s/%s" % (LIB, name), 1


def write_home(card):
    """List every book (folder with index.htm) and text file under /library."""
    d = os.path.join(card, LIB)
    rows = []
    for e in sorted(os.listdir(d)) if os.path.isdir(d) else []:
        p = os.path.join(d, e)
        if os.path.isdir(p) and os.path.exists(os.path.join(p, "index.htm")):
            t = re.search(r"<title>(.*?)</title>", open(os.path.join(p, "index.htm"), encoding="utf-8").read())
            rows.append((html.unescape(t.group(1)) if t else e, "/%s/%s/index.htm" % (LIB, e)))
        elif e.lower().endswith((".txt", ".md")):
            rows.append((os.path.splitext(e)[0], "/%s/%s" % (LIB, e)))
    body = "\n".join('<p><a href="%s">%s</a></p>' % (u, html.escape(t)) for t, u in rows) or "<p>Nothing here yet.</p>"
    open(os.path.join(card, "home.htm"), "w", encoding="utf-8").write(_page("Library", body, []))
    return len(rows)


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    card, srcs = argv[1], argv[2:]
    if not os.path.isdir(card):
        sys.exit("not a directory: " + card)
    for s in srcs:
        low = s.lower()
        if low.endswith(".epub"):
            t, url, n = pack_epub(s, card)
            print("%s: %d pages -> %s" % (t, n, url))
        elif low.endswith((".txt", ".md")):
            t, url, _ = pack_text(s, card)
            print("%s -> %s" % (t, url))
        else:
            print("skipped (not .epub/.txt/.md): " + s)
    print("home.htm lists %d item(s)" % write_home(card))


if __name__ == "__main__":
    main(sys.argv)
