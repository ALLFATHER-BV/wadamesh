// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

extern uint32_t g_usb_serial_jtag_conf0_reg;
#define USB_SERIAL_JTAG_CONF0_REG g_usb_serial_jtag_conf0_reg
#define USB_SERIAL_JTAG_USB_PAD_ENABLE (1u << 14)
#define REG_CLR_BIT(reg, bit) ((reg) &= ~(uint32_t)(bit))
