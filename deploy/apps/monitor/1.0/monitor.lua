-- RF Monitor (Lua) — recently heard packets + live radio stats.
-- The wada.mesh read API in ~90 lines: what the native Monitor shows, as a script.
local ui, sys, mesh, timer = wada.ui, wada.sys, wada.mesh, wada.timer
local C = ui.colors
local app = {}
local head, rows = nil, {}
local W, H

local TYPE_NAMES = { [1]="ADV", [2]="TXT", [3]="ACK", [4]="GRP", [5]="REQ", [6]="RSP" }

local function fmt_ago(ms)
  local s = math.floor(ms / 1000)
  if s < 60 then return s .. "s" end
  if s < 3600 then return math.floor(s / 60) .. "m" end
  return math.floor(s / 3600) .. "h"
end

local function refresh()
  local st = mesh.stats()
  head:set(string.format("RSSI %.0f  noise %.0f  rx %d  err %d",
                         st.rssi, st.noise, st.rx_pkts, st.rx_err))
  local log = mesh.rx_log()
  for i = 1, #rows do
    local r = log[i]
    if r then
      rows[i]:set(string.format("%-4s %-4s %4d dBm  %4.1f  %s",
        fmt_ago(r.ago_ms), TYPE_NAMES[r.type] or ("t" .. r.type), r.rssi, r.snr,
        r.hops == 0 and "direct" or (r.hops .. " hop")))
      rows[i]:color(r.hops == 0 and C.good or C.text)
    else
      rows[i]:set(i == 1 and #log == 0 and "listening - nothing heard yet" or "")
    end
  end
end

function app.on_open(w, h)
  W, H = w, h
  head = ui.label("", 4, 4, 14, C.accent)
  local n = math.floor((h - 30) / 18)
  if n > 16 then n = 16 end
  for i = 1, n do
    rows[i] = ui.label("", 4, 26 + (i - 1) * 18, 12, C.text)
  end
  refresh()
  timer.every(2000)
end

function app.on_tick(dt) refresh() end

return app
