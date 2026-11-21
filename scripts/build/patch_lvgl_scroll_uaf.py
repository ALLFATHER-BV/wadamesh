# SPDX-License-Identifier: GPL-3.0-or-later

# LVGL 8.4's obj_del_core() resets a pointer input device when the deleted
# object is its act_obj or last_obj, but never looks at scroll_obj:
#
#     if(indev->proc.types.pointer.act_obj == obj || indev->proc.types.pointer.last_obj == obj) {
#         lv_indev_reset(indev, obj);
#     }
#
# Usually that is enough, because scroll_obj is an ancestor of act_obj and
# deleting it deletes act_obj too. It is not enough after lv_indev_wait_release():
# the release that ends the wait clears act_obj and last_obj but keeps
# scroll_obj for the scroll throw. popupClose() does exactly that (wait_release,
# then lv_obj_del_async), so closing a sheet while its page was being scrolled
# leaves scroll_obj pointing at the freed page. The next press skips the
# "stop the old throw" step (it needs last_obj) and _lv_indev_scroll_handler
# walks the freed object's parents for transform_angle -> LoadProhibited in
# get_prop_core. This is the beta_102 T-Deck coredump: loopTask,
# indev_proc_press -> _lv_indev_scroll_handler -> lv_obj_get_style_prop,
# EXCVADDR 0x80c08040. Without a press, the same pointer reaches
# _lv_indev_scroll_throw_handler (the beta_18 coredump at popupClose()).
#
# The patch resets the device for scroll_obj as well, as LVGL 9 does.
# lv_indev_reset() sets reset_query; the next read clears scroll_obj before it
# processes the point, and the press and release paths stop after any event
# that set it. _lv_indev_scroll_throw_handler() does not stop: it keeps using
# scroll_obj after lv_obj_scroll_by() has sent LV_EVENT_SCROLL. So a SCROLL or
# SCROLL_END handler must never delete the scrolled object synchronously - close
# through popupClose() (lv_obj_del_async), as every handler does today.
#
# Idempotent and fail-closed, like patch_lvgl_anim_uaf.py. On a fresh checkout
# pre-scripts run before PlatformIO fetches lib_deps, so the pre-link action
# patches the newly fetched source and fails that first link. Re-running then
# compiles the patched source. The IDF builds (Tanmatsu, T-Display P4, CrowPanel
# P4) and the web build vendor LVGL and apply this with --patch-file.

import os
import sys

MARKER = "wadamesh-lvgl-scroll-uaf-patch"
OLD = """        if(indev->proc.types.pointer.act_obj == obj || indev->proc.types.pointer.last_obj == obj) {
            lv_indev_reset(indev, obj);
        }"""
NEW = """        /*wadamesh-lvgl-scroll-uaf-patch: also when `obj` is being scrolled. After
         *lv_indev_wait_release() act_obj and last_obj are NULL but scroll_obj is kept
         *for the throw, and the next press or throw step would use the freed object.*/
        if(indev->proc.types.pointer.act_obj == obj || indev->proc.types.pointer.last_obj == obj ||
           indev->proc.types.pointer.scroll_obj == obj) {
            lv_indev_reset(indev, obj);
        }"""
REQUIRED_LINES = (
        "static void obj_del_core(lv_obj_t * obj)\n{",
        "    /*Reset all input devices if the object to delete is used*/",
        "        if(indev->proc.types.pointer.last_pressed == obj) {",
)


def patch_source(source):
    if any(source.count(line) != 1 for line in REQUIRED_LINES):
        raise RuntimeError(
            "lv_obj_tree.c obj_del_core() does not match LVGL 8.4 (LVGL version drift?)"
        )
    old_count = source.count(OLD)
    new_count = source.count(NEW)
    marker_count = source.count(MARKER)
    if old_count == 0 and new_count == 1 and marker_count == 1:
        return source, False
    if old_count != 1 or new_count != 0 or marker_count != 0:
        raise RuntimeError(
            "lv_obj_tree.c indev reset does not match LVGL 8.4 (LVGL version drift?)"
        )
    patched = source.replace(OLD, NEW, 1)
    if patched.count(OLD) != 0 or patched.count(NEW) != 1 or patched.count(MARKER) != 1:
        raise RuntimeError("lv_obj_tree.c patch verification failed")
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
        raise RuntimeError("lv_obj_tree.c still leaves a deleted scroll_obj in the input device")


def verify_file(path):
    with open(path, encoding="utf-8") as source_file:
        verify_source(source_file.read())


def self_test():
    fixture = "\n".join(REQUIRED_LINES[:2]) + "\n" + OLD + "\n" + REQUIRED_LINES[2] + "\n"
    patched, changed = patch_source(fixture)
    assert changed
    assert MARKER in patched
    assert "indev->proc.types.pointer.scroll_obj == obj) {" in patched
    assert "last_obj == obj) {" not in patched

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

    try:
        patch_source("static void obj_del_core(lv_obj_t * obj) {}\n")
    except RuntimeError:
        pass
    else:
        raise AssertionError("version drift must fail closed")

    for invalid in (fixture + OLD, patched + OLD, "/* " + MARKER + " */\n" + fixture):
        try:
            patch_source(invalid)
        except RuntimeError:
            pass
        else:
            raise AssertionError("ambiguous or poisoned patch state must fail closed")

    print("patch_lvgl_scroll_uaf self-test passed")


def install(platformio_env):
    path = os.path.join(
        platformio_env.subst("$PROJECT_LIBDEPS_DIR"),
        platformio_env.subst("$PIOENV"),
        "lvgl",
        "src",
        "core",
        "lv_obj_tree.c",
    )

    def apply_or_error():
        if not os.path.isfile(path):
            return "LVGL patch target is missing: %s" % path
        try:
            changed = patch_file(path)
        except (OSError, RuntimeError) as error:
            return str(error)
        print(
            "[patch_lvgl_scroll_uaf] %s"
            % ("patched lv_obj_tree.c" if changed else "already patched")
        )
        return None

    if os.path.isfile(path):
        error = apply_or_error()
        if error is not None:
            print("[patch_lvgl_scroll_uaf] ERROR: %s" % error)
            platformio_env.Exit(1)
    else:
        print(
            "[patch_lvgl_scroll_uaf] LVGL not fetched yet - "
            "patch deferred to pre-link check"
        )

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
            print("[patch_lvgl_scroll_uaf] ERROR: %s" % error)
            return 1
        print("[patch_lvgl_scroll_uaf] ERROR: LVGL was patched after compilation")
        print("[patch_lvgl_scroll_uaf] ERROR: re-run `pio run` to compile the fix")
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
            print(
                "patch_lvgl_scroll_uaf: %s"
                % ("patched" if changed else "already patched")
            )
        except (OSError, RuntimeError) as error:
            print("patch_lvgl_scroll_uaf: ERROR: %s" % error, file=sys.stderr)
            raise SystemExit(1)
    elif len(sys.argv) == 3 and sys.argv[1] == "--verify-file":
        try:
            verify_file(sys.argv[2])
            print("patch_lvgl_scroll_uaf: verified")
        except (OSError, RuntimeError) as error:
            print("patch_lvgl_scroll_uaf: ERROR: %s" % error, file=sys.stderr)
            raise SystemExit(1)
    else:
        raise SystemExit(
            "usage: patch_lvgl_scroll_uaf.py "
            "(--self-test | --patch-file PATH | --verify-file PATH)"
        )
