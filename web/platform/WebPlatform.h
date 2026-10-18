#pragma once
// The web board's boot + loop, shared by both front ends:
//   host_main.cpp  native: an SDL2 window (or --headless) + the scripted tour
//   web_main.cpp   Emscripten: a <canvas> in a page, driven by the page's JS
// board.cpp owns the board globals and runs src/main.cpp's setup()/loop() order.
#include <stdint.h>

struct WadaBackendChoice;   // web/mesh/BackendSelect.h
// setup(): storage, the mesh stand-in on the chosen backend (demo or a node), the UI.
void wadaBoardBegin(const WadaBackendChoice& choice);
// One iteration of src/main.cpp's loop(): UI first, then the mesh.
void wadaBoardStep();
// The deliberate power-off paths: flush what the device flushes (chat history is lazy).
void wadaBoardFlush();
