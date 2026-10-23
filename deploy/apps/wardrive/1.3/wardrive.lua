-- Wardrive (Lua) — a LoRa coverage survey that logs to a CSV you can pull off
-- the device afterwards.
--
-- Reference app for the discovery half of the SDK. It works the way a survey
-- has to work: it PROBES rather than listens. A probe is a zero-hop request
-- that every node in earshot answers, so a reply proves the link works from
-- exactly where you are standing. Listening only ever tells you what happened
-- to transmit while you were there, which is a different and much weaker claim.
--
-- Each reply carries both directions of the link: how well we heard them, and
-- how well they heard us. They are rarely equal, and the asymmetry is the point
-- -- "I can hear the repeater but it cannot hear me" is not the same fact as
-- "no coverage", and only a probe reveals it.
--
-- 1.2 adds a REVIEW screen, because until now the run could only be read on a
-- computer after pulling the card, and a survey you cannot check in the field is
-- a survey you repeat. It reads the run's own CSV back and plots it on the
-- firmware's basemap, colouring each stop by whichever direction of the link
-- you are asking about. Probing pauses while you are
-- looking at it: a survey is expected to spend other people's airtime only on a
-- cadence its user chose, and nobody chose to keep probing while reading a map.
local ui, sys, mesh, fs, timer = wada.ui, wada.sys, wada.mesh, wada.fs, wada.timer
local C = ui.colors
local app = {}

-- Older firmware has neither of these; the SDK's own advice is to degrade rather
-- than to require a floor.
local clear = ui.clear or function() end
local tr = sys.tr or function(x) return x end

local SWEEP_MS = 20000        -- above the 15 s floor the firmware enforces on probes
local HARVEST_MS = 4000       -- replies land over the few seconds after a probe
-- The outside edge of a harvest. Past this the replies in hand were heard somewhere the current GPS
-- fix no longer describes, so they are dropped rather than mislocated. Three tick periods of slack
-- past the harvest window: a slow tick stays inside it, a slept display does not.
local STALE_MS = HARVEST_MS + 3000
local TYPE = { [1]="chat", [2]="repeater", [3]="room", [4]="sensor" }

local run, running, samples, sweeps, last_err = "run", false, 0, 0, nil
-- In-flight sweeps discarded rather than mislocated. Two causes, not one: the display slept through
-- the harvest window, or the survey was stopped mid-listen and the replies in hand were never banked.
local dropped_sweeps = 0
local node_count = 0
local phase, phase_at = "idle", 0
-- One predicate, used by the tick path and the Review button alike, so the two cannot drift apart -
-- which is exactly what happened when only one of them had the check.
local function harvest_fresh(now) return (now - phase_at) <= STALE_MS end
local hdr, gps_lbl, stat_lbl, rows = nil, nil, nil, {}
local nodes = {}              -- pubkey -> { name, type, best, worst, seen }
local pending = {}            -- lines waiting on the 1 write/sec limit

-- ---- review state -----------------------------------------------------------
-- view is "survey" or "review". Every screen-building path clears first and
-- re-creates, which is the documented way to hold more than one screen: handles
-- taken before a clear are inert afterwards, so nothing may be reused across it.
local W, H = 0, 0
local view = "survey"
local was_running = false     -- so leaving review restores what the user chose
local MODES = { "heard them", "heard us", "asymmetry" }
local mode = 1
local map, map_note, rv_stat, rv_legend, rv_plot = nil, nil, nil, nil, nil
local plot_px = 0             -- the height build_review actually gave the plot
-- The loader is a state machine on purpose. A 32 KB log is several hundred rows
-- and parsing it in one callback would sail past the 100,000 instruction budget
-- and close the app; a window per tick stays nowhere near it.
local ld = nil
local READ_WIN = 4096
-- How many samples are actually drawn. Every marker and every segment is an LVGL object created
-- synchronously, and the screen shares its loop with the radio, so a long drive must not turn into a
-- long callback. The run is drawn at a stride instead, which keeps the route's shape.
local MAX_DRAW = 150

local function logname() return run .. ".csv" end

