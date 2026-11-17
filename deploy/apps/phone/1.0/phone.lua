-- Phone (Lua) — calls, SMS and mobile data on the T-Deck Max's A7682E 4G modem.
--
-- Everything modem-side lives in firmware (CellularService + wada.cell), and so
-- do the parts that must work with no app open: the 4G signal in the status bar,
-- the incoming-call sheet with its ring, and the SMS inbox on the card. This app
-- is the dialer, the in-call controls, SMS writing and reading the inbox. While
-- it is in front it shows calls itself and the firmware's call sheet stays away.
--
-- Layout: a status card on top, the page in the middle, four tabs at the bottom
-- (dial pad, recent calls, SMS, modem). Digits typed on the hardware keyboard go
-- straight into the dial pad. Drawn for e-paper: no animation, and a label is
-- only touched when its text changes (every change is a panel refresh).
--
-- Needs the "4G: calls, SMS and mobile data" permission. The first dial/SMS/
-- setting asks for it; answering and hanging up never need it.
local ui, sys, store, timer, cell = wada.ui, wada.sys, wada.store, wada.timer, wada.cell
local C = ui.colors
-- Every string goes through the device language table (wada.sys.tr), so a
-- .lang file translates the app with the rest of the interface.
local tr = sys.tr or function(x) return x end
local app = {}

local SYM_CALL, SYM_LIST, SYM_MAIL, SYM_GEAR = "\u{F095}", "\u{F00B}", "\u{F0E0}", "\u{F013}"
local SYM_BACK, SYM_VOLDN, SYM_VOLUP = "\u{F55A}", "\u{F027}", "\u{F028}"

local W, H, LH
local HDR_H, TAB_H, PAD = 38, 34, 6
local view = "dial"
local number = ""
local muted, vol = false, 4
local prompt_open = false     -- a ui.input dialog is up: keys belong to it

-- live widgets (rebuilt by show())
local w = {}

-- ---------------------------------------------------------------- helpers ---
local function st() return cell.status() end

local function say(ok, why)
  if not ok then sys.toast(tostring(why or tr("error"))) end
  return ok
end

