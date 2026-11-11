#pragma once

#include <RadioLib.h>

// Full receiver reset for LR11x0-family chips (LR1110, LR1120, LR1121).
// Warm sleep powers down analog, calibrate(0x3F) refreshes all calibration blocks,
// then re-applies RX settings that calibration may reset.
inline void lr11x0ResetAGC(LR11x0* radio, float freqMHz, bool rx_boost_gain) {
  radio->sleep(true, 0);
  radio->standby(0x00, true); // 0x00 = STDBY_RC (RadioLib macro RADIOLIB_LR11X0_STANDBY_XOSC is inverted in 7.1.2)
#ifdef LR11X0_DIO3_TCXO_VOLTAGE
  // Warm sleep powers down DIO3 VTCXO; re-enable the TCXO before running 0x3F calibration.
  radio->setTCXO(LR11X0_DIO3_TCXO_VOLTAGE);
#else
  radio->setTCXO(3.0f);
#endif
  radio->standby(0x01, true); // 0x01 = STDBY_XOSC
  radio->setRegulatorDCDC();

  radio->calibrate(RADIOLIB_LR11X0_CALIBRATE_ALL);

  // calibrate(0x3F) defaults image calibration to 902-928MHz band.
  // Re-calibrate for the actual operating frequency (band=4MHz matches RadioLib default).
  radio->calibrateImageRejection(freqMHz - 4.0f, freqMHz + 4.0f);

  // Always restore runtime rx_boost_gain regardless of compile-time RX_BOOSTED_GAIN macro
  radio->setRxBoostedGainMode(rx_boost_gain);
}
