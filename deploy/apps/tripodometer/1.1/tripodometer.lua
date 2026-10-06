-- Trip Odometer 1.1 — wadamesh Lua app
-- GPS trip computer: distance, time, avg/max speed, min/current altitude,
-- return-to-home bearing.
-- Tap Start/Stop to track, Reset to clear. Saves your trip across app restarts.
-- Contributed by samuelcoustet

local ui, sys, geo, store, timer = wada.ui, wada.sys, wada.geo, wada.store, wada.timer
local C = ui.colors

local app = {}

local MIN_SPEED_KMH = 2.0  -- below this, GPS drift is ignored
local POLL_MS       = 2000

-- trip state
local total_km, max_kmh, avg_kmh
local min_alt_m, elapsed_s
local last_lat, last_lon
local home_lat, home_lon           -- set on first GPS fix after Start
local running, fix

-- labels
local spd_lbl, dist_lbl, time_lbl
local avgspd_lbl, maxspd_lbl
local alt_lbl, minalt_lbl
local home_lbl
local sats_lbl, st_lbl

-- ── Helpers ────────────────────────────────────────────────────────────────

local function haversine(lat1, lon1, lat2, lon2)
  local R    = 6371.0
  local dlat = (lat2 - lat1) * math.pi / 180.0
  local dlon = (lon2 - lon1) * math.pi / 180.0
  local a    = math.sin(dlat * 0.5) ^ 2
              + math.cos(lat1 * math.pi / 180.0)
              * math.cos(lat2 * math.pi / 180.0)
              * math.sin(dlon * 0.5) ^ 2
  return R * 2.0 * math.atan(math.sqrt(a), math.sqrt(1.0 - a))
end

local function bearing(lat1, lon1, lat2, lon2)
  if geo then return geo.bearing(lat1, lon1, lat2, lon2) end
  local dlon = (lon2 - lon1) * math.pi / 180.0
  local y    = math.sin(dlon) * math.cos(lat2 * math.pi / 180.0)
  local x    = math.cos(lat1 * math.pi / 180.0) * math.sin(lat2 * math.pi / 180.0)
              - math.sin(lat1 * math.pi / 180.0) * math.cos(lat2 * math.pi / 180.0) * math.cos(dlon)
  return (math.atan(y, x) * 180.0 / math.pi + 360.0) % 360.0
end

local function cardinal(brg)
  if geo then return geo.cardinal(brg) end
  local dirs = {"N","NE","E","SE","S","SW","W","NW"}
  return dirs[math.floor((brg + 22.5) / 45) % 8 + 1]
end

local function fmt_time(s)
  local h   = math.floor(s / 3600)
  local m   = math.floor((s % 3600) / 60)
  local sec = s % 60
  if h > 0 then return string.format("%d:%02d:%02d", h, m, sec) end
  return string.format("%d:%02d", m, sec)
end

local function fmt_dist(km)
  if km < 1.0 then return string.format("%d m", math.floor(km * 1000)) end
  return string.format("%.2f km", km)
end

local function save()
  store.set("trip_km_x1000",   math.floor(total_km  * 1000))
  store.set("trip_max_x10",    math.floor(max_kmh   * 10))
  store.set("trip_s",          elapsed_s)
  store.set("trip_minalt",     min_alt_m or -9999)
end

local function redraw()
  local spd = (fix and fix.speed_kmh) or 0.0
  spd_lbl:set(string.format("%.1f", spd))
  dist_lbl:set(fmt_dist(total_km))
  time_lbl:set(fmt_time(elapsed_s))
  maxspd_lbl:set(string.format("%.1f km/h", max_kmh))
  if elapsed_s > 0 then
    avg_kmh = total_km / elapsed_s * 3600.0
  else
    avg_kmh = 0.0
  end
  avgspd_lbl:set(string.format("%.1f km/h", avg_kmh))

  if fix then
    alt_lbl:set(string.format("%d m", fix.alt_m or 0))
    if min_alt_m then
      minalt_lbl:set(string.format("min %d m", min_alt_m))
    else
      minalt_lbl:set("min -- m")
    end
    sats_lbl:set(string.format("%d sats", fix.sats or 0))
    if running then
      st_lbl:set("tracking"); st_lbl:color(C.good)
    else
      st_lbl:set("paused");   st_lbl:color(C.sub)
    end
  else
    alt_lbl:set("-- m")
    minalt_lbl:set("min -- m")
    sats_lbl:set("no GPS")
    st_lbl:set("no fix"); st_lbl:color(C.bad)
  end

  -- return-to-home row
  if home_lat and fix and fix.lat and fix.lon then
    local d    = haversine(home_lat, home_lon, fix.lat, fix.lon)
    local brg  = bearing(fix.lat, fix.lon, home_lat, home_lon)
    local card = cardinal(brg)
    home_lbl:set(string.format("%s  %s", fmt_dist(d), card))
    home_lbl:color(C.accent)
  elseif running then
    home_lbl:set("acquiring…"); home_lbl:color(C.sub)
  else
    home_lbl:set("--"); home_lbl:color(C.sub)
  end
