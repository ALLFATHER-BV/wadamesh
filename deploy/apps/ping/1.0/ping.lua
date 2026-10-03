-- Ping — wadamesh Lua app
-- Send a DM to any contact and measure round-trip time.
-- Both devices exchange their GPS position; the result shows RTT,
-- distance and cardinal bearing when both have a fix.
-- The app also auto-replies to incoming pings from other Ping users.
-- Requires: sdk_ext

local ui, sys, store, timer = wada.ui, wada.sys, wada.store, wada.timer
local mesh = wada.mesh
local geo   = wada.geo   -- great-circle helpers; nil on firmware without sdk_ext
local C = ui.colors

local app = {}

local PING_PFX = "WADAPING:"
local PONG_PFX = "WADAPONG:"

local W, H
local gps_lbl
local rows = {}      -- {name, result_lbl}  (parallel with contacts table)
local pending = {}   -- name → {sent_ms, lat_e6, lon_e6}

-- ---- helpers ---------------------------------------------------------------

local function gps_line()
  local fix = sys.gps()
  if not fix then return "no GPS fix" end
  local ns = fix.lat >= 0 and "N" or "S"
  local ew = fix.lon >= 0 and "E" or "W"
  return string.format("%.4f°%s  %.4f°%s  %d sats",
    math.abs(fix.lat), ns, math.abs(fix.lon), ew, fix.sats or 0)
end

local function dist_str(m)
  if m < 1000 then return string.format("%dm", math.floor(m + 0.5))
  else return string.format("%.1fkm", m / 1000.0) end
end

local function find_row(name)
  for _, r in ipairs(rows) do
    if r.name == name then return r end
  end
end

local function set_row_result(name, rtt_ms, dist_m, cardinal)
  local r = find_row(name)
  if not r then return end
  if rtt_ms then
    local s = tostring(rtt_ms) .. "ms"
    if dist_m and dist_m > 0 then
      s = s .. "  " .. dist_str(dist_m)
      if cardinal then s = s .. "  " .. cardinal end
    end
    r.result_lbl:set(s)
    r.result_lbl:color(C.good)
  else
    r.result_lbl:set("waiting…")
    r.result_lbl:color(C.sub)
  end
end

local function do_ping(name)
  local fix  = sys.gps()
  local lat6 = fix and fix.lat_e6 or 0
  local lon6 = fix and fix.lon_e6 or 0
  local ts   = sys.millis()
  local msg  = string.format("%s%d:%d:%d", PING_PFX, ts, lat6, lon6)
  local ok, err = mesh.send_dm(name, msg)
  if ok then
    pending[name] = { sent_ms = ts, lat_e6 = lat6, lon_e6 = lon6 }
    set_row_result(name, nil, nil, nil)   -- "waiting…"
    sys.toast("Ping → " .. name, 800)
  else
    sys.toast("Error: " .. tostring(err), 2000)
  end
end

-- ---- message handler -------------------------------------------------------

function app.on_message(m)
  if m.kind ~= "dm" then return end
  local text = tostring(m.text or "")

  -- ---- incoming PING: echo back with our position -------------------------
  if text:sub(1, #PING_PFX) == PING_PFX then
    local ts_s, _, _ = text:sub(#PING_PFX + 1):match("(-?%d+):(-?%d+):(-?%d+)")
    if ts_s then
      local fix = sys.gps()
      local my6lat = fix and fix.lat_e6 or 0
      local my6lon = fix and fix.lon_e6 or 0
      mesh.send_dm(m.sender, string.format("%s%s:%d:%d", PONG_PFX, ts_s, my6lat, my6lon))
    end
    return
  end

  -- ---- incoming PONG: compute RTT + distance ------------------------------
  if text:sub(1, #PONG_PFX) == PONG_PFX then
    local ts_s, their_lat_s, their_lon_s =
      text:sub(#PONG_PFX + 1):match("(-?%d+):(-?%d+):(-?%d+)")
    if not ts_s then return end

    local sent_ms = tonumber(ts_s)
    local rtt     = sys.millis() - sent_ms

    local dist_m, cardinal
    local p = pending[m.sender]
    if p and geo and tonumber(their_lat_s) ~= 0 and p.lat_e6 ~= 0 then
      local my_lat  = p.lat_e6        / 1000000.0
      local my_lon  = p.lon_e6        / 1000000.0
      local th_lat  = tonumber(their_lat_s) / 1000000.0
      local th_lon  = tonumber(their_lon_s) / 1000000.0
      dist_m  = geo.distance(my_lat, my_lon, th_lat, th_lon)
      local brg = geo.bearing(my_lat, my_lon, th_lat, th_lon)
      cardinal  = geo.cardinal(brg)
    end

    pending[m.sender] = nil
    set_row_result(m.sender, rtt, dist_m, cardinal)

    local notice = m.sender .. ": " .. rtt .. "ms"
    if dist_m then notice = notice .. "  " .. dist_str(dist_m) end
    sys.toast(notice, 2500)
    return
  end
end

-- ---- lifecycle -------------------------------------------------------------

function app.on_open(w, h)
  W, H = w, h
  ui.scroll(true)

  local cy = 6
  gps_lbl = ui.label(gps_line(), 6, cy, 11, C.sub)
  gps_lbl:width(w - 12)
  cy = cy + 20

  local contacts = mesh.contacts()

  if #contacts == 0 then
    ui.label("No contacts visible", 8, cy, 12, C.sub)
    timer.every(5000)
    return
  end

  for _, ct in ipairs(contacts) do
    local name = ct.name
    local nl = ui.label(name, 8, cy, 13, C.text)
    nl:width(w - 68)
    local rl = ui.label("—", 8, cy + 16, 11, C.sub)
    rl:width(w - 68)
    local cap = name   -- capture for closure
    ui.button("Ping", w - 60, cy + 4, 52, 28, function()
      do_ping(cap)
    end)
    rows[#rows + 1] = { name = name, result_lbl = rl }
    cy = cy + 44
  end

  timer.every(5000)
end

function app.on_tick(dt)
  if gps_lbl then gps_lbl:set(gps_line()) end
end

function app.on_close() end

return app
