#!/usr/bin/env python3
"""
wadamesh beta report service.

Why this exists
---------------
A beta is promoted to stable on a hunch today: it has been out for a while,
nobody shouted, so it ships. There are 13 board images and most of them are in
hands we never hear from, so "nobody shouted" mostly means "nobody with that
board was listening". This service turns that into a checklist: testers report
from the device, per board, per tag, and the promote decision reads off a grid.

Why it is a service at all, rather than the device talking to GitHub
--------------------------------------------------------------------
Two reasons, both hard:

  * The device cannot do TLS. mbedTLS wants ~30 KB of internal heap for a
    handshake and about 5 KB survives Wi-Fi association on the 2 MB boards,
    which is the same reason map tiles come through our own proxy over plain
    HTTP. So the device can only reach a host we control, unencrypted.
  * A GitHub token in a GPL firmware image is a token everybody has. The
    write credential has to live somewhere the public cannot read, which is
    here.

So: device POSTs plain HTTP to firmware.wadamesh.com/report/..., this service
holds the token, and GitHub sees a well-formed matrix instead of raw traffic.

SQLite is the database; GitHub is a view
----------------------------------------
Reports land in SQLite and stay there. The per-tag tracking issue is rendered
from SQLite and rewritten in place, so it can be regenerated at any time and a
GitHub outage loses nothing. Do not read state back out of the issue body.

Bug reports are deliberately NOT here. Those go straight to GitHub as a
prefilled issue form, opened from a QR the device draws, and are submitted by
the reporter's own account: correct attribution, GitHub's own spam controls,
and no token needed. This service only handles the structured test reports and
the opt-in install ping, which have to be one tap on the device to be worth
anything.

Endpoints
---------
    POST /report/test       a tester's verdict for one board on one tag
    POST /report/ping       opt-in "this device is running <tag>" (denominators)
    GET  /report/matrix.json[?tag=beta_84]   the grid, for the site and the device
    GET  /report/health

Run
---
    pip install flask requests
    python3 report-service.py            # listens on 127.0.0.1:5006

Environment
-----------
    WADA_REPORTS_DB    sqlite path         (default /opt/wadamesh-reports/reports.db)
    WADA_FW_ROOT       firmware tree       (default /srv/wadamesh/firmware)
    WADA_GH_TOKEN      GitHub token with issues:write on the repo. Absent = the
                       service runs normally and simply never publishes.
                       DELIBERATELY UNSET (Kaj's call, 2026-09-23): the matrix
                       lives on wadamesh.com/beta only, so no write credential
                       exists for this anywhere. /report/health reporting
                       publishes:false is the expected state, not a fault. The
                       publishing code below stays in place because turning it
                       on later is then one line in /etc/wadamesh-reports.env.
    WADA_GH_REPO       owner/name          (default ALLFATHER-BV/wadamesh)
    WADA_REPORTS_PORT  listen port         (default 5006)
"""

import json
import os
import re
import sqlite3
import threading
import time
from collections import defaultdict

from flask import Flask, jsonify, request

try:
    import requests
except ImportError:                                    # publishing is optional
    requests = None

DB_PATH   = os.environ.get("WADA_REPORTS_DB", "/opt/wadamesh-reports/reports.db")
FW_ROOT   = os.environ.get("WADA_FW_ROOT", "/srv/wadamesh/firmware")
GH_TOKEN  = os.environ.get("WADA_GH_TOKEN", "").strip()
GH_REPO   = os.environ.get("WADA_GH_REPO", "ALLFATHER-BV/wadamesh")
PORT      = int(os.environ.get("WADA_REPORTS_PORT", "5006"))

# What a tester can say they exercised. Keep these stable: they are stored raw
# and the firmware sends the same keys.
AREAS = ("radio", "map", "link", "input", "storage")
AREA_LABEL = {
    "radio":   "radio and messaging",
    "map":     "map and GPS",
    "link":    "phone-app link",
    "input":   "keyboard and input",
    "storage": "storage and SD",
}
RAN = ("boot", "day", "week")
RAN_LABEL = {"boot": "ran it for minutes", "day": "ran it for about a day",
             "week": "ran it for a week or more"}

