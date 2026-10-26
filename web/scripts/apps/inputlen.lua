-- Test app for the web host build (web/scripts/lua-input.wada): wada.ui.input's
-- max_len (d9f763a2), Enter as OK, caps().input_len, and wada.sys.random's ranges
-- (882a2046), all against the real LuaAppHost.cpp. Not a Store app.
local app = {}
local C, rl, ll, dl, fl

local function randomReport()
  local r = wada.sys.random
  local hist, bad = {0, 0, 0, 0, 0, 0}, 0
  for _ = 1, 600 do
    local v = r(6)
    if math.type(v) ~= "integer" or v < 1 or v > 6 then bad = bad + 1 else hist[v] = hist[v] + 1 end
  end
  local all_six = true
  for i = 1, 6 do if hist[i] < 60 then all_six = false end end
  local one_one, rev = true, true
  for _ = 1, 100 do
    if r(1, 1) ~= 1 then one_one = false end
    local v = r(5, 3)
    if v < 3 or v > 5 then rev = false end
  end
  local r1 = true
  for _ = 1, 50 do if r(1) ~= 1 then r1 = false end end
  local big = false
  for _ = 1, 20 do local v = r(); if math.type(v) == "integer" and (v > 70000 or v < -70000) then big = true end end
  local ok = bad == 0 and all_six and one_one and rev and r1 and big
  return string.format("random %s: (6) bad=%d hist=%d/%d/%d/%d/%d/%d (1,1)=%s (5,3)=%s (1)=%s ()raw=%s",
    ok and "OK" or "FAIL", bad, hist[1], hist[2], hist[3], hist[4], hist[5], hist[6],
    tostring(one_one), tostring(rev), tostring(r1), tostring(big)), ok
end

function app.on_open(w, h)
  C = wada.ui.colors
  local caps = wada.sys.caps()
  wada.ui.label("caps().input_len = " .. tostring(caps.input_len), 6, 4, 14, C.text)
  local txt, ok = randomReport()
  rl = wada.ui.label(txt, 6, 28, 12, ok and C.good or C.bad)
  rl:width(w - 12)
  ll = wada.ui.label("max_len 300: not tried", 6, 70, 14, C.sub)
  dl = wada.ui.label("default: not tried", 6, 94, 14, C.sub)
  wada.ui.button("Long", 6, 124, 120, 34, function()
    wada.ui.input("Up to 300", "", function(t)
      ll:set("max_len 300 -> " .. (t and (#t .. " chars, ends '" .. t:sub(-6) .. "'") or "nil"))
      ll:color(t and C.good or C.bad)
    end, 300)
  end)
  fl = wada.ui.label("full range: not tried", 6, 166, 14, C.sub)
  -- random(math.mininteger, math.maxinteger): a span of 2^32 is 0 in 32 bits, and
  -- the host took a modulo by it (a crash: SIGFPE here, a panic on the ESP32).
  wada.ui.button("Full", 262, 124, 120, 34, function()
    local lo, hi, inr = math.mininteger, math.maxinteger, 0
    for _ = 1, 100 do
      local v = wada.sys.random(lo, hi)
      if math.type(v) == "integer" then inr = inr + 1 end
    end
    local edge = wada.sys.random(-1, math.maxinteger)
    local ok = inr == 100 and edge >= -1
    fl:set(string.format("full range: %d/100 integers, (-1,max) gave %s", inr, tostring(edge >= -1)))
    fl:color(ok and C.good or C.bad)
  end)
  wada.ui.button("Default", 134, 124, 120, 34, function()
    wada.ui.input("No max_len", "", function(t)
      dl:set("default -> " .. (t and (#t .. " chars") or "nil"))
      dl:color(t and C.good or C.bad)
    end)
  end)
end

return app
