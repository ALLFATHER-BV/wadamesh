-- Snake — wada.* reference app. Swipe (or trackball) to steer; eat the red
-- food, don't bite yourself. Tap after a game over to play again.
local ui, sys, store, timer = wada.ui, wada.sys, wada.store, wada.timer
local C = ui.colors

local CELL = 14
local app = {}
local cv, score_lbl
local cols, rows, px_w, px_h
local body, len, dx, dy, ndx, ndy, fx, fy
local score, hiscore, running, over

local function place_food()
  repeat
    fx, fy = sys.random(0, cols - 1), sys.random(0, rows - 1)
    local clash = false
    for i = 1, len do
      if body[i].x == fx and body[i].y == fy then clash = true break end
    end
  until not clash
end

local function reset()
  len, score, over, running = 3, 0, false, false
  dx, dy, ndx, ndy = 1, 0, 1, 0
  body = {}
  local cx, cy = math.floor(cols / 2), math.floor(rows / 2)
  for i = 1, len do body[i] = { x = cx - i + 1, y = cy } end
  place_food()
end

local function scoreline()
  score_lbl:set(string.format("Score %d   Best %d%s", score, hiscore,
                              running and "" or (over and "   -  tap to retry" or "   -  swipe to start")))
end

local function draw()
  cv:fill(0x101418)
  cv:rect(fx * CELL + 2, fy * CELL + 2, CELL - 4, CELL - 4, C.bad, true, 4)
  for i = len, 1, -1 do
    cv:rect(body[i].x * CELL + 1, body[i].y * CELL + 1, CELL - 2, CELL - 2,
            i == 1 and C.accent or C.good, true, 3)
  end
  if over then
    cv:text(math.floor(px_w / 2) - 40, math.floor(px_h / 2) - 10, "GAME OVER", C.text, 16)
  end
end

local function step()
  dx, dy = ndx, ndy
  local head = body[1]
  local nx, ny = head.x + dx, head.y + dy
  if nx < 0 or ny < 0 or nx >= cols or ny >= rows then over = true end
  if not over then
    for i = 1, len - 1 do
      if body[i].x == nx and body[i].y == ny then over = true break end
    end
  end
  if over then
    running = false
    if score > hiscore then
      hiscore = score
      store.set("hiscore", hiscore)
      sys.toast("New high score: " .. hiscore, 1600)
    end
    scoreline()
    draw()
    return
  end
  table.insert(body, 1, { x = nx, y = ny })
  if nx == fx and ny == fy then
    len = len + 1
    score = score + 10
    place_food()
    scoreline()
  else
    body[#body] = nil
  end
  draw()
end

function app.on_open(w, h)
  hiscore = store.get("hiscore", 0)
  cols = math.floor(w / CELL)
  rows = math.floor((h - 26) / CELL)
  px_w, px_h = cols * CELL, rows * CELL
  score_lbl = ui.label("", 4, 4, 14, C.text)
  cv = ui.canvas(px_w, px_h)
  cv:pos(math.floor((w - px_w) / 2), 26)
  reset()
  scoreline()
  draw()
  timer.every(120)
end

function app.on_input(ev)
  if ev.type == "swipe" then
    local d = ev.dir
    if     d == "up"    and dy ~= 1  then ndx, ndy = 0, -1
    elseif d == "down"  and dy ~= -1 then ndx, ndy = 0, 1
    elseif d == "left"  and dx ~= 1  then ndx, ndy = -1, 0
    elseif d == "right" and dx ~= -1 then ndx, ndy = 1, 0
    end
    if not running and not over then running = true; scoreline() end
  elseif ev.type == "down" and over then
    reset()
    scoreline()
    draw()
  end
end

function app.on_tick(dt)
  if running and not over then step() end
end

function app.on_close()
  -- high score is already persisted on set; nothing to tear down (the host
  -- frees every widget and the canvas with the app)
end

return app
