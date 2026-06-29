"""
SCons SPAWN override for Windows — two-step compilation workaround.

Root cause:
  xtensa-esp32s3-elf-g++ internally spawns cc1plus via Windows CreateProcess.
  With the full ESP32 SDK include list, the cc1plus command line approaches or
  exceeds Windows' 32767-char CreateProcess limit, causing the spawn to fail.

Fix (compile path):
  When SCons calls SPAWN for a g++/gcc compile command (-c flag), we split the
  compilation into two explicit subprocess calls so GCC never needs to spawn a
  grandchild process:
    1. g++ -S  → produces a .s assembly file in a temp location
    2. xtensa-esp32s3-elf-as → assembles the .s into a .o object

  The assembler command line is short (no include paths), so it never hits limits.
  Any other SCons command (link, archive, …) is passed through unchanged.

Only active on Windows; no-ops silently on Linux/macOS.
"""

import subprocess
import shutil
import shlex
import tempfile
import sys
import os
import datetime

Import("env")

if sys.platform != "win32":
    Return()

_PIO_BUILD_DIR = os.path.join(env["PROJECT_DIR"], ".pio", "build")
_SPAWN_LOG = os.path.join(_PIO_BUILD_DIR, "spawn_debug.log")
os.makedirs(_PIO_BUILD_DIR, exist_ok=True)

# Resolve the assembler path once at load time
_SCONS_PATH = env.get("PATH", "")
if isinstance(_SCONS_PATH, (list, tuple)):
    _SCONS_PATH = os.pathsep.join(str(p) for p in _SCONS_PATH)


def _build_env():
    """subprocess env: real Windows env + toolchain PATH prepended + TEMP redirected."""
    merged = dict(os.environ)
    merged["PATH"] = _SCONS_PATH + os.pathsep + os.environ.get("PATH", "")
    merged["TEMP"] = _PIO_BUILD_DIR
    merged["TMP"] = _PIO_BUILD_DIR
    return merged


def _resolve(exe):
    """Return absolute path of *exe* using the full merged PATH."""
    # Strip surrounding quotes that SCons sometimes leaves in the command
    exe = exe.strip('"').strip("'")
    full_path = _SCONS_PATH + os.pathsep + os.environ.get("PATH", "")
    if os.path.isabs(exe):
        return exe
    found = shutil.which(exe, path=full_path)
    return found if found else exe


def _expand_args(args):
    """Inline-expand any @response-file arguments into a flat list of strings.

    Handles both @path and @"path" (quoted path after @).
    """
    result = []
    for a in args:
        s = str(a)
        if s.startswith("@"):
            path = s[1:].strip('"').strip("'")
            if os.path.isfile(path):
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    content = f.read()
                result.extend(shlex.split(content, posix=True))
                continue
        result.append(s)
    return result


def _two_step_compile(exe, expanded_args, merged_env):
    """
    Replace a single 'g++ -c' invocation with:
      1. g++ -S  → .s assembly file in _PIO_BUILD_DIR
      2. xtensa-esp32s3-elf-as → .o object file

    Returns the process return code.
    """
    # Find the output (-o) and source file (last .cpp/.c/.S arg)
    out_obj = None
    source_file = None
    args_iter = list(expanded_args)
    for i, a in enumerate(args_iter):
        if a == "-o" and i + 1 < len(args_iter):
            out_obj = args_iter[i + 1]
        elif (a.endswith(".cpp") or a.endswith(".c") or a.endswith(".S")
              or a.endswith(".sx") or a.endswith(".cc")):
            source_file = a

    if out_obj is None or source_file is None:
        sys.stderr.write(f"win_spawn_fix: cannot parse compile args, falling back to shell\n")
        return 1  # signal failure — single-step g++ would also fail on this platform

    # Step 1: compile to assembly
    asm_fd, asm_path = tempfile.mkstemp(suffix=".s", dir=_PIO_BUILD_DIR)
    os.close(asm_fd)
    try:
        step1_args = [exe]
        skip_next = False
        for a in expanded_args:
            if skip_next:
                skip_next = False
                step1_args.append(asm_path)  # replace output path
                continue
            if a == "-c":
                step1_args.append("-S")   # compile to assembly, not object
            elif a == "-o":
                skip_next = True
                step1_args.append("-o")
            elif a == "-pipe":
                continue  # skip -pipe
            else:
                step1_args.append(a)

        has_longcalls = "-mlongcalls" in step1_args
        _log(f"STEP1 -mlongcalls={'YES' if has_longcalls else 'NO'} src={source_file[-40:]!r}")
        rc = subprocess.Popen(step1_args, env=merged_env, shell=False).wait()
        if rc != 0:
            return rc

        # Save a copy of the last .s file to help diagnose long-call issues.
        try:
            import shutil as _sh
            _sh.copy2(asm_path, os.path.join(_PIO_BUILD_DIR, "debug_last.s"))
        except OSError:
            pass

        # Step 2: assemble.
        # GCC 8.4.0 Xtensa does NOT emit .longcalls in the -S output even when
        # -mlongcalls is active — it relies on its internal assembler knowing the
        # mode. We must pass --longcalls explicitly to the external assembler so
        # it emits callx8 (indirect, 32-bit range) instead of call8 (direct,
        # ±512 KB), otherwise the linker fails on large firmwares.
        as_exe = _resolve("xtensa-esp32s3-elf-as")
        as_flags = ["--longcalls"] if "-mlongcalls" in step1_args else []
        step2_args = [as_exe] + as_flags + ["-o", out_obj, asm_path]
        rc = subprocess.Popen(step2_args, env=merged_env, shell=False).wait()
        return rc
    finally:
        try:
            os.unlink(asm_path)
        except OSError:
            pass


