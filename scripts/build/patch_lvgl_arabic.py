# SPDX-License-Identifier: GPL-3.0-or-later

# LVGL 8.4 shapes Arabic through the ap_chars_map[] table in src/misc/lv_txt_ap.c, and
# a few of its rows are wrong (LVGL fixed most of them only in v9; #582, reported by a
# native speaker on the T-LoRa Pager). A row is {offset from U+0622, final form, then the
# initial, medial and isolated forms as offsets from the final one, {joins the previous
# letter, joins the next}}. Lookup also matches every presentation form a row produces,
# so one bad row can swallow other characters:
#
#   - آ (U+0622) had no row: it never joined the letter before it (بآ), and لآ never
#     became its ligature (U+FEF5), because the lam-alef check finds the alef by its row.
#     Its forms are final U+FE82 and isolated U+FE81, like the other alef rows.
#   - ئ (U+0626) said it does not join the next letter, so شيئا, رئيس, هيئة broke apart
#     after it (LVGL 9 #6430).
#   - ة (U+0629) had the initial and medial offsets of a joining letter, which land on
#     ت (U+FE95, U+FE96). It never joins the next letter, so it gets isolated and final
#     forms only, as the other non-joining rows do (LVGL 9 still has the medial one wrong).
#   - ۰ (U+06F0) had +1/+2/-1 forms: lookup then took ۱ and ۲ for forms of ۰, and all
#     three were drawn as U+06EF (LVGL 9 #6430, #10718).
#   - The alef row (U+0627) was labelled آ; only the comment changes.
#
# Idempotent and fail-closed, like patch_lvgl_anim_uaf.py, and installed the same way: a
# PlatformIO pre-script with a pre-link check, and --patch-file for the IDF builds
# (Tanmatsu, T-Display P4), which vendor LVGL through their fetch-deps.sh.

import os
import sys

MARKER = "wadamesh-lvgl-arabic-patch"
EDITS = (
    (
        "    /*{Key Offset, End, Beginning, Middle, Isolated, {conjunction}}*/\n"
        "    {1, 0xFE84, -1, 0, -1,  {1, 0}},    // أ\n",
        "    /*{Key Offset, End, Beginning, Middle, Isolated, {conjunction}}*/\n"
        "    {0, 0xFE82, -1, 0, -1,  {1, 0}},    // آ  (wadamesh-lvgl-arabic-patch: LVGL 8 had no row)\n"
        "    {1, 0xFE84, -1, 0, -1,  {1, 0}},    // أ\n",
    ),
    (
        "    {4, 0xFE8A, 1, 2, -1,  {1, 0}},    // ئ\n",
        "    {4, 0xFE8A, 1, 2, -1,  {1, 1}},    // ئ  (joins the next letter)\n",
    ),
    (
        "    {5, 0xFE8E, -1, 0, -1,  {1, 0}},    // آ\n",
        "    {5, 0xFE8E, -1, 0, -1,  {1, 0}},    // ا\n",
    ),
    (
        "    {7, 0xFE94, 1, 2, -1,  {1, 0}},   // ة\n",
        "    {7, 0xFE94, -1, 0, -1,  {1, 0}},   // ة  (never joins the next letter)\n",
    ),
    (
        "    {206, 0x06F0, 1, 2, -1,  {0, 0}},  // ۰\n",
        "    {206, 0x06F0, 0, 0, 0,  {0, 0}},  // ۰  (its own form only)\n",
    ),
)
REQUIRED = ("const ap_chars_map_t ap_chars_map[] = {", "    if(ch_code == 0x0622) {")


def patch_source(source):
    if any(source.count(line) != 1 for line in REQUIRED):
        raise RuntimeError("lv_txt_ap.c does not match LVGL 8.4 (LVGL version drift?)")
    olds = [source.count(old) for old, _ in EDITS]
    news = [source.count(new) for _, new in EDITS]
    marker = source.count(MARKER)
    if all(n == 1 for n in news) and all(o == 0 for o in olds) and marker == 1:
        return source, False
    if not (all(o == 1 for o in olds) and all(n == 0 for n in news) and marker == 0):
        raise RuntimeError("lv_txt_ap.c ap_chars_map does not match LVGL 8.4 (LVGL version drift?)")
    patched = source
    for old, new in EDITS:
        patched = patched.replace(old, new, 1)
    if (any(patched.count(new) != 1 for _, new in EDITS)
            or any(patched.count(old) != 0 for old, _ in EDITS)
            or patched.count(MARKER) != 1):
        raise RuntimeError("lv_txt_ap.c patch verification failed")
    return patched, True


