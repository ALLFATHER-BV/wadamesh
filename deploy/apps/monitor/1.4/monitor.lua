-- RF Monitor (Lua): the channel at a glance. Stat tiles (RSSI, noise, link
-- margin, packets a minute), a filled signal chart with the noise floor dashed
-- under it, and the packets heard most recently with their type and signal.
-- Tap a packet (or select it with the keys) for its details: who sent it, the
-- route, the margin above what the spreading factor can still decode, its size
-- and how long it was on the air.
--
-- Looks best on firmware with wada.ui.panel (beta_95 and later); on older
-- firmware the same screens draw without the framed panels.
local ui, sys, mesh, timer = wada.ui, wada.sys, wada.mesh, wada.timer
local tr = sys.tr or function(x) return x end
local C = ui.colors
local WARN   = C.warn or 0xE8A33D
local PANEL  = C.panel or 0x15181B
local app = {}

local RSSI_MIN, RSSI_MAX = -130, -30
-- MeshCore payload types: the type field of the frame header.
local TYPE  = { [0]="REQ", [1]="RSP", [2]="TXT", [3]="ACK", [4]="ADV", [5]="GRP", [6]="DAT",
                [7]="ANO", [8]="PTH", [9]="TRC", [10]="MPT", [11]="CTL", [15]="RAW" }
local TNAME = { [0]=tr("Request"), [1]=tr("Response"), [2]=tr("Direct message"), [3]=tr("Acknowledgement"),
                [4]=tr("Advert"), [5]=tr("Channel message"), [6]=tr("Channel data"), [7]=tr("Anonymous request"),
                [8]=tr("Path"), [9]=tr("Trace"), [10]=tr("Multipart"), [11]=tr("Control"), [15]=tr("Custom") }
local TCOL  = { [0]=0xE8A33D, [1]=0xF2793C, [2]=0x4F9DF7, [3]=0x53C06B, [4]=0xA784E0,
                [5]=0x35C9C9, [6]=0x35C9C9, [7]=0xE8A33D, [8]=0x9AA3B2, [9]=0xD4537E }
local ROUTE = { [0]=tr("Flood with a region code"), [1]=tr("Flood"), [2]=tr("Direct"), [3]=tr("Direct with a region code") }
-- The SNR each spreading factor can still decode (Semtech SX126x datasheet).
local SF_LIMIT = { [5]=-2.5, [6]=-5, [7]=-7.5, [8]=-10, [9]=-12.5, [10]=-15, [11]=-17.5, [12]=-20 }

local function mix(a, b, t)            -- colour a over b by t (0..1)
  local function ch(s)
    local x, y = (a >> s) & 0xFF, (b >> s) & 0xFF
    return math.floor(y + (x - y) * t + 0.5) << s
  end
  return ch(16) | ch(8) | ch(0)
end
local AREA = { mix(C.accent, PANEL, 0.50), mix(C.accent, PANEL, 0.30), mix(C.accent, PANEL, 0.14) }
local GRID = mix(C.sub, PANEL, 0.30)
local SEL  = mix(C.accent, PANEL, 0.22)

local W, H, narrow, LH12, LH16
local mode = "main"
local tiles = {}
local cv, CW, CH = nil, 0, 0
local NPTS = 2
local info
local rows, row_y0, row_h, rows_n, hl = {}, 0, 0, 0, nil
local sel = 0
local shown = {}                       -- the rx_log rows as last drawn, for the detail card
local names = {}                       -- contact pubkey prefix (8 hex) -> name
local rs, nz = {}, {}                  -- chart history
local rx0, t0
local stats

local function panel(x, y, w, h, col)
  if ui.panel then return ui.panel(x, y, w, h, col or PANEL, 6) end
end

