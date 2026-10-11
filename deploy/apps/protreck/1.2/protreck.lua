-- ProTreck 1.2 — wadamesh outdoor watch
-- 5 tabs: ASTRO · CMPAS · CHRONO · TIMER · ALTI
-- Permanent status bar on every tab: local time · GPS sats · battery %
--
-- ASTRO  : clock, 3 alarms, sun arc + civil twilight, moon disc
--   tap = cycle alarm slot A1/A2/A3  |  double-tap = toggle on/off
--   swipe-up/down = ±15 min on selected alarm
-- CMPAS  : compass rose (magnetometer or GPS course fallback)
--   tap = set / clear waypoint  →  bearing + distance shown on rose
-- CHRONO : tap start/stop · swipe-up lap · swipe-down reset
-- TIMER  : tap start/stop · swipe-up preset cycle · swipe-down reset
-- ALTI   : altitude, min/max, déniv+/−, climb rate m/h, trend graph
--   swipe-down = full reset (min/max + accumulated déniv)
-- Swipe left/right to change tab
-- Contributed by samuelcoustet

local ui, sys, store, tmr = wada.ui, wada.sys, wada.store, wada.timer
local C   = ui.colors
local app = {}

-- ── Constants ─────────────────────────────────────────────────────────────
local TAB      = { "ASTRO", "CMPAS", "CHRONO", "TIMER", "ALTI" }
local TAB_H    = 18
local STATUS_H = 14
local HIST_N   = 60
local PI  = math.pi
local RAD = PI / 180

local COMPASS_MARKS = {  -- {bearing°, label, is_cardinal}
  {   0, "N",  true  }, {  45, "NE", false },
  {  90, "E",  true  }, { 135, "SE", false },
  { 180, "S",  true  }, { 225, "SW", false },
  { 270, "W",  true  }, { 315, "NW", false },
}

local MOON_NAMES = {
  [0]="New",[1]="New",
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
  [27]="Waning Crescent",[28]="Waning Crescent",[29]="New",
}

-- ── Shared display state ───────────────────────────────────────────────────
local tab = 1
local cv, W, H

-- ── Time helpers ──────────────────────────────────────────────────────────
local function now_unix()
  local ep = sys.epoch and sys.epoch()
  if ep then return ep end
  if os then return os.time and os.time() end
  return nil
end

local function decompose(t)
  if os and os.date then return os.date("*t", t) end
  local s   = t % 86400
  local sec  = s % 60;  s = math.floor(s / 60)
  local min2 = s % 60;  local hr = math.floor(s / 60)
  local days = math.floor(t / 86400)
  local y = 1970; local d = days
  while true do
    local yl = ((y%4==0 and y%100~=0) or y%400==0) and 366 or 365
    if d < yl then break end; d = d - yl; y = y + 1
  end
  return { year=y, month=1, day=1, hour=hr, min=min2, sec=sec, yday=d+1 }
end

local function fmt_hms(ms)
  local s  = math.floor(ms / 1000)
  local cs = math.floor((ms % 1000) / 10)
  local m  = math.floor(s / 60); s = s % 60
  local h  = math.floor(m / 60); m = m % 60
  if h > 0 then return string.format("%d:%02d:%02d",    h, m, s)
            else return string.format("%02d:%02d.%02d", m, s, cs) end
end

local function fmt_mm_ss(secs)
  local h = math.floor(secs / 3600)
  local m = math.floor((secs % 3600) / 60)
  local s = secs % 60
  if h > 0 then return string.format("%d:%02d:%02d", h, m, s) end
  return string.format("%02d:%02d", m, s)
end

local function fmt_utch(fh)
  if not fh then return "--:--" end
  local h = math.floor(fh) % 24
  local m = math.floor((fh - math.floor(fh)) * 60 + 0.5)
  if m >= 60 then h = (h+1)%24; m = 0 end
  return string.format("%02d:%02d", h, m)
end

