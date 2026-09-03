# Guard LovyanGFX's ESP32 SPI DMA descriptor allocation against OOM.
# Each PlatformIO environment owns a separate libdeps copy, so this script only
# changes environments that list it in extra_scripts.
Import("env")
import os

MARKER = "wadamesh-wio-tracker-l2-dmadesc-oom-patch"

OLD_ALLOC = """  void Bus_SPI::_alloc_dmadesc(size_t len)
  {
    if (_dmadesc) heap_caps_free(_dmadesc);
    _dmadesc_size = len;
    _dmadesc = (lldesc_t*)heap_caps_malloc(sizeof(lldesc_t) * len, MALLOC_CAP_DMA);
  }"""

NEW_ALLOC = """  void Bus_SPI::_alloc_dmadesc(size_t len)
  {
    // wadamesh-wio-tracker-l2-dmadesc-oom-patch: retain the working descriptor array
    // when a replacement cannot be allocated.
    auto next = (lldesc_t*)heap_caps_malloc(sizeof(lldesc_t) * len, MALLOC_CAP_DMA);
    if (next == nullptr) { return; }
    if (_dmadesc) heap_caps_free(_dmadesc);
    _dmadesc = next;
    _dmadesc_size = len;
  }"""

OLD_SETUP = """    if (_dmadesc_size * SPI_MAX_DMA_LEN < len)
    {
      _alloc_dmadesc(len / SPI_MAX_DMA_LEN + 1);
    }
    lldesc_t *dmadesc = _dmadesc;"""

NEW_SETUP = """    if (_dmadesc_size * SPI_MAX_DMA_LEN < len)
    {
      _alloc_dmadesc(len / SPI_MAX_DMA_LEN + 1);
    }
    // wadamesh-wio-tracker-l2-dmadesc-oom-patch: skip a frame rather than walking a
    // null or undersized descriptor array after allocation failure.
    if (_dmadesc == nullptr || _dmadesc_size * SPI_MAX_DMA_LEN < len) { return; }
    lldesc_t *dmadesc = _dmadesc;"""

path = os.path.join(
    env.subst("$PROJECT_LIBDEPS_DIR"),
    env.subst("$PIOENV"),
    "LovyanGFX", "src", "lgfx", "v1", "platforms", "esp32", "Bus_SPI.cpp"
)

if not os.path.isfile(path):
    print("[patch_lgfx_dmadesc] LovyanGFX not fetched yet; patch deferred")
else:
    with open(path, encoding="utf-8") as source_file:
        source = source_file.read()
    marker_count = source.count(MARKER)
    if marker_count == 2:
        print("[patch_lgfx_dmadesc] already patched")
    elif marker_count != 0 or OLD_ALLOC not in source or OLD_SETUP not in source:
        raise RuntimeError("[patch_lgfx_dmadesc] LovyanGFX source drift or partial patch")
    else:
        source = source.replace(OLD_ALLOC, NEW_ALLOC, 1).replace(OLD_SETUP, NEW_SETUP, 1)
        with open(path, "w", encoding="utf-8") as source_file:
            source_file.write(source)
        print("[patch_lgfx_dmadesc] patched Wio Tracker L2 LovyanGFX DMA allocation")


    def verify_patched(target, source, env):
      del target, source, env
      if not os.path.isfile(path):
        print("[patch_lgfx_dmadesc] ERROR: LovyanGFX still missing at link time")
        return 1
      with open(path, encoding="utf-8") as source_file:
        current = source_file.read()
      if current.count(MARKER) == 2:
        return 0
      if MARKER not in current and OLD_ALLOC in current and OLD_SETUP in current:
        current = current.replace(OLD_ALLOC, NEW_ALLOC, 1).replace(OLD_SETUP, NEW_SETUP, 1)
        with open(path, "w", encoding="utf-8") as source_file:
          source_file.write(current)
        print("[patch_lgfx_dmadesc] patched after dependency fetch; re-run the build")
      else:
        print("[patch_lgfx_dmadesc] ERROR: source drift or partial patch at link time")
      return 1


    env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", verify_patched)