# Boards with no web-flasher manifest to take a display name from.
NAME_FALLBACK = {
    "tdisplay-p4-lcd": "LilyGo T-Display P4 (LCD)",
    "tanmatsu":        "Tanmatsu",
}

RE_TAG   = re.compile(r"^beta_\d{1,4}$")
RE_BOARD = re.compile(r"^[a-z0-9][a-z0-9-]{1,31}$")
RE_RID   = re.compile(r"^[0-9a-f]{16}$")

app = Flask(__name__)

# ---------------------------------------------------------------- storage ----

_db_lock = threading.Lock()
_local = threading.local()


def db():
    """One connection per thread. WAL so the publisher can read while a report writes."""
    if not hasattr(_local, "conn"):
        os.makedirs(os.path.dirname(DB_PATH) or ".", exist_ok=True)
        conn = sqlite3.connect(DB_PATH, timeout=10)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA journal_mode=WAL")
        conn.execute("PRAGMA synchronous=NORMAL")
        _local.conn = conn
    return _local.conn


def init_db():
    conn = db()
    conn.executescript(
        """
        -- One row per (device, tag): a tester who changes their mind updates
        -- their own verdict instead of stacking a second one.
        CREATE TABLE IF NOT EXISTS reports (
            rid     TEXT NOT NULL,
            tag     TEXT NOT NULL,
            board   TEXT NOT NULL,
            status  TEXT NOT NULL,          -- works | issues
            areas   TEXT NOT NULL DEFAULT '',
            ran     TEXT NOT NULL DEFAULT 'boot',
            note    TEXT NOT NULL DEFAULT '',
            hw      TEXT NOT NULL DEFAULT '{}',
            created INTEGER NOT NULL,
            updated INTEGER NOT NULL,
            PRIMARY KEY (rid, tag)
        );
        CREATE INDEX IF NOT EXISTS reports_tag ON reports(tag);

        -- Opt-in install counts. A device runs exactly one build, so last
        -- write wins and there is no history to mine.
        CREATE TABLE IF NOT EXISTS pings (
            rid       TEXT PRIMARY KEY,
            tag       TEXT NOT NULL,
            board     TEXT NOT NULL,
            last_seen INTEGER NOT NULL
        );
        CREATE INDEX IF NOT EXISTS pings_tag ON pings(tag);

        CREATE TABLE IF NOT EXISTS state (k TEXT PRIMARY KEY, v TEXT NOT NULL);
        """
    )
    conn.commit()


def state_get(k, default=None):
    row = db().execute("SELECT v FROM state WHERE k=?", (k,)).fetchone()
    return row["v"] if row else default


def state_set(k, v):
    db().execute("INSERT INTO state(k,v) VALUES(?,?) ON CONFLICT(k) DO UPDATE SET v=excluded.v",
                 (k, str(v)))
    db().commit()


# ------------------------------------------------------------- validation ----

def release_boards(tag):
    """Board ids we actually shipped for this tag, from the bins on disk.

    Authoritative and self-maintaining: a board added mid-cycle appears here the
    moment its bin is rsynced, with no list to keep in step.

    Returns None ONLY when there is no releases tree at all, which happens when
    the service runs off the box (local testing) and means "cannot check". An
    unknown tag with the tree present returns an empty set, which is a rejection:
    otherwise anything could open a matrix for a release that does not exist.
    """
    for channel in ("BETA", "TOUCH"):
        d = os.path.join(FW_ROOT, "releases", channel, tag)
        if os.path.isdir(d):
            out = set()
            for fn in os.listdir(d):
                m = re.match(r"^wadamesh-(.+)-merged\.bin$", fn)
                if m:
                    out.add(m.group(1))
            if out:
                return out
    if os.path.isdir(os.path.join(FW_ROOT, "releases")):
        return set()
    return None


def board_name(board, tag=None):
    """Human name, taken from the web-flasher manifest so there is one list."""
    for feed in ("latest-beta", "latest"):
        p = os.path.join(FW_ROOT, feed, "manifest-%s.json" % board)
        try:
            with open(p) as f:
                n = json.load(f).get("name", "")
            n = n.replace("wadamesh - ", "").replace(" (beta)", "").strip()
            if n:
                return n
        except (OSError, ValueError):
            continue
    return NAME_FALLBACK.get(board, board)


