#include "FriendMeshCoreTypes.h"

#include <ctype.h>
#include <string.h>

namespace friendmesh {

bool idIsZero(const Id128& id) {
  uint8_t combined = 0;
  for (size_t i = 0; i < kIdSize; ++i) combined |= id.bytes[i];
  return combined == 0;
}

bool idsEqual(const Id128& lhs, const Id128& rhs) {
  return memcmp(lhs.bytes, rhs.bytes, kIdSize) == 0;
}

int compareIds(const Id128& lhs, const Id128& rhs) {
  return memcmp(lhs.bytes, rhs.bytes, kIdSize);
}

bool copyBoundedText(char* destination, size_t destinationSize,
                     const char* source, size_t maxBytes) {
  if (!destination || destinationSize == 0 || !source || source[0] == '\0') return false;
  const size_t length = strnlen(source, maxBytes + 1);
  if (length == 0 || length > maxBytes || length >= destinationSize) return false;
  memcpy(destination, source, length);
  destination[length] = '\0';
  return true;
}

bool boundedTextEqualFolded(const char* lhs, const char* rhs) {
  if (!lhs || !rhs) return false;
  while (*lhs && *rhs) {
    const unsigned char left = static_cast<unsigned char>(*lhs++);
    const unsigned char right = static_cast<unsigned char>(*rhs++);
    if (tolower(left) != tolower(right)) return false;
  }
  return *lhs == '\0' && *rhs == '\0';
}

}  // namespace friendmesh
