-- Host test for deploy/apps/cyberspace: a mock of wada.* that keeps the device's
-- rules (100k-instruction budget per callback, one fetch in flight, store types)
-- and a fake api.cyberspace.online. Run through run.sh. NMSG=230 makes the room
-- bigger than the app's 150-message window, to exercise where Load older stops.
-- TOUCH=0 runs it as a board with no touchscreen.
local APP, BW, BH = arg[1], tonumber(arg[2] or 320), tonumber(arg[3] or 196)
local CHARW = 7
local BUDGET = 100000

-- ---------- budget-guarded calls (mirrors LuaAppHost guardedCall) ----------
local peak = 0
local function guarded(fn, ...)
  local count = 0
  debug.sethook(function() count = count + 1000; if count > BUDGET then error("instruction budget exceeded") end end, "", 1000)
  local ok, e = pcall(fn, ...)
  debug.sethook()
  if count > peak then peak = count end
  if not ok then error("callback failed: " .. tostring(e), 0) end
end

-- ---------- UI mock ----------
local W_ = { labels = {}, buttons = {}, lists = {} }
local scroll_on = false
local function reset() W_ = { labels = {}, buttons = {}, lists = {} } end
local function lines_of(text, width)
  local cpl = math.max(1, width // CHARW)
  local n = 0
  for para in (text .. "\n"):gmatch("([^\n]*)\n") do
    n = n + math.max(1, (#para + cpl - 1) // cpl)
  end
  return n
end
local function label(text, x, y, size, color, bold)
  assert(type(text) == "string", "label text must be a string")
  local h = { text = text, x = x, y = y, size = size, color = color, w = nil }
  function h:set(t) assert(type(t) == "string"); self.text = t end
  function h:color(c) assert(type(c) == "number", "color must be a number"); self.c = c end
  function h:width(px) self.w = px end
  function h:pos(px, py) self.x, self.y = px, py end
  function h:on_press(fn) assert(type(fn) == "function"); self.press = fn end
  W_.labels[#W_.labels + 1] = h
  return h
end
local inputs, timers, requests, toasts = {}, {}, {}, {}
local now_ms = 0
local C = { text = 1, sub = 2, bg = 3, panel = 4, accent = 5, good = 6, bad = 7, warn = 8, border = 9 }
local store_t = {}
-- Real refresh tokens run past the 192-byte line wada.store saves.
local RT = "r1" .. string.rep("x", 298)

wada = {
  ui = {
    colors = C,
    label = label,
    button = function(t, x, y, w, h, fn)
      assert(type(fn) == "function")
      local b = label(t, x, y, 14, C.text)
      b.fn, b.bw, b.bh = fn, w, h
      W_.buttons[t] = b
      return b
    end,
    list = function(x, y, w, h)
      local l = { rows = {}, x = x, y = y, w = w, h = h }
      function l:add(t, fn) assert(type(t) == "string"); self.rows[#self.rows + 1] = { text = t, fn = fn }; return #self.rows end
      function l:color(i, c) assert(self.rows[i] and type(c) == "number") end
      function l:set(i, t) self.rows[i].text = t end
      W_.lists[#W_.lists + 1] = l
      return l
    end,
    clear = reset,
    input = function(title, init, cb, max) inputs[#inputs + 1] = { title = title, init = init, cb = cb, max = max } end,
    text_w = function(s, size) assert(type(s) == "string" or type(s) == "number", "bad argument #1 to text_w: " .. type(s)); return #tostring(s) * CHARW end,
    text_h = function(size) return size + 4 end,
    text_lines = function(t, w, size) return lines_of(t, w) end,
    scroll = function(on) scroll_on = on ~= false end,
  },
  sys = {
    caps = function() return { https = true, request = true, widget = false, touch = os.getenv("TOUCH") ~= "0" } end,
    epoch = function() return 1793365570 end,
    toast = function(m) assert(type(m) == "string" or type(m) == "number", "bad argument #1 to toast: " .. type(m)); toasts[#toasts + 1] = m end,
    millis = function() return now_ms end,
  },
  -- Released firmware (up to beta_101): a get with no default for a missing
  -- key returns the store table itself, so the app must always pass one.
  store = { get = function(k, ...) local v = store_t[k]; if v ~= nil then return v end
                  if select("#", ...) == 0 then return store_t end; return (...) end,
            set = function(k, v) assert(v == nil or type(v) == "string" or type(v) == "number", "store type")
              -- the firmware writes "key=value\n" into a 192-byte line
              assert(v == nil or #k + #tostring(v) + 2 < 192, "store value too long for one line: " .. k)
              store_t[k] = v end },
  timer = {
    after = function(ms, fn) timers[#timers + 1] = { at = now_ms + ms, fn = fn }; return { stop = function() end } end,
    every = function(ms, fn)
      local t = { at = now_ms + ms, every = ms, fn = fn }
      function t:stop() self.dead = true end
      timers[#timers + 1] = t
      return t
    end,
  },
  json = JSON,
}

local in_flight = false
wada.net = { request = function(opts, cb)
  assert(type(opts) == "table" and type(cb) == "function")
  if in_flight then error("a fetch is already running") end
  assert(opts.url:sub(1, 8) == "https://", "https")
  for k, v in pairs(opts.headers or {}) do assert(type(k) == "string" and type(v) == "string", "header types") end
  in_flight = true
  requests[#requests + 1] = { opts = opts, cb = cb }
end }

-- ---------- fake server ----------
local S = { valid = { t1 = true }, logins = 0, refreshes = 0, sent = {}, posts = {}, replies = {} }
local function big(n) return string.format("%d", n) end   -- host Lua is 32-bit too: build as text
local function msg_json(i, user, text)
  -- timestamp as a raw JSON number past 32 bits
  return string.format('{"id":"m%d","username":"%s","content":%s,"timestamp":1793%06d000}',
    i, user, JSON.encode(text), 300000 + i)
end
local room_msgs = {}
for i = 1, (tonumber(os.getenv("NMSG")) or 75) do room_msgs[i] = { i, i % 3 == 0 and "me" or ("user" .. i % 5),
  (i % 7 == 0) and string.rep("long words here ", 30) .. "end" .. i or ("message " .. i) } end
local long_post = {}
for i = 1, 70 do long_post[i] = "Lorem ipsum dolor sit amet " .. i .. ", consectetur adipiscing elit." end
long_post = table.concat(long_post, i == 35 and "\n\n" or " ") .. " THE END"

local function route(method, path, body, auth)
  local tok = auth and auth:match("^Bearer (.+)$")
  local function need() return tok and S.valid[tok] end
  if method == "POST" and path == "/v1/auth/login" then
    S.logins = S.logins + 1
    local b = JSON.decode(body)
    if b.email == "a@b.c" and b.password == "pw" then
      return 200, '{"data":{"idToken":"t1","refreshToken":"' .. RT .. '","rtdbUrl":"https://x"}}'
    end
    return 401, '{"error":{"code":"INVALID","message":"Wrong email or password"}}'
  end
  if method == "POST" and path == "/v1/auth/refresh" then
    S.refreshes = S.refreshes + 1
    local b = JSON.decode(body)
    if b.refreshToken == RT then S.valid["t" .. (S.refreshes + 1)] = true
      return 200, '{"data":{"idToken":"t' .. (S.refreshes + 1) .. '","rtdbUrl":"https://x"}}' end
    return 401, '{"error":{"code":"BAD_REFRESH","message":"Session expired"}}'
  end
  if not need() then return 401, '{"error":{"code":"UNAUTHORIZED","message":"Authentication required"}}' end
  if path == "/v1/users/me" then return 200, '{"data":{"username":"me"}}' end
  if path == "/v1/circ" then
    return 200, '{"data":[{"id":"r2","slug":"tech","name":"tech","sortOrder":2,"onlineCount":0},'
      .. '{"id":"r1","slug":"lobby","name":"lobby","sortOrder":1,"onlineCount":4}]}'
  end
  local slug, q = path:match("^/v1/circ/(%w+)%?(.*)$")
  if method == "GET" and slug then
    local before = q:match("before=(%d+)")
    local out, hi = {}, #room_msgs
    if before then
      hi = 0
      for k, m in ipairs(room_msgs) do
        local ts = string.format("1793%06d000", 300000 + m[1])
        if ts == before then hi = k - 1 end
      end
    end
    local lo = math.max(1, hi - 29)
    for k = hi, lo, -1 do out[#out + 1] = msg_json(room_msgs[k][1], room_msgs[k][2], room_msgs[k][3]) end -- newest first
    local cur = lo > 1 and string.format('"1793%06d000"', 300000 + room_msgs[lo][1]) or "null"
    return 200, '{"data":[' .. table.concat(out, ",") .. '],"cursor":' .. cur .. '}'
  end
  if method == "POST" and path:match("^/v1/circ/%w+$") then
    local b = JSON.decode(body)
    S.sent[#S.sent + 1] = b.content
    room_msgs[#room_msgs + 1] = { #room_msgs + 1, "me", b.content }
    -- the real API leaves "reply" out unless a command answered
    return 201, b.content == "/help" and '{"data":{"messageId":"x","reply":"Commands: /me"}}' or '{"data":{"messageId":"x"}}'
  end
  if path:match("^/v1/circ/%w+/read$") then return 204, "" end
  if path == "/v1/cmail" and method == "GET" then
    return 200, '{"data":[{"conversationId":"c1","otherUser":{"userId":"u2","username":"zed"},'
      .. '"lastMessage":"hey\\nthere","lastMessageAt":1793365000000,"unreadCount":2}]}'
  end
  if path == "/v1/cmail" and method == "POST" then
    local b = JSON.decode(body)
    return 201, '{"data":{"conversationId":"c9","otherUser":{"userId":"u9","username":"' .. b.recipientUsername .. '"}}}'
  end
  if path:match("^/v1/cmail/c%d+%?") then
    return 200, '{"data":[{"id":"x1","senderUsername":"zed","content":"hi","timestamp":1793365000000}],"cursor":null}'
  end
  if path:match("^/v1/cmail/c%d+$") and method == "POST" then return 201, '{"data":{"messageId":"y"}}' end
  if path:match("^/v1/cmail/c%d+/read$") then return 204, "" end
  if path:match("^/v1/posts%?") then
    local more = path:match("cursor=")
    local items = {}
    for i = more and 11 or 1, more and 12 or 10 do
      items[#items + 1] = string.format('{"postId":"p%d","authorUsername":"au%d","title":%s,"content":%s,'
        .. '"createdAt":"2026-10-30T10:%02d:00.000Z","repliesCount":%d,"editedAt":null}',
        i, i, i == 1 and '"A long one"' or '""', JSON.encode(i == 1 and long_post or ("short post #" .. i .. " with **md**")), i, i == 1 and 25 or 0)
    end
    return 200, '{"data":[' .. table.concat(items, ",") .. '],"cursor":' .. (more and 'null' or '"c10"') .. '}'
  end
  if path:match("^/v1/posts/p1/replies") then
    local second = path:match("cursor=")
    local items = {}
    for i = second and 21 or 1, second and 25 or 20 do
      items[#items + 1] = string.format('{"replyId":"r%d","postId":"p1","authorUsername":"rr%d","content":"reply %d","createdAt":"2026-10-30T11:00:00Z"}', i, i, i)
    end
    return 200, '{"data":[' .. table.concat(items, ",") .. '],"cursor":' .. (second and 'null' or '"rc"') .. '}'
  end
  if path == "/v1/replies" then S.replies[#S.replies + 1] = JSON.decode(body); return 201, '{"data":{"replyId":"new"}}' end
  if path == "/v1/posts" and method == "POST" then S.posts[#S.posts + 1] = JSON.decode(body); return 201, '{"data":{"postId":"new"}}' end
  return 404, '{"error":{"code":"NOT_FOUND","message":"No route ' .. method .. ' ' .. path .. '"}}'
end

-- ---------- driver ----------
local app
local log = {}
local function serve_one()
  local r = table.remove(requests, 1)
  if not r then return false end
  local o = r.opts
  local path = o.url:gsub("^https://api.cyberspace.online", "")
  local st, body = route(o.method or "GET", path, o.body, o.headers and o.headers.Authorization)
  log[#log + 1] = (o.method or "GET") .. " " .. path .. " -> " .. st
  in_flight = false
  guarded(r.cb, st, body)
  return true
end
local function run_timers(advance)
  now_ms = now_ms + (advance or 0)
  local fired = true
  while fired do
    fired = false
    for i, t in ipairs(timers) do
      if not t.dead and t.at <= now_ms then
        if t.every then t.at = now_ms + t.every else t.dead = true end
        guarded(t.fn)
        fired = true
        break
      end
    end
  end
end
local function settle(advance)
  for _ = 1, 50 do
    run_timers((advance or 0) + 1); advance = 0
    if not serve_one() then
      run_timers(1)
      if #requests == 0 then break end
    end
  end
end
local function answer(text)
  local i = table.remove(inputs, 1)
  assert(i, "no input prompt open")
  guarded(i.cb, text)
  settle()
end
local function press(name)
  local b = W_.buttons[name]
  assert(b, "no button " .. name)
  guarded(b.fn); settle()
end
local function tap(prefix)
  for name, b in pairs(W_.buttons) do
    if name:sub(1, #prefix) == prefix then guarded(b.fn); settle(); return name end
  end
  error("no button starting " .. prefix)
end
local function row(pat)
  for _, l in ipairs(W_.lists) do
    for _, r in ipairs(l.rows) do if r.text:find(pat, 1, true) then assert(r.fn, "row has no fn: " .. r.text); guarded(r.fn); settle(); return end end
  end
  error("no row " .. pat)
end
local function rows()
  local t = {}
  for _, l in ipairs(W_.lists) do for _, r in ipairs(l.rows) do t[#t + 1] = r.text end end
  return t
end
local function texts()
  local t = {}
  for _, l in ipairs(W_.labels) do if not l.bw then t[#t + 1] = l.text end end
  return table.concat(t, " | ")
end
local function status() for _, l in ipairs(W_.labels) do if l.w and l.x > BW / 3 and l.y < 12 then return l.text end end end
-- Body text (labels below the top rows) must not overlap: each starts below
-- the one before it ends. Buttons too, on a scrolling page.
local function body()
  local t = {}
  for _, l in ipairs(W_.labels) do if l.y > 20 then t[#t + 1] = l end end
  table.sort(t, function(a, b) return a.y < b.y or (a.y == b.y and a.x < b.x) end)
  return t
end
local function check_layout()
  local prev
  for _, l in ipairs(body()) do
    local h = l.bh or (l.w and lines_of(l.text, l.w) or 1) * 16
    if prev and l.y ~= prev.y then
      assert(l.y >= prev.y + prev.h, string.format("overlap: y=%d after y=%d h=%d %q", l.y, prev.y, prev.h, l.text:sub(1, 30)))
    end
    if not prev or l.y ~= prev.y then prev = { y = l.y, h = h } end
  end
end
-- message numbers on the page, top to bottom
local function order()
  local t = {}
  for _, l in ipairs(body()) do
    local n = l.text:match("^message (%d+)$") or l.text:match("end(%d+)$")
    if n then t[#t + 1] = tonumber(n) end
  end
  return t
end

local chunk = assert(loadfile(APP))
app = chunk()
guarded(app.on_open, BW, BH)
settle()
print("[open] " .. texts())
assert(W_.buttons["Sign in"], "login screen")

-- wrong password, then right
-- Sign in with nothing filled in says what is missing
press("Sign in")
assert(status() == "Enter your email")
-- the fields are rows on the page; filling the email goes on to the password
tap("Email: tap to enter"); answer("a@b.c"); answer("nope")
assert(W_.buttons["Email: a@b.c"] and W_.buttons["Password: ****"], "fields show what was entered")
press("Sign in")
print("[bad login] status=" .. tostring(status()))
assert(status() == "Wrong email or password")
assert(W_.buttons["Password: tap to enter"], "a failed sign-in clears the password")
tap("Email: "); assert(inputs[1].init == "a@b.c", "email prefilled"); answer("a@b.c")
assert(#inputs == 1 and inputs[1].title == "Password", "an empty password is asked for next")
answer("pw")
press("Sign in")
assert(#inputs == 0, "no prompt left open")
print("[home] rows=" .. table.concat(rows(), " / ") .. " status=" .. tostring(status()))
assert(store_t.rt == nil and store_t.rtn == 3, "refresh token kept, in pieces")
assert(store_t.rt1 .. store_t.rt2 .. store_t.rt3 == RT, "pieces rebuild the token")
assert(rows()[2]:find("2 unread"), "unread count on home")
assert(status() == "@me")

-- rooms, sorted by sortOrder
row("cIRC")
print("[rooms] " .. table.concat(rows(), " / "))
assert(rows()[1]:find("#lobby") and rows()[2]:find("#tech"))
row("#lobby")
print("[room] " .. texts():sub(1, 200))
assert(scroll_on, "the room page scrolls")
assert(app.on_input == nil or os.getenv("TOUCH") ~= "0", "no on_input on a touchless scrolling page")
check_layout()
local NM = (tonumber(os.getenv("NMSG")) or 75)
local o = order()
assert(o[1] == NM and #o == 30, "newest at the top, 30 loaded")
for k = 2, #o do assert(o[k] == o[k - 1] - 1, "newest first, in order") end
-- Load older adds below without redrawing (a redraw scrolls to the top)
local first_label = body()[1]
local loads = 0
while W_.buttons["Load older"] and W_.buttons["Load older"].text == "Load older" do
  press("Load older"); loads = loads + 1
  assert(body()[1] == first_label, "older load did not redraw the page")
  check_layout()
  assert(loads < 20)
end
o = order()
print("[older] loads=" .. loads .. " held=" .. #o .. " foot=" .. tostring(W_.buttons["Load older"] and W_.buttons["Load older"].text))
for k = 2, #o do assert(o[k] == o[k - 1] - 1, "still in order after loading older") end
if NM > 150 then
  assert(#o == 150 and W_.buttons["Load older"].text == "Showing the newest 150.", "older stops at 150")
else
  assert(#o == NM and o[#o] == 1 and W_.buttons["Load older"].text == "Start of the conversation.", "every message reachable")
end

-- pick someone's message: a long press (Enter at the cursor, touchless) replies to them
local picked
for _, l in ipairs(body()) do if l.press then picked = l; break end end
assert(picked, "messages can be picked")
local who
for _, l in ipairs(body()) do if l.y == picked.y - 16 and l.text:match("^user%d$") then who = l.text end end
assert(who, "picked a message by " .. tostring(who))
local nsent = #S.sent
if os.getenv("TOUCH") ~= "0" then
  guarded(picked.press, "click"); settle()
  assert(#inputs == 0, "a plain tap does not reply")
  guarded(picked.press, "long"); settle()
else
  guarded(picked.press, "click"); settle()
end
assert(#inputs == 1 and inputs[1].init == "@" .. who .. " ", "reply starts with @" .. who)
answer("@" .. who .. " ")
assert(#S.sent == nsent, "an untouched reply is not sent")
guarded(picked.press, os.getenv("TOUCH") ~= "0" and "long" or "click"); settle()
answer("@" .. who .. " sure")
assert(S.sent[#S.sent] == "@" .. who .. " sure", "reply sent")
-- your own lines cannot be picked
for _, l in ipairs(body()) do
  if l.text == "me" then
    for _, b in ipairs(body()) do if b.y == l.y + 16 then assert(not b.press, "own message not pickable") end end
  end
end

-- write a message; it shows at the top; a command reply becomes a toast
press("Write"); answer("hello there")
assert(S.sent[#S.sent] == "hello there")
assert(body()[1] and texts():find("hello there", 1, true), "own message shown after send")
local top_texts = {}
for _, l in ipairs(body()) do if not l.bw and #top_texts < 3 then top_texts[#top_texts + 1] = l.text end end
assert(table.concat(top_texts, "|"):find("hello there", 1, true), "own message at the top")
press("Write"); answer("/help")
assert(toasts[#toasts] == "Commands: /me", "command reply toasted")

-- token expiry: the next poll gets a 401, refreshes, retries
S.valid.t1 = false
local nreq = #log
settle(8000)
print("[poll after expiry] " .. table.concat(log, "; ", nreq + 1))
assert(S.refreshes == 1, "refreshed once")
assert(log[#log]:find("GET /v1/circ/lobby", 1, true) and log[#log]:find("200"), "retried with the new token")

-- a new message while the reader has scrolled: told, not moved; Refresh shows it
if os.getenv("TOUCH") ~= "0" then
  guarded(app.on_input, { type = "down", x = 10, y = 80 })
  guarded(app.on_input, { type = "up", x = 10, y = 40 })
  local before = body()[1]
  room_msgs[#room_msgs + 1] = { #room_msgs + 1, "user1", "message " .. (#room_msgs + 1) }
  settle(8000)
  print("[new while reading] status=" .. tostring(status()))
  assert(body()[1] == before and status() == "1 new: Refresh", "reader not moved")
  press("Refresh")
  assert(order()[1] == #room_msgs, "Refresh shows the new message on top")
  -- not touched since: the next one just appears
  room_msgs[#room_msgs + 1] = { #room_msgs + 1, "user1", "message " .. (#room_msgs + 1) }
  settle(8000)
  assert(order()[1] == #room_msgs, "untouched page takes new messages")
end

guarded(app.on_input or function() end, { type = "key", key = "esc" }); settle()
if os.getenv("TOUCH") ~= "0" then assert(rows()[1]:find("#lobby"), "esc goes back to rooms")
else press("Back") end
assert(not scroll_on, "lists: page fixed")
assert((app.on_input ~= nil) == (os.getenv("TOUCH") ~= "0"), "on_input only with touch: keys must reach the cursor")
press("Back")

-- C-Mail
row("C-Mail")
print("[mail] " .. table.concat(rows(), " / "))
assert(rows()[1]:find("@zed") and not rows()[1]:find("\n"))
row("@zed")
assert(texts():find("hi", 1, true))
press("Write"); answer("yo")
press("Back")
press("New"); answer("@newbie")
print("[new conv] " .. texts():sub(1, 80))
assert(texts():find("@newbie", 1, true))
press("Back"); press("Back")

-- Feed: long post pages through completely
row("Feed")
print("[feed] " .. #rows() .. " rows: " .. rows()[1])
assert(#rows() == 11 and rows()[11] == "Load more...")
row("Load more")
assert(#rows() == 12 and not rows()[12]:find("Load more"), "second page appended")
row("A long one")
check_layout()
assert(scroll_on, "a post scrolls")
local function page_text()
  local t = {}
  for _, l in ipairs(body()) do if l.w then t[#t + 1] = l.text end end
  return table.concat(t, "\n")
end
assert(page_text():find(long_post, 1, true), "the whole post on one page")
for i = 1, 20 do assert(page_text():find("reply " .. i .. "\n", 1, true) or page_text():sub(-#("reply " .. i)) == "reply " .. i, "reply " .. i) end
local top = body()[1]
press("More replies")
assert(body()[1] == top, "more replies added below, no redraw")
check_layout()
for i = 21, 25 do assert(page_text():find("reply " .. i, 1, true), "reply " .. i) end
assert(W_.buttons["More replies"].text == "End of the replies.")
press("Reply"); answer("nice")
assert(S.replies[1].postId == "p1" and S.replies[1].content == "nice")
press("Back")
press("Post"); answer("My title"); answer("Body text")
assert(S.posts[1].title == "My title" and S.posts[1].content == "Body text")
press("Back")

-- close and reopen: straight in with the stored refresh token
guarded(app.on_close)
reset(); timers = {}
app = assert(loadfile(APP))()
guarded(app.on_open, BW, BH); settle()
print("[reopen] " .. table.concat(rows(), " / "))
assert(rows()[1] == "cIRC rooms", "reopen signs in from the stored token")
-- stay signed in off -> token removed
row("Stay signed in")
assert(store_t.rt == nil and store_t.rtn == nil and store_t.rt1 == nil and store_t.remember == "0")
row("Sign out")
assert(W_.buttons["Sign in"])

print(string.format("ok (%dx%d) peak budget ~%d of %d", BW, BH, peak, BUDGET))