local function split(s)
  local t = {}
  for p in string.gmatch(s or "", "[^|]+") do t[#t + 1] = p end
  return t
end

local function remember_number(n)
  if n == "" then return end
  local t, out = split(store.get("recent", "")), { n }
  for _, v in ipairs(t) do if v ~= n and #out < 10 then out[#out + 1] = v end end
  store.set("recent", table.concat(out, "|"))
end

local function inbox()
  local t = cell.inbox(20)
  if type(t) == "table" then return t end
  return {}
end

local function input(title, init, cb, max)
  prompt_open = true
  ui.input(title, init, function(t) prompt_open = false; cb(t) end, max)
end

-- E-paper: only touch a label when its text actually changes.
local shown = setmetatable({}, { __mode = "k" })
local function put(lbl, text)
  if lbl and shown[lbl] ~= text then
    shown[lbl] = text
    lbl:set(text)
  end
end
local function label(text, x, y, size, col, bold)
  local l = ui.label(text, x, y, size, col, bold)
  shown[l] = text
  return l
end

local function status_line(s)
  if not s.enabled then return tr("Modem off") end
  if s.power == "booting" then return tr("Starting modem...") end
  if s.power == "error" then return tr("Modem not responding") end
  if s.sim == "absent" then return tr("No SIM card") end
  if s.sim == "pin" then return tr("SIM locked") end
  if s.sim == "puk" then return tr("SIM PUK locked") end
  if s.reg == "searching" or s.reg == "none" then return tr("Searching for network...") end
  if s.reg == "denied" then return tr("Network denied") end
  return (s.operator ~= "" and s.operator or tr("Mobile network")) .. (s.reg == "roaming" and "  R" or "")
end

local function sub_line(s)
  if not s.enabled then return string.format(tr("Tap %s to turn on"), SYM_GEAR) end
  if s.sim == "pin" then return string.format(tr("Set the PIN under %s"), SYM_GEAR) end
  if s.power ~= "ready" then return s.error ~= "" and s.error or "" end
  if s.data then return "4G  ·  " .. tr("data") .. " " .. s.ip end
  return "4G  ·  " .. (s.rssi ~= 0 and (s.rssi .. " dBm") or tr("no signal"))
end

-- A filled (primary / selected) button. Fill with the text colour and write in
-- the ground colour: on 1-bit e-paper the host inverts app colours, so this is
-- black with white text there and light with dark text on a colour screen. A
-- coloured fill (accent, good, bad) came out black with black text on e-paper.
local function filled(b)
  b:bg(C.text, C.text)
  b:color(C.bg)
  return b
end

local function fmt_secs(n)
  return string.format("%d:%02d", n // 60, n % 60)
end

-- ------------------------------------------------------------- the chrome ---
local function header()
  local s = st()
  ui.panel(0, 0, W, HDR_H, C.panel, 10)
  w.hdr = label(status_line(s), 10, 4, 14, C.text, true)
  w.hdr:width(W - 70)
  w.sub = label(sub_line(s), 10, 4 + LH + 1, 12, C.sub)
  w.sub:width(W - 70)
  -- four signal bars, bottom-aligned at the right of the card
  w.bars = {}
  local bh = { 6, 10, 14, 18 }
  local x0, base = W - 44, HDR_H - 9
  for i = 1, 4 do
    w.bars[i] = ui.panel(x0 + (i - 1) * 8, base - bh[i], 5, bh[i], C.sub, 1)
  end
  w.bars_lit = -1
end

local function refresh_bars(s)
  if not w.bars then return end
  local lit = (s.enabled and (s.reg == "home" or s.reg == "roaming")) and (s.bars or 0) or 0
  if lit == w.bars_lit then return end
  w.bars_lit = lit
  for i = 1, 4 do w.bars[i]:color(i <= lit and C.text or C.sub) end
end

local function refresh_header()
  if not w.hdr then return end
  local s = st()
  put(w.hdr, status_line(s))
  put(w.sub, sub_line(s))
  refresh_bars(s)
end

local show   -- forward

local TABS = {
  { id = "dial",     sym = SYM_CALL },
  { id = "recent",   sym = SYM_LIST },
  { id = "sms",      sym = SYM_MAIL },
  { id = "settings", sym = SYM_GEAR },
}
local function tabs()
  local tw = math.floor((W - 2 * PAD - 3 * 4) / 4)
  local y = H - TAB_H
  for i, t in ipairs(TABS) do
    local b = ui.button(t.sym, PAD + (i - 1) * (tw + 4), y, tw, TAB_H - 4, function() show(t.id) end)
    if t.id == view then filled(b) end
  end
end

local function body_top() return HDR_H + PAD end
local function body_bottom() return H - TAB_H - PAD end

-- ------------------------------------------------------------------ views ---
local function refresh_number()
  if not w.num then return end
  if number == "" then
    put(w.num, tr("Enter number"))
    w.num:color(C.sub)
  else
    put(w.num, number)
    w.num:color(C.text)
  end
end

local function dial_now()
  if number == "" then sys.toast(tr("Enter a number")); return end
  if say(cell.dial(number)) then remember_number(number); show("call") end
end

local function key(d)
  if #number >= 20 then return end
  number = number .. d
  refresh_number()
end

local function view_dial()
  local y = body_top()
  -- number display card with backspace
  ui.panel(PAD, y, W - 2 * PAD, 40, C.bg, 10)
  w.num = label("", PAD + 10, y + 8, number == "" and 14 or 24, C.text)
  shown[w.num] = nil
  refresh_number()
  ui.button(SYM_BACK, W - PAD - 44, y + 4, 40, 32, function()
    number = number:sub(1, -2)
    if number == "" then show("dial") else refresh_number() end
  end)
  y = y + 40 + PAD

  local call_h = 40
  local rows = { { "1", "2", "3" }, { "4", "5", "6" }, { "7", "8", "9" }, { "*", "0", "#" } }
  local avail = body_bottom() - y - call_h - PAD
  local gap = 4
  local kh = math.floor((avail - 3 * gap) / 4)
  local kw = math.floor((W - 2 * PAD - 2 * gap) / 3)
  for r, row in ipairs(rows) do
    for c, d in ipairs(row) do
      ui.button(d, PAD + (c - 1) * (kw + gap), y + (r - 1) * (kh + gap), kw, kh, function()
        local was_empty = number == ""
        key(d)
        if was_empty then show("dial") end   -- the big font replaces the placeholder
      end)
    end
  end
  y = y + 4 * kh + 3 * gap + PAD
  local cb = ui.button(SYM_CALL .. "  " .. tr("Call"), PAD, y, W - 2 * PAD, call_h, dial_now)
  filled(cb)
end

local function view_recent()
  local y = body_top()
  label(tr("Recent calls"), PAD + 2, y, 14, C.text, true)
  local bw = 64
  ui.button(tr("Clear"), W - PAD - bw, y - 2, bw, 26, function()
    store.set("recent", "")
    show("recent")
  end)
  y = y + LH + 10
  w.list = ui.list(PAD, y, W - 2 * PAD, body_bottom() - y)
  local rec = split(store.get("recent", ""))
  for _, n in ipairs(rec) do
    w.list:add(SYM_CALL .. "  " .. n, function() number = n; show("dial") end)
  end
  if #rec == 0 then w.list:add(tr("no calls yet")) end
end

local function send_sms(to)
  input(string.format(tr("Message to %s (max 70)"), to), "", function(t)
    if t and t ~= "" and say(cell.sms(to, t)) then sys.toast(tr("Sending...")) end
  end, 70)
end

local function view_sms()
  local y = body_top()
  label(tr("Messages"), PAD + 2, y, 14, C.text, true)
  local bw = 84
  ui.button("+ " .. tr("New"), W - PAD - bw, y - 2, bw, 26, function()
    input(tr("To number"), number, function(n)
      if not n or n == "" then return end
      send_sms((n:gsub("[^%d%+]", "")))
    end, 30)
  end)
  y = y + LH + 10
  w.list = ui.list(PAD, y, W - 2 * PAD, body_bottom() - y)
  local t = inbox()
  for _, m in ipairs(t) do
    local when = (m.when or ""):match("^%d+/(%d+/%d+,%d+:%d+)") or ""
    w.list:add(SYM_MAIL .. " " .. m.from .. "  " .. when .. "\n" .. m.text, function()
      number = m.from
      send_sms(m.from)
    end)
  end
  if #t == 0 then w.list:add(tr("no messages")) end
end

local function view_settings()
  local s = st()
  local y = body_top()
  local info = {
    { tr("Power"), s.enabled and (s.power == "ready" and tr("on") or s.power) or tr("off") },
    { "SIM", ({ ready = tr("ready"), pin = tr("PIN needed"), puk = tr("PUK locked"), absent = tr("missing") })[s.sim] or "-" },
    { tr("Network"), s.operator ~= "" and s.operator or "-" },
    { tr("Signal"), s.csq >= 0 and (s.rssi .. " dBm") or "-" },
    { tr("Data"), s.data and s.ip or tr("off") },
    { "APN", s.apn ~= "" and s.apn or tr("default") },
  }
  local row_h = LH + 1
  ui.panel(PAD, y, W - 2 * PAD, #info * row_h + 8, C.panel, 8)
  for i, kv in ipairs(info) do
    label(kv[1], PAD + 8, y + 4 + (i - 1) * row_h, 12, C.sub)
    label(kv[2], PAD + 70, y + 4 + (i - 1) * row_h, 12, C.text)
  end
  y = y + #info * row_h + 8 + PAD
  -- two rows of buttons: power / data / test, then APN / PIN
  local gap = 6
  local b3 = math.floor((W - 2 * PAD - 2 * gap) / 3)
  local bh = 30
  local b = ui.button(s.enabled and tr("Turn off") or tr("Turn on"), PAD, y, b3, bh, function()
    if say(cell.enable(not st().enabled)) then show("settings") end
  end)
  if not s.enabled then filled(b) end
  ui.button(s.data and tr("Data off") or tr("Data on"), PAD + b3 + gap, y, b3, bh, function()
    if say(cell.data(not st().data)) then sys.toast(tr("Working...")) end
  end)
  ui.button(tr("Test data"), PAD + 2 * (b3 + gap), y, b3, bh, function()
    local ok, why = cell.http_get("http://example.com/", function(code, body)
      sys.toast(string.format("HTTP %d, %d bytes", code, body and #body or 0))
    end, 4096)
    if ok then sys.toast(tr("Fetching example.com...")) else say(ok, why) end
  end)
  y = y + bh + 4
  local b2 = math.floor((W - 2 * PAD - gap) / 2)
  ui.button("APN", PAD, y, b2, bh, function()
    input(tr("APN (e.g. internet)"), s.apn, function(a)
      if a and say(cell.set_apn(a)) then show("settings") end
    end, 40)
  end)
  ui.button("SIM PIN", PAD + b2 + gap, y, b2, bh, function()
    input(tr("SIM PIN (empty = none)"), "", function(p)
      if p and say(cell.set_pin(p)) then sys.toast(tr("PIN saved")) end
    end, 8)
  end)
end

local CALL_STATE = { dialing = "Calling...", alerting = "Ringing...", incoming = "Incoming call",
                     active = "Connected", held = "On hold", idle = "Ended" }

local function update_call()
  if not w.state then return end
  local s = st()
  put(w.state, CALL_STATE[s.call] and tr(CALL_STATE[s.call]) or s.call)
  if s.call == "active" then
    put(w.dur, fmt_secs(s.call_secs - s.call_secs % 5))   -- 5 s steps on e-paper
  else
    put(w.dur, "")
  end
end

-- In-call keypad: DTMF goes to the far end as network signalling (AT+VTS),
-- so it makes no sound here and the modem has no local tone. The keys sent so
-- far are shown instead, so it is clear they went.
local dtmf_open, dtmf_sent = false, ""

local function view_call_keypad(s)
  local y = body_top()
  -- compact caller line + what has been sent
  ui.panel(PAD, y, W - 2 * PAD, 44, C.panel, 10)
  local who = s.number ~= "" and s.number or (number ~= "" and number or tr("Unknown number"))
  label(who, PAD + 10, y + 4, 14, C.text, true)
  w.state = label("", W - PAD - 92, y + 4, 12, C.accent)
  w.dur = label("", W - PAD - 92, y + 4 + LH, 12, C.sub)
  w.tones = label(dtmf_sent ~= "" and dtmf_sent or tr("Press keys"), PAD + 10, y + 4 + LH + 2, 14,
                  dtmf_sent ~= "" and C.text or C.sub)
  y = y + 44 + PAD

  local by = H - PAD - 44
  local rows = { { "1", "2", "3" }, { "4", "5", "6" }, { "7", "8", "9" }, { "*", "0", "#" } }
  local gap = 4
  local kh = math.floor((by - PAD - y - 3 * gap) / 4)
  local kw = math.floor((W - 2 * PAD - 2 * gap) / 3)
  for r, row in ipairs(rows) do
    for c, d in ipairs(row) do
      ui.button(d, PAD + (c - 1) * (kw + gap), y + (r - 1) * (kh + gap), kw, kh, function()
        if cell.dtmf(d) then
          dtmf_sent = (dtmf_sent .. d):sub(-14)
          put(w.tones, dtmf_sent)
          w.tones:color(C.text)
        end
      end)
    end
  end
  local hw = math.floor((W - 2 * PAD - 8) / 2)
  ui.button(tr("Hide"), PAD, by, hw, 44, function() dtmf_open = false; show("call") end)
  filled(ui.button(SYM_CALL .. "  " .. tr("Hang up"), PAD + hw + 8, by, hw, 44, function() cell.hangup() end))
  update_call()
end

local function view_call()
  local s = st()
  if dtmf_open and s.call ~= "incoming" then return view_call_keypad(s) end
  local y = body_top() + 6
  -- caller card
  ui.panel(PAD, y, W - 2 * PAD, 96, C.panel, 14)
  local who = s.number ~= "" and s.number or (number ~= "" and number or tr("Unknown number"))
  local big = who:match("^[%d%+%*#]+$") ~= nil
  local tw = ui.text_w(who, big and 24 or 16)
  label(who, math.max(PAD + 8, math.floor((W - tw) / 2)), y + 14, big and 24 or 16, C.text, true)
  w.state = label("", PAD + 12, y + 52, 14, C.accent)
  w.state:width(W - 2 * PAD - 24)
  w.dur = label("", PAD + 12, y + 52 + LH + 4, 14, C.sub)
  y = y + 96 + PAD + 4

  local bw = math.floor((W - 2 * PAD - 2 * 6) / 3)
  -- Microphone mute. The label says what a tap does; while muted the button is
  -- filled, so it is obvious the other side cannot hear you.
  local mb = ui.button(muted and tr("Unmute") or tr("Mute"), PAD, y, bw, 38, function()
    muted = not muted; cell.mute(muted); show("call")
  end)
  if muted then filled(mb) end
  ui.button(SYM_VOLDN, PAD + bw + 6, y, bw, 38, function()
    vol = math.max(0, vol - 1); cell.volume(vol); sys.toast(string.format(tr("Volume %d"), vol))
  end)
  ui.button(SYM_VOLUP, PAD + 2 * (bw + 6), y, bw, 38, function()
    vol = math.min(5, vol + 1); cell.volume(vol); sys.toast(string.format(tr("Volume %d"), vol))
  end)
  y = y + 44
  if s.call ~= "incoming" then
    ui.button(tr("Keypad"), PAD, y, W - 2 * PAD, 34, function() dtmf_open = true; show("call") end)
  end

  local by = H - PAD - 48
  if s.call == "incoming" then
    local hw = math.floor((W - 2 * PAD - 8) / 2)
    filled(ui.button(SYM_CALL .. "  " .. tr("Answer"), PAD, by, hw, 48, function() cell.answer() end))
    ui.button(tr("Reject"), PAD + hw + 8, by, hw, 48, function() cell.hangup() end)   -- outlined: not the default
  else
    filled(ui.button(SYM_CALL .. "  " .. tr("Hang up"), PAD, by, W - 2 * PAD, 48, function() cell.hangup() end))
  end
  update_call()
end

show = function(v)
  view = v
  w = {}
  ui.clear()
  if v == "call" then
    header()
    view_call()
  else
    header()
    if v == "recent" then view_recent()
    elseif v == "sms" then view_sms()
    elseif v == "settings" then view_settings()
    else view = "dial"; view_dial() end
    tabs()
  end
  refresh_bars(st())
end

-- ---------------------------------------------------------------- events ---
function app.on_cell(ev)
  if ev.type == "call" then
    if ev.state == "idle" then
      dtmf_open, dtmf_sent = false, ""
      if view == "call" then sys.toast(tr("Call ended")); show("dial") end
    elseif view ~= "call" then
      if ev.state == "incoming" and ev.number ~= "" then remember_number(ev.number) end
      show("call")
    elseif ev.state == "active" then
      show("call")            -- incoming -> active swaps Answer/Reject for Hang up
    else
      update_call()
    end
  elseif ev.type == "ring" then
    if view ~= "call" then show("call") end
  elseif ev.type == "sms" then
    sys.toast(string.format(tr("SMS from %s"), ev.from))
    if view == "sms" then show("sms") end
  elseif ev.type == "sms_sent" then
    sys.toast(ev.ok and tr("SMS sent") or tr("SMS failed"))
  elseif ev.type == "data" then
    sys.toast(ev.up and string.format(tr("Data connected %s"), ev.ip) or tr("Data disconnected"))
    if view == "settings" then show("settings") else refresh_header() end
  else
    if view == "settings" then show("settings") else refresh_header() end
  end
end

-- Hardware keyboard: digits go straight into the dial pad.
function app.on_input(ev)
  if ev.type ~= "key" or prompt_open or not ev.key then return end
  if view == "call" and dtmf_open then   -- hardware keys send DTMF while the keypad is up
    local k = ev.key
    if k:match("^[%d%*#]$") and cell.dtmf(k) then
      dtmf_sent = (dtmf_sent .. k):sub(-14)
      put(w.tones, dtmf_sent)
      if w.tones then w.tones:color(C.text) end
    end
    return
  end
  if view ~= "dial" then return end
  local k = ev.key
  if k:match("^[%d%*#%+]$") then
    local was_empty = number == ""
    key(k)
    if was_empty then show("dial") end
  elseif k == "backspace" and number ~= "" then
    number = number:sub(1, -2)
    if number == "" then show("dial") else refresh_number() end
  elseif k == "enter" then
    dial_now()
  end
end

function app.on_open(width, height)
  W, H = width, height
  LH = ui.text_h(12)
  if not (sys.caps().cell) then
    ui.label(tr("This device has no 4G modem."), 6, 10, 14, C.bad)
    return
  end
  local s = st()
  if s.call ~= "idle" then show("call") else show(s.enabled and "dial" or "settings") end
  timer.every(3000, function()
    refresh_header()
    if view == "call" then update_call() end
  end)
end

return app
