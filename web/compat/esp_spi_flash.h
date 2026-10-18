#pragma once
#include <stddef.h>
#include <stdint.h>
#define SPI_FLASH_CACHE2PHYS_FAIL UINT32_MAX
#define SPI_FLASH_SEC_SIZE 4096
static inline size_t spi_flash_cache2phys(const void* p) { (void)p; return SPI_FLASH_CACHE2PHYS_FAIL; }
static inline size_t spi_flash_get_chip_size(void) { return 16u * 1024 * 1024; }