def clean_note(s):
    """One line, bounded, and nothing that can break a markdown table."""
    s = "".join(ch for ch in str(s or "") if ch == " " or (ch.isprintable() and ch != "|"))
    s = re.sub(r"\s+", " ", s).strip()
    return s[:280]


# Per-IP bucket. Not security, just friction: the endpoint is public by
# necessity and the worst case is junk rows, which are one DELETE away.
_ip_hits = defaultdict(list)
_ip_lock = threading.Lock()


def rate_ok(ip, limit=30, window=3600):
    now = time.time()
    with _ip_lock:
        hits = [t for t in _ip_hits[ip] if now - t < window]
        hits.append(now)
        _ip_hits[ip] = hits
        return len(hits) <= limit


def client_ip():
    fwd = request.headers.get("X-Forwarded-For", "")
    return fwd.split(",")[0].strip() or request.remote_addr or "?"


def bad(msg, code=400):
    return jsonify({"ok": False, "error": msg}), code


# ---------------------------------------------------------------- intake ----

@app.post("/report/test")
def post_test():
    if not rate_ok(client_ip()):
        return bad("slow down", 429)
    try:
        body = request.get_json(force=True, silent=False) or {}
    except Exception:
        return bad("not json")

    tag   = str(body.get("tag", ""))
    board = str(body.get("board", "")).replace("wadamesh-", "")
    rid   = str(body.get("rid", "")).lower()
    status = str(body.get("status", ""))
    ran    = str(body.get("ran", "boot"))

    if not RE_TAG.match(tag):
        return bad("bad tag")
    if not RE_BOARD.match(board):
        return bad("bad board")
    if not RE_RID.match(rid):
        return bad("bad rid")
    if status not in ("works", "issues"):
        return bad("bad status")
    if ran not in RAN:
        ran = "boot"

    shipped = release_boards(tag)
    if shipped is not None:
        if not shipped:
            return bad("no such release")
        if board not in shipped:
            return bad("board not in this release")

    areas = [a for a in (body.get("areas") or []) if a in AREAS]
    note = clean_note(body.get("note"))
    hw = body.get("hw") if isinstance(body.get("hw"), dict) else {}
    hw = {k: hw[k] for k in list(hw)[:12]}          # bounded, whatever it holds

    now = int(time.time())
    with _db_lock:
        conn = db()
        prev = conn.execute("SELECT updated FROM reports WHERE rid=? AND tag=?",
                            (rid, tag)).fetchone()
        if prev and now - prev["updated"] < 30:
            return bad("slow down", 429)
        conn.execute(
            """INSERT INTO reports(rid,tag,board,status,areas,ran,note,hw,created,updated)
               VALUES(?,?,?,?,?,?,?,?,?,?)
               ON CONFLICT(rid,tag) DO UPDATE SET
                 board=excluded.board, status=excluded.status, areas=excluded.areas,
                 ran=excluded.ran, note=excluded.note, hw=excluded.hw,
                 updated=excluded.updated""",
            (rid, tag, board, status, ",".join(areas), ran, note,
             json.dumps(hw)[:1000], now, now),
        )
        conn.commit()
    mark_dirty(tag)
    return jsonify({"ok": True, "matrix": "https://wadamesh.com/beta.html"})


@app.post("/report/ping")
def post_ping():
    """Opt-in install count. Deliberately holds no history and no report text."""
    if not rate_ok(client_ip(), limit=60):
        return bad("slow down", 429)
    try:
        body = request.get_json(force=True, silent=True) or {}
    except Exception:
        return bad("not json")
    tag   = str(body.get("tag", ""))
    board = str(body.get("board", "")).replace("wadamesh-", "")
    rid   = str(body.get("rid", "")).lower()
    if not RE_TAG.match(tag) or not RE_BOARD.match(board) or not RE_RID.match(rid):
        return bad("bad ping")
    shipped = release_boards(tag)
    if shipped is not None and board not in shipped:
        return bad("no such release")
    with _db_lock:
        db().execute(
            """INSERT INTO pings(rid,tag,board,last_seen) VALUES(?,?,?,?)
               ON CONFLICT(rid) DO UPDATE SET
                 tag=excluded.tag, board=excluded.board, last_seen=excluded.last_seen""",
            (rid, tag, board, int(time.time())),
        )
        db().commit()
    return jsonify({"ok": True})


