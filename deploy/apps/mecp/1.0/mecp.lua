-- MECP: write and read Mesh Emergency Communication Protocol messages.
-- A MECP message is plain text, MECP/<severity>/<codes> [extras], so it travels
-- over any channel and every reader with a code table sees it in their own
-- language. Pick a severity and the codes; the app adds the head count and your
-- position, shows the exact text, and posts it to the channel you choose.
-- Decode turns a MECP line back into words, and lines heard on your channels
-- while the app is open are listed decoded. Requires: sdk_ext (channel send).

local ui, sys, mesh, store = wada.ui, wada.sys, wada.mesh, wada.store
local C = ui.colors
local clear = ui.clear or function() end

-- English code table from MECP (github.com/xiang-dev-1/MECP, languages/en.json),
-- by xiang-dev-1, CC BY 4.0. The protocol itself is CC0.
local CATS = {
  { id = "M", name = "Medical" },
  { id = "T", name = "Terrain / Infrastructure" },
  { id = "W", name = "Weather / Environment" },
  { id = "S", name = "Supplies" },
  { id = "P", name = "Position / Movement" },
  { id = "C", name = "Coordination" },
  { id = "R", name = "Response" },
  { id = "D", name = "Drill / Test" },
  { id = "L", name = "Life / Leisure" },
  { id = "X", name = "Threat / Security" },
  { id = "H", name = "Have / Offer Resources" },
  { id = "B", name = "Beacon" },
}
local CODES = {
  M01 = "Injury",
  M02 = "Unconscious person",
  M03 = "Breathing difficulty",
  M04 = "Cardiac event",
  M05 = "Hypothermia",
  M06 = "Severe bleeding",
  M07 = "Fracture / immobile",
  M08 = "Burns",
  M09 = "Multiple casualties",
  M10 = "Deceased",
  M11 = "Animal bite / sting",
  M12 = "Allergic reaction / anaphylaxis",
  M13 = "Poisoning / toxic exposure",
  M14 = "Persons located alive",
  M15 = "Area searched, no victims found",
  T01 = "Road blocked",
  T02 = "Bridge out",
  T03 = "Building collapsed",
  T04 = "Flooding",
  T05 = "Landslide",
  T06 = "Power out",
  T07 = "Fire",
  T08 = "Avalanche",
  T09 = "Path impassable",
  T10 = "Shelter available",
  T11 = "Drowning / water rescue needed",
  T12 = "Water contamination",
  T13 = "Earthquake",
  T14 = "Gas leak",
  T15 = "Chemical spill / HAZMAT",
  T16 = "Vehicle accident",
  T17 = "Vehicle fire",
  W01 = "Storm approaching",
  W02 = "Visibility zero",
  W03 = "Extreme cold",
  W04 = "Extreme heat",
  W05 = "Air quality danger",
  W06 = "Tsunami / tidal surge warning",
  S01 = "Need water",
  S02 = "Need food",
  S03 = "Need medication",
  S04 = "Need battery / power",
  S05 = "Need fuel",
  S06 = "Need tools / equipment",
  P01 = "Stranded / stuck",
  P02 = "Evacuating toward",
  P03 = "Sheltering in place",
  P04 = "En route to",
  P05 = "At GPS coordinates",
  P06 = "Lost",
  P07 = "Group separated",
  C01 = "Send rescue",
  C02 = "Need transport",
  C03 = "Relay this message",
  C04 = "Confirm received",
  C05 = "How many people",
  C06 = "What is status",
  C07 = "Can you reach",
  C08 = "Rendezvous at",
  R01 = "Acknowledged",
  R02 = "Help coming",
  R03 = "ETA [minutes]",
  R04 = "Cannot assist",
  R05 = "Redirecting to",
  R06 = "Stand by",
  R07 = "Situation resolved / all clear",
  D01 = "This is a drill",
  D02 = "This is a test",
  D03 = "End of drill",
  D04 = "Ignore previous, sent in error",
  L01 = "Beer / drinks",
  L02 = "Coffee",
  L03 = "Food ready",
  L04 = "Summit reached",
  L05 = "At camp",
  L06 = "Running late",
  L07 = "Good signal here",
  L08 = "Photo opportunity",
  L09 = "Wildlife spotted",
  L10 = "Beautiful view",
  L11 = "Trail conditions good",
  L12 = "Trail conditions bad",
  L13 = "Need a break",
  L14 = "Heading home",
  L15 = "Good morning / check-in",
  L16 = "Good night",
  L17 = "Thank you",
  L18 = "Having fun",
  L19 = "Festival / event here",
  L20 = "Node test / ping",
  X01 = "Dangerous person / threat nearby",
  X02 = "Area unsafe, avoid",
  X03 = "Gunfire / explosions heard",
  X04 = "Civil unrest / crowd danger",
  X05 = "Theft / looting reported",
  X06 = "Authorities / emergency services present",
  X07 = "Checkpoint / road closure (authority)",
  H01 = "Have water available",
  H02 = "Have food available",
  H03 = "Have medical supplies",
  H04 = "Have power / charging",
  H05 = "Have fuel",
  H06 = "Have tools / equipment",
  H07 = "Have shelter / space for [N]pax",
  H08 = "Have transport / vehicle",
  B01 = "Automated distress beacon active, sender may be unresponsive",
  B02 = "Beacon acknowledged, reduce transmission rate",
  B03 = "Cancel beacon, I am OK",
}

