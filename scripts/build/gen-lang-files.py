#!/usr/bin/env python3
"""Export the built-in i18n table as downloadable .lang files.

Parses the kI18n[] rows in src/ui-touch/i18n.cpp (the single source of truth
translators PR against) and writes, for every non-English column:

    out/firmware/apps/lang/<code>.lang     one "EN-key<TAB>translation" per line
    out/firmware/apps/langs.json           the Languages-tab catalog

File format (read by uiLangFileBootLoad in UITask.cpp):
    # wadamesh language file
    # code: nl
    # name: Nederlands
    # base: nl          <- built-in fallback column for keys the file lacks
    Save<TAB>Opslaan
Escapes: \\n \\t \\\\ in both key and translation. '#' lines are headers.

Run from the repo root:  python3 scripts/build/gen-lang-files.py
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "src/ui-touch/i18n.cpp")
OUT = os.path.join(ROOT, "out/firmware/apps")

# Keep in sync with the UiLang enum + kUiLangCodes/kUiLangNames in i18n.cpp.
CODES = ["en", "hu", "nl", "de", "fr", "es", "it",
         "ru", "uk", "bg", "sr", "el", "pt-br", "ro"]
NAMES = ["English", "Magyar", "Nederlands", "Deutsch", "Français", "Español",
         "Italiano", "Русский", "Українська", "Български", "Српски",
         "Ελληνικά", "Português (BR)", "Română"]


def parse_c_strings(blob):
    """Yield every C string literal / bare 0 token at brace depth as a flat list
    of cells for one kI18n row region. Handles \\" \\\\ \\n \\t \\xHH escapes."""
    cells, i, n = [], 0, len(blob)
    while i < n:
        c = blob[i]
        if c == '"':
            out, i = [], i + 1
            while i < n:
                ch = blob[i]
                if ch == '\\':
                    nxt = blob[i + 1]
                    if nxt == 'n':
                        out.append('\n'); i += 2
                    elif nxt == 't':
                        out.append('\t'); i += 2
                    elif nxt == 'x':
                        out.append(chr(int(blob[i + 2:i + 4], 16))); i += 4
                    else:
                        out.append(nxt); i += 2
                elif ch == '"':
                    i += 1
                    break
                else:
                    out.append(ch); i += 1
            # C adjacent-literal concatenation: peek for another opening quote
            j = i
            while j < n and blob[j] in ' \t\r\n':
                j += 1
            if j < n and blob[j] == '"':
                i = j  # loop continues into the next literal, appending later
                # mark a pending continuation by pushing placeholder
                cells.append(('str', ''.join(out), True))
                continue
            cells.append(('str', ''.join(out), False))
        elif blob.startswith('0', i) and (i + 1 >= n or not blob[i + 1].isalnum()):
            cells.append(('null', None, False))
            i += 1
        else:
            i += 1
    # merge adjacent-literal continuations
    merged, acc = [], None
    for kind, val, cont in cells:
        if kind == 'str':
            acc = (acc or '') + val
            if not cont:
                merged.append(acc)
                acc = None
        else:
            merged.append(None)
    return merged


def esc(s):
    return s.replace('\\', '\\\\').replace('\t', '\\t').replace('\n', '\\n')


def main():
    src = open(SRC, encoding='utf-8').read()
    start = src.index('static const I18nEntry kI18n[] = {')
    end = src.index('\n};', start)
    body = src[start:end]

    rows = []
    # each row: { "key", { 0, "c1", ... } },  — split on the row-opening brace
    for m in re.finditer(r'\{\s*"', body):
        i = m.start()
        # find the row's closing "} }," by brace counting
        depth, j = 0, i
        while j < len(body):
            if body[j] == '{':
                depth += 1
            elif body[j] == '}':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        row = body[i:j + 1]
        cells = parse_c_strings(row)
        if len(cells) >= 2 and cells[0] is not None:
            rows.append(cells)
        # skip the scanner past this row so nested braces don't re-match
    # de-dup rows found by overlapping regex matches (inner { "str" of tr[])
    seen, uniq = set(), []
    for cells in rows:
        key = cells[0]
        if key in seen:
            continue
        seen.add(key)
        uniq.append(cells)
    rows = uniq
    print(f"parsed {len(rows)} table rows")
    if len(rows) < 500:
        sys.exit("row count implausibly low - parser drift, aborting")

    lang_dir = os.path.join(OUT, "lang")
    os.makedirs(lang_dir, exist_ok=True)
    catalog = []
    for li in range(1, len(CODES)):
        code, name = CODES[li], NAMES[li]
        lines = [
            "# wadamesh language file",
            f"# code: {code}",
            f"# name: {name}",
            f"# base: {code}",
        ]
        n = 0
        for cells in rows:
            key = cells[0]
            tr = cells[1 + li] if 1 + li < len(cells) else None
            if tr:  # null cell -> built-in fallback handles it
                lines.append(f"{esc(key)}\t{esc(tr)}")
                n += 1
        path = os.path.join(lang_dir, f"{code}.lang")
        open(path, "w", encoding="utf-8").write("\n".join(lines) + "\n")
        catalog.append((code, name))
        print(f"  {code}.lang  {n} rows  {os.path.getsize(path)} bytes")

    entries = ",\n  ".join(
        f'{{"code":"{c}","name":"{n}"}}' for c, n in catalog)
    open(os.path.join(OUT, "langs.json"), "w", encoding="utf-8").write(
        '{"langs":[\n  ' + entries + '\n]}\n')
    print(f"langs.json  {len(catalog)} languages")


if __name__ == "__main__":
    main()
