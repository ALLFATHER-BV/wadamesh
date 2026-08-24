-- Airtime (Lua) — full-parity port: channel utilization over time with the
-- RX/TX split, duty-cycle ceiling, live budget and packet counters.
local ui, sys, mesh, timer = wada.ui, wada.sys, wada.mesh, wada.timer
local C = ui.colors
local app = {}

local W, H, narrow
local head, sub, detail, chart, legend
local base, t0, last = nil, nil, nil
local peak_pct, samples = 0, 0

local function refresh()
  local st = mesh.stats()
  local now = sys.millis()
  local wall = (now - t0) / 1000
  if wall < 0.5 then return end

  -- utilization since open (stable) and since the last tick (live)
  local rx_air = st.rx_air_s - base.rx
  local tx_air = st.tx_air_s - base.tx
  local avg = math.min(100, (rx_air + tx_air) * 100 / wall)

  local dw = (now - last.ms) / 1000
  local live = 0
  if dw > 0 then
    live = math.min(100, ((st.rx_air_s - last.rx) + (st.tx_air_s - last.tx)) * 100 / dw)
  end
  last = { ms = now, rx = st.rx_air_s, tx = st.tx_air_s }
  if live > peak_pct then peak_pct = live end
  samples = samples + 1

  chart:push(1, math.floor(live))
  chart:push(2, math.floor(avg))

  local col = live > 50 and C.bad or live > 20 and 0xE8A33D or C.good
  head:set(string.format("channel busy %.1f%%", live))
  head:color(col)
  sub:set(string.format("avg %.1f%%   peak %.1f%%   over %ds", avg, peak_pct, math.floor(wall)))
  if narrow then
    detail:set(string.format("air rx %ds / tx %ds\nduty %d%%   budget %dms\ntx %d  rx %d  err %d",
      rx_air, tx_air, st.duty_pct, st.tx_budget_ms, st.tx_pkts, st.rx_pkts, st.rx_err))
  else
    detail:set(string.format("air rx %ds / tx %ds   duty %d%%   budget %dms\ntx %d   rx %d   err %d   drop %d   SF%d",
      rx_air, tx_air, st.duty_pct, st.tx_budget_ms, st.tx_pkts, st.rx_pkts, st.rx_err, st.rx_dropped, st.sf))
  end
end

function app.on_open(w, h)
  W, H, narrow = w, h, w < 280
  local st = mesh.stats()
  base = { rx = st.rx_air_s, tx = st.tx_air_s }
  t0 = sys.millis()
  last = { ms = t0, rx = st.rx_air_s, tx = st.tx_air_s }

  local LH = 15                                   -- 12 px line box
  head = ui.label("sampling...", 4, 2, 16, C.accent); head:width(w - 8)
  sub  = ui.label("", 4, 24, 12, C.sub);              sub:width(w - 8)

  -- Reserve the detail block (3 lines narrow, 2 wide) + legend, then give the
  -- chart what is genuinely left — that is what stopped the text falling off.
  local AX = 34
  local ch_y = 42
  local det_lines = narrow and 3 or 2
  local det_h = det_lines * LH + 4
  local ch_h = h - ch_y - det_h - LH - 8
  if ch_h < 48 then ch_h = 48 end
  if ch_h > 110 then ch_h = 110 end
  chart = ui.chart(w - AX - 8, ch_h, 60, C.accent, 0x4F9DF7, true)
  chart:pos(AX, ch_y)
  chart:range(0, 100)
  chart:axis(3, AX)                                -- % scale down the left
  chart:fill(1, 0)
  chart:fill(2, 0)
  legend = ui.label("teal = now   blue = average", 4, ch_y + ch_h + 3, 12, C.sub)
  legend:width(w - 8)
  detail = ui.label("", 4, ch_y + ch_h + 3 + LH, 12, C.text)
  detail:width(w - 8)
  timer.every(1000)
end

function app.on_tick(dt) refresh() end
function app.on_input(ev)
  if ev.type == "down" then   -- tap = restart the window
    local st = mesh.stats()
    base = { rx = st.rx_air_s, tx = st.tx_air_s }
    t0 = sys.millis()
    last = { ms = t0, rx = st.rx_air_s, tx = st.tx_air_s }
    peak_pct = 0
    chart:fill(1, 0); chart:fill(2, 0)
    sys.toast("window restarted", 900)
  end
end

return app
