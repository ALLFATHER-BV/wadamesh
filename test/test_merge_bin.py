#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import runpy
import unittest
from unittest.mock import MagicMock, patch


SCRIPT = Path(__file__).resolve().parents[1] / "merge-bin.py"


class MergeBinTests(unittest.TestCase):
    def load_action(self, output=None):
        self.build_dir = "/project with spaces/board's build"
        self.app_path = self.build_dir + "/firmware.bin"
        self.images = [
            ("0x0", self.build_dir + "/bootloader.bin"),
            ("0x8000", self.build_dir + "/partitions.bin"),
            ("0xe000", "/framework with spaces/boot_app0.bin"),
        ]
        self.process_env = {"PATH": "/tools with spaces"}
        env = MagicMock()
        env.BoardConfig.return_value = {
            "build.mcu": "esp32s3",
            "build.flash_mode": "qio",
            "upload.flash_size": "16MB",
        }
        env.get.return_value = self.images
        env.Flatten.side_effect = lambda images: [value for pair in images for value in pair]
        substitutions = {
            "$PYTHONEXE": "/python with spaces/python",
            "$OBJCOPY": "/tools with spaces/esptool.py",
            "${BUILD_DIR}/${PROGNAME}-merged.bin": self.build_dir + "/firmware-merged.bin",
            "${__get_board_f_flash(__env__)}": "80m",
            "$ESP32_APP_OFFSET": "0x10000",
        }
        env.subst.side_effect = lambda value: substitutions.get(value, value)
        env.__getitem__.return_value = self.process_env
        environ = {} if output is None else {"MERGED_BIN_PATH": output}
        with patch.dict(os.environ, environ, clear=True):
            script = runpy.run_path(str(SCRIPT), init_globals={
                "Import": lambda *args: None,
                "env": env,
                "projenv": env,
            })
        source = MagicMock()
        source.get_abspath.return_value = self.app_path
        return script["merge_bin_action"], [source], env

    @patch("subprocess.run")
    def test_paths_and_flash_offsets_are_preserved(self, run):
        action, source, env = self.load_action()
        run.return_value.returncode = 0
        self.assertEqual(action(source, [], env), 0)
        run.assert_called_once_with([
            "/python with spaces/python",
            "/tools with spaces/esptool.py",
            "--chip", "esp32s3",
            "merge_bin",
            "-o", self.build_dir + "/firmware-merged.bin",
            "--flash_mode", "qio",
            "--flash_freq", "80m",
            "--flash_size", "16MB",
            *[value for pair in self.images for value in pair],
            "0x10000", self.app_path,
        ], env=self.process_env)

    @patch("subprocess.run")
    def test_output_override_is_preserved(self, run):
        output = "/custom output/owner's merged image.bin"
        action, source, env = self.load_action(output)
        run.return_value.returncode = 0
        self.assertEqual(action(source, [], env), 0)
        args = run.call_args.args[0]
        self.assertEqual(args[args.index("-o") + 1], output)

    @patch("subprocess.run")
    def test_merge_failure_is_returned(self, run):
        action, source, env = self.load_action()
        run.return_value.returncode = 2
        self.assertEqual(action(source, [], env), 2)


if __name__ == "__main__":
    unittest.main()