-- ── Astronomical algorithms ────────────────────────────────────────────────
-- Generalised: dep_deg=0 → sunrise, -6 → civil twilight, -12 → nautical
local function sun_utc_dep(lat, lon, yday, dep_deg)
  local B    = RAD * (360/365) * (yday - 81)
  local eqt  = 9.87*math.sin(2*B) - 7.53*math.cos(B) - 1.5*math.sin(B)
  local decl = RAD * 23.45 * math.sin(B)
  local lr   = lat * RAD;  local dr = dep_deg * RAD
  local cha  = (math.sin(dr) - math.sin(lr)*math.sin(decl)) / (math.cos(lr)*math.cos(decl))
  if cha < -1 or cha > 1 then return nil, nil end
  local ha   = math.acos(cha) / RAD
  local noon = 12 - lon/15 - eqt/60
  return noon - ha/15, noon + ha/15
end

local function moon_phase_num(t)
  return math.floor((t/86400 - 10957 - 6.0) % 29.53058867)
end

local function moonrise_set(phase, sr)
  local rise = ((sr or 6) + phase * 24 / 29.5) % 24
  return rise, (rise + 12.4) % 24
end

-- ── Navigation helpers ─────────────────────────────────────────────────────
local function bearing_to(la1, lo1, la2, lo2)
  local dlon = (lo2-lo1)*RAD;  local la1r=la1*RAD;  local la2r=la2*RAD
  local y = math.sin(dlon)*math.cos(la2r)
  local x = math.cos(la1r)*math.sin(la2r) - math.sin(la1r)*math.cos(la2r)*math.cos(dlon)
  return (math.atan(y, x)/RAD + 360) % 360
end

local function distance_m(la1, lo1, la2, lo2)
  local dlat=(la2-la1)*RAD;  local dlon=(lo2-lo1)*RAD
  local a = math.sin(dlat/2)^2 + math.cos(la1*RAD)*math.cos(la2*RAD)*math.sin(dlon/2)^2
  return 6371000 * 2 * math.atan(math.sqrt(a), math.sqrt(math.max(0, 1-a)))
end

local CARDINALS = {"N","NNE","NE","ENE","E","ESE","SE","SSE",
                   "S","SSW","SW","WSW","W","WNW","NW","NNW"}
local function cardinal(deg)
  return CARDINALS[math.floor(((deg%360)+11.25)/22.5)%16 + 1]
end

-- ── Astro state ────────────────────────────────────────────────────────────
local astro_t0  = 0
local astro_sr, astro_ss          -- sunrise/sunset UTC frac hours
local astro_cr, astro_cs          -- civil twilight
local astro_mr, astro_ms          -- moonrise/set (approx)
local astro_phase = 0
local astro_lat, astro_lon
local astro_tz = 0                -- rough UTC offset for local clock

local function update_astro()
  local now = sys.millis()
  if now - astro_t0 < 60000 then return end
  astro_t0 = now
  local t   = now_unix()
  local gps = sys.gps()
  if gps and t then
    local d       = decompose(t)
    local yday    = d.yday or 182
    astro_lat     = gps.lat;  astro_lon = gps.lon
    astro_tz      = math.floor(gps.lon/15 + 0.5)
    astro_sr, astro_ss = sun_utc_dep(gps.lat, gps.lon, yday,  0)
    astro_cr, astro_cs = sun_utc_dep(gps.lat, gps.lon, yday, -6)
    astro_phase        = moon_phase_num(t)
    astro_mr, astro_ms = moonrise_set(astro_phase, astro_sr)
  end
end

-- ── Alarm state ────────────────────────────────────────────────────────────
local alarms = {
  { h=7, m=0, on=0, fire=false },
  { h=8, m=0, on=0, fire=false },
  { h=9, m=0, on=0, fire=false },
}
local alarm_sel   = 1
local alarm_prest = 0    -- millis when "down" was received on ASTRO tab

local function alarm_adjust(delta_min)
  local al  = alarms[alarm_sel]
  local tot = al.h*60 + al.m + delta_min
  tot  = ((tot % (24*60)) + 24*60) % (24*60)
  al.h = math.floor(tot/60);  al.m = tot % 60
  store.set("al"..alarm_sel.."h", al.h)
  store.set("al"..alarm_sel.."m", al.m)
end

-- ── Compass / waypoint state ───────────────────────────────────────────────
local wpt_lat, wpt_lon = nil, nil

-- ── Chrono state ───────────────────────────────────────────────────────────
local chr_run = false;  local chr_t0 = 0;  local chr_acc = 0;  local chr_laps = {}

