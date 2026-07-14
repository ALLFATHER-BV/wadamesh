#!/usr/bin/env python3
"""
Representative demo data for the wadamesh web-control-panel doc harness.

Field names + shapes here MUST match the firmware's webPush* emitters in
src/ui-touch/UITask.cpp. See serve.py for the @-command -> payload mapping.

Timestamps are seconds since the Unix epoch. Everything is expressed relative to
`NOW` (filled in at serve time) so "last heard" ages stay believable regardless
of when the screenshots are taken. Coordinates are *1e6 fixed-point integers
(la/lo), matching c.gps_lat / c.gps_lon on the device.

The self node sits in Amsterdam; located contacts are scattered a few km around
it so the Distance sort/filter produce nice values.
"""

# Self node position (Amsterdam center) as 1e6 fixed-point, echoed in status.sla/slo.
SELF_LAT = 52_372_800   # 52.3728
SELF_LON = 4_893_400    # 4.8934

# Node identity / radio config shown in the Settings modal + header.
NODE_NAME = "Kaj T-Deck"

RADIO = {
    "freq": 869.525,   # MHz
    "bw": 250.0,       # kHz
    "sf": 11,
    "cr": 5,
    "tx": 22,          # dBm
}

# ---------------------------------------------------------------------------
# Contacts  (webPushContacts: fields i,t,rp,rm,fav,ign,dir,la,lo,adv,n)
#   i   = contact index (used by @m/@oc/@ic/@fv/@bk/@pr/@xc)
#   t   = UI thread index if a chat exists, else -1
#   rp  = 1 if repeater, rm = 1 if room server
#   fav = favourite, ign = blocked, dir = direct (out_path_len==0)
#   la/lo = gps *1e6 (0 if unknown), adv = last_advert_timestamp (epoch s)
#   n   = name
# `_dlat/_dlon` below are offsets in degrees from the self node; converted to the
# absolute 1e6 la/lo at build time. Contacts with no GPS use la=lo=0.
# ---------------------------------------------------------------------------
_CONTACTS = [
    # name                  thread  type       fav  ign  dir  age(s)   dlat      dlon
    dict(n="Sanne",              t=0, kind="",    fav=1, ign=0, dir=1, age=95,     dlat=0.021,  dlon=-0.015),
    dict(n="Diego",              t=1, kind="",    fav=0, ign=0, dir=0, age=1500,   dlat=-0.034, dlon=0.052),
    dict(n="Marta",              t=2, kind="",    fav=0, ign=0, dir=1, age=320,    dlat=0.0,    dlon=0.0),
    dict(n="Hilltop Repeater",   t=-1, kind="rp", fav=0, ign=0, dir=0, age=210,    dlat=0.145,  dlon=0.088),
    dict(n="Harbor Room",        t=3, kind="rm",  fav=0, ign=0, dir=0, age=640,    dlat=-0.011, dlon=0.031),
    dict(n="Noah",               t=-1, kind="",   fav=0, ign=0, dir=0, age=18000,  dlat=0.0,    dlon=0.0),
    dict(n="Femke",              t=-1, kind="",   fav=0, ign=0, dir=1, age=86400*3, dlat=-0.062, dlon=-0.044),
    dict(n="Old Node",           t=-1, kind="",   fav=0, ign=1, dir=0, age=86400*9, dlat=0.0,   dlon=0.0),
]

# Group channels (webPushContacts channels[]: i=slot, t=thread, n=name).
_CHANNELS = [
    dict(slot=0, t=-1, n="Public"),
    dict(slot=1, t=-1, n="Emergency"),
]

