-- Breakout — wadamesh Lua app
-- Swipe left/right to move paddle  |  Swipe up to launch
-- Keyboard: arrows to move, Enter / Space to launch
-- Contributed by samuelcoustet

local ui, sys, store, timer = wada.ui, wada.sys, wada.store, wada.timer
local C = ui.colors
local app = {}

local COLS_B, ROWS_B = 8, 5
local BRICK_H, BRICK_GAP = 10, 2
local PADDLE_H, PADDLE_W = 7, 44
local BALL_R = 5
local TOP_H  = 18   -- score strip height
local PAD_OFF = 22  -- paddle distance from bottom

local COLORS = { 0xff3355, 0xff7722, 0xffdd33, 0x22cc77, 0x3399ff }
local PTS    = { 7, 5, 4, 3, 1 }

local cv, W, H, brick_w
local bricks, n_alive
local bx, by, bdx, bdy, speed
local paddle_x
local score, hiscore, lives, level
local launched, over

local function bk_x(c) return BRICK_GAP + (c-1)*(brick_w + BRICK_GAP) end
local function bk_y(r) return TOP_H + 4 + (r-1)*(BRICK_H + BRICK_GAP) end
local function pad_y() return H - PAD_OFF end

local function new_bricks()
  bricks = {}; n_alive = 0
  for r = 1, ROWS_B do
    bricks[r] = {}
    for c = 1, COLS_B do bricks[r][c] = true; n_alive = n_alive + 1 end
  end
end

local function reset_ball()
  launched = false
  bx = paddle_x
  by = pad_y() - BALL_R - 1
  bdx = 0; bdy = 0
end

local function launch_ball()
  local a = sys.random(-40, 40) * math.pi / 180.0
  bdx = speed * math.sin(a)
  bdy = -speed * math.cos(a)
  launched = true
end

local function draw_all()
  cv:fill(0x0a0e14)
  cv:text(6, 3, string.format("Score:%d  Best:%d  Lv:%d", score, hiscore, level), C.text, 11)
  for i = 1, lives do
    cv:circle(W - 6 - (i-1)*13, TOP_H // 2, 4, C.good, true)
  end
  for r = 1, ROWS_B do
    for c = 1, COLS_B do
      if bricks[r][c] then
        cv:rect(bk_x(c), bk_y(r), brick_w, BRICK_H, COLORS[r], true, 2)
      end
    end
  end
  cv:rect(paddle_x - PADDLE_W // 2, pad_y(), PADDLE_W, PADDLE_H, C.accent, true, 3)
  cv:circle(math.floor(bx), math.floor(by), BALL_R, 0xffffff, true)
  if not launched and not over then
    cv:text(W // 2 - 40, pad_y() - 20, "swipe up to launch", C.sub, 11)
  end
  if over then
    cv:rect(W // 2 - 62, H // 2 - 22, 124, 44, 0x141a22, true, 6)
    cv:text(W // 2 - 44, H // 2 - 14, "GAME  OVER", C.bad, 14)
    cv:text(W // 2 - 34, H // 2 + 4, "tap to restart", C.sub, 11)
  end
end

local function check_bricks()
  for r = 1, ROWS_B do
    for c = 1, COLS_B do
      if bricks[r][c] then
        local x1, y1 = bk_x(c), bk_y(r)
        local ox = math.min(bx + BALL_R - x1, x1 + brick_w - (bx - BALL_R))
        local oy = math.min(by + BALL_R - y1, y1 + BRICK_H - (by - BALL_R))
        if ox > 0 and oy > 0 then
          bricks[r][c] = false; n_alive = n_alive - 1
          score = score + PTS[r] * level
          if ox < oy then bdx = -bdx else bdy = -bdy end
          return
        end
      end
    end
  end
end

local function step()
  if over or not launched then return end
  bx = bx + bdx
  by = by + bdy
  -- walls
  if bx - BALL_R < 0        then bx = BALL_R;     bdx =  math.abs(bdx) end
  if bx + BALL_R > W        then bx = W - BALL_R; bdx = -math.abs(bdx) end
  if by - BALL_R < TOP_H    then by = TOP_H + BALL_R; bdy = math.abs(bdy) end
  -- paddle
  local py = pad_y()
  if bdy > 0 and by + BALL_R >= py and by + BALL_R <= py + PADDLE_H + speed + 1 then
    local half = PADDLE_W * 0.5
    if bx >= paddle_x - half - BALL_R and bx <= paddle_x + half + BALL_R then
      local rel = math.max(-1.0, math.min(1.0, (bx - paddle_x) / half))
      bdx = speed * rel * 0.8
      local sq = speed * speed - bdx * bdx
      bdy = -(sq > 0 and math.sqrt(sq) or speed * 0.5)
      by  = py - BALL_R - 1.0
    end
  end
  -- bricks
  check_bricks()
  -- ball lost
  if by - BALL_R > H + 10 then
    lives = lives - 1
    if lives <= 0 then
      if score > hiscore then hiscore = score; store.set("hiscore", hiscore) end
      over = true; launched = false; return
    end
    paddle_x = W // 2; reset_ball()
  end
  -- level won
  if n_alive == 0 then
    level  = level + 1
    speed  = math.min(speed + 0.6, 9.0)
    new_bricks(); reset_ball()
  end
end

local function move_paddle(dx)
  paddle_x = math.max(PADDLE_W // 2 + 2, math.min(W - PADDLE_W // 2 - 2, paddle_x + dx))
  if not launched then bx = paddle_x end
  draw_all()
end

function app.on_open(w, h)
  W, H = w, h
  hiscore = store.get("hiscore", 0)
  brick_w = math.floor((W - (COLS_B + 1) * BRICK_GAP) / COLS_B)
  cv = ui.canvas(W, H); cv:pos(0, 0)
  score = 0; lives = 3; level = 1; speed = 3.5; over = false
  paddle_x = W // 2
  new_bricks(); reset_ball()
  draw_all()
  timer.every(33)
end

function app.on_input(ev)
  if over then
    if ev.type == "down" or (ev.type == "key" and ev.key == "enter") then
      over = false; score = 0; lives = 3; level = 1; speed = 3.5
      paddle_x = W // 2; new_bricks(); reset_ball(); draw_all()
    end
    return
  end
  if ev.type == "swipe" then
    local d = ev.dir
    if     d == "left"  then move_paddle(-32)
    elseif d == "right" then move_paddle( 32)
    elseif d == "up" and not launched then launch_ball(); draw_all()
    end
  elseif ev.type == "key" then
    local k = ev.key
    if     k == "left"  then move_paddle(-20)
    elseif k == "right" then move_paddle( 20)
    elseif (k == "up" or k == "enter" or k == " ") and not launched then
      launch_ball(); draw_all()
    end
  elseif ev.type == "down" then
    if     ev.x < W // 3        then move_paddle(-32)
    elseif ev.x > (W * 2) // 3  then move_paddle( 32)
    elseif not launched          then launch_ball(); draw_all()
    end
  end
end

function app.on_tick(dt)
  step(); draw_all()
end

function app.on_close() end

return app