# Windows cmd.exe built-in commands that cannot be run as external executables.
_SHELL_BUILTINS = frozenset(["del", "copy", "move", "mkdir", "rmdir", "rd", "md",
                              "echo", "ren", "rename", "type", "dir", "cls", "set"])


def _log(msg):
    ts = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
    try:
        with open(_SPAWN_LOG, "a", encoding="utf-8") as f:
            f.write(f"[{ts}] {msg}\n")
    except OSError:
        pass


def _run_shell(cmd_str, merged_env):
    """Run *cmd_str* through cmd.exe (for shell built-ins like del/copy)."""
    _log(f"SHELL START: {cmd_str[:200]!r}")
    try:
        rc = subprocess.Popen(cmd_str, env=merged_env, shell=True).wait()
        _log(f"SHELL DONE rc={rc}: {cmd_str[:80]!r}")
        if rc != 0:
            sys.stderr.write(f"win_spawn_fix: shell rc={rc} cmd={cmd_str[:160]!r}\n")
        return rc
    except OSError as exc:
        _log(f"SHELL EXCEPTION: {exc} cmd={cmd_str[:80]!r}")
        sys.stderr.write(f"win_spawn_fix: shell failed for {cmd_str[:60]!r}: {exc}\n")
        return 1


def _run_direct(args_list, merged_env):
    """Run an external executable directly (shell=False, full path resolved).

    Avoids cmd.exe PATH lookup — we resolve the exe ourselves using the SCons
    PATH so it works even when cmd.exe's PATH doesn't include the toolchain.
    No pipe capture: we inherit parent stdout/stderr as-is.
    """
    _log(f"DIRECT START: {str(args_list[0])[:120]!r} ({len(args_list)} args)")
    try:
        rc = subprocess.Popen(args_list, env=merged_env, shell=False).wait()
        _log(f"DIRECT DONE rc={rc}: {str(args_list[0])[:80]!r}")
        if rc != 0:
            sys.stderr.write(f"win_spawn_fix: direct rc={rc} exe={str(args_list[0])[:80]!r}\n")
        return rc
    except OSError as exc:
        _log(f"DIRECT EXCEPTION: {exc} exe={str(args_list[0])[:80]!r}")
        sys.stderr.write(f"win_spawn_fix: direct failed for {str(args_list[0])[:60]!r}: {exc}\n")
        return 1


def _win_spawn(sh, escape, cmd, args, env_dict):
    if not args:
        return 1

    merged = _build_env()

    # Detect C/C++ compile commands: exe is g++/gcc AND has -c flag
    exe_raw = str(args[0]).strip('"').strip("'")
    exe_base = os.path.basename(exe_raw).lower().replace(".exe", "")
    is_cxx_compiler = "g++" in exe_base or "gcc" in exe_base
    # Expand response file just enough to check for -c
    expanded_peek = _expand_args(list(args)[1:])
    is_compile = is_cxx_compiler and any(a == "-c" for a in expanded_peek)

    if is_compile:
        _log(f"COMPILE: {exe_raw[:60]}")
        exe = _resolve(exe_raw)
        rc = _two_step_compile(exe, expanded_peek, merged)
        _log(f"COMPILE DONE rc={rc}: {exe_raw[:60]}")
        return rc

    # For all other commands (linker, ar, python scripts, del, copy, …):
    def _unquote(s):
        s = str(s)
        if len(s) >= 2 and s[0] == '"' and s[-1] == '"':
            return s[1:-1]
        return s

    exe_raw_unq = _unquote(str(args[0]))
    exe_base_lower = os.path.basename(exe_raw_unq).lower().replace(".exe", "")

    if exe_base_lower in _SHELL_BUILTINS:
        # cmd.exe built-in — must go through shell
        cmd_line = subprocess.list2cmdline([_unquote(a) for a in args])
        return _run_shell(cmd_line, merged)

    # External executable: resolve full path and run directly (shell=False).
    # Expand any @response-file args — some tools (ar) don't support them.
    exe_full = _resolve(exe_raw_unq)
    raw_args = [exe_full] + [_unquote(str(a)) for a in args[1:]]
    expanded = [raw_args[0]] + _expand_args(raw_args[1:])
    return _run_direct(expanded, merged)


env.Replace(SPAWN=_win_spawn)
