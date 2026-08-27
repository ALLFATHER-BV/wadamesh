-- Mock of the wada.* host (mirrors LuaAppHost.cpp argument checks) + scenarios.
local debug, io, loadfile, print, error, assert, pcall, type, tostring, string, math, table, ipairs, pairs =
      debug, io, loadfile, print, error, assert, pcall, type, tostring, string, math, table, ipairs, pairs
local APP_PATH, SCENARIO = APP_PATH, SCENARIO

local BUDGET, INIT_BUDGET = 100000, 500000
local clock_ms = 1000
local toasts, fails, maxinstr = {}, 0, 0
local function checkint(v, what) assert(math.tointeger(v) ~= nil, what .. ": not an integer: " .. tostring(v)) end
local function checkcol(v, what) if v ~= nil then checkint(v, what .. " color") end end
local function checkstr(v, what) assert(type(v) == "string" or type(v) == "number", what .. ": not a string") end

local function valid_utf8(text)
  local i, n = 1, #text
  while i <= n do
    local first = text:byte(i)
    local width = first <= 0x7F and 1
      or (first >= 0xC2 and first <= 0xDF and 2)
      or (first >= 0xE0 and first <= 0xEF and 3)
      or (first >= 0xF0 and first <= 0xF4 and 4) or 0
    if width == 0 or i + width - 1 > n then return false end
    for j = 2, width do
      local byte = text:byte(i + j - 1)
      if byte < 0x80 or byte > 0xBF then return false end
    end
    local second = width > 1 and text:byte(i + 1) or 0
    if (first == 0xE0 and second < 0xA0) or (first == 0xED and second > 0x9F) or
       (first == 0xF0 and second < 0x90) or (first == 0xF4 and second > 0x8F) then return false end
    i = i + width
  end
  return true
end

-- instruction budget like guardedCall(): error() out of the hook, pcall catches
local function guarded(budget, fn, ...)
  local count = 0
  debug.sethook(function() count = count + budget; error("instruction budget exceeded (app tick too long)", 0) end, "", budget)
  local t0 = os and os.clock() or 0
  local ok, err = pcall(fn, ...)
  debug.sethook()
  if not ok then fails = fails + 1; print("  APP ERROR: " .. tostring(err)) end
  return ok
end

-- ---- mock wada ----------------------------------------------------------
local cfg = {}           -- per-scenario device config
local widgets = { canvases = 0, labels = 0, buttons = 0, scroll = false, timer_ms = nil }
local drawlog = { text = {}, circles = {}, ops = 0 }
local storekv = {}
local audio_state = { state = "stopped", path = "", source = "", format = "", error = nil }
local audio_log = {}

