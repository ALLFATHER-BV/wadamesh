-- ProTreck — wadamesh outdoor watch: Astro · Chrono · Timer · Altimeter
-- Swipe left/right to change tab
-- ASTRO: sunrise/sunset + moon phase from GPS fix
-- CHRONO: tap start/stop, swipe-up lap, swipe-down reset
-- TIMER: tap start/stop, swipe-up cycle preset, swipe-down reset
-- ALTI: GPS altitude with min/max and trend graph; swipe-down resets min/max
-- Contributed by samuelcoustet

local ui, sys, store, tmr = wada.ui, wada.sys, wada.store, wada.timer
local C = ui.colors

local app = {}

-- ── Constants ─────────────────────────────────────────────────────────────
local TAB = { "ASTRO", "CHRONO", "TIMER", "ALTI" }
local TAB_H  = 18    -- tab bar height (px)
local HIST_N = 60    -- altimeter ring-buffer size

-- ── State ─────────────────────────────────────────────────────────────────
local tab = 1
local cv, W, H

-- Astro
local astro_t0    = 0     -- last refresh (millis)
local astro_sr, astro_ss  -- sunrise / sunset (UTC fractional hours), or nil
local astro_phase = 0     -- moon phase 0-29
local astro_lat, astro_lon, astro_yday, astro_year

-- Chrono
local chr_run    = false
local chr_t0     = 0     -- millis at last start
local chr_acc    = 0     -- accumulated ms before last start
local chr_laps   = {}    -- list of "MM:SS.cs" strings

-- Timer
local TMR_PRESETS = { 30, 60, 120, 300, 600, 1800, 3600 }
local tmr_pi     = 4     -- preset index (default 300 s)
local tmr_secs   = 300
local tmr_end    = 0     -- millis when alarm fires; 0 = not running
local tmr_done   = false

-- Altimeter
local alti_min   = 99999
local alti_max   = -99999
local alti_hist  = {}
local alti_hi    = 0     -- ring-buffer write head

-- ── Time helpers ──────────────────────────────────────────────────────────
local function now_unix()
  local ep = sys.epoch and sys.epoch()
  if ep then return ep end
  if os then return os.time and os.time() end
  return nil
end

local function decompose(t)
  if os and os.date then return os.date("*t", t) end
  -- minimal fallback (good enough for rough yday on harness without os.date)
  local s = t % 86400
  local sec  = s % 60;            s = math.floor(s / 60)
  local min2 = s % 60;            local hr = math.floor(s / 60)
  local days = math.floor(t / 86400)
  -- Gregorian: days since epoch 1970-01-01
  local y = 1970; local d = days
  while true do
    local ylen = ((y % 4 == 0 and y % 100 ~= 0) or y % 400 == 0) and 366 or 365
    if d < ylen then break end
    d = d - ylen; y = y + 1
  end
  return { year = y, hour = hr, min = min2, sec = sec, yday = d + 1 }
end

local function fmt_hms(ms)
  local s  = math.floor(ms / 1000)
  local cs = math.floor((ms % 1000) / 10)
  local m  = math.floor(s / 60); s = s % 60
  local h  = math.floor(m / 60); m = m % 60
  if h > 0 then
    return string.format("%d:%02d:%02d", h, m, s)
  else
    return string.format("%02d:%02d.%02d", m, s, cs)
  end
end

local function fmt_mm_ss(secs)
  local h = math.floor(secs / 3600)
  local m = math.floor((secs % 3600) / 60)
  local s = secs % 60
  if h > 0 then return string.format("%d:%02d:%02d", h, m, s) end
  return string.format("%02d:%02d", m, s)
end

-- ── Sun / moon algorithms ─────────────────────────────────────────────────
local PI  = math.pi
local RAD = PI / 180

