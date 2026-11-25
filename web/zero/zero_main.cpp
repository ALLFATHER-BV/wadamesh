// SPDX-License-Identifier: GPL-3.0-or-later
// wadamesh-zero: the wadamesh UI on the M5Stack Cardputer Zero (Linux, 320x170 LCD,
// TCA8418 keyboard), driving a MeshCore companion (e.g. MeshCoreRPI on the same Pi)
// over TCP or serial. No SDL, no desktop: the UI renders at the T-LoRa Pager's
// 480x222 layout (keyboard-only, the closest shipping wadamesh form factor) and is
// scaled straight into the Linux framebuffer; keys come from evdev.
//
//   wadamesh-zero [--tcp HOST[:PORT] | --serial PATH] [--read-only]
//                 [--fb /dev/fb0] [--kbd /dev/input/eventN] [--size 480x222]
//                 [--rotate 0|180] [--no-grab]
//   $WADA_DATA_DIR  the virtual flash (default ./wadamesh-data)
//
// Keys (evdev keycodes -> the Pager's keyboard + knob, see sim_pager/SimPager.cpp):
//   F X Z C (fn + them while typing)  up / down / left / right, 2D like the T-Deck trackball
//   OK / Enter         select (in a text field: Enter sends)   Fn+Enter  select always
//   ESC                Back                Fn+Home: back to Home
//   4 5 6 7 8          the tabs under the screen (Messages, Contacts, Home, Map, Settings)
//   1                  system HUD (G: GPX track log, W: Wi-Fi log, S: shell)
//   2                  drop to a Linux shell on the LCD; `exit` comes back
//   fn+H key help      fn+J screenshot      fn+U/I brightness      Ctrl+Q quit
//   (1, 2, F X Z C and 4-8 type normally in a text field, the PIN pad and Lua apps)
#include <Arduino.h>
#include "WebDisplay.h"
#include "WebInput.h"
#include "WebPlatform.h"
#include "BackendSelect.h"
#include "wada_host.h"
#include "lvgl.h"
#include "miniz.h"
#include "zero_help.h"
#include "zero_gps.h"
#include "zero_term.h"
#include "zero_sys.h"
#include "zero_ctl.h"
#include "zero_battery.h"
#include "BackendSelect.h"
#include "WebMesh.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <math.h>
#include <pwd.h>
#include <sys/wait.h>
#include <linux/vt.h>
#include <string>
#include <vector>

extern WebDisplay display;   // board.cpp

void wadaZeroNavArrow(int dir);   // src/ui-touch/UITask.cpp (WADA_ZERO): 2D focus move
bool wadaZeroUiWantsKeys();       // ... a text field, the PIN pad or a Lua app has the keys
bool wadaZeroPhosphor();          // ... the Phosphor look is on
extern MyMesh& the_mesh;
extern void (*g_wada_restart_hook)();   // compat/src/arduino_host.cpp: ESP.restart() calls it

