# SPDX-License-Identifier: GPL-3.0-or-later

# Issue #927: "Heard by N repeaters" often stays at "Sent" for short messages.
#
# After a transmit completes, MeshCore core-v1.18.0 leaves the radio in standby
# until the main loop next reaches Dispatcher::loop() (isSendComplete ->
# onSendFinished -> recvRaw/startRecv). On the touch UI that loop also renders
# LVGL, and the frame right after a send (new bubble, alert) can run longer than
# a repeater's re-flood delay, which is 0-4 x half the packet's airtime: tens of
# milliseconds for a short message. The repeater's echo is on air before we are
# listening, so it is never counted.
#
# Add RadioLibWrapper::rearmRxAfterTx(): when the TX-done flag is up, finish the
# transmit and re-enter RX at once, and remember that so the dispatcher's later
# isSendComplete() still reports the send and onSendFinished() does not knock
# the radio back to standby. The app calls it from the main task (display flush)
# so it never races the main loop. This patch is temporary until the same change
# is available in a tagged meshcomod core dependency.
#
# Idempotent and fail-closed, like patch_meshcore_txt_timestamp.py: on a fresh
# checkout the pre-link action patches the newly fetched source and fails that
# first link; re-running compiles the patched source.

import os
import sys

MARKER = "wadamesh-rx-rearm-after-tx-patch"

HEADER_EDITS = (
    (
        "  void onSendFinished() override;\n",
        "  void onSendFinished() override;\n"
        "  // wadamesh-rx-rearm-after-tx-patch: if a transmit has completed, finish it\n"
        "  // and re-enter RX now instead of at the dispatcher's next loop pass. Call\n"
        "  // from the task that runs the mesh loop. Returns true if it re-armed RX.\n"
        "  bool rearmRxAfterTx();\n",
    ),
)

SOURCE_EDITS = (
    (
        "static volatile uint32_t rx_evt_count = 0;\n",
        "static volatile uint32_t rx_evt_count = 0;\n"
        "\n"
        "// wadamesh-rx-rearm-after-tx-patch: 1 = rearmRxAfterTx() finished the send and\n"
        "// re-armed RX, isSendComplete() still owes the dispatcher its TX-done; 2 = that\n"
        "// was reported, onSendFinished() must leave the radio in RX.\n"
        "static volatile uint8_t tx_rearmed = 0;\n",
    ),
    (
        "  int err = _radio->startTransmit((uint8_t *) bytes, len);\n"
        "  if (err == RADIOLIB_ERR_NONE) {\n"
        "    state = STATE_TX_WAIT;\n",
        "  tx_rearmed = 0;\n"
        "  int err = _radio->startTransmit((uint8_t *) bytes, len);\n"
        "  if (err == RADIOLIB_ERR_NONE) {\n"
        "    state = STATE_TX_WAIT;\n",
    ),
    (
        "bool RadioLibWrapper::isSendComplete() {\n"
        "  if (state & STATE_INT_READY) {\n",
        "bool RadioLibWrapper::isSendComplete() {\n"
        "  if (tx_rearmed == 1) {   // already finished by rearmRxAfterTx(); RX is armed\n"
        "    tx_rearmed = 2;\n"
        "    n_sent++;\n"
        "    return true;\n"
        "  }\n"
        "  if (state & STATE_INT_READY) {\n",
    ),
    (
        "void RadioLibWrapper::onSendFinished() {\n"
        "  rlwLock();\n"
        "  _radio->finishTransmit();\n"
        "  _board->onAfterTransmit();\n"
        "  state = STATE_IDLE;\n"
        "  rlwUnlock();\n"
        "}\n",
        "void RadioLibWrapper::onSendFinished() {\n"
        "  rlwLock();\n"
        "  if (tx_rearmed) {   // finished early; standby now would cut off a reception\n"
        "    tx_rearmed = 0;\n"
        "    rlwUnlock();\n"
        "    return;\n"
        "  }\n"
        "  _radio->finishTransmit();\n"
        "  _board->onAfterTransmit();\n"
        "  state = STATE_IDLE;\n"
        "  rlwUnlock();\n"
        "}\n"
        "\n"
        "bool RadioLibWrapper::rearmRxAfterTx() {\n"
        "  if (tx_rearmed || state != (STATE_TX_WAIT | STATE_INT_READY)) return false;\n"
        "#if defined(ESP32)\n"
        "  if (rxq_suspend) return false;   // the spectrum analyzer owns the radio\n"
        "#endif\n"
        "  rlwLock();\n"
        "  bool done = false;\n"
        "  if (!tx_rearmed && state == (STATE_TX_WAIT | STATE_INT_READY)) {\n"
        "    _radio->finishTransmit();\n"
        "    _board->onAfterTransmit();\n"
        "    state = STATE_IDLE;\n"
        "    tx_rearmed = 1;\n"
        "    startRecv();\n"
        "    done = true;\n"
        "  }\n"
        "  rlwUnlock();\n"
        "  return done;\n"
        "}\n",
    ),
)


