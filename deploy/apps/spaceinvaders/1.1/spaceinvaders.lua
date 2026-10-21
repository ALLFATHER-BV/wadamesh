-- SPDX-License-Identifier: GPL-3.0-or-later
-- Space Invaders for WADAMESH
-- Move: left/right (or A/D) keys, trackball/swipe left-right, or tap left/right third
-- Fire: up / Enter / space / swipe up / tap the middle third
-- Start/restart: Enter, space, swipe up or tap

local app = {}
local C = wada.ui.colors
local fl = math.floor

local ROWS, COLS = 4, 8
local TOP_H = 16
local SY, AH = 14, 10          -- alien row spacing, alien height
local PW, PH = 20, 7           -- player size
local SC, SR, CW, CH = 5, 3, 6, 5  -- shield cols/rows, cell size
local ROWC = { 0xff3355, 0xffaa22, 0x33dd77, 0x33aaff }
local PTS = { 30, 20, 10, 10 }

local cv, W, H
local SX, AW, STEP
local py, shield_y, ufo_y
local px
local alive, n_alive
local fx, fy, fdir, anim, last_step
local pb, abul, ufo
local shields
local score, hi, lives, level
local state = "ready"
local last, resume_at, next_fire, next_ufo, over_at = 0, 0, 0, 0, 0
local msg = nil

local function clamp(v, lo, hi_)
  if v < lo then return lo end
  if v > hi_ then return hi_ end
  return v
end

-- integer in a..b. Uses math.random, not wada.sys.random: the latter
-- misbehaves for tiny ranges, which crashed the game with few aliens left.
local function rnd(a, b)
  if b <= a then return a end
  return math.floor(math.random() * (b - a + 1)) + a
end

-- drawing helpers: the canvas wants whole numbers
local function rect(x, y, w, h, c)
  cv:rect(fl(x), fl(y), fl(w), fl(h), c, true)
end
local function text(x, y, s, c, size)
  cv:text(fl(x), fl(y), s, c, size)
end
local function center(s, y, size, c)
  text((W - wada.ui.text_w(s, size)) / 2, y, s, c, size)
end

local function make_shields()
  shields = {}
  for i = 1, 4 do
    local s = { x = fl((i - 0.5) * W / 4 - SC * CW / 2), cells = {} }
    for r = 1, SR do
      s.cells[r] = {}
      for c = 1, SC do s.cells[r][c] = true end
    end
    shields[i] = s
  end
end

local function new_wave()
  alive = {}
  for r = 1, ROWS do
    alive[r] = {}
    for c = 1, COLS do alive[r][c] = true end
  end
  n_alive = ROWS * COLS
  fx = 8
  fy = TOP_H + 18 + math.min(level - 1, 4) * 8
  fdir = 1
  anim = 0
  last_step = wada.sys.millis()
  pb = nil
  abul = {}
  make_shields()
end

local function init_game()
  score, lives, level = 0, 3, 1
  px = fl((W - PW) / 2)
  ufo = nil
  new_wave()
  local now = wada.sys.millis()
  next_ufo = now + 15000
  next_fire = now + 1000
  resume_at = 0
end

local function start()
  init_game()
  state = "play"
  last = wada.sys.millis()
  resume_at = last + 500
end

local function game_over()
  state = "over"
  over_at = wada.sys.millis()
  if score > hi then
    hi = score
    wada.store.set("hi", tostring(hi))
  end
  wada.sys.beep()
end

-- Returns true if the bullet chewed a shield cell.
-- dir -1: bullet moving up (check bottom row first); 1: moving down.
local function shield_hit(bx, by, bh, dir)
  for i = 1, #shields do
    local s = shields[i]
    if bx >= s.x and bx < s.x + SC * CW and by < shield_y + SR * CH and by + bh > shield_y then
      local c = fl((bx - s.x) / CW) + 1
      local r0, r1, st = 1, SR, 1
      if dir < 0 then r0, r1, st = SR, 1, -1 end
      for r = r0, r1, st do
        if s.cells[r][c] then
          local cy0 = shield_y + (r - 1) * CH
          if by < cy0 + CH and by + bh > cy0 then
            s.cells[r][c] = false
            return true
          end
        end
      end
    end
  end
  return false
end