# ---------------------------------------------------------------------------
# Messages per thread  (webPushMessages: msgs[] fields o,ts,ds,pl,sn,rs,rp,s,x)
#   o  = outgoing (1) / incoming (0)
#   ts = epoch s
#   ds = delivery state for outgoing: 0 none,1 sent,2 delivered,3 failed
#   pl = path length (hops); 255 shows as "Direct" in msg info
#   sn = SNR dB (incoming), rs = RSSI dBm (incoming)
#   rp = repeats heard (outgoing)
#   s  = sender name (shown only in channel threads), x = text
# Ordered oldest..newest here; serve.py emits newest-first (as the firmware does)
# and the browser reverses again for display.
# ---------------------------------------------------------------------------
_MESSAGES = {
    # Sanne (direct peer) — thread 0
    0: [
        dict(o=0, ago=5400, x="Morning! You around for the hike this weekend?", pl=0, sn=8, rs=-64),
        dict(o=1, ago=5300, x="Yeah! Saturday works. Meshing the whole way up.", ds=2, pl=0, rp=1),
        dict(o=0, ago=5200, x="Perfect. I'll bring the spare T-Deck as a relay.", pl=0, sn=7, rs=-66),
        dict(o=1, ago=280,  x="Just tested range from the ridge — 6 km line of sight.", ds=2, pl=0, rp=2),
        dict(o=0, ago=95,   x="Nice, that covers the valley. See you at 8.", pl=0, sn=9, rs=-61),
    ],
    # Diego (2-hop peer) — thread 1
    1: [
        dict(o=0, ago=2000, x="Did the firmware update land on your node?", pl=2, sn=2, rs=-98),
        dict(o=1, ago=1900, x="Flashing now. beta looks solid so far.", ds=2, pl=2, rp=3),
        dict(o=0, ago=1600, x="Great. Ping me once the repeater picks you up.", pl=2, sn=1, rs=-101),
        dict(o=1, ago=1500, x="Will do 👍", ds=1, pl=2, rp=0),
    ],
    # Marta (direct) — thread 2
    2: [
        dict(o=1, ago=900, x="Map tiles are caching fine on the new build.", ds=2, pl=0, rp=1),
        dict(o=0, ago=700, x="Awesome — offline maps at the campsite then.", pl=0, sn=6, rs=-70),
        dict(o=1, ago=360, x="Exactly. Sending you my GPS pin now.", ds=3, pl=0, rp=0),
        dict(o=0, ago=320, x="Didn't come through, try again?", pl=0, sn=5, rs=-72),
    ],
    # Harbor Room (room server channel) — thread 3, has unread
    3: [
        dict(o=0, ago=1200, s="Diego",  x="Anyone near the north dock?", pl=1, sn=3, rs=-92),
        dict(o=0, ago=1100, s="Sanne",  x="I'm 300 m out, what's up?", pl=1, sn=7, rs=-68),
        dict(o=1, ago=1000, s=NODE_NAME, x="Repeater on the crane looks healthy from here.", ds=2, pl=1, rp=2),
        dict(o=0, ago=700,  s="Femke",  x="Copy. Battery at 82% on the solar node.", pl=2, sn=1, rs=-104),
        dict(o=0, ago=640,  s="Diego",  x="Meeting at the boathouse in 10.", pl=1, sn=4, rs=-88),
    ],
}

# ---------------------------------------------------------------------------
# Threads list  (webPushThreads: threads[] fields i,ch,u,ts,n,last)
#   Derived from _MESSAGES so "last" + "ts" always match the open chat.
#   u = unread count (only a couple non-zero), ch=1 for channel/room threads.
# ---------------------------------------------------------------------------
_THREAD_META = {
    0: dict(ch=0, u=0, n="Sanne"),
    1: dict(ch=0, u=1, n="Diego"),
    2: dict(ch=0, u=0, n="Marta"),
    3: dict(ch=1, u=2, n="Harbor Room"),
}

# ---------------------------------------------------------------------------
# Discovered (heard-not-added) nodes  (webPushDiscovered: nodes[] i,rp,rm,pl,adv,la,lo,n)
# ---------------------------------------------------------------------------
_DISCOVERED = [
    dict(n="Lars",             kind="",   pl=2, age=140,  dlat=0.012,  dlon=0.077),
    dict(n="Ridge Repeater",   kind="rp", pl=1, age=380,  dlat=0.191,  dlon=-0.02),
    dict(n="Cafe Beacon",      kind="",   pl=3, age=900,  dlat=-0.008, dlon=0.006),
    dict(n="Yuki",             kind="",   pl=2, age=2600, dlat=0.0,    dlon=0.0),
]


