#pragma once
// Placement attributes are meaningless off-chip.
#define IRAM_ATTR
#define DRAM_ATTR
#define RTC_DATA_ATTR
#define RTC_NOINIT_ATTR
#define RTC_FAST_ATTR
#define RTC_SLOW_ATTR
#define EXT_RAM_ATTR
#define EXT_RAM_BSS_ATTR
#define NOINIT_ATTR
#define __NOINIT_ATTR
#define WORD_ALIGNED_ATTR __attribute__((aligned(4)))
#define DMA_ATTR WORD_ALIGNED_ATTR
#define FORCE_INLINE_ATTR static inline __attribute__((always_inline))
#define IRAM_DATA_ATTR