local function fmt_ago(ms)
  local s = ms // 1000
  if s < 60 then return s .. "s" end
  if s < 3600 then return (s // 60) .. "m" end
  return (s // 3600) .. "h"
end

local function bars_for(rssi, snr)      -- 0..4, mostly from SNR: it is what decodes
  if snr >= 5 then return 4 elseif snr >= 0 then return 3 elseif snr >= -6 then return 2 end
  if rssi > -120 then return 1 end
  return 0
end

local function grade(m)
  if m >= 20 then return C.good end
  if m >= 10 then return C.good end
  if m >= 5 then return WARN end
  return C.bad
end

-- LoRa time on air (Semtech AN1200.13): explicit header, CRC on, 16-symbol preamble.
local function airtime_ms(len, sf, bw_khz, cr)
  if not sf or not bw_khz or bw_khz <= 0 then return nil end
  local ts = (1 << sf) / bw_khz
  local de = ts > 16 and 1 or 0
  local n = math.ceil((8 * len - 4 * sf + 44) / (4 * (sf - 2 * de))) * (cr + 4)
  if n < 0 then n = 0 end
  return (16 + 4.25) * ts + (8 + n) * ts
end

local function sender(r)
  if r.pubkey then return names[r.pubkey] or r.pubkey, r.pubkey end
  if r.src then
    local hit, key
    for pk, nm in pairs(names) do
      if pk:sub(1, 2) == r.src then
        if hit then return r.src, r.src end   -- two contacts share the byte: say so plainly
        hit, key = nm, pk
      end
    end
    if hit then return hit, r.src end
    return r.src, r.src
  end
  return nil
end

-- ---- the chart -------------------------------------------------------------
local function ypix(v)
  v = math.floor(v)
  if v < RSSI_MIN then v = RSSI_MIN elseif v > RSSI_MAX then v = RSSI_MAX end
  return (CH - 1) - (v - RSSI_MIN) * (CH - 1) // (RSSI_MAX - RSSI_MIN)
end

local function draw_chart()
  if not cv then return end
  cv:fill(PANEL)
  for g = 0, 2 do
    local gy = g * (CH - 1) // 2
    cv:line(0, gy, CW - 1, gy, GRID)
  end
  local n = #rs
  local b1, b2 = CH // 3, 2 * CH // 3
  local px, py
  for i = 1, n do
    local x = (NPTS - n + i - 1) * (CW - 1) // (NPTS - 1)
    local x2 = (NPTS - n + i) * (CW - 1) // (NPTS - 1)
    if x2 <= x then x2 = x + 1 end
    local y = ypix(rs[i])
    -- the area under the line, in three tones that fade towards the bottom
    if y < b1 then cv:rect(x, y, x2 - x, b1 - y, AREA[1]); y = b1 end
    if y < b2 then cv:rect(x, y, x2 - x, b2 - y, AREA[2]); y = b2 end
    if y < CH then cv:rect(x, y, x2 - x, CH - y, AREA[3]) end
  end
  for i = 1, n do
    local x = (NPTS - n + i - 1) * (CW - 1) // (NPTS - 1)
    local y = ypix(rs[i])
    if px then cv:line(px, py, x, y, C.accent, 2) end
    px, py = x, y
  end
  px = nil
  for i = 1, #nz do
    local x = (NPTS - #nz + i - 1) * (CW - 1) // (NPTS - 1)
    local y = ypix(nz[i])
    if px and i % 2 == 0 then cv:line(px, py, x, y, C.sub, 1) end
    px, py = x, y
  end
end

-- ---- the feed --------------------------------------------------------------
local function paint_selection()
  if not hl then return end
  if sel > 0 and sel <= rows_n then hl:pos(6, row_y0 + (sel - 1) * row_h) else hl:pos(-2000, 0) end
end

local function paint_feed(log)
  shown = {}
  for i = 1, rows_n do
    local r, row = log[i], rows[i]
    shown[i] = r
    if r then
      if row.pill and row.hidden then         -- an empty row filling up: put its pill and bars back
        row.pill:pos(row.px, row.py)
        for b = 1, 4 do row.bar[b]:pos(row.bx[b], row.by[b]) end
        row.hidden = false
      end
      local col = TCOL[r.type] or C.sub
      row.tag:set(TYPE[r.type] or tostring(r.type)); row.tag:color(col)
      if row.pill then row.pill:color(mix(col, PANEL, 0.22)) end
      local nb = bars_for(r.rssi, r.snr)
      local bc = nb >= 3 and C.good or nb == 2 and WARN or C.bad
      for b = 1, 4 do
        if row.bar[b] then row.bar[b]:color(b <= nb and bc or mix(C.sub, PANEL, 0.25)) end
      end
      row.rssi:set(narrow and tostring(r.rssi) or (r.rssi .. " dBm"))
      row.snr:set(string.format(narrow and "%.1f" or "%.1f dB", r.snr))
      row.hops:set(r.hops == 0 and tr("direct") or (r.hops .. (narrow and "h" or (r.hops == 1 and " hop" or " hops"))))
      row.ago:set(fmt_ago(r.ago_ms))
    else
      row.tag:set(""); row.rssi:set(i == 1 and #log == 0 and tr("listening, nothing heard yet") or "")
      row.snr:set(""); row.hops:set(""); row.ago:set("")
      if row.pill and not row.hidden then    -- an empty row shows nothing at all
        row.pill:pos(-2000, 0)
        for b = 1, 4 do row.bar[b]:pos(-2000, 0) end
        row.hidden = true
      end
    end
  end
end

local function refresh()
  if mode ~= "main" then return end
  stats = mesh.stats()
  local st = stats
  rs[#rs + 1] = st.rssi; if #rs > NPTS then table.remove(rs, 1) end
  nz[#nz + 1] = st.noise; if #nz > NPTS then table.remove(nz, 1) end
  -- Margin: how far the newest packet's SNR sat above what this spreading factor
  -- can still decode. The idle channel's RSSI minus its noise is ~0 by definition.
  local log = mesh.rx_log()
  local newest = log[1]
  local limit = SF_LIMIT[st.sf]
  local mins = math.max((sys.millis() - t0) / 60000, 1 / 60)
  tiles[1]:set(string.format("%.0f", st.rssi))
  tiles[2]:set(string.format("%.0f", st.noise))
  if newest and limit then
    local mg = newest.snr - limit
    tiles[3]:set(string.format("%.0f dB", mg)); tiles[3]:color(grade(mg))
  else
    tiles[3]:set("--"); tiles[3]:color(C.sub)
  end
  tiles[4]:set(string.format("%.1f", (st.rx_pkts - rx0) / mins))
  info:set(string.format(narrow and "%.3f  SF%d  BW%.0f" or "%.3f MHz  SF%d  BW %.1f   rx %d  err %d  drop %d",
                         st.freq, st.sf, st.bw, st.rx_pkts, st.rx_err, st.rx_dropped))
  draw_chart()
  paint_feed(log)
end

local function build_main()
  mode = "main"
  if ui.clear then ui.clear() end
  local gap, y = 4, 4
  local labels = { tr("RSSI"), tr("Noise"), tr("Margin"), tr("RX/min") }
  local tw = (W - 8 - gap * 3) // 4
  local th = LH12 + LH16 + 8
  for i, k in ipairs(labels) do
    local x = 4 + (i - 1) * (tw + gap)
    panel(x, y, tw, th)
    local kl = ui.label(k, x + 6, y + 3, 12, C.sub); kl:width(tw - 8)
    tiles[i] = ui.label("--", x + 6, y + 3 + LH12, 16, C.text, true)
  end
  y = y + th + 6

  -- chart: dB labels in the gutter, the canvas inset in a framed panel
  local AX = (ui.text_w and ui.text_w("-130", 12) or 28) + 8
  -- the chart gives way to the feed on short screens (the T-Deck's 240 px)
  CH = H < 260 and 44 or (narrow and 58 or 70)
  panel(AX, y, W - AX - 4, CH + 6)
  CW = W - AX - 10
  cv = ui.canvas(CW, CH); cv:pos(AX + 3, y + 3)
  NPTS = math.max(CW // 2, 2)
  local gl = { RSSI_MAX, (RSSI_MAX + RSSI_MIN) // 2, RSSI_MIN }
  for g = 0, 2 do
    local l = ui.label(tostring(gl[g + 1]), 0, y + 3 + g * (CH - 1) // 2 - LH12 // 2, 12, C.sub)
    l:width(AX - 6, "right")
  end
  y = y + CH + 6 + 3
  info = ui.label("", 6, y, 12, C.sub); info:width(W - 12)
  y = y + LH12 + 6

  -- recently heard
  row_h = LH12 + 6
  local avail = H - y - 6
  rows_n = math.max(1, math.min(10, avail // row_h))
  panel(4, y, W - 8, rows_n * row_h + 4)
  row_y0 = y + 2
  hl = ui.panel and ui.panel(-2000, 0, W - 12, row_h, SEL, 4) or nil
  local pill_w = (ui.text_w and ui.text_w("ADV", 12) or 26) + 10
  local x_bar = 10 + pill_w + 6
  local x_rssi = x_bar + 18
  local w_rssi = (ui.text_w and ui.text_w(narrow and "-120" or "-120 dBm", 12) or 50) + 8
  local x_snr = x_rssi + w_rssi
  local w_snr = (ui.text_w and ui.text_w(narrow and "-12.5" or "-12.5 dB", 12) or 50) + 8
  local x_hops = x_snr + w_snr
  local w_ago = (ui.text_w and ui.text_w("59m", 12) or 24) + 2
  for i = 1, rows_n do
    local ry = row_y0 + (i - 1) * row_h
    local row = { bar = {}, bx = {}, by = {}, px = 10, py = ry + 2 }
    row.pill = panel(10, ry + 2, pill_w, row_h - 4, PANEL)
    row.tag = ui.label("", 10, ry + 3, 12, C.text); row.tag:width(pill_w, "center")
    for b = 1, 4 do
      local bh = 2 + b * (row_h - 8) // 4
      row.bx[b], row.by[b] = x_bar + (b - 1) * 4, ry + row_h - 3 - bh
      row.bar[b] = panel(row.bx[b], row.by[b], 3, bh, PANEL)
    end
    row.rssi = ui.label("", x_rssi, ry + 3, 12, C.text)
    row.snr = ui.label("", x_snr, ry + 3, 12, C.sub)
    row.hops = ui.label("", x_hops, ry + 3, 12, C.text)
    row.ago = ui.label("", W - 10 - w_ago, ry + 3, 12, C.sub); row.ago:width(w_ago, "right")
    rows[i] = row
  end
  paint_selection()
  refresh()
end

local function build_detail(r)
  mode = "detail"
  if ui.clear then ui.clear() end
  local st = stats or mesh.stats()
  local col = TCOL[r.type] or C.text
  local y = 4
  ui.label(TNAME[r.type] or tr("Packet"), 6, y, 16, col, true)
  local ago = ui.label(string.format(tr("%s ago"), fmt_ago(r.ago_ms)), W - 96, y + 2, 12, C.sub)
  ago:width(90, "right")
  y = y + LH16 + 6

  local nm, key = sender(r)
  local limit = SF_LIMIT[st.sf]
  local margin = limit and (r.snr - limit) or nil
  local air = airtime_ms(r.len, st.sf, st.bw, 1)
  local cells = {
    { tr("RSSI"),   r.rssi .. " dBm", C.text },
    { tr("SNR"),    string.format("%.1f dB", r.snr), C.text },
    { tr("Margin"), margin and string.format("%.1f dB", margin) or "--", margin and grade(margin) or C.sub },
    { tr("Hops"),   r.hops == 0 and tr("direct") or tostring(r.hops), C.text },
    { tr("Size"),   r.len .. " B", C.text },
    { tr("Airtime"), air and string.format("%d ms", math.floor(air + 0.5)) or "--", C.text },
  }
  local gap, cols = 4, 3
  local tw = (W - 8 - gap * (cols - 1)) // cols
  local th = LH12 + LH16 + 8
  for i, c in ipairs(cells) do
    local cx = 4 + ((i - 1) % cols) * (tw + gap)
    local cy = y + ((i - 1) // cols) * (th + gap)
    panel(cx, cy, tw, th)
    local k = ui.label(c[1], cx + 6, cy + 3, 12, C.sub); k:width(tw - 8)
    ui.label(c[2], cx + 6, cy + 3 + LH12, 16, c[3], true)
  end
  y = y + 2 * (th + gap) + 4

  local lines = {}
  if nm then lines[#lines + 1] = { tr("From"), nm ~= key and (nm .. "  (" .. key .. ")") or key } end
  lines[#lines + 1] = { tr("Route"), ROUTE[r.route] or "?" }
  if limit then
    lines[#lines + 1] = { tr("Decodes to"), string.format(tr("%.1f dB SNR at SF%d"), limit, st.sf) }
  end
  local kw = 0
  for _, l in ipairs(lines) do
    local w = ui.text_w and ui.text_w(l[1], 12) or 70
    if w > kw then kw = w end
  end
  for _, l in ipairs(lines) do
    ui.label(l[1], 8, y, 12, C.sub)
    local v = ui.label(l[2], 8 + kw + 10, y, 12, C.text); v:width(W - kw - 26)
    y = y + LH12 * (ui.text_lines and ui.text_lines(l[2], W - kw - 26, 12) or 1) + 3
  end
  local back = ui.label(tr("Tap to go back"), 6, H - LH12 - 4, 12, C.sub); back:width(W - 12, "center")
end

function app.on_open(w, h)
  W, H, narrow = w, h, w < 280
  LH12, LH16 = ui.text_h(12), ui.text_h(16)
  local st = mesh.stats()
  rx0, t0 = st.rx_pkts, sys.millis()
  local ok, list = pcall(mesh.contacts, 0, 250)
  if ok and type(list) == "table" then
    for _, c in ipairs(list) do if c.pubkey and c.name then names[c.pubkey] = c.name end end
  end
  build_main()
  timer.every(1000)
end

function app.on_tick(dt) refresh() end

local function open_row(i)
  if not ui.clear then return end      -- older firmware cannot swap screens
  local r = shown[i]
  if r then build_detail(r) end
end

-- Input: a tap ("up" after "down", no swipe in between) opens the packet under
-- the finger. Swipes, the trackball and the d-pad arrive as "swipe" and move a
-- highlight; Enter, or the select key (a synthetic tap at the page centre),
-- opens the highlighted packet. On the detail card anything goes back.
local swiped = false
function app.on_input(ev)
  if ev.type == "down" then swiped = false; return end
  if mode == "detail" then
    if (ev.type == "up" and not swiped) or ev.type == "swipe"
       or (ev.type == "key" and (ev.key == "enter" or ev.key == "backspace" or ev.key == "left")) then
      build_main()
    end
    if ev.type == "swipe" then swiped = true end
    return
  end
  if ev.type == "swipe" then
    swiped = true
    if ev.dir == "down" then sel = math.min(rows_n, sel + 1); paint_selection()
    elseif ev.dir == "up" then sel = math.max(0, sel - 1); paint_selection() end
  elseif ev.type == "key" then
    if ev.key == "down" then sel = math.min(rows_n, sel + 1); paint_selection()
    elseif ev.key == "up" then sel = math.max(0, sel - 1); paint_selection()
    elseif ev.key == "enter" and sel > 0 then open_row(sel) end
  elseif ev.type == "up" and not swiped then
    if sel > 0 and ev.x == W // 2 and ev.y == H // 2 then open_row(sel)   -- the select key
    elseif ev.y >= row_y0 and ev.y < row_y0 + rows_n * row_h then open_row((ev.y - row_y0) // row_h + 1) end
  end
end

return app
