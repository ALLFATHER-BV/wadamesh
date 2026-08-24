-- RF Monitor (Lua) — full-parity port of the built-in page.
-- Live RSSI + noise-floor chart, peak tracking, link-margin grade, RX rate,
-- and the recently-heard feed. Adapts to narrow (240px) and wide screens.
local ui, sys, mesh, timer = wada.ui, wada.sys, wada.mesh, wada.timer
local C = ui.colors
local app = {}

local RSSI_MIN, RSSI_MAX = -130, -20
local TYPE = { [1]="ADV", [2]="TXT", [3]="ACK", [4]="GRP", [5]="REQ", [6]="RSP", [7]="PTH", [8]="TRC" }
local TCOL = { [1]=0xA784E0, [2]=0x4F9DF7, [3]=0x53C06B, [4]=0x35C9C9, [5]=0xE8A33D, [6]=0xF2793C }

local W, H, narrow
local chart, legend, metrics, feed_hdr, feed_rows = nil, nil, nil, nil, {}
local peak, rx0, t0 = -200, nil, nil

local function fmt_ago(ms)
  local s = ms // 1000
  if s < 60 then return s .. "s" end
  if s < 3600 then return (s // 60) .. "m" end
  return (s // 3600) .. "h"
end

-- Link-margin grade: how far the current signal sits above the noise floor.
local function grade(margin)
  if margin >= 20 then return "excellent", C.good end
  if margin >= 10 then return "good",      C.good end
  if margin >= 5  then return "fair",      0xE8A33D end
  if margin > 0   then return "weak",      C.bad end
  return "no margin", C.bad
end

local function refresh()
  local st = mesh.stats()
  if st.rssi > peak then peak = st.rssi end
  chart:push(1, math.floor(st.rssi))
  chart:push(2, math.floor(st.noise))

  local mg = st.rssi - st.noise
  local gtxt, gcol = grade(mg)
  local mins = math.max((sys.millis() - t0) / 60000, 1 / 60)
  local rate = (st.rx_pkts - rx0) / mins

  legend:set(string.format("RSSI %.0f    noise %.0f    peak %.0f dBm", st.rssi, st.noise, peak))
  if narrow then
    metrics:set(string.format("margin %.0f dB (%s)\nRX ~%.0f/min   %d rx  %d err\n%.3f MHz  SF%d  BW%.0f",
      mg, gtxt, rate, st.rx_pkts, st.rx_err, st.freq, st.sf, st.bw))
  else
    metrics:set(string.format("margin %.0f dB (%s)    RX ~%.0f/min    %d rx / %d err / %d dropped\n%.3f MHz   SF%d   BW %.0f kHz   duty ceiling %d%%   tx budget %dms",
      mg, gtxt, rate, st.rx_pkts, st.rx_err, st.rx_dropped, st.freq, st.sf, st.bw, st.duty_pct, st.tx_budget_ms))
  end
  metrics:color(gcol)

  local log = mesh.rx_log()
  for i = 1, #feed_rows do
    local r = log[i]
    if r then
      local nm = TYPE[r.type] or ("t" .. r.type)
      if narrow then
        feed_rows[i]:set(string.format("%-4s %-3s %4d %4.1f %s",
          fmt_ago(r.ago_ms), nm, r.rssi, r.snr, r.hops == 0 and "dir" or (r.hops .. "h")))
      else
        feed_rows[i]:set(string.format("%-5s %-3s  %4d dBm  %5.1f dB  %s",
          fmt_ago(r.ago_ms), nm, r.rssi, r.snr,
          r.hops == 0 and "direct" or (r.hops .. (r.hops == 1 and " hop" or " hops"))))
      end
      feed_rows[i]:color(TCOL[r.type] or C.text)
    else
      feed_rows[i]:set(i == 1 and #log == 0 and "listening - nothing heard yet" or "")
      feed_rows[i]:color(C.sub)
    end
  end
end

function app.on_open(w, h)
  W, H, narrow = w, h, w < 280
  local st = mesh.stats()
  rx0, t0, peak = st.rx_pkts, sys.millis(), st.rssi

  legend = ui.label("", 4, 2, 12, C.accent)
  local ch_h = narrow and 54 or 66
  chart = ui.chart(w - 8, ch_h, 60, C.accent, C.sub)   -- series 1 = RSSI, 2 = noise
  chart:pos(4, 18)
  chart:range(RSSI_MIN, RSSI_MAX)
  chart:fill(1, math.floor(st.rssi))
  chart:fill(2, math.floor(st.noise))

  local my = 18 + ch_h + 4
  metrics = ui.label("", 4, my, 12, C.text)

  local fy = my + (narrow and 46 or 34)
  feed_hdr = ui.label(narrow and "recently heard" or "recently heard (newest first)", 4, fy, 12, C.sub)
  local rows = math.floor((h - fy - 20) / 15)
  if rows > 14 then rows = 14 end
  for i = 1, rows do feed_rows[i] = ui.label("", 4, fy + 16 + (i - 1) * 15, 12, C.text) end

  refresh()
  timer.every(1000)
end

function app.on_tick(dt) refresh() end
function app.on_input(ev)
  if ev.type == "down" then peak = mesh.stats().rssi end   -- tap resets the peak
end

return app