local SEV     = { [0] = "MAYDAY", "URGENT", "SAFETY", "ROUTINE" }
local SEV_SUB = { [0] = "life-threatening", "time-critical", "important", "normal" }
local MAX_LEN    = 180   -- wada.mesh.send refuses longer text
local KEEP_HEARD = 20
local PAD = 6

-- Codes per category, in code order, for the pickers.
local CAT_CODES = {}
for code in pairs(CODES) do
  local c = code:sub(1, 1)
  CAT_CODES[c] = CAT_CODES[c] or {}
  table.insert(CAT_CODES[c], code)
end
for _, list in pairs(CAT_CODES) do table.sort(list) end

local app = {}
local W, H = 240, 240
local st = { sev = 3, codes = {}, pax = 0, gps = true, note = "", chan = nil }
local heard = {}         -- {from, raw, words, sev}, newest first, this session only
local screen             -- the builder of the screen on display
local cur_cat            -- the category open in the code picker

local function th(size) return ui.text_h and ui.text_h(size) or (size + 4) end
local function btn_h() return th(14) + 14 end
local function lines(text, width, size)
  return ui.text_lines and ui.text_lines(text, width, size) or 1
end

local function go(fn)
  screen = fn
  clear()
  fn()
end

-- ---- encode / decode -------------------------------------------------------

local function has_code(c)
  for i, v in ipairs(st.codes) do if v == c then return i end end
end

local function position()
  local f = sys.gps and sys.gps()
  if not f then return nil end
  return string.format("%.4f,%.4f", f.lat, f.lon)
end

