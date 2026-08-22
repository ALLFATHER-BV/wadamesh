-- GPS Compass — heading dial plus the live GPS fix, in the RF Monitor's look.
--
-- Heading comes from the magnetometer where the board has one (ThinkNode M9:
-- QMC6309, via wada.sys.compass()) and falls back to GPS course-over-ground
-- while moving on every other board. The dial turns so the heading sits
-- under the fixed lubber mark at the top; a contact with a known position
-- can be picked as a target and is drawn on the dial with bearing and range.
--
-- The magnetometer is raw: the firmware hands out x/y/z in Gauss in the
-- sensor's own frame, uncalibrated. This app does the rest —
--   * hard-iron calibration: press C (or Cal), turn the device through every
--     orientation for ~20 s, press C again. Offsets persist in wada.store.
--   * orientation: the sensor's axes vs. the screen are not documented for
--     the M9, so the app works it out. Hold the device flat, point the top
--     edge at north and press A. Which way the sensor's Z axis faces decides
--     whether the heading runs clockwise or anticlockwise, and that follows
--     from the sign of the vertical field: Earth's field dips DOWN in the
--     northern hemisphere and UP in the southern one, so with a GPS fix (or
--     the last known position) the app reads the handedness off the sensor
--     itself and only needs the one press for the rest. Both persist.
-- Magnetic declination is not applied: this is a magnetic compass.
local ui, sys, mesh, store, timer = wada.ui, wada.sys, wada.mesh, wada.store, wada.timer
local C = ui.colors
local AMBER  = 0xE8A33D
local RING   = 0x3A424A        -- dial ring + minor ticks
local RING2  = 0x252C33        -- inner ring
local PANEL  = C.panel or 0x15181B   -- the dial's own surface (older hosts: literal)
local app = {}

local caps, W, H
local landscape
local cv, D, R, CX, CY            -- dial canvas, diameter, radius, centre
local dial_x, dial_y = 0, 0       -- canvas position in the body (tap hit-test)
local sats_cv, SATS_W, SATS_H = nil, 52, 8
local TH12, TH14, TH16 = 15, 17, 19   -- font line heights, replaced from ui.text_h
local L = {}                      -- labels by name

local has_compass = false
local cal = nil                   -- { ox, oy, oz } hard-iron offsets
local calib = nil                 -- in-progress: { mn = {x,y,z}, mx = {x,y,z}, t0 }
local align = 0                   -- degrees added to the raw angle so north reads 000
local mirror = false              -- sensor Z faces into the screen: heading runs the other way
local mag_norm = nil              -- |B| after offsets, Gauss (sanity check for the user)
local mag_z = 0                   -- vertical component after offsets (handedness + dip)
local mag_sat = false             -- last magnetometer sample was flagged as saturated

local hv_x, hv_y = 0, 0           -- smoothed heading unit vector
local heading = nil               -- degrees 0..360, or nil
local src = "none"                -- "mag" | "gps" | "none"
local last_mag_ms = 0
local TICK_MS = 100               -- dial update rate
local SMOOTH = 0.5                -- per-tick blend toward the new heading (1 = none)

local targets, target_i = {}, 0   -- contacts with a position; 0 = none
local next_contacts_ms = 0
local CONTACTS_EVERY = 20000      -- positions only change on adverts

-- Units: altitude and speed each carry their own, imperial by default. Up and
-- down move the selection between the two rows; OK (or a tap on the row)
-- switches that row's units.
local UNITS = { alt = true, spd = true }   -- true = imperial
local sel = "alt"
local row_hit = {}                -- name -> { y, h } in body coordinates
local col_x0, col_x1 = 0, 0       -- the stats column's horizontal span

local diag = false                -- D: show what the app is actually computing
local diag_m = nil                -- last raw sample, for the diagnostic rows

local press = nil                 -- pending touch: { x, y } from the last "down"
local last_swipe_ms = -100000     -- debounce: LVGL's gesture and the hardware swipe
                                  -- detector can both report one finger swipe
local name_max = 16               -- target-name characters that fit the value column
local compact = false             -- narrow column at a big font: short strings
local last_text = {}              -- label text cache: LVGL relayout only on change
local last_dial_key, last_sats_key = nil, nil
local CAL_SECS = 20

local CARD = { "N","NNE","NE","ENE","E","ESE","SE","SSE",
               "S","SSW","SW","WSW","W","WNW","NW","NNW" }
local function cardinal(deg)
  return CARD[(math.floor((deg + 11.25) / 22.5) % 16) + 1]
end

local function norm360(d)
  d = d % 360
  if d < 0 then d = d + 360 end
  return d
end

-- ---------------------------------------------------------------------------
-- persistence
local function load_prefs()
  local ox, oy, r = store.get("cal_ox"), store.get("cal_oy"), store.get("cal_r")
  if type(ox) == "number" and type(oy) == "number" then
    cal = { ox = ox, oy = oy, r = (type(r) == "number" and r > 0) and r or nil }
  end
  -- Orientation settings are versioned: the heading formula changed once the
  -- M9's axes were measured, so values saved against the old one would push a
  -- correct default back off north. Anything older is discarded, not migrated.
  if store.get("orient_ver", 0) == 2 then
    align = tonumber(store.get("align", 0)) or 0
    mirror = (store.get("mirror", 0) == 1)
  else
    align, mirror = 0, false
    store.set("align", nil); store.set("mirror", nil)
    store.set("orient", nil); store.set("flip", nil)   -- the even older pair
    store.set("orient_ver", 2)
  end
  UNITS.alt = store.get("u_alt", 1) == 1
  UNITS.spd = store.get("u_spd", 1) == 1
