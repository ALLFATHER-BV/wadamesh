# Patches the vendored RadioLib so LR11x0::config() tolerates old LR1110
# transceiver firmware that predates DriveDiosInSleepMode (opcode 0x012A,
# added in the 0x0308 firmware release). RadioLib >= 7.x sends it
# unconditionally during begin(); older chip firmware answers CMD_PERR,
# which turns an otherwise healthy radio.begin() into -706
# (RADIOLIB_ERR_SPI_CMD_INVALID). Preproduction Elecrow ThinkNode M9 units
# ship with such firmware (Meshtastic works on them only because it pins an
# older RadioLib that doesn't send the command).
#
# The command is an optional nicety (stops DIO glitches in sleep mode), so
# skipping it on old firmware is safe.
#
# Idempotent. Each PlatformIO env has its own libdeps copy, so this only
# affects envs that list this script in extra_scripts (the M9 env).
#
# Fail-closed: an unpatched binary boots to a fatal -706 radio-init screen on
# preprod units, indistinguishable from a good build except by its build log —
# so any state where the patch can't land ABORTS the build instead of warning.
# The one soft case is a completely fresh checkout: pre-scripts run before the
# LDF fetches lib_deps, so there is nothing to patch yet, and aborting here
# would stop the build before RadioLib can even be downloaded. For that case a
# pre-link check (below) patches late and fails the link, so the NEXT `pio run`
# compiles the patched source — no linked binary can ever ship unpatched.
Import("env")
import os

MARKER = "wadamesh-lr1110-oldfw-patch"
OLD = """  state = this->driveDiosInSleepMode(true);
  RADIOLIB_ASSERT(state);"""
NEW = """  state = this->driveDiosInSleepMode(true);
  // wadamesh-lr1110-oldfw-patch: LR1110 transceiver FW older than 0x0308
  // doesn't implement DriveDiosInSleepMode (0x012A) and answers CMD_PERR,
  // which would abort init (-706) on an otherwise healthy radio. The command
  // only stops DIO glitches in sleep mode - safe to skip on old firmware.
  if(state == RADIOLIB_ERR_SPI_CMD_INVALID) {
    RADIOLIB_DEBUG_BASIC_PRINTLN("DriveDiosInSleepMode unsupported (old LR11x0 FW), skipping");
    state = RADIOLIB_ERR_NONE;
  }
  RADIOLIB_ASSERT(state);"""

path = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"),
                    "RadioLib", "src", "modules", "LR11x0", "LR11x0.cpp")


def apply_patch():
    """Patch LR11x0.cpp in place. Returns an error string, or None on success
    (including already-patched)."""
    with open(path) as f:
        src = f.read()
    if MARKER in src:
        print("[patch_radiolib_lr11x0] already patched")
        return None
    if OLD not in src:
        # lib_deps uses a caret range (^7.6.0), so a `pio pkg update` can pull
        # a RadioLib whose config() no longer matches the expected shape.
        return ("LR11x0::config() doesn't match the expected shape (RadioLib "
                "version drift?) - port the old-FW patch by hand, or pin "
                "lib_deps back to a known-good version")
    with open(path, "w") as f:
        f.write(src.replace(OLD, NEW, 1))
    print("[patch_radiolib_lr11x0] patched LR11x0::config() for old-FW tolerance")
    return None


if os.path.isfile(path):
    error = apply_patch()
    if error is not None:
        print("[patch_radiolib_lr11x0] ERROR: %s" % error)
        env.Exit(1)
else:
    # Fresh checkout: RadioLib isn't fetched yet. Let the build proceed so the
    # LDF can download it; the pre-link check below is what keeps this run from
    # producing an unpatched binary.
    print("[patch_radiolib_lr11x0] RadioLib not fetched yet - patch deferred to pre-link check")


def verify_patched(target, source, env):
    # Runs right before the link, after libdeps exist and every RadioLib object
    # has been compiled. A missing marker here means those objects came from
    # UNPATCHED source: patch now (SCons's content signatures then recompile
    # LR11x0.cpp on the next run) and fail this link. Non-zero return = SCons
    # build failure, so no .elf/.bin is produced.
    if not os.path.isfile(path):
        print("[patch_radiolib_lr11x0] ERROR: %s still missing at link time" % path)
        return 1
    with open(path) as f:
        if MARKER in f.read():
            return 0
    error = apply_patch()
    if error is not None:
        print("[patch_radiolib_lr11x0] ERROR: %s" % error)
        return 1
    print("[patch_radiolib_lr11x0] ERROR: RadioLib was fetched during this build and is")
    print("[patch_radiolib_lr11x0] ERROR: now patched - re-run `pio run` to compile it in")
    return 1


env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", verify_patched)