local function sun_utc(lat, lon, yday)
  -- Spencer 1971 simplified; returns rise_utc, set_utc (fractional hours) or nil,nil
  local B    = RAD * (360 / 365) * (yday - 81)
  local eqt  = 9.87 * math.sin(2 * B) - 7.53 * math.cos(B) - 1.5 * math.sin(B)  -- minutes
  local decl = RAD * 23.45 * math.sin(B)
  local cha  = -math.tan(lat * RAD) * math.tan(decl)
  if cha < -1 or cha > 1 then return nil, nil end
  local ha   = math.acos(cha) / RAD  -- degrees
  local noon = 12 - lon / 15 - eqt / 60
  return noon - ha / 15, noon + ha / 15
end

local function moon_phase_num(t)
  -- days since known new moon 2000-01-06 (JD 2451549.5) → phase 0-29
  local days = t / 86400 - 10957  -- 10957 = days from 1970-01-01 to 2000-01-01
  local cycle = 29.53058867
  return math.floor((days - 6.0) % cycle)
end

local MOON_NAMES = {
  [0]="New Moon", [1]="New Moon",
  [2]="Waxing Crescent",[3]="Waxing Crescent",[4]="Waxing Crescent",
  [5]="Waxing Crescent",[6]="Waxing Crescent",[7]="Waxing Crescent",
  [8]="First Quarter",[9]="First Quarter",
  [10]="Waxing Gibbous",[11]="Waxing Gibbous",[12]="Waxing Gibbous",
  [13]="Waxing Gibbous",[14]="Waxing Gibbous",
  [15]="Full Moon",[16]="Full Moon",
  [17]="Waning Gibbous",[18]="Waning Gibbous",[19]="Waning Gibbous",
  [20]="Waning Gibbous",[21]="Waning Gibbous",
  [22]="Last Quarter",[23]="Last Quarter",
  [24]="Waning Crescent",[25]="Waning Crescent",[26]="Waning Crescent",
  [27]="Waning Crescent",[28]="Waning Crescent",[29]="New Moon",
}

local function update_astro()
  local now = sys.millis()
  if now - astro_t0 < 60000 then return end
  astro_t0 = now
  local t = now_unix()
  local gps = sys.gps()
  if gps and t then
    local d = decompose(t)
    astro_lat  = gps.lat
    astro_lon  = gps.lon
    astro_yday = d.yday or 182
    astro_year = d.year or 2024
    astro_sr, astro_ss = sun_utc(gps.lat, gps.lon, astro_yday)
    astro_phase = moon_phase_num(t)
  end
end

-- ── Draw helpers ──────────────────────────────────────────────────────────
local function tx(x)   return math.floor(x) end
local function ty(y)   return math.floor(y) end

local function label(x, y, s, col, sz)
  cv:text(tx(x), ty(y), tostring(s), col, sz)
end

local function head(x, y, s, col)
  label(x, y, s, col or C.sub, 11)
end

local function big(x, y, s, col, sz)
  label(x, y, s, col or C.text, sz or 24)
  return #s * (sz and math.floor(sz * 0.6) or 14)  -- rough width estimate
end

