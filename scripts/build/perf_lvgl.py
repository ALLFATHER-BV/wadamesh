# PlatformIO pre-script: how LVGL itself is compiled, per environment.
#
#   custom_lvgl_o2 = yes
#       LVGL's sources at -O2 instead of the framework's -Os. The software renderer is
#       where a frame's time goes (walking the object tree, styles, glyphs, filling),
#       and -Os gives its loops up for size. Everything else keeps -Os, so the flash
#       cost stays with the code that earns it. -O2 goes a little deeper into the loop
#       task's stack, so such builds also get WADA_LVGL_O2, for which src/main.cpp
#       gives that task 1 KB more.
#
#   custom_lvgl_iram = lv_draw_sw_blend.c, lv_draw_label.c, ...
#       The functions LVGL marks LV_ATTRIBUTE_FAST_MEM in these source files run from
#       IRAM instead of through the S3's 16 KB instruction cache, which the rest of
#       the frame then has to itself. Costs their size in internal RAM, so only the
#       files that carry a frame (the pixel blend, labels, masks), not every marked
#       function (lines, arcs, shadows, images barely run in this UI).
#
# Perf benchmark builds override both from PLATFORMIO_BUILD_FLAGS: -DPERF_LVGL_O2 or
# -DBENCH_NO_O2, and -DBENCH_LVGL_IRAM=a.c+b.c (or =none).
import os
import re
Import("env")

flags_env = os.environ.get("PLATFORMIO_BUILD_FLAGS", "")

o2 = str(env.GetProjectOption("custom_lvgl_o2", "")).strip().lower() in ("1", "yes", "true", "on")
if "-DPERF_LVGL_O2" in flags_env:
    o2 = True
if "-DBENCH_NO_O2" in flags_env:
    o2 = False

iram = [f.strip() for f in str(env.GetProjectOption("custom_lvgl_iram", "")).split(",") if f.strip()]
m = re.search(r"-DBENCH_LVGL_IRAM=(\S+)", flags_env)
if m:
    iram = [] if m.group(1) == "none" else m.group(1).split("+")

IRAM_H = os.path.join(env.subst("$PROJECT_DIR"), "scripts", "build", "lvgl_iram.h")

if o2 or iram:
    if o2:
        env.Append(CPPDEFINES=["WADA_LVGL_O2"])   # src/main.cpp gives the loop task more stack

    def lvgl_object(env, node):
        flags = list(env["CCFLAGS"])
        if o2:
            flags = [f for f in flags if f not in ("-Os", "-O1", "-O3")] + ["-O2"]
        if node.name in iram:
            flags += ["-include", IRAM_H]
        return env.Object(node, CCFLAGS=flags)

    env.AddBuildMiddleware(lvgl_object, "*lvgl/src/*")
    print("perf: LVGL %s%s" % ("at -O2" if o2 else "at -Os",
                               (", IRAM: " + " ".join(iram)) if iram else ""))
