-- GPS Compass — a compass rose plus the live GPS fix.
--
-- Heading comes from the magnetometer where the board has one (ThinkNode M9:
-- QMC6309, via wada.sys.compass()) and falls back to GPS course-over-ground
-- while moving on every other board. The rose turns so the heading sits
-- under the fixed index at the top; a contact with a known position can be
-- picked as a target and is drawn on the rose with its bearing and distance.
--
-- The magnetometer is raw: the firmware hands out x/y/z in Gauss in the
-- sensor's own frame, uncalibrated. This app does the rest —
--   * hard-iron calibration: press C (or Cal), turn the device through every
--     orientation for ~20 s, press C again. Offsets persist in wada.store.
--   * orientation: the sensor's axes vs. the screen are not documented for
--     the M9, so O (Rot) steps the frame by 90° and F (Flip) mirrors it.
--     Point the top of the device at a known north and adjust until the rose
--     reads 0° and the number grows as you turn clockwise. Persisted too.
-- Magnetic declination is not applied: this is a magnetic compass.
local ui, sys, mesh, store, timer = wada.ui, wada.sys, wada.mesh, wada.store, wada.timer
local C = ui.colors
local AMBER = 0xE8A33D
local app = {}

local caps, W, H
local landscape
local cv, D, R, CX, CY            -- rose canvas, diameter, radius, centre
local rose_x, rose_y = 0, 0       -- canvas position in the body (tap hit-test)
local TH12, TH14, TH16 = 15, 17, 19   -- font line heights, replaced from ui.text_h
local L = {}                      -- readout labels by name

local has_compass = false
local cal = nil                   -- { ox, oy, oz } hard-iron offsets
local calib = nil                 -- in-progress: { mn = {x,y,z}, mx = {x,y,z}, t0 }
local orient, flip = 0, false     -- quarter turns + mirror applied to the sensor frame
local mag_norm = nil              -- |B| after offsets, Gauss (sanity check for the user)
local mag_sat = false             -- last magnetometer sample was flagged as saturated

local hv_x, hv_y = 0, 0           -- smoothed heading unit vector
local heading = nil               -- degrees 0..360, or nil
local src = "none"                -- "mag" | "gps" | "none"
local last_mag_ms = 0

local targets, target_i = {}, 0   -- contacts with a position; 0 = none
local next_contacts_ms = 0
local CONTACTS_EVERY = 20000      -- positions only change on adverts

local press = nil                 -- pending touch: { x, y } from the last "down"
local last_swipe_ms = -100000     -- debounce: LVGL's gesture and the hardware swipe
                                  -- detector can both report one finger swipe
local name_max = 18               -- target-name characters that fit the column
local compact = false             -- narrow column at a big font: drop the cardinals
local last_text = {}              -- label text cache: LVGL relayout only on change
local last_rose_key = nil
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
  local ox, oy, oz = store.get("cal_ox"), store.get("cal_oy"), store.get("cal_oz")
  if type(ox) == "number" and type(oy) == "number" and type(oz) == "number" then
    cal = { ox = ox, oy = oy, oz = oz }
  end
  orient = math.floor(tonumber(store.get("orient", 0)) or 0) % 4
  flip = (store.get("flip", 0) == 1)
end

local function save_cal()
  if cal then
    store.set("cal_ox", cal.ox); store.set("cal_oy", cal.oy); store.set("cal_oz", cal.oz)
  else
    store.set("cal_ox", nil); store.set("cal_oy", nil); store.set("cal_oz", nil)
  end
end

-- ---------------------------------------------------------------------------
-- heading
-- Sensor frame -> screen frame -> heading. After the offsets, `fx` is the
-- field component along the screen's top edge direction and `fy` along its
-- right edge. Device top pointing north: field along +fx -> 0°. Pointing
-- east: north is to the device's left -> fy negative -> 90°.
local function mag_heading(m)
  local x, y = m.x, m.y
  if cal then x, y = x - cal.ox, y - cal.oy end
  if flip then y = -y end
  for _ = 1, orient do x, y = -y, x end
  return norm360(math.deg(math.atan(-y, x)))
end

local function smooth_heading(h)
  local r = math.rad(h)
  local sx, sy = math.sin(r), math.cos(r)
  if hv_x == 0 and hv_y == 0 then
    hv_x, hv_y = sx, sy
  else
    hv_x = hv_x + (sx - hv_x) * 0.35
    hv_y = hv_y + (sy - hv_y) * 0.35
  end
  return norm360(math.deg(math.atan(hv_x, hv_y)))