-- Return at most max_bytes without splitting a UTF-8 sequence. The row's
-- fixed-width columns are byte-budgeted, not character-budgeted; a raw
-- string.sub(1, 14) cut Ouderkerk + sun + VS16 inside the final codepoint and
-- left LVGL unable to advance through the label (#323, same class as #223).
local function utf8_prefix_bytes(text, max_bytes)
  local offset, last, length = 1, 0, #text
  while offset <= length and offset <= max_bytes do
    local first = text:byte(offset)
    local width = first <= 0x7F and 1
      or (first >= 0xC2 and first <= 0xDF and 2)
      or (first >= 0xE0 and first <= 0xEF and 3)
      or (first >= 0xF0 and first <= 0xF4 and 4) or 0
    if width == 0 or offset + width - 1 > length or offset + width - 1 > max_bytes then break end
    local valid = true
    for i = 2, width do
      local byte = text:byte(offset + i - 1)
      if byte < 0x80 or byte > 0xBF then valid = false; break end
    end
    local second = width > 1 and text:byte(offset + 1) or 0
    if (first == 0xE0 and second < 0xA0) or (first == 0xED and second > 0x9F) or
       (first == 0xF0 and second < 0x90) or (first == 0xF4 and second > 0x8F) then valid = false end
    if not valid then break end
    last = offset + width - 1
    offset = last + 1
  end
  return text:sub(1, last)
end

-- Lua here is built with 32-bit floats, so fix.lat is good to about a metre and
-- no better. fix.lat_e6 is the same reading as an exact integer in
-- micro-degrees, which is what belongs in a log: a survey you plot months later
-- should not carry rounding the device never had.
local HEADER = "epoch,lat_e6,lon_e6,alt_m,pubkey,name,type,rssi,snr,their_snr,hops"
-- nil means "not yet known": the file may already carry a header from an earlier session, and
-- wrote_header is process-local, so assuming false appended a second header mid-file on every
-- reopen. The device's own reader skips those lines; a spreadsheet on a laptop does not.
local wrote_header = nil

-- The filesystem allows one write a second. Sweeps produce a burst of rows, so
-- they queue here and drain a chunk per tick instead of being dropped.
local function flush()
  if #pending == 0 then return end
  local chunk = table.concat(pending, "\n") .. "\n"
  if wrote_header == nil then
    local d = fs.read(logname(), 0, 8)
    wrote_header = (d ~= nil and #d > 0)     -- a non-empty file already has one
  end
  if not wrote_header then chunk = HEADER .. "\n" .. chunk end
  local ok = fs.append(logname(), chunk)
  if ok then pending, wrote_header = {}, true end
end

local function record(fix, hit)
  local key = hit.pubkey
  local n = nodes[key]
  if not n then
    n = { name = hit.name or key, type = hit.type, best = hit.snr, worst = hit.snr, seen = 0 }
    nodes[key] = n
    node_count = node_count + 1
  end
  if hit.snr > n.best  then n.best  = hit.snr end
  if hit.snr < n.worst then n.worst = hit.snr end
  n.seen = n.seen + 1
  if hit.name then n.name = hit.name end

  samples = samples + 1
  pending[#pending + 1] = string.format("%d,%d,%d,%d,%s,%s,%d,%d,%.2f,%.2f,%d",
    fix.time or sys.epoch() or 0, fix.lat_e6, fix.lon_e6, fix.alt_m or 0,
    key, (hit.name or ""):gsub(",", " "), hit.type,
    hit.rssi, hit.snr, hit.their_snr, hit.hops)
end

local function sweep()
  local tag, err = mesh.discover()          -- every node type
  if not tag then last_err = err; return false end
  last_err = nil
  sweeps = sweeps + 1
  return true
end

local function harvest()
  local fix = sys.gps()
  if not fix then
    -- No fix means the sample cannot be placed, so it is discarded rather than
    -- logged at 0,0. A survey file with phantom points at Null Island is worse
    -- than a shorter one.
    mesh.discover_clear()
    return
  end
  for _, hit in ipairs(mesh.discovered()) do record(fix, hit) end
  mesh.discover_clear()                     -- next sample must not inherit this one
end

local function redraw()
  local fix = sys.gps()
  if fix then
    gps_lbl:set(string.format("%.5f, %.5f  %dm  %d sats", fix.lat, fix.lon, fix.alt_m or 0, fix.sats))
    gps_lbl:color(C.good)
  else
    gps_lbl:set(tr("waiting for a GPS fix - samples are discarded until then"))
    gps_lbl:color(C.bad)
  end

  local state = running and (phase == "probe" and "listening..." or "sweeping") or "stopped"
  stat_lbl:set(string.format("%s  |  %s  |  %d sweeps, %d samples, %d nodes%s%s",
    run, state, sweeps, samples, node_count,
    dropped_sweeps > 0 and ("  ·  " .. dropped_sweeps .. " dropped") or "",
    last_err and ("  [" .. last_err .. "]") or ""))
  stat_lbl:color(last_err and C.bad or C.accent)

  local list = {}
  for key, n in pairs(nodes) do list[#list + 1] = { key = key, n = n } end
  table.sort(list, function(a, b) return a.n.best > b.n.best end)
  for i = 1, #rows do
    local e = list[i]
    if e then
      rows[i]:set(string.format("%-14s %-8s best %5.1f  worst %5.1f  x%d",
        utf8_prefix_bytes(e.n.name, 14), TYPE[e.n.type] or "?", e.n.best, e.n.worst, e.n.seen))
      rows[i]:color(e.n.best > 0 and C.good or C.text)
    else
      rows[i]:set(i == 1 and tr("nothing has answered a probe yet") or "")
      rows[i]:color(C.sub)
    end
  end
end

-- ---- review: reading the run back ------------------------------------------

-- Which link direction a sample is judged on. "asymmetry" is the one the app
-- exists to show: a positive value means we hear them better than they hear us,
-- which is where a repeater earns its place.
-- A stop is one GPS fix, and a sweep writes one row per responder at that same fix, so a street
-- corner where ten nodes answered is ten rows sharing one coordinate. Drawing a row therefore drew
-- whichever responder happened to come first in that sweep, and mesh.discovered() is in arrival
-- order - not strongest, not worst. The marker now answers the question the button actually asks,
-- taken over every responder heard at that stop.
local function metric_of(st)
  if mode == 2 then return st.them_max end      -- the best anyone heard US from here
  if mode == 3 then return st.asym end          -- the most one-sided link here, signed
  return st.snr_max                             -- the best WE heard anything from here
end

local function colour_for(v)
  if mode == 3 then
    -- asymmetry: near zero is a balanced link, and that is the good case
    if v > 6 or v < -6 then return C.bad end
    if v > 3 or v < -3 then return C.accent end
    return C.good
  end
  if v >= 0 then return C.good end
  if v >= -10 then return C.accent end
  return C.bad
end

local function ld_reset()
  -- every field the rest of the code reads is created here, including plotted: relying on a
  -- nil read would work in Lua but leaves the reader guessing whether it was meant to exist
  ld = { off = 0, total = nil, tail = "", stops = {}, n = 0, skipped = 0, done = false, plotted = false,
         minlat = nil, maxlat = nil, minlon = nil, maxlon = nil, metres = 0, wrapped = false,
         prev_lat = nil, prev_lon = nil, by_node = {}, node_n = 0, step = 1,
         stop_n = 0, pos_i = 0, keep_every = 1, last_key = nil, cur = nil, tailstop = nil }
end

local MAX_STOPS = 400
local function stop_push(lat, lon)
  ld.pos_i = ld.pos_i + 1
  ld.stop_n = ld.stop_n + 1
  if #ld.stops >= MAX_STOPS then
    -- Halving keeps the odd indices, which drops the newest stop every time. Carry it over, or the
    -- drawn route walks backwards from the truth a little at each collapse.
    local last = ld.stops[#ld.stops]
    local out, j = {}, 0
    for i = 1, #ld.stops, 2 do j = j + 1; out[j] = ld.stops[i] end
    if out[j] ~= last then j = j + 1; out[j] = last end
    ld.stops = out
    ld.keep_every = ld.keep_every * 2
  end
  -- Always build the stop and always aggregate into it, whether or not it is kept for drawing: the
  -- last position seen has to be available at the end of the walk even if the stride passed over it.
  local st = { lat = lat, lon = lon, n = 0 }
  ld.tailstop = st
  if (ld.pos_i % ld.keep_every) == 0 then
    ld.stops[#ld.stops + 1] = st
    st.kept = true
  end
  return st
end

-- The route ends where the run ended. Called once, when the walk completes.
local function stops_finalise()
  local t = ld and ld.tailstop
  if t and not t.kept and #ld.stops > 0 then
    ld.stops[#ld.stops + 1] = t
    t.kept = true
  end
end

local function ld_row(line)
  -- epoch,lat_e6,lon_e6,alt_m,pubkey,name,type,rssi,snr,their_snr,hops
  -- Split on the sentinel rather than with "([^,]*)", which matches the empty
  -- string between every pair of characters and walks the line twice.
  local f, n = {}, 0
  for part in (line .. ","):gmatch("([^,]*),") do
    n = n + 1; f[n] = part
    if n >= 11 then break end
  end
  if n < 11 then ld.skipped = ld.skipped + 1; return end
  local lat_e6, lon_e6 = tonumber(f[2]), tonumber(f[3])
  local snr, their = tonumber(f[9]), tonumber(f[10])
  if not (lat_e6 and lon_e6 and snr and their) then ld.skipped = ld.skipped + 1; return end
  local lat, lon = lat_e6 / 1000000, lon_e6 / 1000000
  -- A row at exactly 0,0 is the Null Island shape the writer refuses to create;
  -- if one appears it came from somewhere else, so it is counted, not plotted.
  if lat_e6 == 0 and lon_e6 == 0 then ld.skipped = ld.skipped + 1; return end

  ld.n = ld.n + 1
  if not ld.minlat or lat < ld.minlat then ld.minlat = lat end
  if not ld.maxlat or lat > ld.maxlat then ld.maxlat = lat end
  if not ld.minlon or lon < ld.minlon then ld.minlon = lon end
  if not ld.maxlon or lon > ld.maxlon then ld.maxlon = lon end

  if ld.prev_lat and wada.geo then
    local ok, d = pcall(wada.geo.distance, ld.prev_lat, ld.prev_lon, lat, lon)
    if ok and d then ld.metres = ld.metres + d end
  end
  ld.prev_lat, ld.prev_lon = lat, lon

  local key = f[5]
  local nd = ld.by_node[key]
  if not nd then
    nd = { name = (f[6] ~= "" and f[6]) or key, type = tonumber(f[7]) or 0,
           best = snr, worst = snr, best_them = their, asym = snr - their, seen = 0 }
    ld.by_node[key] = nd
    ld.node_n = ld.node_n + 1
  end
  if snr > nd.best then nd.best = snr end
  if snr < nd.worst then nd.worst = snr end
  if their > nd.best_them then nd.best_them = their end
  -- The imbalance has to come from a SINGLE reply. Peak-heard-them minus peak-heard-us can pair two
  -- different moments and cancel a genuinely one-sided link to zero (10/-5 then -5/10 reads as 0).
  do
    local a = snr - their
    if (a < 0 and -a or a) > (nd.asym < 0 and -nd.asym or nd.asym) then nd.asym = a end
  end
  nd.seen = nd.seen + 1

  -- Rows sharing a coordinate are one stop. They arrive contiguously, because a sweep harvests
  -- every reply against a single fix, so comparing with the previous row's raw integers is enough
  -- and costs no table lookup.
  local ekey = f[2] .. "," .. f[3]
  if ekey ~= ld.last_key then
    ld.last_key = ekey
    ld.cur = stop_push(lat, lon)
  end
  local st = ld.cur
  if st then
    st.n = st.n + 1
    if not st.snr_max  or snr   > st.snr_max  then st.snr_max  = snr   end
    if not st.snr_min  or snr   < st.snr_min  then st.snr_min  = snr   end
    if not st.them_max or their > st.them_max then st.them_max = their end
    if not st.them_min or their < st.them_min then st.them_min = their end
    local a = snr - their
    if not st.asym or (a < 0 and -a or a) > (st.asym < 0 and -st.asym or st.asym) then st.asym = a end
  end
end

-- One window per tick. Returns true while there is more to do.
local function ld_step()
  if not ld or ld.done then return false end
  local data, total = fs.read(logname(), ld.off, READ_WIN)
  if not data then
    -- no such file yet, or nothing left to hand back: either way the walk is over, but a tail
    -- held from the previous window is still a real row and must not be thrown away
    if ld.tail ~= "" and not ld.tail:find("^epoch,") then ld_row(ld.tail); ld.tail = "" end
    stops_finalise()
    ld.done = true
    return false
  end
  ld.total = total or (ld.off + #data)
  ld.off = ld.off + #data
  local buf = ld.tail .. data
  local last = 0
  for line, pos in buf:gmatch("([^\n]*)\n()") do
    last = pos - 1
    if line ~= "" and not line:find("^epoch,") then ld_row(line) end
  end
  ld.tail = buf:sub(last + 1)
  -- A short read is NOT the end: flush() still drains queued rows while the review screen is up,
  -- so the file can grow between two windows. Only a read that yields nothing means finished, and
  -- the leftover tail is parsed at that point because it can only then be a complete final line.
  if #data == 0 then
    if ld.tail ~= "" and not ld.tail:find("^epoch,") then ld_row(ld.tail) end
    ld.tail = ""
    stops_finalise()
    ld.done = true
  end
  return not ld.done
end

-- Pick the zoom that just fits the run's own bounding box.
--
-- Both axes are measured in Web Mercator's own normalised units, where the whole
-- world is 1.0 and a tile is 1/2^z of it, so a span fits when
-- span_norm <= pixels / (256 * 2^z). Longitude is linear in that space, but
-- LATITUDE IS NOT: the projection stretches by 1/cos(latitude), so treating a
-- degree of latitude as a degree of longitude picks a zoom that is too close and
-- lets the route run off the view - and by more the further from the equator.
-- Hence the real y transform rather than a shared 360.
local function merc_y(lat)
  -- clamp before the tangent: the projection is asymptotic at the poles
  if lat >  85 then lat =  85 end
  if lat < -85 then lat = -85 end
  local r = lat * math.pi / 180
  return 0.5 - math.log(math.tan(math.pi / 4 + r / 2)) / (2 * math.pi)
end

-- The inverse, needed for the centre. Fitting the span in Mercator units and then centring on the
-- arithmetic mean LATITUDE re-introduces the same error one level down: the visual middle of a
-- north-south box is the inverse of the mean y, and on a long run away from the equator the
-- geographic midpoint sits south of it, so the northern end clips off a correctly fitted view.
local function merc_lat(y)
  local r = 2 * (math.atan(math.exp((0.5 - y) * 2 * math.pi)) - math.pi / 4)
  return r * 180 / math.pi
end

local function fit_zoom(px, py)
  if not ld or not ld.minlat then return 14 end
  local sy = math.max(math.abs(merc_y(ld.minlat) - merc_y(ld.maxlat)), 1e-9)
  -- A run either side of the antimeridian has a min near -180 and a max near +180, so the naive
  -- span is ~358 degrees and the fit collapses to zoom 1 - the whole world, to show one street.
  -- Rather than pretend to handle a wrap, fit on latitude alone and say so on the card.
  local sx = (ld.maxlon - ld.minlon) / 360
  if sx > 0.5 then ld.wrapped = true end
  sx = math.max(sx, 1e-9)
  local function z_for(span_norm, pixels)
    if not (pixels > 0) then return 1 end
    local v = pixels / (256 * span_norm)
    if not (v > 1) then return 1 end
    return math.floor(math.log(v) / math.log(2))
  end
  local z = ld.wrapped and z_for(sy, py) or math.min(z_for(sx, px), z_for(sy, py))
  -- A single-point run gives a 1e-9 span and therefore the maximum zoom, which is
  -- right: there is nothing to fit, so show the tightest view of where it was.
  if not (z == z) then z = 14 end        -- nan guard; z ~= z is only true for nan
  if z < 1 then z = 1 end
  if z > 19 then z = 19 end
  return z
end

local function review_plot()
  if not ld then return end
  local drawn = 0
  local step = math.max(1, math.ceil(#ld.stops / MAX_DRAW))
  ld.step = step
  if map then
    map:clear()
    if ld.minlat then
      -- The centre is the inverse of the mean Mercator y, not the mean latitude: on a long
      -- north-south run the two differ, and centring on the latter clips the northern end off a
      -- span that fit_zoom sized correctly.
      local clat = merc_lat((merc_y(ld.minlat) + merc_y(ld.maxlat)) / 2)
      local clon
      if ld.wrapped then
        -- Unwrap onto 0..360 before averaging, then fold back, so the centre lands near the
        -- antimeridian the run actually straddles instead of on the far side of the world.
        local a = ld.minlon < 0 and ld.minlon + 360 or ld.minlon
        local b = ld.maxlon < 0 and ld.maxlon + 360 or ld.maxlon
        clon = (a + b) / 2
        if clon > 180 then clon = clon - 360 end
      else
        clon = (ld.minlon + ld.maxlon) / 2
      end
      map:center(clat, clon, fit_zoom(W - 8, plot_px))
      -- One marker and one segment are each an LVGL object created synchronously, and the host puts
      -- no ceiling on an app's view (k_map_markers_max = 256 belongs to the firmware's own Map tab),
      -- so the bound comes from here. The stride runs over STOPS, each already the aggregate of
      -- every responder heard at that fix.
      local prev = nil
      for i = 1, #ld.stops, step do
        local st = ld.stops[i]
        if prev then map:line(prev.lat, prev.lon, st.lat, st.lon, C.sub, 1) end
        if map:marker(st.lat, st.lon, colour_for(metric_of(st)), 3) then drawn = drawn + 1 end
        prev = st
      end
    end
    local tiles = 0
    local ok, t = pcall(function() return map:tiles() end)
    if ok and t then tiles = t end
    if not ld.minlat then
      -- Nothing was plotted, so map:center() was never called and no render was ever requested:
      -- tiles is 0 because of that, not because the area is uncached. Say nothing about the cache.
      map_note:set("")
    elseif tiles == 0 then
      -- Short enough to wrap to two lines at the narrowest label width (230 px), which is what the
      -- layout below reserves for it. The longer wording ran to three and overdrew the legend.
      map_note:set(tr("no tiles cached here - blank ground, not open water"))
      map_note:color(C.bad)
    else
      map_note:set(string.format("%d tiles  ·  %d of %d stops drawn%s%s", tiles, drawn, ld.stop_n,
        step > 1 and ("  ·  1 in " .. step) or "",
        ld.keep_every > 1 and ("  ·  sampled 1 in " .. ld.keep_every .. " while reading") or ""))
      map_note:color(C.sub)
    end
  elseif rv_plot then
    -- No map view. caps().map and caps().discover are the same flag today, so this is a view that
    -- failed to open rather than a board without a basemap. The geography is drawn bare and
    -- self-scaled; without tiles there is no way to say WHERE this is, so it is labelled a shape
    -- rather than a map. The same stride
    -- applies: these are host draw calls on the loop the radio shares, and an uncapped run would
    -- put hundreds of them in one callback.
    local pw, ph = math.min(W - 8, 480), math.min(plot_px, 480)   -- must match the ui.canvas clamp below
    rv_plot:fill(C.bg)
    if ld.minlat then
      local dlat = math.max(ld.maxlat - ld.minlat, 1e-6)
      local dlon = math.max(ld.maxlon - ld.minlon, 1e-6)
      local function sx(lon) return math.floor(4 + (lon - ld.minlon) / dlon * (pw - 8)) end
      local function sy(lat) return math.floor(4 + (ld.maxlat - lat) / dlat * (ph - 8)) end
      local prev = nil
      for i = 1, #ld.stops, step do
        local st = ld.stops[i]
        local x, y = sx(st.lon), sy(st.lat)
        if prev then rv_plot:line(prev[1], prev[2], x, y, C.sub) end
        rv_plot:circle(x, y, 2, colour_for(metric_of(st)))
        drawn = drawn + 1
        prev = { x, y }
      end
    end
    rv_plot:text(4, 2, string.format(tr("route shape, not to scale - %d of %d stops"), drawn, ld.stop_n), C.sub, 12)
  end
end

local function review_stat()
  if not ld then return end
  if not ld.done then
    rv_stat:set(string.format(tr("reading %s ... %d rows"), logname(), ld.n))
    rv_stat:color(C.sub)
    return
  end
  if ld.n == 0 then
    rv_stat:set(string.format(tr("%s holds no plottable rows yet"), logname()))
    rv_stat:color(C.bad)
    return
  end
  local km = ld.metres / 1000
  rv_stat:set(string.format("%s  ·  %d samples at %d stops, %d nodes  ·  %.2f km%s%s",
    run, ld.n, ld.stop_n, ld.node_n, km,
    ld.wrapped and "  ·  crosses the antimeridian, fitted on latitude only" or "",
    ld.skipped > 0 and ("  ·  " .. ld.skipped .. " unusable") or ""))
  rv_stat:color(C.accent)
  -- from a single reply, per node, so a genuinely one-sided link cannot be cancelled out by a
  -- later reply that leaned the other way
  local worst_name, worst_gap = nil, nil
  for _, nd in pairs(ld.by_node) do
    local g = nd.asym < 0 and -nd.asym or nd.asym
    if not worst_gap or g > worst_gap then worst_gap, worst_name = g, nd.name end
  end
  local legend
  if mode == 3 then
    legend = tr("colour: balance, green even, red one-sided")
    if worst_name then
      legend = legend .. string.format("  ·  worst %s %.1f dB",
        utf8_prefix_bytes(worst_name, 10), worst_gap)
    end
  elseif mode == 2 then
    legend = tr("colour: how well they heard US")
  else
    legend = tr("colour: how well WE heard them")
  end
  rv_legend:set(legend)
  rv_legend:color(C.sub)
end

-- ---- screens ----------------------------------------------------------------

local build_survey, build_review

-- ⚠️ ORDER IS LOAD-BEARING: a live map view must be closed BEFORE ui.clear().
-- Every other handle type carries a generation and self-invalidates when clear()
-- bumps it, so a stale call is a no-op. The map userdata does not: clear() deletes
-- the view's container as a child of the app body, but leaves the one-view counter
-- set and the pointer non-nil. Clearing first would therefore (a) leak the tile
-- pixels, (b) make the next wada.map.view() fail with "only one map view at a
-- time", and (c) leave a later :close() or __gc freeing memory that is already
-- gone. So the teardown lives here, and both screen builders call it first rather
-- than each remembering to.
local function drop_map()
  if map then pcall(function() map:close() end) end
  map = nil
end

local function enter_review()
  -- Bank the in-flight probe before pausing. Those replies were heard at a position we still hold,
  -- so recording them is better than discarding them, and harvest() clears the set on its way out -
  -- which is the stale-sample protection this needs: without it, replies from before the pause would
  -- later be stamped with a fix taken after it.
  -- Only a probe still inside its own window may be banked. Outside it the set holds replies heard
  -- somewhere else - the survey was stopped mid-probe, or the display slept through the harvest - and
  -- the fix we have now is not where they were heard, so they are dropped on the same terms on_tick
  -- drops them.
  if running and phase == "probe" and harvest_fresh(sys.millis()) then
    harvest()
  else
    if phase == "probe" then dropped_sweeps = dropped_sweeps + 1 end
    mesh.discover_clear()
  end
  flush()                       -- and land them, so the reader below sees this run complete
  was_running = running
  running = false               -- a survey probes on a cadence its user chose,
  phase = "idle"                -- and nobody chose to keep probing while reading
  view = "review"
  ld_reset()
  build_review()
end

local function leave_review()
  drop_map()
  view = "survey"
  running = was_running
  if running then phase, phase_at = "idle", 0 end
  mesh.discover_clear()         -- same reason on the way back in
  ld = nil
  build_survey()
end

build_review = function()
  drop_map()          -- must precede clear(); see drop_map
  clear()
  ui.scroll(true)     -- the button row sits below the fold on a 240-tall board
  hdr, gps_lbl, stat_lbl, rows = nil, nil, nil, {}
  local LH = ui.text_h(12)
  local y = 4
  ui.label(tr("Run review"), 4, y, 12, C.accent); y = y + LH + 3
  rv_stat = ui.label("", 4, y, 12, C.text); rv_stat:width(W - 10); y = y + LH * 2 + 3

  -- Reserve every fixed element first, then let the map have the remainder. H is the BODY height
  -- (display minus the app bar), so on a T-Deck this is 218 rather than 240.
  local BTN_H = 32
  local reserved = 4 + (LH + 3) + (LH * 2 + 3) + 4 + (LH * 2 + 2) + (LH * 2 + 3) + BTN_H + 4
  local plot_h = math.max(40, H - reserved)
  plot_px = plot_h
  rv_plot = nil
  if sys.caps().map then
    -- A view is one per app and a second live one errors, so this is wrapped:
    -- a board or firmware that refuses it falls through to the bare plot rather
    -- than taking the app down.
    local ok, m = pcall(function() return wada.map.view(4, y, W - 8, plot_h) end)
    if ok and m then map = m end
  end
  if not map then
    -- ui.canvas argchecks both dimensions at 1..480 where map.view allows 800, so W - 8 is out of
    -- range on an 800 px board and would raise. This is the recovery path, so a refusal has to leave
    -- the screen standing rather than close the app.
    local okc, cv = pcall(ui.canvas, math.min(W - 8, 480), math.min(plot_h, 480))
    if okc and cv then rv_plot = cv; rv_plot:pos(4, y) end
  end
  y = y + plot_h + 4

  map_note = ui.label("", 4, y, 12, C.sub); map_note:width(W - 10); y = y + LH * 2 + 2
  rv_legend = ui.label("", 4, y, 12, C.sub); rv_legend:width(W - 10); y = y + LH * 2 + 3

  local bw = math.min(96, (W - 20) // 3)
  ui.button(tr("Back"), 4, y, bw, 32, function() leave_review() end)
  ui.button(tr("Colour"), 8 + bw, y, bw, 32, function()
    mode = mode % #MODES + 1
    sys.toast(MODES[mode], 1000)
    review_plot(); review_stat()
  end)
  -- Refit re-reads from the start rather than only redrawing: queued rows can land in the file
  -- while this screen is open, and this is the way to see them without leaving and coming back.
  ui.button(tr("Refit"), 12 + bw * 2, y, bw, 32, function()
    ld_reset()
    review_stat()
  end)

  review_stat()
end

build_survey = function()
  drop_map()          -- must precede clear(); see drop_map
  clear()
  rv_plot, rv_stat, rv_legend, map_note = nil, nil, nil, nil
  ui.scroll(true)
  local LH = ui.text_h(12)
  local y = 4

  hdr = ui.label(tr("LoRa coverage survey"), 4, y, 12, C.accent); hdr:width(W - 10); y = y + LH + 3
  gps_lbl = ui.label("", 4, y, 12, C.sub); gps_lbl:width(W - 10); y = y + LH + 3
  stat_lbl = ui.label("", 4, y, 12, C.text); stat_lbl:width(W - 10); y = y + LH * 2 + 5

  local bw = math.min(96, (W - 20) // 3)
  ui.button(tr("Start"), 4, y, bw, 32, function()
    running = not running
    if running then phase, phase_at = "idle", 0 end
    sys.toast(running and tr("Survey running") or tr("Survey stopped"), 1200)
  end)
  ui.button(tr("Name"), 8 + bw, y, bw, 32, function()
    ui.input(tr("Name this run"), run, function(text)
      if text then
        -- Land whatever is queued into the file it was surveyed for. Renaming with rows still in
        -- the queue used to write the old sweep into the new run's file.
        flush()
        pending = {}                  -- anything the rate limit refused belongs to the old run
        run = text:gsub("[^%w%-_]", "_")
        wrote_header = nil            -- unknown again: this file may or may not already have one
        sys.toast("Logging to " .. logname(), 1500)
      end
    end)
  end)
  ui.button(tr("Reset"), 12 + bw * 2, y, bw, 32, function()
    nodes, samples, sweeps, pending, node_count = {}, 0, 0, {}, 0
    dropped_sweeps = 0
    mesh.discover_clear()
    fs.remove(logname())
    wrote_header = nil            -- the file is gone, so the next flush re-checks and writes one
    sys.toast("Cleared " .. logname(), 1200)
  end)
  y = y + 38

  -- Its own row rather than a fourth column: the three-button row already lands
  -- at 53 px each on a 240 px board, and a fourth would make every label a
  -- guess. The page scrolls, so a row costs nothing a narrow screen cannot pay.
  ui.button(tr("Review run"), 4, y, math.min(bw * 2 + 4, W - 8), 32, function()
    enter_review()             -- harvests and flushes for itself
  end)
  y = y + 38

  ui.label(tr("strongest first"), 4, y, 12, C.sub); y = y + LH + 2
  for i = 1, 12 do
    rows[i] = ui.label("", 4, y, 12, C.text); rows[i]:width(W - 10); y = y + LH + 2
  end

  redraw()
end

function app.on_open(w, h)
  W, H = w, h
  -- Say what was actually tested. caps().discover and caps().sdk_ext are separate published flags,
  -- and the old wording asserted the one the "if" never looked at.
  if not sys.caps().discover then
    ui.label(tr("This board cannot send discovery probes,"), 6, 8, 12, C.bad)
    ui.label(tr("so it cannot run a coverage survey."), 6, 26, 12, C.bad)
    return
  end
  build_survey()
  timer.every(1000)
end

function app.on_tick()
  -- flush() runs in every view. Queued rows are only cleared once the write
  -- lands, so a view that stopped draining would grow `pending` without bound.
  if view == "review" then
    local more = ld_step()
    review_stat()
    if not more and ld and ld.done and not ld.plotted then
      ld.plotted = true
      review_plot()
    end
    flush()
    return
  end

  if running then
    local now = sys.millis()
    if phase == "idle" or (phase == "wait" and now - phase_at >= SWEEP_MS) then
      -- A refused or rate-limited probe backs off a full sweep interval. Retrying
      -- every tick would re-enter the permission path once a second for nothing.
      phase, phase_at = sweep() and "probe" or "wait", now
    elseif phase == "probe" and now - phase_at >= HARVEST_MS then
      -- Ticks pause while the display sleeps; millis() does not. Waking finds the deadline long
      -- past, the radio still holding replies heard at the old position, and sys.gps() reporting
      -- the new one - so recording them would place the old doorway's radio on this sidewalk.
      -- Anything far beyond the harvest window is discarded rather than mislocated.
      if harvest_fresh(now) then
        harvest()
      else
        mesh.discover_clear()
        dropped_sweeps = dropped_sweeps + 1
      end
      phase = "wait"          -- phase_at stays at the probe time, so sweeps stay on cadence
    end
  end
  flush()
  redraw()
end

function app.on_close()
  flush()                     -- one last drain; anything queued would otherwise be lost
  drop_map()
end

return app
