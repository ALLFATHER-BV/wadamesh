-- To-Do: A simple, customizable, To Do list manager.
-- Contributor: Hernán Moraga-Müller
-- Version: 1.0, November 2nd, 2026
-- CHANGE LOG:
-- It stores Tasks entries within the 200 bytes budget in the todo.sav file.
-- Added a Budget indicator in bytes to keep track of free space available.
-- (in this version there is a 175 Bytes budget, anything written after the
-- Budget reaches 0 Bytes won't be saved.)

local ui, sys, store = wada.ui, wada.sys, wada.store
local C = ui.colors

local app = {}
local cv
local W, H
local has_pkbd = false
local KEY_DIAG = false

-- ---------------------------------------------------------------- state
local categories = {}
local todos      = {}
local next_id    = 1
local filter     = "All"
local mode       = "list"
local edit_ctx   = nil
local confirm_ctx= nil

local selected_task_id = nil
local selected_cat     = nil

local group_scroll = 0
local task_scroll  = 0
-- Drag-scroll tracking. active_col is "group" or "task" while the user is
-- touching that column; nil otherwise. start_y remembers where the touch
-- began so successive swipes during a single press move by one row each.
local drag_col = nil
local drag_start_y = 0
local drag_last_y  = 0

local TOP = 4
local HEADER_H = 22
local LEFT_W = 0
local ROW_H = 26
local GAP = 4
local BOTTOM_H = 44
local LIST_TOP = 0
local LIST_BOTTOM = 0
local VISIBLE_ROWS = 0
local SCROLL_W = 12
local SCROLL_PAD = 2

local CH_W_LEFT_NAME  = 7
local CH_W_LEFT_COUNT = 7
local CH_W_RIGHT      = 7
local LEFT_MIN = 70
local LEFT_MAX = 180

local EDIT_BOX_X, EDIT_BOX_Y, EDIT_BOX_W, EDIT_BOX_H = 8, 30, 0, 36
local EDIT_FONT = 14
local EDIT_CHAR_W = 8

local KEY_W, KEY_H = 0, 0
local keys = {}

-- Forward declarations for helpers defined later in the file.
local edit_visible_start
local edit_hit_caret
local snapshot_or_recompute
local clear_snapshot
local pin_visible_list
local bottom_metrics
local draw_bottom_bar
local confirm_metrics
local open_edit
local commit_edit
local cancel_edit
local clear_done
local delete_selected_task
local delete_selected_cat
local handle_edit_key
local bottom_hit
local left_hit
local right_hit
local scrollbar_hit
local scroll_page
local selected_task
local do_action
local draw
local draw_edit

-- ---------------------------------------------------------------- storage
-- ---------------------------------------------------------------- storage
-- Single-key, versioned, compact payload. Designed to fit 255 bytes and to
-- be transmittable verbatim over mesh later. Wire format:
--
--   <VER><HEADER>|<RECORDS>
--
--   VER     : 1 byte, 0x01 for this version.
--   HEADER  : category names joined by "," ending with "|". Any length.
--   RECORDS : one record per task, no separator between records:
--               <state><groupID><text>
--             state    = one of 0xE6 0xE7 0xE8 0xE9
--                        0xE6 urgent pending
--                        0xE7 urgent done
--                        0xE8 pending
--                        0xE9 done
--             groupID  = "A".."Z", index into the header list (1-based)
--             text     = any bytes except 0xE6-0xE9, "," and "|"
--
-- Every record is fully delimited because a state byte cannot appear inside
-- a task's text. The decoder walks the string and reassembles; the encoder
-- strips disallowed bytes from user text before writing.
--
-- save() enforces a 255-byte hard ceiling. If the payload overflows, the
-- last task(s) are omitted from the written string (in-memory list is not
-- touched) and a toast tells the user how many could not be persisted.

local PAYLOAD_KEY  = "list"
local PAYLOAD_VER  = string.char(0x01)
local PAYLOAD_MAX  = 255

local S_UP = "="   -- urgent pending  (61)
local S_UD = "*"   -- urgent done     (42)
local S_P  = "_"   -- pending         (95)
local S_D  = "\\"  -- done            (92)
local STATE_SET = { [61]=true, [42]=true, [95]=true, [92]=true }

local function state_char(urgent, done)
  if urgent then return done and S_UD or S_UP end
  return done and S_D or S_P
end

local function state_decode(b)
  return (b == 61 or b == 42), (b == 42 or b == 92)
end

local function clean_text(s)
  return (tostring(s or ""):gsub("[%z\1-\31,|=*_\\]", " "))
end

local function clean_label(s)
  local v = tostring(s or ""):gsub("[%z\1-\31,|=*_\\]", " ")
  v = v:sub(1, 3)
  while #v < 3 do v = v .. " " end
  return v
end

local function group_index(name)
  for i, c in ipairs(categories) do
    if c == name then return i end
  end
  return 1
end

local function index_to_letter(i)
  if i < 1 then i = 1 end
  if i > 26 then i = 26 end
  return string.char(64 + i)  -- 1 -> "A", 26 -> "Z"
end

-- Build the full payload. Returns a string of arbitrary length; the caller
-- decides whether it fits.
local function encode_payload()
  local labels = {}
  for _, c in ipairs(categories) do labels[#labels + 1] = clean_label(c) end
  local header = table.concat(labels) .. "|"
  local parts = {}
  for _, t in ipairs(todos) do
    local gi = group_index(t.cat or "")
    parts[#parts + 1] = state_char(t.urgent, t.done) ..
                        index_to_letter(gi) ..
                        clean_text(t.text)
  end
  return PAYLOAD_VER .. header .. table.concat(parts)
end

local function decode_payload(s)
  local cats, tasks = {}, {}
  if type(s) ~= "string" or #s == 0 then return cats, tasks end

  -- Migration shim: rewrite any legacy marker byte to the current set.
  -- Handles high bytes 0xE6-0xE9 and every prior printable set. Idempotent:
  -- characters already in the current set (=, *, _, \) are not in the
  -- match pattern and pass through untouched.
  s = s:gsub("[\230\231\232\233]", function(c)
    local b = c:byte()
    if b == 230 then return "=" end
    if b == 231 then return "*" end
    if b == 232 then return "_" end
    if b == 233 then return "\\" end
    return c
  end)

  -- Skip version byte (0x01 in current, 0x03 in an older variant).
  local i = 1
  local vb = s:byte(1)
  if vb == 0x01 or vb == 0x03 then i = 2 end

  -- Header: up to the first "|". Read as fixed 3-byte chunks with spaces
  -- padded on the right. If a comma is present, the header is a legacy
  -- comma-separated format and is parsed that way instead.
  local bar = s:find("|", i, true)
  if not bar then return cats, tasks end
  local header = s:sub(i, bar - 1)
  if header:find(",", 1, true) then
    for piece in (header .. ","):gmatch("(.-),") do
      if piece ~= "" then cats[#cats + 1] = piece end
    end
  else
    local j = 1
    while j + 2 <= #header do
      local label = header:sub(j, j + 2)
      label = label:gsub("%s+$", "")
      if label ~= "" then cats[#cats + 1] = label end
      j = j + 3
    end
  end

  -- Records: <state><groupID><text> with no separator between records.
  -- The state byte cannot appear inside text (clean_text strips it), so
  -- every record is fully delimited by the next state byte.
  i = bar + 1
  local n = #s
  while i <= n do
    local b = s:byte(i)
    if not STATE_SET[b] then
      -- Unknown byte where a state marker was expected: skip forward to
      -- the next marker so the remaining records are not lost.
      repeat i = i + 1 until i > n or STATE_SET[s:byte(i)]
    else
      local gid = 1
      if i + 1 <= n then
        local gl = s:byte(i + 1)
        if gl and gl >= 65 and gl <= 90 then gid = gl - 64 end
      end
      i = i + 2
      local start = i
      while i <= n do
        local nb = s:byte(i)
        if STATE_SET[nb] then break end
        i = i + 1
      end
      local txt = s:sub(start, i - 1)
      local urgent, done = state_decode(b)
      local cat = cats[gid] or cats[1] or "Gen"
      tasks[#tasks + 1] = {
        id = #tasks + 1,
        cat = cat,
        done = done,
        urgent = urgent,
        text = txt,
      }
    end
  end
  return cats, tasks
end

-- Truncate a payload to a complete-record boundary at or below limit.
-- Returns the truncated string, or nil if even the header does not fit.
local function truncate_payload(s, limit)
  if #s <= limit then return s, 0 end
  -- Find the header end. Everything after is records.
  local bar = s:find("|", 2, true)
  if not bar then return nil, 0 end
  if bar > limit then return nil, 0 end

  -- Walk records and remember the last record boundary at or below limit.
  local last_ok = bar   -- header end is always a valid boundary
  local i = bar + 1
  local n = #s
  local kept = 0
  while i <= n do
    local b = s:byte(i)
    if not STATE_SET[b] then break end
    -- Find the end of this record.
    local j = i + 2
    while j <= n do
      local nb = s:byte(j)
      if STATE_SET[nb] then break end
      j = j + 1
    end
    if j - 1 <= limit then
      last_ok = j - 1
      kept = kept + 1
      i = j
    else
      break
    end
  end
  local truncated = s:sub(1, last_ok)
  -- Report how many task records were dropped.
  local total_records = 0
  do
    local k = bar + 1
    while k <= n do
      if STATE_SET[s:byte(k)] then
        total_records = total_records + 1
        k = k + 2
        while k <= n and not STATE_SET[s:byte(k)] do k = k + 1 end
      else
        k = k + 1
      end
    end
  end
  return truncated, total_records - kept
end

local function save()
  local payload = encode_payload()
  local dropped = 0
  if #payload > PAYLOAD_MAX then
    payload, dropped = truncate_payload(payload, PAYLOAD_MAX)
    if not payload then
      -- Header alone overflows; cannot save anything coherent. Keep the
      -- previous on-disk payload intact rather than overwriting it.
      sys.toast("Payload header too long", 2000)
      return false
    end
  end
  local ok = store.set(PAYLOAD_KEY, payload)
  if ok == false then
    sys.toast("Store refused write (" .. #payload .. "B)", 2000)
    return false
  end
  if dropped > 0 then
    sys.toast("Saved; " .. dropped .. " task(s) omitted (" .. #payload .. "B)",
              2000)
  end
  return true
end

local function ensure_general()
  if #categories == 0 then categories[1] = "General" end
end

local function load()
  local raw = store.get(PAYLOAD_KEY, nil)
  if raw == nil or raw == "" then
    categories = { "Per", "Job", "Buy" }
    todos = {}
    next_id = 1
    ensure_general()
    save()
    return
  end
  local cats, tasks = decode_payload(raw)
  if #cats == 0 then cats = { "Personal", "Work", "Market" } end
  categories = cats
  todos = tasks
  ensure_general()

  -- Reassign IDs sequentially and set next_id beyond the highest.
  for i, t in ipairs(todos) do t.id = i end
  next_id = #todos + 1

  -- Reject any task whose category no longer exists.
  local fb = categories[1]
  for _, t in ipairs(todos) do
    local ok = false
    for _, c in ipairs(categories) do if t.cat == c then ok = true; break end end
    if not ok then t.cat = fb end
  end

  -- Filter: if the current filter no longer names a real category, reset.
  if filter ~= "All" then
    local ok = false
    for _, c in ipairs(categories) do if c == filter then ok = true; break end end
    if not ok then filter = "All" end
  end
end

-- ---------------------------------------------------------------- helpers
local function toast(m) sys.toast(m, 1400) end

local function sort_tasks(list)
  table.sort(list, function(a, b)
    if a.urgent ~= b.urgent then return a.urgent end
    local la, lb = (a.text or ""):lower(), (b.text or ""):lower()
    if la ~= lb then return la < lb end
    return (a.id or 0) < (b.id or 0)
  end)
  return list
end

local function visible_todos()
  local out = {}
  for _, t in ipairs(todos) do
    if filter == "All" or t.cat == filter then out[#out + 1] = t end
  end
  return sort_tasks(out)
end


local function in_box(x, y, bx, by, bw, bh)
  return x and y and x >= bx and x < bx + bw and y >= by and y < by + bh
end

local function pending_count()
  local n = 0
  for _, t in ipairs(todos) do
    if not t.done and (filter == "All" or t.cat == filter) then n = n + 1 end
  end
  return n
end

local function tasks_in_cat(name)
  local n = 0
  for _, t in ipairs(todos) do if t.cat == name then n = n + 1 end end
  return n
end

local function group_names()
  local names = { "All" }
  for _, c in ipairs(categories) do names[#names + 1] = c end
  return names
end

-- ---------------------------------------------------------------- width calc
local function compute_left_width()
  local widest = 0
  local names = group_names()
  for _, name in ipairs(names) do
    local cnt = "(" ..
      (name == "All" and #todos or tasks_in_cat(name)) .. ")"
    local name_px  = #name * CH_W_LEFT_NAME
    local count_px = #cnt  * CH_W_LEFT_COUNT
    local need = name_px + 6 + count_px
    if need > widest then widest = need end
  end
  local total = widest + 16 + SCROLL_W + SCROLL_PAD
  if total < LEFT_MIN then total = LEFT_MIN end
  if total > LEFT_MAX then total = LEFT_MAX end
  local cap = math.floor(W * 0.45)
  if total > cap then total = cap end
  return total
end

local function recalc_layout()
  LIST_TOP = TOP + HEADER_H + 4
  LIST_BOTTOM = H - BOTTOM_H - 4
  VISIBLE_ROWS = math.max(1,
    math.floor((LIST_BOTTOM - (LIST_TOP + 2) + 1) / ROW_H))
  if VISIBLE_ROWS < 5 then
    VISIBLE_ROWS = math.min(5,
      math.max(1, math.floor((LIST_BOTTOM - (LIST_TOP + 2) + 1) / ROW_H)))
  end
  LEFT_W = compute_left_width()
end

local function clamp_scroll(offset, total, visible)
  local maxoff = math.max(0, total - visible)
  if offset > maxoff then offset = maxoff end
  if offset < 0 then offset = 0 end
  return offset
end

local function reclamp_scrolls()
  group_scroll = clamp_scroll(group_scroll, #group_names(), VISIBLE_ROWS)
  task_scroll  = clamp_scroll(task_scroll,  #visible_todos(), VISIBLE_ROWS)
end

local KEYCHARS = {
  "Q","W","E","R","T","Y","U","I","O","P",
  "A","S","D","F","G","H","J","K","L",";",
  "Z","X","C","V","B","N","M",",",".","/",
  "1","2","3","4","5","6","7","8","9","0",
  "-","_","(",")"," ","'","\"","+","&","!",
}

local function layout_keys()
  keys = {}
  if has_pkbd then return end
  local cols, pad = 10, 4
  KEY_W = math.floor((W - pad * (cols + 1)) / cols)
  KEY_H = 26
  local x0, y0 = pad, H - BOTTOM_H - (6 * (KEY_H + pad)) - 8
  for i, ch in ipairs(KEYCHARS) do
    local r = math.floor((i - 1) / cols)
    local c = (i - 1) % cols
    keys[i] = { ch = ch, x = x0 + c * (KEY_W + pad),
                y = y0 + r * (KEY_H + pad), w = KEY_W, h = KEY_H }
  end
end

local function draw_header()
  local y = TOP
  cv:rect(4, y, W - 8, HEADER_H, C.panel, true, 4)

  local n = pending_count()
  local msg = "To Do " .. filter .. ": " .. n ..
              " task" .. (n == 1 and "" or "s")
  cv:text(12, y + 4, msg, C.text, 14)

  local used = #encode_payload()
  local budget = 255 - used
  if budget < 0 then budget = 0 end
  local right = "Budget: " .. budget - 69 .. " bytes"
  -- Right-align: approximate character width is 7 px at 12 pt.
  local rx = W - 12 - #right * 7
  local color = (budget < 40) and C.bad or C.sub
  cv:text(rx, y + 5, right, color, 12)
end

local function draw_scrollbar(x, y, h, total, visible, offset)
  if total <= visible or total == 0 then return end
  cv:rect(x, y, SCROLL_W, h, C.panel, true, 3)
  local thumb_h = math.max(10, math.floor(h * visible / total))
  local maxoff  = total - visible
  local ratio   = maxoff > 0 and (offset / maxoff) or 0
  local thumb_y = y + math.floor((h - thumb_h) * ratio)
  cv:rect(x + 1, thumb_y, SCROLL_W - 2, thumb_h, C.accent, true, 3)
end

local function draw_left_column()
  local col_x = 4
  local col_w = LEFT_W
  local content_w = col_w - SCROLL_W - SCROLL_PAD
  cv:rect(col_x, LIST_TOP, col_w, LIST_BOTTOM - LIST_TOP, C.bg, true, 4)

  local names = group_names()
  group_scroll = clamp_scroll(group_scroll, #names, VISIBLE_ROWS)

  local y = LIST_TOP + 2
  local first = group_scroll + 1
  local last  = math.min(#names, group_scroll + VISIBLE_ROWS)
  for i = first, last do
    local name = names[i]
    local is_filter = (name == filter)
    local is_sel    = (name ~= "All" and name == selected_cat)
    local bg
    if is_sel then bg = C.good
    elseif is_filter then bg = C.accent
    else bg = C.panel end
    cv:rect(col_x + 2, y, content_w - 2, ROW_H - 2, bg, true, 4)
    cv:text(col_x + 8, y + 5, name, (is_filter or is_sel) and C.bg or C.text, 13)
    local cnt = "(" ..
      (name == "All" and #todos or tasks_in_cat(name)) .. ")"
    local cw = #cnt * CH_W_LEFT_COUNT
    cv:text(col_x + content_w - cw - 4, y + 6, cnt,
            (is_filter or is_sel) and C.bg or C.sub, 11)
    y = y + ROW_H
  end

  draw_scrollbar(col_x + col_w - SCROLL_W - 1, LIST_TOP + 2,
                 LIST_BOTTOM - LIST_TOP - 4, #names, VISIBLE_ROWS, group_scroll)
end

local function draw_right_column()
  local rx = 4 + LEFT_W + GAP
  local rw = W - rx - 4
  local content_w = rw - SCROLL_W - SCROLL_PAD
  cv:rect(rx, LIST_TOP, rw, LIST_BOTTOM - LIST_TOP, C.bg, true, 4)

  local items = snapshot_or_recompute()
  task_scroll = clamp_scroll(task_scroll, #items, VISIBLE_ROWS)

  if #items == 0 then
    cv:text(rx + 8, LIST_TOP + 6, "(no tasks)", C.sub, 12)
    clear_snapshot()
    return
  end

  local lead_px = 22
  local tag_allow_px = 0
  if filter == "All" then tag_allow_px = 70 end
  local text_px = content_w - lead_px - tag_allow_px - 10
  local max_chars = math.max(4, math.floor(text_px / CH_W_RIGHT))

  local y = LIST_TOP + 2
  local first = task_scroll + 1
  local last  = math.min(#items, task_scroll + VISIBLE_ROWS)
  for i = first, last do
    local t = items[i]
    local selected = (selected_task_id == t.id)
    cv:rect(rx + 2, y, content_w - 2, ROW_H - 2,
            selected and C.accent or C.panel, true, 4)
    local x = rx + 8
    if t.urgent then
      cv:text(x, y + 4, "!", C.bad, 16); x = x + 10
    end
    if t.done then
      cv:text(x, y + 5, "x", C.good, 14); x = x + 12
    else
      cv:text(x, y + 5, "o", selected and C.bg or C.sub, 14); x = x + 12
    end
    local label = t.text
    if #label > max_chars then label = label:sub(1, max_chars - 1) .. "…" end
    cv:text(x, y + 5, label,
            selected and C.bg or (t.done and C.sub or C.text), 13)
    if filter == "All" and t.cat and t.cat ~= "" then
      local tag = "[" .. t.cat .. "]"
      cv:text(rx + content_w - 4 - #tag * CH_W_RIGHT, y + 6, tag,
              selected and C.bg or C.sub, 11)
    end
    y = y + ROW_H
  end

  draw_scrollbar(rx + rw - SCROLL_W - 1, LIST_TOP + 2,
                 LIST_BOTTOM - LIST_TOP - 4, #items, VISIBLE_ROWS, task_scroll)

  clear_snapshot()
end

function draw_edit()
  local title = (edit_ctx.kind == "rename") and "Rename group"
             or (edit_ctx.kind == "cat")    and "New group"
             or (edit_ctx.kind == "todo_rename") and "Edit task"
             or "New task"
  cv:text(8, 6, title, C.text, 16)

  EDIT_BOX_X, EDIT_BOX_Y = 8, 30
  EDIT_BOX_W, EDIT_BOX_H = W - 16, 36
  cv:rect(EDIT_BOX_X, EDIT_BOX_Y, EDIT_BOX_W, EDIT_BOX_H, C.panel, true, 5)

  local start = edit_visible_start()
  local caret = edit_ctx.caret or (#edit_ctx.text + 1)
  local visible = edit_ctx.text:sub(start)
  cv:text(EDIT_BOX_X + 8, EDIT_BOX_Y + 9, visible, C.text, EDIT_FONT)

  -- Caret: draw the prefix (text up to the caret) in red on a second line
  -- below the text. The boundary between the red prefix and the remaining
  -- text is the caret. Contrast comes from red glyphs on the panel; we do
  -- not try to cover a full-width strip because cv:text draws glyphs, not
  -- a background.
  local vis_col = caret - start
  if vis_col >= 0 then
    local ux = EDIT_BOX_X + 8
    local uy = EDIT_BOX_Y + EDIT_BOX_H - 14
    local prefix = edit_ctx.text:sub(start, caret - 1)
    if #prefix > 0 then
      cv:text(ux, uy, prefix, C.bad, EDIT_FONT)
    end
  end

  if has_pkbd then
    local hint = (edit_ctx.kind == "cat" or edit_ctx.kind == "rename")
               and "Type (max 3) · Enter=OK · Esc=Back"
               or  "Type · Enter=OK · Esc=Back · Backspace=Del · Tap to move caret"
    cv:text(8, EDIT_BOX_Y + EDIT_BOX_H + 12, hint, C.text, 12)
    return
  end

  for _, k in ipairs(keys) do
    cv:rect(k.x, k.y, k.w, k.h, C.panel, true, 4)
    cv:text(k.x + 6, k.y + 6, k.ch, C.text, 13)
  end
  local cy2 = H - BOTTOM_H + 6
  local bw = math.floor((W - 16) / 3)
  local cmds = { "Space", "Back", "OK" }
  for i, txt in ipairs(cmds) do
    local x = 4 + (i - 1) * (bw + 4)
    cv:rect(x, cy2, bw, 32, i == 3 and C.good or C.accent, true, 5)
    cv:text(x + 8, cy2 + 9, txt, C.bg, 12)
  end
end

local function draw_confirm()
  cv:fill(C.bg)
  draw_header(); draw_left_column(); draw_right_column(); draw_bottom_bar()

  local cw, ch, cx, cyy, bw, by = confirm_metrics()
  cv:rect(cx, cyy, cw, ch, C.panel, true, 6)
  local msg = confirm_ctx.msg
  local lines, line = {}, ""
  for word in (msg .. " "):gmatch("(%S+)%s+") do
    if #line + #word + 1 > 34 then
      lines[#lines + 1] = line
      line = word
    else
      line = (#line == 0) and word or (line .. " " .. word)
    end
  end
  if #line > 0 then lines[#lines + 1] = line end
  local ty = cyy + 10
  for i = 1, math.min(3, #lines) do
    cv:text(cx + 10, ty + (i - 1) * 16, lines[i], C.text, 12)
  end
  cv:rect(cx + 4, by, bw, 30, C.bad, true, 5)
  cv:text(cx + 12, by + 8, "Delete", C.bg, 12)
  cv:rect(cx + 8 + bw, by, bw, 30, C.accent, true, 5)
  cv:text(cx + 8 + bw + 8, by + 8, "Cancel", C.bg, 12)
end

function draw()
  if mode == "confirm" then draw_confirm(); return end
  cv:fill(C.bg)
  if mode == "edit" then draw_edit(); return end
  draw_header(); draw_left_column(); draw_right_column(); draw_bottom_bar()
end

local function edit_set_caret(i)
  if i < 1 then i = 1 end
  if i > #edit_ctx.text + 1 then i = #edit_ctx.text + 1 end
  edit_ctx.caret = i
end

local function edit_insert(ch)
  -- Per-kind input caps. A group label is stored as a comma-terminated
  -- header field, and the header has a fixed 255-byte budget; three
  -- characters is the design limit.
  local cap = math.huge
  if edit_ctx and edit_ctx.kind == "cat"    then cap = 3 end
  if edit_ctx and edit_ctx.kind == "rename" then cap = 3 end

  if #edit_ctx.text >= cap then
    if edit_ctx.kind == "cat" or edit_ctx.kind == "rename" then
      toast("Max 3 chars")
    end
    return
  end

  local caret = edit_ctx.caret or (#edit_ctx.text + 1)
  local before = edit_ctx.text:sub(1, caret - 1)
  local after  = edit_ctx.text:sub(caret)
  edit_ctx.text = before .. ch .. after
  edit_ctx.caret = caret + 1
end

local function edit_backspace()
  local caret = edit_ctx.caret or (#edit_ctx.text + 1)
  if caret <= 1 then return end
  local before = edit_ctx.text:sub(1, caret - 2)
  local after  = edit_ctx.text:sub(caret)
  edit_ctx.text = before .. after
  edit_ctx.caret = caret - 1
end

local function edit_max_chars()
  return math.max(1, math.floor((EDIT_BOX_W - 24) / EDIT_CHAR_W))
end

function edit_visible_start()
  local maxc  = edit_max_chars()
  local caret = edit_ctx.caret or (#edit_ctx.text + 1)
  if caret > maxc then return caret - maxc + 1 end
  return 1
end

function edit_hit_caret(x)
  if edit_ctx and edit_ctx.text and edit_ctx.text ~= "" then
    local ok, fn = pcall(function() return wada.ui.text_w or wada.ui.text_width end)
    if ok and type(fn) == "function" then
      local prefix = edit_ctx.text
      local best, bestd = 1, math.huge
      for i = 1, #edit_ctx.text + 1 do
        local w_ok, w = pcall(fn, prefix:sub(1, i - 1), EDIT_FONT)
        if w_ok and type(w) == "number" then
          local d = math.abs((EDIT_BOX_X + 8 + w) - x)
          if d < bestd then best, bestd = i, d end
        end
      end
      edit_set_caret(best)
      return
    end
  end
  local start = edit_visible_start()
  local rel = x - (EDIT_BOX_X + 8)
  if rel < 0 then rel = 0 end
  local col = math.floor(rel / EDIT_CHAR_W + 0.5)
  edit_set_caret(start + col)
end

-- Todo — part 2 of 2. Paste immediately after part 1 to form todo.lua.

local PAD = 4
local CHAR = 7
local BTN_LABEL_PAD = 9
local BTN_TEXT_PAD  = 6

local BOTTOM_BUTTONS = {
  { label = "+Task",      action = "add"    },
  { label = "+Group",     action = "cats"   },
  { label = "Edit",       action = "edit"   },
  { label = "Urgent",     action = "urgent" },
  { label = "Clear Done", action = "clear"  },
  { label = "Del",        action = "del"    },
}

function bottom_metrics()
  local n = #BOTTOM_BUTTONS
  local xs, ws = {}, {}
  -- Each button is exactly its label width plus fixed padding. No deltas,
  -- no proportional slack. This is what removes the trailing space: every
  -- button's width is derived from the text it contains.
  local total_w = 0
  for i, b in ipairs(BOTTOM_BUTTONS) do
    ws[i] = #b.label * CHAR + BTN_LABEL_PAD
    total_w = total_w + ws[i]
  end

  -- Give any remaining pixels to the last button (Del in the current order).
  local avail = W - PAD * (n + 1)
  local extra = avail - total_w
  if extra > 0 then ws[n] = ws[n] + extra end

  -- Lay out left to right.
  local x = PAD
  for i = 1, n do
    xs[i] = x
    x = x + ws[i] + PAD
  end
  return n, xs, ws
end

function draw_bottom_bar()
  local y = H - BOTTOM_H + 6
  local n, xs, ws = bottom_metrics()
  for i, b in ipairs(BOTTOM_BUTTONS) do
    cv:rect(xs[i], y, ws[i], 32, C.accent, true, 5)
    cv:text(xs[i] + BTN_TEXT_PAD, y + 9, b.label, C.bg, 12)
  end
end

function confirm_metrics()
  local cw = math.min(W - 40, 280)
  local ch = 110
  local cx = math.floor((W - cw) / 2)
  local cyy = math.floor((H - ch) / 2)
  local bw = math.floor((cw - 12) / 2)
  local by = cyy + ch - 38
  return cw, ch, cx, cyy, bw, by
end

local visible_snapshot = nil

function pin_visible_list()
  visible_snapshot = visible_todos()
  return visible_snapshot
end

function snapshot_or_recompute()
  if visible_snapshot then return visible_snapshot end
  return visible_todos()
end

function clear_snapshot()
  visible_snapshot = nil
end

function open_edit(kind, initial, target)
  edit_ctx = { kind = kind, text = initial or "", target = target,
               caret = #(initial or "") + 1 }
  mode = "edit"; draw()
end

function commit_edit()
  local txt = edit_ctx.text:match("^%s*(.-)%s*$")
  if txt == "" then toast("Cannot be empty"); return end
  if edit_ctx.kind == "todo" then
    local cat = (filter ~= "All") and filter or (categories[1] or "General")
    todos[#todos + 1] = { id = next_id, text = txt, cat = cat,
                          done = false, urgent = false }
    next_id = next_id + 1; toast("Added")
  elseif edit_ctx.kind == "todo_rename" then
    for _, t in ipairs(todos) do
      if t.id == edit_ctx.target then t.text = txt break end
    end
    toast("Updated")
  elseif edit_ctx.kind == "cat" then
    if #txt > 3 then toast("Max 3 chars"); return end
    for _, c in ipairs(categories) do
      if c == txt then toast("Already exists"); return end
    end
    categories[#categories + 1] = txt
  elseif edit_ctx.kind == "rename" then
    if #txt > 3 then toast("Max 3 chars"); return end
    local idx = edit_ctx.target
    for j, c in ipairs(categories) do
      if j ~= idx and c == txt then toast("Already exists"); return end
    end
    local old = categories[idx]
    for _, t in ipairs(todos) do if t.cat == old then t.cat = txt end end
    categories[idx] = txt
    if filter == old then filter = txt end
    if selected_cat == old then selected_cat = txt end
  end
  save(); mode = "list"; edit_ctx = nil
  clear_snapshot()
  recalc_layout(); reclamp_scrolls(); draw()
end

function cancel_edit() mode = "list"; edit_ctx = nil; clear_snapshot(); draw() end

function clear_done()
  local kept = {}
  for _, t in ipairs(todos) do if not t.done then kept[#kept + 1] = t end end
  todos = kept
  if selected_task_id then
    local still = false
    for _, t in ipairs(todos) do
      if t.id == selected_task_id then still = true break end
    end
    if not still then selected_task_id = nil end
  end
  save(); toast("Cleared completed")
  clear_snapshot(); reclamp_scrolls(); draw()
end

function delete_selected_task()
  if not selected_task_id then return end
  local kept = {}
  for _, t in ipairs(todos) do
    if t.id ~= selected_task_id then kept[#kept + 1] = t end
  end
  todos = kept; selected_task_id = nil
  save(); toast("Task deleted")
  clear_snapshot(); reclamp_scrolls(); draw()
end

function delete_selected_cat()
  if not selected_cat then return end
  if #categories <= 1 then toast("Keep at least one"); return end
  local name = selected_cat
  local idx
  for i, c in ipairs(categories) do if c == name then idx = i break end end
  if not idx then selected_cat = nil; return end
  table.remove(categories, idx)
  local fb = categories[1] or "General"
  for _, t in ipairs(todos) do if t.cat == name then t.cat = fb end end
  if filter == name then filter = "All" end
  selected_cat = nil
  save(); toast("Removed " .. name)
  clear_snapshot(); recalc_layout(); reclamp_scrolls(); draw()
end

function handle_edit_key(k)
  if KEY_DIAG then
    local len = type(k) == "string" and #k or "?"
    sys.toast("key='" .. tostring(k) .. "' len=" .. tostring(len), 1500)
  end
  local n = tonumber(k)
  if n == 13 or n == 10 then commit_edit(); return end
  if n == 27 then cancel_edit(); return end
  if n == 8 or n == 127 then edit_backspace(); draw(); return end
  if type(k) == "string" then
    local lk = k:lower()
    if k == "\r" or k == "\n" or lk == "enter" or lk == "return" then commit_edit(); return end
    if k == "\27" or lk == "esc" or lk == "escape" then cancel_edit(); return end
    if k == "\8" or k == "\127" or lk == "backspace" or lk == "del" or lk == "delete" then
      edit_backspace(); draw(); return
    end
    if lk == "left"  or k == "\27[D" then edit_set_caret((edit_ctx.caret or 1) - 1); draw(); return end
    if lk == "right" or k == "\27[C" then edit_set_caret((edit_ctx.caret or 1) + 1); draw(); return end
    if lk == "home" then edit_set_caret(1); draw(); return end
    if lk == "end" then edit_set_caret(#edit_ctx.text + 1); draw(); return end
    if #k == 1 then
      local b = k:byte()
      if b and b >= 32 and b < 127 then edit_insert(k); draw() end
    end
  end
end

function bottom_hit(x, y)
  local by = H - BOTTOM_H + 6
  if y < by or y >= by + 32 then return nil end
  local n, xs, ws = bottom_metrics()
  for i, b in ipairs(BOTTOM_BUTTONS) do
    if x >= xs[i] and x < xs[i] + ws[i] then return b.action end
  end
  return nil
end

function left_hit(x, y)
  if x < 4 or x >= 4 + LEFT_W then return nil end
  if y < LIST_TOP + 2 or y >= LIST_BOTTOM then return nil end
  local names = group_names()
  local vis_idx = math.floor((y - (LIST_TOP + 2)) / ROW_H) + 1
  if vis_idx < 1 or vis_idx > VISIBLE_ROWS then return nil end
  local idx = group_scroll + vis_idx
  if idx <= #names then return names[idx] end
  return nil
end

-- Todo — part 2 of 2. Paste immediately after part 1 to form todo.lua.

function right_hit(x, y)
  local rx = 4 + LEFT_W + GAP
  if x < rx or x >= W - 4 then return nil end
  if y < LIST_TOP + 2 or y >= LIST_BOTTOM then return nil end
  local items = pin_visible_list()
  local vis_idx = math.floor((y - (LIST_TOP + 2)) / ROW_H) + 1
  if vis_idx < 1 or vis_idx > VISIBLE_ROWS then return nil end
  local idx = task_scroll + vis_idx
  return items[idx]
end

function scrollbar_hit(x, y)
  if y < LIST_TOP + 2 or y >= LIST_BOTTOM then return nil end
  local function check(x0, total, offset, kind)
    if total <= VISIBLE_ROWS then return nil end
    if x < x0 or x >= x0 + SCROLL_W then return nil end
    local bar_y = LIST_TOP + 2
    local bar_h = LIST_BOTTOM - LIST_TOP - 4
    local thumb_h = math.max(10, math.floor(bar_h * VISIBLE_ROWS / total))
    local maxoff  = total - VISIBLE_ROWS
    local ratio   = maxoff > 0 and (offset / maxoff) or 0
    local thumb_y = bar_y + math.floor((bar_h - thumb_h) * ratio)
    if y >= thumb_y and y < thumb_y + thumb_h then return kind, "thumb", y end
    if y < thumb_y then return kind, "up", y end
    return kind, "down", y
  end
  local left_bar_x = 4 + LEFT_W - SCROLL_W - 1
  local k, page, py = check(left_bar_x, #group_names(), group_scroll, "group")
  if k then return k, page, py end
  local rx = 4 + LEFT_W + GAP
  local right_bar_x = rx + (W - rx - 4) - SCROLL_W - 1
  k, page, py = check(right_bar_x, #visible_todos(), task_scroll, "task")
  if k then return k, page, py end
  return nil
end

function scroll_page(kind, page)
  local step = math.max(1, VISIBLE_ROWS - 1)
  if kind == "group" then
    if page == "up" then group_scroll = group_scroll - step end
    if page == "down" then group_scroll = group_scroll + step end
    group_scroll = clamp_scroll(group_scroll, #group_names(), VISIBLE_ROWS)
  elseif kind == "task" then
    if page == "up" then task_scroll = task_scroll - step end
    if page == "down" then task_scroll = task_scroll + step end
    task_scroll = clamp_scroll(task_scroll, #visible_todos(), VISIBLE_ROWS)
  end
  clear_snapshot()
  draw()
end

function selected_task()
  if not selected_task_id then return nil end
  for _, t in ipairs(todos) do
    if t.id == selected_task_id then return t end
  end
  return nil
end

function do_action(action)
  if action == "add" then
    open_edit("todo", "")
  elseif action == "clear" then
    clear_done()
  elseif action == "cats" then
    open_edit("cat", "")
  elseif action == "edit" then
    if selected_cat then
      local idx
      for i, c in ipairs(categories) do if c == selected_cat then idx = i break end end
      if idx then open_edit("rename", selected_cat, idx); return end
    end
    local t = selected_task()
    if t then open_edit("todo_rename", t.text, t.id); return end
    toast("Select a group or a task first")
  elseif action == "del" then
    if selected_cat then
      local n = tasks_in_cat(selected_cat)
      if n > 0 then
        confirm_ctx = {
          kind = "cat", target = selected_cat,
          msg = "Delete group '" .. selected_cat .. "'? It has " .. n ..
                " task" .. (n == 1 and "" or "s") ..
                " that will move to the first remaining group.",
        }
        mode = "confirm"; draw()
      else
        delete_selected_cat()
      end
      return
    end
    if selected_task_id then delete_selected_task(); return end
    toast("Select a group or a task first")
  elseif action == "urgent" then
    local t = selected_task()
    if not t then toast("Select a task first"); return end
    t.urgent = not t.urgent; save()
    toast(t.urgent and "Marked urgent" or "Urgent cleared")
    clear_snapshot(); reclamp_scrolls(); draw()
  end
end

function app.on_input(ev)

  if mode == "confirm" then
    if ev.type == "down" and ev.x and ev.y then
      local cw, ch, cx, cyy, bw, by = confirm_metrics()
      if in_box(ev.x, ev.y, cx + 4, by, bw, 30) then
        local kind = confirm_ctx.kind
        confirm_ctx = nil; mode = "list"
        if kind == "cat" then delete_selected_cat() end
        return
      elseif in_box(ev.x, ev.y, cx + 8 + bw, by, bw, 30) then
        confirm_ctx = nil; mode = "list"; draw(); return
      end
    end
    return
  end

  if mode == "edit" and has_pkbd and ev.type == "key" then
    if ev.key ~= nil then handle_edit_key(ev.key) end
    return
  end

  if mode == "edit" then
    if ev.type == "swipe" then
      if ev.dir == "left" then edit_insert(" "); draw()
      elseif ev.dir == "up" then commit_edit()
      elseif ev.dir == "down" then cancel_edit() end
      return
    end
    if ev.type == "down" and ev.x and ev.y then
      if in_box(ev.x, ev.y, EDIT_BOX_X, EDIT_BOX_Y, EDIT_BOX_W, EDIT_BOX_H) then
        edit_hit_caret(ev.x); draw(); return
      end
      if not has_pkbd then
        for _, k in ipairs(keys) do
          if in_box(ev.x, ev.y, k.x, k.y, k.w, k.h) then
            edit_insert(k.ch); draw(); return
          end
        end
        local cy2 = H - BOTTOM_H + 6
        local bw = math.floor((W - 16) / 3)
        if in_box(ev.x, ev.y, 4, cy2, bw, 32) then edit_insert(" "); draw()
        elseif in_box(ev.x, ev.y, 4 + 2 * (bw + 4), cy2, bw, 32) then commit_edit()
        elseif in_box(ev.x, ev.y, 4 + (bw + 4), cy2, bw, 32) then cancel_edit() end
      end
    end
    return
  end

  if ev.type ~= "down" or not ev.x or not ev.y then return end

  local sb_kind, sb_page = scrollbar_hit(ev.x, ev.y)
  if sb_kind then
    if sb_page == "up" or sb_page == "down" then
      scroll_page(sb_kind, sb_page)
    end
    return
  end

  local action = bottom_hit(ev.x, ev.y)
  if action then do_action(action); return end

  local cat = left_hit(ev.x, ev.y)
  if cat then
    clear_snapshot()
    if cat == "All" then
      filter = "All"; selected_cat = nil; selected_task_id = nil
    else
      filter = cat
      if selected_cat == cat then selected_cat = nil
      else selected_cat = cat; selected_task_id = nil end
    end
    save(); reclamp_scrolls(); draw(); return
  end

  local t = right_hit(ev.x, ev.y)
  if t then
    selected_cat = nil
    if selected_task_id == t.id then t.done = not t.done; save()
    else selected_task_id = t.id end
    clear_snapshot(); draw(); return
  end
end

function app.on_open(w, h)

  W, H = w, h
  local ok, caps = pcall(sys.caps)
  has_pkbd = ok and type(caps) == "table" and caps.keyboard == true

  recalc_layout()
  load()
  recalc_layout()
  reclamp_scrolls()

  cv = ui.canvas(W, H)
  cv:pos(0, 0)
  layout_keys()
  draw()
end

function app.on_close() save() end

return app

