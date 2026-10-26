#pragma once
#include <cstdint>
#include <cstring>
struct bhy2_data_xyz { int16_t x, y, z; };
inline void bhy2_parse_xyz(const uint8_t* d, bhy2_data_xyz* v) { memcpy(v, d, 6); }
