-- Cyberspace (Lua) — a client for cyberspace.online, the text-only social
-- network: cIRC rooms, C-Mail and the Feed, over the site's own REST API at
-- api.cyberspace.online. The calls and their JSON shapes follow
-- monster-cyberspace (oumike/monster-cyberspace, src/cs/cs_api.cpp), the
-- standalone ESP32 client.
--
-- Needs wada.net.request (https, request headers, the real status code) and
-- wada.json, which arrived together; caps().request / caps().https say whether
-- this firmware and board have them. Nothing here is pushed: the open room or
-- conversation is polled while it is on screen, and nothing runs once the app
-- is closed.
--
-- Sign-in keeps the refresh token in wada.store (the app's own KV file) so the
-- next open goes straight in; "Stay signed in" on Home turns that off. The
-- password is sent once and never stored. Timestamps from the API are
-- millisecond integers past 32 bits, which wada.json hands over as exact text;
-- they are compared as text and only cut down to seconds for "5m ago".
local ui, sys, net, store, timer, json = wada.ui, wada.sys, wada.net, wada.store, wada.timer, wada.json
local C = ui.colors
local clear = ui.clear or function() end
local app = {}

local API = "https://api.cyberspace.online"
local PAD = 4
local MAX_MSGS = 150           -- per open room/conversation; older ones are dropped
local W, H, LH
local caps = sys.caps and sys.caps() or {}

local id_token, refresh_token, username
-- Every store.get passes a default: on firmware up to beta_101 a get with no
-- default, for a key that is not set, returns the whole store table, not nil.
local remember = store.get("remember", "1") ~= "0"

local function th(size) return ui.text_h and ui.text_h(size) or (size + 4) end
local function btn_h() return th(14) + 14 end
local function lines(text, width, size)
  return ui.text_lines and ui.text_lines(text, width, size) or 1
end

-- ---- screens -----------------------------------------------------------------
-- A screen is a table: draw() builds it from its own state, and optional
-- key(ev) / nav(dir) / leave(). go() switches (and invalidates every reply still
-- in flight for the old screen, via gen); redraw() rebuilds the same one.
local cur, gen, status_lbl, poll = nil, 0, nil, nil
local touches = 0              -- presses on the page so far (on_input "down")
local on_input                 -- app.on_input, set aside on scrolling screens of touchless boards

local function redraw()
  clear()                        -- (also puts a scrolled page back at the top)
  status_lbl = nil
  if ui.scroll then ui.scroll(cur.scroll == true) end
  cur.touch_mark = touches
  cur.draw()
end

local function go(s)
  if poll then poll:stop(); poll = nil end
  if cur and cur.leave then cur.leave() end
  gen = gen + 1
  cur = s
  -- With no touch, the keys drive the page only while the app has no on_input:
  -- the firmware then moves the cursor through the buttons, list rows and
  -- messages, and scrolls when there is nothing further to focus. With an
  -- on_input every key would come here instead and nothing could be reached.
  if caps.touch == false then app.on_input = nil else app.on_input = on_input end
  redraw()
end

local function live(g) return g == gen end

local function set_status(text, color)
  if status_lbl then
    status_lbl:set(text or "")
    status_lbl:color(color or C.sub)
  end
end

-- Trim s to fit px at size, with an ellipsis. Bisects on bytes and backs off
-- to a UTF-8 boundary, so it is a handful of text_w calls, not one per char.
local function fit_line(s, px, size)
  s = s or ""
  if not ui.text_w or ui.text_w(s, size) <= px then return s end
  local lo, hi = 0, #s
  while lo < hi do
    local mid = (lo + hi + 1) // 2
    if ui.text_w(s:sub(1, mid) .. "...", size) <= px then lo = mid else hi = mid - 1 end
  end
  while lo > 0 and ((s:byte(lo + 1) or 0) & 0xC0) == 0x80 do lo = lo - 1 end
  return s:sub(1, lo) .. "..."
end

-- The first n bytes of s, not splitting a UTF-8 character.
local function utf8_cut(s, n)
  if #s <= n then return s end
  while n > 0 and ((s:byte(n + 1) or 0) & 0xC0) == 0x80 do n = n - 1 end
  return s:sub(1, n)
end

local function header(title)
  local sw = W * 2 // 5
  ui.label(fit_line(title, W - sw - 3 * PAD, 14), PAD, PAD, 14, C.text, true)
  status_lbl = ui.label("", W - sw - PAD, PAD + 2, 12, C.sub)
  status_lbl:width(sw, "right")
  return PAD + th(14) + PAD
end

-- A row of equal buttons at y; returns the y below it.
local function button_row(defs, y)
  local n, bh = #defs, btn_h()
  local bw = (W - PAD * (n + 1)) // n
  for k, d in ipairs(defs) do ui.button(d[1], PAD + (k - 1) * (bw + PAD), y, bw, bh, d[2]) end
  return y + bh + PAD
end

-- The same along the bottom (fixed screens); returns the y where they start.
local function buttons(defs)
  local y = H - btn_h() - PAD
  button_row(defs, y)
  return y
end

local function note(text, y, color)
  local l = ui.label(text, PAD, y, 12, color or C.sub)
  l:width(W - 2 * PAD)
  return l
end

-- ---- time --------------------------------------------------------------------
-- Seconds from a millisecond timestamp, which arrives as exact text (or as a
-- number when small). Seconds fit int32 until 2038.
local function ms_to_s(v)
  if type(v) == "string" and #v > 3 then return tonumber(v:sub(1, -4)) end
  if math.type(v) == "integer" then return v // 1000 end
  return nil
end

-- "2026-10-30T12:34:56.789Z" -> Unix seconds (days-from-civil, UTC).
local function iso_to_s(s)
  if type(s) ~= "string" then return nil end
  local y, mo, d, h, mi, se = s:match("^(%d+)-(%d+)-(%d+)T(%d+):(%d+):(%d+)")
  if not y then return nil end
  y, mo, d = tonumber(y), tonumber(mo), tonumber(d)
  if mo <= 2 then y = y - 1 end
  local era = y // 400
  local yoe = y - era * 400
  local doy = (153 * ((mo + 9) % 12) + 2) // 5 + d - 1
  local days = era * 146097 + yoe * 365 + yoe // 4 - yoe // 100 + doy - 719468
  return days * 86400 + tonumber(h) * 3600 + tonumber(mi) * 60 + tonumber(se)
end

local function ago(s)
  local now = sys.epoch and sys.epoch()
  if not now or not s then return "" end
  local d = now - s
  if d < 60 then return "now" end
  if d < 3600 then return (d // 60) .. "m" end
  if d < 86400 then return (d // 3600) .. "h" end
  return (d // 86400) .. "d"
end

-- A ms timestamp (exact decimal text, or a small integer) as a fixed-width
-- key, so ordering is one string compare: what table.sort calls ~n log n times.
local function ts_key(v)
  local t = tostring(v or "0")
  return #t < 20 and (string.rep("0", 20 - #t) .. t) or t
end

-- ---- the API -----------------------------------------------------------------
-- One request in flight (wada.net's rule), so everything goes through a queue.
-- A 401 on a signed-in call puts the call back behind a token refresh, once.
local queue, busy = {}, false
local pump, sign_out

-- The refresh token is kept in pieces: wada.store writes each value as one
-- line of at most 192 bytes, and the token is longer than that.
local RT_PIECE = 120

local function save_token()
  local t = remember and refresh_token or ""
  local n = math.max(0, -(-#t // RT_PIECE))
  local old = tonumber(store.get("rtn", 0)) or 0
  for i = 1, math.max(n, old) do
    store.set("rt" .. i, i <= n and t:sub((i - 1) * RT_PIECE + 1, i * RT_PIECE) or nil)
  end
  store.set("rtn", n > 0 and n or nil)
  store.set("rt", nil)            -- the old single value, cut short when it was saved
end

local function load_token()
  local n, parts = tonumber(store.get("rtn", 0)) or 0, {}
  for i = 1, n do
    local p = store.get("rt" .. i, false)
    if type(p) ~= "string" then return nil end
    parts[i] = p
  end
  return n > 0 and table.concat(parts) or nil
end

local function err_text(status, data)
  if status == -1 then return "No connection" end
  if status == -2 then return "Reply too large" end
  if type(data) == "table" and type(data.error) == "table" and data.error.message then
    return tostring(data.error.message)
  end
  return "Server error " .. tostring(status)
end

local function refresh_item(done)
  return { method = "POST", path = "/v1/auth/refresh", auth = false,
    body = { refreshToken = refresh_token },
    cb = function(ok, d, status)
      if ok and type(d) == "table" and type(d.data) == "table" and d.data.idToken then
        id_token = d.data.idToken
        if d.data.refreshToken and d.data.refreshToken ~= refresh_token then
          refresh_token = d.data.refreshToken
          save_token()
        end
        if done then done(true) end
      elseif status == 400 or status == 401 or status == 403 then
        sign_out("Signed out: " .. tostring(d))
      else
        set_status(tostring(d), C.bad)
        if done then done(false, d) end
      end
    end }
end

pump = function()
  if busy or #queue == 0 then return end
  local it = table.remove(queue, 1)
  local headers = { Accept = "application/json" }
  if it.auth ~= false and id_token then headers.Authorization = "Bearer " .. id_token end
  busy = true
  local ok, e = pcall(net.request, {
    method = it.method, url = API .. it.path, headers = headers,
    body = it.body and json.encode(it.body) or nil, max = it.max or 24576,
  }, function(status, raw)
    busy = false
    local data = (raw and raw ~= "") and json.decode(raw) or nil
    if status == 401 and it.auth ~= false and refresh_token and not it.retried then
      it.retried = true
      table.insert(queue, 1, it)
      table.insert(queue, 1, refresh_item())
    elseif it.cb then
      if status >= 200 and status < 300 then it.cb(true, data, status)
      else it.cb(false, err_text(status, data), status) end
    end
    pump()
  end)
  if not ok then
    busy = false
    if it.cb then it.cb(false, tostring(e), -1) end
  end
end

local function api(method, path, body, cb, max)
  queue[#queue + 1] = { method = method, path = path, body = body, cb = cb, max = max }
  pump()
end

local function enc(s)
  return (tostring(s):gsub("[^%w%-%._~]", function(c) return string.format("%%%02X", c:byte()) end))
end

local function arr(d)
  return type(d) == "table" and type(d.data) == "table" and d.data or {}
end

-- ---- scrolling text ----------------------------------------------------------
-- Chat lines, posts and replies are blocks {head, sub, body, color, dim, press}: a bold
-- name line and a wrapped body, stacked down a page that scrolls (wada.ui.scroll,
-- in every release since beta_59). Draws one at y; returns the y below it.
local function draw_block(b, y)
  local x, w = PAD, W - 2 * PAD
  local hw = ui.text_w and ui.text_w(b.head, 12) or 60
  ui.label(b.head, x, y, 12, b.color or C.accent, true)
  if b.sub and b.sub ~= "" then ui.label(b.sub, x + hw + 8, y, 12, C.sub) end
  y = y + LH
  if b.body ~= "" then
    local l = ui.label(b.body, x, y, 12, b.dim and C.sub or C.text)
    l:width(w)
    -- press(kind): the text can be picked (label:on_press, newer firmware only)
    if b.press and l.on_press then l:on_press(b.press) end
    y = y + lines(b.body, w, 12) * LH
  end
  return y + 4
end

-- True when the page has been pressed since it was drawn: the user may have
-- scrolled down, and a redraw would take them back to the top.
local function reading(s) return touches ~= s.touch_mark end

-- ---- screens: sign-in --------------------------------------------------------
local home, login

local function do_login(email, password)
  set_status("Signing in...")
  local g = gen
  queue[#queue + 1] = { method = "POST", path = "/v1/auth/login", auth = false,
    body = { email = email, password = password },
    cb = function(ok, d)
      if not live(g) then return end
      local t = ok and type(d) == "table" and d.data
      if t and t.idToken and t.refreshToken then
        id_token, refresh_token = t.idToken, t.refreshToken
        save_token()
        go(home())
      else
        set_status(ok and "Unexpected reply" or tostring(d), C.bad)
      end
    end }
  pump()
end

-- The form shows its two fields as rows you tap to fill (wada.ui.input, the
-- firmware's own dialog and keyboard), then Sign in. The password lives in
-- this closure only until it is sent, and is shown as dots.
login = function(msg)
  local s = {}
  local email, password = store.get("email", ""), ""
  function s.draw()
    local y = header("Cyberspace")
    if not net.request or not json then
      note("This app needs newer firmware (wada.net.request and wada.json).", y, C.bad)
      return
    end
    if caps.https == false then
      note("Cyberspace is https only, and this board cannot open https connections.", y, C.bad)
      return
    end
    local intro = "Sign in with your cyberspace.online account."
    note(intro, y, C.text)
    y = y + lines(intro, W - 2 * PAD, 12) * LH + PAD
    local bh, bw = btn_h(), W - 2 * PAD
    ui.button("Email: " .. (email ~= "" and fit_line(email, bw - 70, 14) or "tap to enter"),
      PAD, y, bw, bh, function()
        ui.input("Email", email, function(t)
          if t == nil then return end
          email = t
          if remember then store.set("email", email ~= "" and email or nil) end
          redraw()
          if password == "" and email ~= "" then s.ask_password() end
        end, 128)
      end)
    y = y + bh + PAD
    ui.button("Password: " .. (password ~= "" and string.rep("*", math.min(#password, 12)) or "tap to enter"),
      PAD, y, bw, bh, function() s.ask_password() end)
    y = y + bh + PAD
    note("The password is sent once, over https, and not stored.", y)
    if msg then set_status(msg, C.bad) end
    buttons({ { "Sign in", function()
      if email == "" then set_status("Enter your email", C.bad)
      elseif password == "" then set_status("Enter your password", C.bad)
      else
        local pw = password
        password = ""
        redraw()
        do_login(email, pw)
      end
    end } })
  end
  function s.ask_password()
    ui.input("Password", "", function(t)
      if t == nil then return end
      password = t
      redraw()
    end, 128)
  end
  return s
end

sign_out = function(msg)
  queue = {}                     -- nothing queued for the old session goes out
  id_token, refresh_token, username = nil, nil, nil
  save_token()
  go(login(msg))
end

-- ---- screens: chat (a cIRC room or a C-Mail conversation) --------------------
-- src: { title, load(before, cb(ok, msgs_or_err, cursor)), send(text, cb),
--        read(), poll_ms }
local function circ_text(m)
  if m.deleted then return "(deleted)" end
  local t = m.content or ""
  if m.isEightball and m.eightballAnswer then t = t .. "  => " .. m.eightballAnswer end
  if m.isFortune and m.fortuneText then t = t .. "  " .. m.fortuneText end
  if m.imageUrl or m.gifUrl then t = t .. (t ~= "" and " " or "") .. "[image]" end
  if m.audioAttachment then t = t .. (t ~= "" and " " or "") .. "[audio]" end
  return t
end

local function chat(src)
  -- Newest at the top: a redraw always lands there (the firmware cannot be
  -- asked to scroll), so a room opens on its latest lines on every build.
  local s = { msgs = {}, ids = {}, more = false, loaded = false, scroll = true }
  local loading, fresh = false, 0
  local write

  -- In a room, a long press on someone's message (Enter on it at the cursor,
  -- on a board with no touch) starts a reply addressed to them.
  local function reply_to(who)
    return function(kind)
      if kind == "long" or caps.touch == false then write("@" .. who .. " ") end
    end
  end

  local function block(m)
    local who = m.username or m.senderUsername or "?"
    return { head = (m.isAction and "* " or "") .. who, sub = ago(ms_to_s(m.timestamp)),
             body = circ_text(m), color = (who == username) and C.good or C.accent,
             dim = m.deleted,
             press = src.mention and who ~= "?" and who ~= username and reply_to(who) or nil }
  end

  -- Adds what is new to s.msgs (oldest first); returns how many. Past MAX_MSGS
  -- the oldest go, except on an older load (Load older stops at MAX_MSGS).
  local function merge(list, older)
    local added = 0
    for _, m in ipairs(list) do
      if type(m) == "table" and m.id and not s.ids[m.id] then
        s.ids[m.id] = true
        m.key = ts_key(m.timestamp)
        s.msgs[#s.msgs + 1] = m
        added = added + 1
      end
    end
    if added == 0 then return 0 end
    table.sort(s.msgs, function(a, b) return a.key < b.key end)
    while not older and #s.msgs > MAX_MSGS do
      s.ids[table.remove(s.msgs, 1).id] = nil
      s.more = true
    end
    return added
  end

  local function foot_text()
    if s.more then return "Showing the newest " .. #s.msgs .. "." end
    return "Start of the conversation."
  end

  local fetch
  local function older()
    if loading or not s.more or #s.msgs >= MAX_MSGS then return end
    if s.foot then s.foot:set("Loading...") end
    fetch(true)
  end

  -- The end of the page: Load older, or why there is nothing below.
  local function foot(y)
    if s.more and #s.msgs < MAX_MSGS then
      s.foot = ui.button("Load older", PAD, y, W - 2 * PAD, btn_h(), older)
    else
      s.foot = note(foot_text(), y)
    end
  end

  fetch = function(older_load)
    if loading then return end
    loading = true
    local g = gen
    local oldest = s.msgs[1]
    local before = older_load and oldest and tostring(oldest.timestamp) or nil
    src.load(before, function(ok, list, cursor)
      loading = false
      if not live(g) then return end
      if not ok then
        set_status(tostring(list), C.bad)
        if older_load and s.foot then s.foot:set("Load older") end
        return
      end
      local more = cursor ~= nil and cursor ~= "" and #list > 0
      if older_load then
        s.more = more
        local added = merge(list, true)
        if added == 0 then s.more = false end
        set_status("")
        -- They belong below what is on screen. Added there, not redrawn: a
        -- redraw would take the reader back up to the top.
        if not (s.y and s.foot and oldest) then redraw(); return end
        local k, y = 0, s.y
        for _, m in ipairs(s.msgs) do if m.key < oldest.key then k = k + 1 end end
        for i = k, 1, -1 do y = draw_block(block(s.msgs[i]), y) end
        s.y = y
        s.foot:pos(PAD, y)
        s.foot:set((s.more and #s.msgs < MAX_MSGS) and "Load older" or foot_text())
        return
      end
      if not s.loaded then s.more = more end
      local first = not s.loaded
      s.loaded = true
      local added = merge(list, false)
      local force = s.force
      s.force = false
      if added == 0 and not first and not force then
        if fresh == 0 then set_status("") end
        return
      end
      -- New lines go on top. Someone scrolled down reading is told, not moved.
      if added > 0 and not first and not force and reading(s) then
        fresh = fresh + added
        set_status(fresh .. " new: Refresh", C.accent)
        return
      end
      fresh = 0
      redraw()
    end)
  end

  local function refresh() s.force = true; fetch(false) end

  write = function(start)
    ui.input(start and ("Reply in " .. src.title) or src.title, start or "", function(text)
      if not text or text:match("^%s*$") or (start and text:match("^%s*(.-)%s*$") == start:match("^%s*(.-)%s*$")) then return end
      set_status("Sending...")
      local g = gen
      src.send(text, function(ok, reply)
        if not live(g) then return end
        if not ok then set_status(tostring(reply), C.bad); return end
        if type(reply) == "string" and reply ~= "" then sys.toast(reply) end
        set_status("")
        refresh()
      end)
    end, 1000)
  end

  function s.draw()
    local y = header(src.title)
    y = button_row({ { "Back", src.back }, { "Write", function() write() end }, { "Refresh", refresh } }, y)
    s.y, s.foot = nil, nil
    if #s.msgs == 0 then
      note(s.loaded and "No messages yet." or "Loading...", y)
      return
    end
    for i = #s.msgs, 1, -1 do y = draw_block(block(s.msgs[i]), y) end
    s.y = y
    foot(y)
  end

  s.back = src.back
  function s.key(ev)
    if ev.key == "w" or ev.key == "enter" then write()
    elseif ev.key == "r" then refresh() end
  end

  -- first load once the screen exists, then poll while it is open
  timer.after(1, function()
    if cur ~= s then return end
    fetch(false)
    if src.read then src.read() end
    poll = timer.every(src.poll_ms, function()
      if cur == s and not busy then fetch(false) end
    end)
  end)
  return s
end

local function room(r, back)
  return chat({
    title = "#" .. (r.slug or r.name or "?"),   -- slug is the channel; name is its title
    poll_ms = 8000,
    mention = true,
    back = back,
    load = function(before, cb)
      local path = "/v1/circ/" .. enc(r.slug) .. "?limit=30"
      if before then path = path .. "&before=" .. enc(before) end
      api("GET", path, nil, function(ok, d)
        if not ok then return cb(false, d) end
        cb(true, arr(d), d.cursor)
      end, 32768)
    end,
    send = function(text, cb)
      api("POST", "/v1/circ/" .. enc(r.slug), { content = text }, function(ok, d)
        if not ok then return cb(false, d) end
        -- reply: an optional line from the server (a bot answer); usually absent
        local reply = type(d) == "table" and type(d.data) == "table" and d.data.reply
        cb(true, type(reply) == "string" and reply or nil)
      end)
    end,
    read = function() api("POST", "/v1/circ/" .. enc(r.slug) .. "/read") end,
  })
end

local function conversation(c, back)
  return chat({
    title = "@" .. (c.otherUser and c.otherUser.username or "?"),
    poll_ms = 12000,
    back = back,
    load = function(before, cb)
      local path = "/v1/cmail/" .. enc(c.conversationId) .. "?limit=30"
      if before then path = path .. "&before=" .. enc(before) end
      api("GET", path, nil, function(ok, d)
        if not ok then return cb(false, d) end
        cb(true, arr(d), d.cursor)
      end, 32768)
    end,
    send = function(text, cb)
      api("POST", "/v1/cmail/" .. enc(c.conversationId), { content = text }, function(ok, d)
        cb(ok, not ok and d or nil)
      end)
    end,
    read = function() api("POST", "/v1/cmail/" .. enc(c.conversationId) .. "/read") end,
  })
end

-- ---- screens: lists ----------------------------------------------------------
-- header + a list + buttons; fill(list, g) adds the rows (and may fetch).
local function list_screen(title, defs, fill)
  local s = { back = defs[1] and defs[1][1] == "Back" and defs[1][2] or nil }
  function s.draw()
    local top = header(title)
    local bottom = buttons(defs)
    local l = ui.list(PAD, top, W - 2 * PAD, bottom - top - PAD)
    fill(l, gen)
  end
  return s
end

local function rooms()
  local data
  local s
  local function load()
    set_status("Loading...")
    local g = gen
    api("GET", "/v1/circ", nil, function(ok, d)
      if not live(g) then return end
      if not ok then set_status(d, C.bad); return end
      data = arr(d)
      table.sort(data, function(a, b) return (a.sortOrder or 0) < (b.sortOrder or 0) end)
      redraw()
    end)
  end
  s = list_screen("cIRC rooms", { { "Back", function() go(home()) end }, { "Refresh", load } },
    function(l)
      if not data then l:add("Loading..."); return end
      for _, r in ipairs(data) do
        local n = r.onlineCount and r.onlineCount > 0 and ("  (" .. r.onlineCount .. " here)") or ""
        l:add("#" .. (r.slug or r.name or "?") .. n, function() go(room(r, function() go(s) end)) end)
      end
      if #data == 0 then l:add("No rooms") end
    end)
  timer.after(1, function() if cur == s then load() end end)
  return s
end

local function mail()
  local data
  local s
  local function load()
    set_status("Loading...")
    local g = gen
    api("GET", "/v1/cmail", nil, function(ok, d)
      if not live(g) then return end
      if not ok then set_status(d, C.bad); return end
      data = arr(d)
      redraw()
    end)
  end
  local function new_conv()
    ui.input("Write to (username)", "", function(name)
      if not name or name == "" then return end
      name = name:gsub("^@", "")
      set_status("Opening...")
      local g = gen
      api("POST", "/v1/cmail", { recipientUsername = name }, function(ok, d)
        if not live(g) then return end
        local c = ok and type(d) == "table" and d.data
        if type(c) == "table" and c.conversationId then
          go(conversation(c, function() go(s) end))
        else
          set_status(ok and "Unexpected reply" or d, C.bad)
        end
      end)
    end, 40)
  end
  s = list_screen("C-Mail", { { "Back", function() go(home()) end }, { "New", new_conv },
                              { "Refresh", load } },
    function(l)
      if not data then l:add("Loading..."); return end
      for _, c in ipairs(data) do
        local who = c.otherUser and c.otherUser.username or "?"
        local unread = (c.unreadCount or 0) > 0 and ("(" .. c.unreadCount .. ") ") or ""
        local last = utf8_cut((c.lastMessage or ""):gsub("%s+", " "), 90)
        local k = l:add(unread .. "@" .. who .. "  " .. last, function()
          go(conversation(c, function() go(s) end))
        end)
        if unread ~= "" then l:color(k, C.accent) end
      end
      if #data == 0 then l:add("No conversations yet: New starts one") end
    end)
  timer.after(1, function() if cur == s then load() end end)
  return s
end

-- ---- screens: feed and a post ------------------------------------------------
local function post_body(p)
  local t = ""
  if p.title and p.title ~= "" then t = p.title .. "\n\n" end
  if p.contentWarning and p.contentWarning ~= "" then t = t .. "CW: " .. p.contentWarning .. "\n\n" end
  return t .. (p.deleted and "(deleted)" or (p.content or ""))
end

local function post(p, back)
  -- The post, then its replies in order, down one scrolling page.
  local s = { replies = {}, cursor = nil, loaded = false, scroll = true }
  local loading = false

  local function post_block()
    return { head = "@" .. (p.authorUsername or "?"),
             sub = ago(iso_to_s(p.createdAt)) .. "  " .. (p.repliesCount or 0) .. " replies",
             body = post_body(p) }
  end
  local function reply_block(r)
    local head = "@" .. (r.authorUsername or "?")
    if r.parentReplyAuthor and r.parentReplyAuthor ~= "" then head = head .. " > @" .. r.parentReplyAuthor end
    return { head = head, sub = ago(iso_to_s(r.createdAt)),
             body = r.deleted and "(deleted)" or (r.content or ""),
             color = (r.authorUsername == username) and C.good or C.accent, dim = r.deleted }
  end

  local load_more
  local function foot(y)
    if s.cursor then
      s.foot = ui.button("More replies", PAD, y, W - 2 * PAD, btn_h(), function() load_more() end)
    else
      s.foot = note(s.loaded and (#s.replies == 0 and "No replies yet." or "End of the replies.")
                    or "Loading replies...", y)
    end
  end

  load_more = function()
    if loading then return end
    loading = true
    set_status("Loading replies...")
    local g = gen
    local path = "/v1/posts/" .. enc(p.postId) .. "/replies?limit=20"
    if s.cursor then path = path .. "&cursor=" .. enc(s.cursor) end
    api("GET", path, nil, function(ok, d)
      loading = false
      if not live(g) then return end
      if not ok then set_status(d, C.bad); return end
      local from = #s.replies
      for _, r in ipairs(arr(d)) do s.replies[#s.replies + 1] = r end
      s.cursor = (type(d.cursor) == "string" and d.cursor ~= "") and d.cursor or nil
      local first = not s.loaded
      s.loaded = true
      set_status("")
      -- The first batch redraws (the page is still at the top); later ones go
      -- in below, so the reader is not taken back up.
      if first or not (s.y and s.foot and s.foot_btn) then redraw(); return end
      local y = s.y
      for i = from + 1, #s.replies do y = draw_block(reply_block(s.replies[i]), y) end
      s.y = y
      s.foot:pos(PAD, y)
      if not s.cursor then s.foot:set("End of the replies.") end
    end, 32768)
  end

  local function reply()
    ui.input("Reply to @" .. (p.authorUsername or "?"), "", function(text)
      if not text or text == "" then return end
      set_status("Sending...")
      local g = gen
      api("POST", "/v1/replies", { postId = p.postId, content = text }, function(ok, d)
        if not live(g) then return end
        if not ok then set_status(d, C.bad); return end
        p.repliesCount = (p.repliesCount or 0) + 1
        -- reload from the start so the new reply lands in order
        s.replies, s.cursor, s.loaded = {}, nil, false
        load_more()
      end)
    end, 2000)
  end

  function s.draw()
    local y = header(p.title and p.title ~= "" and p.title or ("@" .. (p.authorUsername or "?")))
    y = button_row({ { "Back", back }, { "Reply", reply } }, y)
    y = draw_block(post_block(), y)
    for _, r in ipairs(s.replies) do y = draw_block(reply_block(r), y) end
    s.y = y
    foot(y)
    s.foot_btn = s.cursor ~= nil
  end
  s.back = back
  function s.key(ev) if ev.key == "w" or ev.key == "enter" then reply() end end

  timer.after(1, function() if cur == s then load_more() end end)
  return s
end

local function feed()
  local data, cursor
  local s
  local function load(more)
    set_status("Loading...")
    local g = gen
    local path = "/v1/posts?limit=10"
    if more and cursor then path = path .. "&cursor=" .. enc(cursor) end
    api("GET", path, nil, function(ok, d)
      if not live(g) then return end
      if not ok then set_status(d, C.bad); return end
      if not more then data = {} end
      for _, p in ipairs(arr(d)) do data[#data + 1] = p end
      cursor = (type(d.cursor) == "string" and d.cursor ~= "") and d.cursor or nil
      set_status("")
      redraw()
    end, 32768)
  end
  local function new_post()
    ui.input("Title (optional)", "", function(title)
      if title == nil then return end
      ui.input("Post", "", function(body)
        if not body or body == "" then return end
        local b = { content = body }
        if title ~= "" then b.title = title end
        set_status("Posting...")
        local g = gen
        api("POST", "/v1/posts", b, function(ok, d)
          if not live(g) then return end
          if not ok then set_status(d, C.bad); return end
          sys.toast("Posted")
          load(false)
        end)
      end, 2000)
    end, 100)
  end
  s = list_screen("Feed", { { "Back", function() go(home()) end }, { "Post", new_post },
                            { "Refresh", function() load(false) end } },
    function(l)
      if not data then l:add("Loading..."); return end
      for _, p in ipairs(data) do
        local what = (p.title and p.title ~= "") and p.title or (p.content or "")
        what = utf8_cut(what:gsub("[%s#*_>`]+", " "), 90)
        local meta = "  @" .. (p.authorUsername or "?") .. " " .. ago(iso_to_s(p.createdAt))
        if (p.repliesCount or 0) > 0 then meta = meta .. " (" .. p.repliesCount .. ")" end
        l:add(what .. meta, function() go(post(p, function() go(s) end)) end)
      end
      if cursor then l:add("Load more...", function() load(true) end) end
      if #data == 0 then l:add("Nothing here yet") end
    end)
  timer.after(1, function() if cur == s then load(false) end end)
  return s
end

-- ---- screens: home -----------------------------------------------------------
home = function()
  local unread
  local s = {}
  function s.draw()
    local top = header("Cyberspace")
    local l = ui.list(PAD, top, W - 2 * PAD, H - top - PAD)
    l:add("cIRC rooms", function() go(rooms()) end)
    l:add("C-Mail" .. ((unread or 0) > 0 and ("  (" .. unread .. " unread)") or ""),
          function() go(mail()) end)
    l:add("Feed", function() go(feed()) end)
    l:add("Stay signed in: " .. (remember and "yes" or "no"), function()
      remember = not remember
      store.set("remember", remember and "1" or "0")
      if not remember then store.set("email", nil) end
      save_token()
      redraw()
    end)
    l:add("Sign out", function() sign_out() end)
    set_status(username and ("@" .. username) or "")
  end
  timer.after(1, function()
    if cur ~= s then return end
    local g = gen
    if not username then
      api("GET", "/v1/users/me", nil, function(ok, d)
        if not live(g) or not ok or type(d) ~= "table" then return end
        username = (type(d.data) == "table" and d.data.username) or d.username
        set_status(username and ("@" .. username) or "")
      end)
    end
    api("GET", "/v1/cmail", nil, function(ok, d)
      if not live(g) or not ok then return end
      local n = 0
      for _, c in ipairs(arr(d)) do n = n + (c.unreadCount or 0) end
      unread = n
      if wada.widget and caps.widget then
        wada.widget.set({ title = "Cyberspace", value = tostring(n),
                          line = n == 1 and "unread C-Mail" or "unread C-Mails" })
      end
      if n > 0 then redraw() end
    end, 32768)
  end)
  return s
end

-- ---- lifecycle ---------------------------------------------------------------
function app.on_open(w, h)
  W, H = w, h
  LH = th(12)
  refresh_token = remember and load_token() or nil
  if refresh_token and net.request and json then
    go(login())
    set_status("Signing in...")
    queue[#queue + 1] = refresh_item(function(ok) if ok then go(home()) end end)
    pump()
  else
    go(login())
  end
end

on_input = function(ev)
  if not cur then return end
  if ev.type == "down" then touches = touches + 1; return end
  if ev.type == "key" then
    if (ev.key == "esc" or ev.key == "backspace") and cur.back then cur.back()
    elseif cur.key then cur.key(ev) end
  end
end
app.on_input = on_input

function app.on_close()
  if poll then poll:stop(); poll = nil end
end

return app
