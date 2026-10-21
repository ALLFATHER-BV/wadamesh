#pragma once
// SPIFFS on a host: the arduino-esp32 VFS file layer (arduino/vfs_api.cpp, unmodified)
// over a host directory <data>/spiffs. SPIFFS has a flat namespace ("/a/b" is a file
// name, not a folder), so writes create missing parent folders here to match.
// totalBytes() reports the touch boards' SPIFFS partition size; usedBytes() is real.
#include "arduino/FS.h"
namespace fs {
class SPIFFSFS : public FS {
public:
  SPIFFSFS();
  bool begin(bool formatOnFail = false, const char* basePath = "/spiffs", uint8_t maxOpenFiles = 10,
             const char* partitionLabel = NULL);
  bool format();
  size_t totalBytes();
  size_t usedBytes();
  void end();
};
}
extern fs::SPIFFSFS SPIFFS;