-- MECP/<sev>/<codes> <n>pax P05 <lat>,<lon> <note>. P05 ("At GPS coordinates")
-- always sits right before the coordinates, so a picked P05 is not repeated.
local function encode()
  local parts = {}
  for _, c in ipairs(st.codes) do
    if not (c == "P05" and st.gps) then parts[#parts + 1] = c end
  end
  if st.pax > 0 then parts[#parts + 1] = st.pax .. "pax" end
  if st.gps then
    local pos = position()
    if pos then parts[#parts + 1] = "P05 " .. pos end
  end
  if st.note ~= "" then parts[#parts + 1] = st.note end
  return "MECP/" .. st.sev .. "/" .. table.concat(parts, " ")
end

-- Words for a MECP line, or nil. Finds the message anywhere in the text, so a
-- "name: MECP/..." channel line decodes too. Unknown codes stay as they are.
local function decode(text)
  local sev, rest = tostring(text or ""):match("MECP/(%d)/(.*)$")
  if not sev then return nil end
  sev = tonumber(sev)
  local words, note, at_pos = {}, {}, false
  for tok in rest:gmatch("%S+") do
    if tok == "P05" then
      at_pos = true
    elseif CODES[tok] then
      words[#words + 1] = CODES[tok]
    elseif tok:match("^%u%d%d$") then
      words[#words + 1] = tok
    elseif tok:match("^%d+pax$") then
      words[#words + 1] = tok:match("^(%d+)") .. " people"
    elseif tok:match("^%-?%d+%.?%d*,%-?%d+%.?%d*$") then
      words[#words + 1] = "at " .. tok
      at_pos = false
    elseif tok:match("^@%d%d%d%d$") then
      words[#words + 1] = "at " .. tok:sub(2, 3) .. ":" .. tok:sub(4, 5)
    else
      note[#note + 1] = tok
    end
  end
  if at_pos then words[#words + 1] = CODES.P05 end
  local s = (SEV[sev] or ("Severity " .. sev)) .. ": " .. table.concat(words, ", ")
  if #note > 0 then s = s .. ' "' .. table.concat(note, " ") .. '"' end
  return s, sev
end

-- ---- screens ---------------------------------------------------------------

local compose, pick_severity, pick_category, pick_codes, pick_channel, confirm_mayday, show_heard

local function title(text)
  ui.label(text, PAD, PAD, 14, C.text)
  return PAD + th(14) + 6
end

local function list_screen(heading)
  ui.scroll(false)
  local top = title(heading)
  return ui.list(PAD, top, W - 2 * PAD, H - top - PAD)
end

local function do_send()
  if #st.codes == 0 then sys.toast("Pick at least one code", 1500) return end
  if not st.chan then go(pick_channel) return end
  local text = encode()
  if #text > MAX_LEN then sys.toast("Too long: " .. #text .. " of " .. MAX_LEN, 2000) return end
  local ok, err, wait = mesh.send(st.chan, text)
  if ok then
    sys.toast("Sent to " .. st.chan, 1500)
  elseif err == "rate limited" then
    sys.toast("Sent too often, wait " .. tostring(wait or "a few") .. " s", 2500)
  else
    sys.toast("Not sent: " .. tostring(err), 2500)
  end
end

compose = function()
  ui.scroll(true)
  local bw, bh = W - 2 * PAD, btn_h()
  local half = math.floor((bw - PAD) / 2)
  local y = PAD

  ui.button("Severity: " .. SEV[st.sev], PAD, y, bw, bh, function() go(pick_severity) end)
  y = y + bh + 4
  ui.button(#st.codes == 0 and "Pick codes" or ("Codes: " .. #st.codes), PAD, y, bw, bh,
            function() go(pick_category) end)
  y = y + bh + 4
  if #st.codes > 0 then
    local names = {}
    for _, c in ipairs(st.codes) do names[#names + 1] = c .. " " .. (CODES[c] or "") end
    local text = table.concat(names, "\n")
    local l = ui.label(text, PAD, y, 12, C.sub)
    l:width(bw)
    y = y + lines(text, bw, 12) * th(12) + 4
  end
  ui.button("People: " .. (st.pax > 0 and st.pax or "-"), PAD, y, half, bh, function()
    ui.input("How many people", st.pax > 0 and tostring(st.pax) or "", function(t)
      if t then st.pax = math.max(0, math.min(999, math.floor(tonumber(t) or 0))) end
      go(compose)
    end)
  end)
  ui.button("Position: " .. (st.gps and "on" or "off"), PAD + half + PAD, y, half, bh, function()
    st.gps = not st.gps
    go(compose)
  end)
  y = y + bh + 4
  ui.button(st.note == "" and "Add a note" or ("Note: " .. st.note), PAD, y, bw, bh, function()
    ui.input("Note (plain words)", st.note, function(t)
      if t then st.note = t:gsub("[\r\n]", " "):gsub("^%s+", ""):gsub("%s+$", "") end
      go(compose)
    end)
  end)
  y = y + bh + 4
  ui.button("Channel: " .. (st.chan or "pick one"), PAD, y, bw, bh, function() go(pick_channel) end)
  y = y + bh + 6

  local text = encode()
  local too_long = #text > MAX_LEN
  local pv = ui.label(text, PAD, y, 14, too_long and C.bad or (st.sev == 0 and C.bad or C.accent))
  pv:width(bw)
  y = y + lines(text, bw, 14) * th(14) + 2
  if st.gps and not position() then
    ui.label("No GPS fix: sent without a position", PAD, y, 12, C.sub)
    y = y + th(12) + 2
  end
  y = y + 4

  ui.button(st.sev == 0 and "Send MAYDAY" or "Send", PAD, y, half, bh, function()
    if st.sev == 0 then go(confirm_mayday) else do_send() end
  end)
  ui.button("Decode", PAD + half + PAD, y, half, bh, function()
    ui.input("MECP text", "MECP/", function(t)
      if t then
        local words, sev = decode(t)
        if words then
          table.insert(heard, 1, { from = "Decoded", raw = t, words = words, sev = sev })
          while #heard > KEEP_HEARD do table.remove(heard) end
          go(show_heard)
          return
        end
        sys.toast("Not a MECP message", 1500)
      end
      go(compose)
    end)
  end)
  y = y + bh + 4
  ui.button("Heard (" .. #heard .. ")", PAD, y, half, bh, function() go(show_heard) end)
  ui.button("Start over", PAD + half + PAD, y, half, bh, function()
    st.sev, st.codes, st.pax, st.note = 3, {}, 0, ""
    go(compose)
  end)
end

pick_severity = function()
  local l = list_screen("Severity")
  for s = 0, 3 do
    local i = l:add(SEV[s] .. ", " .. SEV_SUB[s], function() st.sev = s; go(compose) end)
    if s == 0 then l:color(i or (s + 1), C.bad) end
  end
  l:select(st.sev + 1)
end

pick_category = function()
  local l = list_screen("Codes: pick a category")
  l:add("Done", function() go(compose) end)
  for _, cat in ipairs(CATS) do
    local n = 0
    for _, c in ipairs(st.codes) do if c:sub(1, 1) == cat.id then n = n + 1 end end
    local i = l:add(cat.id .. "  " .. cat.name .. (n > 0 and ("  (" .. n .. ")") or ""), function()
      cur_cat = cat
      go(pick_codes)
    end)
    if n > 0 and i then l:color(i, C.accent) end
  end
end

pick_codes = function()
  local l = list_screen(cur_cat.name)
  l:add("Back", function() go(pick_category) end)
  for _, code in ipairs(CAT_CODES[cur_cat.id] or {}) do
    local function text() return (has_code(code) and "[x] " or "[ ] ") .. code .. " " .. CODES[code] end
    local i
    i = l:add(text(), function()
      local at = has_code(code)
      if at then table.remove(st.codes, at) else st.codes[#st.codes + 1] = code end
      if i then
        l:set(i, text())
        l:color(i, has_code(code) and C.accent or C.text)
      end
    end)
    if i and has_code(code) then l:color(i, C.accent) end
  end
end

pick_channel = function()
  local chans = mesh.channels and mesh.channels() or {}
  local l = list_screen("Post to channel")
  if #chans == 0 then l:add("No channels on this device", function() go(compose) end) return end
  for _, name in ipairs(chans) do
    local i = l:add(name, function()
      st.chan = name
      store.set("chan", name)
      go(compose)
    end)
    if name == st.chan and i then l:select(i) end
  end
end

confirm_mayday = function()
  ui.scroll(true)
  local bw, bh = W - 2 * PAD, btn_h()
  local y = title("Send a MAYDAY?")
  local msg = "A MAYDAY asks everyone who hears it for immediate help. It goes to "
           .. tostring(st.chan or "the channel you pick") .. " as:\n" .. encode()
  local l = ui.label(msg, PAD, y, 12, C.text)
  l:width(bw)
  y = y + lines(msg, bw, 12) * th(12) + 8
  ui.button("Send MAYDAY", PAD, y, bw, bh, function() do_send(); go(compose) end)
  y = y + bh + 4
  ui.button("Cancel", PAD, y, bw, bh, function() go(compose) end)
end

show_heard = function()
  ui.scroll(true)
  local bw, bh = W - 2 * PAD, btn_h()
  local y = PAD
  ui.button("Back", PAD, y, bw, bh, function() go(compose) end)
  y = y + bh + 6
  if #heard == 0 then
    local t = "Nothing yet. MECP messages on your channels show up here, decoded, while this app is open."
    local l = ui.label(t, PAD, y, 12, C.sub)
    l:width(bw)
    return
  end
  for _, h in ipairs(heard) do
    local head = h.from .. ": " .. h.words
    local l = ui.label(head, PAD, y, 12, h.sev == 0 and C.bad or C.text)
    l:width(bw)
    y = y + lines(head, bw, 12) * th(12)
    local r = ui.label(h.raw, PAD, y, 12, C.sub)
    r:width(bw)
    y = y + lines(h.raw, bw, 12) * th(12) + 8
  end
end

-- ---- lifecycle -------------------------------------------------------------

function app.on_message(m)
  local words, sev = decode(m.text)
  if not words then return end
  local from = tostring(m.sender or "?")
  if m.channel then from = m.channel .. ", " .. from end
  table.insert(heard, 1, { from = from, raw = tostring(m.text), words = words, sev = sev })
  while #heard > KEEP_HEARD do table.remove(heard) end
  if sev == 0 then sys.toast(from .. ": " .. words, 5000) end
  -- MECP traffic is rare, so a rebuild is cheap: the list grows, or the Heard
  -- count on the compose screen moves.
  if screen == show_heard or screen == compose then go(screen) end
end

function app.on_open(w, h)
  W, H = w, h
  if not mesh.send then
    local l = ui.label("MECP needs a board with the extended app SDK (it posts to channels).",
                       PAD, PAD, 14, C.text)
    l:width(W - 2 * PAD)
    return
  end
  local saved = store.get("chan")
  local chans = mesh.channels and mesh.channels() or {}
  for _, name in ipairs(chans) do
    if name == saved then st.chan = name end
  end
  if not st.chan then
    for _, name in ipairs(chans) do
      if not st.chan and name:lower():find("emergency", 1, true) then st.chan = name end
    end
  end
  go(compose)
end

function app.on_close() end

return app