end

local function save_align()
  store.set("align", math.floor(align + 0.5))
  store.set("mirror", mirror and 1 or 0)
  store.set("orient_ver", 2)
end

local function save_cal()
  if cal then
    store.set("cal_ox", cal.ox); store.set("cal_oy", cal.oy); store.set("cal_r", cal.r)
  else
    store.set("cal_ox", nil); store.set("cal_oy", nil); store.set("cal_r", nil)
  end
  store.set("cal_oz", nil)   -- the 3D fit's third offset; nothing reads it now
end

-- ---------------------------------------------------------------------------
-- calibration: least-squares circle fit from a FLAT, IN-PLACE turn
--
-- Turning the device flat traces a circle in the sensor's x/y plane whose
-- CENTRE is the hard-iron offset and whose radius is the local horizontal
-- field. Only that centre is needed for a heading, so this fits exactly it.
--
-- Why flat-and-in-place rather than tumbling the device by hand: hard-iron
-- calibration assumes the device ROTATES in a uniform field. Carrying it
-- through the air around a desk also TRANSLATES it through the field of the
-- laptop, the desk frame and anything else ferrous, which corrupts the fit --
-- measured here as a centre that moved 0.15 G between sessions, which is more
-- than half the entire horizontal signal. Rotated flat on one spot, the same
-- sensor fitted a circle to within 4%.
--
-- |p - c|^2 = r^2 is linear in (c, k = r^2 - |c|^2), so this is a 3x3 solve
-- over running sums: no sample storage, which matters in a 256 KB app heap.
-- Sums are kept relative to the first sample because Lua here is
-- single-precision and squaring raw values near 3.5 G throws away the
-- precision the fit depends on.
local function calib_new()
  return { n = 0, o = nil, t0 = sys.millis(),
           sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0, sxs = 0, sys_ = 0, ss = 0,
           mnx = 1e9, mxx = -1e9, mny = 1e9, mxy = -1e9 }
end

local function calib_add(m)
  local c = calib
  if not c.o then c.o = { m.x, m.y } end
  local x, y = m.x - c.o[1], m.y - c.o[2]
  local s = x * x + y * y
  c.n = c.n + 1
  c.sx = c.sx + x; c.sy = c.sy + y
  c.sxx = c.sxx + x * x; c.syy = c.syy + y * y; c.sxy = c.sxy + x * y
  c.sxs = c.sxs + x * s; c.sys_ = c.sys_ + y * s; c.ss = c.ss + s
  if x < c.mnx then c.mnx = x end; if x > c.mxx then c.mxx = x end
  if y < c.mny then c.mny = y end; if y > c.mxy then c.mxy = y end
end

local function solve3(M, v)
  for col = 1, 3 do
    local piv, best = col, math.abs(M[col][col])
    for r = col + 1, 3 do
      local a = math.abs(M[r][col])
      if a > best then piv, best = r, a end
    end
    if best < 1e-9 then return nil end
    if piv ~= col then M[col], M[piv] = M[piv], M[col]; v[col], v[piv] = v[piv], v[col] end
    local d = M[col][col]
    for r = col + 1, 3 do
      local f = M[r][col] / d
      if f ~= 0 then
        for k = col, 3 do M[r][k] = M[r][k] - f * M[col][k] end
        v[r] = v[r] - f * v[col]
      end
    end
  end
  local out = {}
  for r = 3, 1, -1 do
    local acc = v[r]
    for k = r + 1, 3 do acc = acc - M[r][k] * out[k] end
    out[r] = acc / M[r][r]
  end
  return out
end

-- Returns { ox, oy, r } or nil plus the reason it was refused. Every refusal
-- leaves the previous calibration in place: a silently-accepted bad fit is
-- worse than no new one, and that is exactly how the last one went wrong.
local function calib_solve()
  local c = calib
  if c.n < 60 then return nil, "too few readings - turn slower" end
  local M = {
    { 4 * c.sxx, 4 * c.sxy, 2 * c.sx },
    { 4 * c.sxy, 4 * c.syy, 2 * c.sy },
    { 2 * c.sx,  2 * c.sy,  c.n },
  }
  local v = { 2 * c.sxs, 2 * c.sys_, c.ss }
  local sol = solve3(M, v)
  if not sol then return nil, "turn it right around" end
  local cx, cy, k = sol[1], sol[2], sol[3]
  local r2 = k + cx * cx + cy * cy
  if r2 <= 0 then return nil, "turn it right around" end
  local r = math.sqrt(r2)
  -- Earth's HORIZONTAL field is 0.08 G near the poles and 0.41 G at the
  -- magnetic equator. Outside that the fit has locked onto something else.
  if r < 0.08 or r > 0.45 then return nil, string.format("field reads %.2f G", r) end
  -- Coverage: a full turn makes each axis span 2r. Much less than that means
  -- an arc, and an arc puts the centre almost anywhere.
  if (c.mxx - c.mnx) < 1.4 * r or (c.mxy - c.mny) < 1.4 * r then
    return nil, "turn it a FULL circle"
  end
  -- Roundness: sums give the mean square radius in closed form, so the
  -- residual costs nothing to check. A fit distorted by moving the device
  -- through nearby metal shows up here with a perfectly normal radius.
  local mean_r2 = (c.ss - 2 * (cx * c.sx + cy * c.sy)) / c.n + cx * cx + cy * cy
  local resid = math.sqrt(math.max(mean_r2 - r * r, 0))
  if resid > 0.18 * r then
    return nil, string.format("too distorted (%.0f%%) - move away from metal", 100 * resid / r)
  end
  return { ox = c.o[1] + cx, oy = c.o[2] + cy, r = r }, r
