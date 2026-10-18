#pragma once
// <target.h> for the web board: the globals a variants/*/target.h declares.
#include <Mesh.h>
#include <helpers/SensorManager.h>
#include "../../src/helpers/ClockFloorRTC.h"
#include "WebBoard.h"
#include "WebRadio.h"
#include "WebDisplay.h"
#include "WebLocation.h"
#include "WebFiles.h"     // CAP_FILE_HANDOFF: downloads and the file picker

extern WebBoard board;
extern WebRadio radio_driver;
extern ClockFloorRTC rtc_clock;
extern WebSensorManager sensors;   // GPS = the host's location (WebLocation.h)
extern WebDisplay display;