local function chr_elapsed()
  return chr_run and chr_acc + (sys.millis() - chr_t0) or chr_acc
end

-- ── Timer state ────────────────────────────────────────────────────────────
local TMR_PRESETS = {30,60,120,300,600,1800,3600}
local tmr_pi   = 4;  local tmr_secs = 300;  local tmr_end = 0;  local tmr_done = false

local function tmr_rem()
  if tmr_end == 0 then return tmr_secs*1000 end
  local r = tmr_end - sys.millis();  return r < 0 and 0 or r
end

-- ── Altimeter state ────────────────────────────────────────────────────────
local alti_min    = 99999;  local alti_max = -99999;  local alti_hist = {}
local alti_dplus  = 0;      local alti_dminus = 0;    local alti_prev = nil

-- ── Status bar ────────────────────────────────────────────────────────────
local function draw_status_bar(cw)
  cv:rect(0, 0, cw, STATUS_H, 0x0d1520, true, 0)
  cv:rect(0, STATUS_H-1, cw, 1, 0x1e2a38, true, 0)
  -- local time + UTC offset
  local t = now_unix();  local d = t and decompose(t) or nil
  if d then
    local lh = (d.hour + astro_tz + 24) % 24
    cv:text(4, 2, string.format("%02d:%02d", lh, d.min), C.text, 10)
    cv:text(44, 2, string.format("UTC%+d", astro_tz), C.sub, 9)
  else
    cv:text(4, 2, "--:--", C.sub, 10)
  end
  -- GPS sats
  local gps = sys.gps()
  local gcol = gps and gps.sats >= 4 and C.good or C.bad
  cv:text(math.floor(cw/2)-12, 2, gps and ("G:"..gps.sats) or "G:--", gcol, 10)
  -- battery
  local bat = sys.battery and sys.battery()
  if bat then
    local pct = math.floor(bat.pct or 0)
    cv:text(cw-28, 2, pct.."%", pct > 20 and C.good or C.bad, 10)
  end
end

-- ── Sun arc diagram ────────────────────────────────────────────────────────
local function draw_sun_arc(cx, hy, r, cur_h)
  for step = 0, 22 do
    local a = PI - step/22*PI
    cv:rect(math.floor(cx+(r+3)*math.cos(a)), math.floor(hy-(r+3)*math.sin(a)), 2, 2, 0x1e2d40, true, 0)
  end
  cv:rect(cx-r-8, hy, r*2+16, 1, C.sub, true, 0)              -- horizon
  if astro_cr then
    cv:rect(cx-r-10, hy-3, 1, 6, 0x4466aa, true, 0)           -- civil rise tick
    cv:rect(cx+r+ 9, hy-3, 1, 6, 0x4466aa, true, 0)           -- civil set tick
  end
  if astro_sr and astro_ss then
    cv:rect(cx-r-3, hy-8, 1, 16, C.good, true, 0)             -- rise tick
    cv:rect(cx+r+2, hy-8, 1, 16, C.bad,  true, 0)             -- set tick
    if cur_h and cur_h >= astro_sr and cur_h <= astro_ss then
      local prog = (cur_h - astro_sr) / (astro_ss - astro_sr)
      local a  = PI - prog*PI
      local sx = math.floor(cx + r*math.cos(a))
      local sy = math.floor(hy - r*math.sin(a))
      cv:circle(sx, sy, 5, 0xffcc00, true, 0)
      cv:circle(sx, sy, 5, 0xff9900, false, 1)
    else
      cv:circle(cx, hy+9, 4, 0x334455, true, 0)               -- below horizon
    end
  else
    cv:circle(cx, hy - math.floor(r/2), 4, 0x445566, true, 0) -- polar
  end
end

-- ── Moon phase disc (exact terminator scan-lines) ─────────────────────────
local function draw_moon_disc(cx, cy, r, phase)
  local phi  = phase / 29.5 * 2 * PI
  local drk  = 0x1a2433;  local lit = 0xdde8f8
  local cosp = math.cos(phi)
  for yi = -r, r-1, 2 do
    local chord = math.floor(math.sqrt(r*r - yi*yi) + 0.5)
    if chord > 0 then
      cv:rect(cx-chord, cy+yi, chord*2, 2, drk, true, 0)
      local term = math.floor(cosp * chord + 0.5)
      local lx, lw
      if phi <= PI then lx = cx+term;   lw = chord-term
                   else lx = cx-chord;  lw = chord-term end
      if lw > 0 then cv:rect(lx, cy+yi, lw, 2, lit, true, 0) end
    end
  end
  cv:circle(cx, cy, r, 0x4a5f70, false, 1)
