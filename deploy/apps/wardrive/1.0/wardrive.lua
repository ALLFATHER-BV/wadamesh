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
local ui, sys, mesh, fs, timer = wada.ui, wada.sys, wada.mesh, wada.fs, wada.timer
local C = ui.colors
local app = {}

local SWEEP_MS = 20000        -- above the 15 s floor the firmware enforces on probes
local HARVEST_MS = 4000       -- replies land over the few seconds after a probe
local TYPE = { [1]="chat", [2]="repeater", [3]="room", [4]="sensor" }

local run, running, samples, sweeps, last_err = "run", false, 0, 0, nil
local node_count = 0
local phase, phase_at = "idle", 0
local hdr, gps_lbl, stat_lbl, rows = nil, nil, nil, {}
local nodes = {}              -- pubkey -> { name, type, best, worst, seen }
local pending = {}            -- lines waiting on the 1 write/sec limit

local function logname() return run .. ".csv" end

-- Lua here is built with 32-bit floats, so fix.lat is good to about a metre and
-- no better. fix.lat_e6 is the same reading as an exact integer in
-- micro-degrees, which is what belongs in a log: a survey you plot months later
-- should not carry rounding the device never had.
local HEADER = "epoch,lat_e6,lon_e6,alt_m,pubkey,name,type,rssi,snr,their_snr,hops"
local wrote_header = false

-- The filesystem allows one write a second. Sweeps produce a burst of rows, so
-- they queue here and drain a chunk per tick instead of being dropped.
local function flush()
  if #pending == 0 then return end
  local chunk = table.concat(pending, "\n") .. "\n"
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
    fix.time or sys.epoch(), fix.lat_e6, fix.lon_e6, fix.alt_m or 0,
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
    gps_lbl:set("waiting for a GPS fix - samples are discarded until then")
    gps_lbl:color(C.bad)
  end

  local state = running and (phase == "probe" and "listening..." or "sweeping") or "stopped"
  stat_lbl:set(string.format("%s  |  %s  |  %d sweeps, %d samples, %d nodes%s",
    run, state, sweeps, samples, node_count,
    last_err and ("  [" .. last_err .. "]") or ""))
  stat_lbl:color(last_err and C.bad or C.accent)

  local list = {}
  for key, n in pairs(nodes) do list[#list + 1] = { key = key, n = n } end
  table.sort(list, function(a, b) return a.n.best > b.n.best end)
  for i = 1, #rows do
    local e = list[i]
    if e then
      rows[i]:set(string.format("%-14s %-8s best %5.1f  worst %5.1f  x%d",
        e.n.name:sub(1, 14), TYPE[e.n.type] or "?", e.n.best, e.n.worst, e.n.seen))
      rows[i]:color(e.n.best > 0 and C.good or C.text)
    else
      rows[i]:set(i == 1 and "nothing has answered a probe yet" or "")
      rows[i]:color(C.sub)
    end
  end
end

function app.on_open(w, h)
  if not sys.caps().discover then
    ui.label("This board does not carry the extended SDK,", 6, 8, 12, C.bad)
    ui.label("so it cannot send discovery probes.", 6, 26, 12, C.bad)
    return
  end
  ui.scroll(true)
  local LH = ui.text_h(12)
  local y = 4

  hdr = ui.label("LoRa coverage survey", 4, y, 12, C.accent); hdr:width(w - 10); y = y + LH + 3
  gps_lbl = ui.label("", 4, y, 12, C.sub); gps_lbl:width(w - 10); y = y + LH + 3
  stat_lbl = ui.label("", 4, y, 12, C.text); stat_lbl:width(w - 10); y = y + LH * 2 + 5

  local bw = math.min(96, (w - 20) // 3)
  ui.button("Start", 4, y, bw, 32, function()
    running = not running
    if running then phase, phase_at = "idle", 0 end
    sys.toast(running and "Survey running" or "Survey stopped", 1200)
  end)
  ui.button("Name", 8 + bw, y, bw, 32, function()
    ui.input("Name this run", run, function(text)
      if text then
        run = text:gsub("[^%w%-_]", "_")
        wrote_header = false          -- a new file needs its own header row
        sys.toast("Logging to " .. logname(), 1500)
      end
    end)
  end)
  ui.button("Reset", 12 + bw * 2, y, bw, 32, function()
    nodes, samples, sweeps, pending, node_count = {}, 0, 0, {}, 0
    mesh.discover_clear()
    fs.remove(logname())
    wrote_header = false
    sys.toast("Cleared " .. logname(), 1200)
  end)
  y = y + 38

  ui.label("strongest first", 4, y, 12, C.sub); y = y + LH + 2
  for i = 1, 12 do
    rows[i] = ui.label("", 4, y, 12, C.text); rows[i]:width(w - 10); y = y + LH + 2
  end

  redraw()
  timer.every(1000)
end

function app.on_tick()
  if running then
    local now = sys.millis()
    if phase == "idle" or (phase == "wait" and now - phase_at >= SWEEP_MS) then
      -- A refused or rate-limited probe backs off a full sweep interval. Retrying
      -- every tick would re-enter the permission path once a second for nothing.
      phase, phase_at = sweep() and "probe" or "wait", now
    elseif phase == "probe" and now - phase_at >= HARVEST_MS then
      harvest()
      phase = "wait"          -- phase_at stays at the probe time, so sweeps stay on cadence
    end
  end
  flush()
  redraw()
end

function app.on_close()
  flush()                     -- one last drain; anything queued would otherwise be lost
end

return app
