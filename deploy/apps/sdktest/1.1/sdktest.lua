-- SDK self-test. Exercises everything added to the extended SDK so the results
-- can be read off the screen instead of inferred from a build log.
-- Published to the store as a developer/bench tool.
local ui, sys, store, timer = wada.ui, wada.sys, wada.store, wada.timer
local C = ui.colors

local app = {}
local rows, keyline, sendline = {}, nil, nil
local W = 300   -- replaced with the real body width in on_open

local function row(y, text, color)
  local l = ui.label(text, 6, y, 12, color or C.text)
  l:width(W - 12)
  rows[#rows + 1] = l
  return l
end

local function yn(v) return v and "yes" or "NO" end

function app.on_open(w, h)
  W = w or 300
  ui.scroll(true)
  local y = 4

  -- 1. capability report --------------------------------------------------
  local c = sys.caps()
  row(y, "caps: sdk_ext=" .. yn(c.sdk_ext) .. "  kbd=" .. yn(c.keyboard) ..
         "  touch=" .. yn(c.touch) .. "  sd=" .. yn(c.sd), C.accent); y = y + 16

  -- 2. time (#245) --------------------------------------------------------
  local ep = sys.epoch()
  if ep then
    local d = sys.datetime()
    row(y, string.format("time: %d  %04d-%02d-%02d %02d:%02d:%02d",
        ep, d.year, d.month, d.day, d.hour, d.min, d.sec), C.good)
  else
    row(y, "time: nil (clock not set yet - expected with no GPS/NTP)", C.sub)
  end
  y = y + 16

  if not c.sdk_ext then
    row(y, "extended SDK is OFF on this board - nothing further to test.", C.sub)
    return
  end

  -- 3. battery ------------------------------------------------------------
  local b = sys.battery()
  row(y, string.format("battery: %d mV  %d%%  charging=%s", b.mv, b.pct, yn(b.charging)),
      b.mv > 0 and C.good or C.bad); y = y + 16

  -- 4. gps ----------------------------------------------------------------
  local g = sys.gps()
  if g then
    row(y, string.format("gps: %.5f, %.5f  sats=%d", g.lat, g.lon, g.sats), C.good)
  else
    row(y, "gps: nil (no fix - correct indoors)", C.sub)
  end
  y = y + 16

  -- 5. filesystem round trip + rate limit ---------------------------------
  local stamp = tostring(sys.millis())
  local ok, err = wada.fs.write("selftest.txt", "hello " .. stamp)
  local back = ok and wada.fs.read("selftest.txt") or nil
  row(y, "fs write: " .. yn(ok) .. (err and (" (" .. err .. ")") or "") ..
         "   read back: " .. (back == ("hello " .. stamp) and "MATCH" or tostring(back)),
      ok and C.good or C.bad); y = y + 16

  local ok2, err2 = wada.fs.write("selftest.txt", "again")   -- immediately: must be refused
  row(y, "fs rate limit: " .. (ok2 == false and err2 == "too fast"
        and "REFUSED as designed" or ("UNEXPECTED " .. tostring(ok2) .. " " .. tostring(err2))),
      (ok2 == false) and C.good or C.bad); y = y + 16

  local bad = wada.fs.write("../identity", "x")
  row(y, "fs traversal '../identity': " .. (bad == false and "REJECTED" or "LEAKED - BUG"),
      bad == false and C.good or C.bad); y = y + 16

  local files = wada.fs.list()
  row(y, "fs list: " .. #files .. " file(s)" ..
         (files[1] and (" first=" .. files[1].name .. " " .. files[1].size .. "B") or ""),
      C.text); y = y + 20

  -- 6. keys ---------------------------------------------------------------
  keyline = row(y, "keys: press any key on the keyboard...", C.sub); y = y + 20

  -- 7. mesh send (consent) -------------------------------------------------
  sendline = row(y, "mesh.send: not tried yet", C.sub); y = y + 22

  ui.button("Send to Public", 6, y, 130, 30, function()
    local sok, serr = wada.mesh.send("Public", "wadamesh SDK self-test")
    sendline:set("mesh.send: " .. tostring(sok) .. "  " .. tostring(serr))
    sendline:color(sok and C.good or C.bad)
  end)
  ui.button("Beep", 146, y, 70, 30, function()
    sys.beep()
  end)
end

-- keys and touch both arrive here
function app.on_input(ev)
  if ev.type == "key" and keyline then
    keyline:set("keys: got '" .. tostring(ev.key) .. "'  code=" .. tostring(ev.code))
    keyline:color(C.good)
  end
end

return app