end

-- ---------------------------------------------------------------------------
-- heading
-- Heading from the horizontal field, in the sensor's own frame.
--
-- The default is MEASURED, not guessed: on the ThinkNode M9, holding the
-- device flat and reading the field at north / east / south / west gives
-- (after subtracting the hard-iron centre)
--     N  x'=-0.055 y'=+0.310      E  x'=+0.268 y'=-0.018
--     S  x'=+0.013 y'=-0.275      W  x'=-0.225 y'=-0.016
-- i.e. +Y points at the device's top edge and +X to its left, so
-- atan2(x, y) reads 0/90/180/270 at N/E/S/W and counts up clockwise. That
-- needs no offset and no press.
--
-- `mirror` covers a sensor mounted the other way up (its Z facing out of the
-- screen instead of into it), which reverses the direction of travel;
-- `align` is the fine offset A sets, and stays 0 on a known board.
local function mag_heading(m)
  local x, y = m.x, m.y
  if cal then x, y = x - cal.ox, y - cal.oy end
  local a = mirror and math.atan(-x, y) or math.atan(x, y)
  return norm360(math.deg(a) + align)
end

local function smooth_heading(h)
  local r = math.rad(h)
  local sx, sy = math.sin(r), math.cos(r)
  if hv_x == 0 and hv_y == 0 then
    hv_x, hv_y = sx, sy
  else
    hv_x = hv_x + (sx - hv_x) * SMOOTH
    hv_y = hv_y + (sy - hv_y) * SMOOTH
  end
  return norm360(math.deg(math.atan(hv_x, hv_y)))
end

local function update_heading(now)
  local m = has_compass and sys.compass() or nil
  if m then
    last_mag_ms = now
    diag_m = m
    mag_sat = m.ovfl == true
    if not mag_sat then
      if calib then calib_add(m) end
      -- horizontal magnitude: that is what the heading is made of, and what
      -- the calibrated radius can be compared against to detect tilt
      local x, y = m.x, m.y
      if cal then x, y = x - cal.ox, y - cal.oy end
      mag_norm = math.sqrt(x * x + y * y)
      mag_z = m.z
      heading, src = smooth_heading(mag_heading(m)), "mag"
    end
    return
  end
  -- No magnetometer (or nothing fresh for a while): GPS course while moving.
  if has_compass and (now - last_mag_ms) < 1500 and heading then return end
  local g = caps.sdk_ext and sys.gps() or nil
  if g and g.course then
    heading, src = smooth_heading(g.course), "gps"
  else
    heading, src = nil, "none"
    hv_x, hv_y = 0, 0
  end
end

