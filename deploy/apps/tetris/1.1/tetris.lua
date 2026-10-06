-- Tetris — wadamesh Lua app
-- Swipe left/right to move  |  Swipe up to rotate  |  Swipe down to hard-drop
-- Keyboard: arrows to move/rotate, Enter or Space to hard-drop
-- Contributed by samuelcoustet

local ui, sys, store, timer = wada.ui, wada.sys, wada.store, wada.timer
local C = ui.colors

local app = {}

local COLS, ROWS = 10, 20
local CELL = 12

local COLORS = {
  0x00d4ff,  -- 1 I: cyan
  0xffe066,  -- 2 O: yellow
  0xcc44ff,  -- 3 T: purple
  0x00dd88,  -- 4 S: green
  0xff4455,  -- 5 Z: red
  0x3399ff,  -- 6 J: blue
  0xff8833,  -- 7 L: orange
}

-- Piece rotations: each entry is 4 {dr, dc} offsets from bounding-box origin
-- Row and column are 0-indexed; field coords = py+dr (row), px+dc (col)
local PIECES = {
  -- 1 I (4×4 bounding box)
  { {{1,0},{1,1},{1,2},{1,3}}, {{0,2},{1,2},{2,2},{3,2}},
    {{1,0},{1,1},{1,2},{1,3}}, {{0,1},{1,1},{2,1},{3,1}} },
  -- 2 O
  { {{0,0},{0,1},{1,0},{1,1}}, {{0,0},{0,1},{1,0},{1,1}},
    {{0,0},{0,1},{1,0},{1,1}}, {{0,0},{0,1},{1,0},{1,1}} },
  -- 3 T
  { {{0,1},{1,0},{1,1},{1,2}}, {{0,1},{1,1},{1,2},{2,1}},
    {{1,0},{1,1},{1,2},{2,1}}, {{0,1},{1,0},{1,1},{2,1}} },
  -- 4 S
  { {{0,1},{0,2},{1,0},{1,1}}, {{0,0},{1,0},{1,1},{2,1}},
    {{0,1},{0,2},{1,0},{1,1}}, {{0,0},{1,0},{1,1},{2,1}} },
  -- 5 Z
  { {{0,0},{0,1},{1,1},{1,2}}, {{0,1},{1,0},{1,1},{2,0}},
    {{0,0},{0,1},{1,1},{1,2}}, {{0,1},{1,0},{1,1},{2,0}} },
  -- 6 J
  { {{0,0},{1,0},{1,1},{1,2}}, {{0,1},{0,2},{1,1},{2,1}},
    {{1,0},{1,1},{1,2},{2,2}}, {{0,1},{1,1},{2,0},{2,1}} },
  -- 7 L
  { {{0,2},{1,0},{1,1},{1,2}}, {{0,1},{1,1},{2,1},{2,2}},
    {{1,0},{1,1},{1,2},{2,0}}, {{0,0},{0,1},{1,1},{2,1}} },
}

local board, cv, side_cv
local px, py, rot, pid, npid
local score, hiscore, level, cleared_total
local over
local SIDE_W = 58

local function new_board()
  local b = {}
  for r = 1, ROWS do
    b[r] = {}
    for c = 1, COLS do b[r][c] = 0 end
  end
  return b
end

