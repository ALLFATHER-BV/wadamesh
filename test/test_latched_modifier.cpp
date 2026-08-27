// SPDX-License-Identifier: GPL-3.0-or-later

#include <assert.h>

#include "helpers/input/LatchedModifier.h"

int main() {
  LatchedModifier modifier;

  modifier.press();
  assert(modifier.consumeForKey());
  modifier.release(100);
  assert(!modifier.consumeForKey());

  modifier.press();
  modifier.release(200);
  assert(modifier.consumeForKey());
  assert(!modifier.consumeForKey());

  modifier.press();
  modifier.release(400);
  modifier.press();
  modifier.release(700);
  assert(modifier.locked());
  assert(modifier.consumeForKey());
  assert(modifier.consumeForKey());

  modifier.press();
  modifier.release(800);
  assert(!modifier.active());

  modifier.tap(1000);
  modifier.tap(1500);
  assert(!modifier.locked());
  assert(modifier.consumeForKey());

  modifier.tap(2000);
  modifier.clearLatched();
  assert(!modifier.active());

  modifier.tap(2100);
  modifier.discard();
  assert(!modifier.active());

  modifier.press();
  modifier.discard();
  modifier.release(2200);
  assert(!modifier.active());

  LatchedModifier wrapped;
  wrapped.tap(UINT32_MAX - 100);
  wrapped.tap(100);
  assert(wrapped.locked());

  wrapped.baseline(true);
  assert(wrapped.held());
  wrapped.release(200);
  assert(!wrapped.active());
  wrapped.baseline(false);
  assert(!wrapped.active());
  return 0;
}