-- ---------------------------------------------------------------------------
-- targets: contacts that have shared a position
local function refresh_contacts()
  local list = {}
  for _, c in ipairs(mesh.contacts()) do
    if (c.lat ~= 0 or c.lon ~= 0) and c.name and c.name ~= "" then
      list[#list + 1] = c
    end
  end
  table.sort(list, function(a, b) return a.name < b.name end)
  -- keep the same target selected across a refresh when it is still there
  local cur = targets[target_i]
  targets = list
  if cur then
    target_i = 0
    for i, c in ipairs(list) do
      if c.name == cur.name then target_i = i break end
    end
  elseif target_i > #targets then
    target_i = 0
  end
end

local function cycle_target(step)
  if #targets == 0 then
    target_i = 0
    sys.toast("No contact has shared a position yet", 1500)
    return
  end
  target_i = (target_i + step) % (#targets + 1)
  if target_i < 0 then target_i = target_i + #targets + 1 end
end

-- great-circle distance (m) and initial bearing (deg) from (lat1,lon1) to (lat2,lon2)
local function geo(lat1, lon1, lat2, lon2)
  local p1, p2 = math.rad(lat1), math.rad(lat2)
  local dl = math.rad(lon2 - lon1)
  local a = math.sin((p2 - p1) / 2) ^ 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ^ 2
  local dist = 2 * 6371000 * math.atan(math.sqrt(a), math.sqrt(1 - a))
  local y = math.sin(dl) * math.cos(p2)
  local x = math.cos(p1) * math.sin(p2) - math.sin(p1) * math.cos(p2) * math.cos(dl)
  return dist, norm360(math.deg(math.atan(y, x)))
end

-- range to a target follows the ALTITUDE row's units (one "how far" setting)
local function fmt_dist(m)
  if UNITS.alt then
    local ft = m * 3.28084
    if ft < 1000 then return string.format("%d ft", math.floor(ft + 0.5)) end
    local mi = m / 1609.344
    if mi < 10 then return string.format("%.2f mi", mi) end
    return string.format("%.1f mi", mi)
  end
  if m < 1000 then return string.format("%d m", math.floor(m + 0.5)) end
  if m < 10000 then return string.format("%.2f km", m / 1000) end
  return string.format("%.1f km", m / 1000)
end

local function fmt_alt(m)
  if UNITS.alt then return string.format("%d ft", math.floor(m * 3.28084 + 0.5)) end
  return string.format("%d m", math.floor(m + 0.5))
end

local function fmt_speed(kmh)
  if UNITS.spd then return string.format("%.1f mph", kmh * 0.621371) end
  return string.format("%.1f km/h", kmh)
end

-- ---------------------------------------------------------------------------
-- drawing
local function set_text(name, text, color)
  local l = L[name]
  if not l then return end
  if last_text[name] ~= text then l:set(text); last_text[name] = text end
  if color and last_text[name .. "#"] ~= color then l:color(color); last_text[name .. "#"] = color end
end

local function pt(deg, r)
  local a = math.rad(deg)
  return math.floor(CX + math.sin(a) * r + 0.5), math.floor(CY - math.cos(a) * r + 0.5)
end

-- Canvas text is left-anchored and the app cannot measure glyphs, so widths
-- are estimated at ~0.55 x line height per CHARACTER — counting characters,
-- not bytes: "°" is two bytes in UTF-8 and counting those pushed centred text
-- half a glyph off.
local function text_w(s, lh)
  local n = 0
  for _ in s:gmatch("[%z\1-\127\194-\244]") do n = n + 1 end
  return math.floor(n * lh * 0.55)
end

local function draw_dial(tgt_bearing)
  local h = heading or 0
  local live = heading ~= nil
  -- the page is the firmware's black; the dial sits on its own raised disc so
  -- it reads as an instrument rather than as drawing on the page
  cv:fill(C.bg)
  cv:circle(CX, CY, R, PANEL, true)
  -- rings
  cv:circle(CX, CY, R, live and RING or RING2, false, 2)
  cv:circle(CX, CY, R - 14, RING2, false, 1)

  -- graduations: every 10° a minor tick, every 30° a major one, the four
  -- cardinals as letters. The whole card rotates by -heading so the current
  -- heading sits under the lubber mark.
  for deg = 0, 350, 10 do
    local a = deg - h
    local major = deg % 30 == 0
    local cardinalp = deg % 90 == 0
    local len = cardinalp and 9 or major and 6 or 3
    local x1, y1 = pt(a, R - 3)
    local x2, y2 = pt(a, R - 3 - len)
    local col = (deg == 0) and C.bad or (major and (live and C.text or C.sub) or RING)
    cv:line(x1, y1, x2, y2, col, cardinalp and 2 or 1)
    if cardinalp then
      local lx, ly = pt(a, R - 14 - math.floor(TH12 * 0.6))
      local letter = CARD[math.floor(deg / 90) * 4 + 1]
      cv:text(lx - math.floor(TH12 * 0.3), ly - math.floor(TH12 / 2), letter, col, 12)
    end
  end

  -- lubber mark: a small solid triangle pointing in from the top
  for i = 0, 6 do
    cv:line(CX - 6 + i, i, CX + 6 - i, i, C.accent, 1)
  end
  cv:line(CX, 0, CX, 9, C.accent, 2)

  -- target: dot on the inner ring plus a thin spoke, relative to the card
  if tgt_bearing then
    local mx, my = pt(tgt_bearing - h, R - 14)
    local sx, sy = pt(tgt_bearing - h, R - 26)
    cv:line(CX, CY, sx, sy, RING, 1)
    cv:circle(mx, my, 4, AMBER, true)
  end

  -- centre: heading number with the degree sign hanging off its right edge —
  -- the DIGITS are what should sit centred, so the number does not appear to
  -- shift when the reading crosses 100 or 200
  if live then
    local digits = string.format("%03d", math.floor(h + 0.5) % 360)
    local dw = text_w(digits, TH16)
    local dx = CX - math.floor(dw / 2)
    cv:text(dx, CY - TH16 + 1, digits, C.text, 16)
    cv:text(dx + dw, CY - TH16 + 1, "\194\176", C.sub, 16)
    local cd = cardinal(h)
    cv:text(CX - math.floor(text_w(cd, TH12) / 2), CY + 3, cd, src == "mag" and C.accent or AMBER, 12)
  else
    cv:text(CX - math.floor(text_w("--", TH16) / 2), CY - math.floor(TH16 / 2), "--", C.sub, 16)
  end
end

-- satellite meter: ten cells, filled in the status colour up to the count
local function draw_sats(n, col)
  if not sats_cv then return end
  sats_cv:fill(C.bg)
  local cw = math.floor((SATS_W - 9) / 10)
  for i = 0, 9 do
    local x = i * (cw + 1)
    if i < n then sats_cv:rect(x, 0, cw, SATS_H, col, true)
    else sats_cv:rect(x, 0, cw, SATS_H, PANEL, true) end
  end
end

local function refresh(now)
  update_heading(now)

  -- GPS readout
  local g = caps.sdk_ext and sys.gps() or nil
  local me_lat, me_lon
  local sats_n, sats_col = 0, RING
  if g then
    me_lat, me_lon = g.lat, g.lon
    sats_n = g.sats or 0
    sats_col = sats_n >= 6 and C.good or AMBER
    set_text("fix", string.format("%d sats", sats_n), C.text)
    set_text("lat", string.format("%.5f", g.lat), C.text)
    set_text("lon", string.format("%.5f", g.lon), C.text)
    set_text("alt", fmt_alt(g.alt_m or 0), C.text)
    if g.speed_kmh then
      local s = fmt_speed(g.speed_kmh)
      if g.course then s = s .. string.format("  %03d\194\176", math.floor(g.course + 0.5) % 360) end
      set_text("spd", s, C.text)
    else
      set_text("spd", "--", C.sub)
    end
  else
    local me = mesh.self()
    if me and (me.lat ~= 0 or me.lon ~= 0) then
      me_lat, me_lon = me.lat, me.lon
      set_text("lat", string.format("%.5f", me.lat), C.sub)
      set_text("lon", string.format("%.5f", me.lon), C.sub)
    else
      set_text("lat", "--", C.sub)
      set_text("lon", "--", C.sub)
    end
    set_text("fix", caps.sdk_ext and "no fix" or "no GPS", C.sub)
    set_text("alt", "--", C.sub)
    set_text("spd", "--", C.sub)
  end
  local sk = sats_n .. "|" .. sats_col
  if sk ~= last_sats_key then draw_sats(sats_n, sats_col); last_sats_key = sk end

  -- heading-source line over the dial (<= 18 glyphs: it spans the dial width)
  if calib then
    local left = CAL_SECS - math.floor((now - calib.t0) / 1000)
    -- coverage, not just a countdown: the span of each axis reaches 2r over a
    -- full turn, so this reads ~100% exactly when the sweep is complete
    local spanx, spany = calib.mxx - calib.mnx, calib.mxy - calib.mny
    local cov = math.floor(math.min(spanx, spany) / 0.5 * 100)
    set_text("src", string.format("Turn it round  %ds  %d%%", math.max(left, 0), math.min(cov, 99)), AMBER)
  elseif mag_sat then
    set_text("src", "Field saturated", C.bad)
  elseif src == "mag" then
    -- Tilt is the accuracy ceiling for a 2-axis compass: the field dips ~60
    -- degrees at mid latitudes, so tipping the device swaps vertical field
    -- into the horizontal pair and swings the heading. The app cannot correct
    -- that without the IMU, but it CAN notice: held level the horizontal
    -- magnitude equals the calibrated radius, and tilting shrinks or inflates
    -- it. So say when the reading should not be trusted.
    local level_off = cal and cal.r and mag_norm and math.abs(mag_norm - cal.r) > 0.22 * cal.r
    if not cal then
      set_text("src", "Calibrate: press C", AMBER)
    elseif level_off then
      set_text("src", "Hold it level", AMBER)
    else
      set_text("src", "Magnetometer ok", C.good)
    end
  elseif src == "gps" then
    set_text("src", "GPS course", AMBER)
  elseif has_compass then
    set_text("src", "Sensor: no data", C.bad)
  else
    set_text("src", "GPS when moving", C.sub)
  end

  -- D: what the app is actually computing, so a wrong heading can be diagnosed
  -- from the screen instead of guessed at. Takes over the target rows.
  if diag then
    local m = diag_m
    set_text("tgt", string.format("cal %s", cal and string.format("%.3f %.3f r%.2f", cal.ox, cal.oy, cal.r or 0)
                                                or "NONE - press C"), cal and C.text or C.bad)
    set_text("tgt2", m and string.format("raw %.2f %.2f %.2f", m.x, m.y, m.z) or "raw --", C.text)
    if m and cal then
      set_text("rel", string.format("hor %.3f %.3f |H| %.3f", m.x - cal.ox, m.y - cal.oy, mag_norm or 0), C.text)
    else
      set_text("rel", "hor --", C.sub)
    end
    set_text("seen", string.format("align %d  mirror %d  |B| %.2f", math.floor(align + 0.5),
                                   mirror and 1 or 0, mag_norm or 0), AMBER)
    draw_dial(nil)
    last_dial_key = nil          -- keep the dial live while diagnosing
    return
  end

  -- target: name, range + bearing, which way to turn, when it was last heard
  local tgt_bearing
  local t = targets[target_i]
  if t then
    set_text("tgt", t.name:sub(1, name_max), AMBER)
    if me_lat then
      local dist, brg = geo(me_lat, me_lon, t.lat, t.lon)
      tgt_bearing = brg
      set_text("tgt2", string.format("%s  %03d\194\176 %s", fmt_dist(dist), math.floor(brg + 0.5) % 360,
                                     cardinal(brg)), C.text)
      if heading then
        local rel = norm360(brg - heading)
        local turn = rel <= 180 and rel or 360 - rel
        local s
        if turn <= 6 then s = "ahead"
        elseif turn >= 174 then s = "behind"
        else s = string.format("%d\194\176 %s", math.floor(turn + 0.5), rel <= 180 and "right" or "left") end
        set_text("rel", s, turn <= 6 and C.good or C.text)
      else
        set_text("rel", "no heading", C.sub)
      end
    else
      set_text("tgt2", compact and "no own position" or "own position unknown", C.sub)
      set_text("rel", "", C.sub)
    end
    local ago = t.ago_s or 0
    if ago <= 0 then set_text("seen", "", C.sub)
    elseif ago < 60 then set_text("seen", "heard just now", C.sub)
    elseif ago < 3600 then set_text("seen", string.format("heard %dm ago", math.floor(ago / 60)), C.sub)
    elseif ago < 86400 then set_text("seen", string.format("heard %dh ago", math.floor(ago / 3600)), C.sub)
    else set_text("seen", string.format("heard %dd ago", math.floor(ago / 86400)), C.sub) end
  else
    set_text("tgt", #targets > 0 and string.format("none  (%d)  <>", #targets) or "none", C.sub)
    set_text("tgt2", "", C.sub)
    set_text("rel", "", C.sub)
    set_text("seen", "", C.sub)
  end

  -- dial: redraw only when what it shows changed
  local key = string.format("%d|%s|%s|%s|%s", heading and math.floor(heading + 0.5) or -1, src,
                            tgt_bearing and math.floor(tgt_bearing + 0.5) or "-", tostring(cal ~= nil),
                            tostring(mag_sat))
  if key ~= last_dial_key then
    draw_dial(tgt_bearing)
    last_dial_key = key
  end
end

-- ---------------------------------------------------------------------------
-- actions
local function toggle_cal()
  if not has_compass then sys.toast("No magnetometer on this board", 1500) return end
  if calib then
    local fit, r = calib_solve()
    if fit then
      cal = fit
      save_cal()
      sys.toast(string.format("Calibrated  field %.2f G", r), 2200)
    else
      sys.toast("Not calibrated: " .. tostring(r), 2800)
    end
    calib = nil
    if sys.keep_awake then sys.keep_awake(false) end
  else
    -- Hold the screen and the app's tick for the sweep: the default screen
    -- timeout is the same 20 s as this calibration, and a blanked screen used
    -- to stop the sampling dead half way through and save the partial fit.
    if sys.keep_awake then sys.keep_awake(true) end
    calib = calib_new()
    sys.toast("Lay it FLAT and turn it slowly right around", 2800)
  end
  hv_x, hv_y = 0, 0
end

-- One press does the whole orientation job: hold the device flat with the top
-- edge at north and press A.
--   * handedness — whether the heading runs clockwise or anticlockwise depends
--     on which way the sensor's Z axis faces, and that shows up in the sign of
--     the vertical field: Earth's field dips DOWN north of the magnetic
--     equator and UP south of it. With a position (a fix, or the last one the
--     node knows) the sign of `mag_z` therefore says which way Z points.
--     Near the equator the dip is too shallow to read, so the mirror is left
--     as it was and only the offset is set.
--   * offset — whatever angle the sensor reports while pointing north becomes
--     the zero.
-- Note on what "north" means here: this is a MAGNETIC compass. Magnetic north
-- and true north differ by the local declination — about 13° in California,
-- and over 15° in parts of the US — so a dial that disagrees with a phone (which
-- shows true north) by roughly that much is not broken, it is measuring a
-- different north. Pressing A while pointing at TRUE north folds the local
-- declination into `align` and makes the two agree.
local function align_north()
  if not has_compass then sys.toast("No magnetometer on this board", 1500) return end
  if not cal then sys.toast("Calibrate first: press C", 2000) return end
  local m = sys.compass()
  if not m or m.ovfl then sys.toast("No usable reading", 1500) return end

  local lat
  local g = caps.sdk_ext and sys.gps() or nil
  if g then lat = g.lat else
    local me = mesh.self()
    if me and (me.lat ~= 0 or me.lon ~= 0) then lat = me.lat end
  end
  local dip_ok = math.abs(mag_z) > 0.05 * (mag_norm or 1)
  if lat and dip_ok then
    -- Held flat, Earth's field points DOWN north of the magnetic equator and
    -- UP south of it. The base formula above is the one for a sensor whose Z
    -- faces INTO the screen, and such a sensor reads that downward field as a
    -- POSITIVE z in the north. (This test was inverted at first, which is
    -- what made a correctly-defaulted M9 turn the wrong way.)
    local z_into_screen = (lat >= 0) == (mag_z > 0)
    mirror = not z_into_screen
  end

  align = 0
  align = -mag_heading(m)          -- whatever it reads now becomes 000
  save_align()
  hv_x, hv_y = 0, 0
  if lat and dip_ok then
    sys.toast("North set", 1200)
  elseif not lat then
    sys.toast("North set (no position: turn right, then A again if it counts down)", 3000)
  else
    sys.toast("North set (field too flat here to check direction)", 2500)
  end
end

-- Fallback when align_north cannot read the dip (near the magnetic equator, or
-- no position at all): flip the direction of travel by hand.
-- the selected row's key is drawn in the accent colour so it is obvious which
-- one OK will switch
local function paint_selection()
  for _, n in ipairs({ "alt", "spd" }) do
    local kl = L["k_" .. n]
    if kl then kl:color(n == sel and C.accent or C.sub) end
  end
end

local function move_sel(dir)
  sel = (sel == "alt") and "spd" or "alt"
  paint_selection()
end

local function toggle_units(which)
  which = which or sel
  UNITS[which] = not UNITS[which]
  store.set(which == "alt" and "u_alt" or "u_spd", UNITS[which] and 1 or 0)
  sel = which
  paint_selection()
  last_text[which] = nil                      -- force the row to re-render now
  if which == "alt" then
    sys.toast(UNITS.alt and "Altitude and range: feet / miles" or "Altitude and range: metres / km", 1400)
  else
    sys.toast(UNITS.spd and "Speed: mph" or "Speed: km/h", 1200)
  end
end

local function flip_frame()
  mirror = not mirror
  align = 0
  save_align()
  hv_x, hv_y = 0, 0
  sys.toast(mirror and "Direction reversed - press A again facing north"
                    or "Direction normal - press A again facing north", 2500)
end

-- ---------------------------------------------------------------------------
-- layout
local function label(name, x, y, size, color, width)
  local l = ui.label("", x, y, size or 12, color or C.text)
  if width then l:width(width) end
  L[name] = l
end

function app.on_open(w, h)
  W, H = w, h
  caps = sys.caps()
  has_compass = caps.compass == true
  load_prefs()
  landscape = w >= h * 1.3
  TH12, TH14, TH16 = ui.text_h(12), ui.text_h(14), ui.text_h(16)

  -- panel rows: { name, key } — a key/value pair per row, keys in the muted
  -- colour at x0, values in the value column. Key-less rows after TGT are the
  -- target's detail lines and sit in the value column. The magnetometer
  -- status line is not a row: it is centred over the dial. The key hint is
  -- not a row either: it sits on the bottom edge of the view.
  local rows = {
    { "fix", "FIX" }, { "lat", "LAT" }, { "lon", "LON" }, { "alt", "ALT" }, { "spd", "SPD" },
    { "tgt", "TGT" }, { "tgt2", false }, { "rel", false }, { "seen", false },
  }
  local gap_before = { tgt = 4 }   -- group spacing
  local x0, y0, colw, line
  local hint_y

  if landscape then
    -- stats column on the left, dial on the right under its status line, the
    -- hint centred along the bottom edge
    hint_y = h - TH12 - 3
    dial_y = TH12 + 4
    D = math.min(hint_y - 4 - dial_y, w - 176)
    cv = ui.canvas(D, D)
    dial_x = w - D - 2
    x0, y0 = 4, 4
    colw = dial_x - x0 - 8
    line = TH12 + 2
    local function total()
      local t = 0
      for _, r in ipairs(rows) do t = t + line + (gap_before[r[1]] or 0) end
      return t
    end
    local room = hint_y - 4 - y0                 -- rows must clear the hint line
    while #rows > 6 and total() > room do table.remove(rows) end
    if total() > room then line = math.floor(room / #rows) end
  else
    D = math.min(w - 8, 200)
    cv = ui.canvas(D, D)
    dial_x, dial_y = math.floor((w - D) / 2), TH12 + 4
    x0, y0 = 6, dial_y + D + 6
    colw = w - 12
    line = TH12 + 2
    if caps.touch then ui.scroll(true) end
  end
  cv:pos(dial_x, dial_y)
  R = math.floor(D / 2) - 2
  CX, CY = math.floor(D / 2), math.floor(D / 2)
  -- magnetometer / heading-source status, centred over the dial
  L.src = ui.label("", dial_x, 2, 12, C.sub)
  L.src:width(D, "center")

  local keyw = math.floor(TH12 * 2.1)           -- "LON" at 12 px is ~24 px; leave a gap
  local valx = x0 + keyw + 6
  local valw = colw - keyw - 6
  col_x0, col_x1 = x0, x0 + colw
  name_max = math.max(8, math.min(18, math.floor((valw - 4) / (TH12 * 0.62))))
  -- Montserrat runs ~0.48 x line height per glyph: the status line over the
  -- dial is the longest (22 glyphs, ~10.6 x); go compact when the dial is
  -- narrower than that
  compact = D < TH12 * 10.7

  local y = y0
  for _, r in ipairs(rows) do
    local name, key = r[1], r[2]
    y = y + (gap_before[name] or 0)
    if key then
      local kl = ui.label(key, x0, y, 12, C.sub)
      if name == "alt" or name == "spd" then L["k_" .. name] = kl end
    end
    label(name, valx, y, 12, C.text, valw)   -- key-less rows line up with the values
    if name == "alt" or name == "spd" then row_hit[name] = { y = y, h = line } end
    if name == "fix" then
      -- The meter sits right after the count. The reserve is sized for the
      -- widest text ("99 sats") so the bars hold still as the number changes,
      -- but measured properly: text_w's 0.55-per-character estimate is for
      -- mixed text, while digits and spaces in Montserrat run nearer 0.39 of
      -- the line height, which left an obvious gap.
      local sats_x = valx + math.floor(TH12 * 2.8) + 4
      SATS_W = math.max(20, math.min(52, valx + valw - sats_x - 2))
      sats_cv = ui.canvas(SATS_W, SATS_H)
      sats_cv:pos(sats_x, y + math.floor((TH12 - SATS_H) / 2))
      sats_cv:fill(C.bg)
    end
    y = y + line
  end

  -- touch boards in portrait: the same actions as buttons (keys may not exist)
  if caps.touch and not landscape then
    local bw, bh = math.floor((w - 12 - 9) / 4), 30
    local by = y + 2
    local bx = 6
    if has_compass then
      ui.button("Cal", bx, by, bw, bh, toggle_cal);   bx = bx + bw + 3
      ui.button("North", bx, by, bw, bh, align_north); bx = bx + bw + 3
      ui.button("Flip", bx, by, bw, bh, flip_frame);   bx = bx + bw + 3
    end
    ui.button("Target", bx, by, bw, bh, function() cycle_target(1) end)
    y = by + bh + 4
  end

  -- key hint: centred across the whole view on the bottom edge in landscape,
  -- below everything else in portrait (where the body scrolls)
  L.hint = ui.label("", 0, hint_y or (y + 2), 12, C.sub)
  L.hint:width(w, "center")
  if caps.keyboard then
    -- "A set north" is deliberately not advertised: the axis mapping is
    -- measured, so a calibrated device points north on its own. A (and F)
    -- still work for an unknown board or a stubborn environment.
    set_text("hint", has_compass and "C calibrate   up/down + OK units   <> target"
                                  or "up/down + OK units   <> target", C.sub)
  elseif caps.touch then
    set_text("hint", "Tap a row for units, the dial for the next target", C.sub)
  else
    set_text("hint", "up/down + OK units   <> target", C.sub)
  end

  paint_selection()
  refresh_contacts()
  next_contacts_ms = sys.millis() + CONTACTS_EVERY
  refresh(sys.millis())
  timer.every(TICK_MS)
end

function app.on_tick(dt)
  local now = sys.millis()
  if (now - next_contacts_ms) >= 0 then     -- wrap-safe: millis is a 32-bit integer here
    refresh_contacts()
    next_contacts_ms = now + CONTACTS_EVERY
  end
  if calib and (now - calib.t0) >= CAL_SECS * 1000 then toggle_cal() end
  refresh(now)
end

local function in_dial(x, y)
  return x >= dial_x and x < dial_x + D and y >= dial_y and y < dial_y + D
end

-- which stats row a press landed on, or nil
local function row_at(x, y)
  if x < col_x0 or x > col_x1 then return nil end
  for name, r in pairs(row_hit) do
    if y >= r.y - 2 and y < r.y + r.h + 2 then return name end
  end
  return nil
end

function app.on_input(ev)
  if ev.type == "swipe" then
    press = nil
    -- one finger swipe can arrive twice on touch boards (LVGL's gesture and
    -- the firmware's own swipe detector both report it): take the first only
    local now = sys.millis()
    if (now - last_swipe_ms) < 300 then return end
    last_swipe_ms = now
    -- the M9's d-pad arrives here, not as key events
    if ev.dir == "left" then cycle_target(-1)
    elseif ev.dir == "right" then cycle_target(1)
    elseif ev.dir == "up" or ev.dir == "down" then move_sel(ev.dir)
    end
  elseif ev.type == "down" then
    -- "down" fires at the start of every touch, swipe or scroll drag, so a tap
    -- is only recognised on the matching "up" that landed within 12 px. The
    -- M9's OK key synthesises down+up at the body centre, which passes too.
    press = { x = ev.x or 0, y = ev.y or 0 }
  elseif ev.type == "up" then
    if press then
      local dx, dy = (ev.x or 0) - press.x, (ev.y or 0) - press.y
      if dx * dx + dy * dy <= 144 then
        if caps.touch then
          -- a tap on the ALT or SPD row switches that row's units; on the dial
          -- it steps the target
          local r = row_at(press.x, press.y)
          if r then toggle_units(r)
          elseif in_dial(press.x, press.y) then cycle_target(1) end
        else
          -- no touchscreen: this is the OK key's synthetic press, wherever the
          -- host put it — it switches the selected row's units
          toggle_units()
        end
      end
      press = nil
    end
  elseif ev.type == "key" then
    local k = ev.key
    -- deliberately no "enter" here: the host answers OK with a synthetic
    -- down/up pair AND an enter key event, so acting on both would toggle
    -- twice and look like nothing happened
    if k == "up" or k == "down" then move_sel(k)
    elseif k == "d" or k == "D" then
      diag = not diag
      last_dial_key = nil
      sys.toast(diag and "Diagnostics on" or "Diagnostics off", 900)
    elseif k == "c" or k == "C" then toggle_cal()
    elseif k == "a" or k == "A" then align_north()
    elseif k == "f" or k == "F" then flip_frame()      -- fallback, see align_north
    elseif k == "x" or k == "X" then
      if cal then
        cal = nil; save_cal()
        align = 0; mirror = false; save_align()
        sys.toast("Calibration cleared", 1000)
      end
    end
  end
end

function app.on_close()
  calib = nil
end

return app
