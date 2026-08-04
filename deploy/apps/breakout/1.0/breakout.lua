-- Breakout — swipe or use the trackball to steer the paddle, clear the bricks.
-- Shows the canvas + per-frame tick side of the API (Snake is the grid version).
local ui, sys, store, timer = wada.ui, wada.sys, wada.store, wada.timer
local C = ui.colors
local app = {}

local cv, score_lbl
local W, H
local bx, by, vx, vy, px, pw
local bricks, rows_n, cols_n, bw, bh
local score, lives, best, playing, over

local BALL_R = 3
local PAD_H = 5

local function reset_ball()
  bx, by = W / 2, H * 0.6
  vx, vy = (sys.random(0, 1) == 0 and -1 or 1) * 1.6, -1.9
end

local function build_bricks()
  bricks = {}
  cols_n = math.max(5, math.floor(W / 34))
  rows_n = 4
  bw = math.floor((W - 8) / cols_n)
  bh = 9
  for r = 1, rows_n do
    bricks[r] = {}
    for c = 1, cols_n do bricks[r][c] = true end
  end
end

local function status()
  score_lbl:set(string.format("Score %d   Lives %d   Best %d%s",
    score, lives, best, playing and "" or (over and "   -  tap to retry" or "   -  swipe to start")))
end

local function draw()
  cv:fill(0x101418)
  local BCOL = { C.accent, 0x4F9DF7, 0xA784E0, 0xE8A33D }
  for r = 1, rows_n do
    for c = 1, cols_n do
      if bricks[r][c] then
        cv:rect(4 + (c - 1) * bw, 4 + (r - 1) * (bh + 3), bw - 2, bh, BCOL[r] or C.good, true, 2)
      end
    end
  end
  cv:rect(math.floor(px), H - 12, pw, PAD_H, C.text, true, 2)
  cv:circle(math.floor(bx), math.floor(by), BALL_R, C.good, true)
  if over then cv:text(math.floor(W / 2) - 40, math.floor(H / 2) - 8, "GAME OVER", C.bad, 16) end
end

local function step()
  bx = bx + vx
  by = by + vy
  if bx < BALL_R then bx, vx = BALL_R, -vx end
  if bx > W - BALL_R then bx, vx = W - BALL_R, -vx end
  if by < BALL_R then by, vy = BALL_R, -vy end

  -- paddle
  if vy > 0 and by >= H - 12 - BALL_R and by <= H - 6 and bx >= px and bx <= px + pw then
    vy = -vy
    vx = vx + ((bx - (px + pw / 2)) / (pw / 2)) * 0.8   -- english off the paddle
    if vx > 3 then vx = 3 elseif vx < -3 then vx = -3 end
  end

  -- bricks
  for r = 1, rows_n do
    for c = 1, cols_n do
      if bricks[r][c] then
        local x1, y1 = 4 + (c - 1) * bw, 4 + (r - 1) * (bh + 3)
        if bx >= x1 and bx <= x1 + bw and by >= y1 and by <= y1 + bh then
          bricks[r][c] = false
          vy = -vy
          score = score + 10
          status()
        end
      end
    end
  end

  -- lost the ball
  if by > H then
    lives = lives - 1
    if lives <= 0 then
      playing, over = false, true
      if score > best then best = score; store.set("best", best); sys.toast("New best: " .. best, 1600) end
    else
      reset_ball()
    end
    status()
  end

  -- cleared
  local left = 0
  for r = 1, rows_n do for c = 1, cols_n do if bricks[r][c] then left = left + 1 end end end
  if left == 0 then
    build_bricks()
    score = score + 50
    reset_ball()
    sys.toast("cleared! next wave", 1200)
    status()
  end
  draw()
end

local function new_game()
  score, lives, over, playing = 0, 3, false, false
  pw = math.max(34, math.floor(W / 5))
  px = (W - pw) / 2
  build_bricks()
  reset_ball()
  status()
  draw()
end

function app.on_open(w, h)
  best = store.get("best", 0)
  score_lbl = ui.label("", 4, 4, 12, C.text)
  score_lbl:width(w - 10)
  local top = ui.text_h(12) + 8
  W, H = w - 8, h - top - 4
  cv = ui.canvas(W, H)
  cv:pos(4, top)
  new_game()
  timer.every(40)
end

function app.on_input(ev)
  if ev.type == "swipe" then
    if ev.dir == "left"  then px = math.max(0, px - pw * 0.6) end
    if ev.dir == "right" then px = math.min(W - pw, px + pw * 0.6) end
    if not playing and not over then playing = true; status() end
  elseif ev.type == "down" then
    if over then new_game()
    else
      px = math.max(0, math.min(W - pw, ev.x - pw / 2))   -- drag the paddle straight to the touch
      if not playing then playing = true; status() end
    end
  end
end

function app.on_tick(dt) if playing and not over then step() end end

return app