def _abs_coord(base, delta_deg):
    """Contact coord (1e6 fixed-point) from a degree offset; 0 stays 0 (unknown)."""
    if delta_deg == 0:
        return 0
    return int(round(base + delta_deg * 1_000_000))


def contacts_payload(now):
    out = []
    for i, c in enumerate(_CONTACTS):
        la = _abs_coord(SELF_LAT, c["dlat"])
        lo = _abs_coord(SELF_LON, c["dlon"])
        out.append({
            "i": i, "t": c["t"],
            "rp": 1 if c["kind"] == "rp" else 0,
            "rm": 1 if c["kind"] == "rm" else 0,
            "fav": c["fav"], "ign": c["ign"], "dir": c["dir"],
            "la": la, "lo": lo,
            "adv": now - c["age"],
            "n": c["n"],
        })
    channels = [{"i": ch["slot"], "t": ch["t"], "n": ch["n"]} for ch in _CHANNELS]
    return {"t": "c", "dc": len(_DISCOVERED), "contacts": out, "channels": channels}


def _thread_last(tidx):
    msgs = _MESSAGES.get(tidx, [])
    return msgs[-1] if msgs else None


def threads_payload(now):
    out = []
    for tidx, meta in _THREAD_META.items():
        last = _thread_last(tidx)
        ts = now - last["ago"] if last else 0
        text = last["x"] if last else ""
        out.append({
            "i": tidx, "ch": meta["ch"], "u": meta["u"],
            "ts": ts, "n": meta["n"], "last": text,
        })
    return {"t": "t", "threads": out}


def messages_payload(now, tidx):
    meta = _THREAD_META.get(tidx)
    msgs = _MESSAGES.get(tidx, [])
    ch = meta["ch"] if meta else 0
    name = meta["n"] if meta else "Chat"
    # firmware emits newest-first; the browser reverses for display
    arr = []
    for m in reversed(msgs):
        arr.append({
            "o": m["o"],
            "ts": now - m["ago"],
            "ds": m.get("ds", 0),
            "pl": m.get("pl", 0),
            "sn": m.get("sn", 0),
            "rs": m.get("rs", 0),
            "rp": m.get("rp", 0),
            "s": m.get("s", ""),
            "x": m["x"],
        })
    return {"t": "m", "i": tidx, "ch": ch, "n": name, "msgs": arr}


def discovered_payload(now):
    out = []
    for i, d in enumerate(_DISCOVERED):
        out.append({
            "i": i,
            "rp": 1 if d["kind"] == "rp" else 0,
            "rm": 1 if d["kind"] == "rm" else 0,
            "pl": d["pl"],
            "adv": now - d["age"],
            "la": _abs_coord(SELF_LAT, d["dlat"]),
            "lo": _abs_coord(SELF_LON, d["dlon"]),
            "n": d["n"],
        })
    return {"t": "dc", "nodes": out}


def contact_info_payload(now, cidx):
    """webPushContactInfo: fields i,rp,rm,fav,ign,pl,adv,la,lo,k,n."""
    if cidx < 0 or cidx >= len(_CONTACTS):
        return {"t": "ci", "i": cidx, "rp": 0, "rm": 0, "fav": 0, "ign": 0,
                "pl": -1, "adv": 0, "la": 0, "lo": 0, "k": "000000000000", "n": "Contact"}
    c = _CONTACTS[cidx]
    la = _abs_coord(SELF_LAT, c["dlat"])
    lo = _abs_coord(SELF_LON, c["dlon"])
    # a stable pseudo pubkey-prefix (12 hex chars) so the Info card's ID looks real
    k = "%02x%02x%02x%02x%02x%02x" % tuple((0x2a + cidx * 7 + j * 31) & 0xff for j in range(6))
    return {
        "t": "ci", "i": cidx,
        "rp": 1 if c["kind"] == "rp" else 0,
        "rm": 1 if c["kind"] == "rm" else 0,
        "fav": c["fav"], "ign": c["ign"],
        "pl": 0 if c["dir"] else (2 if c["kind"] != "rp" else 3),
        "adv": now - c["age"],
        "la": la, "lo": lo, "k": k, "n": c["n"],
    }


