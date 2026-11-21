# SPDX-License-Identifier: GPL-3.0-or-later

# LVGL 8.4 never cancels a pending lv_obj_del_async(). When the object is freed
# before the queued call runs - its parent is deleted, a synchronous
# lv_obj_clean() takes it, or a second lv_obj_del_async() for the same object
# ran first - lv_obj_del_async_cb() later calls lv_obj_del() on freed memory.
# The chat divider crash noted at chatVirtReset in UITask.cpp was this, fixed
# there by not queueing; 15 other lv_obj_del_async() calls, popupClose() among
# them, still depend on nothing freeing the object first.
#
# LVGL 9 cancels them in obj_delete_core(), and this patch does the same in two
# halves that only work together:
#   lv_obj_tree.c  obj_del_core() cancels the object's pending async deletes;
#   lv_async.c     lv_async_timer_cb() frees its info and deletes its timer
#                  BEFORE the call (LVGL 9 order). The queued delete that runs
#                  deletes its object, which now cancels that same call; with
#                  the stock order the info would be freed twice.
# So --patch-root patches lv_async.c first, and verification needs both.
#
# Idempotent and fail-closed, like patch_lvgl_anim_uaf.py. On a fresh checkout
# pre-scripts run before PlatformIO fetches lib_deps, so the pre-link action
# patches the newly fetched source and fails that first link. Re-running then
# compiles the patched source. The IDF builds (Tanmatsu, T-Display P4, CrowPanel
# P4) and the web build vendor LVGL and apply this with --patch-root.

import os
import sys

MARKER = "wadamesh-lvgl-async-del-patch"

ASYNC = {
    "path": os.path.join("src", "misc", "lv_async.c"),
    "required": (
        "lv_res_t lv_async_call_cancel(lv_async_cb_t async_xcb, void * user_data)\n{",
        "    lv_timer_set_repeat_count(timer, 1);",
    ),
    "old": """static void lv_async_timer_cb(lv_timer_t * timer)
{
    lv_async_info_t * info = (lv_async_info_t *)timer->user_data;

    info->cb(info->user_data);
    lv_mem_free(info);
}""",
    "new": """static void lv_async_timer_cb(lv_timer_t * timer)
{
    /*wadamesh-lvgl-async-del-patch: free the info and delete the timer BEFORE the call
     *(LVGL 9 order). The callback may cancel this very call: lv_obj_del_async_cb deletes
     *its object, and deleting an object cancels the object's pending async deletes.*/
    lv_async_info_t info = *(lv_async_info_t *)timer->user_data;
    lv_mem_free(timer->user_data);
    lv_timer_del(timer);
    info.cb(info.user_data);
}""",
}

OBJ_TREE = {
    "path": os.path.join("src", "core", "lv_obj_tree.c"),
    "required": (
        '#include "../misc/lv_async.h"',
        "static void lv_obj_del_async_cb(void * obj);",
        "static void obj_del_core(lv_obj_t * obj)\n{",
    ),
    "old": """    /*All children deleted. Now clean up the object specific data*/
    _lv_obj_destruct(obj);""",
    "new": """    /*wadamesh-lvgl-async-del-patch: cancel the object's pending lv_obj_del_async calls
     *(LVGL 9 does). One queued before the object was freed - by its parent's delete, a
     *synchronous clean, or a second lv_obj_del_async - would delete freed memory later.
     *Needs the lv_async.c half: the queued delete that runs now cancels itself.*/
    lv_async_call_cancel(lv_obj_del_async_cb, obj);

    /*All children deleted. Now clean up the object specific data*/
    _lv_obj_destruct(obj);""",
}

HALVES = (ASYNC, OBJ_TREE)   # lv_async.c first: the lv_obj_tree.c half alone double-frees


def patch_source(half, source):
    name = os.path.basename(half["path"])
    if any(source.count(line) != 1 for line in half["required"]):
        raise RuntimeError("%s does not match LVGL 8.4 (LVGL version drift?)" % name)
    # The lv_obj_tree.c half inserts before its anchor, so "new" contains "old".
    old_in_new = half["new"].count(half["old"])
    old_count = source.count(half["old"])
    new_count = source.count(half["new"])
    marker_count = source.count(MARKER)
    if old_count == old_in_new and new_count == 1 and marker_count == 1:
        return source, False
    if old_count != 1 or new_count != 0 or marker_count != 0:
        raise RuntimeError("%s patch site does not match LVGL 8.4 (LVGL version drift?)" % name)
    patched = source.replace(half["old"], half["new"], 1)
    if (patched.count(half["old"]) != old_in_new or patched.count(half["new"]) != 1 or
            patched.count(MARKER) != 1):
        raise RuntimeError("%s patch verification failed" % name)
    return patched, True


