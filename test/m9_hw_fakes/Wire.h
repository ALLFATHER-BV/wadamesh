// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <deque>
#include <vector>

struct FakeQmc6309 {
  bool present = true;
  uint8_t chip_id = 0x90;
  uint8_t ctrl1 = 0x00;
  uint8_t ctrl2 = 0x00;
  bool ovfl = false;
  bool drdy = true;
  bool clear_drdy_on_status_read = true;
  int wake_drdy_polls_remaining = 0;
  int configured_wake_drdy_delay_polls = 0;
  int status_polls_since_wake = 0;
  int data_reads_since_wake = 0;
  int status_polls_before_first_data_read = -1;
  int16_t raw_x = 0;
  int16_t raw_y = 0;
  int16_t raw_z = 0;
  std::vector<uint8_t> reg_read_trace;

  void setFieldMilliGauss(int16_t mx_mg, int16_t my_mg, int16_t mz_mg, bool overflow = false) {
    raw_x = mx_mg;
    raw_y = my_mg;
    raw_z = mz_mg;
    ovfl = overflow;
    drdy = true;
  }
};

struct FakeQmi8658 {
  bool present = true;
  uint8_t who_am_i = 0x05;
  uint8_t revision = 0x7C;
  uint8_t ctrl1 = 0x00;
  uint8_t ctrl2 = 0x00;
  uint8_t ctrl3 = 0x00;
  uint8_t ctrl5 = 0x00;
  uint8_t ctrl7 = 0x00;
  uint8_t reset_ok = 0x00;
  bool a_drdy = true;
  int16_t raw_ax = 0;
  int16_t raw_ay = 0;
  int16_t raw_az = 16384;
  int data_reads = 0;

  void setAccelG(float ax_g, float ay_g, float az_g, bool ready = true) {
    raw_ax = (int16_t)(ax_g * 16384.0f);
    raw_ay = (int16_t)(ay_g * 16384.0f);
    raw_az = (int16_t)(az_g * 16384.0f);
    a_drdy = ready;
  }
};

class TwoWire {
public:
  FakeQmc6309 qmc;
  FakeQmi8658 qmi;
  uint32_t clock_hz = 100000;
  uint16_t timeout_ms = 50;

  uint8_t cur_addr = 0;
  std::vector<uint8_t> tx_buf;
  uint8_t active_reg = 0;
  std::deque<uint8_t> rx_buf;

  bool begin(int = -1, int = -1) { return true; }
  bool setClock(uint32_t hz) { clock_hz = hz; return true; }
  void setTimeOut(uint16_t ms) { timeout_ms = ms; }

  void beginTransmission(uint8_t addr) {
    cur_addr = addr;
    tx_buf.clear();
  }

  size_t write(uint8_t b) {
    tx_buf.push_back(b);
    return 1;
  }

  uint8_t endTransmission(bool = true) {
    if (cur_addr == 0x7C) {
      if (!qmc.present) return 2;
      if (tx_buf.empty()) return 0;
      active_reg = tx_buf[0];
      if (tx_buf.size() >= 2) {
        const uint8_t val = tx_buf[1];
        if (active_reg == 0x0A) {
          const uint8_t prev = qmc.ctrl1;
          qmc.ctrl1 = val;
          // Transition from suspend (mode 00) to normal (mode 01):
          if ((prev & 0x03) == 0x00 && (val & 0x03) == 0x01) {
            qmc.wake_drdy_polls_remaining = qmc.configured_wake_drdy_delay_polls;
            qmc.drdy = (qmc.wake_drdy_polls_remaining == 0);
            qmc.status_polls_since_wake = 0;
            qmc.data_reads_since_wake = 0;
            qmc.status_polls_before_first_data_read = -1;
          }
        } else if (active_reg == 0x0B) {
          if (val & 0x80) {
            qmc.ctrl1 = 0x00;
            qmc.ctrl2 = 0x00;
          } else {
            qmc.ctrl2 = val;
          }
        }
      }
      return 0;
    }
    if (cur_addr == 0x6B) {
      if (!qmi.present) return 2;
      if (tx_buf.empty()) return 0;
      active_reg = tx_buf[0];
      if (tx_buf.size() >= 2) {
        const uint8_t val = tx_buf[1];
        switch (active_reg) {
          case 0x02: qmi.ctrl1 = val; break;
          case 0x03: qmi.ctrl2 = val; break;
          case 0x04: qmi.ctrl3 = val; break;
          case 0x06: qmi.ctrl5 = val; break;
          case 0x08: qmi.ctrl7 = val; break;
          case 0x60:
            if (val == 0xB0) qmi.reset_ok = 0x80;
            break;
          default: break;
        }
      }
      return 0;
    }
    return 2;
  }

