#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "RegionDiscovery.h"

int main() {
  assert(RegionDiscoveryResults::validPublicScope("north"));
  assert(RegionDiscoveryResults::validPublicScope("#north"));
  assert(RegionDiscoveryResults::validPublicScope("", true));
  assert(!RegionDiscoveryResults::validPublicScope("", false));
  assert(!RegionDiscoveryResults::validPublicScope("$private"));
  assert(!RegionDiscoveryResults::validPublicScope("bad name"));
  assert(!RegionDiscoveryResults::validPublicScope("#foo*"));
  assert(!RegionDiscoveryResults::validPublicScope("#foo!"));
  assert(!RegionDiscoveryResults::validPublicScope("Upper"));

  RegionDiscoveryResults results;
  const char csv[] = "north,south, north ,#east,north,*,$private,bad name";
  assert(results.addCsv(reinterpret_cast<const uint8_t*>(csv), strlen(csv)) == 3);
  assert(results.count() == 3);
  assert(strcmp(results.name(0), "#north") == 0);
  assert(strcmp(results.name(1), "#south") == 0);
  assert(strcmp(results.name(2), "#east") == 0);

  const char max_name[] = "abcdefghijklmnopqrstuvwxy1234";
  assert(strlen(max_name) == RegionDiscoveryResults::MAX_PUBLIC_NAME_BYTES);
  assert(RegionDiscoveryResults::validPublicScope(max_name));
  assert(results.add(reinterpret_cast<const uint8_t*>(max_name), strlen(max_name)));
  const char too_long[] = "abcdefghijklmnopqrstuvwxy12345";
  assert(!RegionDiscoveryResults::validPublicScope(too_long));
  assert(!results.add(reinterpret_cast<const uint8_t*>(too_long), strlen(too_long)));
  assert(results.name(results.count()) == nullptr);

  RegionDiscoveryResults response_results;
  const uint8_t response[] = {
    0x11, 0x22, 0x33, 0x44,  // reflected request tag
    0x55, 0x66, 0x77, 0x88,  // repeater clock
    'n', 'o', 'r', 't', 'h', ',', 's', 'o', 'u', 't', 'h'
  };
  assert(response_results.addResponse(response, sizeof response) == 2);
  assert(strcmp(response_results.name(0), "#north") == 0);
  assert(strcmp(response_results.name(1), "#south") == 0);
  assert(response_results.addResponse(nullptr, 0) == 0);
  assert(response_results.addResponse(response, 8) == 0);

  RegionDiscoveryResults bounded;
  for (uint8_t i = 0; i < RegionDiscoveryResults::MAX_RESULTS; ++i) {
    char name[8];
    const int length = snprintf(name, sizeof name, "r%u", (unsigned)i);
    assert(length > 0);
    assert(bounded.add(reinterpret_cast<const uint8_t*>(name), (size_t)length));
  }
  assert(bounded.count() == RegionDiscoveryResults::MAX_RESULTS);
  const char overflow[] = "overflow";
  assert(!bounded.add(reinterpret_cast<const uint8_t*>(overflow), strlen(overflow)));
  return 0;
}