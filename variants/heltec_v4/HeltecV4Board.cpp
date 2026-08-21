#include "HeltecV4Board.h"

void HeltecV4Board::begin() {
    ESP32Board::begin();


#if defined(PIN_ADC_CTRL) && PIN_ADC_CTRL >= 0
    pinMode(PIN_ADC_CTRL, OUTPUT);
    digitalWrite(PIN_ADC_CTRL, LOW); // Initially inactive
#endif  // V4-R8: no ADC-control MOSFET — the battery divider is read directly

    loRaFEMControl.init();

#if defined(HELTEC_LORA_V4_R8) && defined(PIN_SD_CS)
    // Park the shared-FSPI micro-SD's chip-select HIGH before LGFX starts
    // driving the bus (display.begin + logo blit run before the first
    // SD.begin): GPIO3 is a strapping pin with no firmware pull, and a card
    // that samples CS low during that window can latch a confused state the
    // mount ladder then has to rescue. Same discipline as M9Board/the pager.
    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);
    // The R8 boot FEM-type line makes the auto-detect visible (the About page
    // reports the board name only) — needed to verify GPIO46 really is the
    // FEM TX-enable on this board rather than a plain LED (see r8 audit).
    Serial.printf("[R8] FEM type=%d (0=GC1109 1=KCT8103L 2=other)\n", (int)loRaFEMControl.getFEMType());
#endif

    periph_power.begin();
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_DEEPSLEEP) {
      long wakeup_source = esp_sleep_get_ext1_wakeup_status();
      if (wakeup_source & (1 << P_LORA_DIO_1)) {  // received a LoRa packet (while in deep sleep)
        startup_reason = BD_STARTUP_RX_PACKET;
    }

      rtc_gpio_hold_dis((gpio_num_t)P_LORA_NSS);
      rtc_gpio_deinit((gpio_num_t)P_LORA_DIO_1);
#if defined(HELTEC_LORA_V4_R8)
      // Release the digital-pad holds powerOffCb armed before deep sleep
      // (VEXT / GPS_EN / backlight) — held pads would otherwise block this
      // boot's own rail and backlight drives.
      gpio_deep_sleep_hold_dis();
      gpio_hold_dis((gpio_num_t)PIN_VEXT_EN);
      gpio_hold_dis((gpio_num_t)PIN_GPS_EN);
      gpio_hold_dis((gpio_num_t)PIN_TFT_LEDA_CTL);
#endif
    }
  }

  void HeltecV4Board::onBeforeTransmit(void) {
    digitalWrite(P_LORA_TX_LED, HIGH);   // turn TX LED on
    loRaFEMControl.setTxModeEnable();
  }

  void HeltecV4Board::onAfterTransmit(void) {
    digitalWrite(P_LORA_TX_LED, LOW);   // turn TX LED off
    loRaFEMControl.setRxModeEnable();
  }

  void HeltecV4Board::enterDeepSleep(uint32_t secs, int pin_wake_btn) {
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    // Make sure the DIO1 and NSS GPIOs are hold on required levels during deep sleep
    rtc_gpio_set_direction((gpio_num_t)P_LORA_DIO_1, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_en((gpio_num_t)P_LORA_DIO_1);

    rtc_gpio_hold_en((gpio_num_t)P_LORA_NSS);

    loRaFEMControl.setRxModeEnableWhenMCUSleep();//It also needs to be enabled in receive mode

    if (pin_wake_btn < 0) {
      esp_sleep_enable_ext1_wakeup( (1L << P_LORA_DIO_1), ESP_EXT1_WAKEUP_ANY_HIGH);  // wake up on: recv LoRa packet
    } else {
      esp_sleep_enable_ext1_wakeup( (1L << P_LORA_DIO_1) | (1L << pin_wake_btn), ESP_EXT1_WAKEUP_ANY_HIGH);  // wake up on: recv LoRa packet OR wake btn
    }

    if (secs > 0) {
      esp_sleep_enable_timer_wakeup(secs * 1000000);
    }

    // Finally set ESP32 into sleep
    esp_deep_sleep_start();   // CPU halts here and never returns!
  }

  void HeltecV4Board::powerOff()  {
    enterDeepSleep(0);
  }

  uint16_t HeltecV4Board::getBattMilliVolts()  {
#if defined(HELTEC_LORA_V4_R8)
    // R8: match Meshtastic's heltec_v4_r8 variant — the always-connected
    // high-resistance divider wants LOW attenuation (2.5 dB, full-scale
    // ~1.05 V; the node sits at VBAT/5.07 ≈ 0.65-0.85 V) and a CALIBRATED
    // millivolt read. The generic uncalibrated raw*(3.3/1024) at the default
    // 11 dB under-read this divider by ~5-10%: a full 4.2 V pack displayed
    // ~3.9 V, so the charging-bolt threshold (batteryFullMv()+50) was
    // unreachable by construction and one ADC count quantized to ~16 mV of
    // display (the "frozen at 3839 mV" report).
    static bool s_atten_set = false;
    if (!s_atten_set) { analogSetPinAttenuation(PIN_VBAT_READ, ADC_2_5db); s_atten_set = true; }
    uint32_t mv = 0;
    for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(PIN_VBAT_READ);
    return (uint16_t)((mv / 8) * adc_mult);
#else
    analogReadResolution(10);
#if defined(PIN_ADC_CTRL) && PIN_ADC_CTRL >= 0
    digitalWrite(PIN_ADC_CTRL, HIGH);   // enable the battery divider
    delay(10);
#endif
    uint32_t raw = 0;
    for (int i = 0; i < 8; i++) {
      raw += analogRead(PIN_VBAT_READ);
    }
    raw = raw / 8;
#if defined(PIN_ADC_CTRL) && PIN_ADC_CTRL >= 0
    digitalWrite(PIN_ADC_CTRL, LOW);
#endif
    return (adc_mult * (3.3 / 1024.0) * raw) * 1000;
#endif
  }

  const char* HeltecV4Board::getManufacturerName() const {
#if defined(HELTEC_LORA_V4_R8)
    return "Heltec V4-R8";
#elif defined(HELTEC_LORA_V4_TFT)
    return loRaFEMControl.getFEMType() == KCT8103L_PA ? "Heltec V4.3 TFT" : "Heltec V4 TFT";
#else
    return loRaFEMControl.getFEMType() == KCT8103L_PA ? "Heltec V4.3 OLED" : "Heltec V4 OLED";
#endif
  }