end

local function update_heading(now)
  local m = has_compass and sys.compass() or nil
  if m then
    last_mag_ms = now
    mag_sat = m.ovfl == true
    if not mag_sat then
      if calib then
        local mn, mx = calib.mn, calib.mx
        if m.x < mn[1] then mn[1] = m.x end; if m.x > mx[1] then mx[1] = m.x end
        if m.y < mn[2] then mn[2] = m.y end; if m.y > mx[2] then mx[2] = m.y end
        if m.z < mn[3] then mn[3] = m.z end; if m.z > mx[3] then mx[3] = m.z end
      end
      local x, y, z = m.x, m.y, m.z
      if cal then x, y, z = x - cal.ox, y - cal.oy, z - cal.oz end
      mag_norm = math.sqrt(x * x + y * y + z * z)
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

local function fmt_dist(m)
  if m < 1000 then return string.format("%d m", math.floor(m + 0.5)) end
  if m < 10000 then return string.format("%.2f km", m / 1000) end
  return string.format("%.1f km", m / 1000)
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

-- canvas text is left-anchored; approximate glyph width as 0.55 x line height
local function text_w(s, lh) return math.floor(#s * lh * 0.55) end

local function draw_rose(tgt_bearing)
  local h = heading or 0
  local live = heading ~= nil
  local ring = live and C.text or C.sub
  cv:fill(C.bg)
  cv:circle(CX, CY, R, ring, false, 1)

  -- ticks every 10°, longer every 30°, cardinal letters every 90°; the rose
  -- rotates by -heading so the current heading is under the top index
  for deg = 0, 350, 10 do
    local a = deg - h
    local len = (deg % 90 == 0) and 10 or (deg % 30 == 0) and 7 or 3
    local x1, y1 = pt(a, R - 1)
    local x2, y2 = pt(a, R - 1 - len)
    local col = (deg == 0) and C.bad or ((deg % 90 == 0) and C.text or C.sub)
    cv:line(x1, y1, x2, y2, col, (deg % 90 == 0) and 2 or 1)
    if deg % 90 == 0 then
      local lx, ly = pt(a, R - 12 - math.floor(TH12 / 2))
      local letter = CARD[math.floor(deg / 90) * 4 + 1]
      cv:text(lx - math.floor(TH12 * 0.3), ly - math.floor(TH12 / 2), letter, col, 12)
    end
  end

  -- fixed index at the top
  cv:line(CX, 1, CX - 6, 12, C.accent, 2)
  cv:line(CX, 1, CX + 6, 12, C.accent, 2)
  cv:line(CX - 6, 12, CX + 6, 12, C.accent, 2)

  -- target marker, relative to the rose
  if tgt_bearing then
    local mx, my = pt(tgt_bearing - h, R - 22 - TH12)
    cv:circle(mx, my, 4, AMBER, true)
    cv:line(CX, CY, mx, my, AMBER, 1)
  end

  -- centre: heading number + cardinal
  if live then
    local txt = string.format("%03d\194\176", math.floor(h + 0.5) % 360)
    cv:text(CX - math.floor(text_w(txt, TH16) / 2) + 2, CY - TH16, txt, C.text, 16)
    local cd = cardinal(h)
    cv:text(CX - math.floor(text_w(cd, TH14) / 2), CY + 2, cd, src == "mag" and C.accent or AMBER, 14)
  else
    cv:text(CX - math.floor(text_w("--", TH16) / 2), CY - math.floor(TH16 / 2), "--", C.sub, 16)
  end
end

local function refresh(now)
  update_heading(now)

  -- GPS readout
  local g = caps.sdk_ext and sys.gps() or nil
  local me_lat, me_lon
  if g then
    me_lat, me_lon = g.lat, g.lon
    set_text("lat", string.format("Lat %.5f", g.lat), C.text)
    set_text("lon", string.format("Lon %.5f", g.lon), C.text)
    set_text("alt", string.format("Alt %d m  Sats %d", g.alt or 0, g.sats or 0), C.text)
    local mot
    if g.speed_kmh then
      mot = string.format("%.1f km/h", g.speed_kmh)
      if g.course then
        mot = mot .. string.format(" Crs %03d\194\176", math.floor(g.course + 0.5) % 360)
        if not compact then mot = mot .. " " .. cardinal(g.course) end
      else
        mot = mot .. " (still)"
      end
    else
      mot = "Speed --"
    end
    set_text("mot", mot, C.text)
  else
    local me = mesh.self()
    if me and (me.lat ~= 0 or me.lon ~= 0) then
      me_lat, me_lon = me.lat, me.lon
      set_text("lat", string.format("Lat %.5f", me.lat), C.sub)
      set_text("lon", string.format("Lon %.5f (last)", me.lon), C.sub)
    else
      set_text("lat", "Lat --", C.sub)
      set_text("lon", "Lon --", C.sub)
    end
    set_text("alt", caps.sdk_ext and "No GPS fix" or "No GPS on this board", C.sub)
    set_text("mot", "Speed --", C.sub)
  end

  -- heading source line (fits a 160 px column at the 12 px font)
  if calib then
    local left = CAL_SECS - math.floor((now - calib.t0) / 1000)
    set_text("src", string.format("CAL: turn device %ds", math.max(left, 0)), AMBER)
  elseif mag_sat then
    set_text("src", "Compass SATURATED", C.bad)
  elseif src == "mag" then
    set_text("src", string.format("%s %s %.2f G", compact and "Mag" or "Compass", cal and "ok" or "UNCAL",
                                  mag_norm or 0), cal and C.good or AMBER)
  elseif src == "gps" then
    set_text("src", "Heading: GPS course", AMBER)
  elseif has_compass then
    set_text("src", "Compass: no data", C.bad)
  else
    set_text("src", "Heading: GPS (move)", C.sub)
  end

  -- target
  local tgt_bearing
  local t = targets[target_i]
  if t then
    set_text("tgt", "> " .. t.name:sub(1, name_max), AMBER)
    if me_lat then
      local dist, brg = geo(me_lat, me_lon, t.lat, t.lon)
      tgt_bearing = brg
      local s = string.format("%s brg %03d\194\176", fmt_dist(dist), math.floor(brg + 0.5) % 360)
      if not compact then s = s .. " " .. cardinal(brg) end
      set_text("tgt2", s, C.text)
    else
      set_text("tgt2", "need own position", C.sub)
    end
  else
    set_text("tgt", #targets > 0 and string.format("No target (%d avail)", #targets) or "No target", C.sub)
    set_text("tgt2", "", C.sub)
  end

  -- rose: redraw only when what it shows changed
  local key = string.format("%d|%s|%s|%s|%s", heading and math.floor(heading + 0.5) or -1, src,
                            tgt_bearing and math.floor(tgt_bearing + 0.5) or "-", tostring(cal ~= nil),
                            tostring(mag_sat))
  if key ~= last_rose_key then
    draw_rose(tgt_bearing)
    last_rose_key = key
  end
end

-- ---------------------------------------------------------------------------
-- actions
local function toggle_cal()
  if not has_compass then sys.toast("No magnetometer on this board", 1500) return end
  if calib then
    local mn, mx = calib.mn, calib.mx
    local span = math.min(mx[1] - mn[1], mx[2] - mn[2])
    if span < 0.2 then
      sys.toast("Calibration cancelled: turn the device more", 2000)
    else
      cal = { ox = (mn[1] + mx[1]) / 2, oy = (mn[2] + mx[2]) / 2, oz = (mn[3] + mx[3]) / 2 }
      save_cal()
      sys.toast("Compass calibrated", 1200)
    end
    calib = nil
  else
    calib = { mn = { 1e9, 1e9, 1e9 }, mx = { -1e9, -1e9, -1e9 }, t0 = sys.millis() }
    sys.toast("Turn the device through every orientation", 2000)
  end
  hv_x, hv_y = 0, 0
end

local function rotate_frame()
  orient = (orient + 1) % 4
  store.set("orient", orient)
  hv_x, hv_y = 0, 0
  sys.toast(string.format("Frame rotated: %d\194\176", orient * 90), 900)
end

local function flip_frame()
  flip = not flip
  store.set("flip", flip and 1 or 0)
  hv_x, hv_y = 0, 0
  sys.toast(flip and "Frame mirrored" or "Frame normal", 900)
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

  -- readout rows, most expendable last: the hint rows go first when the body
  -- is too short for all of them at the user's font size
  local rows = { "src", "lat", "lon", "alt", "mot", "tgt", "tgt2", "hint", "hint2" }
  local x0, y0, colw, line

  if landscape then
    -- the rose takes what is left after a column wide enough for ~22 glyphs
    D = math.min(h - 4, w - 176)
    cv = ui.canvas(D, D)
    rose_x, rose_y = 2, math.floor((h - D) / 2)
    x0, y0 = D + 8, 4
    colw = w - x0 - 4
    line = TH12 + 2
    while #rows > 7 and #rows * line > h - 8 do table.remove(rows) end
    if #rows * line > h - 8 then line = math.floor((h - 8) / #rows) end
  else
    D = math.min(w - 8, 240)
    cv = ui.canvas(D, D)
    rose_x, rose_y = math.floor((w - D) / 2), 2
    x0, y0 = 6, D + 8
    colw = w - 12
    line = TH12 + 2
    if caps.touch then ui.scroll(true) end
  end
  cv:pos(rose_x, rose_y)
  R = math.floor(D / 2) - 2
  CX, CY = math.floor(D / 2), math.floor(D / 2)
  -- what fits one row: names by a pixel budget (capitals run ~0.62 x line
  -- height), and the widest fixed strings (~10.5 x line height) decide
  -- whether the cardinal suffixes have to go
  name_max = math.max(8, math.min(18, math.floor((colw - 14) / (TH12 * 0.62))))
  compact = colw < TH12 * 10.5

  local y = y0
  for _, name in ipairs(rows) do
    label(name, x0, y, 12, C.text, colw)
    y = y + line
  end

  -- control hints (the keyboard shortcuts only exist where there is a keyboard)
  if caps.keyboard then
    set_text("hint", has_compass and "C cal  O rot  F flip" or "", C.sub)
    set_text("hint2", "<> target", C.sub)
  elseif caps.touch then
    set_text("hint", "Tap rose: next target", C.sub)
  else
    set_text("hint", "<> target", C.sub)
  end
  -- touch boards in portrait: the same actions as buttons (keys may not exist)
  if caps.touch and not landscape then
    local bw, bh = math.floor((w - 12 - 9) / 4), 30
    local by = y + 2
    local bx = 6
    if has_compass then
      ui.button("Cal", bx, by, bw, bh, toggle_cal);  bx = bx + bw + 3
      ui.button("Rot", bx, by, bw, bh, rotate_frame); bx = bx + bw + 3
      ui.button("Flip", bx, by, bw, bh, flip_frame);  bx = bx + bw + 3
    end
    ui.button("Target", bx, by, bw, bh, function() cycle_target(1) end)
  end

  refresh_contacts()
  next_contacts_ms = sys.millis() + CONTACTS_EVERY
  refresh(sys.millis())
  timer.every(150)
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

local function in_rose(x, y)
  return x >= rose_x and x < rose_x + D and y >= rose_y and y < rose_y + D
end

function app.on_input(ev)
  if ev.type == "swipe" then
    press = nil
    -- one finger swipe can arrive twice on touch boards (LVGL's gesture and
    -- the firmware's own swipe detector both report it): take the first only
    local now = sys.millis()
    if (now - last_swipe_ms) < 300 then return end
    last_swipe_ms = now
    if ev.dir == "left" then cycle_target(-1)
    elseif ev.dir == "right" then cycle_target(1)
    end
  elseif ev.type == "down" then
    -- "down" fires at the start of every touch, swipe or scroll drag, so a tap
    -- is only recognised on the matching "up" that landed within 12 px. The
    -- M9's OK key synthesises down+up at the body centre, which passes too.
    press = { x = ev.x or 0, y = ev.y or 0 }
  elseif ev.type == "up" then
    if press then
      local dx, dy = (ev.x or 0) - press.x, (ev.y or 0) - press.y
      -- without a touchscreen the only down/up pair is the OK key's synthetic
      -- one, so it counts wherever the host placed it (the body centre is not
      -- inside the rose on every layout)
      if dx * dx + dy * dy <= 144 and (not caps.touch or in_rose(press.x, press.y)) then cycle_target(1) end
      press = nil
    end
  elseif ev.type == "key" then
    local k = ev.key
    if k == "c" or k == "C" then toggle_cal()
    elseif k == "o" or k == "O" then rotate_frame()
    elseif k == "f" or k == "F" then flip_frame()
    elseif k == "x" or k == "X" then
      if cal then cal = nil; save_cal(); sys.toast("Calibration cleared", 1000) end
    end
  end
end

function app.on_close()
  calib = nil
end

return app