namespace {
volatile sig_atomic_t s_quit = 0;
void onSignal(int) { s_quit = 1; }

// ---- overlays: screens of our own over the UI (help, boot log, HUD, the easter egg) ----
enum Overlay { OV_NONE, OV_HELP, OV_BOOT, OV_HUD, OV_EGG };
Overlay s_ov = OV_NONE;
bool s_ov_dirty = false;
TermCanvas s_cv;
bool s_link_up = false;
std::string s_link_name;
const uint16_t C_BG = rgb565(0x000000), C_GREEN = rgb565(0x39FF6A), C_DIM = rgb565(0x1A7A36),
               C_TEXT = rgb565(0xB8FFC8), C_AMBER = rgb565(0xFFB000), C_RED = rgb565(0xFF4A4A),
               C_GRAY = rgb565(0x5A6A60), C_CYAN = rgb565(0x2FE0C0);

// ---- framebuffer --------------------------------------------------------------------
struct Fb {
  int fd = -1;
  uint8_t* mem = nullptr;
  size_t len = 0;
  int w = 0, h = 0, bpp = 0, stride = 0;
  fb_var_screeninfo v{};
  std::vector<int> xmap, ymap;   // destination pixel -> source pixel (nearest, centred)
  std::vector<uint16_t> back;    // the frame as last written (skip unchanged rows)
  int rotate = 0;
} s_fb;

bool fbOpen(const char* path, int rotate) {
  s_fb.fd = open(path, O_RDWR);
  if (s_fb.fd < 0) { fprintf(stderr, "[zero] open %s: %s\n", path, strerror(errno)); return false; }
  fb_fix_screeninfo f{};
  if (ioctl(s_fb.fd, FBIOGET_VSCREENINFO, &s_fb.v) || ioctl(s_fb.fd, FBIOGET_FSCREENINFO, &f)) {
    // Testing off-device: WADA_ZERO_FAKEFB=WxH treats the path as a raw RGB565 file.
    int fw = 0, fh = 0;
    const char* fake = getenv("WADA_ZERO_FAKEFB");
    if (!fake || sscanf(fake, "%dx%d", &fw, &fh) != 2) {
      fprintf(stderr, "[zero] fb ioctl: %s\n", strerror(errno)); return false;
    }
    s_fb.v = {}; s_fb.v.xres = fw; s_fb.v.yres = fh; s_fb.v.bits_per_pixel = 16;
    s_fb.v.red.offset = 11; s_fb.v.green.offset = 5;
    f = {}; f.line_length = fw * 2; f.smem_len = fw * fh * 2;
    if (ftruncate(s_fb.fd, f.smem_len)) return false;
  }
  s_fb.w = s_fb.v.xres; s_fb.h = s_fb.v.yres; s_fb.bpp = s_fb.v.bits_per_pixel;
  s_fb.stride = f.line_length; s_fb.len = f.smem_len; s_fb.rotate = rotate;
  if (s_fb.bpp != 16 && s_fb.bpp != 32 && s_fb.bpp != 24) {
    fprintf(stderr, "[zero] fb %d bpp not supported\n", s_fb.bpp); return false;
  }
  s_fb.mem = (uint8_t*)mmap(nullptr, s_fb.len, PROT_READ | PROT_WRITE, MAP_SHARED, s_fb.fd, 0);
  if (s_fb.mem == MAP_FAILED) { fprintf(stderr, "[zero] mmap: %s\n", strerror(errno)); s_fb.mem = nullptr; return false; }
  fprintf(stderr, "[zero] %s: %dx%d %d bpp, stride %d, rgb offsets r%d g%d b%d\n", path, s_fb.w, s_fb.h,
          s_fb.bpp, s_fb.stride, s_fb.v.red.offset, s_fb.v.green.offset, s_fb.v.blue.offset);
  return true;
}

// The Pager UI draws 480x222 into the top-left of the web board's 800x480 canvas (as the
// Pager simulator does); s_src_w/h is that region, the canvas stride stays WADA_WEB_W.
int s_src_w = 480, s_src_h = 222;

// Area-averaging (box filter) downscale, 480x222 -> 320x170: each LCD pixel is the exact
// weighted average of the UI pixels it covers (1.5 x 1.31 of them), so strokes keep their
// weight and edges stay put instead of the blur/jitter of nearest-plus-neighbours.
struct Taps { int n; int idx[4]; int w[4]; };   // weights sum to 256
std::vector<Taps> s_xt, s_yt;

void buildTaps(std::vector<Taps>& out, int dst, int src) {
  out.assign(dst, Taps{});
  for (int d = 0; d < dst; ++d) {
    const double a = (double)d * src / dst, b = (double)(d + 1) * src / dst;
    Taps t{}; int sum = 0;
    for (int i = (int)a; i < b && i < src && t.n < 4; ++i) {
      const double ov = std::min(b, (double)i + 1) - std::max(a, (double)i);
      if (ov <= 0) continue;
      t.idx[t.n] = i; t.w[t.n] = (int)(ov / (b - a) * 256.0 + 0.5); sum += t.w[t.n]; t.n++;
    }
    if (t.n) t.w[t.n - 1] += 256 - sum;   // exact 256 total
    out[d] = t;
  }
}

void fbBuildMaps() {
  buildTaps(s_xt, s_fb.w, s_src_w);
  buildTaps(s_yt, s_fb.h, s_src_h);
  s_fb.back.assign((size_t)s_fb.w * s_fb.h, 0xFFFF);
}

inline uint16_t sampleBox(const uint16_t* src, const Taps& tx, const Taps& ty) {
  const int stride = WADA_WEB_W;
  uint32_t r = 0, g = 0, b = 0;
  for (int j = 0; j < ty.n; ++j) {
    const uint16_t* row = src + (size_t)ty.idx[j] * stride;
    uint32_t rr = 0, gg = 0, bb = 0;
    for (int i = 0; i < tx.n; ++i) {
      const uint16_t p = row[tx.idx[i]];
      const uint32_t w = tx.w[i];
      rr += w * (((p >> 11) & 31) * 527 + 23 >> 6);   // 5/6-bit -> 8-bit
      gg += w * (((p >> 5) & 63) * 259 + 33 >> 6);
      bb += w * ((p & 31) * 527 + 23 >> 6);
    }
    r += rr * ty.w[j]; g += gg * ty.w[j]; b += bb * ty.w[j];
  }
  r = (r + 32768) >> 16; g = (g + 32768) >> 16; b = (b + 32768) >> 16;   // back to 0..255
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void fbPresent(bool force) {
  if (!s_fb.mem) return;
  if (s_ov != OV_NONE) {   // our own screen, 1:1 at the LCD's size
    if (!s_ov_dirty && !force) return;
    s_ov_dirty = false;
    for (int y = 0; y < s_fb.h && y < s_cv.h; ++y) {
      const int dy = s_fb.rotate == 180 ? s_fb.h - 1 - y : y;
      uint8_t* row = s_fb.mem + (size_t)(dy + s_fb.v.yoffset) * s_fb.stride;
      for (int x = 0; x < s_fb.w && x < s_cv.w; ++x) {
        const int dx = s_fb.rotate == 180 ? s_fb.w - 1 - x : x;
        const uint16_t p = s_cv.px[(size_t)y * s_cv.w + x];
        s_fb.back[(size_t)dy * s_fb.w + dx] = p ^ 1;   // the UI redraws in full once we're done
        if (s_fb.bpp == 16) ((uint16_t*)row)[dx] = p;
        else if (s_fb.bpp == 32) {
          const uint32_t r = ((p >> 11) & 31) * 255 / 31, g = ((p >> 5) & 63) * 255 / 63, b = (p & 31) * 255 / 31;
          ((uint32_t*)row)[dx] = (r << s_fb.v.red.offset) | (g << s_fb.v.green.offset) | (b << s_fb.v.blue.offset);
        }
      }
    }
    return;
  }
  if (!display.takeDirty() && !force) return;
  const uint16_t* src = display.framebuffer();
  const bool scan = wadaZeroPhosphor();   // Phosphor look: every third row a little darker, CRT style
  for (int y = 0; y < s_fb.h; ++y) {
    const int dy = s_fb.rotate == 180 ? s_fb.h - 1 - y : y;
    uint16_t* bk = &s_fb.back[(size_t)dy * s_fb.w];
    uint8_t* row = s_fb.mem + (size_t)(dy + s_fb.v.yoffset) * s_fb.stride;
    const Taps& ty = s_yt[y];
    const bool dark_row = scan && (y % 3) == 2;
    for (int x = 0; x < s_fb.w; ++x) {
      const int dx = s_fb.rotate == 180 ? s_fb.w - 1 - x : x;
      uint16_t p = sampleBox(src, s_xt[x], ty);
      if (dark_row) p = (uint16_t)((((p >> 11) * 5 / 8) << 11) | ((((p >> 5) & 63) * 5 / 8) << 5) | ((p & 31) * 5 / 8));
      if (!force && bk[dx] == p) continue;
      bk[dx] = p;
      if (s_fb.bpp == 16) {
        ((uint16_t*)row)[dx] = p;
      } else {
        const uint32_t r = ((p >> 11) & 31) * 255 / 31, g = ((p >> 5) & 63) * 255 / 63, b = (p & 31) * 255 / 31;
        if (s_fb.bpp == 32) {
          ((uint32_t*)row)[dx] = (r << s_fb.v.red.offset) | (g << s_fb.v.green.offset) | (b << s_fb.v.blue.offset) |
                                 (s_fb.v.transp.length ? (0xFFu << s_fb.v.transp.offset) : 0);
        } else {
          uint8_t* px = row + dx * 3;
          px[s_fb.v.red.offset / 8] = r; px[s_fb.v.green.offset / 8] = g; px[s_fb.v.blue.offset / 8] = b;
        }
      }
    }
  }
}

// ---- console: keep the text console off the LCD while we own it ----------------------
int s_tty = -1;
void ttyGraphics(bool on) {
  if (s_tty < 0) s_tty = open("/dev/tty0", O_RDWR);
  if (s_tty < 0) s_tty = open("/dev/tty1", O_RDWR);
  if (s_tty < 0) return;
  ioctl(s_tty, KDSETMODE, on ? KD_GRAPHICS : KD_TEXT);
}

// ---- keyboard (evdev) ----------------------------------------------------------------
int s_kbd = -1;
bool s_shift = false, s_ctrl = false, s_alt = false, s_caps = false, s_fn = false;

std::string findKeyboard() {
  // Prefer the Cardputer Zero's own TCA8418 matrix; else the first device with letter keys.
  std::string fallback;
  DIR* d = opendir("/dev/input");
  if (!d) return "";
  std::vector<std::string> names;
  for (dirent* e; (e = readdir(d));) if (!strncmp(e->d_name, "event", 5)) names.push_back(e->d_name);
  closedir(d);
  for (auto& n : names) {
    std::string p = "/dev/input/" + n;
    int fd = open(p.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) continue;
    char name[128] = {0};
    ioctl(fd, EVIOCGNAME(sizeof name), name);
    unsigned long keys[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
    close(fd);
    auto has = [&](int k) { return (keys[k / (8 * sizeof(long))] >> (k % (8 * sizeof(long)))) & 1; };
    if (strstr(name, "tca8418")) { fprintf(stderr, "[zero] keyboard %s (%s)\n", p.c_str(), name); return p; }
    if (fallback.empty() && has(KEY_A) && has(KEY_Z) && has(KEY_ENTER)) fallback = p;
  }
  if (!fallback.empty()) fprintf(stderr, "[zero] keyboard %s\n", fallback.c_str());
  return fallback;
}

// US layout: keycode -> {plain, shifted}
struct KeyMap { int code; char lo, hi; };
const KeyMap kMap[] = {
  {KEY_1,'1','!'},{KEY_2,'2','@'},{KEY_3,'3','#'},{KEY_4,'4','$'},{KEY_5,'5','%'},{KEY_6,'6','^'},
  {KEY_7,'7','&'},{KEY_8,'8','*'},{KEY_9,'9','('},{KEY_0,'0',')'},{KEY_MINUS,'-','_'},{KEY_EQUAL,'=','+'},
  {KEY_Q,'q','Q'},{KEY_W,'w','W'},{KEY_E,'e','E'},{KEY_R,'r','R'},{KEY_T,'t','T'},{KEY_Y,'y','Y'},
  {KEY_U,'u','U'},{KEY_I,'i','I'},{KEY_O,'o','O'},{KEY_P,'p','P'},{KEY_LEFTBRACE,'[','{'},{KEY_RIGHTBRACE,']','}'},
  {KEY_A,'a','A'},{KEY_S,'s','S'},{KEY_D,'d','D'},{KEY_F,'f','F'},{KEY_G,'g','G'},{KEY_H,'h','H'},
  {KEY_J,'j','J'},{KEY_K,'k','K'},{KEY_L,'l','L'},{KEY_SEMICOLON,';',':'},{KEY_APOSTROPHE,'\'','"'},
  {KEY_GRAVE,'`','~'},{KEY_BACKSLASH,'\\','|'},{KEY_Z,'z','Z'},{KEY_X,'x','X'},{KEY_C,'c','C'},
  {KEY_V,'v','V'},{KEY_B,'b','B'},{KEY_N,'n','N'},{KEY_M,'m','M'},{KEY_COMMA,',','<'},{KEY_DOT,'.','>'},
  {KEY_SLASH,'/','?'},{KEY_SPACE,' ',' '},
};



// fn + J (PrintScreen): the 480x222 UI frame as a PNG in the data dir.
std::string saveScreenshot() {
  const uint16_t* fb = display.framebuffer();
  const int w = s_src_w, h = s_src_h;
  std::vector<uint8_t> rgb((size_t)w * h * 3);
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
    const uint16_t p = fb[(size_t)y * WADA_WEB_W + x];
    uint8_t* o = &rgb[((size_t)y * w + x) * 3];
    o[0] = ((p >> 11) & 31) * 255 / 31; o[1] = ((p >> 5) & 63) * 255 / 63; o[2] = (p & 31) * 255 / 31;
  }
  size_t len = 0;
  void* png = tdefl_write_image_to_png_file_in_memory(rgb.data(), w, h, 3, &len);
  if (!png) return "error: png";
  const char* dir = getenv("WADA_DATA_DIR");
  char path[512];
  snprintf(path, sizeof path, "%s/screenshot-%ld.png", dir ? dir : ".", (long)time(nullptr));
  if (FILE* f = fopen(path, "wb")) { fwrite(png, 1, len, f); fclose(f); fprintf(stderr, "[zero] screenshot %s\n", path); }
  mz_free(png);
  return path;
}

// ---- are we typing? -------------------------------------------------------------------
// CardputerZero's key guide: outside text input, F / Z / X / C work as the arrows without
// fn, and the 4-8 keys under the screen pick the five things along its bottom edge (here
// the tab bar: Messages, Contacts, Home, Apps, Settings). Inside a text field they type.
bool focusedTextareaIn(lv_obj_t* o, int depth) {
  if (!o || depth > 24 || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
  if (lv_obj_check_type(o, &lv_textarea_class) && lv_obj_has_state(o, LV_STATE_FOCUSED)) return true;
  const uint32_t n = lv_obj_get_child_cnt(o);
  for (uint32_t i = 0; i < n; ++i) if (focusedTextareaIn(lv_obj_get_child(o, i), depth + 1)) return true;
  return false;
}
bool typingNow() {
  if (wadaZeroUiWantsKeys()) return true;
  for (lv_indev_t* in = lv_indev_get_next(nullptr); in; in = lv_indev_get_next(in)) {
    if (in->group) {
      lv_obj_t* f = lv_group_get_focused(in->group);
      if (f && lv_obj_check_type(f, &lv_textarea_class)) return true;
    }
  }
  if (webEditing()) return true;
  return focusedTextareaIn(lv_layer_top(), 0) || focusedTextareaIn(lv_scr_act(), 0);
}

// Fn + U / I: the LCD backlight (first /sys/class/backlight device), in 10 % steps.
void backlightStep(int dir) {
  DIR* d = opendir("/sys/class/backlight");
  if (!d) return;
  std::string dev;
  for (dirent* e; (e = readdir(d));) if (e->d_name[0] != '.') { dev = std::string("/sys/class/backlight/") + e->d_name; break; }
  closedir(d);
  if (dev.empty()) return;
  int cur = 0, max = 0;
  if (FILE* f = fopen((dev + "/max_brightness").c_str(), "r")) { fscanf(f, "%d", &max); fclose(f); }
  if (FILE* f = fopen((dev + "/brightness").c_str(), "r")) { fscanf(f, "%d", &cur); fclose(f); }
  if (max <= 0) return;
  int step = max / 10 > 0 ? max / 10 : 1;
  int v = cur + dir * step;
  if (v < step) v = step;   // never fully dark
  if (v > max) v = max;
  if (FILE* f = fopen((dev + "/brightness").c_str(), "w")) { fprintf(f, "%d", v); fclose(f); }
}

// At start: a backlight left at 0 (the stock launcher's screen-off) shows nothing at all.
void backlightEnsureOn() {
  DIR* d = opendir("/sys/class/backlight");
  if (!d) return;
  for (dirent* e; (e = readdir(d));) {
    if (e->d_name[0] == '.') continue;
    std::string dev = std::string("/sys/class/backlight/") + e->d_name;
    int cur = 0, max = 0;
    if (FILE* f = fopen((dev + "/max_brightness").c_str(), "r")) { fscanf(f, "%d", &max); fclose(f); }
    if (FILE* f = fopen((dev + "/brightness").c_str(), "r")) { fscanf(f, "%d", &cur); fclose(f); }
    if (FILE* f = fopen((dev + "/bl_power").c_str(), "w")) { fprintf(f, "0"); fclose(f); }
    if (max > 0 && cur == 0) {
      if (FILE* f = fopen((dev + "/brightness").c_str(), "w")) { fprintf(f, "%d", max * 8 / 10); fclose(f); }
      fprintf(stderr, "[zero] backlight %s was 0, set to %d/%d\n", e->d_name, max * 8 / 10, max);
    }
  }
  closedir(d);
}


// ---- key help (fn + H) -----------------------------------------------------------------
void showHelp() {
  s_cv.resize(s_fb.w, s_fb.h);
  s_cv.clear(C_BG);
  for (int y = 0; y < s_cv.h && y < kZeroHelpH; ++y)
    for (int x = 0; x < s_cv.w && x < kZeroHelpW; ++x) s_cv.px[(size_t)y * s_cv.w + x] = kZeroHelp[(size_t)y * kZeroHelpW + x];
  s_ov = OV_HELP; s_ov_dirty = true;
}
void closeOverlay() { s_ov = OV_NONE; fbPresent(true); }

// ---- boot log: real probe results in a terminal, then the UI ----------------------------
struct BootLine { int tag; std::string text; };   // tag: 0 OK, 1 WARN, 2 FAIL, 3 info
std::vector<BootLine> s_boot;
size_t s_boot_shown = 0;
uint32_t s_boot_next = 0, s_boot_done_at = 0, s_boot_start = 0;
bool s_boot_mesh_line = false;

void bootAdd(int tag, const std::string& t) { s_boot.push_back({tag, t}); }
void bootDraw() {
  s_cv.resize(s_fb.w, s_fb.h);
  s_cv.clear(C_BG);
  const int ch = TermCanvas::cellH(), cw = TermCanvas::cellW();
  s_cv.text(2, 1, "wadamesh-zero", C_GREEN);
  s_cv.text(2 + 14 * cw, 1, "// m5stack cardputer zero", C_DIM);
  s_cv.fill(0, ch + 2, s_cv.w, 1, C_DIM);
  const int rows = (s_cv.h - ch - 4) / ch - 1;
  const size_t first = s_boot_shown > (size_t)rows ? s_boot_shown - rows : 0;
  int y = ch + 4;
  for (size_t i = first; i < s_boot_shown; ++i, y += ch) {
    const BootLine& l = s_boot[i];
    int x = 2;
    if (l.tag <= 2) {
      static const char* const kTag[3] = {" OK ", "WARN", "FAIL"};
      const uint16_t c = l.tag == 0 ? C_GREEN : l.tag == 1 ? C_AMBER : C_RED;
      x = s_cv.text(x, y, "[", C_GRAY); x = s_cv.text(x, y, kTag[l.tag], c); x = s_cv.text(x, y, "] ", C_GRAY);
    }
    s_cv.text(x, y, l.text.c_str(), l.tag == 3 ? C_CYAN : C_TEXT);
  }
  if ((millis() / 400) % 2) s_cv.fill(2, y + 2, cw - 1, ch - 3, C_GREEN);   // the cursor
  s_cv.dimRows(3, 200);
  s_ov_dirty = true;
}
void bootProbe(const std::string& kbd) {
  s_ov = OV_BOOT; s_boot_start = millis();
  char b[160];
  snprintf(b, sizeof b, "lcd: fb0 %dx%d %d bpp", s_fb.w, s_fb.h, s_fb.bpp); bootAdd(0, b);
  bootAdd(kbd.empty() ? 2 : 0, kbd.empty() ? "kbd: none found" : "kbd: tca8418 " + kbd);
  const ZeroBattery& bat = zeroBattery();
  if (bat.valid) { snprintf(b, sizeof b, "bat: bq27220 %d%% %.2fV %s", bat.soc, bat.mv / 1000.0, bat.charging ? "charging" : ""); bootAdd(0, b); }
  else bootAdd(1, "bat: no gauge");
  const ZeroSys sy = zeroSys();
  if (!sy.ip.empty()) { snprintf(b, sizeof b, "net: %s %s %s", sy.iface.c_str(), sy.ip.c_str(), sy.ssid.c_str()); bootAdd(0, b); }
  else bootAdd(1, "net: offline");
  const char* dd = getenv("WADA_DATA_DIR");
  std::string cities = std::string(dd && dd[0] ? dd : ".") + "/cities.bin";
  if (FILE* f = fopen(cities.c_str(), "rb")) {
    fseek(f, 0, SEEK_END); snprintf(b, sizeof b, "city: cities.bin %.1f MB", ftell(f) / 1048576.0); fclose(f); bootAdd(0, b);
  }
  if (sy.cpu_temp_c > 0) { snprintf(b, sizeof b, "cpu: %.1fC load %.2f", sy.cpu_temp_c, sy.load1); bootAdd(0, b); }
  s_boot_next = millis();
}
void bootAfterBegin() {
  const ZeroGpsStatus g = zeroGpsStatus();
  bootAdd(g.port ? 0 : 1, g.port ? "gps: " + g.path : "gps: not reading (port busy or absent)");
  bootAdd(3, "mesh: linking companion ...");
}
void bootTick() {
  if (s_ov != OV_BOOT) return;
  const uint32_t now = millis();
  if (!s_boot_mesh_line && (s_link_up || now - s_boot_start > 6000) && s_boot_shown >= s_boot.size()) {
    s_boot_mesh_line = true;
    if (s_link_up) {
      const NodePrefs* p = the_mesh.getNodePrefs();
      char b[160];
      bootAdd(0, "mesh: " + s_link_name);
      snprintf(b, sizeof b, "radio: %.3f MHz SF%u BW%.1f CR%u", p->freq, (unsigned)p->sf, p->bw, (unsigned)p->cr); bootAdd(0, b);
      snprintf(b, sizeof b, "peers: %d contacts", the_mesh.getNumContacts()); bootAdd(0, b);
    } else bootAdd(1, "mesh: companion not answering yet");
    bootAdd(3, "sacvalley: mesh up. all hops lead home.");
  }
  if (s_boot_shown < s_boot.size() && now >= s_boot_next) { s_boot_shown++; s_boot_next = now + 90; }
  if (s_boot_mesh_line && s_boot_shown >= s_boot.size() && !s_boot_done_at) s_boot_done_at = now + 900;
  bootDraw();
  if (s_boot_done_at && now >= s_boot_done_at) closeOverlay();
}

// ---- system HUD (key 1) ----------------------------------------------------------------
uint32_t s_hud_at = 0;
std::string s_hud_note;
void hudDraw() {
  s_cv.resize(s_fb.w, s_fb.h);
  s_cv.clear(C_BG);
  const int ch = TermCanvas::cellH(), cw = TermCanvas::cellW();
  const ZeroSys sy = zeroSys();
  const ZeroBattery& bat = zeroBattery();
  const ZeroGpsStatus g = zeroGpsStatus();
  const NodePrefs* p = the_mesh.getNodePrefs();
  char b[200];
  snprintf(b, sizeof b, "SYS // %s", sy.host.c_str());
  s_cv.text(2, 1, b, C_GREEN);
  time_t t = time(nullptr); tm lt{}; localtime_r(&t, &lt);
  strftime(b, sizeof b, "%H:%M:%S", &lt);
  s_cv.text(s_cv.w - 8 * cw - 2, 1, b, C_DIM);
  s_cv.fill(0, ch + 2, s_cv.w, 1, C_DIM);
  int y = ch + 4;
  auto row = [&](const char* k, const std::string& v, uint16_t c) {
    s_cv.text(2, y, k, C_DIM); s_cv.text(2 + 6 * cw, y, v.c_str(), c); y += ch - 1;
  };
  row("net", sy.ip.empty() ? "offline" : sy.iface + " " + sy.ip + " " + sy.ssid, sy.ip.empty() ? C_AMBER : C_TEXT);
  row("mesh", s_link_up ? s_link_name : "linking...", s_link_up ? C_TEXT : C_AMBER);
  snprintf(b, sizeof b, "%.3fMHz SF%u BW%.1f %ddBm", p->freq, (unsigned)p->sf, p->bw, (int)p->tx_power_dbm);
  row("radio", b, C_TEXT);
  snprintf(b, sizeof b, "%d%% %.2fV %+dmA%s", bat.soc, bat.mv / 1000.0, bat.ma, bat.charging ? " chg" : bat.full ? " full" : "");
  row("batt", b, bat.soc < 15 ? C_RED : C_TEXT);
  if (g.fix) snprintf(b, sizeof b, "%.5f,%.5f %dsat", g.lat, g.lon, g.sats);
  else snprintf(b, sizeof b, "%s", g.nmea ? "searching sky..." : g.port ? "no nmea" : "off");
  row("gps", b, g.fix ? C_TEXT : C_AMBER);
  if (sy.cpu_temp_c > 0) snprintf(b, sizeof b, "%.1fC load %.2f up %s", sy.cpu_temp_c, sy.load1, zeroUptimeText(sy.uptime_s).c_str());
  else snprintf(b, sizeof b, "load %.2f up %s", sy.load1, zeroUptimeText(sy.uptime_s).c_str());
  row("cpu", b, sy.cpu_temp_c > 70 ? C_AMBER : C_TEXT);
  snprintf(b, sizeof b, "%d/%dMB  disk %.1fG free", sy.mem_total_mb - sy.mem_avail_mb, sy.mem_total_mb, sy.disk_free_mb / 1024.0);
  row("mem", b, C_TEXT);
  std::string logs = std::string("gpx ") + (g.gpx_on ? "REC " + std::to_string(g.gpx_points) : "off") +
                     "  wifi " + (g.wifi_on ? "REC " + std::to_string(g.wifi_seen) : "off");
  row("logs", logs, (g.gpx_on || g.wifi_on) ? C_RED : C_TEXT);
  s_cv.fill(0, s_cv.h - ch - 2, s_cv.w, 1, C_DIM);
  s_cv.text(2, s_cv.h - ch, s_hud_note.empty() ? "G gpx  W wifi  S shell  any key: back" : s_hud_note.c_str(),
            s_hud_note.empty() ? C_DIM : C_AMBER);
  s_cv.dimRows(3, 200);
  s_ov_dirty = true;
  s_hud_at = millis();
}
void openHud() { s_ov = OV_HUD; s_hud_note.clear(); hudDraw(); }
void hudTick() { if (s_ov == OV_HUD && millis() - s_hud_at >= 1000) hudDraw(); }

// ---- Sac Valley MeshCore easter egg (F F X X Z C Z C B A, or `wadamesh-cli sac`) -------
struct EggNode { float x, y, vx, vy; };
std::vector<EggNode> s_egg;
uint32_t s_egg_start = 0;
void eggStart() {
  s_egg.clear();
  srand((unsigned)time(nullptr));
  for (int i = 0; i < 16; ++i)
    s_egg.push_back({(float)(rand() % 320), (float)(48 + rand() % 90), (rand() % 100 - 50) / 120.f, (rand() % 100 - 50) / 160.f});
  s_egg_start = millis();
  s_ov = OV_EGG;
  fprintf(stderr, "[zero] sac valley mode\n");
}
void eggTick() {
  if (s_ov != OV_EGG) return;
  const uint32_t t = millis() - s_egg_start;
  s_cv.resize(s_fb.w, s_fb.h);
  s_cv.clear(C_BG);
  // the mesh: drifting nodes, links within range, packets hopping along them
  for (auto& n : s_egg) {
    n.x += n.vx; n.y += n.vy;
    if (n.x < 4 || n.x > s_cv.w - 4) n.vx = -n.vx;
    if (n.y < 46 || n.y > s_cv.h - 30) n.vy = -n.vy;
  }
  for (size_t i = 0; i < s_egg.size(); ++i)
    for (size_t j = i + 1; j < s_egg.size(); ++j) {
      const float dx = s_egg[j].x - s_egg[i].x, dy = s_egg[j].y - s_egg[i].y, d = sqrtf(dx * dx + dy * dy);
      if (d > 90) continue;
      const int a = (int)(170 * (1.0f - d / 90));
      s_cv.line((int)s_egg[i].x, (int)s_egg[i].y, (int)s_egg[j].x, (int)s_egg[j].y, C_DIM, a);
      const float ph = fmodf(t / 900.0f + (i * 7 + j * 3) * 0.13f, 1.0f);   // a packet on this link
      if ((i + j) % 3 == 0) s_cv.fill((int)(s_egg[i].x + dx * ph) - 1, (int)(s_egg[i].y + dy * ph) - 1, 3, 3, C_AMBER);
    }
  for (auto& n : s_egg) s_cv.fill((int)n.x - 2, (int)n.y - 2, 5, 5, C_GREEN);
  // the title, typed out, with a glitch now and then
  const char* title = "SAC VALLEY";
  const char* title2 = "MESHCORE";
  const int sc = 2, cw = TermCanvas::cellW() * sc;
  const int shown = (int)(t / 70);
  std::string t1(title, std::min<int>(shown, (int)strlen(title)));
  std::string t2(title2, std::max(0, std::min<int>(shown - (int)strlen(title), (int)strlen(title2))));
  const int gx = ((t / 120) % 23 == 0) ? 2 : 0;
  s_cv.text((s_cv.w - (int)strlen(title) * cw) / 2 + gx, 2, t1.c_str(), C_GREEN, sc);
  s_cv.text((s_cv.w - (int)strlen(title2) * cw) / 2 - gx, 24, t2.c_str(), C_CYAN, sc);
  static const char* const kGreetz[] = {
    "> the valley is listening", "> greetz to every repeater on the ridge", "> zero hops from home",
    "> long range, short packets, good people", "> 909.875 // sf9 // all hops lead home", nullptr };
  int n = 0; while (kGreetz[n]) ++n;
  if (t > 1500) {
    char b[96];
    s_cv.fill(0, s_cv.h - 28, s_cv.w, 28, C_BG);
    s_cv.text(4, s_cv.h - 26, kGreetz[(t / 2600) % n], C_TEXT);
    snprintf(b, sizeof b, "73 de %s", the_mesh.getNodePrefs()->node_name);
    s_cv.text(4, s_cv.h - 13, b, C_DIM);
  }
  s_cv.dimRows(3, 170);
  s_ov_dirty = true;
}

// Konami, on the Cardputer's letter arrows: F F X X Z C Z C B A (or the real arrows).
std::string s_konami;
bool konamiFeed(char tok) {   // true: the key was the code's B or A (swallow it)
  s_konami += tok;
  if (s_konami.size() > 10) s_konami.erase(0, s_konami.size() - 10);
  if (tok == 'B') return s_konami.size() >= 9 && s_konami.compare(s_konami.size() - 9, 9, "UUDDLRLRB") == 0;
  if (tok == 'A' && s_konami == "UUDDLRLRBA") { s_konami.clear(); eggStart(); return true; }
  return false;
}

// ---- a Linux shell on the LCD (key 2), back with `exit` ---------------------------------
pid_t s_shell_pid = -1;
int s_shell_vt = -1, s_home_vt = -1, s_shell_fd = -1;
uint32_t s_shell_esc_down = 0;
bool s_grab = true;

bool fbconOnLcd() {
  // The kernel's text console must be drawing on fb0, or the shell would be invisible.
  for (int i = 0; i < 4; ++i) {
    char p[64], name[96] = {0}, bind[8] = {0};
    snprintf(p, sizeof p, "/sys/class/vtconsole/vtcon%d/name", i);
    FILE* f = fopen(p, "r");
    if (!f) continue;
    if (!fgets(name, sizeof name, f)) name[0] = 0;
    fclose(f);
    snprintf(p, sizeof p, "/sys/class/vtconsole/vtcon%d/bind", i);
    if ((f = fopen(p, "r"))) { if (!fgets(bind, sizeof bind, f)) bind[0] = 0; fclose(f); }
    if (strstr(name, "frame buffer") && bind[0] == '1') return true;
  }
  return false;
}
bool startShell(std::string& err) {
  if (s_shell_pid > 0) return true;
  if (!fbconOnLcd()) { err = "no text console on the LCD (fbcon off)"; return false; }
  const int tty0 = open("/dev/tty0", O_RDWR | O_CLOEXEC);
  if (tty0 < 0) { err = "no /dev/tty0 access"; return false; }
  vt_stat st{};
  int freevt = -1;
  if (ioctl(tty0, VT_GETSTATE, &st) || ioctl(tty0, VT_OPENQRY, &freevt) || freevt < 1) {
    close(tty0); err = "no free virtual terminal"; return false;
  }
  char path[32]; snprintf(path, sizeof path, "/dev/tty%d", freevt);
  const int fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (fd < 0) { close(tty0); err = std::string("open ") + path + ": " + strerror(errno); return false; }
  const char* banner = "\033[2J\033[H\033[32mwadamesh-zero\033[0m // shell. the mesh keeps running.\r\n"
                       "type \033[32mexit\033[0m to go back (or hold ESC 2 s)\r\n\r\n";
  (void)!write(fd, banner, strlen(banner));
  // Everything the child needs, built before fork (other threads may hold the heap lock).
  const passwd* pw = getpwuid(getuid());
  std::string home = std::string("HOME=") + (pw ? pw->pw_dir : "/"), user = std::string("USER=") + (pw ? pw->pw_name : "root");
  std::string shell = pw && pw->pw_shell && pw->pw_shell[0] ? pw->pw_shell : "/bin/bash";
  std::string sh_env = "SHELL=" + shell, dashname = "-" + shell.substr(shell.rfind('/') + 1);
  const char* path_env = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
  char* envp[] = {&home[0], &user[0], &sh_env[0], (char*)path_env, (char*)"TERM=linux", (char*)"WADAMESH=1", nullptr};
  char* argv[] = {&dashname[0], nullptr};
  const std::string dir = pw ? pw->pw_dir : "/";
  const pid_t pid = fork();
  if (pid < 0) { close(fd); close(tty0); err = "fork failed"; return false; }
  if (pid == 0) {
    prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0);   // the shell is plain `pi`, not the service's caps
    setsid();
    ioctl(fd, TIOCSCTTY, 1);
    dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
    (void)!chdir(dir.c_str());
    execve(shell.c_str(), argv, envp);
    _exit(127);
  }
  s_shell_pid = pid; s_shell_fd = fd; s_shell_vt = freevt; s_home_vt = st.v_active;
  if (s_kbd >= 0 && s_grab) ioctl(s_kbd, EVIOCGRAB, 0);   // the console reads the keyboard now
  ioctl(tty0, VT_ACTIVATE, freevt);
  ioctl(tty0, VT_WAITACTIVE, freevt);
  close(tty0);
  fprintf(stderr, "[zero] shell on tty%d (pid %d)\n", freevt, (int)pid);
  return true;
}
void endShell() {
  if (s_shell_pid > 0) { kill(s_shell_pid, SIGHUP); waitpid(s_shell_pid, nullptr, 0); }
  s_shell_pid = -1;
  const int tty0 = open("/dev/tty0", O_RDWR | O_CLOEXEC);
  if (tty0 >= 0) {
    if (s_home_vt > 0) { ioctl(tty0, VT_ACTIVATE, s_home_vt); ioctl(tty0, VT_WAITACTIVE, s_home_vt); }
    if (s_shell_vt > 0) ioctl(tty0, VT_DISALLOCATE, s_shell_vt);
    close(tty0);
  }
  if (s_shell_fd >= 0) { close(s_shell_fd); s_shell_fd = -1; }
  ttyGraphics(true);
  if (s_kbd >= 0 && s_grab) ioctl(s_kbd, EVIOCGRAB, 1);
  s_ov = OV_NONE;
  fbPresent(true);
  fprintf(stderr, "[zero] back from the shell\n");
}
void shellTick() {
  if (s_shell_pid <= 0) return;
  int st;
  if (waitpid(s_shell_pid, &st, WNOHANG) == s_shell_pid) { s_shell_pid = -1; endShell(); }
}
void openShell() {
  std::string err;
  if (!startShell(err)) { openHud(); s_hud_note = "shell: " + err; hudDraw(); }
}

std::string ctlScreenshot();   // below
void ctlHud() { openHud(); }
void ctlSac() { eggStart(); }

uint32_t s_esc_down_ms = 0;   // ESC: short press = Back, long press (guide) = Home

void keyEvent(int code, int value) {
  const bool down = value != 0;   // 1 press, 2 autorepeat
  if (s_shell_pid > 0) {   // the console has the keyboard; only watch for a 2 s ESC hold
    if (code == KEY_ESC && value == 1) s_shell_esc_down = millis() | 1;
    if (code == KEY_ESC && value == 0) s_shell_esc_down = 0;
    return;
  }
  if (s_ov == OV_BOOT && value == 1) { closeOverlay(); return; }   // any key skips the boot log
  if (s_ov == OV_EGG) { if (value == 1 && millis() - s_egg_start > 800) closeOverlay(); return; }
  if (s_ov == OV_HUD && value == 1) {
    switch (code) {
      case KEY_G: zeroGpsSetGpx(!zeroGpsStatus().gpx_on); hudDraw(); return;
      case KEY_W: zeroGpsSetWifiLog(!zeroGpsStatus().wifi_on); hudDraw(); return;
      case KEY_S: closeOverlay(); openShell(); return;
      case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
      case KEY_LEFTALT: case KEY_RIGHTALT: case KEY_DVD: case KEY_CD: return;
      default: closeOverlay(); return;
    }
  }
  if (s_ov == OV_HUD) return;
  if (code == KEY_ESC && s_ov != OV_HELP) {
    if (value == 1) { s_esc_down_ms = millis() ? millis() : 1; return; }
    if (value == 2) return;   // autorepeat while held
    if (value == 0 && s_esc_down_ms) {
      const bool longp = millis() - s_esc_down_ms >= 600;
      s_esc_down_ms = 0;
      if (longp) { for (int i = 0; i < 4; ++i) webKeyPush(0x1B); webKeyPush('h'); }
      else webKeyPush(0x1B);
    }
    return;
  }
  switch (code) {
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: s_shift = down; return;
    case KEY_LEFTCTRL: case KEY_RIGHTCTRL: s_ctrl = down; return;
    case KEY_LEFTALT: case KEY_RIGHTALT: s_alt = down; return;
    case KEY_FN: case KEY_LEFTMETA: case KEY_RIGHTMETA: s_fn = down; return;
    // Cardputer Zero's TCA8418 keymap: its Fn and symbol layers are applied in the kernel
    // (Fn sends KEY_DVD, the symbol key KEY_CD, then the already-translated key), so these
    // two are modifiers we only need to swallow.
    case KEY_DVD: case KEY_CD: return;
    default: break;
  }
  if (!down) return;
  if (s_ov == OV_HELP) { closeOverlay(); return; }   // any key closes the key help
  if (code == KEY_HELP) { showHelp(); return; }      // fn + H
  switch (code) {
    case KEY_KPASTERISK: webKeyPush('*'); return;
    case KEY_KPPLUS: webKeyPush('+'); return;
    case KEY_KPMINUS: webKeyPush('-'); return;
    case KEY_KPSLASH: webKeyPush('/'); return;
    case KEY_KPDOT: webKeyPush('.'); return;
    case KEY_KPEQUAL: webKeyPush('='); return;
    case KEY_PAGEUP: for (int i = 0; i < 5; ++i) webKeyPush(0xB5); return;
    case KEY_PAGEDOWN: for (int i = 0; i < 5; ++i) webKeyPush(0xB6); return;
    case KEY_HOME: webKeyPush(0x1B); webKeyPush(0x1B); webKeyPush(0x1B); return;   // back out to Home
    case KEY_BRIGHTNESSUP: backlightStep(+1); return;     // fn + I
    case KEY_BRIGHTNESSDOWN: backlightStep(-1); return;   // fn + U
    case KEY_VOLUMEUP: case KEY_VOLUMEDOWN: case KEY_MUTE:   // fn + D / S / A: no sound here yet
    case KEY_PLAYPAUSE: case KEY_PREVIOUSSONG: case KEY_NEXTSONG: return;
    case KEY_SYSRQ: saveScreenshot(); return;   // fn + J (PrintScreen)
    case KEY_END: case KEY_INSERT: return;
    case KEY_CAPSLOCK: s_caps = !s_caps; return;
    case KEY_ENTER: case KEY_KPENTER: webKeyPush(s_fn ? 0xB7 : 0x0D); return;   // Fn+Enter = knob click
    case KEY_BACKSPACE: webKeyPush(0x08); return;
    case KEY_DELETE: webKeyPush(0x08); return;
    case KEY_ESC: webKeyPush(0x1B); return;
    case KEY_TAB: webKeyPush(0x09); return;
    // The Cardputer Zero's arrows are Fn + F / Z / X / C (the kernel sends real arrow keys).
    // The Pager UI is driven by a knob (turn = move focus, click = select, hold = Back), so
    // the four arrows all walk the focus: Up / Left = previous, Down / Right = next.
    // Select is OK / Enter; Back is ESC.
    case KEY_UP: konamiFeed('U'); wadaZeroNavArrow(0); return;
    case KEY_DOWN: konamiFeed('D'); wadaZeroNavArrow(1); return;
    case KEY_LEFT: konamiFeed('L'); wadaZeroNavArrow(2); return;
    case KEY_RIGHT: konamiFeed('R'); wadaZeroNavArrow(3); return;
    default: break;
  }
  // Cardputer-style arrows if the keyboard has none of its own: Fn (or Alt) + ; . , /
  // (the keys printed with arrows), Fn + ` = Esc, Fn + Backspace = Delete (as Backspace).
  if (s_alt) {
    switch (code) {
      case KEY_SEMICOLON: wadaZeroNavArrow(0); return;
      case KEY_DOT:       wadaZeroNavArrow(1); return;
      case KEY_COMMA:     wadaZeroNavArrow(2); return;
      case KEY_SLASH:     wadaZeroNavArrow(3); return;
      case KEY_GRAVE:     webKeyPush(0x1B); return;
      default: break;
    }
  }
  // Outside a text field (CardputerZero key guide): F Z X C are the arrows without fn,
  // and 4 5 6 7 8 (the keys right under the screen) pick the five tabs along its bottom.
  if (!s_shift && !s_ctrl && !s_alt && !typingNow()) {
    switch (code) {
      case KEY_F: konamiFeed('U'); wadaZeroNavArrow(0); return;   // up
      case KEY_X: konamiFeed('D'); wadaZeroNavArrow(1); return;   // down
      case KEY_Z: konamiFeed('L'); wadaZeroNavArrow(2); return;   // left
      case KEY_C: konamiFeed('R'); wadaZeroNavArrow(3); return;   // right
      case KEY_B: if (konamiFeed('B')) return; break;
      case KEY_A: if (konamiFeed('A')) return; break;
      case KEY_1: openHud(); return;                               // system HUD
      case KEY_2: openShell(); return;                             // a shell on the LCD
      case KEY_4: webKeyPush('m'); return;   // Messages
      case KEY_5: webKeyPush('c'); return;   // Contacts
      case KEY_6: webKeyPush('h'); return;   // Home
      case KEY_7: webKeyPush('a'); return;   // Map
      case KEY_8: webKeyPush('s'); return;   // Settings
      default: break;
    }
  }
  for (const auto& m : kMap) {
    if (m.code != code) continue;
    if (s_ctrl && (m.lo == 'q')) { s_quit = 1; return; }
    bool up = s_shift;
    if (s_caps && m.lo >= 'a' && m.lo <= 'z') up = !up;
    webKeyPush((uint8_t)(up ? m.hi : m.lo));
    return;
  }
  if (getenv("WADA_ZERO_KEYDEBUG")) fprintf(stderr, "[zero] unmapped key %d\n", code);
}

void pumpKeys() {
  if (s_kbd < 0) return;
  input_event ev[32];
  for (;;) {
    const ssize_t n = read(s_kbd, ev, sizeof ev);
    if (n <= 0) break;
    for (size_t i = 0; i < (size_t)n / sizeof(input_event); ++i) {
      if (ev[i].type != EV_KEY) continue;
      if (getenv("WADA_ZERO_KEYDEBUG")) fprintf(stderr, "[zero] key %d %d\n", ev[i].code, ev[i].value);
      keyEvent(ev[i].code, ev[i].value);
    }
  }
}
std::string ctlScreenshot() { return saveScreenshot(); }

// ESP.restart() (a new theme, a factory reset, Power > Restart): come back as a fresh
// process in place, the way the device reboots, instead of just exiting.
char** s_argv = nullptr;
void zeroRestart() {
  fprintf(stderr, "[zero] respawn\n");
  if (s_shell_pid > 0) endShell();
  zeroCtlShutdown();
  zeroGpsShutdown();
  if (s_kbd >= 0) { ioctl(s_kbd, EVIOCGRAB, 0); close(s_kbd); s_kbd = -1; }
  if (s_fb.mem) { munmap(s_fb.mem, s_fb.len); s_fb.mem = nullptr; }
  if (s_fb.fd >= 0) close(s_fb.fd);
  if (s_tty >= 0) close(s_tty);
  fflush(stdout); fflush(stderr);
  execv("/proc/self/exe", s_argv);
  fprintf(stderr, "[zero] respawn failed: %s\n", strerror(errno));
}
}  // namespace

// The scripted/SDL host keeps this hook; nothing else calls it here.
bool wadaHostStep() {
  pumpKeys();
  zeroGpsPoll();
  zeroCtlPoll();
  wadaBoardStep();
  shellTick();
  if (s_shell_pid > 0) {   // the shell owns the LCD; a 2 s ESC hold takes it back
    if (s_shell_esc_down && millis() - s_shell_esc_down > 2000) { s_shell_esc_down = 0; endShell(); }
    return !s_quit;
  }
  bootTick();
  hudTick();
  eggTick();
  fbPresent(false);
  return !s_quit;
}

int main(int argc, char** argv) {
  WadaBackendChoice choice;
  std::string fb_path = "/dev/fb0", kbd_path;
  int rotate = 0;
  bool grab = true;
  int sw = 480, sh = 222;   // the T-LoRa Pager layout
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--serial" && i + 1 < argc) { choice.kind = "serial"; choice.arg = argv[++i]; }
    else if (a == "--tcp" && i + 1 < argc) { choice.kind = "tcp"; choice.arg = argv[++i]; }
    else if (a == "--read-only") choice.read_only = true;
    else if (a == "--fb" && i + 1 < argc) fb_path = argv[++i];
    else if (a == "--kbd" && i + 1 < argc) kbd_path = argv[++i];
    else if (a == "--rotate" && i + 1 < argc) rotate = atoi(argv[++i]);
    else if (a == "--no-grab") grab = false;
    else if (a == "--size" && i + 1 < argc) sscanf(argv[++i], "%dx%d", &sw, &sh);
    else if (a == "-h" || a == "--help") {
      printf("wadamesh-zero [--tcp HOST[:PORT] | --serial PATH] [--read-only] [--fb DEV] [--kbd DEV]\n"
             "              [--size WxH] [--rotate 0|180] [--no-grab]\n");
      return 0;
    }
  }
  s_argv = argv;
  g_wada_restart_hook = zeroRestart;
  setvbuf(stdout, nullptr, _IOLBF, 0);
  signal(SIGINT, onSignal); signal(SIGTERM, onSignal); signal(SIGHUP, onSignal);
  signal(SIGPIPE, SIG_IGN);