# ---------------------------------------------------------------- matrix ----

PING_WINDOW = 21 * 86400          # a device silent for three weeks is not "on" a tag


def stable_tag():
    """The tag the stable channel is serving, from the feed the devices read."""
    try:
        with open(os.path.join(FW_ROOT, "latest", "version.json")) as f:
            t = json.load(f).get("tag", "")
        if RE_TAG.match(t):
            return t
    except (OSError, ValueError):
        pass
    return None


def tag_n(tag):
    try:
        return int(tag.split("_")[1])
    except (IndexError, ValueError):
        return -1


def tags_since_stable(limit=12):
    """Every beta published above the current stable, newest first.

    This is the set a promote decision spans. They are kept SEPARATE on purpose:
    a report is about one build, so a green on beta_85 vouches for beta_85 and
    for nothing that came after it. Merging them would quietly let an old tester
    sign off code they never ran.
    """
    floor = tag_n(stable_tag() or "")
    out = []
    try:
        for name in os.listdir(os.path.join(FW_ROOT, "releases", "BETA")):
            if RE_TAG.match(name) and tag_n(name) > floor:
                out.append(name)
    except OSError:
        pass
    # Anything reported on but no longer on disk still belongs in the list.
    for r in db().execute("SELECT DISTINCT tag FROM reports").fetchall():
        if RE_TAG.match(r["tag"]) and tag_n(r["tag"]) > floor and r["tag"] not in out:
            out.append(r["tag"])
    out.sort(key=tag_n, reverse=True)
    return out[:limit]


def current_beta():
    """Highest beta_N with bins on disk; falls back to the newest tag reported."""
    d = os.path.join(FW_ROOT, "releases", "BETA")
    best = None
    try:
        for name in os.listdir(d):
            if RE_TAG.match(name):
                n = int(name.split("_")[1])
                if best is None or n > best[0]:
                    best = (n, name)
    except OSError:
        pass
    if best:
        return best[1]
    row = db().execute("SELECT tag FROM reports ORDER BY updated DESC LIMIT 1").fetchone()
    return row["tag"] if row else None