end

-- ── App lifecycle ──────────────────────────────────────────────────────────

function app.on_open(w, h)
  total_km  = store.get("trip_km_x1000", 0) / 1000.0
  max_kmh   = store.get("trip_max_x10",  0) / 10.0
  elapsed_s = store.get("trip_s",        0)
  local saved_min = store.get("trip_minalt", -9999)
  min_alt_m = (saved_min ~= -9999) and saved_min or nil
  running   = false
  fix       = nil
  last_lat  = nil; last_lon  = nil
  home_lat  = nil; home_lon  = nil
  avg_kmh   = 0.0

  local col2 = math.floor(w / 2)

  -- large speed at top
  ui.label("SPEED", 8, 6, 11, C.sub)
  spd_lbl = ui.label("0.0", 8, 20, 36, C.accent)
  ui.label("km/h", 8, 60, 12, C.sub)

  -- row 2: distance | time
  local y2 = 84
  ui.label("DISTANCE", 8,    y2, 11, C.sub)
  dist_lbl = ui.label("0 m", 8,  y2 + 14, 20, C.text)
  ui.label("TIME",     col2, y2, 11, C.sub)
  time_lbl = ui.label("0:00",col2, y2 + 14, 20, C.text)

  -- row 3: avg speed | max speed
  local y3 = y2 + 48
  ui.label("AVG SPEED", 8,    y3, 11, C.sub)
  avgspd_lbl = ui.label("0.0 km/h", 8, y3 + 14, 16, C.text)
  ui.label("MAX SPEED", col2, y3, 11, C.sub)
  maxspd_lbl = ui.label("0.0 km/h", col2, y3 + 14, 16, C.text)

  -- row 4: altitude | min altitude
  local y4 = y3 + 42
  ui.label("ALTITUDE", 8,    y4, 11, C.sub)
  alt_lbl    = ui.label("-- m",     8,    y4 + 14, 16, C.text)
  minalt_lbl = ui.label("min -- m", col2, y4 + 14, 13, C.sub)

  -- row 5: return to home
  local y5 = y4 + 38
  ui.label("HOME", 8, y5, 11, C.sub)
  home_lbl = ui.label("--", 8, y5 + 14, 16, C.sub)

  -- GPS status
  local y6 = y5 + 34
  sats_lbl = ui.label("no GPS", 8,    y6, 13, C.sub)
  st_lbl   = ui.label("no fix", col2, y6, 13, C.sub)

  -- buttons anchored to bottom
  local bw    = math.floor((w - 20) / 2)
  local btn_y = h - 46
  ui.button("Start / Stop", 8, btn_y, bw, 36, function()
    running = not running
    if running then
      last_lat = nil; last_lon = nil  -- fresh segment on resume
    end
    redraw()
  end)
  ui.button("Reset", 12 + bw, btn_y, bw, 36, function()
    total_km  = 0; max_kmh = 0; elapsed_s = 0; avg_kmh = 0
    min_alt_m = nil
    last_lat  = nil; last_lon  = nil
    home_lat  = nil; home_lon  = nil
    running   = false
    save()
    redraw()
    sys.toast("Trip reset", 1200)
  end)

  timer.every(POLL_MS)
  redraw()
end

function app.on_tick()
  fix = sys.gps()

  if running then
    elapsed_s = elapsed_s + math.floor(POLL_MS / 1000)

    if fix and fix.lat and fix.lon then
      local spd = fix.speed_kmh or 0.0
      if spd > max_kmh then max_kmh = spd end

      -- track altitude extremes
      if fix.alt_m then
        if min_alt_m == nil or fix.alt_m < min_alt_m then
          min_alt_m = fix.alt_m
        end
      end

      if spd >= MIN_SPEED_KMH then
        -- record home on first moving fix
        if home_lat == nil then
          home_lat = fix.lat; home_lon = fix.lon
        end

        if last_lat then
          local d = haversine(last_lat, last_lon, fix.lat, fix.lon)
          if d > 0.003 then  -- ignore < 3 m jitter
            total_km = total_km + d
          end
        end
        last_lat = fix.lat; last_lon = fix.lon
      end
    end

    if elapsed_s % 10 == 0 then save() end
  end

  redraw()
end

function app.on_input(ev)
  -- swipe down = quick reset (only when stopped)
  if ev.type == "swipe" and ev.dir == "down" and not running then
    total_km  = 0; max_kmh = 0; elapsed_s = 0; avg_kmh = 0
    min_alt_m = nil
    last_lat  = nil; last_lon  = nil
    home_lat  = nil; home_lon  = nil
    save(); redraw()
    sys.toast("Trip reset", 1200)
  end
end

function app.on_close()
  save()
end

return app