  s_src_w = sw; s_src_h = sh;   // the region of the canvas the Pager layout uses

  if (!fbOpen(fb_path.c_str(), rotate)) return 1;
  fbBuildMaps();
  ttyGraphics(true);
  backlightEnsureOn();
  // Another program drawing to the same framebuffer (the stock M5CardputerZero launcher)
  // overwrites us; say so, since the fix is to stop it.
  if (FILE* p = popen("fuser /dev/fb0 2>/dev/null", "r")) {
    char buf[256] = {0}; size_t n = fread(buf, 1, sizeof buf - 1, p); pclose(p);
    std::string me = std::to_string(getpid()), others;
    for (char* t = strtok(buf, " \n"); t; t = strtok(nullptr, " \n")) if (me != t) { others += " "; others += t; }
    if (n && !others.empty()) fprintf(stderr, "[zero] WARNING: /dev/fb0 is also open by pid%s (stop it, e.g. the M5CardputerZero launcher)\n", others.c_str());
  }

  s_grab = grab;
  if (kbd_path.empty()) kbd_path = findKeyboard();
  if (!kbd_path.empty()) {
    s_kbd = open(kbd_path.c_str(), O_RDONLY | O_NONBLOCK);
    if (s_kbd < 0) fprintf(stderr, "[zero] open %s: %s\n", kbd_path.c_str(), strerror(errno));
    else if (grab && ioctl(s_kbd, EVIOCGRAB, 1)) fprintf(stderr, "[zero] EVIOCGRAB: %s\n", strerror(errno));
  } else {
    fprintf(stderr, "[zero] no keyboard found (use --kbd /dev/input/eventN)\n");
  }

