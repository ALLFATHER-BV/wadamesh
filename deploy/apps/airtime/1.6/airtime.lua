-- Airtime (Lua): how busy the channel is. The live load as a big figure, tiles
-- for the average, the peak and this node's own time on the air, a filled
-- chart of the load with its average and the duty-cycle ceiling, and the
-- receive / transmit split as a bar. While it runs it also keeps its Home
-- widget current (wada.widget, extended SDK boards).
--
-- Looks best on firmware with wada.ui.panel (beta_95 and later); on older
-- firmware the same screen draws without the framed panels and the split bar.
local ui, sys, mesh, timer = wada.ui, wada.sys, wada.mesh, wada.timer
local widget = wada.widget            -- nil on firmware or boards without it
-- Translate through the device's language table (wada.sys.tr, added for #257).
-- Older firmware has no sys.tr, so fall back to the English string rather than error.
local tr = sys.tr or function(x) return x end
local C = ui.colors
local WARN  = C.warn or 0xE8A33D
local PANEL = C.panel or 0x15181B
local AVG   = 0x4F9DF7
local app = {}

local function mix(a, b, t)            -- colour a over b by t (0..1)
  local function ch(s)
    local x, y = (a >> s) & 0xFF, (b >> s) & 0xFF
    return math.floor(y + (x - y) * t + 0.5) << s
  end
  return ch(16) | ch(8) | ch(0)
end
local AREA = { mix(C.accent, PANEL, 0.50), mix(C.accent, PANEL, 0.30), mix(C.accent, PANEL, 0.14) }
local GRID = mix(C.sub, PANEL, 0.30)

local W, H, narrow, LH12, LH16
local big, sub, tiles, cv, CW, CH, NPTS = nil, nil, {}, nil, 0, 0, 2
local grid_lbl = {}
local split_rx, split_tx, split_w, split_lbl, foot
local split_x0, split_y = 0, 0
local base, t0, last
local peak_pct = 0
local live_h, avg_h = {}, {}           -- chart history, percent
local top = 10                         -- chart ceiling, percent
local spark, wlast = {}, 0             -- the Home widget's curve (one point per 5 s)

local function panel(x, y, w, h, col)
  if ui.panel then return ui.panel(x, y, w, h, col or PANEL, 6) end
end

local function load_col(p)
  return p > 50 and C.bad or p > 20 and WARN or C.good
end

local function nice_top(v)             -- the chart ceiling: 10, 20, 30, 50 or 100 percent
  for _, t in ipairs({ 10, 20, 30, 50, 100 }) do if v <= t then return t end end
  return 100
end

local function ypix(p)
  if p < 0 then p = 0 elseif p > top then p = top end
  return (CH - 1) - math.floor(p * (CH - 1) / top)
end

local function draw_chart(duty)
  if not cv then return end
  cv:fill(PANEL)
  for g = 0, 2 do
    local gy = g * (CH - 1) // 2
    cv:line(0, gy, CW - 1, gy, GRID)
  end
  local n = #live_h
  local b1, b2 = CH // 3, 2 * CH // 3
  for i = 1, n do
    local x = (NPTS - n + i - 1) * (CW - 1) // (NPTS - 1)
    local x2 = (NPTS - n + i) * (CW - 1) // (NPTS - 1)
    if x2 <= x then x2 = x + 1 end
    local y = ypix(live_h[i])
    if y < b1 then cv:rect(x, y, x2 - x, b1 - y, AREA[1]); y = b1 end
    if y < b2 then cv:rect(x, y, x2 - x, b2 - y, AREA[2]); y = b2 end
    if y < CH then cv:rect(x, y, x2 - x, CH - y, AREA[3]) end
  end
  local px, py, qx, qy
  for i = 1, n do
    local x = (NPTS - n + i - 1) * (CW - 1) // (NPTS - 1)
    local y, ya = ypix(live_h[i]), ypix(avg_h[i])
    if px then
      cv:line(px, py, x, y, C.accent, 2)
      cv:line(qx, qy, x, ya, AVG, 2)
    end
    px, py, qx, qy = x, y, x, ya
  end
  -- the duty-cycle ceiling, dashed, when the region has one and it is on the scale
  if duty and duty > 0 and duty <= top then
    local dy = ypix(duty)
    for x = 0, CW - 1, 8 do cv:line(x, dy, math.min(x + 4, CW - 1), dy, WARN) end
  end
end

local function set_split(rx, tx)
  local tot = rx + tx
  local rp = tot > 0 and math.floor(rx * 100 / tot + 0.5) or 0
  local txt = tot > 0 and string.format(tr("rx %d%%  tx %d%%"), rp, 100 - rp) or tr("no airtime yet")
  if not split_rx then
    if split_lbl then split_lbl:set(txt) end
    return
  end
  local wr = tot > 0 and math.floor(split_w * rx / tot + 0.5) or 0
  if tot > 0 and tx > 0 and wr >= split_w then wr = split_w - 1 end
  split_rx:size(math.max(wr, 1), 8)
  split_tx:pos(split_x0 + wr, split_y)
  split_tx:size(math.max(split_w - wr, 1), 8)
  split_tx:color(tot > 0 and tx > 0 and AVG or mix(C.sub, PANEL, 0.25))
  if tot == 0 then split_rx:color(mix(C.sub, PANEL, 0.25)) else split_rx:color(C.accent) end
  split_lbl:set(txt)
end