end

-- ── ASTRO tab ─────────────────────────────────────────────────────────────
local function draw_astro(cw, ch)
  draw_status_bar(cw)
  local t     = now_unix();  local d = t and decompose(t) or nil
  local cur_h = d and (d.hour + d.min/60 + d.sec/3600) or nil
  local SY    = STATUS_H + 2
  local col_w = math.floor(cw/2)
  local cx_r  = math.floor(cw*3/4)
  local arc_r = math.min(34, math.floor(col_w*0.42))
  local moo_r = math.min(20, math.floor(col_w*0.24))

  -- ── Alarms row ────────────────────────────────────────────────────────
  for i = 1, 3 do
    local al  = alarms[i]
    local sel = i == alarm_sel
    local col = al.on==1 and C.good or (sel and C.accent or C.sub)
    local ax  = 4 + (i-1)*math.floor((cw-8)/3)
    cv:text(ax,    SY,    "A"..i,                              sel and C.accent or C.sub, 9)
    cv:text(ax,    SY+10, string.format("%02d:%02d",al.h,al.m), col, 11)
    cv:circle(ax+40, SY+14, 3, al.on==1 and C.good or 0x223344, true, 0)
  end
  local SEP1 = SY + 30
  cv:rect(0, SEP1, cw, 1, 0x1e2a38, true, 0)

  -- ── Sun ───────────────────────────────────────────────────────────────
  local SUN_Y = SEP1 + 4
  cv:text(4, SUN_Y, "SUN", C.accent, 11)
  if astro_sr then
    cv:text(4, SUN_Y+13, "Rise "..fmt_utch(astro_sr), C.text, 11)
    cv:text(4, SUN_Y+25, "Set  "..fmt_utch(astro_ss), C.text, 11)
    if astro_cr then
      cv:text(4, SUN_Y+37, "Twl  "..fmt_utch(astro_cr), 0x5588cc, 10)
    end
  elseif astro_lat then cv:text(4, SUN_Y+13, "Polar", C.sub, 11)
  else              cv:text(4, SUN_Y+13, "No GPS", C.bad, 11) end

  local ARC_HY = SUN_Y + 40
  draw_sun_arc(cx_r, ARC_HY, arc_r, cur_h)
  if astro_sr then
    cv:text(cx_r-arc_r-8, ARC_HY+6, fmt_utch(astro_sr), C.good, 9)
    cv:text(cx_r+arc_r+2, ARC_HY+6, fmt_utch(astro_ss), C.bad,  9)
  end

  local SEP2 = ARC_HY + 18
  cv:rect(0, SEP2, cw, 1, 0x1e2a38, true, 0)

  -- ── Moon ──────────────────────────────────────────────────────────────
  local MOO_Y = SEP2 + 4
  cv:text(4, MOO_Y, "MOON", C.accent, 11)
  if astro_lat and t then
    cv:text(4, MOO_Y+13, MOON_NAMES[astro_phase] or "?",        C.text, 11)
    cv:text(4, MOO_Y+25, "Rise "..fmt_utch(astro_mr),            C.sub,  10)
    cv:text(4, MOO_Y+36, "Set  "..fmt_utch(astro_ms),            C.sub,  10)
    cv:text(4, MOO_Y+47, "Phase "..astro_phase.."/29",            C.sub,  10)
  else
    cv:text(4, MOO_Y+13, "No GPS+time", C.bad, 11)
  end
  draw_moon_disc(cx_r, math.floor(MOO_Y+30), moo_r, astro_phase)

  -- GPS footer
  local gps = sys.gps()
  if gps then cv:text(4, ch-12, string.format("%.3f,%.3f  %dm", gps.lat, gps.lon, math.floor(gps.alt_m or 0)), C.sub, 10)
  else        cv:text(4, ch-12, "GPS: no fix", C.bad, 10) end