  const char* bl = getenv("WADA_ZERO_BOOTLOG");
  const bool bootlog = !(bl && !strcmp(bl, "0"));
  if (bootlog) { bootProbe(kbd_path); bootTick(); fbPresent(true); }
  g_wada_link_hook = [](int state, const char* name, const char* via, const char*) {
    s_link_up = state != 0;
    s_link_name = s_link_up ? std::string(name) + " via " + via : "";
    zeroCtlSetLink(s_link_up ? "up: " + s_link_name : "connecting");
  };
  wadaBoardBegin(choice);
  if (choice.kind == "demo") g_wada_link_hook(1, the_mesh.getNodePrefs()->node_name, "demo", "");   // the demo mesh: no link to wait for
  zeroGpsBegin();
  if (bootlog) bootAfterBegin();
  ZeroCtlActions acts;
  acts.screenshot = ctlScreenshot; acts.hud = ctlHud; acts.sac = ctlSac;
  zeroCtlBegin(acts);
  fbPresent(true);
  while (wadaHostStep()) {
    struct timespec ts{0, 8 * 1000 * 1000};   // ~120 loop passes/s max; LVGL ticks off millis()
    nanosleep(&ts, nullptr);
  }
  if (s_shell_pid > 0) endShell();
  zeroCtlShutdown();
  zeroGpsShutdown();
  wadaBoardFlush();
  if (s_kbd >= 0) { ioctl(s_kbd, EVIOCGRAB, 0); close(s_kbd); }
  if (s_fb.mem) memset(s_fb.mem, 0, s_fb.len);
  ttyGraphics(false);
  fflush(stdout);
  _exit(0);
}