local function restart_window()
  local st = mesh.stats()
  base = { rx = st.rx_air_s, tx = st.tx_air_s }
  t0 = sys.millis()
  last = { ms = t0, rx = st.rx_air_s, tx = st.tx_air_s }
  peak_pct = 0
  live_h, avg_h = {}, {}
  top = 10
  big:set("--"); big:color(C.accent)
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

  live_h[#live_h + 1] = live; if #live_h > NPTS then table.remove(live_h, 1) end
  avg_h[#avg_h + 1] = avg;    if #avg_h > NPTS then table.remove(avg_h, 1) end
  local hi = st.duty_pct or 0
  for i = 1, #live_h do if live_h[i] > hi then hi = live_h[i] end end
  local t = nice_top(hi * 1.15)
  if t ~= top then
    top = t
    local v = { top, top // 2, 0 }
    for g = 1, 3 do if grid_lbl[g] then grid_lbl[g]:set(v[g] .. "%") end end
  end
  draw_chart(st.duty_pct)

  big:set(string.format("%.1f%%", live)); big:color(load_col(live))
  local m = math.floor(wall)
  sub:set(string.format(tr("over %d:%02d"), m // 60, m % 60))
  tiles[1]:set(string.format("%.1f%%", avg))
  tiles[2]:set(string.format("%.1f%%", peak_pct)); tiles[2]:color(load_col(peak_pct))
  tiles[3]:set(string.format("%d s", tx_air))
  tiles[4]:set(string.format("%d s", rx_air))
  set_split(rx_air, tx_air)
  foot:set(string.format(narrow and tr("budget %d s  tx %d  rx %d") or tr("budget %d s   tx %d   rx %d   err %d   duty %d%%   SF%d"),
    st.tx_budget_ms // 1000, st.tx_pkts, st.rx_pkts, st.rx_err, st.duty_pct, st.sf))

  if widget and now - wlast >= 5000 then
    wlast = now
    spark[#spark + 1] = math.floor(live * 10)
    if #spark > 24 then table.remove(spark, 1) end
    widget.set{ title = tr("Channel busy"), value = string.format("%.1f%%", live),
                line = string.format(tr("avg %.1f%%, peak %.1f%%"), avg, peak_pct), spark = spark }
  end
end

function app.on_open(w, h)
  W, H, narrow = w, h, w < 280
  LH12, LH16 = ui.text_h(12), ui.text_h(16)
  local LHB = ui.text_h(24)

  -- the live figure, what it means, and Reset
  local y = 4
  big = ui.label("--", 6, y, 24, C.accent, true)
  local bw = (ui.text_w and ui.text_w("100.0%", 24) or 70) + 10
  local l1 = ui.label(tr("channel busy now"), 6 + bw, y + 2, 12, C.sub); l1:width(W - bw - 90)
  sub = ui.label("", 6 + bw, y + 2 + LH12, 12, C.sub); sub:width(W - bw - 90)
  ui.button(tr("Reset"), W - 76, y + 1, 70, math.max(28, LHB - 2), function() restart_window() end)
  y = y + math.max(LHB, 2 * LH12 + 4, 30) + 6

  local gap = 4
  local labels = { tr("Average"), tr("Peak"), tr("TX"), tr("RX") }
  local tw = (W - 8 - gap * 3) // 4
  local th = LH12 + LH16 + 8
  for i, k in ipairs(labels) do
    local x = 4 + (i - 1) * (tw + gap)
    panel(x, y, tw, th)
    local kl = ui.label(k, x + 6, y + 3, 12, C.sub); kl:width(tw - 8)
    tiles[i] = ui.label("--", x + 6, y + 3 + LH12, 16, C.text, true)
  end
  y = y + th + 6

  -- the chart, with % labels in the gutter
  local AX = (ui.text_w and ui.text_w("100%", 12) or 30) + 8
  local foot_h = LH12 + 4
  local split_h = LH12 + 6
  CH = H - y - split_h - foot_h - 18
  if CH > 120 then CH = 120 end
  if CH < 40 then CH = 40 end
  panel(AX, y, W - AX - 4, CH + 6)
  CW = W - AX - 10
  cv = ui.canvas(CW, CH); cv:pos(AX + 3, y + 3)
  NPTS = math.max(CW // 2, 2)
  local v = { top, top // 2, 0 }
  for g = 0, 2 do
    local l = ui.label(v[g + 1] .. "%", 0, y + 3 + g * (CH - 1) // 2 - LH12 // 2, 12, C.sub)
    l:width(AX - 6, "right")
    grid_lbl[g + 1] = l
  end
  y = y + CH + 6 + 6

  -- the receive / transmit split
  ui.label(tr("Air split"), 6, y, 12, C.sub)
  local kw = (ui.text_w and ui.text_w(tr("Air split"), 12) or 60) + 10
  local lw = (ui.text_w and ui.text_w("rx 100%  tx 100%", 12) or 110) + 4
  split_x0, split_y = 6 + kw, y + (LH12 - 8) // 2
  split_w = W - split_x0 - lw - 12
  if ui.panel and split_w > 20 then
    ui.panel(split_x0, split_y, split_w, 8, mix(C.sub, PANEL, 0.25), 4)
    split_rx = ui.panel(split_x0, split_y, 1, 8, C.accent, 4)
    split_tx = ui.panel(split_x0 + split_w - 1, split_y, 1, 8, AVG, 4)
    split_lbl = ui.label("", W - lw - 6, y, 12, C.text); split_lbl:width(lw, "right")
  else
    split_lbl = ui.label("", 6 + kw, y, 12, C.text); split_lbl:width(W - kw - 12)
  end
  y = y + split_h
  foot = ui.label("", 6, y, 12, C.sub); foot:width(W - 12)

  local st = mesh.stats()
  base = { rx = st.rx_air_s, tx = st.tx_air_s }
  t0 = sys.millis()
  last = { ms = t0, rx = st.rx_air_s, tx = st.tx_air_s }
  timer.every(1000)
end

function app.on_tick(dt) refresh() end

return app
