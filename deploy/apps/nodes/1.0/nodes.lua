-- Nodes — who is on the mesh, sorted by how recently they were heard.
-- Scrollable list with a freshness dot, node type and last-heard age, plus a
-- summary of how many are currently "live" (heard in the last 15 minutes).
local ui, sys, mesh, timer = wada.ui, wada.sys, wada.mesh, wada.timer
local C = ui.colors
local app = {}

local TYPE = { [1]="chat", [2]="repeater", [3]="room", [4]="sensor" }
local ROWS = 24

local head, rows, W, narrow = nil, {}, 0, false

local function fmt_age(s)
  if s == 0 then return "now" end
  if s < 60 then return s .. "s" end
  if s < 3600 then return (s // 60) .. "m" end
  if s < 86400 then return (s // 3600) .. "h" end
  return (s // 86400) .. "d"
end

-- green = heard within 15 min, amber within 2 h, grey beyond
local function age_color(s)
  if s < 900 then return C.good end
  if s < 7200 then return 0xE8A33D end
  return C.sub
end

local function refresh()
  local list = mesh.contacts()
  table.sort(list, function(a, b) return a.ago_s < b.ago_s end)

  local live = 0
  for _, c in ipairs(list) do if c.ago_s < 900 then live = live + 1 end end
  head:set(string.format("%d node%s   %d heard recently", #list, #list == 1 and "" or "s", live))

  for i = 1, ROWS do
    local c = list[i]
    if c then
      local ty = TYPE[c.type] or ""
      if narrow then
        rows[i]:set(string.format("%-14s %5s", c.name:sub(1, 14), fmt_age(c.ago_s)))
      else
        rows[i]:set(string.format("%-22s %-9s %6s", c.name:sub(1, 22), ty, fmt_age(c.ago_s)))
      end
      rows[i]:color(age_color(c.ago_s))
    else
      rows[i]:set(i == 1 and #list == 0 and "no contacts yet" or "")
      rows[i]:color(C.sub)
    end
  end
end

function app.on_open(w, h)
  W, narrow = w, w < 280
  ui.scroll(true)
  local LH = ui.text_h(12)
  head = ui.label("", 4, 4, 14, C.accent)
  head:width(w - 10)
  local y = 4 + ui.text_h(14) + 6
  for i = 1, ROWS do
    rows[i] = ui.label("", 4, y, 12, C.text)
    rows[i]:width(w - 10)
    y = y + LH + 3
  end
  refresh()
  timer.every(5000)
end

function app.on_tick(dt) refresh() end

return app
