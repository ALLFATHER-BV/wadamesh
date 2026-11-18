"""Rebuilds dict/en.txt, the English word list behind compose spell suggestions.

A maintainer tool, run by hand when the list should change -- the build only
reads dict/en.txt (see scripts/build/gen_spell_dict.py). Needs network access.

Source: hermitdave/FrequencyWords, en_50k (OpenSubtitles 2018 word counts),
https://github.com/hermitdave/FrequencyWords -- content CC-BY-SA-4.0, so
dict/en.txt is under that licence too and says so in its header.

A frequency list rather than a dictionary because suggestions need an order:
of all the words one or two edits from a typo, the most common one is the best
guess. Subtitle counts also track how people actually write to each other,
which is closer to mesh chat than a newspaper corpus is.

Cleaning:
  - FRAGMENTS are dropped, and MESH_WORDS added.
  - letters only: the corpus splits "don't" into "don" + "'t", so the
    apostrophe entries are fragments. Contractions are added from CONTRACTIONS.
  - MISSPELLINGS are dropped: they are frequent in the corpus (people type
    "alot") and a word in the list is never flagged. Taken from the hits of
    Wikipedia's "Lists of common misspellings/For machines" in the top 30k, plus
    the apostrophe-less contractions.
  - 2..20 letters. Single letters are never checked, and nothing longer is
    worth the bytes.

Usage:  python3 scripts/build/build_spell_wordlist.py [--words 25000] [--src en_50k.txt]
        (--src reads a downloaded copy; for Pythons without a CA bundle)
"""
import argparse
import os
import re
import urllib.request

SRC_URL = ("https://raw.githubusercontent.com/hermitdave/FrequencyWords/"
           "master/content/2018/en/en_50k.txt")

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DST_PATH = os.path.join(PROJECT_DIR, "dict", "en.txt")

MISSPELLINGS = {
    # Wikipedia list hits
    "dum", "didnt", "ther", "everytime", "alot", "noone", "doesnt", "isnt",
    "overthere", "upto", "happend", "holliday", "fleed", "loosing", "theyre",
    "thru",
    # Contractions without the apostrophe. Not "ill", "well", "were", "its",
    # "wed", "hell", "shed", "id", "wont"... those are words in their own right
    # (and "cant"/"wont" are rare enough as words that flagging them helps).
    "dont", "im", "cant", "thats", "wont", "youre", "ive", "wasnt", "arent",
    "werent", "couldnt", "wouldnt", "shouldnt", "hasnt", "havent", "hadnt",
    "aint", "whats", "wheres", "theres", "heres", "whos", "lets", "youve",
    "youll", "theyve", "theyll", "weve", "shes", "hes", "itll", "thatll",
    "mustnt", "neednt", "yall", "gonna", "wanna", "gotta", "dunno",
}
# "lets" is a real word, but "let's" is far more common in chat; flagging it
# only offers "let's" as a choice, it never changes anything by itself.
# gonna/wanna/gotta/dunno are left out on purpose too: slang is fine, but
# suggesting "donna" for "gonna" is not.
MISSPELLINGS -= {"gonna", "wanna", "gotta", "dunno"}

# What the corpus's contraction split leaves behind: "didn" from "didn't", "ll"
# from "we'll". "don" goes too -- a word, but its count is really "don't"'s,
# and at that count it would outrank "don't" as the fix for "dont".
FRAGMENTS = {
    "don", "didn", "doesn", "isn", "wasn", "couldn", "wouldn", "shouldn",
    "hasn", "hadn", "aren", "weren", "mustn", "needn", "ain", "ll", "ve", "re",
}

