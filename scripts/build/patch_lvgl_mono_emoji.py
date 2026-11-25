# SPDX-License-Identifier: GPL-3.0-or-later

# The e-paper boards (WADA_EMOJI_MONO) draw emoji as black ink with an alpha mask, and
# LVGL's image-font path (lv_draw_sw_letter.c) blits them as they are, whatever the text
# colour. Black emoji vanish wherever the text is light on a dark fill: a selection
# (select-all in a text field), or any label drawn white on black.
#
# This recolours an image glyph to the text colour when that colour is light, so the
# emoji flips to white ink exactly where the letters beside it did. Dark text, and every
# build without WADA_EMOJI_MONO, draw as before; the recolour is a per-pixel mix over
# 16 x 16 pixels and only runs for light text.
#
# Idempotent and fail-closed, like patch_lvgl_async_del.py, and installed the same way: a
# PlatformIO pre-script with a pre-link check, so the first build on a fresh checkout
# patches LVGL after it is fetched and fails its link; running it again compiles the
# patched source.

import os
import sys

MARKER = "wadamesh-lvgl-mono-emoji-patch"
PATH = os.path.join("src", "draw", "sw", "lv_draw_sw_letter.c")
REQUIRED = (
    "static void LV_ATTRIBUTE_FAST_MEM draw_letter_normal(",
    "    if(bpp == LV_IMGFONT_BPP) { //is imgfont",
)
OLD = """        img_dsc.blend_mode = dsc->blend_mode;
        lv_draw_img(draw_ctx, &img_dsc, &fill_area, map_p);"""
NEW = """        img_dsc.blend_mode = dsc->blend_mode;
#if defined(WADA_EMOJI_MONO) && WADA_EMOJI_MONO
        /*wadamesh-lvgl-mono-emoji-patch: black-ink emoji take the text colour when it is light
         *(white on black, a selection), or they would vanish.*/
        if(lv_color_brightness(dsc->color) > 127) {
            img_dsc.recolor = dsc->color;
            img_dsc.recolor_opa = LV_OPA_COVER;
        }
#endif
        lv_draw_img(draw_ctx, &img_dsc, &fill_area, map_p);"""


def patch_source(source):
    """Returns (source, changed)."""
    if any(source.count(line) != 1 for line in REQUIRED):
        raise RuntimeError("lv_draw_sw_letter.c does not match LVGL 8.4 (LVGL version drift?)")
    old_count, new_count, marker_count = source.count(OLD), source.count(NEW), source.count(MARKER)
    if old_count == 0 and new_count == 1 and marker_count == 1:
        return source, False
    if old_count != 1 or new_count != 0 or marker_count != 0:
        raise RuntimeError("lv_draw_sw_letter.c patch site does not match LVGL 8.4 (LVGL version drift?)")
    patched = source.replace(OLD, NEW, 1)
    if patched.count(NEW) != 1 or patched.count(MARKER) != 1:
        raise RuntimeError("lv_draw_sw_letter.c patch verification failed")
    return patched, True


def patch_root(root):
    path = os.path.join(root, PATH)
    with open(path, encoding="utf-8") as f:
        patched, changed = patch_source(f.read())
    if changed:
        with open(path, "w", encoding="utf-8") as f:
            f.write(patched)
    return changed


def verify_root(root):
    with open(os.path.join(root, PATH), encoding="utf-8") as f:
        _, changed = patch_source(f.read())
    if changed:
        raise RuntimeError("lv_draw_sw_letter.c still lacks the mono emoji patch")


def self_test():
    fixture = "\n".join(REQUIRED) + "\n" + OLD + "\n"
    patched, changed = patch_source(fixture)
    assert changed and MARKER in patched

    same, changed = patch_source(patched)
    assert not changed and same == patched

    for invalid in (OLD + "\n", fixture + OLD, patched + OLD, patched + NEW,
                    "/* " + MARKER + " */\n" + fixture):
        try:
            patch_source(invalid)
        except RuntimeError:
            pass
        else:
            raise AssertionError("version drift or an ambiguous state must fail closed")
    print("patch_lvgl_mono_emoji self-test passed")


def install(platformio_env):
    root = os.path.join(
        platformio_env.subst("$PROJECT_LIBDEPS_DIR"),
        platformio_env.subst("$PIOENV"),
        "lvgl",
    )
    target = os.path.join(root, PATH)

    def apply_or_error():
        if not os.path.isfile(target):
            return "LVGL patch target is missing: %s" % target
        try:
            changed = patch_root(root)
        except (OSError, RuntimeError) as error:
            return str(error)
        print("[patch_lvgl_mono_emoji] %s" % ("patched" if changed else "already patched"))
        return None

    if os.path.isfile(target):
        error = apply_or_error()
        if error is not None:
            print("[patch_lvgl_mono_emoji] ERROR: %s" % error)
            platformio_env.Exit(1)
    else:
        print("[patch_lvgl_mono_emoji] LVGL not fetched yet - patch deferred to pre-link check")

    def verify_patched(target, source, env):
        del target, source, env
        try:
            verify_root(root)
            return 0
        except (OSError, RuntimeError):
            pass
        error = apply_or_error()
        if error is not None:
            print("[patch_lvgl_mono_emoji] ERROR: %s" % error)
            return 1
        print("[patch_lvgl_mono_emoji] ERROR: LVGL was patched after compilation")
        print("[patch_lvgl_mono_emoji] ERROR: re-run `pio run` to compile the fix")
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
    else:
        raise SystemExit("usage: patch_lvgl_mono_emoji.py --self-test")