local function kill_alien(r, c)
  alive[r][c] = false
  n_alive = n_alive - 1
  score = score + PTS[r]
  wada.sys.beep()
end

local function step_formation(now)
  local minc, maxc, low = COLS + 1, 0, 0
  for r = 1, ROWS do
    for c = 1, COLS do
      if alive[r][c] then
        if c < minc then minc = c end
        if c > maxc then maxc = c end
        if r > low then low = r end
      end
    end
  end
  if maxc == 0 then return end
  local left = fx + (minc - 1) * SX
  local right = fx + (maxc - 1) * SX + AW
  if (fdir > 0 and right + STEP > W - 2) or (fdir < 0 and left - STEP < 2) then
    fy = fy + 8
    fdir = -fdir
  else
    fx = fx + fdir * STEP
  end
  anim = 1 - anim
  local bottom = fy + (low - 1) * SY + AH
  if bottom >= shield_y then
    for i = 1, #shields do
      for r = 1, SR do
        for c = 1, SC do shields[i].cells[r][c] = false end
      end
    end
  end
  if bottom >= py then game_over() end
end

local function alien_fire(now)
  local cols = {}
  for c = 1, COLS do
    for r = 1, ROWS do
      if alive[r][c] then cols[#cols + 1] = c; break end
    end
  end
  if #cols == 0 then return end
  local c = cols[rnd(1, #cols)] or cols[1]
  local row = 1
  for r = ROWS, 1, -1 do
    if alive[r][c] then row = r; break end
  end
  abul[#abul + 1] = {
    x = fx + (c - 1) * SX + AW // 2,
    y = fy + (row - 1) * SY + AH,
  }
end

local function update(dt, now)
  -- player bullet
  if pb then
    pb.y = pb.y - 220 * dt
    if pb.y + 8 < TOP_H then
      pb = nil
    else
      local tx, ty = pb.x + 1, pb.y
      local c = fl((tx - fx) / SX) + 1
      local r = fl((ty - fy) / SY) + 1
      if r >= 1 and r <= ROWS and c >= 1 and c <= COLS and alive[r][c] then
        local ox = tx - (fx + (c - 1) * SX)
        local oy = ty - (fy + (r - 1) * SY)
        if ox <= AW and oy <= AH then
          kill_alien(r, c)
          pb = nil
        end
      end
      if pb and ufo and tx >= ufo.x and tx <= ufo.x + 20 and ty < ufo_y + 7 then
        score = score + ({ 50, 100, 150 })[rnd(1, 3)]
        ufo = nil
        pb = nil
        wada.sys.beep()
      end
      if pb and shield_hit(pb.x, pb.y, 8, -1) then pb = nil end
    end
  end

  -- wave cleared
  if n_alive == 0 then
    level = level + 1
    new_wave()
    resume_at = now + 800
    return
  end

  -- formation movement
  local iv = math.max(35, 50 + n_alive * (14 - math.min(level - 1, 6)))
  if now - last_step >= iv then
    last_step = now
    step_formation(now)
    if state ~= "play" then return end
  end

  -- alien fire
  if now >= next_fire and #abul < 3 then
    alien_fire(now)
    next_fire = now + math.max(250, rnd(500, 1300) - level * 40)
  end

  -- alien bullets
  local bspd = 110 + math.min(level, 8) * 6
  for i = #abul, 1, -1 do
    local b = abul[i]
    b.y = b.y + bspd * dt
    if b.y > H then
      table.remove(abul, i)
    elseif shield_hit(b.x, b.y, 8, 1) then
      table.remove(abul, i)
    elseif b.x + 2 > px and b.x < px + PW and b.y + 8 > py and b.y < py + PH then
      lives = lives - 1
      abul = {}
      pb = nil
      resume_at = now + 1200
      wada.sys.beep()
      if lives <= 0 then game_over() end
      return
    end
  end

  -- UFO
  if ufo then
    ufo.x = ufo.x + ufo.dir * 80 * dt
    if ufo.x < -24 or ufo.x > W + 4 then ufo = nil end
  elseif now >= next_ufo and fy > TOP_H + 12 then
    local d = rnd(1, 2) == 1 and 1 or -1
    ufo = { x = d > 0 and -20 or W, dir = d }
    next_ufo = now + rnd(12000, 25000)
  end
end

local function draw()
  cv:fill(C.bg)
  text(4, 2, "Score " .. score .. "  Hi " .. hi .. "  Lv " .. level, C.text, 12)
  for i = 1, lives do
    rect(W - 10 - (i - 1) * 12, 5, 9, 5, C.good)
  end

  -- shields (one rect per run of cells)
  for i = 1, #shields do
    local s = shields[i]
    for r = 1, SR do
      local c = 1
      while c <= SC do
        if s.cells[r][c] then
          local e = c
          while e < SC and s.cells[r][e + 1] do e = e + 1 end
          rect(s.x + (c - 1) * CW, shield_y + (r - 1) * CH, (e - c + 1) * CW, CH, C.good)
          c = e + 1
        else
          c = c + 1
        end
      end
    end
  end

  -- aliens
  for r = 1, ROWS do
    for c = 1, COLS do
      if alive[r][c] then
        local x = fx + (c - 1) * SX
        local y = fy + (r - 1) * SY
        rect(x, y, AW, 7, ROWC[r])
        if anim == 0 then
          rect(x + 1, y + 7, AW - 2, 3, ROWC[r])
        else
          rect(x + 3, y + 7, AW - 6, 3, ROWC[r])
        end
      end
    end
  end

  if ufo then
    rect(ufo.x, ufo_y, 20, 7, C.bad)
  end

  -- player (blinks while recovering)
  local now = wada.sys.millis()
  if now >= resume_at or (now // 100) % 2 == 0 then
    rect(px, py, PW, PH, C.accent)
    rect(px + PW // 2 - 1, py - 3, 2, 3, C.accent)
  end

  if pb then rect(pb.x, pb.y, 2, 8, C.text) end
  for i = 1, #abul do rect(abul[i].x, abul[i].y, 2, 8, C.warn or 0xE8A33D) end

  if state == "ready" then
    center("SPACE INVADERS", H * 0.18, 16, C.text)
    center("Left/right to move", H * 0.40, 12, C.sub)
    center("Up / Enter / tap middle to fire", H * 0.47, 12, C.sub)
    center("Tap or Enter to start", H * 0.60, 12, C.sub)
  elseif state == "over" then
    center("GAME OVER", H * 0.30, 24, C.bad)
    center("Score " .. score .. "   Hi " .. hi, H * 0.46, 14, C.sub)
    center("Tap or Enter to play again", H * 0.58, 12, C.sub)
  end
end

local function move(d)
  px = clamp(px + d, 2, W - PW - 2)
end

local function fire()
  if state == "play" and not pb and wada.sys.millis() >= resume_at then
    pb = { x = px + PW // 2 - 1, y = py - 8 }
  end
end

local function try_start()
  if wada.sys.millis() - over_at > 800 then start() end
end

function app.on_open(w, h)
  W, H = w, h
  SX = fl(W / 11)
  AW = fl(SX * 0.6)
  STEP = math.max(2, fl(W / 80))
  py = H - 14
  shield_y = py - 36
  ufo_y = TOP_H + 1
  hi = tonumber(wada.store.get("hi")) or 0
  cv = wada.ui.canvas(W, H)
  cv:pos(0, 0)
  init_game()
  last = wada.sys.millis()
  wada.timer.every(40)
  draw()
end

function app.on_tick()
  local now = wada.sys.millis()
  local dt = clamp((now - last) / 1000, 0, 0.045)
  last = now
  if state == "play" and now >= resume_at then update(dt, now) end
  draw()
end

function app.on_input(ev)
  if ev.type == "key" then
    local k = ev.key
    if k == "left" or k == "a" then
      move(-14)
    elseif k == "right" or k == "d" then
      move(14)
    elseif k == "up" or k == "w" or k == "enter" or k == " " then
      if state == "play" then fire() else try_start() end
    end
  elseif ev.type == "swipe" then
    if ev.dir == "left" then
      move(-22)
    elseif ev.dir == "right" then
      move(22)
    elseif ev.dir == "up" then
      if state == "play" then fire() else try_start() end
    end
  elseif ev.type == "down" then
    if state ~= "play" then
      try_start()
    elseif ev.x < W // 3 then
      move(-22)
    elseif ev.x > (W * 2) // 3 then
      move(22)
    else
      fire()
    end
  end
end

function app.on_close()
  if score and score > hi then wada.store.set("hi", tostring(score)) end
end

return app
