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
--     the M9, so O (Rot) steps the frame by 90° and F (Flip) mirrors it.
--     Point the top of the device at a known north and adjust until the dial
--     reads 000 and the number grows as you turn clockwise. Persisted too.
-- Magnetic declination is not applied: this is a magnetic compass.
local ui, sys, mesh, store, timer = wada.ui, wada.sys, wada.mesh, wada.store, wada.timer
local C = ui.colors
local AMBER  = 0xE8A33D
local RING   = 0x3A424A        -- dial ring + minor ticks
local RING2  = 0x1C2228        -- inner ring
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
local orient, flip = 0, false     -- quarter turns + mirror applied to the sensor frame
local mag_norm = nil              -- |B| after offsets, Gauss (sanity check for the user)
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
    hv_x = hv_x + (sx - hv_x) * SMOOTH
    hv_y = hv_y + (sy - hv_y) * SMOOTH
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
  cv:fill(C.bg)
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
    else sats_cv:rect(x, 0, cw, SATS_H, RING, true) end
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
    set_text("alt", string.format("%d m", g.alt or 0), C.text)
    if g.speed_kmh then
      local s = string.format("%.1f km/h", g.speed_kmh)
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
    set_text("src", string.format("Calibrating  %ds", math.max(left, 0)), AMBER)
  elseif mag_sat then
    set_text("src", "Field saturated", C.bad)
  elseif src == "mag" then
    set_text("src", cal and "Magnetometer ok" or "Calibrate: press C", cal and C.good or AMBER)
  elseif src == "gps" then
    set_text("src", "GPS course", AMBER)
  elseif has_compass then
    set_text("src", "Sensor: no data", C.bad)
  else
    set_text("src", "GPS when moving", C.sub)
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
    local mn, mx = calib.mn, calib.mx
    local span = math.min(mx[1] - mn[1], mx[2] - mn[2])
    if span < 0.2 then
      sys.toast("Calibration cancelled: turn the device more", 2000)
    else
      cal = { ox = (mn[1] + mx[1]) / 2, oy = (mn[2] + mx[2]) / 2, oz = (mn[3] + mx[3]) / 2 }
      save_cal()
      -- field strength: a fully rotated axis spans +/-|B|, so the widest half-
      -- span is the field. Earth is 0.25..0.65 G; far off means a magnet
      -- nearby or a poor calibration
      local hb = math.max((mx[1] - mn[1]) / 2, (mx[2] - mn[2]) / 2, (mx[3] - mn[3]) / 2)
      sys.toast(string.format("Calibrated  field %.2f G", hb), 2000)
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
    -- status line over the dial, the dial below it taking what is left after
    -- a panel wide enough for the values, the hint along the bottom edge
    hint_y = h - TH12 - 3
    dial_y = TH12 + 4
    D = math.min(hint_y - 4 - dial_y, w - 176)
    cv = ui.canvas(D, D)
    dial_x = 2
    x0, y0 = D + 10, 4
    colw = w - x0 - 4
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
  name_max = math.max(8, math.min(18, math.floor((valw - 4) / (TH12 * 0.62))))
  -- Montserrat runs ~0.48 x line height per glyph: the status line over the
  -- dial is the longest (22 glyphs, ~10.6 x); go compact when the dial is
  -- narrower than that
  compact = D < TH12 * 10.7

  local y = y0
  for _, r in ipairs(rows) do
    local name, key = r[1], r[2]
    y = y + (gap_before[name] or 0)
    if key then ui.label(key, x0, y, 12, C.sub) end
    label(name, valx, y, 12, C.text, valw)   -- key-less rows line up with the values
    if name == "fix" then
      SATS_W = math.min(52, math.max(30, valw - math.floor(TH12 * 0.55 * 8)))
      sats_cv = ui.canvas(SATS_W, SATS_H)
      sats_cv:pos(valx + valw - SATS_W, y + math.floor((TH12 - SATS_H) / 2))
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
      ui.button("Cal", bx, by, bw, bh, toggle_cal);  bx = bx + bw + 3
      ui.button("Rot", bx, by, bw, bh, rotate_frame); bx = bx + bw + 3
      ui.button("Flip", bx, by, bw, bh, flip_frame);  bx = bx + bw + 3
    end
    ui.button("Target", bx, by, bw, bh, function() cycle_target(1) end)
    y = by + bh + 4
  end

  -- key hint: centred across the whole view on the bottom edge in landscape,
  -- below everything else in portrait (where the body scrolls)
  L.hint = ui.label("", 0, hint_y or (y + 2), 12, C.sub)
  L.hint:width(w, "center")
  if caps.keyboard then
    local wide = w >= TH12 * 20                 -- room for the target hint too
    set_text("hint", has_compass and (wide and "C calibrate   O rotate   F flip   <> target"
                                              or "C calibrate   O rotate   F flip")
                                  or "<> target", C.sub)
  elseif caps.touch then
    set_text("hint", "Tap dial: next target", C.sub)
  else
    set_text("hint", "<> target", C.sub)
  end

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
      -- one, so it counts wherever the host placed it
      if dx * dx + dy * dy <= 144 and (not caps.touch or in_dial(press.x, press.y)) then cycle_target(1) end
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
