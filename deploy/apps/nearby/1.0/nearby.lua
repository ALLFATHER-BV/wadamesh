-- Nearby (Lua) — contacts on a real map, with distance and bearing, and a live
-- packet counter.
--
-- Reference app for the four things the SDK gained alongside it: wada.map (the
-- firmware's own tiles inside an app page), wada.ui.list (scrollable selectable
-- rows), app.on_packet (each frame delivered once instead of polled), and
-- wada.geo (great-circle maths in C rather than in your instruction budget).
local ui, sys, mesh, geo, timer = wada.ui, wada.sys, wada.mesh, wada.geo, wada.timer
local C = ui.colors
local app = {}

local map, list, hdr, detail = nil, nil, nil, nil
local pts, sel = {}, 0
local pkt_count, pkt_adverts = 0, 0
local W, H

local function here()
  local fix = sys.gps()
  if fix then return fix.lat, fix.lon, true end
  local me = mesh.self()
  -- self() is the last known position, which may be old. Say which one is in
  -- use rather than presenting a stale fix as a live one.
  if me.lat ~= 0 or me.lon ~= 0 then return me.lat, me.lon, false end
  return nil
end

local function fmt_dist(m)
  if m < 1000 then return string.format("%.0f m", m) end
  return string.format("%.1f km", m / 1000)
end

local function rebuild()
  local lat, lon, live = here()
  pts, sel = {}, 0
  for _, c in ipairs(mesh.contacts()) do
    if c.lat ~= 0 or c.lon ~= 0 then
      local e = { name = c.name, lat = c.lat, lon = c.lon, type = c.type, pubkey = c.pubkey }
      if lat then
        e.dist = geo.distance(lat, lon, c.lat, c.lon)
        e.brg  = geo.bearing(lat, lon, c.lat, c.lon)
      end
      pts[#pts + 1] = e
    end
  end
  if lat then table.sort(pts, function(a, b) return (a.dist or 1e9) < (b.dist or 1e9) end) end

  list:clear()
  for i, e in ipairs(pts) do
    local label = e.dist
      and string.format("%-12s  %8s  %-2s", e.name:sub(1, 12), fmt_dist(e.dist), geo.cardinal(e.brg))
      or  e.name
    list:add(label, function() app.pick(i) end)
  end
  if #pts == 0 then list:add("no contacts have a position yet") end

  hdr:set(string.format("%d placed  |  %s  |  %d frames (%d adverts)",
    #pts, lat and (live and "GPS fix" or "last known position") or "no position", pkt_count, pkt_adverts))
  hdr:color(lat and C.accent or C.bad)

  if lat then app.draw(lat, lon) end
end

function app.draw(lat, lon)
  if not map then return end
  map:center(lat, lon)
  map:clear()
  if map:tiles() == 0 then
    -- An empty rectangle looks like open water. Say what it actually is.
    detail:set("no map tiles cached here at zoom " .. map:zoom())
    detail:color(C.sub)
  end
  for i, e in ipairs(pts) do
    map:line(lat, lon, e.lat, e.lon, i == sel and C.good or 0x2A3340, i == sel and 2 or 1)
  end
  for i, e in ipairs(pts) do
    map:marker(e.lat, e.lon, i == sel and C.good or C.accent, i == sel and 12 or 8)
  end
  map:marker(lat, lon, 0xE8A33D, 10)      -- us, last so it sits on top
end

function app.pick(i)
  sel = i
  list:select(i)
  local e = pts[i]
  if not e then return end
  local lat, lon = here()
  if e.dist then
    detail:set(string.format("%s  %s  %.0f deg (%s)  %s",
      e.name, fmt_dist(e.dist), e.brg, geo.cardinal(e.brg), e.pubkey))
  else
    detail:set(e.name .. "  " .. e.pubkey)
  end
  detail:color(C.text)
  if lat then app.draw(lat, lon) end
end

function app.on_open(w, h)
  W, H = w, h
  local LH = ui.text_h(12)
  hdr = ui.label("", 4, 4, 12, C.accent); hdr:width(w - 10)

  -- hdr and detail below are width-limited, which turns wrapping on, and both
  -- carry variable-length text (node names, a status phrase). Reserve two lines
  -- for each: a wrapped line would otherwise be drawn over by whatever follows.
  local map_h = math.floor(h * 0.45)
  local y = 4 + LH * 2 + 4
  if sys.caps().map then
    map = wada.map.view(4, y, w - 8, map_h)
  else
    ui.label("this board has no map support", 6, y + 10, 12, C.sub)
  end
  y = y + map_h + 4

  detail = ui.label("pick a contact", 4, y, 12, C.sub); detail:width(w - 10)
  y = y + LH * 2 + 4

  list = ui.list(4, y, w - 8, h - y - 4)

  rebuild()
  -- Two cadences, which is what wada.timer.after/every made possible: a slow
  -- rebuild (contacts and GPS move slowly) and no fast tick at all, because
  -- packets now arrive by callback instead of being polled for.
  timer.every(5000, rebuild)
end

-- One call per frame the radio received, in arrival order. Counting was simply
-- not possible against a polled 16-entry ring.
function app.on_packet(p)
  pkt_count = pkt_count + 1
  if p.pubkey then pkt_adverts = pkt_adverts + 1 end
end

function app.on_close()
  if map then map:close() end
end

return app