  int requestFrom(int addr, int len) {
    rx_buf.clear();
    if (addr == 0x7C) {
      if (!qmc.present) return 0;
      qmc.reg_read_trace.push_back(active_reg);
      if (active_reg == 0x00 && len == 1) {
        rx_buf.push_back(qmc.chip_id);
      } else if (active_reg == 0x09 && len == 1) {
        ++qmc.status_polls_since_wake;
        if (qmc.wake_drdy_polls_remaining > 0) {
          --qmc.wake_drdy_polls_remaining;
          qmc.drdy = false;
        } else {
          qmc.drdy = true;
        }
        uint8_t st = (qmc.drdy ? 0x01 : 0x00) | (qmc.ovfl ? 0x02 : 0x00);
        rx_buf.push_back(st);
        if (qmc.drdy && qmc.clear_drdy_on_status_read) {
          qmc.drdy = false;
        }
      } else if (active_reg == 0x0A && len == 1) {
        rx_buf.push_back(qmc.ctrl1);
      } else if (active_reg == 0x0B && len == 1) {
        rx_buf.push_back(qmc.ctrl2);
      } else if (active_reg == 0x01 && len == 6) {
        if (qmc.data_reads_since_wake == 0) {
          qmc.status_polls_before_first_data_read = qmc.status_polls_since_wake;
        }
        ++qmc.data_reads_since_wake;
        const uint16_t ux = (uint16_t)qmc.raw_x;
        const uint16_t uy = (uint16_t)qmc.raw_y;
        const uint16_t uz = (uint16_t)qmc.raw_z;
        rx_buf.push_back((uint8_t)(ux & 0xFF));
        rx_buf.push_back((uint8_t)(ux >> 8));
        rx_buf.push_back((uint8_t)(uy & 0xFF));
        rx_buf.push_back((uint8_t)(uy >> 8));
        rx_buf.push_back((uint8_t)(uz & 0xFF));
        rx_buf.push_back((uint8_t)(uz >> 8));
      }
      return (int)rx_buf.size();
    }
    if (addr == 0x6B) {
      if (!qmi.present) return 0;
      if (active_reg == 0x00 && len == 1) {
        rx_buf.push_back(qmi.who_am_i);
      } else if (active_reg == 0x01 && len == 1) {
        rx_buf.push_back(qmi.revision);
      } else if (active_reg == 0x02 && len == 1) {
        rx_buf.push_back(qmi.ctrl1);
      } else if (active_reg == 0x03 && len == 1) {
        rx_buf.push_back(qmi.ctrl2);
      } else if (active_reg == 0x2E && len == 1) {
        rx_buf.push_back(qmi.a_drdy ? 0x01 : 0x00);
      } else if (active_reg == 0x4D && len == 1) {
        rx_buf.push_back(qmi.reset_ok);
      } else if (active_reg == 0x35 && len == 6) {
        ++qmi.data_reads;
        const uint16_t ux = (uint16_t)qmi.raw_ax;
        const uint16_t uy = (uint16_t)qmi.raw_ay;
        const uint16_t uz = (uint16_t)qmi.raw_az;
        rx_buf.push_back((uint8_t)(ux & 0xFF));
        rx_buf.push_back((uint8_t)(ux >> 8));
        rx_buf.push_back((uint8_t)(uy & 0xFF));
        rx_buf.push_back((uint8_t)(uy >> 8));
        rx_buf.push_back((uint8_t)(uz & 0xFF));
        rx_buf.push_back((uint8_t)(uz >> 8));
      }
      return (int)rx_buf.size();
    }
    return 0;
  }

  int read() {
    if (rx_buf.empty()) return -1;
    uint8_t v = rx_buf.front();
    rx_buf.pop_front();
    return v;
  }
};

extern TwoWire Wire;
extern TwoWire Wire1;