local function audio_host_close()
  audio_state.state = "stopped"
  audio_log[#audio_log + 1] = "release"
end

local function mkcanvas(w, h)
  checkint(w, "canvas w"); checkint(h, "canvas h")
  assert(w > 0 and w <= 480 and h > 0 and h <= 480, "canvas size 1..480")
  widgets.canvases = widgets.canvases + 1
  local c = { w = w, h = h }
  function c:fill(col) checkcol(col, "fill"); drawlog.ops = drawlog.ops + 1 end
  function c:rect(x, y, rw, rh, col, filled, radius)
    checkint(x, "rect x"); checkint(y, "rect y"); checkint(rw, "rect w"); checkint(rh, "rect h"); checkcol(col, "rect")
    drawlog.ops = drawlog.ops + 1 end
  function c:line(x1, y1, x2, y2, col, width)
    checkint(x1, "line x1"); checkint(y1, "line y1"); checkint(x2, "line x2"); checkint(y2, "line y2"); checkcol(col, "line")
    if width ~= nil then checkint(width, "line width") end
    drawlog.ops = drawlog.ops + 1 end
  function c:circle(x, y, r, col, filled, sw)
    checkint(x, "circle x"); checkint(y, "circle y"); checkint(r, "circle r"); checkcol(col, "circle")
    if sw ~= nil then checkint(sw, "circle stroke") end
    drawlog.circles[#drawlog.circles + 1] = { x = x, y = y, r = r, col = col, filled = filled }
    drawlog.ops = drawlog.ops + 1 end
  function c:text(x, y, s, col, size)
    checkint(x, "text x"); checkint(y, "text y"); checkstr(s, "text"); checkcol(col, "text")
    if size ~= nil then checkint(size, "text size") end
    drawlog.text[#drawlog.text + 1] = tostring(s); drawlog.ops = drawlog.ops + 1 end
  function c:pos(x, y) checkint(x, "canvas pos x"); checkint(y, "canvas pos y"); c.x, c.y = x, y end
  return c
end

local labels = {}
local function mklabel(text, x, y, size, col)
  checkstr(text, "label text"); if x then checkint(x, "label x") end; if y then checkint(y, "label y") end
  if size then checkint(size, "label size") end; checkcol(col, "label")
  widgets.labels = widgets.labels + 1
  local l = { text = tostring(text), x = x, y = y }
  function l:set(s) checkstr(s, "label:set"); l.text = tostring(s) end
  function l:pos(px, py) checkint(px, "label:pos"); checkint(py, "label:pos") end
  function l:color(c) checkcol(c, "label:color") end
  function l:width(w) checkint(w, "label:width") end
  labels[#labels + 1] = l
  return l
end

local buttons = {}
local function mkbutton(text, x, y, w, h, fn)
  checkstr(text, "button text"); checkint(x, "button x"); checkint(y, "button y")
  if w then checkint(w, "button w") end; if h then checkint(h, "button h") end
  widgets.buttons = widgets.buttons + 1
  local b = mklabel(text, x, y, 14, nil); b.fn = fn; buttons[#buttons + 1] = b
  return b
end

local function build_wada()
  local wada = { ui = {}, sys = {}, mesh = {}, store = {}, timer = {}, net = {}, fs = {} }
  wada.ui.colors = { accent = 0x15B6A6, text = 0xE6E9ED, sub = 0x7A7F87, bg = 0x000000, panel = 0x15181B, bad = 0xD7574E, good = 0x53C06B }
  wada.ui.canvas = mkcanvas
  wada.ui.label = mklabel
  wada.ui.button = mkbutton
  wada.ui.scroll = function(on) widgets.scroll = (on == nil) or on end
  wada.ui.text_h = function(sz) return (sz or 12) + 4 end
  -- upstream's exact measurement: ~0.48 of the line height per character
  wada.ui.text_w = function(str, sz)
    local n = 0
    for _ in str:gmatch('[%z\1-\127\194-\244]') do n = n + 1 end
    return math.floor(n * ((sz or 12) + 4) * 0.48)
  end
  wada.ui.chart = function() error("chart not mocked") end

  wada.sys.millis = function() return clock_ms end
  wada.sys.keep_awake = function(on) cfg.awake = (on == nil) or on end
  wada.sys.toast = function(msg, ms) checkstr(msg, "toast"); toasts[#toasts + 1] = tostring(msg) end
  wada.sys.board = function() return { w = cfg.w, h = cfg.h, touch = cfg.caps.touch } end
  wada.sys.random = function(lo, hi) lo = lo or 0; hi = hi or 0; if hi <= lo then return 12345 end return lo end
  wada.sys.epoch = function() return nil end
  wada.sys.datetime = function() return nil end
  wada.sys.beep = function() return false end
  wada.sys.caps = function() return { sdk_ext = cfg.caps.sdk_ext, keyboard = cfg.caps.keyboard,
                                       touch = cfg.caps.touch, sd = true, compass = cfg.caps.compass,
                                       accel = cfg.caps.accel, discover = cfg.caps.discover,
                                       sd_list = cfg.caps.sd_list or false,
                                       audio = cfg.caps.audio or false,
                                       audio_wav = cfg.caps.audio or false,
                                       audio_mp3 = cfg.caps.audio or false,
                                       audio_sd = cfg.caps.audio_sd or false } end
  if cfg.caps.sdk_ext then
    wada.sys.battery = function() return { mv = 3900, pct = 70, charging = false } end
    wada.sys.gps = function() return cfg.gps and cfg.gps() or nil end
  end
  if cfg.caps.compass then
    wada.sys.compass = function() return cfg.compass and cfg.compass() or nil end
  end
  if cfg.caps.accel then
    wada.sys.accel = function() return cfg.accel and cfg.accel() or { x = 0, y = 0, z = 1 } end
  end

  wada.mesh.contacts = function() return cfg.contacts or {} end
  wada.mesh.self = function() return { name = "me", lat = cfg.self_lat or 0, lon = cfg.self_lon or 0 } end
  wada.mesh.stats = function() return {} end
  wada.mesh.rx_log = function() return {} end
  wada.mesh.discover = function() return "probe-1" end
  wada.mesh.discovered = function() return cfg.discovered or {} end
  wada.mesh.discover_clear = function() cfg.discovered = {} end

  wada.fs.append = function(name, data) checkstr(name, "fs.append name"); checkstr(data, "fs.append data"); return true end
  wada.fs.remove = function(name) checkstr(name, "fs.remove name"); return true end

  wada.store.get = function(k, d) assert(type(k) == "string"); local v = storekv[k]; if v == nil then return d end return v end
  wada.store.set = function(k, v) assert(type(k) == "string", "store key must be a string")
    assert(v == nil or type(v) == "string" or type(v) == "number", "store values must be strings or numbers")
    storekv[k] = v end

  wada.timer.every = function(ms) checkint(ms, "timer.every"); if ms < 33 then ms = 33 end; widgets.timer_ms = ms end
  wada.timer.stop = function() widgets.timer_ms = nil end

  if cfg.caps.audio then
    wada.audio = {}
    wada.audio.play = function(path)
      assert(type(path) == "string", "audio.play path must be a string")
      local source = "app"
      if path:sub(1, 3) == "sd:" then
        if not cfg.caps.audio_sd then return nil, "no sd" end
        local card_path = path:sub(4)
        if card_path:sub(1, 1) ~= "/" or card_path:find("//", 1, true) or
           card_path:find("/../", 1, true) or card_path:sub(-3) == "/.." or
           card_path:sub(-2) == "/." or card_path:sub(-1) == "/" then
          return nil, "bad path"
        end
        source = "sd"
      elseif #path == 0 or #path > 32 or path:sub(1, 1) == "." or
             not path:match("^[A-Za-z0-9._-]+$") then
        return nil, "bad path"
      end
      local format = path:lower():sub(-4)
      if format ~= ".wav" and format ~= ".mp3" then return nil, "unsupported format" end
      audio_state = { state = "playing", path = path, source = source,
              format = format:sub(2), error = nil }
      audio_log[#audio_log + 1] = "play:" .. path
      return true
    end
    wada.audio.pause = function()
      if audio_state.state ~= "playing" then return false end
      audio_state.state = "paused"; audio_log[#audio_log + 1] = "pause"; return true
    end
    wada.audio.resume = function()
      if audio_state.state ~= "paused" then return false end
      audio_state.state = "playing"; audio_log[#audio_log + 1] = "resume"; return true
    end
    wada.audio.stop = function()
      if audio_state.state ~= "playing" and audio_state.state ~= "paused" then return false end
      audio_state.state = "stopped"; audio_log[#audio_log + 1] = "stop"; return true
    end
    wada.audio.status = function()
      local out = {}
      for key, value in pairs(audio_state) do out[key] = value end
      return out
    end
  end
  if cfg.caps.sd_list then
    wada.sd = { list = function(path)
      assert(path == "/", "mock SD only exposes the root")
      return {}
    end }
  end
  return wada
end

-- ---- scenario driver -------------------------------------------------------------
local function load_app()
  local chunk, err = loadfile(APP_PATH)
  assert(chunk, err)
  local app
  assert(guarded(INIT_BUDGET, function() app = chunk() end), "chunk failed")
  assert(type(app) == "table", "app did not return a table")
  return app
end

local function send(app, ev) if app.on_input then guarded(BUDGET, app.on_input, ev) end end
local function tick(app, n, dt)
  for _ = 1, (n or 1) do clock_ms = clock_ms + (dt or 150); if app.on_tick then guarded(BUDGET, app.on_tick, dt or 150) end end
end
local function key(app, k, code) send(app, { type = "key", key = k, code = code or (k and k:byte()) or 0 }) end
local function swipe(app, dir) send(app, { type = "swipe", dir = dir, x = 0, y = 0 }) end
local function tap(app, x, y) send(app, { type = "down", x = x, y = y }); send(app, { type = "up", x = x, y = y }) end
local function drag(app, x, y, x2, y2, dir) send(app, { type = "down", x = x, y = y }); send(app, { type = "swipe", dir = dir, x = 0, y = 0 }); send(app, { type = "up", x = x2, y = y2 }) end
local function enter(app, w, h) tap(app, w // 2, h // 2); key(app, "enter", 13) end

local function label_dump()
  local out = {}
  for _, l in ipairs(labels) do if l.text ~= "" then out[#out + 1] = l.text end end
  return table.concat(out, " | ")
end

local function reset_world()
  widgets = { canvases = 0, labels = 0, buttons = 0, scroll = false, timer_ms = nil }
  labels, buttons, toasts, drawlog = {}, {}, {}, { text = {}, circles = {}, ops = 0 }
  clock_ms = 1000
  audio_state = { state = "stopped", path = "", source = "", format = "", error = nil }
  audio_log = {}
end

-- simulated magnetometer: Earth field 0.45 G at true heading `deg` (device frame ==
-- sensor frame, orient 0), plus a hard-iron bias; tilt ignored.
local BIAS = { x = 1.5, y = -0.8, z = 0.3 }
-- the real M9's accelerometer reads ~0.08 g high on Y lying flat: about 5
-- degrees of tilt that is not there
local ABIAS = { x = -0.02, y = 0.08, z = -0.01 }
-- Physical simulation of the M9 in a known attitude, in the MEASURED frames:
--   body/accel: +X top edge (forward), +Y right edge, +Z into the screen (down)
--   magnetometer: +Y forward, +X left, +Z down  ->  raw = (-by, bx, bz) + bias
-- Field: San Francisco-like, 0.24 G horizontal north, 0.42 G down (dip ~60).
-- The accelerometer reads specific force, so the skyward axis reads +1 and the
-- level device reads z = -1, exactly as the real one was measured to.
local BH, BV = 0.24, 0.42
local function attitude(hdg, roll, pitch)
  local h, r, p = math.rad(hdg), math.rad(roll or 0), math.rad(pitch or 0)
  -- earth (north, east, down) -> body, ZYX yaw-pitch-roll
  local function e2b(n, e, d)
    local x1 =  n * math.cos(h) + e * math.sin(h)      -- yaw
    local y1 = -n * math.sin(h) + e * math.cos(h)
    local z1 =  d
    local x2 =  x1 * math.cos(p) - z1 * math.sin(p)    -- pitch
    local y2 =  y1
    local z2 =  x1 * math.sin(p) + z1 * math.cos(p)
    local x3 =  x2                                      -- roll
    local y3 =  y2 * math.cos(r) + z2 * math.sin(r)
    local z3 = -y2 * math.sin(r) + z2 * math.cos(r)
    return x3, y3, z3
  end
  local bx, by, bz = e2b(BH, 0, BV)         -- magnetic field in body axes
  local gx, gy, gz = e2b(0, 0, 1)           -- "down" in body axes
  return { x = -by + BIAS.x, y = bx + BIAS.y, z = bz + BIAS.z },   -- magnetometer raw
         { x = -gx + ABIAS.x, y = -gy + ABIAS.y, z = -gz + ABIAS.z }  -- accelerometer raw
end

local contacts_fixture = {
  { name = "Repeater A", type = 2, ago_s = 10, lat = 37.80000, lon = -122.40000 },
  { name = "Bob", type = 1, ago_s = 300, lat = 37.70000, lon = -122.50000 },
  { name = "NoPos", type = 1, ago_s = 5, lat = 0, lon = 0 },
  { name = "Carol", type = 1, ago_s = 60, lat = 38.00000, lon = -122.00000 },
}

local scenarios = {}

scenarios.wardrive_utf8 = function()
  cfg = {
    w = 320, h = 196,
    caps = { sdk_ext = true, keyboard = true, touch = false, compass = false, accel = false, discover = true },
    discovered = {
      { pubkey = "01020304", name = "Ouderkerk☀️", type = 2,
        rssi = -72, snr = 7.25, their_snr = 6.5, hops = 0 }
    }
  }
  cfg.gps = function()
    return { lat = 52.295, lon = 4.907, lat_e6 = 52295000, lon_e6 = 4907000,
             sats = 9, alt_m = 3, time = 1787620000 }
  end
  storekv = {}
  wada = build_wada()
  local app = load_app()
  assert(guarded(BUDGET, app.on_open, cfg.w, cfg.h))
  assert(buttons[1] and buttons[1].fn, "Wardrive Start button missing")
  assert(guarded(BUDGET, buttons[1].fn))
  tick(app, 1, 100)
  tick(app, 1, 4000)
  local found = false
  for _, label in ipairs(labels) do
    assert(valid_utf8(label.text), "Wardrive rendered invalid UTF-8")
    if label.text:find("Ouderkerk☀", 1, true) then found = true end
  end
  assert(found, "emoji-bearing repeater name was not rendered")
  if app.on_close then guarded(BUDGET, app.on_close) end
end

-- The declination model, as it actually ships. Rather than testing a copy in
-- out/wmm, this pulls the do-block straight out of the app file that gets
-- sideloaded, so an inlining mistake fails here instead of on the device.
-- Reference values are NOAA's own calculator for WMM2025 at epoch 2026.6.
scenarios.declination = function()
  local src = assert(io.open(APP_PATH)):read("a")
  local a = src:find("-- WMM-GEN BEGIN", 1, true)
  assert(a, "the declination module is missing from the app")
  local e = src:find("end -- WMM-GEN END", a, true)
  assert(e, "the generated block is not closed by its END marker")
  local chunk = src:sub(a, e + #"end -- WMM-GEN END") .. "\nreturn declination"
  local declination = assert(load(chunk, "decl"))()

  local REF = {
    {  37.75, -122.45,   12.8470, 22928.0, "harness fixture SF" },
    {  43.66,  -70.26,  -14.4653, 20159.9, "Portland ME" },
    { -33.87,  151.21,   12.8236, 24625.1, "Sydney" },
    {  64.13,  -21.90,  -11.0771, 13242.4, "Reykjavik" },
    { -70.00,  100.00, -103.9366, 12624.7, "Antarctic (steep)" },
    {  82.00, -100.00,  -34.1519,  1905.5, "82N: weak-field zone" },
  }
  local worst = 0
  for _, r in ipairs(REF) do
    local lat, lon, want_d, want_h, name = r[1], r[2], r[3], r[4], r[5]
    local got_d, got_h = declination(lat, lon, 2026.6)
    local err = math.abs(((got_d - want_d + 540) % 360) - 180)
    if err > worst then worst = err end
    print(string.format("  %-22s %9.4f vs %9.4f  (%.4f)  H=%.0f nT", name, got_d, want_d, err, got_h))
    assert(err < 0.02, "declination wrong at " .. name)
    assert(math.abs(got_h - want_h) < 30, "horizontal field wrong at " .. name)
  end
  print(string.format("  worst error vs NOAA: %.4f deg", worst))
  -- the secular variation has to be live, or this is a frozen 2025 snapshot
  local d25 = declination(43.66, -70.26, 2025.0)
  local d30 = declination(43.66, -70.26, 2030.0)
  assert(math.abs(d30 - d25) > 0.2,
         string.format("secular variation is not applied (%.3f -> %.3f)", d25, d30))
  print(string.format("  Portland ME drifts %+.2f deg across the model's 5 years", d30 - d25))
end

-- Pressing A means "this direction is TRUE north". With no fix the app has no
-- declination to work with, so the offset it stores quietly contains one; if
-- the model then added its own the heading would be wrong by twice the
-- declination -- the very error that sent every marker 22 deg west, but on the
-- dial instead. The offset has to give the declination back when the fix lands.
scenarios.align_nofix = function()
  local FIX_DECL = 12.847
  -- no self position either: on a real node the stored one is usually enough to
  -- get a declination before GPS locks, so being truly blind takes both gone
  cfg = { w = 320, h = 196, caps = { sdk_ext = true, keyboard = true, touch = false, compass = true, accel = true },
          contacts = contacts_fixture }
  local true_heading, true_roll, true_pitch = 0, 0, 0
  cfg.compass = function() return (attitude(true_heading, true_roll, true_pitch)) end
  cfg.accel = function() local _, a = attitude(true_heading, true_roll, true_pitch); return a end
  local have_fix = false
  cfg.gps = function()
    if not have_fix then return nil end
    return { lat = 37.75, lon = -122.45, sats = 9, alt_m = 42 }
  end
  storekv = {}
  wada = build_wada()
  local app = load_app()
  assert(guarded(BUDGET, app.on_open, cfg.w, cfg.h))

  key(app, "c")                  -- calibrate with no fix at all
  for i = 1, 140 do
    true_heading = (i * 360 / 23) % 360
    true_roll    = (i * 360 / 11) % 360
    true_pitch   = ((i * 360 / 17) % 360) - 180
    tick(app, 1, 150)
  end
  true_roll, true_pitch = 0, 0
  assert(storekv.cal_ox, "calibration must still work with no GPS")

  local function shown()
    tick(app, 15)
    return tonumber(drawlog.text[#drawlog.text - 3]:match("^(%d+)$"))
  end

  -- the user believes they are facing true north and says so
  true_heading = 30            -- i.e. magnetic 30; with no model the app cannot know
  tick(app, 15)                -- settle: A reads the live tilt, not the sweep's last
  key(app, "a"); tick(app, 4)
  assert(storekv.align_pd, "an offset set with no declination must be remembered as such")
  assert(toasts[#toasts]:find("magnetic"), "the toast must admit it: " .. tostring(toasts[#toasts]))
  assert(math.abs(((shown() - 0 + 540) % 360) - 180) <= 2, "must read 000 where north was set")
  assert(drawlog.text[#drawlog.text - 1] == "M", "with no model the dial is magnetic")

  -- the fix arrives: the model now knows the declination the offset swallowed
  have_fix = true
  tick(app, 20)
  assert(not storekv.align_pd, "the pending flag must clear once the model has a value")
  assert(math.abs(((shown() - 0 + 540) % 360) - 180) <= 2,
         string.format("the fix must not move a direction the user already fixed (%d)", shown()))
  assert(drawlog.text[#drawlog.text - 1] == "T",
         "the dial must repaint as TRUE the moment the model has a declination")
  print(string.format("  align %d, decl %+.1f: %d before the fix, %d after",
                      storekv.align, FIX_DECL, 0, shown()))

  -- and a quarter turn still counts a quarter turn
  true_heading = 30 + 90
  assert(math.abs(((shown() - 90 + 540) % 360) - 180) <= 3, "a right turn must count UP")
  app.on_close()
end

-- Is a contact MIRRORED rather than merely rotated? A flip and a rotation look
-- identical at one heading, so the existing marker test cannot tell them apart:
-- it checks the dot against the app's OWN bearing, which stays self-consistent
-- even if that bearing has east and west swapped. This checks the bearing
-- against an absolute compass direction instead -- contacts placed due north,
-- east, south, west and north-east of the fixture -- and then checks the dot
-- against it. A longitude sign error puts east at 270; a lat/lon transpose
-- puts north-east at 51.7 instead of 38.3.
scenarios.bearings_absolute = function()
  local ME_LAT, ME_LON = 37.75, -122.45
  local POINTS = {
    { name = "N",  lat = 37.85, lon = -122.45, brg =   0.000 },
    { name = "E",  lat = 37.75, lon = -122.30, brg =  89.954 },
    { name = "S",  lat = 37.65, lon = -122.45, brg = 180.000 },
    { name = "W",  lat = 37.75, lon = -122.60, brg = 270.046 },
    { name = "NE", lat = 37.85, lon = -122.35, brg =  38.284 },
  }
  local marks = {}
  for i, pt in ipairs(POINTS) do
    marks[i] = { name = pt.name, type = 1, ago_s = 10, lat = pt.lat, lon = pt.lon }
  end
  cfg = { w = 320, h = 196, caps = { sdk_ext = true, keyboard = true, touch = false, compass = true, accel = true },
          contacts = marks, self_lat = ME_LAT, self_lon = ME_LON }
  local true_heading, true_roll, true_pitch = 0, 0, 0
  cfg.compass = function() return (attitude(true_heading, true_roll, true_pitch)) end
  cfg.accel = function() local _, a = attitude(true_heading, true_roll, true_pitch); return a end
  cfg.gps = function() return { lat = ME_LAT, lon = ME_LON, sats = 9, alt_m = 42 } end
  storekv = {}
  wada = build_wada()
  local app = load_app()
  assert(guarded(BUDGET, app.on_open, cfg.w, cfg.h))

  key(app, "c")                          -- calibrate so the dial is live
  for i = 1, 140 do
    true_heading = (i * 360 / 23) % 360
    true_roll    = (i * 360 / 11) % 360
    true_pitch   = ((i * 360 / 17) % 360) - 180
    tick(app, 1, 150)
  end
  true_roll, true_pitch = 0, 0
  true_heading = 0
  tick(app, 20)

  local D2 = 144 / 2
  local by_name = {}
  for _, pt in ipairs(POINTS) do by_name[pt.name] = pt end
  local seen = 0
  -- the app picks its own target order (nearest first), so go by the name it
  -- reports rather than assuming the contact list order
  for _ = 1, #POINTS do
    swipe(app, "right"); tick(app, 6)
    local dump = label_dump()
    local shown_name = dump:match("TGT | ([%w]+) |") or dump:match("TGT | ([%w]+)")
    local pt = by_name[shown_name]
    assert(pt, "unexpected target selected: " .. tostring(shown_name))
    by_name[shown_name] = nil; seen = seen + 1
    local brg = tonumber(dump:match("mi%s+(%d%d%d)") or dump:match("km%s+(%d%d%d)")
                         or dump:match("m%s+(%d%d%d)"))
    assert(brg, "could not read a bearing for " .. pt.name .. ": " .. dump:sub(1, 130))
    local err = math.abs(((brg - pt.brg + 540) % 360) - 180)
    -- the drawn dot, as a screen angle clockwise from up
    local dot
    for _, c in ipairs(drawlog.circles) do if c.col == 0xE8A33D and c.filled then dot = c end end
    assert(dot, "no marker drawn for " .. pt.name)
    local ang = math.deg(math.atan(dot.x - D2, -(dot.y - D2))) % 360
    local hdg = tonumber(drawlog.text[#drawlog.text - 3]:match("^(%d+)$"))
    local want_ang = (pt.brg - hdg) % 360
    local aerr = math.abs(((ang - want_ang + 540) % 360) - 180)
    print(string.format("  %-2s (%s): bearing %3d want %6.2f err %4.1f | dot %6.1f want %6.2f err %4.1f",
                        pt.name, tostring(shown_name), brg, pt.brg, err, ang, want_ang, aerr))
    assert(err <= 1.5, string.format("%s reads %d, should be %.1f -- bearings are wrong, not just offset",
                                     pt.name, brg, pt.brg))
    assert(aerr <= 3, string.format("%s is drawn %.1f deg from where its own bearing puts it", pt.name, aerr))
  end
  assert(seen == #POINTS, "every contact must be reachable, saw " .. seen)
  app.on_close()
end

scenarios.m9 = function()
  local FIX_DECL = 12.847   -- WMM2025 at the fixture position, epoch 2026.6
  cfg = { w = 320, h = 196, caps = { sdk_ext = true, keyboard = true, touch = false, compass = true, accel = true },
          contacts = contacts_fixture, self_lat = 37.75, self_lon = -122.45 }
  local true_heading, true_roll, true_pitch = 0, 0, 0
  cfg.compass = function() return (attitude(true_heading, true_roll, true_pitch)) end
  cfg.accel = function() local _, a = attitude(true_heading, true_roll, true_pitch); return a end
  local moving = false
  cfg.gps = function()
    local g = { lat = 37.75000, lon = -122.45000, sats = 9, alt_m = 42 }
    if moving then g.speed_kmh = 23.4; g.course = 215.0 end
    return g
  end
  storekv = {}
  wada = build_wada()
  local app = load_app()
  assert(guarded(BUDGET, app.on_open, cfg.w, cfg.h))
  print("  widgets:", "canvases=" .. widgets.canvases, "labels=" .. widgets.labels, "buttons=" .. widgets.buttons,
        "timer=" .. tostring(widgets.timer_ms))
  assert(widgets.timer_ms and widgets.timer_ms >= 33)
  tick(app, 10)
  print("  uncal:", label_dump())
  -- calibration: press C, rotate through 360 over 20 s, auto-finish
  key(app, "c")
  assert(cfg.awake == true, "calibration must hold the screen awake")
  for i = 1, 140 do
    true_heading = (i * 360 / 23) % 360    -- rotated every way, in one place
    true_roll    = (i * 360 / 11) % 360
    true_pitch   = ((i * 360 / 17) % 360) - 180
    tick(app, 1, 150)
  end
  true_roll, true_pitch = 0, 0
  assert(cfg.awake == false, "the hold must be released when calibration ends")
  assert(storekv.cal_ox, "calibration did not persist; last toast: " .. tostring(toasts[#toasts]))
  print(string.format("  cal offsets: %.3f %.3f r=%.3f (bias %.3f %.3f)", storekv.cal_ox, storekv.cal_oy, storekv.cal_r, BIAS.x, BIAS.y))
  assert(math.abs(storekv.cal_ox - BIAS.x) < 0.02 and math.abs(storekv.cal_oy - BIAS.y) < 0.02
         and math.abs(storekv.cal_oz - BIAS.z) < 0.02, "hard-iron offsets wrong")
  assert(storekv.cal_r and math.abs(storekv.cal_r - 0.484) < 0.02, "fitted field radius wrong: " .. tostring(storekv.cal_r))
  -- Heading accuracy after calibration. The simulated field points at MAGNETIC
  -- north, so a correct app shows magnetic + declination: at the fixture's
  -- position (37.75, -122.45) WMM2025 gives +12.85 deg east for 2026.6, which
  -- is the clock the app falls back to because this harness returns no
  -- datetime. Asserting the offset is present is the regression test for the
  -- fault Chris saw on hardware -- every contact sitting ~22 deg west.
  for _, th in ipairs({ 0, 45, 90, 180, 270, 359 }) do
    true_heading = th; tick(app, 15)
    -- drawn order at the centre: digits, degree sign, T/M reference, cardinal
    local shown = drawlog.text[#drawlog.text - 3]
    local num = tonumber(shown:match("^(%d+)$"))
    assert(num, "expected bare digits at the dial centre, got: " .. tostring(shown))
    assert(drawlog.text[#drawlog.text - 2] == "\194\176", "degree sign must follow the digits")
    assert(drawlog.text[#drawlog.text - 1] == "T",
           "with a fix the heading must be marked true, got: " .. tostring(drawlog.text[#drawlog.text - 1]))
    local want = (th + FIX_DECL) % 360
    local err = math.abs(((num - want + 540) % 360) - 180)
    print(string.format("  mag %3d -> true %6.1f  shown %s (err %.0f)", th, want, shown, err))
    assert(err <= 2, "heading error too large at " .. th)
  end
  -- targets: d-pad right/left arrive as swipes; enter = tap at body centre
  swipe(app, "right"); tick(app, 2); print("  target1:", label_dump())
  swipe(app, "right"); swipe(app, "right"); swipe(app, "right"); tick(app, 2); print("  wrap:", label_dump())
  swipe(app, "left"); tick(app, 2)
  -- OK (synthetic down/up + an enter key event) switches the SELECTED row's
  -- units exactly once; up/down moves the selection between ALT and SPD
  assert(storekv.u_alt == nil and label_dump():find("ft"), "altitude must default to imperial")
  enter(app, cfg.w, cfg.h); tick(app, 2)
  assert(storekv.u_alt == 0 and label_dump():find("42 m"), "OK must switch altitude to metric exactly once: " .. label_dump())
  enter(app, cfg.w, cfg.h); tick(app, 2)
  assert(storekv.u_alt == 1 and label_dump():find("ft"), "OK must switch it back")
  swipe(app, "down"); tick(app, 1); clock_ms = clock_ms + 400
  enter(app, cfg.w, cfg.h); tick(app, 2)
  assert(storekv.u_spd == 0 and storekv.u_alt == 1, "down then OK must switch SPEED, not altitude")
  swipe(app, "up"); tick(app, 1); clock_ms = clock_ms + 400
  enter(app, cfg.w, cfg.h); tick(app, 2)
  assert(storekv.u_alt == 0, "up then OK must switch altitude again")
  enter(app, cfg.w, cfg.h); tick(app, 2)          -- back to imperial for the rest
  swipe(app, "right"); tick(app, 2); print("  target:", label_dump())
  -- saturation flag: heading must hold, src line must say so
  local keep = cfg.compass
  cfg.compass = function() local m = attitude(true_heading, 0, 0); m.ovfl = true; return m end
  tick(app, 3); print("  saturated:", label_dump()); assert(label_dump():find("Field saturated"))
  cfg.compass = keep; tick(app, 3); assert(not label_dump():find("Field saturated"))
  -- every readout string must fit the 320-wide column (~23 glyphs at 12 px)
  for _, l in ipairs(labels) do if l.x ~= 0 then assert(#l.text <= 23, "too wide for the M9 column: " .. l.text) end end
  -- orientation / flip / clear / unknown keys
  -- A PARTIAL turn must be REFUSED: an arc leaves the centre almost anywhere,
  -- which is exactly how a silently-accepted bad fit broke north on hardware.
  do
    local saved = { storekv.cal_ox, storekv.cal_oy }
    key(app, "c")
    for i = 1, 80 do true_heading = (i * 60 / 80) % 360; tick(app, 1, 150) end   -- flat, 60 deg only
    key(app, "c"); tick(app, 2)
    assert(toasts[#toasts]:find("Not calibrated"), "a partial turn must be refused, got: " .. toasts[#toasts])
    assert(storekv.cal_ox == saved[1] and storekv.cal_oy == saved[2],
           "a refused calibration must not overwrite the stored one")
  end
  -- the measured default must be right out of the box: no A press, no offset
  do
    local function shown0() return tonumber(drawlog.text[#drawlog.text - 3]:match("^(%d+)$")) end
    for _, th in ipairs({ 0, 90, 180, 270 }) do
      true_heading = th; tick(app, 15)
      local err = math.abs(((shown0() - (th + FIX_DECL) + 540) % 360) - 180)
      assert(err <= 2, string.format("default mapping wrong at %d: showed %d", th, shown0()))
    end
    assert(storekv.align == nil or storekv.align == 0, "no offset should be needed by default")
    assert(storekv.mirror == nil or storekv.mirror == 0, "no mirror should be needed by default")
  end
  -- the accelerometer's own zero-g offset must come out of the same sweep,
  -- or the tilt correction is applied against a gravity vector that is 5
  -- degrees wrong
  assert(storekv.acc_by and math.abs(storekv.acc_by - ABIAS.y) < 0.02,
         "accel bias not recovered: " .. tostring(storekv.acc_by))
  do
    true_heading, true_roll, true_pitch = 0, 0, 0
    tick(app, 10)
    local t = label_dump():match("north%s+tilt%s+(%d+)")   -- "MAG north  tilt 0°"
    assert(t and tonumber(t) <= 2, "flat device should read ~0 tilt, got: " .. tostring(t))
  end
  -- TILT COMPENSATION, the whole point of the IMU: heading must hold while the
  -- device is tipped. Without it the error is ~1.5 deg per deg of tilt here.
  do
    local function shown() return tonumber(drawlog.text[#drawlog.text - 3]:match("^(%d+)$")) end
    for _, hdg in ipairs({ 0, 90, 200 }) do
      true_heading, true_roll, true_pitch = hdg, 0, 0
      tick(app, 15)
      local level = shown()
      for _, tilt in ipairs({ { 20, 0 }, { 0, 20 }, { -15, 15 } }) do
        true_roll, true_pitch = tilt[1], tilt[2]
        tick(app, 15)
        local err = math.abs(((shown() - level + 540) % 360) - 180)
        assert(err <= 3, string.format("tilt %d/%d at heading %d moved the reading %d deg (%d -> %d)",
                                       tilt[1], tilt[2], hdg, err, level, shown()))
      end
      true_roll, true_pitch = 0, 0
    end
    print("  tilt: heading held within 3 deg through 20 deg of roll and pitch")
  end
  -- WAYPOINT PLACEMENT: with a known heading and a known target bearing, the
  -- amber dot must sit at (bearing - heading) clockwise from the top of the
  -- dial. AMBER is 0xE8A33D; the dial centre is (D/2, D/2).
  do
    true_heading, true_roll, true_pitch = 0, 0, 0
    swipe(app, "right"); tick(app, 12)          -- pick the first target
    print("  rows: " .. label_dump())
    -- the target row reads like "7.09 km  brg 038° NE": take THAT number, not
    -- the first degree sign on the page (the course readout also has one)
    local brg = tonumber((label_dump():match("mi%s+(%d%d%d)")) or (label_dump():match("km%s+(%d%d%d)")))
    assert(brg, "could not read the target bearing from the panel")
    for _, hdg in ipairs({ 0, 90, 210 }) do
      true_heading = hdg; tick(app, 20)
      local dot
      for _, c in ipairs(drawlog.circles) do if c.col == 0xE8A33D and c.filled then dot = c end end
      assert(dot, "no waypoint marker drawn")
      -- screen angle of the dot, clockwise from up, around the dial centre
      local D2 = 144 / 2                        -- landscape D on the M9 body
      local ang = math.deg(math.atan(dot.x - D2, -(dot.y - D2))) % 360
      local want = (brg - hdg) % 360
      local err = math.abs(((ang - want + 540) % 360) - 180)
      print(string.format("  waypoint: heading %3d  bearing %3d  -> want %3d, drawn %6.1f, err %.1f",
                          hdg, brg, want, ang, err))
    end
  end
  -- DIAGNOSTICS must fit: at 12 px the panel is ~22 characters wide in normal
  -- layout and ~26 with the key column reclaimed, and a longer line wraps onto
  -- the row beneath it.
  do
    key(app, "d"); tick(app, 4)
    for _, l in ipairs(labels) do
      if l.text ~= "" and l.x ~= 0 then
        assert(#l.text <= 26, "diagnostic row too wide (" .. #l.text .. "): " .. l.text)
      end
    end
    print("  diag rows: " .. label_dump())
    key(app, "d"); tick(app, 2)
  end
  -- align north: with the device pointing at an arbitrary true heading, one
  -- press must make THAT direction read 000 and keep the dial turning the
  -- right way (a later true heading must read back as itself minus the offset)
  true_heading = 137; tick(app, 8)
  key(app, "a"); tick(app, 8)
  assert(storekv.align ~= nil, "align must persist")
  assert(storekv.mirror == nil, "mirror is gone: the frames are measured, not hand-flipped")
  assert(toasts[#toasts] == "North set here", "A must set the offset: " .. tostring(toasts[#toasts]))
  local function shown_deg()
    return tonumber(drawlog.text[#drawlog.text - 3]:match("^(%d+)$"))
  end
  assert(math.abs(((shown_deg() - 0 + 540) % 360) - 180) <= 2, "the aligned direction must read 000")
  true_heading = 137 + 90; tick(app, 15)
  assert(math.abs(((shown_deg() - 90 + 540) % 360) - 180) <= 3, "a right turn must count UP")
  -- F and O must do NOTHING now: both frames are measured, so a key that
  -- hand-flips handedness can only break a correct compass
  local before_f = { storekv.align, storekv.mirror, storekv.orient, storekv.flip }
  key(app, "f"); key(app, "F"); key(app, "o"); key(app, "O"); tick(app, 5)
  assert(storekv.align == before_f[1] and storekv.mirror == before_f[2]
         and storekv.orient == before_f[3] and storekv.flip == before_f[4],
         "F and O must have no effect")
  key(app, "a"); tick(app, 8)
  true_heading = 0
  key(app, nil, 0x87); key(app, "backspace", 8); key(app, "z"); tick(app, 2)
  -- moving: GPS course appears in the readout while the compass still drives the rose
  moving = true; tick(app, 5); print("  moving:", label_dump())
  -- compass drops out -> GPS course takes over, then nothing
  cfg.compass = function() return nil end; tick(app, 15); print("  mag lost:", label_dump())
  assert(label_dump():find("GPS course"), "expected GPS-course fallback")
  moving = false; tick(app, 5); print("  still:", label_dump())
  cfg.gps = function() return nil end; tick(app, 5); print("  no fix:", label_dump())
  key(app, "x"); tick(app, 1); assert(storekv.cal_ox == nil, "clear cal")
  if app.on_close then guarded(BUDGET, app.on_close) end
  print("  toasts:", table.concat(toasts, " / "))
end

scenarios.r8 = function()
  cfg = { w = 240, h = 276, caps = { sdk_ext = true, keyboard = false, touch = true, compass = false },
          contacts = contacts_fixture, self_lat = 0, self_lon = 0 }
  local moving = true
  cfg.gps = function() return { lat = 37.75, lon = -122.45, sats = 6, alt_m = 10, speed_kmh = 5.5, course = 90.0 } end
  storekv = {}
  wada = build_wada()
  local app = load_app()
  assert(guarded(BUDGET, app.on_open, cfg.w, cfg.h))
  print("  widgets:", "canvases=" .. widgets.canvases, "labels=" .. widgets.labels, "buttons=" .. widgets.buttons, "scroll=" .. tostring(widgets.scroll))
  assert(widgets.buttons == 1, "portrait touch board without compass should get one button")
  tick(app, 10); print("  gps heading:", label_dump())
  assert(label_dump():find("GPS course"))
  tap(app, 120, 60); tick(app, 2); print("  tap rose:", label_dump())
  drag(app, 120, 60, 40, 62, "left"); tick(app, 2)   -- swipe handled once (left), not also as a tap
  print("  drag:", label_dump()); assert(label_dump():find("none"), "swipe must cycle exactly once")
  local before = label_dump()
  send(app, { type = "down", x = 120, y = 60 }); send(app, { type = "up", x = 150, y = 60 }); tick(app, 1)  -- 30 px drag, no swipe event
  assert(label_dump() == before, "a drag without a swipe must be ignored")
  tap(app, 120, 270); tick(app, 2)          -- below the rose: no-op
  buttons[1].fn(); tick(app, 2); print("  button:", label_dump())
  -- portrait rows sit under the dial: ALT/SPD near y=280/298, so tap those to
  -- switch units and something well clear of them to prove it is bounded
  local u0 = storekv.u_alt
  tap(app, 20, 282); tick(app, 2)
  assert(storekv.u_alt ~= u0, "a tap on the ALT row must switch its units")
  local u1, s1 = storekv.u_alt, storekv.u_spd
  tap(app, 20, 440); tick(app, 2)      -- well below every row
  assert(storekv.u_alt == u1 and storekv.u_spd == s1, "a tap outside the rows must not change units")
  local b2 = label_dump(); swipe(app, "right"); swipe(app, "right"); tick(app, 1)   -- duplicate within 300 ms
  print("  dbl swipe:", label_dump()); assert(label_dump() ~= b2, "first swipe must count")
  clock_ms = clock_ms + 400; local b3 = label_dump(); swipe(app, "right"); tick(app, 1); assert(label_dump() ~= b3, "a later swipe must count")
  if app.on_close then guarded(BUDGET, app.on_close) end
end

scenarios.v4 = function()
  cfg = { w = 240, h = 276, caps = { sdk_ext = false, keyboard = false, touch = true, compass = false },
          contacts = {}, self_lat = 37.1, self_lon = -122.1 }
  storekv = {}
  wada = build_wada()
  local app = load_app()
  assert(guarded(BUDGET, app.on_open, cfg.w, cfg.h))
  tick(app, 10); print("  base sdk:", label_dump())
  swipe(app, "right"); tick(app, 1)
  if app.on_close then guarded(BUDGET, app.on_close) end
end

scenarios.pager = function()
  cfg = { w = 480, h = 178, caps = { sdk_ext = true, keyboard = true, touch = false, compass = false },
          contacts = contacts_fixture, self_lat = 37.75, self_lon = -122.45 }
  cfg.gps = function() return nil end
  storekv = {}
  wada = build_wada()
  local app = load_app()
  assert(guarded(BUDGET, app.on_open, cfg.w, cfg.h))
  tick(app, 10); print("  pager:", label_dump())
  key(app, "c"); tick(app, 1)
  if app.on_close then guarded(BUDGET, app.on_close) end
end

scenarios.pager_portrait_jumbo = function()
  cfg = { w = 222, h = 436, caps = { sdk_ext = true, keyboard = true, touch = false, compass = false },
          contacts = { { name = "WADAMESH-BASE-NODE-EXTRA", type = 2, ago_s = 1, lat = 37.8, lon = -122.4 } }, self_lat = 37.75, self_lon = -122.45 }
  cfg.gps = function() return { lat = 37.75, lon = -122.45, sats = 7, alt_m = 5, speed_kmh = 12.3, course = 215 } end
  storekv = {}
  wada = build_wada(); wada.ui.text_h = function(sz) return ({ [12] = 21, [14] = 22, [16] = 27 })[sz] or 21 end
  local app = load_app()
  assert(guarded(BUDGET, app.on_open, cfg.w, cfg.h))
  tick(app, 3); swipe(app, "right"); tick(app, 3); print("  pager-portrait-jumbo:", label_dump())
  for _, l in ipairs(labels) do if l.x ~= 0 then assert(#l.text <= 19, "too wide for a 210 px column at 18 px: " .. l.text) end end
  if app.on_close then guarded(BUDGET, app.on_close) end
end

scenarios.tanmatsu = function()
  cfg = { w = 480, h = 756, caps = { sdk_ext = true, keyboard = true, touch = false, compass = false },
          contacts = contacts_fixture, self_lat = 37.75, self_lon = -122.45 }
  cfg.gps = function() return { lat = 37.75, lon = -122.45, sats = 12, alt_m = 1203 } end
  storekv = {}
  wada = build_wada()
  local app = load_app()
  assert(guarded(BUDGET, app.on_open, cfg.w, cfg.h))
  tick(app, 5); swipe(app, "right"); tick(app, 5); print("  tanmatsu:", label_dump())
  if app.on_close then guarded(BUDGET, app.on_close) end
end

scenarios.audio_api = function()
  cfg = { w = 320, h = 196,
     caps = { sdk_ext = true, keyboard = true, touch = false,
         audio = true, audio_sd = true, sd_list = true } }
  wada = build_wada()
  local caps = wada.sys.caps()
    assert(caps.audio and caps.audio_wav and caps.audio_mp3 and caps.audio_sd,
      "audio capability flags do not match the WAV/MP3 contract")
  assert(wada.audio and wada.audio.play and wada.audio.pause and wada.audio.resume and
    wada.audio.stop and wada.audio.status, "wada.audio API is incomplete")

  assert(wada.audio.play("track.wav"))
  local status = wada.audio.status()
  assert(status.state == "playing" and status.path == "track.wav" and
    status.source == "app" and status.format == "wav", "app-storage play status is wrong")
  assert(wada.audio.pause() and wada.audio.status().state == "paused", "pause failed")
  assert(wada.audio.resume() and wada.audio.status().state == "playing", "resume failed")
  assert(wada.audio.play("next.wav") and wada.audio.status().path == "next.wav",
    "a second play must replace the active track")
  assert(wada.audio.play("sd:/Music/card.wav"))
  assert(wada.audio.status().source == "sd", "sd: source was not reported")

  local value, err = wada.audio.play("../escape.wav")
  assert(value == nil and err == "bad path", "app sandbox traversal was accepted")
  value, err = wada.audio.play("sd:/Music/../escape.wav")
  assert(value == nil and err == "bad path", "SD traversal was accepted")
    assert(wada.audio.play("track.mp3") and wada.audio.status().format == "mp3",
      "MP3 playback was not accepted or reported")
    value, err = wada.audio.play("track.flac")
    assert(value == nil and err == "unsupported format", "unknown audio format was accepted")

  assert(wada.audio.stop() and wada.audio.status().state == "stopped", "stop failed")
  assert(wada.audio.play("close.wav"))
  audio_host_close()
  assert(wada.audio.status().state == "stopped" and audio_log[#audio_log] == "release",
    "host close did not release playback")
  print("  transport + storage sandbox: PASS (" .. #audio_log .. " host calls)")

  if APP_PATH:match("audio_test%.lua$") then
    local app = load_app()
    assert(guarded(BUDGET, app.on_open, cfg.w, cfg.h))
    assert(#buttons == 5, "audio test app did not create all transport/source buttons")
    buttons[1].fn(); assert(wada.audio.status().state == "playing", "Play button failed")
    buttons[2].fn(); assert(wada.audio.status().state == "paused", "Pause button failed")
    buttons[3].fn(); assert(wada.audio.status().state == "playing", "Resume button failed")
    buttons[4].fn(); assert(wada.audio.status().state == "stopped", "Stop button failed")
    buttons[5].fn(); buttons[1].fn()
    assert(wada.audio.status().source == "app", "source switch did not select app storage")
    guarded(BUDGET, app.on_tick)
    guarded(BUDGET, app.on_close)
    assert(wada.audio.status().state == "stopped", "test app close did not stop playback")
    print("  scripts/audio_test.lua UI: PASS")
  end
end

-- instruction cost of one heavy tick (redraw forced every tick by spinning the heading)
scenarios.cost = function()
  cfg = { w = 320, h = 196, caps = { sdk_ext = true, keyboard = true, touch = false, compass = true, accel = true },
          contacts = contacts_fixture, self_lat = 37.75, self_lon = -122.45 }
  local th = 0
  cfg.compass = function() th = th + 7; return (attitude(th, 0, 0)) end
  cfg.accel = function() local _, a = attitude(th, 0, 0); return a end
  cfg.gps = function() return { lat = 37.75, lon = -122.45, sats = 9, alt_m = 42, speed_kmh = 10, course = 100 } end
  storekv = { cal_ox = BIAS.x, cal_oy = BIAS.y, cal_oz = BIAS.z, cal_r = 0.484 }
  wada = build_wada()
  local app = load_app()
  assert(guarded(BUDGET, app.on_open, cfg.w, cfg.h))
  swipe(app, "right")
  local worst = 0
  for i = 1, 50 do
    local n = 0
    debug.sethook(function() n = n + 1000 end, "", 1000)
    clock_ms = clock_ms + 150
    if i == 25 then clock_ms = clock_ms + 10000 end  -- force a contacts refresh inside a tick
    local ok, err = pcall(app.on_tick, 150)
    debug.sethook()
    assert(ok, err)
    if n > worst then worst = n end
  end
  print(string.format("  worst tick ~%d instructions (budget %d), draw ops so far %d", worst, BUDGET, drawlog.ops))
  assert(worst < BUDGET / 4, "tick too expensive")
end

local order = APP_PATH:find("/wardrive/", 1, true)
  and { "wardrive_utf8" }
  or { "declination", "align_nofix", "bearings_absolute", "m9", "r8", "v4", "pager", "pager_portrait_jumbo", "tanmatsu", "audio_api", "cost" }
for _, name in ipairs(order) do
  if SCENARIO == "all" or SCENARIO == name then
    print("== " .. name)
    reset_world()
    local ok, err = pcall(scenarios[name])
    if not ok then fails = fails + 1; print("  SCENARIO FAILED: " .. tostring(err)) end
  end
end
print(fails == 0 and "ALL OK" or ("FAILURES: " .. fails))
if fails ~= 0 then error("harness failures") end
