// The ESP32-S3 shares one bus between flash and the octal PSRAM. While a flash
// write or erase holds it, the RGB panel's DMA can't read the framebuffer, the
// LCD FIFO runs dry and the picture jumps sideways until the next frame. The
// IDF 5 fixes (bounce buffers, flash auto-suspend) aren't in this Arduino core,
// so the LCD's clock source is switched off for the length of each flash
// operation: the panel holds its image for those few ms and nothing underruns.
// On hardware the sideways jump becomes a brief freeze or flicker. The
// crowpanel_50 env enables it with
//   -DCROWPANEL50_FLASH_PAUSE -Wl,--wrap=esp_flash_write -Wl,--wrap=esp_flash_erase_region
#ifdef CROWPANEL50_FLASH_PAUSE
#include <Arduino.h>
#include <esp_flash.h>
#include <soc/lcd_cam_struct.h>

namespace {
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
int s_depth = 0;
uint32_t s_saved = 0;

void lcdPause() {
  portENTER_CRITICAL(&s_mux);
  if (s_depth++ == 0) {
    s_saved = LCD_CAM.lcd_clock.val;
    if (LCD_CAM.lcd_clock.lcd_clk_sel != 0) LCD_CAM.lcd_clock.lcd_clk_sel = 0;
  }
  portEXIT_CRITICAL(&s_mux);
}

void lcdResume() {
  portENTER_CRITICAL(&s_mux);
  if (--s_depth == 0) LCD_CAM.lcd_clock.val = s_saved;
  portEXIT_CRITICAL(&s_mux);
}
}  // namespace

extern "C" {
esp_err_t __real_esp_flash_write(esp_flash_t*, const void*, uint32_t, uint32_t);
esp_err_t __real_esp_flash_erase_region(esp_flash_t*, uint32_t, uint32_t);

esp_err_t __wrap_esp_flash_write(esp_flash_t* chip, const void* buf, uint32_t addr, uint32_t len) {
  lcdPause();
  esp_err_t r = __real_esp_flash_write(chip, buf, addr, len);
  lcdResume();
  return r;
}

esp_err_t __wrap_esp_flash_erase_region(esp_flash_t* chip, uint32_t addr, uint32_t len) {
  lcdPause();
  esp_err_t r = __real_esp_flash_erase_region(chip, addr, len);
  lcdResume();
  return r;
}
}
#endif  // CROWPANEL50_FLASH_PAUSE
