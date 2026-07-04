# Compile the vendored gnuboy core (src/ui-touch/gnuboy/*.c) at -O2 with jump
# tables restored, instead of the Arduino-S3 framework default of
# `-Os -fno-jump-tables -fno-tree-switch-conversion`. gnuboy's hot path is a
# 228-case opcode switch (gb_cpu_emulate) — the textbook case that wants an
# indexed jump table; the size-optimized default forces a slow decision tree.
#
# GCC honours the LAST -O and the LAST -f<flag> on the command line, so appending
# these overrides the framework flags without any unflagging. Scoped to the
# gnuboy directory only (build_src_flags would hit the whole firmware). gnuboy
# runs from flash, so the only cost is a little extra .text.
Import("env")

GNUBOY_DIR = "ui-touch/gnuboy/"
FAST_FLAGS = ["-O2", "-fjump-tables", "-ftree-switch-conversion", "-funroll-loops"]

def _fast_gnuboy(env, node):
    path = node.get_path().replace("\\", "/")
    if GNUBOY_DIR in path:
        return env.Object(node, CCFLAGS=env["CCFLAGS"] + FAST_FLAGS)
    return node

env.AddBuildMiddleware(_fast_gnuboy)