def status_payload(now):
    """webPushStatus: batt,chg,wifi,rssi,ble,clk,snr,unr,mi,sla,slo,nm."""
    unread = sum(m["u"] for m in _THREAD_META.values())
    return {
        "t": "st",
        "batt": 78, "chg": 0,
        "wifi": 1, "rssi": -58,
        "ble": 1,
        "clk": now,
        "snr": 8,          # good last-heard signal -> 4 bars
        "unr": unread,
        "mi": 0,           # km, not miles
        "sla": SELF_LAT, "slo": SELF_LON,
        "nm": NODE_NAME,
    }


def settings_payload(now):
    """webPushSettings: freq,bw,sf,cr,tx,wifi,ble,blecap,tcp,name."""
    return {
        "t": "sg",
        "freq": RADIO["freq"], "bw": RADIO["bw"],
        "sf": RADIO["sf"], "cr": RADIO["cr"], "tx": RADIO["tx"],
        "wifi": 1, "ble": 1, "blecap": 1, "tcp": 1,
        "name": NODE_NAME,
    }


# ---------------------------------------------------------------------------
# Terminal (0x00 text) canned replies for the most common CLI commands, so the
# Terminal tab shows believable output in screenshots. Anything not listed gets
# a generic acknowledgement.
# ---------------------------------------------------------------------------
def terminal_reply(now, cmd):
    import datetime
    c = cmd.strip()
    cl = c.lower()
    clk = datetime.datetime.fromtimestamp(now).strftime("%Y-%m-%d %H:%M:%S")
    if cl in ("help", "?"):
        return (
            "commands:\n"
            "  help ?            this list\n"
            "  ver               firmware version\n"
            "  clock             RTC time\n"
            "  status            node status\n"
            "  get               radio params\n"
            "  advert            send an advert\n"
            "  set <k> <v>       name/freq/bw/sf/cr/tx\n"
            "  wifi|ble|tcp ...  connectivity\n"
            "  list contacts channels\n"
            "  to <name> / send <text> / public <text>\n"
            "  reboot bootloader\n"
        )
    if cl == "ver":
        return "wadamesh " + FIRMWARE_VERSION + " (demo)\n"
    if cl == "clock":
        return clk + "\n"
    if cl == "status":
        return ("node: %s\nbattery: 78%%  (discharging)\nwifi: connected  -58 dBm\n"
                "ble: on   tcp: on\nmesh: 4 bars   contacts: %d\n"
                % (NODE_NAME, len(_CONTACTS)))
    if cl == "get":
        return ("freq: %.3f MHz\nbw: %.0f kHz\nsf: %d\ncr: %d\ntx: %d dBm\n"
                % (RADIO["freq"], RADIO["bw"], RADIO["sf"], RADIO["cr"], RADIO["tx"]))
    if cl == "list" or cl == "list contacts" or cl == "contacts":
        lines = ["contacts:"]
        for i, c2 in enumerate(_CONTACTS):
            tag = " [RPT]" if c2["kind"] == "rp" else (" [ROOM]" if c2["kind"] == "rm" else "")
            lines.append("  %2d  %s%s" % (i, c2["n"], tag))
        return "\n".join(lines) + "\n"
    if cl == "channels":
        return "channels:\n" + "\n".join("  %d  %s" % (c2["slot"], c2["n"]) for c2 in _CHANNELS) + "\n"
    if cl == "advert" or cl == "advert.zerohop":
        return "advert sent.\n"
    if cl.startswith("wifi"):
        return "wifi: connected  ssid=meshnet  -58 dBm  10.0.0.42\n"
    if cl.startswith("ble"):
        return "ble: advertising as \"" + NODE_NAME + "\"\n"
    if cl.startswith("tcp"):
        return "tcp: listening on :5000 (phone app)\n"
    if cl.startswith("set "):
        return "ok\n"
    if cl in ("reboot", "bootloader"):
        return "(demo) ignored: " + c + "\n"
    return "ok\n"


# firmware version string surfaced by `ver` (kept generic; edit if you want an
# exact tag in the screenshot).
FIRMWARE_VERSION = "v1.x-touch"
