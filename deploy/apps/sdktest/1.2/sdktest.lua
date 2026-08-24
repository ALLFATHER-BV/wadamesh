-- SDK self-test. Exercises the extended SDK so the results can be read off the
-- screen instead of inferred from a build log. Published to the store as a
-- developer/bench tool.
--
-- 1.2 adds wada.crypto (checked against published RFC vectors, so a PASS here is
-- real evidence and not just "it returned something"), wada.mesh.channels, and
-- wada.mesh.send_dm.
local ui, sys, store, timer = wada.ui, wada.sys, wada.store, wada.timer
local C = ui.colors

local app = {}
local rows, keyline, sendline, dmline, msgline = {}, nil, nil, nil, nil
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

  -- contacts, so send_dm has a target to name
  local cts = wada.mesh.contacts()
  local first = cts[1] and cts[1].name or nil
  row(y, "contacts: " .. #cts .. (first and ("  first=" .. first) or ""), C.text); y = y + 20

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
  ui.button("Beep", 6, y, 70, 30, function() sys.beep() end)
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
