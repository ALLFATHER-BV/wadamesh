# SPDX-License-Identifier: GPL-3.0-or-later

# MeshCore core-v1.17.4 seeds _txt_last_ts from an unrestricted random uint32.
# BaseChatMesh::sendMessage() then raises the real RTC timestamp above that
# value and writes it into the encrypted TXT payload. On current dates this
# produces a future timestamp on most boots (issue #374).
#
# Keep the real clock-derived timestamp and its within-boot monotonic ratchet.
# The other randomized fields remain unchanged. This patch is temporary until
# the same fix is available in a tagged meshcomod core dependency.
#
# Idempotent and fail-closed. On a fresh checkout, pre-scripts run before
# PlatformIO fetches lib_deps, so the pre-link action patches the newly fetched
# source and fails that first link. Re-running then compiles the patched source;
# no firmware artifact can be linked from the known-bad source.

import os
import sys

MARKER = "wadamesh-real-txt-timestamp-patch"
OLD = """  g->random(w, 4);
  _txt_last_ts = (uint32_t)w[0] | ((uint32_t)w[1] << 8) | ((uint32_t)w[2] << 16) | ((uint32_t)w[3] << 24);"""
NEW = """  // wadamesh-real-txt-timestamp-patch: this field is a semantic Unix
  // timestamp, not a nonce. sendMessage() seeds it from the validated RTC on
  // first use and ratchets it only for later sends in this boot.
  _txt_last_ts = 0;"""
REQUIRED_SEND_LINES = (
        "  uint32_t uniq_ts = getRTCClock()->getCurrentTimeUnique();",
        "  if (uniq_ts <= _txt_last_ts) uniq_ts = _txt_last_ts + 1;",
        "  _txt_last_ts = uniq_ts;",
        "composeMsgPacket(recipient, uniq_ts, uniq_attempt, text",
)


def patch_source(source):
    if any(source.count(line) != 1 for line in REQUIRED_SEND_LINES):
        raise RuntimeError(
            "BaseChatMesh direct-message timestamp path does not match core-v1.17.4 "
            "(MeshCore version drift?)"
        )
    old_count = source.count(OLD)
    new_count = source.count(NEW)
    marker_count = source.count(MARKER)
    if old_count == 0 and new_count == 1 and marker_count == 1:
        return source, False
    if old_count != 1 or new_count != 0 or marker_count != 0:
        raise RuntimeError(
            "BaseChatMesh timestamp initialization does not match core-v1.17.4 "
            "(MeshCore version drift?)"
        )
    patched = source.replace(OLD, NEW, 1)
    if patched.count(OLD) != 0 or patched.count(NEW) != 1 or patched.count(MARKER) != 1:
        raise RuntimeError("BaseChatMesh timestamp patch verification failed")
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
        raise RuntimeError("BaseChatMesh contains the unpatched random timestamp seed")


def verify_file(path):
    with open(path, encoding="utf-8") as source_file:
        verify_source(source_file.read())


def self_test():
    fixture = "before\n" + OLD + "\n" + "\n".join(REQUIRED_SEND_LINES) + "\nafter\n"
    patched, changed = patch_source(fixture)
    assert changed
    assert MARKER in patched
    assert "_txt_last_ts = 0;" in patched
    assert "g->random(w, 4);" not in patched

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
        patch_source("void initTxtTxUniquenessFromRng() {}\n")
    except RuntimeError:
        pass
    else:
        raise AssertionError("version drift must fail closed")

    for invalid in (fixture + OLD, patched + OLD, "// " + MARKER + "\n" + fixture):
        try:
            patch_source(invalid)
        except RuntimeError:
            pass
        else:
            raise AssertionError("ambiguous or poisoned patch state must fail closed")

    print("patch_meshcore_txt_timestamp self-test passed")


def install(platformio_env):
    path = os.path.join(
        platformio_env.subst("$PROJECT_LIBDEPS_DIR"),
        platformio_env.subst("$PIOENV"),
        "MeshCore",
        "src",
        "helpers",
        "BaseChatMesh.cpp",
    )

    def apply_or_error():
        if not os.path.isfile(path):
            return "MeshCore patch target is missing: %s" % path
        try:
            changed = patch_file(path)
        except (OSError, RuntimeError) as error:
            return str(error)
        print(
            "[patch_meshcore_txt_timestamp] %s"
            % ("patched BaseChatMesh.cpp" if changed else "already patched")
        )
        return None

    if os.path.isfile(path):
        error = apply_or_error()
        if error is not None:
            print("[patch_meshcore_txt_timestamp] ERROR: %s" % error)
            platformio_env.Exit(1)
    else:
        print(
            "[patch_meshcore_txt_timestamp] MeshCore not fetched yet - "
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
            print("[patch_meshcore_txt_timestamp] ERROR: %s" % error)
            return 1
        print("[patch_meshcore_txt_timestamp] ERROR: MeshCore was patched after compilation")
        print("[patch_meshcore_txt_timestamp] ERROR: re-run `pio run` to compile the fix")
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
                "patch_meshcore_txt_timestamp: %s"
                % ("patched" if changed else "already patched")
            )
        except (OSError, RuntimeError) as error:
            print("patch_meshcore_txt_timestamp: ERROR: %s" % error, file=sys.stderr)
            raise SystemExit(1)
    elif len(sys.argv) == 3 and sys.argv[1] == "--verify-file":
        try:
            verify_file(sys.argv[2])
            print("patch_meshcore_txt_timestamp: verified")
        except (OSError, RuntimeError) as error:
            print("patch_meshcore_txt_timestamp: ERROR: %s" % error, file=sys.stderr)
            raise SystemExit(1)
    else:
        raise SystemExit(
            "usage: patch_meshcore_txt_timestamp.py "
            "(--self-test | --patch-file PATH | --verify-file PATH)"
        )