end

-- ── CMPAS tab ─────────────────────────────────────────────────────────────
local function get_heading()
  local c = sys.compass and sys.compass()
  if c and c.hdg then return c.hdg end
  local gps = sys.gps()
  if gps and (gps.speed_kmh or 0) > 2 then return gps.course end
  return nil
end

local function draw_cmpas(cw, ch)
  draw_status_bar(cw)
  local hdg = get_heading()
  local cx  = math.floor(cw/2)
  local r   = math.min(68, math.floor(math.min(cw, ch-STATUS_H-56)/2) - 4)
  local cy  = STATUS_H + r + 14

  cv:circle(cx, cy, r,   0x151e2a, true, 0)
  cv:circle(cx, cy, r,   C.sub,    false, 1)
  cv:circle(cx, cy, r-5, 0x0d1117, true, 0)

  if hdg then
    -- rotating rose
    for _, mk in ipairs(COMPASS_MARKS) do
      local sa   = (mk[1] - hdg) * RAD
      local sins = math.sin(sa);  local coss = math.cos(sa)
      local tk   = mk[3] and 9 or 5
      local ox = math.floor(cx +  r    * sins);  local oy = math.floor(cy -  r    * coss)
      local ix = math.floor(cx + (r-tk)* sins);  local iy = math.floor(cy - (r-tk)* coss)
      local tc = mk[1] == 0 and C.bad or (mk[3] and C.text or C.sub)
      cv:line(ix, iy, ox, oy, tc, mk[3] and 2 or 1)
      if mk[3] then
        local lx = math.floor(cx + (r+10)*sins)
        local ly = math.floor(cy - (r+10)*coss)
        cv:text(lx-3, ly-5, mk[2], tc, 10)
      end
    end
    -- needle: red toward N, grey toward S
    local na  = (-hdg)*RAD
    local nx = math.floor(cx + (r-6)*math.sin(na));  local ny = math.floor(cy - (r-6)*math.cos(na))
    local sx = math.floor(cx - (r/3)*math.sin(na));  local sy = math.floor(cy + (r/3)*math.cos(na))
    cv:line(cx, cy, nx, ny, 0xff3333, 2)
    cv:line(cx, cy, sx, sy, C.sub,    2)
    cv:circle(cx, cy, 4, C.text, true, 0)
    cv:rect(cx-2, cy-r-10, 4, 8, C.accent, true, 2)   -- fixed course indicator

    -- heading readout
    local hy2 = cy + r + 8
    cv:text(cx-18, hy2,   string.format("%d", math.floor(hdg)), C.text, 18)
    cv:text(cx+22, hy2+4, cardinal(hdg), C.sub, 12)

    -- waypoint line on rose + info
    local gps = sys.gps()
    if wpt_lat and gps then
      local brg    = bearing_to(gps.lat, gps.lon, wpt_lat, wpt_lon)
      local dist_m = distance_m(gps.lat, gps.lon, wpt_lat, wpt_lon)
      local wa  = (brg - hdg)*RAD
      local wx  = math.floor(cx + (r-3)*math.sin(wa))
      local wy  = math.floor(cy - (r-3)*math.cos(wa))
      cv:line(cx, cy, wx, wy, C.accent, 2)
      cv:circle(wx, wy, 4, C.accent, true, 0)
      local dist_s = dist_m >= 1000
        and string.format("WPT %.1fkm / %d° %s", dist_m/1000, math.floor(brg), cardinal(brg))
        or  string.format("WPT %dm / %d° %s",    math.floor(dist_m), math.floor(brg), cardinal(brg))
      cv:text(4, ch-24, dist_s, C.accent, 11)
    end
  else
    cv:text(cx-24, cy-8,  "No signal", C.sub, 14)
  end

  local gps = sys.gps()
  local hint = wpt_lat and "tap: clear waypoint"
            or (gps and "tap: save waypoint here" or "tap: (need GPS)")
  cv:text(4, ch-12, hint, C.sub, 10)
end

