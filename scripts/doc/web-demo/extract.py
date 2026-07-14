#!/usr/bin/env python3
"""
Extract the wadamesh web control-panel SPA from the firmware C source.

The page is the C string literal `WS_HTML_TERMINAL_PAGE[]` in
    src/helpers/esp32/WebSocketCompanionServer.cpp
built from many adjacent "..."  fragments. This re-joins the fragments,
un-escapes the C escapes, strips the leading HTTP/1.1 response headers, and
writes a standalone scripts/doc/web-demo/index.html.

Re-run whenever the page changes:
    python3 scripts/doc/web-demo/extract.py
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
SRC = os.path.join(REPO, "src", "helpers", "esp32", "WebSocketCompanionServer.cpp")
OUT = os.path.join(HERE, "index.html")

DECL = "static const char WS_HTML_TERMINAL_PAGE[] ="

# C escape sequences that appear in the literal. Order matters: handle the
# backslash-escapes we know about; leave any others (\uXXXX etc. don't occur here)
# untouched. We decode via a single regex pass so a literal "\\n" (escaped
# backslash then n) is NOT turned into a newline.
_C_ESCAPES = {
    "\\n": "\n",
    "\\r": "\r",
    "\\t": "\t",
    '\\"': '"',
    "\\\\": "\\",
    "\\'": "'",
    "\\a": "\a",
    "\\b": "\b",
    "\\f": "\f",
    "\\v": "\v",
    "\\0": "\0",
}
_ESC_RE = re.compile(r"\\(?:[abfnrtv0'\"\\]|x[0-9A-Fa-f]{2})")


def _unescape(s):
    def repl(m):
        tok = m.group(0)
        if tok.startswith("\\x"):
            return chr(int(tok[2:], 16))
        return _C_ESCAPES.get(tok, tok)
    return _ESC_RE.sub(repl, s)


def _extract_string_fragments(block):
    """Yield the decoded content of every "..." fragment in `block`, in order.

    Parses char-by-char so escaped quotes (\") inside a fragment don't end it,
    and text between fragments (whitespace / newlines in the C source) is ignored.
    """
    out = []
    i, n = 0, len(block)
    in_str = False
    frag = []
    while i < n:
        ch = block[i]
        if not in_str:
            if ch == '"':
                in_str = True
                frag = []
            elif ch == ";":
                break  # end of the C statement
            i += 1
            continue
        # inside a string literal
        if ch == "\\" and i + 1 < n:
            frag.append(block[i:i + 2])
            i += 2
            continue
        if ch == '"':
            out.append(_unescape("".join(frag)))
            in_str = False
            i += 1
            continue
        frag.append(ch)
        i += 1
    return "".join(out)


def extract(src_path=SRC):
    with open(src_path, "r", encoding="utf-8") as f:
        text = f.read()
    start = text.find(DECL)
    if start < 0:
        raise SystemExit("could not find %s in %s" % (DECL, src_path))
    start += len(DECL)
    # The statement ends at the first ';' that terminates the literal. Because
    # fragments can contain ';' we cannot naively split; instead let the fragment
    # parser stop at the first ';' found OUTSIDE a string.
    joined = _extract_string_fragments(text[start:])
    # Strip the HTTP response head: everything up to and including the blank line
    # (\r\n\r\n) that precedes the HTML body.
    marker = "\r\n\r\n"
    idx = joined.find(marker)
    if idx >= 0 and joined[:idx].startswith("HTTP/"):
        joined = joined[idx + len(marker):]
    else:
        # fall back: chop to the doctype if the header shape ever changes
        d = joined.find("<!DOCTYPE")
        if d > 0:
            joined = joined[d:]
    return joined


def main():
    html = extract()
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(html)
    print("wrote %s (%d bytes)" % (OUT, len(html)))
    # sanity: the reconstructed page must have the key anchors
    for needle in ("<!DOCTYPE html>", "WADAMESH", "/term", "id=w", "showTab"):
        if needle not in html:
            print("  WARNING: expected %r not found in output" % needle, file=sys.stderr)


if __name__ == "__main__":
    main()
