// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
struct RefCountedDigitalPin { int claims = 0; void claim() { ++claims; } void release() { --claims; } };