-- ── CHRONO tab ────────────────────────────────────────────────────────────
local function draw_chrono(cw, ch)
  draw_status_bar(cw)
  local SY = STATUS_H + 8
  local ms  = chr_elapsed()
  local ts  = fmt_hms(ms)
  local tw  = math.floor(#ts*14)
  cv:text(math.max(4, math.floor((cw-tw)/2)), SY, ts, chr_run and C.good or C.text, 22)
  local stat = chr_run and "RUNNING" or (chr_acc > 0 and "STOPPED" or "READY")
  cv:text(math.floor((cw-#stat*6)/2), SY+34, stat, chr_run and C.good or C.sub, 11)
  cv:text(4, SY+48, "tap: start/stop   up: lap   down: reset", C.sub, 11)
  local start_i = math.max(1, #chr_laps-5)
  local ly = SY + 66
  for i = start_i, #chr_laps do
    local col = i==#chr_laps and C.text or C.sub
    cv:text(4,  ly, string.format("Lap %d", i), col, 12)
    cv:text(44, ly, chr_laps[i],                col, 12)
    ly = ly + 14;  if ly > ch-20 then break end
  end
end

-- ── TIMER tab ─────────────────────────────────────────────────────────────
local function draw_timer(cw, ch)
  draw_status_bar(cw)
  local SY  = STATUS_H + 8
  local rem  = tmr_rem()
  local ts   = fmt_mm_ss(math.floor(rem/1000))
  local col  = tmr_done and C.bad or (tmr_end~=0 and C.good or C.text)
  local tw   = math.floor(#ts*16)
  cv:text(math.max(4, math.floor((cw-tw)/2)), SY, ts, col, 26)
  local total = tmr_secs*1000
  local pct   = total>0 and math.floor((total-rem)*(cw-8)/total) or 0
  if pct > 0 then cv:rect(4, SY+38, pct, 4, tmr_done and C.bad or C.accent, true, 0) end
  cv:rect(4, SY+38, cw-8, 4, 0x1a1f26, false, 0)
  cv:text(4, SY+50, "Preset: "..fmt_mm_ss(tmr_secs), C.sub, 11)
  cv:text(4, SY+64, "tap: start/stop   up: preset   down: reset", C.sub, 11)
  if tmr_done then
    local ax = math.floor((cw-60)/2)
    cv:rect(ax, SY+80, 60, 22, 0x1a1f26, true, 4)
    cv:text(ax+8, SY+84, "ALARM!", C.bad, 16)
  end
end

-- ── ALTI tab ──────────────────────────────────────────────────────────────
local function draw_alti(cw, ch)
  draw_status_bar(cw)
  local SY  = STATUS_H + 2
  local gps = sys.gps()
  local alt = gps and gps.alt_m or nil
  local ats = alt and string.format("%d m", math.floor(alt)) or "--- m"
  cv:text(math.max(4, math.floor((cw-#ats*14)/2)), SY, ats, alt and C.text or C.sub, 24)

  -- min / max
  local mn_s = alti_min<99999  and string.format("%dm",math.floor(alti_min))  or "---"
  local mx_s = alti_max>-99999 and string.format("%dm",math.floor(alti_max))  or "---"
  cv:text(4,                   SY+32, "MIN", C.sub, 10)
  cv:text(4,                   SY+42, mn_s,  C.sub, 12)
  cv:text(math.floor(cw/3)+4,  SY+32, "MAX", C.sub, 10)
  cv:text(math.floor(cw/3)+4,  SY+42, mx_s,  C.sub, 12)

  -- déniv
  local dp_s = string.format("+%d -%dm", math.floor(alti_dplus), math.floor(alti_dminus))
  cv:text(math.floor(cw*2/3)+4, SY+32, "D+/-", C.sub, 10)
  cv:text(math.floor(cw*2/3)+4, SY+42, dp_s,   C.sub, 11)

  -- climb rate (last 12 samples = 6 s at 500ms tick)
  local n = #alti_hist
  if n >= 2 then
    local from = math.max(1, n-12)
    local rate = (alti_hist[n] - alti_hist[from]) * 7200 / (n - from)
    local rate_s = string.format("%+d m/h", math.floor(rate))
    local rcol = math.abs(rate)<30 and C.sub or (rate>0 and C.good or 0x6699ff)
    cv:text(4, SY+58, rate_s, rcol, 12)
    if gps then
      cv:text(60, SY+58, string.format("  Sats %d  %dkm/h", gps.sats,
                                        math.floor(gps.speed_kmh or 0)),
              gps.sats>=4 and C.good or C.bad, 11)
    end
  elseif gps then
    cv:text(4, SY+58, string.format("Sats %d", gps.sats), gps.sats>=4 and C.good or C.bad, 11)
  end

  -- trend graph
  if n > 2 then
    local gy = SY + 74
    local gh = ch - gy - 20
    local gw = cw - 8
    cv:rect(4, gy, gw, gh, 0x1a1f26, true, 2)
    local mn2, mx2 = alti_hist[1], alti_hist[1]
    for _, v in ipairs(alti_hist) do if v<mn2 then mn2=v end; if v>mx2 then mx2=v end end
    local rng = mx2-mn2;  if rng < 1 then rng = 1 end
    for i = 2, n do
      local x1=math.floor(4+(i-2)*gw/(HIST_N-1));  local x2=math.floor(4+(i-1)*gw/(HIST_N-1))
      local y1=math.floor(gy+gh-(alti_hist[i-1]-mn2)*gh/rng)
      local y2=math.floor(gy+gh-(alti_hist[i]  -mn2)*gh/rng)
      cv:rect(math.min(x1,x2), math.min(y1,y2),
              math.max(1,math.abs(x2-x1)), math.max(1,math.abs(y2-y1)), C.accent, true, 0)
    end
    cv:text(6, gy+2,      math.floor(mx2).."m", C.sub, 10)
    cv:text(6, gy+gh-12,  math.floor(mn2).."m", C.sub, 10)
  end

  cv:text(4, ch-12, "down: reset min/max + deniv", C.sub, 10)
end

-- ── Tab bar + redraw ──────────────────────────────────────────────────────
local function draw_tab_bar()
  local tw = math.floor(W / #TAB)
  for i, name in ipairs(TAB) do
    local tx = (i-1)*tw
    if i == tab then
      cv:rect(tx, H-TAB_H, tw, TAB_H, C.accent, true, 0)
      cv:text(math.floor(tx+(tw-#name*6)/2), H-TAB_H+4, name, 0x000000, 10)
    else
      cv:text(math.floor(tx+(tw-#name*6)/2), H-TAB_H+4, name, C.sub, 10)
    end
  end
end

local function redraw()
  if not cv then return end
  local ch = H - TAB_H
  cv:fill(0x0d1117)
  if     tab==1 then draw_astro( W, ch)
  elseif tab==2 then draw_cmpas( W, ch)
  elseif tab==3 then draw_chrono(W, ch)
  elseif tab==4 then draw_timer( W, ch)
  elseif tab==5 then draw_alti(  W, ch)
  end
  draw_tab_bar()
end

-- ── App lifecycle ─────────────────────────────────────────────────────────
function app.on_open(w, h)
  W, H = w, h
  cv = ui.canvas(w, h);  cv:pos(0, 0)
  -- restore alarms
  for i = 1, 3 do
    alarms[i].h  = tonumber(store.get("al"..i.."h",  alarms[i].h))  or alarms[i].h
    alarms[i].m  = tonumber(store.get("al"..i.."m",  alarms[i].m))  or alarms[i].m
    alarms[i].on = tonumber(store.get("al"..i.."on", 0))            or 0
  end
  alti_min = tonumber(store.get("alti_min",   99999)) or  99999
  alti_max = tonumber(store.get("alti_max",  -99999)) or -99999
  tmr_pi   = tonumber(store.get("tmr_pi",         4)) or 4
  if tmr_pi < 1 or tmr_pi > #TMR_PRESETS then tmr_pi = 4 end
  tmr_secs = TMR_PRESETS[tmr_pi]
  -- restore waypoint
  local wlat = tonumber(store.get("wpt_lat", ""))
  local wlon = tonumber(store.get("wpt_lon", ""))
  if wlat and wlon then wpt_lat = wlat;  wpt_lon = wlon end
  astro_t0 = 0;  update_astro();  redraw();  tmr.every(500)
end

function app.on_tick(dt)
  -- altimeter sampling (all tabs, not just ALTI, to accumulate déniv while exploring)
  local gps = sys.gps()
  if gps and gps.alt_m then
    local a = gps.alt_m
    alti_hist[#alti_hist+1] = a
    if #alti_hist > HIST_N then table.remove(alti_hist, 1) end
    if a < alti_min then alti_min = a;  store.set("alti_min", alti_min) end
    if a > alti_max then alti_max = a;  store.set("alti_max", alti_max) end
    if alti_prev then
      local diff = a - alti_prev
      if diff > 0.5 then alti_dplus  = alti_dplus  + diff
      elseif diff < -0.5 then alti_dminus = alti_dminus - diff end
    end
    alti_prev = a
  end
  -- countdown timer
  if tmr_end ~= 0 and not tmr_done and sys.millis() >= tmr_end then
    tmr_done = true;  tmr_end = 0;  sys.beep()
  end
  -- alarm clock check
  local t = now_unix();  local dc = t and decompose(t) or nil
  if dc then
    for _, al in ipairs(alarms) do
      if al.on==1 and dc.hour==al.h and dc.min==al.m then
        if not al.fire then
          al.fire = true;  sys.beep()
          sys.toast(string.format("Alarm %02d:%02d", al.h, al.m), 4000)
        end
      else
        al.fire = false
      end
    end
  end
  update_astro();  redraw()
end

function app.on_input(ev)
  if ev.type == "swipe" then
    local d = ev.dir
    if     d == "left"  then tab = (tab % #TAB) + 1
    elseif d == "right" then tab = ((tab-2) % #TAB) + 1
    elseif d == "up" then
      if tab==1 then alarm_adjust(15)
      elseif tab==3 and chr_run then
        chr_laps[#chr_laps+1] = fmt_hms(chr_elapsed())
        sys.toast("Lap "..(#chr_laps), 800)
      elseif tab==4 then
        tmr_pi = (tmr_pi%#TMR_PRESETS)+1;  tmr_secs = TMR_PRESETS[tmr_pi]
        store.set("tmr_pi", tmr_pi);  tmr_end=0;  tmr_done=false
      end
    elseif d == "down" then
      if tab==1 then alarm_adjust(-15)
      elseif tab==3 then chr_run=false;  chr_t0=0;  chr_acc=0;  chr_laps={}
      elseif tab==4 then tmr_end=0;  tmr_done=false
      elseif tab==5 then
        alti_min=99999;  alti_max=-99999;  alti_hist={}
        alti_dplus=0;  alti_dminus=0;  alti_prev=nil
        store.set("alti_min",alti_min);  store.set("alti_max",alti_max)
        sys.toast("Alt + déniv reset", 1000)
      end
    end
    redraw()
  elseif ev.type == "down" then
    if tab == 1 then
      alarm_prest = sys.millis()   -- start timing for long-press detection
    elseif tab == 2 then
      local gps2 = sys.gps()
      if wpt_lat then
        wpt_lat=nil;  wpt_lon=nil
        store.set("wpt_lat","");  store.set("wpt_lon","")
        sys.toast("Waypoint cleared", 800)
      elseif gps2 then
        wpt_lat=gps2.lat;  wpt_lon=gps2.lon
        store.set("wpt_lat", wpt_lat);  store.set("wpt_lon", wpt_lon)
        sys.toast(string.format("WPT %.4f,%.4f saved", wpt_lat, wpt_lon), 1200)
      end
      redraw()
    elseif tab == 3 then
      if chr_run then chr_acc=chr_elapsed();  chr_run=false
      else            chr_t0=sys.millis();    chr_run=true end
      redraw()
    elseif tab == 4 then
      if tmr_end~=0 then tmr_end=0;  tmr_done=false
      elseif tmr_done then tmr_done=false
      else tmr_end=sys.millis()+tmr_secs*1000 end
      redraw()
    end
  elseif ev.type == "up" then
    -- ASTRO: short release = cycle alarm slot; long hold (≥500ms) = toggle on/off
    if tab == 1 then
      local dur = sys.millis() - alarm_prest
      if dur >= 500 then
        local al = alarms[alarm_sel]
        al.on = al.on==1 and 0 or 1
        store.set("al"..alarm_sel.."on", al.on)
      else
        alarm_sel = (alarm_sel % 3) + 1
      end
      alarm_prest = 0
      redraw()
    end
  end
end

function app.on_close()  chr_run = false  end

return app