def build_matrix(tag):
    conn = db()
    rows = conn.execute("SELECT * FROM reports WHERE tag=? ORDER BY updated", (tag,)).fetchall()
    cutoff = int(time.time()) - PING_WINDOW
    pings = conn.execute(
        "SELECT board, COUNT(*) c FROM pings WHERE tag=? AND last_seen>? GROUP BY board",
        (tag, cutoff)).fetchall()
    ping_by_board = {r["board"]: r["c"] for r in pings}

    boards = release_boards(tag) or set()
    boards |= {r["board"] for r in rows} | set(ping_by_board)

    out = []
    for b in sorted(boards):
        mine = [r for r in rows if r["board"] == b]
        works  = [r for r in mine if r["status"] == "works"]
        issues = [r for r in mine if r["status"] == "issues"]
        # "Green" wants two independent devices that actually lived with the
        # build. One report, or a report from someone who only watched it boot,
        # is worth recording and is not worth promoting on.
        soaked = [r for r in works if r["ran"] in ("day", "week")]
        if issues:
            status = "red"
        elif len(soaked) >= 2:
            status = "green"
        elif works:
            status = "amber"
        else:
            status = "none"
        covered = sorted({a for r in works for a in (r["areas"] or "").split(",") if a})
        out.append({
            "board": b,
            "name": board_name(b, tag),
            "status": status,
            "works": len(works),
            "issues": len(issues),
            "soaked": len(soaked),
            "devices": ping_by_board.get(b, 0),
            "areas": covered,
            "missing_areas": [a for a in AREAS if a not in covered],
            "last": max([r["updated"] for r in mine], default=0),
            "notes": [
                {"status": r["status"], "ran": r["ran"], "note": r["note"], "at": r["updated"]}
                for r in mine if r["note"]
            ][-6:],
        })

    red = [b for b in out if b["status"] == "red"]
    # Which boards are OUT THERE, so that silence on one of them means untested
    # rather than unowned. The install count would be the obvious signal, but it
    # is opt-in and most people will leave it off, which would let one green
    # board declare a whole release ready. So a board also counts as owned once
    # anybody has ever reported on it, on any build: reports are not opt-in, and
    # somebody who reported on beta_85 still has that board when beta_86 lands.
    owned = {r["board"] for r in conn.execute(
        "SELECT DISTINCT board FROM reports").fetchall()}
    owned |= {b for b in ping_by_board}
    blocking = [b for b in out
                if b["status"] in ("amber", "none") and (b["devices"] > 0 or b["board"] in owned)]
    return {
        "tag": tag,
        "generated": int(time.time()),
        "issue": state_get("issue:" + tag),
        "stable": stable_tag(),
        "tags": tag_summaries(),
        "boards": out,
        "summary": {
            "green": sum(1 for b in out if b["status"] == "green"),
            "amber": sum(1 for b in out if b["status"] == "amber"),
            "red": len(red),
            "none": sum(1 for b in out if b["status"] == "none"),
            "reports": len(rows),
            "devices": sum(ping_by_board.values()),
            # No reports is not the same as a clean bill of health: with nothing
            # in the table "no reds and nothing blocking" is trivially true, and
            # that is exactly the habit this is meant to replace. At least one
            # board has to have been vouched for.
            "promote_ready": not red and not blocking and any(b["status"] == "green" for b in out),
            "owned": sorted(owned),
            "blocked_by": [b["board"] for b in red] + [b["board"] for b in blocking],
        },
    }


def tag_summaries():
    """One row per beta above stable, for the selector. Counts only."""
    conn = db()
    cutoff = int(time.time()) - PING_WINDOW
    rows = []
    for t in tags_since_stable():
        reps = conn.execute("SELECT board, status, ran FROM reports WHERE tag=?", (t,)).fetchall()
        devs = conn.execute("SELECT COUNT(DISTINCT rid) c FROM pings WHERE tag=? AND last_seen>?",
                            (t, cutoff)).fetchone()["c"]
        by_board = defaultdict(list)
        for r in reps:
            by_board[r["board"]].append(r)
        green = red = amber = 0
        for b, mine in by_board.items():
            if any(r["status"] == "issues" for r in mine):
                red += 1
            elif sum(1 for r in mine if r["status"] == "works" and r["ran"] in ("day", "week")) >= 2:
                green += 1
            else:
                amber += 1
        rows.append({"tag": t, "green": green, "amber": amber, "red": red,
                     "reports": len(reps), "devices": devs})
    return rows


_matrix_cache = {}


@app.get("/report/matrix.json")
def get_matrix():
    tag = request.args.get("tag") or current_beta()
    if not tag:
        return jsonify({"ok": False, "error": "no tag"}), 404
    if not RE_TAG.match(tag):
        return bad("bad tag")
    hit = _matrix_cache.get(tag)
    if hit and time.time() - hit[0] < 15:
        data = hit[1]
    else:
        data = build_matrix(tag)
        _matrix_cache[tag] = (time.time(), data)
    resp = jsonify(data)
    resp.headers["Access-Control-Allow-Origin"] = "*"
    resp.headers["Cache-Control"] = "public, max-age=30"
    return resp


@app.get("/report/health")
def health():
    n = db().execute("SELECT COUNT(*) c FROM reports").fetchone()["c"]
    return jsonify({"ok": True, "reports": n, "publishes": bool(GH_TOKEN and requests)})


# ------------------------------------------------------------- publishing ----

_dirty = set()
_dirty_lock = threading.Lock()


def mark_dirty(tag):
    with _dirty_lock:
        _dirty.add(tag)


