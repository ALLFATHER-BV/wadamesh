-- MapFetch — wadamesh Lua app
-- Pre-caches offline map tiles for a zone by panning the firmware's built-in
-- map view through each tile; the firmware downloads and saves any missing
-- tile to the SD card exactly as the Map tab does.
--
-- Swipe up/down : radius   |  swipe left/right : max zoom level
-- Tap           : start download / cancel
--
-- Requires: ext SDK (wada.map), a GPS fix, and Wi-Fi connected.
-- Contributed by samuelcoustet

local ui, sys, timer = wada.ui, wada.sys, wada.timer
local C = ui.colors
local app = {}

local RADIUS_KM = { 2, 5, 10, 20 }
local ZOOM_MAX  = { 12, 13, 14, 15 }
local Z_MIN     = 10
local MS_PER_TILE = 3000  -- time to display each tile (gives firmware time to DL)

local map_view, lbl_top, lbl_bot
local W, H

local r_idx   = 2  -- default 5 km
local zm_idx  = 3  -- default z14
local running = false
local tiles   = {}
local t_idx   = 0
local cached  = 0  -- tiles that map:tiles() > 0 at the time we visited

-- ---- Slippy-tile maths -------------------------------------------------------

local function tile_x(lon, z)
  return math.floor((lon + 180) / 360 * 2 ^ z)
end

local function tile_y(lat, z)
  local r = lat * math.pi / 180
  return math.floor((1 - math.log(math.tan(r) + 1 / math.cos(r)) / math.pi) / 2 * 2 ^ z)
end

local function tile_clat(ty, z)
  local s = math.pi * (1 - 2 * (ty + 0.5) / 2 ^ z)
  return math.deg(math.atan((math.exp(s) - math.exp(-s)) / 2))
end

local function tile_clon(tx, z)
  return (tx + 0.5) / 2 ^ z * 360 - 180
end

-- ---- Build the ordered list of (z, x, y) tiles for the current settings -----

local function build_tiles(lat, lon)
  local r   = RADIUS_KM[r_idx]
  local z_max = ZOOM_MAX[zm_idx]
  local dlat  = r / 111
  local dlon  = r / (111 * math.max(0.01, math.cos(lat * math.pi / 180)))
  local out = {}
  for z = Z_MIN, z_max do
    local x0 = tile_x(lon - dlon, z)
    local x1 = tile_x(lon + dlon, z)
    local y0 = tile_y(lat + dlat, z)  -- north edge → smaller y
    local y1 = tile_y(lat - dlat, z)  -- south edge → larger  y
    for ty = y0, y1 do
      for tx = x0, x1 do
        out[#out + 1] = { z = z, x = tx, y = ty }
      end
    end
  end
  return out
end

-- ---- UI helpers --------------------------------------------------------------

local function fmt_dur(s)
  if s < 60 then return s .. "s"
  elseif s < 3600 then return math.floor(s / 60) .. "m"
  else return math.floor(s / 3600) .. "h " .. (math.floor(s / 60) % 60) .. "m"
  end
end

local function show_setup(fix)
  local n = 0
  if fix then
    tiles = build_tiles(fix.lat, fix.lon)
    n = #tiles
  end
  local gps = fix
    and string.format("GPS %.4f %.4f", fix.lat, fix.lon)
    or  "no GPS fix"
  lbl_top:set(string.format("%s  |  r:%dkm  z%d-%d",
    gps, RADIUS_KM[r_idx], Z_MIN, ZOOM_MAX[zm_idx]))
  lbl_top:color(fix and C.accent or C.bad)
  if n > 0 then
    lbl_bot:set(string.format("%d tiles  ~%s  — tap to start  swipe to adjust",
      n, fmt_dur(n * MS_PER_TILE / 1000)))
    lbl_bot:color(C.sub)
  else
    lbl_bot:set("no GPS fix — cannot estimate tiles")
    lbl_bot:color(C.bad)
  end
  if fix and map_view then map_view:center(fix.lat, fix.lon, Z_MIN) end
end

local function show_progress()
  local t = tiles[t_idx]
  if not t then return end
  local rem  = (#tiles - t_idx) * MS_PER_TILE / 1000
  lbl_top:set(string.format("z%d  tile %d/%d  cached:%d  ~%s left",
    t.z, t_idx, #tiles, cached, fmt_dur(rem)))
  lbl_top:color(C.accent)
  lbl_bot:set("tap to cancel")
  lbl_bot:color(C.sub)
end

-- ---- App lifecycle ----------------------------------------------------------

function app.on_open(w, h)
  W, H = w, h

  if not sys.caps().map then
    ui.label("No map support on this board (needs ext SDK)", 6, 6, 11, C.bad)
    return
  end

  local LH = ui.text_h(11)
  lbl_top = ui.label("", 4, 2, 11, C.accent);  lbl_top:width(w - 8)
  lbl_bot = ui.label("", 4, h - LH - 2, 11, C.sub); lbl_bot:width(w - 8)
  map_view = wada.map.view(0, LH + 4, w, h - (LH + 4) * 2)

  show_setup(sys.gps())
  sys.keep_awake(true)
  timer.every(5000)
end

function app.on_input(ev)
  if ev.type == "swipe" then
    if running then return end
    local d = ev.dir
    if d == "up"   then r_idx  = math.min(#RADIUS_KM, r_idx  + 1)
    elseif d == "down"  then r_idx  = math.max(1, r_idx  - 1)
    elseif d == "left"  then zm_idx = math.min(#ZOOM_MAX,  zm_idx + 1)
    elseif d == "right" then zm_idx = math.max(1, zm_idx - 1)
    end
    show_setup(sys.gps())

  elseif ev.type == "down" then
    if running then
      -- Cancel
      running = false
      timer.every(5000)
      lbl_top:set(string.format("Cancelled — %d/%d tiles visited (%d cached)",
        t_idx, #tiles, cached))
      lbl_top:color(C.bad)
      lbl_bot:set("swipe to adjust  tap to restart")
      lbl_bot:color(C.sub)
      show_setup(sys.gps())
    else
      -- Start
      local fix = sys.gps()
      if not fix then
        lbl_top:set("Need a GPS fix to start"); lbl_top:color(C.bad)
        return
      end
      tiles  = build_tiles(fix.lat, fix.lon)
      t_idx  = 0
      cached = 0
      if #tiles == 0 then
        lbl_top:set("No tiles in range"); lbl_top:color(C.bad)
        return
      end
      running = true
      timer.every(MS_PER_TILE)
    end
  end
end

function app.on_tick(dt)
  if not running then
    show_setup(sys.gps())
    return
  end

  -- Visit the next tile
  t_idx = t_idx + 1
  if t_idx > #tiles then
    running = false
    timer.every(5000)
    lbl_top:set(string.format("Done — %d tiles visited, %d already/now cached",
      #tiles, cached))
    lbl_top:color(C.good)
    lbl_bot:set("swipe to adjust  tap for another area")
    lbl_bot:color(C.sub)
    return
  end

  local t   = tiles[t_idx]
  local lat = tile_clat(t.y, t.z)
  local lon = tile_clon(t.x, t.z)
  map_view:center(lat, lon, t.z)
  if map_view:tiles() > 0 then cached = cached + 1 end
  show_progress()
end

function app.on_close()
  if map_view then map_view:close() end
  sys.keep_awake(false)
end

return app
