Import("env")
import os

MARKER = "wadamesh-rv3028-guard-patch"
OLD = "#define RV3028_ADDRESS   0x52"
NEW = """#ifndef RV3028_ADDRESS
#define RV3028_ADDRESS   0x52
#endif  // wadamesh-rv3028-guard-patch"""

path = os.path.join(
    env.subst("$PROJECT_LIBDEPS_DIR"),
    env.subst("$PIOENV"),
    "MeshCore",
    "src",
    "helpers",
    "AutoDiscoverRTCClock.cpp",
)

if not os.path.isfile(path):
    print("[patch_meshcore_rv3028_define] WARNING: %s not found (libdeps not fetched yet?)" % path)
    print("[patch_meshcore_rv3028_define] WARNING: MeshCore NOT patched - re-run build once libdeps exist")
else:
    with open(path) as f:
        src = f.read()

    if MARKER in src:
        print("[patch_meshcore_rv3028_define] already patched")
    elif OLD in src:
        with open(path, "w") as f:
            f.write(src.replace(OLD, NEW, 1))
        print("[patch_meshcore_rv3028_define] patched RV3028_ADDRESS guard in AutoDiscoverRTCClock.cpp")
    else:
        print("[patch_meshcore_rv3028_define] WARNING: pattern not found - MeshCore version drift?")
        print("[patch_meshcore_rv3028_define] WARNING: check AutoDiscoverRTCClock.cpp by hand, NOT patched")