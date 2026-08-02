#include <assert.h>

#include "helpers/CompanionRetryPolicy.h"

int main() {
  using namespace CompanionRetryPolicy;

  static_assert(DIRECT_MAX_ATTEMPTS == 21, "direct retry count changed");
  static_assert(FLOOD_MAX_ATTEMPTS == 15, "flood retry count changed");

  assert(directDelay(100, 0) == 900);
  assert(directDelay(100, 20) == 2900);

  assert(floodDelay(300, 100, 0) == 2300);
  assert(floodDelay(300, 100, 100) == 2400);
  assert(floodDelay(300, 100, 200) == 2500);

  assert(isDirectEcho(5, 4));
  assert(isDirectEcho(5, 0));
  assert(!isDirectEcho(5, 5));
  assert(!isDirectEcho(5, 6));

  assert(isFloodEcho(0, 1));
  assert(isFloodEcho(2, 5));
  assert(!isFloodEcho(2, 2));
  assert(!isFloodEcho(2, 1));

  const uint8_t empty_key[8] = {};
  const uint8_t retry_key[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  const uint8_t same_retry_key[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  const uint8_t prefix_collision[8] = {1, 2, 3, 4, 5, 6, 7, 9};
  assert(!keyIsSet(empty_key, sizeof(empty_key)));
  assert(keyIsSet(retry_key, sizeof(retry_key)));
  assert(keysEqual(retry_key, same_retry_key, sizeof(retry_key)));
  assert(!keysEqual(retry_key, prefix_collision, sizeof(retry_key)));

  assert(shouldReplacePendingText(123, retry_key, same_retry_key,
                                  sizeof(retry_key)));
  assert(!shouldReplacePendingText(123, retry_key, prefix_collision,
                                   sizeof(retry_key)));
  assert(!shouldReplacePendingText(0, retry_key, same_retry_key,
                                   sizeof(retry_key)));

  uint32_t expected_ack = 0x12345678UL;
  uint8_t received_ack[4];
  memcpy(received_ack, &expected_ack, sizeof(received_ack));
  assert(ackMatches(expected_ack, received_ack));
  assert(!ackMatches(0, empty_key));

  assert(wakeDelay(1000, 1500) == 500);
  assert(wakeDelay(1500, 1500) == 0);
  assert(wakeDelay(1600, 1500) == 0);
  assert(wakeDelay(0xFFFFFFF0UL, 0x00000020UL) == 48);
  return 0;
}