def render_issue(m):
    """The tracking issue body. Rendered from SQLite every time, never parsed back."""
    s = m["summary"]
    L = []
    L.append("Test reports for **%s**, sent from the device by the people running it." % m["tag"])
    L.append("")
    L.append("This body is rewritten automatically as reports arrive. Do not edit it by hand; "
             "comments are read, the table is not.")
    L.append("")
    L.append("**To report:** on the device, Settings, About, \"Send a test report\". "
             "To report a bug instead, use the QR on that page, which opens a prefilled issue form.")
    L.append("")
    L.append("| Board | State | Works | Problems | Running it | Untested areas |")
    L.append("|---|---|---|---|---|---|")
    word = {"green": "green", "amber": "needs a second report",
            "red": "problem reported", "none": "no reports"}
    for b in m["boards"]:
        missing = ", ".join(AREA_LABEL[a] for a in b["missing_areas"]) or "none"
        if b["status"] == "none":
            missing = "everything"
        L.append("| %s | %s | %d | %d | %s | %s |" % (
            b["name"], word[b["status"]], b["works"], b["issues"],
            (str(b["devices"]) if b["devices"] else "unknown"), missing))
    L.append("")
    if s["promote_ready"]:
        L.append("**Ready to promote:** no problems outstanding, and every board anyone is "
                 "known to be running has been vouched for by two testers who lived with it.")
    else:
        blocked = ", ".join(s["blocked_by"]) or "nothing"
        L.append("**Not ready to promote.** Waiting on: %s." % blocked)
    L.append("")
    L.append("Green needs two separate devices whose owners ran the build for a day or more. "
             "A board nobody has ever reported on cannot go green and does not block the "
             "promote; a board somebody has reported on before is known to be out there, so "
             "silence on it now blocks until somebody vouches for this build.")
    notes = [(b, n) for b in m["boards"] for n in b["notes"]]
    if notes:
        L.append("")
        L.append("<details><summary>What testers wrote (%d)</summary>" % len(notes))
        L.append("")
        for b, n in sorted(notes, key=lambda x: -x[1]["at"]):
            L.append("- **%s**, %s, %s: %s" % (
                b["name"], "works" if n["status"] == "works" else "problem",
                RAN_LABEL.get(n["ran"], n["ran"]), n["note"]))
        L.append("")
        L.append("</details>")
    L.append("")
    L.append("_Updated %s UTC._" % time.strftime("%Y-%m-%d %H:%M", time.gmtime(m["generated"])))
    return "\n".join(L)


def gh(method, path, payload=None):
    r = requests.request(
        method, "https://api.github.com/repos/%s%s" % (GH_REPO, path),
        headers={"Authorization": "Bearer " + GH_TOKEN,
                 "Accept": "application/vnd.github+json",
                 "X-GitHub-Api-Version": "2022-11-28",
                 "User-Agent": "wadamesh-reports"},
        json=payload, timeout=20)
    r.raise_for_status()
    return r.json()


def publish(tag):
    if not (GH_TOKEN and requests):
        return
    m = build_matrix(tag)
    body = render_issue(m)
    num = state_get("issue:" + tag)
    if num:
        gh("PATCH", "/issues/%s" % num, {"body": body})
    else:
        issue = gh("POST", "/issues", {
            "title": "Test matrix: %s" % tag,
            "body": body,
            "labels": ["test-matrix"],
        })
        state_set("issue:" + tag, issue["number"])


def publisher():
    """Debounced: a burst of reports on one tag costs one API write, not one each."""
    while True:
        time.sleep(30)
        with _dirty_lock:
            tags = list(_dirty)
            _dirty.clear()
        for t in tags:
            try:
                publish(t)
            except Exception as e:                      # never lose a report to GitHub
                print("[reports] publish %s failed: %s" % (t, e), flush=True)
                mark_dirty(t)


if __name__ == "__main__":
    init_db()
    threading.Thread(target=publisher, daemon=True).start()
    print("[reports] db=%s fw=%s github=%s" %
          (DB_PATH, FW_ROOT, GH_REPO if GH_TOKEN else "disabled (no token)"), flush=True)
    app.run(host="127.0.0.1", port=PORT, threaded=True)