def patch_file(path):
    with open(path, encoding="utf-8") as source_file:
        source = source_file.read()
    patched, changed = patch_source(source)
    if changed:
        with open(path, "w", encoding="utf-8") as source_file:
            source_file.write(patched)
    return changed


def verify_source(source):
    _, changed = patch_source(source)
    if changed:
        raise RuntimeError("lv_txt_ap.c still has LVGL 8's Arabic shaping rows")


def verify_file(path):
    with open(path, encoding="utf-8") as source_file:
        verify_source(source_file.read())


def self_test():
    fixture = (REQUIRED[0] + "\n" + "".join(old for old, _ in EDITS)
               + "    LV_AP_END_CHARS_LIST\n};\n" + REQUIRED[1] + "\n")
    patched, changed = patch_source(fixture)
    assert changed
    assert MARKER in patched
    assert "{0, 0xFE82, -1, 0, -1,  {1, 0}}" in patched
    assert "{4, 0xFE8A, 1, 2, -1,  {1, 1}}" in patched
    assert "{7, 0xFE94, -1, 0, -1,  {1, 0}}" in patched
    assert "{206, 0x06F0, 0, 0, 0,  {0, 0}}" in patched

    same, changed = patch_source(patched)
    assert not changed
    assert same == patched
    verify_source(patched)

    try:
        verify_source(fixture)
    except RuntimeError:
        pass
    else:
        raise AssertionError("verification must reject an unpatched source")

    for invalid in ("static void nothing(void) {}\n",
                    fixture.replace(EDITS[1][0], ""),
                    patched + EDITS[0][0],
                    "/* " + MARKER + " */\n" + fixture):
        try:
            patch_source(invalid)
        except RuntimeError:
            pass
        else:
            raise AssertionError("drift or a half-patched source must fail closed")

    print("patch_lvgl_arabic self-test passed")


def install(platformio_env):
    path = os.path.join(
        platformio_env.subst("$PROJECT_LIBDEPS_DIR"),
        platformio_env.subst("$PIOENV"),
        "lvgl",
        "src",
        "misc",
        "lv_txt_ap.c",
    )

    def apply_or_error():
        if not os.path.isfile(path):
            return "LVGL patch target is missing: %s" % path
        try:
            changed = patch_file(path)
        except (OSError, RuntimeError) as error:
            return str(error)
        print("[patch_lvgl_arabic] %s" % ("patched lv_txt_ap.c" if changed else "already patched"))
        return None

    if os.path.isfile(path):
        error = apply_or_error()
        if error is not None:
            print("[patch_lvgl_arabic] ERROR: %s" % error)
            platformio_env.Exit(1)
    else:
        print("[patch_lvgl_arabic] LVGL not fetched yet - patch deferred to pre-link check")

    def verify_patched(target, source, env):
        del target, source
        if os.path.isfile(path):
            try:
                verify_file(path)
                return 0
            except (OSError, RuntimeError):
                pass
        error = apply_or_error()
        if error is not None:
            print("[patch_lvgl_arabic] ERROR: %s" % error)
            return 1
        print("[patch_lvgl_arabic] ERROR: LVGL was patched after compilation")
        print("[patch_lvgl_arabic] ERROR: re-run `pio run` to compile the fix")
        return 1

    platformio_env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", verify_patched)


try:
    Import("env")  # noqa: F821 - provided by PlatformIO/SCons
except NameError:
    env = None

if env is not None:
    install(env)

if env is None and __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "--self-test":
        self_test()
    elif len(sys.argv) == 3 and sys.argv[1] == "--patch-file":
        try:
            changed = patch_file(sys.argv[2])
            print("patch_lvgl_arabic: %s" % ("patched" if changed else "already patched"))
        except (OSError, RuntimeError) as error:
            print("patch_lvgl_arabic: ERROR: %s" % error, file=sys.stderr)
            raise SystemExit(1)
    elif len(sys.argv) == 3 and sys.argv[1] == "--verify-file":
        try:
            verify_file(sys.argv[2])
            print("patch_lvgl_arabic: verified")
        except (OSError, RuntimeError) as error:
            print("patch_lvgl_arabic: ERROR: %s" % error, file=sys.stderr)
            raise SystemExit(1)
    else:
        raise SystemExit(
            "usage: patch_lvgl_arabic.py --self-test | --patch-file <lv_txt_ap.c> | --verify-file <lv_txt_ap.c>"
        )