-- ── ASTRO tab ─────────────────────────────────────────────────────────────
local function draw_astro(cw, ch)
  local t = now_unix()
  local d = t and decompose(t) or nil

  -- Date header
  if d then
    local ds = string.format("%04d-%02d-%02d", d.year or 0, d.month or 0, d.day or 0)
    head(4, 4, ds, C.sub)
    local ts2 = string.format("%02d:%02d:%02d UTC", d.hour or 0, d.min or 0, d.sec or 0)
    head(cw - tx(#ts2 * 6 + 4), 4, ts2, C.sub)
  end

  local y = 22
  -- Sun section
  cv:rect(4, y, cw - 8, 1, C.sub, true, 0)
  y = y + 6
  head(4, y, "SUN", C.accent)
  y = y + 14
  if astro_sr then
    local rh = math.floor(astro_sr)
    local rm = math.floor((astro_sr - rh) * 60 + 0.5)
    local sh = math.floor(astro_ss)
    local sm = math.floor((astro_ss - sh) * 60 + 0.5)
    label(8,       y, string.format("Rise  %02d:%02d", rh, rm), C.text, 13)
    label(8,  y + 16, string.format("Set   %02d:%02d", sh, sm), C.text, 13)
  elseif astro_lat then
    label(8, y, "Polar day / night", C.sub, 12)
    y = y - 16
  else
    label(8, y, "Need GPS fix", C.bad, 12)
    y = y - 16
  end

  y = y + 40
  -- Moon section
  cv:rect(4, y, cw - 8, 1, C.sub, true, 0)
  y = y + 6
  head(4, y, "MOON", C.accent)
  y = y + 14
  if astro_lat and t then
    local name = MOON_NAMES[astro_phase] or "?"
    label(8, y,      string.format("Phase %d / 29", astro_phase), C.text, 13)
    label(8, y + 16, name, C.sub, 12)
    -- draw a simple moon disc glyph (filled = new/full, half = quarters)
    local mx = math.floor(cw * 3 / 4)
    local my = ty(y + 8)
    local r  = 10
    cv:circle(mx, my, r, C.sub, false, 1)
    if astro_phase < 3 or astro_phase > 27 then
      cv:circle(mx, my, r - 2, C.sub, true, 0)        -- new moon: dark
    elseif astro_phase < 15 then
      cv:circle(mx, my, r - 2, C.accent, true, 0)     -- waxing: right half lit
    elseif astro_phase < 17 then
      cv:circle(mx, my, r - 2, C.good, true, 0)       -- full moon
    else
      cv:circle(mx, my, r - 2, C.accent, true, 0)     -- waning: left half lit
    end
  else
    label(8, y, "Need GPS + time", C.bad, 12)
  end

  -- GPS status row at bottom
  local gps = sys.gps()
  if gps then
    label(4, ch - 16, string.format("GPS %d sats  %.4f,%.4f", gps.sats, gps.lat, gps.lon), C.sub, 10)
  else
    label(4, ch - 16, "GPS: no fix", C.bad, 10)
  end
end

-- ── CHRONO tab ────────────────────────────────────────────────────────────
local function chr_elapsed()
  if chr_run then
    return chr_acc + (sys.millis() - chr_t0)
  else
    return chr_acc
  end
end

local function draw_chrono(cw, ch)
  local ms  = chr_elapsed()
  local ts  = fmt_hms(ms)
  -- big time
  local tw  = tx(#ts * 14)
  local tx2 = math.max(4, math.floor((cw - tw) / 2))
  cv:text(tx2, 20, ts, chr_run and C.good or C.text, 22)

  -- status
  local stat = chr_run and "RUNNING" or (chr_acc > 0 and "STOPPED" or "READY")
  head(math.floor((cw - #stat * 6) / 2), 52, stat, chr_run and C.good or C.sub)

  -- hints
  head(4, 66, "tap: start/stop   up: lap   down: reset", C.sub)

  -- lap list (last 6)
  local start_lap = math.max(1, #chr_laps - 5)
  local ly = 84
  for i = start_lap, #chr_laps do
    local col = i == #chr_laps and C.text or C.sub
    label(4,  ly, string.format("Lap %d", i), col, 12)
    label(44, ly, chr_laps[i],                col, 12)
    ly = ly + 14
    if ly > ch - 20 then break end
  end
end

-- ── TIMER tab ─────────────────────────────────────────────────────────────
local function tmr_remaining()
  if tmr_end == 0 then return tmr_secs * 1000 end
  local r = tmr_end - sys.millis()
  return r < 0 and 0 or r
end

local function draw_timer(cw, ch)
  local rem  = tmr_remaining()
  local secs = math.floor(rem / 1000)
  local ts   = fmt_mm_ss(secs)
  local col  = tmr_done and C.bad or (tmr_end ~= 0 and C.good or C.text)
  local tw   = tx(#ts * 16)
  local tx2  = math.max(4, math.floor((cw - tw) / 2))
  cv:text(tx2, 20, ts, col, 26)

  -- progress bar
  local total_ms = tmr_secs * 1000
  local pct = (total_ms > 0) and math.floor((total_ms - rem) * (cw - 8) / total_ms) or 0
  if pct > 0 then
    cv:rect(4, 58, pct, 4, tmr_done and C.bad or C.accent, true, 0)
  end
  cv:rect(4, 58, cw - 8, 4, 0x1a1f26, false, 0)

  -- preset
  local pre_s = string.format("Preset: %s", fmt_mm_ss(tmr_secs))
  head(4, 70, pre_s, C.sub)
  head(4, 84, "tap: start/stop   up: cycle preset   down: reset", C.sub)

  if tmr_done then
    local ax = math.floor((cw - 60) / 2)
    cv:rect(ax, 100, 60, 22, 0x1a1f26, true, 4)
    label(ax + 8, 104, "ALARM!", C.bad, 16)
  end
end

-- ── ALTI tab ──────────────────────────────────────────────────────────────
local function draw_alti(cw, ch)
  local gps = sys.gps()
  local alt = gps and gps.alt_m or nil

  -- big altitude
  local ats = alt and string.format("%d m", math.floor(alt)) or "--- m"
  local tw  = tx(#ats * 14)
  cv:text(math.max(4, math.floor((cw - tw) / 2)), 10, ats,
          alt and C.text or C.sub, 24)

  -- min / max
  local mn_s = alti_min < 99999 and string.format("%d m", math.floor(alti_min)) or "---"
  local mx_s = alti_max > -99999 and string.format("%d m", math.floor(alti_max)) or "---"
  head(4, 44, "MIN")
  label(4, 56, mn_s, C.sub, 13)
  head(math.floor(cw / 2) + 4, 44, "MAX")
  label(math.floor(cw / 2) + 4, 56, mx_s, C.sub, 13)

  -- GPS info
  if gps then
    label(4, 76, string.format("Sats %d   %d km/h", gps.sats, math.floor(gps.speed_kmh or 0)),
          gps.sats >= 4 and C.good or C.bad, 11)
  else
    label(4, 76, "No GPS fix", C.bad, 11)
  end

  -- trend graph
  local n = #alti_hist
  if n > 2 then
    local gy = 92
    local gh = ch - gy - 20
    local gw = cw - 8
    cv:rect(4, gy, gw, gh, 0x1a1f26, true, 2)
    -- find range
    local mn2, mx2 = alti_hist[1], alti_hist[1]
    for _, v in ipairs(alti_hist) do
      if v < mn2 then mn2 = v end
      if v > mx2 then mx2 = v end
    end
    local rng = mx2 - mn2
    if rng < 1 then rng = 1 end
    -- draw line as series of small rects
    for i = 2, n do
      local x1 = math.floor(4 + (i - 2) * gw / (HIST_N - 1))
      local x2 = math.floor(4 + (i - 1) * gw / (HIST_N - 1))
      local y1 = math.floor(gy + gh - (alti_hist[i-1] - mn2) * gh / rng)
      local y2 = math.floor(gy + gh - (alti_hist[i]   - mn2) * gh / rng)
      local lx = math.min(x1, x2)
      local lw = math.max(1, math.abs(x2 - x1))
      local ly = math.min(y1, y2)
      local lh = math.max(1, math.abs(y2 - y1))
      cv:rect(lx, ly, lw, lh, C.accent, true, 0)
    end
    -- axis labels
    head(6,  gy + 2,       math.floor(mx2) .. "m", C.sub)
    head(6,  gy + gh - 12, math.floor(mn2) .. "m", C.sub)
  end

  head(4, ch - 14, "down: reset min/max", C.sub)
end

-- ── Tab bar ───────────────────────────────────────────────────────────────
local function draw_tab_bar()
  local tw = math.floor(W / #TAB)
  for i, name in ipairs(TAB) do
    local tx2 = (i - 1) * tw
    if i == tab then
      cv:rect(tx2, H - TAB_H, tw, TAB_H, C.accent, true, 0)
      cv:text(math.floor(tx2 + (tw - #name * 6) / 2), H - TAB_H + 4, name, 0x000000, 10)
    else
      cv:text(math.floor(tx2 + (tw - #name * 6) / 2), H - TAB_H + 4, name, C.sub, 10)
    end
  end
end

-- ── Full redraw ───────────────────────────────────────────────────────────
local function redraw()
  if not cv then return end
  local ch = H - TAB_H
  cv:fill(0x0d1117)
  if     tab == 1 then draw_astro( W, ch)
  elseif tab == 2 then draw_chrono(W, ch)
  elseif tab == 3 then draw_timer( W, ch)
  elseif tab == 4 then draw_alti(  W, ch)
  end
  draw_tab_bar()
end

-- ── App lifecycle ─────────────────────────────────────────────────────────
function app.on_open(w, h)
  W, H = w, h
  cv = ui.canvas(w, h)
  cv:pos(0, 0)
  -- restore persistent state
  alti_min  = tonumber(store.get("alti_min",   99999)) or 99999
  alti_max  = tonumber(store.get("alti_max",  -99999)) or -99999
  tmr_pi    = tonumber(store.get("tmr_pi",          4)) or 4
  if tmr_pi < 1 or tmr_pi > #TMR_PRESETS then tmr_pi = 4 end
  tmr_secs  = TMR_PRESETS[tmr_pi]
  astro_t0  = 0   -- force an immediate astro refresh
  update_astro()
  redraw()
  tmr.every(500)
end

function app.on_tick(dt)
  -- altimeter: sample GPS when on the alti tab
  if tab == 4 then
    local gps = sys.gps()
    if gps and gps.alt_m then
      local a = gps.alt_m
      alti_hi = (alti_hi % HIST_N) + 1
      alti_hist[alti_hi] = a
      if a < alti_min then alti_min = a; store.set("alti_min", alti_min) end
      if a > alti_max then alti_max = a; store.set("alti_max", alti_max) end
    end
  end
  -- timer alarm
  if tmr_end ~= 0 and not tmr_done and sys.millis() >= tmr_end then
    tmr_done = true
    tmr_end  = 0
    sys.beep()
  end
  -- astro refresh (throttled inside update_astro)
  update_astro()
  redraw()
end

function app.on_input(ev)
  if ev.type == "swipe" then
    local d = ev.dir
    if d == "left" then
      tab = (tab % #TAB) + 1
    elseif d == "right" then
      tab = ((tab - 2) % #TAB) + 1
    elseif d == "up" then
      if tab == 2 and chr_run then
        chr_laps[#chr_laps + 1] = fmt_hms(chr_elapsed())
        sys.toast("Lap " .. #chr_laps, 800)
      elseif tab == 3 then
        tmr_pi   = (tmr_pi % #TMR_PRESETS) + 1
        tmr_secs = TMR_PRESETS[tmr_pi]
        store.set("tmr_pi", tmr_pi)
        tmr_end = 0; tmr_done = false
      end
    elseif d == "down" then
      if tab == 2 then
        chr_run  = false; chr_t0 = 0; chr_acc = 0; chr_laps = {}
      elseif tab == 3 then
        tmr_end = 0; tmr_done = false
      elseif tab == 4 then
        alti_min = 99999; alti_max = -99999; alti_hist = {}; alti_hi = 0
        store.set("alti_min", alti_min); store.set("alti_max", alti_max)
        sys.toast("Alt min/max reset", 1000)
      end
    end
    redraw()
  elseif ev.type == "down" then
    if tab == 2 then
      if chr_run then
        chr_acc = chr_elapsed(); chr_run = false
      else
        chr_t0 = sys.millis(); chr_run = true
      end
      redraw()
    elseif tab == 3 then
      if tmr_end ~= 0 then
        tmr_end = 0; tmr_done = false
      elseif tmr_done then
        tmr_done = false
      else
        tmr_end = sys.millis() + tmr_secs * 1000
      end
      redraw()
    end
  end
end

function app.on_close()
  chr_run = false
end

return app
