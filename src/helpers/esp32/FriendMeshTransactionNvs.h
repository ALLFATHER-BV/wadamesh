#pragma once

#include <stddef.h>
#include <stdint.h>

bool friendmeshTransactionNvsLoad(uint8_t slot, uint8_t* destination,
                                  size_t capacity, size_t& length);
bool friendmeshTransactionNvsPresent(uint8_t slot);
bool friendmeshTransactionNvsSave(uint8_t slot, const uint8_t* source,
                                  size_t length);