local function get_cells(p, r, bx, by)
  local out = {}
  for _, off in ipairs(PIECES[p][r]) do
    out[#out+1] = {by + off[1], bx + off[2]}
  end
  return out
end

local function collides(p, r, bx, by)
  for _, fc in ipairs(get_cells(p, r, bx, by)) do
    local fr, fc2 = fc[1] + 1, fc[2] + 1  -- 0-indexed to 1-indexed
    if fc[2] < 0 or fc[2] >= COLS or fc[1] >= ROWS then return true end
    if fc[1] >= 0 and board[fr][fc2] ~= 0 then return true end
  end
  return false
end

local function lock_piece()
  for _, fc in ipairs(get_cells(pid, rot, px, py)) do
    local fr, fc2 = fc[1] + 1, fc[2] + 1
    if fr >= 1 then board[fr][fc2] = pid end
  end
  local cleared = 0
  local r = ROWS
  while r >= 1 do
    local full = true
    for c = 1, COLS do
      if board[r][c] == 0 then full = false; break end
    end
    if full then
      table.remove(board, r)
      table.insert(board, 1, {})
      for c = 1, COLS do board[1][c] = 0 end
      cleared = cleared + 1
    else
      r = r - 1
    end
  end
  if cleared > 0 then
    cleared_total = cleared_total + cleared
    local pts = {100, 300, 500, 800}
    score = score + (pts[cleared] or 800) * level
    level = math.max(1, math.floor(cleared_total / 10) + 1)
    timer.every(math.max(80, 700 - (level - 1) * 60))
  end
end

local function spawn()
  pid, npid = npid, sys.random(1, 7)
  px = math.floor((COLS - 4) / 2)  -- center the 4-wide bounding box
  py = 0
  rot = 1
  if collides(pid, rot, px, py) then
    over = true
    if score > hiscore then
      hiscore = score
      store.set("hiscore", hiscore)
      sys.toast("New high score: " .. hiscore, 2000)
    end
  end
end

local function draw_board()
  cv:fill(0x0d1117)
  for r = 1, ROWS do
    for c = 1, COLS do
      local v = board[r][c]
      if v ~= 0 then
        cv:rect((c-1)*CELL+1, (r-1)*CELL+1, CELL-2, CELL-2, COLORS[v], true, 2)
      end
    end
  end
  if not over then
    for _, fc in ipairs(get_cells(pid, rot, px, py)) do
      local r, c = fc[1]+1, fc[2]+1
      if r >= 1 then
        cv:rect((c-1)*CELL+1, (r-1)*CELL+1, CELL-2, CELL-2, COLORS[pid], true, 2)
      end
    end
  end
  if over then
    local mx = math.floor(COLS * CELL / 2)
    local my = math.floor(ROWS * CELL / 2)
    cv:rect(mx - 44, my - 18, 88, 36, 0x1a1f26, true, 4)
    cv:text(mx - 38, my - 12, "GAME OVER", C.bad, 14)
    cv:text(mx - 30, my + 4,  "tap to retry", C.sub, 11)
  end
end

local function draw_side()
  side_cv:fill(0x0d1117)
  side_cv:text(6, 4, "NEXT", C.sub, 11)
  local PC = 10
  local ox = math.floor((SIDE_W - 4 * PC) / 2)
  for _, off in ipairs(PIECES[npid][1]) do
    side_cv:rect(ox + off[2]*PC+1, 20 + off[1]*PC+1, PC-2, PC-2, COLORS[npid], true, 2)
  end
  side_cv:text(6, 76,  "SCORE", C.sub, 11)
  side_cv:text(6, 90,  tostring(score), C.text, 13)
  side_cv:text(6, 116, "BEST", C.sub, 11)
  side_cv:text(6, 130, tostring(hiscore), C.accent, 13)
  side_cv:text(6, 156, "LEVEL", C.sub, 11)
  side_cv:text(6, 170, tostring(level), C.good, 15)
end

local function try_rotate()
  local nr = (rot % 4) + 1
  if      not collides(pid, nr, px,   py) then rot = nr
  elseif  not collides(pid, nr, px-1, py) then px = px-1; rot = nr
  elseif  not collides(pid, nr, px+1, py) then px = px+1; rot = nr
  end
  draw_board()
end

local function hard_drop()
  while not collides(pid, rot, px, py+1) do py = py+1 end
  lock_piece()
  spawn()
  draw_board()
  draw_side()
end

local function reset()
  board = new_board()
  score, cleared_total, level = 0, 0, 1
  over = false
  npid = sys.random(1, 7)
  spawn()
  draw_board()
  draw_side()
  timer.every(700)
end

function app.on_open(w, h)
  hiscore = store.get("hiscore", 0)
  local board_w = COLS * CELL
  local board_h = ROWS * CELL
  local total_w = board_w + 4 + SIDE_W
  local ox = math.max(0, math.floor((w - total_w) / 2))
  local oy = math.max(0, math.floor((h - board_h) / 2))
  cv = ui.canvas(board_w, board_h)
  cv:pos(ox, oy)
  side_cv = ui.canvas(SIDE_W, board_h)
  side_cv:pos(ox + board_w + 4, oy)
  reset()
end

function app.on_input(ev)
  if over then
    if ev.type == "down" then reset() end
    return
  end
  if ev.type == "swipe" then
    local d = ev.dir
    if d == "left" then
      if not collides(pid, rot, px-1, py) then px = px-1; draw_board() end
    elseif d == "right" then
      if not collides(pid, rot, px+1, py) then px = px+1; draw_board() end
    elseif d == "up" then
      try_rotate()
    elseif d == "down" then
      hard_drop()
    end
  elseif ev.type == "key" then
    local k = ev.key
    if k == "left" then
      if not collides(pid, rot, px-1, py) then px = px-1; draw_board() end
    elseif k == "right" then
      if not collides(pid, rot, px+1, py) then px = px+1; draw_board() end
    elseif k == "up" then
      try_rotate()
    elseif k == "down" then
      if not collides(pid, rot, px, py+1) then py = py+1; draw_board() end
    elseif k == "enter" or k == " " then
      hard_drop()
    end
  end
end

function app.on_tick(dt)
  if over then return end
  if collides(pid, rot, px, py+1) then
    lock_piece()
    spawn()
    draw_board()
    draw_side()
  else
    py = py + 1
    draw_board()
  end
end

function app.on_close() end

return app
