# SPDX-License-Identifier: GPL-3.0-or-later

# LVGL 8.4's anim_timer() reads the animation again after calling its exec_cb:
#
#     if(a->exec_cb) a->exec_cb(a->var, new_value);
#     ...
#     if(a->act_time >= a->time) anim_ready_handler(a);
#
# exec_cb can delete that animation. A scroll animation's exec_cb sends
# LV_EVENT_SCROLL, and any handler that then scrolls the same object without
# animation (lv_obj_scroll_to_y(..., LV_ANIM_OFF)) deletes it through
# lv_anim_del(). When that step was also the animation's last one, LVGL finishes
# the freed animation: it runs the ready callback on freed memory and frees it a
# second time. By then the block usually belongs to something else, so the
# second free releases a live allocation. This is the ThinkNode M9 chat-scroll
# panic in issues #428 and #475: a later animation reused that memory and
# anim_timer jumped to 0x00020000.
#
# The loop already records any list change made during this animation's
# callbacks in anim_list_changed, and restarts from the list head when it is
# set. LVGL 9 skips the completion check in that case (the animation, if it
# still exists, completes on the next round), and this patch does the same.
#
# Idempotent and fail-closed, like patch_meshcore_txt_timestamp.py. On a fresh
# checkout pre-scripts run before PlatformIO fetches lib_deps, so the pre-link
# action patches the newly fetched source and fails that first link. Re-running
# then compiles the patched source. The IDF builds (Tanmatsu, T-Display P4)
# vendor LVGL through tanmatsu/fetch-deps.sh and apply this with --patch-file.

import os
import sys

MARKER = "wadamesh-lvgl-anim-uaf-patch"
OLD = """                /*If the time is elapsed the animation is ready*/
                if(a->act_time >= a->time) {
                    anim_ready_handler(a);
                }"""
NEW = """                /*If the time is elapsed the animation is ready.
                 *wadamesh-lvgl-anim-uaf-patch: exec_cb may have deleted `a` (a scroll
                 *handler that scrolls the same object without animation does), so do not
                 *read it when the list changed. It completes on the next round (LVGL 9).*/
                if(!anim_list_changed && a->act_time >= a->time) {
                    anim_ready_handler(a);
                }"""
REQUIRED_LINES = (
        "static bool anim_list_changed;",
        "        anim_list_changed = false;",
        "                    if(a->exec_cb) a->exec_cb(a->var, new_value);",
        "        if(anim_list_changed)",
)


def patch_source(source):
    if any(source.count(line) != 1 for line in REQUIRED_LINES):
        raise RuntimeError(
            "lv_anim.c anim_timer() does not match LVGL 8.4 (LVGL version drift?)"
        )
    old_count = source.count(OLD)
    new_count = source.count(NEW)
    marker_count = source.count(MARKER)
    if old_count == 0 and new_count == 1 and marker_count == 1:
        return source, False
    if old_count != 1 or new_count != 0 or marker_count != 0:
        raise RuntimeError(
            "lv_anim.c completion check does not match LVGL 8.4 (LVGL version drift?)"
        )
    patched = source.replace(OLD, NEW, 1)
    if patched.count(OLD) != 0 or patched.count(NEW) != 1 or patched.count(MARKER) != 1:
        raise RuntimeError("lv_anim.c patch verification failed")
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
        raise RuntimeError("lv_anim.c still finishes animations deleted by their exec_cb")


def verify_file(path):
    with open(path, encoding="utf-8") as source_file:
        verify_source(source_file.read())


def self_test():
    fixture = "\n".join(REQUIRED_LINES[:3]) + "\n" + OLD + "\n" + REQUIRED_LINES[3] + "\n"
    patched, changed = patch_source(fixture)
    assert changed
    assert MARKER in patched
    assert "if(!anim_list_changed && a->act_time >= a->time) {" in patched
    assert "                if(a->act_time >= a->time) {" not in patched

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
        patch_source("static void anim_timer(lv_timer_t * param) {}\n")
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

    print("patch_lvgl_anim_uaf self-test passed")


def install(platformio_env):
    path = os.path.join(
        platformio_env.subst("$PROJECT_LIBDEPS_DIR"),
        platformio_env.subst("$PIOENV"),
        "lvgl",
        "src",
        "misc",
        "lv_anim.c",
    )

    def apply_or_error():
        if not os.path.isfile(path):
            return "LVGL patch target is missing: %s" % path
        try:
            changed = patch_file(path)
        except (OSError, RuntimeError) as error:
            return str(error)
        print(
            "[patch_lvgl_anim_uaf] %s"
            % ("patched lv_anim.c" if changed else "already patched")
        )
        return None

    if os.path.isfile(path):
        error = apply_or_error()
        if error is not None:
            print("[patch_lvgl_anim_uaf] ERROR: %s" % error)
            platformio_env.Exit(1)
    else:
        print(
            "[patch_lvgl_anim_uaf] LVGL not fetched yet - "
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
            print("[patch_lvgl_anim_uaf] ERROR: %s" % error)
            return 1
        print("[patch_lvgl_anim_uaf] ERROR: LVGL was patched after compilation")
        print("[patch_lvgl_anim_uaf] ERROR: re-run `pio run` to compile the fix")
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
                "patch_lvgl_anim_uaf: %s"
                % ("patched" if changed else "already patched")
            )
        except (OSError, RuntimeError) as error:
            print("patch_lvgl_anim_uaf: ERROR: %s" % error, file=sys.stderr)
            raise SystemExit(1)
    elif len(sys.argv) == 3 and sys.argv[1] == "--verify-file":
        try:
            verify_file(sys.argv[2])
            print("patch_lvgl_anim_uaf: verified")
        except (OSError, RuntimeError) as error:
            print("patch_lvgl_anim_uaf: ERROR: %s" % error, file=sys.stderr)
            raise SystemExit(1)
    else:
        raise SystemExit(
            "usage: patch_lvgl_anim_uaf.py "
            "(--self-test | --patch-file PATH | --verify-file PATH)"
        )
