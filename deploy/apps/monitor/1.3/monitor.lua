-- RF Monitor (Lua) — live RSSI/noise chart with a dBm axis, link-margin grade,
-- RX rate, radio parameters and the recently-heard feed. The page scrolls, so
-- the whole feed is reachable on short screens.
local ui, sys, mesh, timer = wada.ui, wada.sys, wada.mesh, wada.timer
local C = ui.colors
local app = {}

local RSSI_MIN, RSSI_MAX = -130, -20
local TYPE = { [1]="ADV", [2]="TXT", [3]="ACK", [4]="GRP", [5]="REQ", [6]="RSP", [7]="PTH", [8]="TRC" }
local TCOL = { [1]=0xA784E0, [2]=0x4F9DF7, [3]=0x53C06B, [4]=0x35C9C9, [5]=0xE8A33D, [6]=0xF2793C }

local W, narrow
local legend, chart, metrics, radio_lbl, feed_hdr, feed_rows = nil, nil, nil, nil, nil, {}
local peak, rx0, t0

local function fmt_ago(ms)
  local s = ms // 1000
  if s < 60 then return s .. "s" end
  if s < 3600 then return (s // 60) .. "m" end
  return (s // 3600) .. "h"
end

local function grade(m)
  if m >= 20 then return "excellent", C.good end
  if m >= 10 then return "good",      C.good end
  if m >= 5  then return "fair",      0xE8A33D end
  if m > 0   then return "weak",      C.bad end
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

  legend:set(string.format("RSSI %.0f   noise %.0f   peak %.0f dBm", st.rssi, st.noise, peak))
  metrics:set(string.format("margin %.0f dB (%s)   RX ~%.0f/min", mg, gtxt, (st.rx_pkts - rx0) / mins))
  metrics:color(gcol)
  radio_lbl:set(string.format("%.3f MHz  SF%d  BW %.0f   %d rx / %d err / %d drop   duty %d%%",
                              st.freq, st.sf, st.bw, st.rx_pkts, st.rx_err, st.rx_dropped, st.duty_pct))

  local log = mesh.rx_log()
  for i = 1, #feed_rows do
    local r = log[i]
    if r then
      local nm = TYPE[r.type] or ("t" .. r.type)
      if narrow then
        feed_rows[i]:set(string.format("%-4s %-3s %4d %5.1f %s", fmt_ago(r.ago_ms), nm, r.rssi, r.snr,
                                       r.hops == 0 and "dir" or (r.hops .. "h")))
      else
        feed_rows[i]:set(string.format("%-5s %-3s  %4d dBm  %5.1f dB  %s", fmt_ago(r.ago_ms), nm, r.rssi, r.snr,
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
  W, narrow = w, w < 280
  ui.scroll(true)                      -- the feed runs past the screen; let it scroll
  local LH12 = ui.text_h(12)           -- real line heights, no guessing
  local st = mesh.stats()
  rx0, t0, peak = st.rx_pkts, sys.millis(), st.rssi

  local y = 4
  legend = ui.label("", 4, y, 12, C.accent); legend:width(w - 10); y = y + LH12 + 4

  local AX, ch_h = 38, narrow and 62 or 76
  chart = ui.chart(w - AX - 8, ch_h, 72, C.accent, C.sub)
  chart:pos(AX, y)
  chart:range(RSSI_MIN, RSSI_MAX)
  chart:axis(3, AX)
  y = y + ch_h + 6

  metrics = ui.label("", 4, y, 12, C.text); metrics:width(w - 10)
  y = y + LH12 * (narrow and 2 or 1) + 3
  radio_lbl = ui.label("", 4, y, 12, C.sub); radio_lbl:width(w - 10)
  y = y + LH12 * (narrow and 3 or 2) + 5

  feed_hdr = ui.label(narrow and "recently heard" or "recently heard (newest first)", 4, y, 12, C.sub)
  feed_hdr:width(w - 10)
  y = y + LH12 + 3
  for i = 1, 16 do                     -- scrolling means we can show the whole ring
    feed_rows[i] = ui.label("", 4, y, 12, C.text)
    feed_rows[i]:width(w - 10)
    y = y + LH12 + 2
  end

  refresh()
  timer.every(1000)
end

function app.on_tick(dt) refresh() end

return app