def patch_root(root):
    """Patch both halves under an LVGL checkout; returns the files changed."""
    changed = []
    for half in HALVES:
        path = os.path.join(root, half["path"])
        with open(path, encoding="utf-8") as source_file:
            source = source_file.read()
        patched, did = patch_source(half, source)
        if did:
            with open(path, "w", encoding="utf-8") as source_file:
                source_file.write(patched)
            changed.append(os.path.basename(path))
    return changed


def verify_root(root):
    for half in HALVES:
        path = os.path.join(root, half["path"])
        with open(path, encoding="utf-8") as source_file:
            _, did = patch_source(half, source_file.read())
        if did:
            raise RuntimeError(
                "%s still lacks the pending async delete fix" % os.path.basename(path)
            )


def self_test():
    for half in HALVES:
        fixture = "\n".join(half["required"]) + "\n" + half["old"] + "\n"
        patched, changed = patch_source(half, fixture)
        assert changed
        assert MARKER in patched

        same, changed = patch_source(half, patched)
        assert not changed
        assert same == patched

        try:
            patch_source(half, half["old"] + "\n")
        except RuntimeError:
            pass
        else:
            raise AssertionError("version drift must fail closed")

        for invalid in (fixture + half["old"], patched + half["old"],
                        "/* " + MARKER + " */\n" + fixture):
            try:
                patch_source(half, invalid)
            except RuntimeError:
                pass
            else:
                raise AssertionError("ambiguous or poisoned patch state must fail closed")

    assert "lv_async_call_cancel(lv_obj_del_async_cb, obj);" in OBJ_TREE["new"]
    assert HALVES[0] is ASYNC
    print("patch_lvgl_async_del self-test passed")


def install(platformio_env):
    root = os.path.join(
        platformio_env.subst("$PROJECT_LIBDEPS_DIR"),
        platformio_env.subst("$PIOENV"),
        "lvgl",
    )
    targets = [os.path.join(root, half["path"]) for half in HALVES]

    def apply_or_error():
        missing = [path for path in targets if not os.path.isfile(path)]
        if missing:
            return "LVGL patch target is missing: %s" % missing[0]
        try:
            changed = patch_root(root)
        except (OSError, RuntimeError) as error:
            return str(error)
        print(
            "[patch_lvgl_async_del] %s"
            % ("patched " + ", ".join(changed) if changed else "already patched")
        )
        return None

    if all(os.path.isfile(path) for path in targets):
        error = apply_or_error()
        if error is not None:
            print("[patch_lvgl_async_del] ERROR: %s" % error)
            platformio_env.Exit(1)
    else:
        print(
            "[patch_lvgl_async_del] LVGL not fetched yet - "
            "patch deferred to pre-link check"
        )

    def verify_patched(target, source, env):
        del target, source
        try:
            verify_root(root)
            return 0
        except (OSError, RuntimeError):
            pass
        error = apply_or_error()
        if error is not None:
            print("[patch_lvgl_async_del] ERROR: %s" % error)
            return 1
        print("[patch_lvgl_async_del] ERROR: LVGL was patched after compilation")
        print("[patch_lvgl_async_del] ERROR: re-run `pio run` to compile the fix")
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
    elif len(sys.argv) == 3 and sys.argv[1] == "--patch-root":
        try:
            changed = patch_root(sys.argv[2])
            print(
                "patch_lvgl_async_del: %s"
                % ("patched " + ", ".join(changed) if changed else "already patched")
            )
        except (OSError, RuntimeError) as error:
            print("patch_lvgl_async_del: ERROR: %s" % error, file=sys.stderr)
            raise SystemExit(1)
    elif len(sys.argv) == 3 and sys.argv[1] == "--verify-root":
        try:
            verify_root(sys.argv[2])
            print("patch_lvgl_async_del: verified")
        except (OSError, RuntimeError) as error:
            print("patch_lvgl_async_del: ERROR: %s" % error, file=sys.stderr)
            raise SystemExit(1)
    else:
        raise SystemExit(
            "usage: patch_lvgl_async_del.py "
            "(--self-test | --patch-root LVGL_DIR | --verify-root LVGL_DIR)"
        )