def apply_edits(source, edits, what):
    marker_count = source.count(MARKER)
    if marker_count == 0:
        for old, new in edits:
            if source.count(old) != 1 or source.count(new) != 0:
                raise RuntimeError(
                    "%s does not match core-v1.18.0 (MeshCore version drift?)" % what
                )
        patched = source
        for old, new in edits:
            patched = patched.replace(old, new, 1)
        for _, new in edits:
            if patched.count(new) != 1:
                raise RuntimeError("%s patch verification failed" % what)
        return patched, True
    if all(source.count(new) == 1 for _, new in edits) and marker_count == 1:
        return source, False
    raise RuntimeError("%s is partially patched (MeshCore version drift?)" % what)


def patch_header(source):
    return apply_edits(source, HEADER_EDITS, "RadioLibWrappers.h")


def patch_source(source):
    return apply_edits(source, SOURCE_EDITS, "RadioLibWrappers.cpp")


PATCHERS = (("RadioLibWrappers.h", patch_header), ("RadioLibWrappers.cpp", patch_source))


def patch_dir(radiolib_dir):
    changed_any = False
    for name, patcher in PATCHERS:
        path = os.path.join(radiolib_dir, name)
        with open(path, encoding="utf-8") as f:
            source = f.read()
        patched, changed = patcher(source)
        if changed:
            with open(path, "w", encoding="utf-8") as f:
                f.write(patched)
            changed_any = True
    return changed_any


def verify_dir(radiolib_dir):
    for name, patcher in PATCHERS:
        with open(os.path.join(radiolib_dir, name), encoding="utf-8") as f:
            _, changed = patcher(f.read())
        if changed:
            raise RuntimeError("%s is unpatched" % name)


def self_test():
    for patcher, edits in ((patch_header, HEADER_EDITS), (patch_source, SOURCE_EDITS)):
        fixture = "before\n" + "middle\n".join(old for old, _ in edits) + "after\n"
        patched, changed = patcher(fixture)
        assert changed
        assert patched.count(MARKER) == 1
        same, changed = patcher(patched)
        assert not changed and same == patched

        for invalid in ("unrelated\n", fixture + edits[0][0], "// " + MARKER + "\n" + fixture):
            try:
                patcher(invalid)
            except RuntimeError:
                pass
            else:
                raise AssertionError("drifted or poisoned source must fail closed")

    patched, _ = patch_source("".join(old for old, _ in SOURCE_EDITS))
    assert "tx_rearmed = 0;\n  int err = _radio->startTransmit" in patched
    assert "bool RadioLibWrapper::rearmRxAfterTx() {" in patched
    print("patch_meshcore_rx_rearm self-test passed")


def install(platformio_env):
    radiolib_dir = os.path.join(
        platformio_env.subst("$PROJECT_LIBDEPS_DIR"),
        platformio_env.subst("$PIOENV"),
        "MeshCore",
        "src",
        "helpers",
        "radiolib",
    )
    present = os.path.isfile(os.path.join(radiolib_dir, "RadioLibWrappers.cpp"))

    def apply_or_error():
        if not os.path.isfile(os.path.join(radiolib_dir, "RadioLibWrappers.cpp")):
            return "MeshCore patch target is missing: %s" % radiolib_dir
        try:
            changed = patch_dir(radiolib_dir)
        except (OSError, RuntimeError) as error:
            return str(error)
        print(
            "[patch_meshcore_rx_rearm] %s"
            % ("patched RadioLibWrappers" if changed else "already patched")
        )
        return None

    if present:
        error = apply_or_error()
        if error is not None:
            print("[patch_meshcore_rx_rearm] ERROR: %s" % error)
            platformio_env.Exit(1)
    else:
        print("[patch_meshcore_rx_rearm] MeshCore not fetched yet - patch deferred to pre-link check")

    def verify_patched(target, source, env):
        del target, source, env
        try:
            verify_dir(radiolib_dir)
            return 0
        except (OSError, RuntimeError):
            pass
        error = apply_or_error()
        if error is not None:
            print("[patch_meshcore_rx_rearm] ERROR: %s" % error)
            return 1
        print("[patch_meshcore_rx_rearm] ERROR: MeshCore was patched after compilation")
        print("[patch_meshcore_rx_rearm] ERROR: re-run `pio run` to compile the fix")
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
    elif len(sys.argv) == 3 and sys.argv[1] in ("--patch-dir", "--verify-dir"):
        try:
            if sys.argv[1] == "--patch-dir":
                print("patch_meshcore_rx_rearm: %s"
                      % ("patched" if patch_dir(sys.argv[2]) else "already patched"))
            else:
                verify_dir(sys.argv[2])
                print("patch_meshcore_rx_rearm: verified")
        except (OSError, RuntimeError) as error:
            print("patch_meshcore_rx_rearm: ERROR: %s" % error, file=sys.stderr)
            raise SystemExit(1)
    else:
        raise SystemExit(
            "usage: patch_meshcore_rx_rearm.py (--self-test | --patch-dir DIR | --verify-dir DIR)"
        )
