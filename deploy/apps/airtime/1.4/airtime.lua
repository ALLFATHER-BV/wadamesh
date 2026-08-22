-- Airtime (Lua) — channel utilization over time: live + average bars with a %
-- axis, rx/tx air split, duty ceiling, tx budget and packet counters.
local ui, sys, mesh, timer = wada.ui, wada.sys, wada.mesh, wada.timer
-- Translate through the device's language table (wada.sys.tr, added for #257).
-- Older firmware has no sys.tr, so fall back to the English string rather than error.
local tr = sys.tr or function(x) return x end
local C = ui.colors
local app = {}

local W, narrow
local head, sub, legend, detail, chart
local base, t0, last
local peak_pct = 0

local function restart_window()
  local st = mesh.stats()
  base = { rx = st.rx_air_s, tx = st.tx_air_s }
  t0 = sys.millis()
  last = { ms = t0, rx = st.rx_air_s, tx = st.tx_air_s }
  peak_pct = 0
  chart:fill(1, 0)
  chart:fill(2, 0)
  head:set(tr("sampling..."))
  head:color(C.accent)
  sys.toast(tr("window restarted"), 900)
end

local function refresh()
  local st = mesh.stats()
  local now = sys.millis()
  local wall = (now - t0) / 1000
  if wall < 0.5 then return end

  local rx_air, tx_air = st.rx_air_s - base.rx, st.tx_air_s - base.tx
  local avg = math.min(100, (rx_air + tx_air) * 100 / wall)

  local dw = (now - last.ms) / 1000
  local live = 0
  if dw > 0 then
    live = math.min(100, ((st.rx_air_s - last.rx) + (st.tx_air_s - last.tx)) * 100 / dw)
  end
  last = { ms = now, rx = st.rx_air_s, tx = st.tx_air_s }
  if live > peak_pct then peak_pct = live end

  chart:push(1, math.floor(live))
  chart:push(2, math.floor(avg))

  head:set(string.format(tr("channel busy %.1f%%"), live))
  head:color(live > 50 and C.bad or live > 20 and 0xE8A33D or C.good)
  sub:set(string.format(tr("avg %.1f%%   peak %.1f%%   over %ds"), avg, peak_pct, math.floor(wall)))
  if narrow then
    detail:set(string.format("air rx %ds / tx %ds\nduty %d%%   budget %dms\ntx %d  rx %d  err %d",
      rx_air, tx_air, st.duty_pct, st.tx_budget_ms, st.tx_pkts, st.rx_pkts, st.rx_err))
  else
    detail:set(string.format("air rx %ds / tx %ds   duty %d%%   budget %dms\ntx %d   rx %d   err %d   drop %d   SF%d",
      rx_air, tx_air, st.duty_pct, st.tx_budget_ms, st.tx_pkts, st.rx_pkts, st.rx_err, st.rx_dropped, st.sf))
  end
end

function app.on_open(w, h)
  W, narrow = w, w < 280
  local LH12, LH16 = ui.text_h(12), ui.text_h(16)

  local y = 2
  head = ui.label(tr("sampling..."), 4, y, 16, C.accent); head:width(w - 86)
  ui.button(tr("Reset"), w - 76, y, 70, 30, function() restart_window() end)   -- top-right
  y = y + LH16 + 4
  sub = ui.label("", 4, y, 12, C.sub); sub:width(w - 10)
  y = y + LH12 + 5

  local AX = 34
  local det_lines = narrow and 3 or 2
  local ch_h = h - y - (det_lines * LH12) - LH12 - 14
  if ch_h < 48 then ch_h = 48 end
  if ch_h > 110 then ch_h = 110 end
  chart = ui.chart(w - AX - 8, ch_h, 60, C.accent, 0x4F9DF7, true)
  chart:pos(AX, y)
  chart:range(0, 100)
  chart:axis(3, AX)
  y = y + ch_h + 4

  legend = ui.label(tr("teal = now   blue = average"), 4, y, 12, C.sub); legend:width(w - 10)
  y = y + LH12 + 2
  detail = ui.label("", 4, y, 12, C.text); detail:width(w - 10)

  local st = mesh.stats()
  base = { rx = st.rx_air_s, tx = st.tx_air_s }
  t0 = sys.millis()
  last = { ms = t0, rx = st.rx_air_s, tx = st.tx_air_s }
  timer.every(1000)
end

function app.on_tick(dt) refresh() end

return app
