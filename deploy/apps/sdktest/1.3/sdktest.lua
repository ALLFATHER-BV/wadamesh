-- SDK self-test. Exercises the extended SDK so the results can be read off the
-- screen instead of inferred from a build log. Published to the store as a
-- developer/bench tool.
--
-- 1.2 adds wada.crypto (checked against published RFC vectors, so a PASS here is
-- real evidence and not just "it returned something"), wada.mesh.channels, and
-- wada.mesh.send_dm.
-- 1.3 adds the discovery surface (wada.mesh.discover / discovered), packet
-- identity in rx_log, exact micro-degree coordinates, wada.ui.input and the
-- windowed wada.fs.read.
local ui, sys, store, timer = wada.ui, wada.sys, wada.store, wada.timer
local C = ui.colors

local app = {}
local rows, keyline, sendline, dmline, msgline = {}, nil, nil, nil, nil
local discline, inputline = nil, nil
local W = 300

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

  local c = sys.caps()
  row(y, "caps: sdk_ext=" .. yn(c.sdk_ext) .. "  kbd=" .. yn(c.keyboard) ..
         "  touch=" .. yn(c.touch) .. "  sd=" .. yn(c.sd), C.accent); y = y + 16
  row(y, "caps: discover=" .. yn(c.discover) .. "  input=" .. yn(c.input) ..
         "  rx_identity=" .. yn(c.rx_identity), C.accent); y = y + 20

  -- crypto: published test vectors, so this is checkable rather than merely alive
  if wada.crypto then
    local hex = wada.crypto.hex
    local k20 = string.rep(string.char(0x0b), 20)
    local checks = {
      { "sha256('abc')", hex(wada.crypto.sha256("abc")),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
      { "sha1('abc')", hex(wada.crypto.sha1("abc")),
        "a9993e364706816aba3e25717850c26c9cd0d89d" },
      { "hmac_sha1 RFC2202#1", hex(wada.crypto.hmac_sha1(k20, "Hi There")),
        "b617318655057264e28bc0b6fb378c8ef146be00" },
      { "hmac_sha256 RFC4231#1", hex(wada.crypto.hmac_sha256(k20, "Hi There")),
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7" },
    }
    local allok = true
    for _, t in ipairs(checks) do
      local ok = (t[2] == t[3])
      if not ok then allok = false end
      row(y, "crypto " .. t[1] .. ": " .. (ok and "PASS" or ("FAIL got " .. tostring(t[2]):sub(1, 16))),
          ok and C.good or C.bad); y = y + 16
    end
    -- The point of having it in C: this loop would blow the instruction budget in Lua.
    local t0 = sys.millis()
    for _ = 1, 200 do wada.crypto.hmac_sha1(k20, "Hi There") end
    row(y, string.format("crypto: 200 x hmac_sha1 in %d ms%s", sys.millis() - t0,
        allok and "" or "  (VECTORS FAILED)"), allok and C.good or C.bad); y = y + 20
  else
    row(y, "wada.crypto: MISSING", C.bad); y = y + 20
  end

  -- channel discovery
  local chans = wada.mesh.channels and wada.mesh.channels() or nil
  if chans then
    local names = table.concat(chans, ", ")
    row(y, "channels(): " .. #chans .. "  " .. names:sub(1, 60), C.text); y = y + 20
  else
    row(y, "wada.mesh.channels: MISSING", C.bad); y = y + 20
  end

  if not c.sdk_ext then
    row(y, "extended SDK is OFF on this board - stopping here.", C.sub)
    return
  end

  -- contacts, so send_dm has a target to name. 1.3 also checks the pubkey field.
  local cts = wada.mesh.contacts()
  local first = cts[1] and cts[1].name or nil
  row(y, "contacts: " .. #cts .. (first and ("  first=" .. first) or ""), C.text); y = y + 16
  if cts[1] then
    local pk = cts[1].pubkey
    row(y, "contacts[1].pubkey: " .. tostring(pk) ..
           (type(pk) == "string" and #pk == 8 and "  PASS" or "  FAIL"),
        (type(pk) == "string" and #pk == 8) and C.good or C.bad); y = y + 16
  end

  local me = wada.mesh.self()
  row(y, "self: " .. tostring(me.name) .. "  pubkey=" .. tostring(me.pubkey), C.text); y = y + 16

  -- Exact coordinates. Lua's floats here are 32-bit, so an app that logs a track
  -- must use the _e6 integers; this proves they are present and consistent.
  local fix = sys.gps()
  if fix then
    local drift = math.abs(fix.lat_e6 / 1e6 - fix.lat)
    row(y, string.format("gps: %d,%d e6  alt %dm  %d sats  drift %.6f  %s",
        fix.lat_e6, fix.lon_e6, fix.alt_m or 0, fix.sats, drift,
        drift < 0.001 and "PASS" or "FAIL"), drift < 0.001 and C.good or C.bad)
  else
    row(y, "gps: no fix (normal indoors)", C.sub)
  end
  y = y + 20

  -- rx_log identity: adverts carry a real public key, addressed frames carry
  -- one-byte hashes, everything else carries nothing. All three are correct.
  local log = wada.mesh.rx_log()
  local withpk, withsrc = 0, 0
  for _, r in ipairs(log) do
    if r.pubkey then withpk = withpk + 1 end
    if r.src then withsrc = withsrc + 1 end
  end
  row(y, string.format("rx_log: %d frames, %d with a pubkey (adverts), %d with src/dst",
      #log, withpk, withsrc), #log > 0 and C.text or C.sub); y = y + 20

  -- fs: windowed read. Writes once (the 1/sec limit means one write per open).
  if wada.fs then
    local probe = "0123456789abcdef"
    wada.fs.write("sdktest.bin", probe)
    local part, total = wada.fs.read("sdktest.bin", 4, 4)
    local ok = (part == "4567" and total == #probe)
    row(y, "fs.read(name,4,4): " .. tostring(part) .. " total=" .. tostring(total) ..
           (ok and "  PASS" or "  FAIL"), ok and C.good or C.bad); y = y + 20
  end

  sendline = row(y, "mesh.send: not tried yet", C.sub); y = y + 18
  dmline   = row(y, "mesh.send_dm: not tried yet", C.sub); y = y + 18
  msgline  = row(y, "on_message: waiting (needs the read permissions)", C.sub); y = y + 22

  ui.button("Send to Public", 6, y, 120, 30, function()
    local ok, err = wada.mesh.send("Public", "wadamesh SDK self-test")
    sendline:set("mesh.send: " .. tostring(ok) .. "  " .. tostring(err))
    sendline:color(ok and C.good or C.bad)
  end)
  ui.button("DM first contact", 132, y, 130, 30, function()
    if not first then dmline:set("mesh.send_dm: no contacts to target"); dmline:color(C.bad); return end
    local ok, err = wada.mesh.send_dm(first, "wadamesh SDK self-test (DM)")
    dmline:set("mesh.send_dm -> " .. first .. ": " .. tostring(ok) .. "  " .. tostring(err))
    dmline:color(ok and C.good or C.bad)
  end)
  y = y + 34
  y = y + 4
  discline  = row(y, "mesh.discover: not tried yet", C.sub); y = y + 18
  inputline = row(y, "ui.input: not tried yet", C.sub); y = y + 22

  -- Probing TRANSMITS and makes every neighbour reply, so it is a button, not
  -- something this app does on open.
  ui.button("Probe", 6, y, 90, 30, function()
    local tag, err = wada.mesh.discover()
    if not tag then
      discline:set("mesh.discover: " .. tostring(err)); discline:color(C.bad); return
    end
    discline:set("mesh.discover: sent, waiting for replies..."); discline:color(C.text)
  end)
  ui.button("Results", 102, y, 90, 30, function()
    local hits = wada.mesh.discovered()
    if #hits == 0 then
      discline:set("discovered(): nothing yet - probe, then wait a few seconds")
      discline:color(C.sub); return
    end
    local h = hits[1]
    discline:set(string.format("discovered(): %d  first %s snr %.1f/%.1f %s",
      #hits, h.name or h.pubkey, h.snr, h.their_snr, h.direct and "direct" or (h.hops .. "h")))
    discline:color(C.good)
  end)
  y = y + 34

  ui.button("Beep", 6, y, 70, 30, function() sys.beep() end)
  ui.button("Input", 82, y, 90, 30, function()
    ui.input("Type anything", "hello", function(text)
      inputline:set("ui.input -> " .. (text and ("'" .. text .. "'") or "cancelled"))
      inputline:color(text and C.good or C.sub)
    end)
  end)
end

function app.on_input(ev)
  if ev.type == "key" and keyline then
    keyline:set("keys: got '" .. tostring(ev.key) .. "'")
    keyline:color(C.good)
  end
end

-- Proves the kind field and that DMs/rooms reach an app at all, not just channels.
function app.on_message(m)
  if not msgline then return end
  msgline:set("on_message: kind=" .. tostring(m.kind) .. " from=" .. tostring(m.sender) ..
              " ch=" .. tostring(m.channel) .. " text=" .. tostring(m.text):sub(1, 20))
  msgline:color(C.good)
end

return app
