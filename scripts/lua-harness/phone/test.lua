-- Host test for deploy/apps/phone: a mock of wada.* (ui, sys, store, timer,
-- cell) driving the app through the dial pad, a call, an SMS and the modem page.
-- Mirrors the device rules that matter here: 100k-instruction budget per
-- callback, integer geometry, store values strings/numbers only, one
-- wada.cell request at a time, and no redraws when nothing changed (each
-- label:set() is an e-paper refresh on the device).
--
--   cc -O1 -w -Ilib/lua/src -o /tmp/lua <a main() that runs a file> lib/lua/src/*.c -lm
--   /tmp/lua scripts/lua-harness/phone/test.lua deploy/apps/phone/1.0/phone.lua
local APP = arg[1] or "deploy/apps/phone/1.0/phone.lua"
local BUDGET = 100000
local SCREEN_W, SCREEN_H = 240, 296

local buttons   -- forward (the mock's live button table)
-- With the tab bar on screen, nothing else may reach into it.
local function validate()
  if not buttons then return end
  local tab = buttons["\u{F013}"]
  if not tab then return end              -- the call screen has no tabs
  for name, b in pairs(buttons) do
    if b.y < tab.y and b.y + b.h > tab.y then
      error("button '" .. name .. "' runs into the tab bar (bottom " .. (b.y + b.h) .. ", tabs at " .. tab.y .. ")", 0)
    end
  end
end

local function guarded(fn, ...)
  local count = 0
  debug.sethook(function() count = count + 1000; if count > BUDGET then error("instruction budget exceeded") end end, "", 1000)
  local ok, e = pcall(fn, ...)
  debug.sethook()
  if not ok then error("callback failed: " .. tostring(e), 0) end
  validate()
end

local fails = 0
local function check(cond, what)
  if cond then print("  ok   " .. what) else fails = fails + 1; print("  FAIL " .. what) end
end

-- ---------------------------------------------------------------- mocks ---
buttons = {}
local labels, lists, panels, toasts, inputs, timers = {}, {}, {}, {}, {}, {}
local storekv = {}
label_sets = 0

local function int(v, what) assert(math.type(v) == "integer", what .. " must be an integer, got " .. tostring(v)) end
local function inside(x, y, w, h, what)
  int(x, what .. " x"); int(y, what .. " y"); int(w, what .. " w"); int(h, what .. " h")
  assert(w > 0 and h > 0, what .. ": empty size " .. w .. "x" .. h)
  assert(x >= 0 and y >= 0 and x + w <= SCREEN_W and y + h <= SCREEN_H,
         string.format("%s off screen: %d,%d %dx%d", what, x, y, w, h))
end

local function widget(kind, text)
  local o = { kind = kind, text = text }
  function o:set(t) assert(type(t) == "string", kind .. ":set wants a string"); self.text = t; label_sets = label_sets + 1 end
  function o:color(c) int(c, kind .. " color"); self.fg = c end
  function o:bg(c, b) int(c, kind .. " bg"); self.bgc = c end
  function o:width(v) int(v, kind .. " width") end
  function o:pos() end
  return o
end

local status = {
  enabled = false, power = "off", sim = "unknown", reg = "none", call = "idle", number = "",
  call_secs = 0, csq = -1, rssi = 0, bars = 0, operator = "", data = false, ip = "", apn = "", error = "",
}
local dialed, sent_sms, granted, dtmf = {}, {}, true, {}
local http_cb = nil
local fw_inbox = {}   -- what the firmware keeps on the card (oldest first)

local function gate() if not granted then return false, "permission requested" end return true end

wada = {
  ui = {
    colors = { accent = 1, text = 2, sub = 3, bg = 0, panel = 4, bad = 5, good = 6, warn = 7, border = 8 },
    text_h = function(sz) return sz + 6 end,
    text_w = function(t, sz) return #t * math.floor(sz * 0.6) end,
    label = function(t, x, y, sz, col, bold)
      assert(type(t) == "string", "label text must be a string")
      if col ~= nil then int(col, "label color") end
      local o = widget("label", t); labels[#labels + 1] = o; return o
    end,
    button = function(t, x, y, w, h, fn)
      inside(x, y, w, h, "button '" .. t .. "'")
      local b = widget("button", t); b.fn = fn; b.y, b.h = y, h; buttons[t] = b; return b
    end,
    panel = function(x, y, w, h, col, r)
      inside(x, y, w, h, "panel")
      if col ~= nil then int(col, "panel color") end
      local p = { }
      function p:color(c) int(c, "panel color") end
      panels[#panels + 1] = p; return p
    end,
    list = function(x, y, w, h)
      inside(x, y, w, h, "list")
      local l = { rows = {} }
      function l:add(t, fn) assert(type(t) == "string"); self.rows[#self.rows + 1] = { t = t, fn = fn } end
      function l:clear() self.rows = {} end
      function l:select() end
      lists[#lists + 1] = l; return l
    end,
    input = function(title, init, cb, max) inputs[#inputs + 1] = { title = title, cb = cb } end,
    clear = function() buttons, labels, lists, panels = {}, {}, {}, {} end,
  },
  sys = {
    toast = function(m) toasts[#toasts + 1] = m end,
    caps = function() return { cell = true } end,
  },
  store = {
    get = function(k, d) local v = storekv[k]; if v == nil then return d end return v end,
    set = function(k, v)
      assert(type(v) == "string" or type(v) == "number" or v == nil, "store values must be strings or numbers")
      storekv[k] = v
    end,
  },
  timer = { every = function(ms, fn) timers[#timers + 1] = fn end, after = function(ms, fn) timers[#timers + 1] = fn end },
  cell = {
    status = function() local c = {}; for k, v in pairs(status) do c[k] = v end; return c end,
    enable = function(on) local ok, e = gate(); if ok then status.enabled = on; status.power = on and "booting" or "off" end; return ok, e end,
    set_apn = function(a) local ok, e = gate(); if ok then status.apn = a end; return ok, e end,
    set_pin = function(p) return gate() end,
    dial = function(n) local ok, e = gate(); if ok then dialed[#dialed + 1] = n end; return ok, e end,
    answer = function() return true end,
    hangup = function() return true end,
    dtmf = function(d) dtmf[#dtmf + 1] = d; return true end,
    volume = function(v) int(v, "volume"); return true end,
    mute = function() return true end,
    sms = function(n, t) local ok, e = gate(); if ok then sent_sms[#sent_sms + 1] = { n, t } end; return ok, e end,
    data = function(up) return gate() end,
    inbox = function(max)
      local ok, e = gate(); if not ok then return false, e end
      local out = {}
      for i = #fw_inbox, math.max(1, #fw_inbox - (max or 20) + 1), -1 do out[#out + 1] = fw_inbox[i] end
      return out
    end,
    http_get = function(url, cb, max)
      if http_cb then return false, "a request is already running" end
      http_cb = cb; return true
    end,
  },
}

local function names()
  local t = {} for k in pairs(buttons) do t[#t + 1] = k end
  table.sort(t); return table.concat(t, ", ")
end
local function press(lbl)
  local b = buttons[lbl]
  assert(b, "no button '" .. lbl .. "' (have: " .. names() .. ")")
  guarded(b.fn)
end
local function answer_input(text)
  local p = table.remove(inputs, 1)
  assert(p, "no input prompt open")
  guarded(p.cb, text)
end
local function has_label(sub)
  for _, l in ipairs(labels) do if l.text:find(sub, 1, true) then return true end end
  return false
end
local function last_toast() return toasts[#toasts] or "" end
local function tick() for _, t in ipairs(timers) do guarded(t) end end
local CALL, LIST, MAIL, GEAR = "\u{F095}", "\u{F00B}", "\u{F0E0}", "\u{F013}"

-- ------------------------------------------------------------------ run ---
local chunk = assert(loadfile(APP))
local app
guarded(function() app = chunk() end)
assert(type(app) == "table", "app must return a table")

print("open with the modem off")
guarded(app.on_open, SCREEN_W, SCREEN_H)
check(has_label("Modem off"), "status card says the modem is off")
check(buttons["Turn on"] ~= nil, "opens on the modem page with a power button")
check(buttons[CALL] and buttons[LIST] and buttons[MAIL] and buttons[GEAR], "four tabs at the bottom")
-- A filled button must not write in its own fill colour (that was black on black on e-paper).
local function readable(b) return b.bgc == nil or (b.fg ~= nil and b.fg ~= b.bgc) end
check(buttons[GEAR].bgc ~= nil and readable(buttons[GEAR]), "selected tab is filled with readable text")
check(readable(buttons["Turn on"]), "primary button text is readable")

print("turn the modem on, registration arrives")
press("Turn on")
check(status.enabled, "enable() was called")
status.power, status.sim, status.reg, status.operator, status.csq, status.rssi, status.bars =
  "ready", "ready", "home", "Operator", 22, -69, 4
guarded(app.on_cell, { type = "status" })
check(has_label("Operator"), "status card shows the operator")
label_sets = 0
for _ = 1, 10 do tick() end
check(label_sets == 0, "idle ticks redraw nothing when nothing changed (" .. label_sets .. " sets)")

print("dial pad")
press(CALL)
check(has_label("Enter number"), "empty number shows a placeholder")
for _, d in ipairs({ "1", "2", "3" }) do press(d) end
check(has_label("123"), "keypad digits build the number")
press("\u{F55A}")
check(has_label("12") and not has_label("123"), "backspace removes the last digit")
for _, k in ipairs({ "3", "4", "backspace", "4" }) do guarded(app.on_input, { type = "key", key = k }) end
check(has_label("1234"), "hardware keyboard digits and backspace work in the dial pad")
press(CALL .. "  Call")
check(dialed[1] == "1234", "dials the typed number (got " .. tostring(dialed[1]) .. ")")
check(storekv.recent == "1234", "number saved to recent calls")
check(buttons[CALL .. "  Hang up"] ~= nil, "call screen with Hang up")
check(readable(buttons[CALL .. "  Hang up"]), "hang-up button text is readable")

print("call progress")
status.call, status.number = "alerting", "1234"
guarded(app.on_cell, { type = "call", state = "alerting", number = "1234" })
check(has_label("Ringing"), "alerting shown")
status.call, status.call_secs = "active", 67
guarded(app.on_cell, { type = "call", state = "active", number = "1234" })
tick()
check(has_label("1:05"), "duration shown in 5 s steps")
press("\u{F028}")
press("Mute")
check(buttons["Unmute"] ~= nil and buttons["Unmute"].bgc ~= nil, "mic mute toggles its label and shows it is on")
press("Unmute")
check(buttons["Mute"] ~= nil, "and unmutes again")
press("Keypad")
check(buttons["Hide"] ~= nil and buttons["5"] ~= nil, "in-call keypad on screen")
check(has_label("Press keys"), "keypad shows a hint before anything is sent")
for _, d in ipairs({ "1", "#", "9" }) do press(d) end
guarded(app.on_input, { type = "key", key = "4" })
check(table.concat(dtmf) == "1#94", "keypad and hardware keys send DTMF (" .. table.concat(dtmf) .. ")")
check(has_label("1#94"), "sent keys are shown")
check(#inputs == 0, "no text dialog for DTMF any more")
press("Hide")
check(buttons["Keypad"] ~= nil and buttons["Hide"] == nil, "keypad hides again")
status.call, status.call_secs, status.number = "idle", 0, ""
guarded(app.on_cell, { type = "call", state = "idle", number = "1234" })
check(buttons[CALL .. "  Call"] ~= nil, "back on the dial pad after hang-up")

print("incoming call")
status.call, status.number = "incoming", "+4587654321"
guarded(app.on_cell, { type = "call", state = "incoming", number = "+4587654321" })
check(buttons[CALL .. "  Answer"] ~= nil and buttons["Reject"] ~= nil, "Answer / Reject offered")
check(has_label("+4587654321"), "caller number shown")
press(CALL .. "  Answer")
status.call = "active"
guarded(app.on_cell, { type = "call", state = "active", number = "+4587654321" })
check(buttons[CALL .. "  Hang up"] ~= nil and buttons[CALL .. "  Answer"] == nil, "buttons swap to Hang up once answered")
status.call = "idle"
guarded(app.on_cell, { type = "call", state = "idle", number = "" })

print("recent calls")
press(LIST)
check(#lists[1].rows == 2 and lists[1].rows[1].t:find("4587654321", 1, true), "incoming number is first in recent")
guarded(lists[1].rows[2].fn)
check(has_label("1234"), "tapping a recent number puts it in the dial pad")

print("sms")
fw_inbox[#fw_inbox + 1] = { from = "+4500000000", text = "kom mens appen var lukket", when = "26/11/16,09:00:00+04" }
fw_inbox[#fw_inbox + 1] = { from = "+4511111111", text = "Hej med dig æøå", when = "26/11/16,10:00:00+04" }
guarded(app.on_cell, { type = "sms", from = "+4511111111", text = "Hej med dig æøå", when = "26/11/16,10:00:00+04" })
check(last_toast():find("SMS from", 1, true) ~= nil, "toast on incoming SMS")
press(MAIL)
check(#lists[1].rows == 2 and lists[1].rows[1].t:find("æøå", 1, true), "inbox newest first, æøå intact")
check(lists[1].rows[1].t:find("11/16,10:00", 1, true) ~= nil, "message shows its date and time")
guarded(lists[1].rows[1].fn)
answer_input("Svar")
check(sent_sms[1] and sent_sms[1][1] == "+4511111111" and sent_sms[1][2] == "Svar", "tapping a message replies to its sender")
press("+ New")
answer_input("+45 22 22 22 22")
answer_input("Test")
check(sent_sms[2] and sent_sms[2][1] == "+4522222222", "new SMS to a cleaned number")
guarded(app.on_cell, { type = "sms_sent", ok = true, number = "+4522222222" })
check(last_toast() == "SMS sent", "sent confirmation")

print("modem page")
press(GEAR)
check(has_label("Operator") and has_label("-69 dBm"), "modem card shows operator and signal")
press("APN")
answer_input("internet")
check(status.apn == "internet", "APN saved")
press("Test data")
check(http_cb ~= nil, "HTTP request started")
guarded(http_cb, 200, "<html>hello</html>")
check(last_toast() == "HTTP 200, 18 bytes", "HTTP result shown (" .. last_toast() .. ")")

print("permission refused")
granted = false
press(CALL)
for _, d in ipairs({ "1", "1", "2" }) do press(d) end
local before = #dialed
press(CALL .. "  Call")
check(#dialed == before, "no dial without the grant")
check(last_toast() == "permission requested", "user told why")

print(fails == 0 and "ALL PASSED" or (fails .. " FAILED"))
os.exit(fails == 0 and 0 or 1)
