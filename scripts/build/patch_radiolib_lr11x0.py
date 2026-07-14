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
# NOTE: on a completely fresh checkout the first build may run before
# RadioLib is downloaded - the script then warns and the patch lands on the
# next build. Re-run `pio run` once if you see the warning.
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

if not os.path.isfile(path):
    print("[patch_radiolib_lr11x0] WARNING: %s not found (libdeps not fetched yet?)" % path)
    print("[patch_radiolib_lr11x0] WARNING: RadioLib NOT patched - re-run the build once libdeps exist")
else:
    with open(path) as f:
        src = f.read()
    if MARKER in src:
        print("[patch_radiolib_lr11x0] already patched")
    elif OLD in src:
        with open(path, "w") as f:
            f.write(src.replace(OLD, NEW, 1))
        print("[patch_radiolib_lr11x0] patched LR11x0::config() for old-FW tolerance")
    else:
        print("[patch_radiolib_lr11x0] WARNING: pattern not found - RadioLib version drift?")
        print("[patch_radiolib_lr11x0] WARNING: check LR11x0::config() by hand, NOT patched")