# Mesh and radio words a subtitle corpus does not have (or has too rarely to
# make the cut), so they are never flagged and are there to be suggested.
# Ranked like the contractions, below.
MESH_WORDS = [
    "repeater", "repeaters", "antenna", "antennas", "mesh", "node", "nodes",
    "firmware", "telemetry", "meshtastic", "callsign", "callsigns", "relay",
    "relayed", "router", "routers", "gateway", "bluetooth", "wifi", "uplink",
    "downlink", "packet", "packets", "ack", "acks", "hop", "hops", "sensor",
    "sensors", "solar", "lora", "config", "reboot", "rebooted", "offline",
    "online", "ping", "pinged", "timestamp", "encrypted", "encryption",
    "trackball", "emoji", "dm", "dms", "channel", "channels", "waypoint",
    "waypoints", "coax", "dipole", "yagi", "hotspot", "dashboard", "backup",
    "setup", "username", "screenshot", "app", "apps", "ok", "okay",
    "meshcore", "wadamesh", "advert", "adverts", "companion", "companions",
    "flood", "hashtag", "keyboard", "touchscreen", "reticulum",
]

# Chat shorthand: known, so never flagged, but at the bottom of the ranking --
# "teh" should offer "the" and "ten", not "tbh".
SHORTHAND = ["lol", "btw", "thx", "pls", "plz", "tbh", "imo", "idk", "brb", "afaik"]

CONTRACTIONS = [
    "i'm", "don't", "it's", "that's", "can't", "you're", "i'll", "didn't",
    "i've", "what's", "he's", "there's", "let's", "doesn't", "isn't",
    "won't", "we're", "she's", "they're", "i'd", "wasn't", "you'll", "we'll",
    "aren't", "couldn't", "wouldn't", "haven't", "you've", "who's", "here's",
    "we've", "shouldn't", "where's", "you'd", "they'll", "it'll", "hasn't",
    "weren't", "he'll", "they've", "we'd", "she'll", "that'll", "he'd",
    "hadn't", "they'd", "she'd", "ain't", "how's", "y'all", "o'clock",
    "mustn't", "needn't", "would've", "should've", "could've", "might've",
]

WORD_RE = re.compile(r"^[a-z]{2,20}$")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--words", type=int, default=25000)
    ap.add_argument("--src", help="local copy of en_50k.txt instead of downloading")
    args = ap.parse_args()

    if args.src:
        with open(args.src, "r", encoding="utf-8") as fh:
            rows = fh.read().splitlines()
    else:
        with urllib.request.urlopen(SRC_URL) as r:
            rows = r.read().decode("utf-8").splitlines()

    words = []
    for row in rows:
        parts = row.split()
        if len(parts) != 2:
            continue
        w, n = parts[0], int(parts[1])
        if WORD_RE.match(w) and w not in MISSPELLINGS and w not in FRAGMENTS:
            words.append((w, n))
        if len(words) >= args.words:
            break

    # The corpus has no counts for contractions (it split them). Rank them
    # together as common: the count of the 300th word, which puts them among
    # the words a suggestion should prefer without outranking "the".
    have = {w for w, _ in words}
    cn = words[min(300, len(words) - 1)][1]
    extra = [(c, cn) for c in CONTRACTIONS + MESH_WORDS if c not in have]
    # Mesh words the corpus had, but rarely: lift them to the same rank.
    words = [(w, max(n, cn) if w in MESH_WORDS else n) for w, n in words]
    low = words[-1][1]
    extra += [(c, low) for c in SHORTHAND if c not in have]
    words = sorted(words + extra, key=lambda p: -p[1])

    os.makedirs(os.path.dirname(DST_PATH), exist_ok=True)
    with open(DST_PATH, "w", encoding="utf-8") as fh:
        fh.write("# English word list for compose spell suggestions.\n")
        fh.write("# word<TAB>count, most common first. Built by scripts/build/build_spell_wordlist.py\n")
        fh.write("# from hermitdave/FrequencyWords en_50k (OpenSubtitles 2018),\n")
        fh.write("# https://github.com/hermitdave/FrequencyWords -- CC-BY-SA-4.0.\n")
        for w, n in words:
            fh.write(f"{w}\t{n}\n")
    print(f"{DST_PATH}: {len(words)} words")


main()
