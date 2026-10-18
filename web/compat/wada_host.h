#pragma once
// Host-side knobs shared by the compat layer and web/platform.
#include <string>
// Root of the virtual flash: every SPIFFS/LittleFS file and the NVS store live under it.
// $WADA_DATA_DIR, else ./wadamesh-data. (Phase 2: an IDBFS mount in the browser.)
const std::string& wadaHostDataDir();
// mkdir -p
bool wadaHostMkdirs(const std::string& path);
