#!/usr/bin/python3

# Post-link: copy app .bin to out/<PIOENV>.bin and a timestamped copy; optional merge_bin target for merged flash image.

import os
import shutil
import subprocess
from datetime import datetime

Import("env", "projenv")

board_config = env.BoardConfig()
firmware_bin = "${BUILD_DIR}/${PROGNAME}.bin"
merged_bin = os.environ.get("MERGED_BIN_PATH", "${BUILD_DIR}/${PROGNAME}-merged.bin")


def _git_short_sha(project_dir):
    try:
        r = subprocess.run(
            ["git", "-C", project_dir, "rev-parse", "--short", "HEAD"],
            capture_output=True,
            text=True,
            timeout=8,
        )
        sha = (r.stdout or "").strip()
        if r.returncode != 0 or not sha:
            return "nogit"
        dirty = subprocess.run(
            ["git", "-C", project_dir, "diff", "--quiet"],
            timeout=8,
            capture_output=True,
        )
        return sha + ("-dirty" if dirty.returncode != 0 else "")
    except (OSError, subprocess.TimeoutExpired):
        return "nogit"


def _copy_one(src_path, dst_path):
    shutil.copy2(src_path, dst_path)
    print("Copied %s -> %s" % (src_path, dst_path))


def _copy_to_out(env, src_path, out_suffix):
    if not src_path or not os.path.isfile(src_path):
        return
    pioenv = env["PIOENV"]
    project_dir = env["PROJECT_DIR"]
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    sha = _git_short_sha(project_dir)
    stable_name = pioenv + out_suffix + ".bin"
    stamped_name = "%s%s-%s-%s.bin" % (pioenv, out_suffix, stamp, sha)

    out_dir = os.path.join(project_dir, "out")
    os.makedirs(out_dir, exist_ok=True)
    _copy_one(src_path, os.path.join(out_dir, stable_name))
    _copy_one(src_path, os.path.join(out_dir, stamped_name))

    # When the project lives in a subfolder (e.g. HeltecV4/MeshCore), also copy to <workspace>/out
    parent_out = os.path.normpath(os.path.join(project_dir, "..", "out"))
    if parent_out != os.path.normpath(out_dir):
        try:
            os.makedirs(parent_out, exist_ok=True)
            _copy_one(src_path, os.path.join(parent_out, stable_name))
            _copy_one(src_path, os.path.join(parent_out, stamped_name))
        except OSError as exc:
            print("Note: could not mirror to parent out/: %s" % exc)


def copy_app_bin_to_out(target, source, env):
    bin_path = os.path.join(env.subst("$BUILD_DIR"), env.subst("${PROGNAME}") + ".bin")
    _copy_to_out(env, bin_path, "")


def merge_bin_action(source, target, env):
    flash_images = [
        *env.Flatten(env.get("FLASH_EXTRA_IMAGES", [])),
        "$ESP32_APP_OFFSET",
        source[0].get_abspath(),
    ]
    merge_cmd = [
        "$PYTHONEXE",
        "$OBJCOPY",
        "--chip",
        board_config.get("build.mcu", "esp32"),
        "merge_bin",
        "-o",
        merged_bin,
        "--flash_mode",
        board_config.get("build.flash_mode", "dio"),
        "--flash_freq",
        "${__get_board_f_flash(__env__)}",
        "--flash_size",
        board_config.get("upload.flash_size", "4MB"),
        *flash_images,
    ]
    merge_args = [env.subst(str(arg)) for arg in merge_cmd]
    return subprocess.run(merge_args, env=env["ENV"]).returncode


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_app_bin_to_out)

env.AddCustomTarget(
    name="mergebin",
    dependencies=firmware_bin,
    actions=merge_bin_action,
    title="Merge binary",
    description="Build combined image",
    always_build=True,
)