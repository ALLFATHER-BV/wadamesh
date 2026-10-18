#pragma once
// LittleFS on a host: same mechanism as SPIFFS.h, over <data>/littlefs (the tiles partition).
#include "arduino/FS.h"
namespace fs {
class LittleFSFS : public FS {
public:
  LittleFSFS();
  bool begin(bool formatOnFail = false, const char* basePath = "/littlefs", uint8_t maxOpenFiles = 10,
             const char* partitionLabel = "spiffs");
  bool format();
  size_t totalBytes();
  size_t usedBytes();
  void end();
};
}
extern fs::LittleFSFS LittleFS;
