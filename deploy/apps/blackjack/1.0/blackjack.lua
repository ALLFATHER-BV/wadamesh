-- Blackjack 21 — WADAMESH Lua app for LilyGo T-Deck, T-Display P4 (AMOLED & LCD), & MeshCore boards
-- Controls:
--   Touch:     Tap any bottom-bar button, or tap the table after a hand to deal
--   Keyboard:  [H] Hit  [S] Stand  [D] Double/Deal  [P] Split  [Enter/Space] Select/Deal
--              [Left/Right] Cycle buttons  [Up/Down] Adjust bet  [M] Mute beep
--   Trackball: Swipe Left/Right to select button, Swipe Up/Down to adjust bet or Hit/Stand

local ui, sys, store, timer = wada.ui, wada.sys, wada.store, wada.timer
local C = ui.colors

local app = {}

-- Screen, device profile & layout metrics
local W, H = 320, 208
local cv
local MAX_CANVAS_DIM = 480 -- LuaAppHost.cpp enforces 1..480 per wada.ui.canvas call

local DEV = {
  id = "tdeck",
  name = "T-Deck",
  is_p4 = false,
  portrait = false,
  scale = 1,
}

local L = {
  top_h = 20,
  bot_h = 30,
  table_h = 158,
  cw = 34,
  ch = 46,
  overlap = 20,
  split_overlap = 16,
  pip_sm = 3,
  pip_lg = 5,
  f_sm = 12,
  f_md = 12,
  f_lg = 14,
  f_xl = 16,
  f_rank = 12,
  lh_sm = 16,
  lh_md = 16,
  lh_lg = 18,
  lh_xl = 20,
  lh_rank = 16,
  pad = 8,
  btn_gap = 4,
  card_rad = 4,
}

-- Palette tailored for casino table on dark WadaMesh UI
local COL_FELT       = 0x0B2B1E
local COL_FELT_DARK  = 0x082016
local COL_FELT_LINE  = 0x164A35
local COL_TOPBAR     = 0x0E1318
local COL_BOTBAR     = 0x0E1318
local COL_CARD_BG    = 0xF5F7FA
local COL_CARD_SHAD  = 0x06150E
local COL_CARD_BACK  = 0x1B4F8A
local COL_CARD_TRIM  = 0x4A8FE7
local COL_RED        = 0xD9383A
local COL_BLACK      = 0x181C24
local COL_GOLD       = 0xF2C14E
local COL_BTN        = 0x1A222B
local COL_BTN_ACT    = 0x1E3A3A
local COL_BTN_DIS    = 0x11161C
local COL_TEXT_DIM   = 0x525A65

-- Game constants
local NUM_DECKS  = 6
local MIN_BET    = 10
local MAX_BET    = 1000
local START_BANK = 500

local RANKS = { "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K" }
-- Suits: 1=Spade, 2=Heart, 3=Diamond, 4=Club

-- Persistent state
local bankroll  = START_BANK
local best_bank = START_BANK
local base_bet  = 25
local wins      = 0
local rounds    = 0
local sound_on  = true

-- Round state
-- state: "betting" | "player" | "dealer" | "done"
local state = "betting"
local shoe = {}
local dealer_hand = {}
local dealer_hide = true
local hands = {}       -- list of { cards = {}, bet = number, status = "play"|"stand"|"bust"|"bj"|"doubled", result = nil }
local cur_hand = 1
local banner_title = ""
local banner_sub = ""
local banner_col = 0x15B6A6

-- UI interactive buttons for current state
local active_btns = {}
local focus_idx = 1

-- Integer helper (LuaAppHost.cpp requires strict integers for all canvas calls)
local function I(x)
  return math.floor(tonumber(x) or 0)
end

local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

local function sfx()
  if sound_on and sys.beep then
    pcall(sys.beep)
  end
end

-- Safe RNG wrapper: LuaAppHost.cpp sysRandom(lo, hi) requires hi > lo
local function rand_int(lo, hi)
  lo, hi = I(lo), I(hi)
  if hi <= lo then return lo end
  local r = sys.random(lo, hi)
  if r < lo or r > hi then
    r = lo + (math.abs(r) % (hi - lo + 1))
  end
  return r
end

-- Safe text width & line height wrappers
local function text_w(str, sz)
  local s = tostring(str or "")
  local fsz = I(sz or L.f_sm)
  if ui.text_w then
    return I(ui.text_w(s, fsz))
  end
  return I(#s * (fsz + 4) * 0.48)
end

local function text_h(sz)
  local fsz = I(sz or L.f_sm)
  if ui.text_h then
    return I(ui.text_h(fsz))
  end
  return fsz + 4
end

-- Auto-detect WADAMESH device (T-Display P4 AMOLED, T-Display P4 LCD, T-Deck, Heltec V4, Pager, etc.)
-- Note: LuaAppHost passes body_w, body_h (where body_h = screen_h - app_bar_h).
--   LilyGo T-Display P4 AMOLED: native 568x1232 (portrait) or 1232x568 (landscape)
--   LilyGo T-Display P4 LCD:    native 540x1168 (portrait) or 1168x540 (landscape)
--   LilyGo T-Deck / M9 / L2:    native 320x240  (body ~320x208 or 320x196)
--   Heltec V4 / T-Deck Pro:     native 240x320  (body ~240x276)
--   LilyGo T-LoRa Pager:        native 480x222  (body ~480x178 or 222x436)
--   Tanmatsu / CrowPanel 5.0:   native 800x480  (body ~800x436 or 480x756)
local function detect_device(w, h)
  local short_side = math.min(w, h)
  local long_side  = math.max(w, h)
  local portrait   = (h > w)

  -- T-Display P4 AMOLED: panel is 568x1232 (in landscape w=1232, body_h=~500..568; in portrait w=568, body_h=~1150..1232)
  if long_side >= 1185 or (w == 568 and h >= 900) or (w >= 1185 and h >= 460) then
    DEV = {
      id = "p4_amoled",
      name = "T-Display P4 AMOLED",
      is_p4 = true,
      portrait = portrait,
      scale = 2,
    }
  -- T-Display P4 LCD (HI8561): panel is 540x1168 (in landscape w=1168, body_h=~460..540; in portrait w=540, body_h=~1080..1168)
  elseif (long_side >= 1050 and long_side < 1185) or (w == 540 and h >= 900) or (w >= 500 and w < 560 and h >= 900) then
    DEV = {
      id = "p4_lcd",
      name = "T-Display P4 LCD",
      is_p4 = true,
      portrait = portrait,
      scale = 2,
    }
  -- Other high-resolution large screens (e.g. Tanmatsu / CrowPanel 5.0 800x480)
  elseif long_side >= 700 or short_side >= 460 then
    DEV = {
      id = "large_hd",
      name = "Large HD",
      is_p4 = false,
      portrait = portrait,
      scale = 2,
    }
  -- Wide compact strip (e.g. T-LoRa Pager 480x178)
  elseif w >= 440 and h < 320 then
    DEV = {
      id = "pager",
      name = "Wide Compact",
      is_p4 = false,
      portrait = false,
      scale = 1,
    }
  -- Compact portrait (e.g. Heltec V4 240x276, T-Deck Pro 240x320, Pager Portrait 222x436)
  elseif portrait and w <= 260 then
    DEV = {
      id = "v4_portrait",
      name = "Compact Portrait",
      is_p4 = false,
      portrait = true,
      scale = 1,
    }
  -- Default: LilyGo T-Deck / standard landscape (320x208)
  else
    DEV = {
      id = "tdeck",
      name = "T-Deck",
      is_p4 = false,
      portrait = portrait,
      scale = 1,
    }
  end
end

-- Compute responsive layout metrics for the detected device & orientation
local function compute_layout()
  detect_device(W, H)

  if DEV.scale >= 2 then
    -- High-DPI / Large screen (LilyGo T-Display P4 AMOLED 568x1232, P4 LCD 540x1168, etc.)
    L.f_sm   = 14
    L.f_md   = 16
    L.f_lg   = 20
    L.f_xl   = 24
    L.f_rank = 16
  else
    -- Standard / Compact screen (T-Deck 320x208, Heltec V4 240x276, Pager 480x178)
    L.f_sm   = 12
    L.f_md   = 12
    L.f_lg   = 14
    L.f_xl   = 16
    L.f_rank = 12
  end

  L.lh_sm   = text_h(L.f_sm)
  L.lh_md   = text_h(L.f_md)
  L.lh_lg   = text_h(L.f_lg)
  L.lh_xl   = text_h(L.f_xl)
  L.lh_rank = text_h(L.f_rank)

  if DEV.scale >= 2 then
    L.top_h    = I(math.max(L.lh_md + 16, 40))
    L.bot_h    = DEV.portrait and I(math.max(L.lh_md + 42, 72)) or I(math.max(L.lh_md + 26, 54))
    L.pad      = 14
    L.btn_gap  = 8
    L.card_rad = 8
  else
    L.top_h    = I(math.max(L.lh_sm + 4, 20))
    L.bot_h    = I(math.max(L.lh_sm + 14, 30))
    L.pad      = 6
    L.btn_gap  = 4
    L.card_rad = 4
  end

  L.table_h = math.max(80, H - L.top_h - L.bot_h)

  if DEV.is_p4 and DEV.portrait then
    -- T-Display P4 Portrait (568x1232 AMOLED or 540x1168 LCD)
    L.cw            = I(clamp(W * 0.175, 88, 104))
    L.ch            = I(clamp(L.cw * 1.40, 122, 148))
    L.overlap       = I(L.cw * 0.55)
    L.split_overlap = I(L.cw * 0.38)
    L.pip_sm        = 6
    L.pip_lg        = 14
  elseif DEV.scale >= 2 then
    -- T-Display P4 Landscape (1232x568 or 1168x540) or Large HD
    L.ch            = I(clamp(L.table_h * 0.30, 88, 128))
    L.cw            = I(L.ch * 0.72)
    L.overlap       = I(L.cw * 0.56)
    L.split_overlap = I(L.cw * 0.44)
    L.pip_sm        = 5
    L.pip_lg        = 12
  elseif DEV.portrait then
    -- Compact Portrait (Heltec V4 240x276, Pager Portrait 222x436)
    L.cw            = I(clamp(W * 0.16, 32, 42))
    L.ch            = I(clamp(L.cw * 1.38, 44, 58))
    L.overlap       = I(L.cw * 0.54)
    L.split_overlap = I(L.cw * 0.38)
    L.pip_sm        = 3
    L.pip_lg        = 5
  else
    -- LilyGo T-Deck (320x208) & Wide Compact
    L.cw            = 34
    L.ch            = clamp(I(L.table_h * 0.29), 42, 48)
    L.overlap       = 20
    L.split_overlap = 16
    L.pip_sm        = 3
    L.pip_lg        = 5
  end
end

-- Create a full-screen canvas matching (W, H) without violating LuaAppHost.cpp's
-- 1..480 per-canvas dimension check on large displays like the LilyGo T-Display P4.
local function create_screen_canvas(w, h)
  w, h = I(w), I(h)
  if w <= MAX_CANVAS_DIM and h <= MAX_CANVAS_DIM then
    local single = ui.canvas(w, h)
    single:pos(0, 0)
    return single
  end

  local nx = math.ceil(w / MAX_CANVAS_DIM)
  local ny = math.ceil(h / MAX_CANVAS_DIM)
  local base_tw = I(math.ceil(w / nx))
  local base_th = I(math.ceil(h / ny))

  local tiles = {}
  for ry = 0, ny - 1 do
    local ty = ry * base_th
    local th = math.min(base_th, h - ty)
    for cx = 0, nx - 1 do
      local tx = cx * base_tw
      local tw = math.min(base_tw, w - tx)
      if tw > 0 and th > 0 then
        local tc = ui.canvas(tw, th)
        tc:pos(tx, ty)
        tiles[#tiles + 1] = {
          c  = tc,
          x  = tx,
          y  = ty,
          w  = tw,
          h  = th,
          x2 = tx + tw,
          y2 = ty + th,
        }
      end
    end
  end

  local vcv = { w = w, h = h, tiles = tiles }

  function vcv:pos(px, py)
    px, py = I(px), I(py)
    for i = 1, #tiles do
      local t = tiles[i]
      t.c:pos(px + t.x, py + t.y)
    end
  end

  function vcv:fill(col)
    col = I(col)
    for i = 1, #tiles do
      tiles[i].c:fill(col)
    end
  end

  function vcv:rect(x, y, rw, rh, col, filled, radius)
    x = x - ( x % 1 ); y = y - ( y % 1 )
    rw = rw - ( rw % 1 ); rh = rh - ( rh % 1 )
    col = col - ( col % 1 )
    local rad = radius ~= nil and (radius - (radius % 1)) or nil
    local rx2, ry2 = x + rw, y + rh
    for i = 1, #tiles do
      local t = tiles[i]
      if rx2 > t.x and x < t.x2 and ry2 > t.y and y < t.y2 then
        if rad ~= nil then
          t.c:rect(x - t.x, y - t.y, rw, rh, col, filled, rad)
        else
          t.c:rect(x - t.x, y - t.y, rw, rh, col, filled)
        end
      end
    end
  end

  function vcv:line(x1, y1, x2, y2, col, width)
    x1 = x1 - (x1 % 1); y1 = y1 - (y1 % 1)
    x2 = x2 - (x2 % 1); y2 = y2 - (y2 % 1)
    col = col - (col % 1)
    local lw = width ~= nil and (width - (width % 1)) or 1
    local lx1 = (x1 < x2 and x1 or x2) - lw
    local ly1 = (y1 < y2 and y1 or y2) - lw
    local lx2 = (x1 > x2 and x1 or x2) + lw + 1
    local ly2 = (y1 > y2 and y1 or y2) + lw + 1
    for i = 1, #tiles do
      local t = tiles[i]
      if lx2 > t.x and lx1 < t.x2 and ly2 > t.y and ly1 < t.y2 then
        t.c:line(x1 - t.x, y1 - t.y, x2 - t.x, y2 - t.y, col, lw)
      end
    end
  end

  function vcv:circle(x, y, r, col, filled)
    x = x - (x % 1); y = y - (y % 1); r = r - (r % 1); col = col - (col % 1)
    local cx1, cy1 = x - r - 2, y - r - 2
    local cx2, cy2 = x + r + 2, y + r + 2
    for i = 1, #tiles do
      local t = tiles[i]
      if cx2 > t.x and cx1 < t.x2 and cy2 > t.y and cy1 < t.y2 then
        t.c:circle(x - t.x, y - t.y, r, col, filled)
      end
    end
  end

  local function draw_text_segment(x, y, s, col, sz)
    x = x - (x % 1); y = y - (y % 1); col = col - (col % 1); sz = sz - (sz % 1)
    local tw = text_w(s, sz)
    local th = text_h(sz)
    -- LuaAppHost.cpp passes tile.w as max_width to lv_canvas_draw_text; split
    -- any string wider than a single tile so LVGL never wraps it mid-line.
    if tw > base_tw - 10 then
      local mid = math.floor(#s / 2)
      local split_pos = nil
      for d = 0, mid - 1 do
        if s:sub(mid + d, mid + d) == " " then split_pos = mid + d; break end
        if mid - d >= 1 and s:sub(mid - d, mid - d) == " " then split_pos = mid - d; break end
      end
      if split_pos and split_pos > 1 and split_pos < #s then
        local left_s = s:sub(1, split_pos - 1)
        local right_s = s:sub(split_pos + 1)
        draw_text_segment(x, y, left_s, col, sz)
        draw_text_segment(x + text_w(left_s .. " ", sz), y, right_s, col, sz)
        return
      end
    end
    local tx2, ty2 = x + tw + 4, y + th + 4
    for i = 1, #tiles do
      local t = tiles[i]
      if tx2 > t.x and x < t.x2 and ty2 > t.y and y < t.y2 then
        t.c:text(x - t.x, y - t.y, s, col, sz)
      end
    end
  end

  function vcv:text(x, y, s, col, size)
    draw_text_segment(x, y, tostring(s or ""), col, size or L.f_sm)
  end

  return vcv
end

-- Persistence & Home Widget
local function save_stats()
  if bankroll > best_bank then
    best_bank = bankroll
  end
  store.set("bank", bankroll)
  store.set("best", best_bank)
  store.set("bet", base_bet)
  store.set("wins", wins)
  store.set("rounds", rounds)
  if wada.widget and wada.widget.set then
    local pct = rounds > 0 and I((wins * 100) / rounds) or 0
    pcall(wada.widget.set, {
      title = "Blackjack",
      value = "$" .. tostring(bankroll),
      line  = string.format("Peak $%d  ·  Win %d%% (%d/%d)", best_bank, pct, wins, rounds)
    })
  end
end

-- Shoe management: pre-allocate the 312-card pool once and shuffle in-place
local shoe_top = 0

local function init_shoe_pool()
  if #shoe == NUM_DECKS * 52 then return end
  shoe = {}
  local idx = 1
  for _ = 1, NUM_DECKS do
    for s = 1, 4 do
      for r = 1, 13 do
        shoe[idx] = { r = r, s = s }
        idx = idx + 1
      end
    end
  end
end

local function build_shoe()
  init_shoe_pool()
  local n = #shoe
  for i = n, 2, -1 do
    local j = sys.random(1, i)
    if j < 1 or j > i then j = 1 + (math.abs(j) % i) end
    shoe[i], shoe[j] = shoe[j], shoe[i]
  end
  shoe_top = n
end

local function draw_card()
  if shoe_top < 52 then
    build_shoe()
    sys.toast("Shoe reshuffled")
  end
  local c = shoe[shoe_top]
  shoe_top = shoe_top - 1
  return c
end

-- Hand evaluation
local function card_val(r)
  if r == 1 then return 11 end
  if r >= 10 then return 10 end
  return r
end

local function eval_hand(cards)
  local total = 0
  local aces = 0
  for i = 1, #cards do
    local r = cards[i].r
    if r == 1 then
      aces = aces + 1
      total = total + 11
    elseif r >= 10 then
      total = total + 10
    else
      total = total + r
    end
  end
  while total > 21 and aces > 0 do
    total = total - 10
    aces = aces - 1
  end
  local is_soft = (aces > 0 and total <= 21)
  local is_bj = (#cards == 2 and total == 21)
  return total, is_soft, is_bj
end

local function can_split()
  if state ~= "player" or #hands ~= 1 then return false end
  local h = hands[1]
  if #h.cards ~= 2 then return false end
  if bankroll < h.bet then return false end
  return card_val(h.cards[1].r) == card_val(h.cards[2].r)
end

local function can_double()
  if state ~= "player" then return false end
  local h = hands[cur_hand]
  if not h or #h.cards ~= 2 then return false end
  return bankroll >= h.bet
end

-- Forward declarations
local rebuild_buttons
local draw_screen
local start_dealer_turn
local settle_round

-- Advance to next split hand or dealer turn
local function next_hand_or_dealer()
  if cur_hand < #hands then
    cur_hand = cur_hand + 1
    local h = hands[cur_hand]
    local total = eval_hand(h.cards)
    if total == 21 then
      h.status = "stand"
      next_hand_or_dealer()
      return
    end
    rebuild_buttons()
    draw_screen()
  else
    start_dealer_turn()
  end
end

-- Player actions
local function do_hit()
  if state ~= "player" then return end
  local h = hands[cur_hand]
  if not h or h.status ~= "play" then return end
  sfx()
  h.cards[#h.cards + 1] = draw_card()
  local total = eval_hand(h.cards)
  if total > 21 then
    h.status = "bust"
    next_hand_or_dealer()
  elseif total == 21 then
    h.status = "stand"
    next_hand_or_dealer()
  else
    rebuild_buttons()
    draw_screen()
  end
end

local function do_stand()
  if state ~= "player" then return end
  local h = hands[cur_hand]
  if not h or h.status ~= "play" then return end
  sfx()
  h.status = "stand"
  next_hand_or_dealer()
end

local function do_double()
  if not can_double() then return end
  local h = hands[cur_hand]
  sfx()
  bankroll = bankroll - h.bet
  h.bet = h.bet * 2
  h.cards[#h.cards + 1] = draw_card()
  local total = eval_hand(h.cards)
  if total > 21 then
    h.status = "bust"
  else
    h.status = "doubled"
  end
  next_hand_or_dealer()
end

local function do_split()
  if not can_split() then return end
  sfx()
  local h1 = hands[1]
  local c2 = h1.cards[2]
  h1.cards[2] = nil
  bankroll = bankroll - h1.bet

  local h2 = {
    cards = { c2, draw_card() },
    bet = h1.bet,
    status = "play"
  }
  h1.cards[2] = draw_card()
  hands[2] = h2
  cur_hand = 1

  local t1 = eval_hand(h1.cards)
  if t1 == 21 then
    h1.status = "stand"
    next_hand_or_dealer()
    return
  end
  rebuild_buttons()
  draw_screen()
end

-- Dealer turn & round settlement
start_dealer_turn = function()
  local any_alive = false
  for i = 1, #hands do
    if hands[i].status ~= "bust" then
      any_alive = true
      break
    end
  end

  dealer_hide = false
  if not any_alive then
    settle_round()
    return
  end

  local d_tot = eval_hand(dealer_hand)
  if d_tot < 17 then
    state = "dealer"
    rebuild_buttons()
    draw_screen()
    timer.every(420)
  else
    settle_round()
  end
end

settle_round = function()
  timer.stop()
  state = "done"
  dealer_hide = false

  local d_tot, _, d_bj = eval_hand(dealer_hand)
  local net_delta = 0
  local bj_win = false

  for i = 1, #hands do
    local h = hands[i]
    local p_tot, _, p_bj = eval_hand(h.cards)
    local is_natural_bj = (p_bj and #hands == 1 and h.status == "bj")

    if h.status == "bust" then
      h.result = "BUST"
      net_delta = net_delta - h.bet
    elseif is_natural_bj and not d_bj then
      local win_amt = I((h.bet * 3) / 2)
      bankroll = bankroll + h.bet + win_amt
      net_delta = net_delta + win_amt
      h.result = "BJ!"
      bj_win = true
    elseif d_bj and not is_natural_bj then
      h.result = "LOSE"
      net_delta = net_delta - h.bet
    elseif is_natural_bj and d_bj then
      bankroll = bankroll + h.bet
      h.result = "PUSH"
    elseif d_tot > 21 then
      bankroll = bankroll + h.bet * 2
      net_delta = net_delta + h.bet
      h.result = "WIN"
    elseif p_tot > d_tot then
      bankroll = bankroll + h.bet * 2
      net_delta = net_delta + h.bet
      h.result = "WIN"
    elseif p_tot == d_tot then
      bankroll = bankroll + h.bet
      h.result = "PUSH"
    else
      h.result = "LOSE"
      net_delta = net_delta - h.bet
    end
  end

  rounds = rounds + 1
  if net_delta > 0 then
    wins = wins + 1
  end

  if bj_win then
    banner_title = "BLACKJACK!"
    banner_sub = string.format("+$%d  (3:2 Payout)", net_delta)
    banner_col = COL_GOLD
  elseif net_delta > 0 then
    banner_title = d_tot > 21 and "DEALER BUSTS!" or "YOU WIN!"
    banner_sub = string.format("+$%d", net_delta)
    banner_col = C.good
  elseif net_delta < 0 then
    if #hands == 1 and hands[1].status == "bust" then
      banner_title = "BUST!"
    elseif d_bj then
      banner_title = "DEALER BLACKJACK"
    else
      banner_title = "DEALER WINS"
    end
    banner_sub = string.format("-$%d", math.abs(net_delta))
    banner_col = C.bad
  else
    banner_title = "PUSH"
    banner_sub = "Bet returned"
    banner_col = C.accent
  end

  if bankroll < MIN_BET then
    base_bet = MIN_BET
  elseif base_bet > bankroll then
    base_bet = bankroll
  end

  save_stats()
  sfx()
  rebuild_buttons()
  draw_screen()
end

-- Deal a new round
local function start_deal()
  if bankroll < MIN_BET then
    bankroll = START_BANK
    base_bet = 25
    sys.toast("Free $500 bailout chips!")
  end
  base_bet = clamp(base_bet, MIN_BET, math.min(MAX_BET, bankroll))
  bankroll = bankroll - base_bet

  dealer_hand = { draw_card(), draw_card() }
  dealer_hide = true
  hands = {
    {
      cards = { draw_card(), draw_card() },
      bet = base_bet,
      status = "play"
    }
  }
  cur_hand = 1
  banner_title = ""
  banner_sub = ""
  state = "player"
  sfx()

  local _, _, p_bj = eval_hand(hands[1].cards)
  local _, _, d_bj = eval_hand(dealer_hand)

  if p_bj or d_bj then
    if p_bj then hands[1].status = "bj" end
    dealer_hide = false
    settle_round()
    return
  end

  rebuild_buttons()
  draw_screen()
end

local function adjust_bet(delta)
  if bankroll < MIN_BET then
    bankroll = START_BANK
    sys.toast("Bailout: +$500 chips")
  end
  local max_allowed = math.min(MAX_BET, bankroll)
  base_bet = I(clamp(base_bet + delta, MIN_BET, max_allowed))
  sfx()
  rebuild_buttons()
  draw_screen()
end

-- Build bottom bar buttons based on state & screen width
rebuild_buttons = function()
  active_btns = {}
  local compact_btns = (W < 280)
  if state == "betting" then
    active_btns = {
      { label = "-50",  key = "1", enabled = true, fn = function() adjust_bet(-50) end },
      { label = "-10",  key = "-", enabled = true, fn = function() adjust_bet(-10) end },
      { label = "+10",  key = "+", enabled = true, fn = function() adjust_bet(10) end },
      { label = "+50",  key = "2", enabled = true, fn = function() adjust_bet(50) end },
      { label = "MAX",  key = "x", enabled = true, fn = function() base_bet = I(math.min(MAX_BET, math.max(MIN_BET, bankroll))); sfx(); rebuild_buttons(); draw_screen() end },
      { label = "DEAL", key = "d", primary = true, enabled = true, fn = start_deal },
    }
    focus_idx = 6
  elseif state == "player" then
    local dbl = can_double()
    local spl = can_split()
    active_btns = {
      { label = compact_btns and "HIT"   or "HIT [H]",   key = "h", primary = true, enabled = true, fn = do_hit },
      { label = compact_btns and "STAND" or "STAND [S]", key = "s", enabled = true, fn = do_stand },
      { label = compact_btns and "DBL"   or "DBL [D]",   key = "d", enabled = dbl,  fn = do_double },
      { label = compact_btns and "SPLIT" or "SPLIT [P]", key = "p", enabled = spl,  fn = do_split },
    }
    focus_idx = 1
  elseif state == "dealer" then
    active_btns = {
      { label = "DEALER DRAWING...", key = "", enabled = false, fn = function() end }
    }
    focus_idx = 1
  elseif state == "done" then
    local deal_amt = I(clamp(base_bet, MIN_BET, math.max(MIN_BET, bankroll)))
    active_btns = {
      { label = compact_btns and "BET" or "BET [B]", key = "b", enabled = true, fn = function() state = "betting"; rebuild_buttons(); draw_screen() end },
      { label = "-10", key = "-", enabled = bankroll >= MIN_BET + 10, fn = function() adjust_bet(-10) end },
      { label = "+10", key = "+", enabled = bankroll >= base_bet + 10, fn = function() adjust_bet(10) end },
      { label = string.format("DEAL $%d", deal_amt), key = "d", primary = true, enabled = true, fn = start_deal },
    }
    focus_idx = 4
  end
end

-- Scalable vector suit pip renderer (crisp at any radius r >= 3 across all LVGL font builds)
local function draw_suit_pip(x, y, suit, r)
  x, y, r = I(x), I(y), math.max(3, I(r))
  local col = (suit == 2 or suit == 3) and COL_RED or COL_BLACK
  local step = r >= 9 and 2 or 1

  -- Fast path: if using a tiled virtual canvas and this pip lies wholly within one tile,
  -- issue draw calls directly to that tile's native canvas with local coordinates.
  local tc = cv
  local ox, oy = 0, 0
  local tiles = cv.tiles
  if tiles then
    local x1, y1, x2, y2 = x - r - 2, y - r - 2, x + r + 2, y + r + 4
    for i = 1, #tiles do
      local t = tiles[i]
      if x1 >= t.x and x2 <= t.x2 and y1 >= t.y and y2 <= t.y2 then
        tc = t.c
        ox = t.x
        oy = t.y
        break
      end
    end
  end
  local px, py = x - ox, y - oy

  if suit == 1 then -- Spade
    local body_bot = I(r * 0.25)
    local span = math.max(1, r + body_bot)
    for dy = -r, body_bot, step do
      local hw = I(((dy + r) * r) / span)
      tc:rect(px - hw, py + dy, hw * 2 + 1, step, col, true)
    end
    local lr = math.max(1, I(r * 0.36))
    local lx = math.max(1, I(r * 0.42))
    local ly = I(r * 0.22)
    tc:circle(px - lx, py + ly, lr, col, true)
    tc:circle(px + lx, py + ly, lr, col, true)
    local stem_w = math.max(1, I(r * 0.22) * 2 + 1)
    local stem_top = I(r * 0.25)
    tc:rect(px - I(stem_w / 2), py + stem_top, stem_w, math.max(2, r - stem_top + 1), col, true)
    local base_hw = math.max(2, I(r * 0.55))
    tc:rect(px - base_hw, py + r, base_hw * 2 + 1, math.max(1, I(r * 0.18)), col, true)

  elseif suit == 2 then -- Heart
    local lr = math.max(1, I(r * 0.46))
    local lx = math.max(1, I(r * 0.46))
    local ly = I(r * 0.32)
    tc:circle(px - lx, py - ly, lr, col, true)
    tc:circle(px + lx, py - ly, lr, col, true)
    local top_dy = -I(r * 0.18)
    local span = math.max(1, r - top_dy)
    for dy = top_dy, r, step do
      local hw = math.max(0, I(((r - dy) * r) / span))
      tc:rect(px - hw, py + dy, hw * 2 + 1, step, col, true)
    end

  elseif suit == 3 then -- Diamond
    for dy = -r, r, step do
      local hw = math.max(0, r - math.abs(dy))
      tc:rect(px - hw, py + dy, hw * 2 + 1, step, col, true)
    end

  else -- Club
    local lr = math.max(1, I(r * 0.38))
    local lx = math.max(2, I(r * 0.48))
    local ty = I(r * 0.48)
    local by = I(r * 0.14)
    tc:circle(px, py - ty, lr, col, true)
    tc:circle(px - lx, py + by, lr, col, true)
    tc:circle(px + lx, py + by, lr, col, true)
    local stem_w = math.max(1, I(r * 0.22) * 2 + 1)
    tc:rect(px - I(stem_w / 2), py - I(r * 0.1), stem_w, math.max(2, r + I(r * 0.1) + 1), col, true)
    local base_hw = math.max(2, I(r * 0.52))
    tc:rect(px - base_hw, py + r, base_hw * 2 + 1, math.max(1, I(r * 0.18)), col, true)
  end
end

-- Draw a playing card at (x, y) of size (cw, ch)
local function draw_playing_card(x, y, cw, ch, card, hidden, pip_sm_ov, pip_lg_ov)
  x, y, cw, ch = I(x), I(y), I(cw), I(ch)
  local rad = cw >= 64 and 8 or 4
  local shad = cw >= 64 and 4 or 2
  local p_sm = pip_sm_ov or L.pip_sm
  local p_lg = pip_lg_ov or L.pip_lg
  local rank_fsz = (cw >= 64) and L.f_rank or 12

  -- Drop shadow
  cv:rect(x + shad, y + shad, cw, ch, COL_CARD_SHAD, true, rad)

  if hidden then
    local pad1 = cw >= 64 and 4 or 2
    local pad2 = cw >= 64 and 8 or 4
    cv:rect(x, y, cw, ch, COL_CARD_BG, true, rad)
    cv:rect(x + pad1, y + pad1, cw - pad1 * 2, ch - pad1 * 2, COL_CARD_BACK, true, math.max(2, rad - 1))
    cv:rect(x + pad2, y + pad2, cw - pad2 * 2, ch - pad2 * 2, COL_CARD_TRIM, false, math.max(2, rad - 2))
    local mx, my = I(x + cw / 2), I(y + ch / 2)
    local er = math.max(5, I(cw * 0.16))
    cv:circle(mx, my, er, COL_GOLD, false)
    cv:circle(mx, my, math.max(2, I(er * 0.42)), COL_GOLD, true)
    return
  end

  cv:rect(x, y, cw, ch, COL_CARD_BG, true, rad)
  cv:rect(x, y, cw, ch, 0xCBD2D9, false, rad)

  local col = (card.s == 2 or card.s == 3) and COL_RED or COL_BLACK
  local rstr = RANKS[card.r]
  local pad_x = cw >= 64 and 6 or 3
  local pad_y = cw >= 64 and 4 or 2
  cv:text(x + pad_x, y + pad_y, rstr, col, rank_fsz)

  -- Top-right mini suit pip
  if cw >= 30 then
    local pip_x = x + cw - p_sm - pad_x - 1
    local pip_y = y + pad_y + p_sm + 2
    draw_suit_pip(pip_x, pip_y, card.s, p_sm)
  end

  -- Bottom-left mini pip & bottom-right rank on large cards (T-Display P4)
  if cw >= 68 and ch >= 96 then
    local r_w = text_w(rstr, rank_fsz)
    local r_h = text_h(rank_fsz)
    cv:text(x + cw - pad_x - r_w, y + ch - pad_y - r_h, rstr, col, rank_fsz)
    draw_suit_pip(x + pad_x + p_sm + 1, y + ch - pad_y - p_sm - 3, card.s, p_sm)
  end

  -- Center large pip
  local center_y = (cw >= 68) and I(y + ch / 2) or (y + I(ch / 2) + 4)
  if ch >= 42 then
    draw_suit_pip(x + I(cw / 2), center_y, card.s, p_lg)
  else
    draw_suit_pip(x + I(cw / 2), y + I(ch / 2) + 3, card.s, p_sm)
  end
end

-- Draw a hand of cards centered around (cx, y)
local function draw_hand_cards(cards, cx, y, cw, ch, overlap, hide_second)
  local n = #cards
  if n == 0 then return end
  -- Tighten overlap automatically if a hand has many cards so it never clips off screen
  local max_span = math.max(cw + 12, W - 24)
  local eff_overlap = overlap
  if cw + (n - 1) * eff_overlap > max_span and n > 1 then
    eff_overlap = math.max(12, I((max_span - cw) / (n - 1)))
  end
  local total_w = cw + (n - 1) * eff_overlap
  local start_x = I(cx - total_w / 2)
  for i = 1, n do
    local hx = start_x + (i - 1) * eff_overlap
    local is_hidden = (hide_second and i == 2)
    draw_playing_card(hx, y, cw, ch, cards[i], is_hidden)
  end
end

-- Draw a centered pill badge (used in portrait layout on P4 & V4)
local function draw_badge_pill(cx, y, label_str, text_col, border_col, font_sz)
  local fsz = font_sz or L.f_md
  local lh = text_h(fsz)
  local tw = text_w(label_str, fsz)
  local pw = tw + (DEV.scale >= 2 and 28 or 16)
  local ph = lh + (DEV.scale >= 2 and 10 or 6)
  local px = I(cx - pw / 2)
  local py = I(y)
  cv:rect(px, py, pw, ph, 0x0D151C, true, 6)
  cv:rect(px, py, pw, ph, border_col or COL_FELT_LINE, false, 6)
  cv:text(I(cx - tw / 2), py + I((ph - lh) / 2), label_str, text_col or C.text, fsz)
  return ph
end

-- Main canvas render
draw_screen = function()
  local top_h   = L.top_h
  local bot_h   = L.bot_h
  local table_h = L.table_h
  local cw      = L.cw
  local ch      = L.ch
  local overlap = L.overlap

  -- Felt background & decorative casino border
  cv:fill(COL_FELT)
  local margin = DEV.scale >= 2 and 8 or 4
  cv:rect(margin, top_h + margin - 1, W - margin * 2, table_h - (margin - 1) * 2, COL_FELT_LINE, false, DEV.scale >= 2 and 12 or 8)

  -- Top status bar
  cv:rect(0, 0, W, top_h, COL_TOPBAR, true)
  cv:line(0, top_h, W, top_h, 0x1F2933, 1)

  local top_fsz = DEV.scale >= 2 and L.f_md or L.f_sm
  local top_lh  = text_h(top_fsz)
  local top_ty  = math.max(1, I((top_h - top_lh) / 2))

  local bank_str = string.format("BANK $%d", bankroll)
  cv:text(L.pad, top_ty, bank_str, COL_GOLD, top_fsz)

  local active_bet = base_bet
  if state ~= "betting" and #hands > 0 then
    active_bet = 0
    for i = 1, #hands do active_bet = active_bet + hands[i].bet end
  end
  local bet_str = string.format("BET $%d", active_bet)
  local bet_x = I((W - text_w(bet_str, top_fsz)) / 2)
  cv:text(bet_x, top_ty, bet_str, C.accent, top_fsz)

  local right_str = string.format("PEAK $%d%s", best_bank, sound_on and "" or " [M]")
  local rw = text_w(right_str, top_fsz)
  cv:text(math.max(bet_x + text_w(bet_str, top_fsz) + 6, W - rw - L.pad), top_ty, right_str, C.sub, top_fsz)

  if state == "betting" then
    local pad_x = DEV.scale >= 2 and 20 or 12
    local pad_y = DEV.scale >= 2 and 16 or 8
    local box_w = W - pad_x * 2
    local box_h = table_h - pad_y * 2
    local box_y = top_h + pad_y
    cv:rect(pad_x, box_y, box_w, box_h, COL_FELT_DARK, true, DEV.scale >= 2 and 12 or 8)
    cv:rect(pad_x, box_y, box_w, box_h, COL_FELT_LINE, false, DEV.scale >= 2 and 12 or 8)

    if DEV.portrait and DEV.scale >= 2 then
      -- High-res Portrait Betting Screen (LilyGo T-Display P4 AMOLED 568x1232 & LCD 540x1168)
      local title = "BLACKJACK 21"
      local sub   = "3:2 Natural Payout  ·  Dealer Stands on 17"
      local dev_s = string.format("%s  (%dx%d)", DEV.name, W, H)

      local cur_y = box_y + I(box_h * 0.07)
      cv:text(I((W - text_w(title, L.f_xl)) / 2), cur_y, title, COL_GOLD, L.f_xl)
      cur_y = cur_y + L.lh_xl + 10

      cv:text(I((W - text_w(sub, L.f_sm)) / 2), cur_y, sub, C.sub, L.f_sm)
      cur_y = cur_y + L.lh_sm + 6

      cv:text(I((W - text_w(dev_s, L.f_sm)) / 2), cur_y, dev_s, COL_FELT_LINE, L.f_sm)
      cur_y = cur_y + L.lh_sm + I(box_h * 0.05)

      -- Decorative feature cards (Ace of Spades + King of Hearts) centered side-by-side
      local dec_w, dec_h = cw, ch
      local dec_gap = 24
      local dec_x1 = I(W / 2 - dec_w - dec_gap / 2)
      local dec_x2 = I(W / 2 + dec_gap / 2)
      draw_playing_card(dec_x1, cur_y, dec_w, dec_h, { r = 1, s = 1 }, false)
      draw_playing_card(dec_x2, cur_y, dec_w, dec_h, { r = 13, s = 2 }, false)
      cur_y = cur_y + dec_h + I(box_h * 0.06)

      -- Prominent wager pill
      local wager_lbl = string.format("WAGER: $%d", base_bet)
      local ww = text_w(wager_lbl, L.f_xl)
      local pill_h = L.lh_xl + 24
      local pill_w = ww + 56
      cv:rect(I((W - pill_w) / 2), cur_y, pill_w, pill_h, COL_BTN_ACT, true, 10)
      cv:rect(I((W - pill_w) / 2), cur_y, pill_w, pill_h, C.accent, false, 10)
      cv:text(I((W - ww) / 2), cur_y + I((pill_h - L.lh_xl) / 2), wager_lbl, C.text, L.f_xl)
      cur_y = cur_y + pill_h + I(box_h * 0.05)

      -- Session stats summary panel
      local pct = rounds > 0 and I((wins * 100) / rounds) or 0
      local stat_str = string.format("Hands: %d   ·   Won: %d (%d%%)   ·   Shoe: %d cards", rounds, wins, pct, shoe_top)
      cv:text(I((W - text_w(stat_str, L.f_sm)) / 2), cur_y, stat_str, C.sub, L.f_sm)

      local hint = "Tap chip buttons below to adjust wager, then tap DEAL"
      cv:text(I((W - text_w(hint, L.f_sm)) / 2), box_y + box_h - L.lh_sm - 18, hint, C.sub, L.f_sm)
    else
      -- Standard / Landscape Betting Screen (T-Deck, P4 Landscape, Heltec V4, Pager)
      local dec_w = DEV.scale >= 2 and 64 or 30
      local dec_h = DEV.scale >= 2 and 90 or 42
      local dec_pip_sm = DEV.scale >= 2 and 4 or 3
      local dec_pip_lg = DEV.scale >= 2 and 9 or 5
      if W >= 280 then
        draw_playing_card(pad_x + 10, box_y + 10, dec_w, dec_h, { r = 1, s = 1 }, false, dec_pip_sm, dec_pip_lg)
        draw_playing_card(W - pad_x - dec_w - 10, box_y + 10, dec_w, dec_h, { r = 13, s = 2 }, false, dec_pip_sm, dec_pip_lg)
      end

      local title = "BLACKJACK 21"
      local title_y = box_y + 10
      cv:text(I((W - text_w(title, L.f_xl)) / 2), title_y, title, COL_GOLD, L.f_xl)

      local sub = "3:2 Payout  ·  Stands on 17"
      local sub_y = title_y + L.lh_xl + 4
      cv:text(I((W - text_w(sub, L.f_sm)) / 2), sub_y, sub, C.sub, L.f_sm)

      local wager_lbl = string.format("WAGER: $%d", base_bet)
      local ww = text_w(wager_lbl, L.f_xl)
      local pill_h = L.lh_xl + (DEV.scale >= 2 and 16 or 8)
      local pill_w = ww + (DEV.scale >= 2 and 40 or 28)
      local pill_y = math.max(sub_y + L.lh_sm + 10, I(box_y + (box_h - pill_h) * 0.54))
      cv:rect(I((W - pill_w) / 2), pill_y, pill_w, pill_h, COL_BTN_ACT, true, 6)
      cv:rect(I((W - pill_w) / 2), pill_y, pill_w, pill_h, C.accent, false, 6)
      cv:text(I((W - ww) / 2), pill_y + I((pill_h - L.lh_xl) / 2), wager_lbl, C.text, L.f_xl)

      local hint = W >= 300 and "Tap buttons or [Up/Down] Bet  [D] Deal" or "Tap buttons below to Bet & Deal"
      local hint_y = box_y + box_h - L.lh_sm - (DEV.scale >= 2 and 12 or 6)
      if hint_y > pill_y + pill_h + 4 then
        cv:text(I((W - text_w(hint, L.f_sm)) / 2), hint_y, hint, C.sub, L.f_sm)
      end
    end

  elseif DEV.portrait then
    -- Portrait Active Table Layout (LilyGo T-Display P4 AMOLED 568x1232, P4 LCD 540x1168, Heltec V4)
    local v_pad = DEV.scale >= 2 and 22 or 8
    local cx = I(W / 2)

    -- 1. Dealer section at top
    local d_val_str
    if dealer_hide then
      local up_val = card_val(dealer_hand[1].r)
      d_val_str = string.format("DEALER: %d + ?", up_val)
    else
      local dtot, _, dbj = eval_hand(dealer_hand)
      d_val_str = dbj and "DEALER: 21 (BLACKJACK)" or string.format("DEALER: %d", dtot)
    end
    local d_badge_h = draw_badge_pill(cx, top_h + v_pad, d_val_str, C.text, COL_FELT_LINE, L.f_md)
    local d_cards_y = top_h + v_pad + d_badge_h + (DEV.scale >= 2 and 14 or 6)
    draw_hand_cards(dealer_hand, cx, d_cards_y, cw, ch, overlap, dealer_hide)

    -- 2. Player section at bottom
    local p_cards_y = top_h + table_h - ch - v_pad
    local p_header_bottom = p_cards_y - (DEV.scale >= 2 and 12 or 6)

    if #hands == 1 then
      local h = hands[1]
      local ptot, psoft, pbj = eval_hand(h.cards)
      local p_val_str
      if pbj then
        p_val_str = string.format("PLAYER: 21 (BJ!)  ·  $%d", h.bet)
      elseif psoft and ptot <= 21 and state == "player" then
        p_val_str = string.format("PLAYER: %d / %d  ·  $%d", ptot - 10, ptot, h.bet)
      else
        p_val_str = string.format("PLAYER: %d  ·  $%d", ptot, h.bet)
      end
      local p_col = h.status == "bust" and C.bad or (pbj and COL_GOLD or C.text)
      local p_bdr = h.status == "bust" and C.bad or (pbj and COL_GOLD or C.accent)
      local p_badge_h = L.lh_md + (DEV.scale >= 2 and 10 or 6)
      draw_badge_pill(cx, p_header_bottom - p_badge_h, p_val_str, p_col, p_bdr, L.f_md)
      draw_hand_cards(h.cards, cx, p_cards_y, cw, ch, overlap, false)
    else
      -- Split hands side-by-side
      for idx = 1, 2 do
        local h = hands[idx]
        local h_cx = idx == 1 and I(W * 0.27) or I(W * 0.73)
        local is_active = (state == "player" and cur_hand == idx)
        local box_hw = I(W * 0.22)
        if is_active then
          cv:rect(h_cx - box_hw, p_cards_y - 4, box_hw * 2, ch + 8, C.accent, false, 6)
        end
        local ptot = eval_hand(h.cards)
        local tag = h.result or (h.status == "bust" and "BUST" or string.format("$%d", h.bet))
        local lbl = string.format("H%d: %d (%s)", idx, ptot, tag)
        local lcol = is_active and C.accent or (h.status == "bust" and C.bad or C.text)
        cv:text(h_cx - I(text_w(lbl, L.f_sm) / 2), p_header_bottom - L.lh_sm, lbl, lcol, L.f_sm)
        draw_hand_cards(h.cards, h_cx, p_cards_y, cw, ch, L.split_overlap, false)
      end
    end

    -- 3. Center table strip (Result Banner or Table Rules)
    local mid_top = d_cards_y + ch + 8
    local mid_bot = p_header_bottom - (L.lh_md + 16)
    local mid_cy  = I((mid_top + mid_bot) / 2)

    if state == "done" and banner_title ~= "" then
      local pad_b = DEV.scale >= 2 and 44 or 28
      local bw = math.min(W - 32, math.max(DEV.scale >= 2 and 280 or 164,
        math.max(text_w(banner_title, L.f_lg), text_w(banner_sub, L.f_md)) + pad_b))
      local bh = L.lh_lg + L.lh_md + (DEV.scale >= 2 and 28 or 12)
      local bx = I((W - bw) / 2)
      local by = I(mid_cy - bh / 2)
      cv:rect(bx, by, bw, bh, 0x0D131A, true, 8)
      cv:rect(bx, by, bw, bh, banner_col, false, 8)
      local t_y = by + (DEV.scale >= 2 and 8 or 3)
      local s_y = t_y + L.lh_lg + (DEV.scale >= 2 and 6 or 2)
      cv:text(I((W - text_w(banner_title, L.f_lg)) / 2), t_y, banner_title, banner_col, L.f_lg)
      cv:text(I((W - text_w(banner_sub, L.f_md)) / 2), s_y, banner_sub, C.text, L.f_md)
    else
      local line_m = DEV.scale >= 2 and 48 or 24
      cv:line(line_m, mid_cy, W - line_m, mid_cy, COL_FELT_LINE, 1)
      if DEV.scale >= 2 and (mid_bot - mid_top) >= L.lh_sm * 2 + 20 then
        local rule_txt = "BLACKJACK PAYS 3 TO 2  ·  DEALER STANDS ON ALL 17"
        cv:text(I((W - text_w(rule_txt, L.f_sm)) / 2), mid_cy - L.lh_sm - 8, rule_txt, COL_FELT_LINE, L.f_sm)
        local act_txt = state == "dealer" and "Dealer is drawing..." or "Choose your action below"
        cv:text(I((W - text_w(act_txt, L.f_sm)) / 2), mid_cy + 8, act_txt, C.sub, L.f_sm)
      end
    end

  else
    -- Landscape Active Table Layout (LilyGo T-Deck 320x208, P4 Landscape, Pager)
    local d_cards_y = top_h + (DEV.scale >= 2 and 12 or 6)
    local cards_cx  = I(W * 0.58)
    local lbl_x     = DEV.scale >= 2 and 22 or 12

    local d_val_str
    if dealer_hide then
      local up_val = card_val(dealer_hand[1].r)
      d_val_str = string.format("%d + ?", up_val)
    else
      local dtot, _, dbj = eval_hand(dealer_hand)
      d_val_str = dbj and "21 (BJ)" or tostring(dtot)
    end
    cv:text(lbl_x, d_cards_y + 4, "DEALER", C.sub, L.f_sm)
    cv:text(lbl_x, d_cards_y + 4 + L.lh_sm + 2, d_val_str, C.text, L.f_lg)
    draw_hand_cards(dealer_hand, cards_cx, d_cards_y, cw, ch, overlap, dealer_hide)

    local p_cards_y = top_h + table_h - ch - (DEV.scale >= 2 and 12 or 6)

    if #hands == 1 then
      local h = hands[1]
      local ptot, psoft, pbj = eval_hand(h.cards)
      local p_val_str
      if pbj then
        p_val_str = "21 (BJ!)"
      elseif psoft and ptot <= 21 and state == "player" then
        p_val_str = string.format("%d / %d", ptot - 10, ptot)
      else
        p_val_str = tostring(ptot)
      end
      local p_col = h.status == "bust" and C.bad or (pbj and COL_GOLD or C.text)
      cv:text(lbl_x, p_cards_y + 4, "PLAYER", C.sub, L.f_sm)
      cv:text(lbl_x, p_cards_y + 4 + L.lh_sm + 2, p_val_str, p_col, L.f_lg)
      draw_hand_cards(h.cards, cards_cx, p_cards_y, cw, ch, overlap, false)
    else
      for idx = 1, 2 do
        local h = hands[idx]
        local h_cx = idx == 1 and I(W * 0.28) or I(W * 0.74)
        local is_active = (state == "player" and cur_hand == idx)
        local box_hw = DEV.scale >= 2 and 140 or 60
        if is_active then
          cv:rect(h_cx - box_hw, p_cards_y - 2, box_hw * 2, ch + 4, C.accent, false, 5)
        end
        local ptot = eval_hand(h.cards)
        local tag = h.result or (h.status == "bust" and "BUST" or string.format("$%d", h.bet))
        local lbl = string.format("H%d:%d (%s)", idx, ptot, tag)
        local lcol = is_active and C.accent or (h.status == "bust" and C.bad or C.text)
        cv:text(h_cx - I(text_w(lbl, L.f_sm) / 2), p_cards_y - L.lh_sm - 2, lbl, lcol, L.f_sm)
        draw_hand_cards(h.cards, h_cx, p_cards_y, cw, ch, L.split_overlap, false)
      end
    end

    local mid_top = d_cards_y + ch + 3
    local mid_bot = (#hands > 1) and (p_cards_y - L.lh_sm - 4) or (p_cards_y - 3)
    local mid_cy  = I((mid_top + mid_bot) / 2)

    if state == "done" and banner_title ~= "" then
      local bw = math.min(W - 24, math.max(164,
        math.max(text_w(banner_title, L.f_lg), text_w(banner_sub, L.f_sm)) + 36))
      local bh = L.lh_lg + L.lh_sm + (DEV.scale >= 2 and 14 or 4)
      local bx = I((W - bw) / 2)
      local by = I(mid_cy - bh / 2)
      cv:rect(bx, by, bw, bh, 0x0D131A, true, 6)
      cv:rect(bx, by, bw, bh, banner_col, false, 6)
      cv:text(I((W - text_w(banner_title, L.f_lg)) / 2), by + 2, banner_title, banner_col, L.f_lg)
      cv:text(I((W - text_w(banner_sub, L.f_sm)) / 2), by + 2 + L.lh_lg, banner_sub, C.text, L.f_sm)
    else
      cv:line(24, mid_cy, W - 24, mid_cy, COL_FELT_LINE, 1)
    end
  end

  -- Bottom button bar
  local by = H - bot_h
  cv:rect(0, by, W, bot_h, COL_BOTBAR, true)
  cv:line(0, by, W, by, 0x1F2933, 1)

  local nb = #active_btns
  if nb > 0 then
    local gap = L.btn_gap
    local pad_v = DEV.scale >= 2 and 8 or 3
    local bw = I((W - gap * (nb + 1)) / nb)
    local bh = bot_h - pad_v * 2
    local btn_fsz = DEV.scale >= 2 and L.f_md or L.f_sm
    local btn_lh  = text_h(btn_fsz)
    local btn_rad = DEV.scale >= 2 and 8 or 4

    for i = 1, nb do
      local b = active_btns[i]
      local bx = gap + (i - 1) * (bw + gap)
      local bby = by + pad_v
      b.x, b.y, b.w, b.h = bx, bby, bw, bh

      local bg = not b.enabled and COL_BTN_DIS or (b.primary and COL_BTN_ACT or COL_BTN)
      cv:rect(bx, bby, bw, bh, bg, true, btn_rad)

      if i == focus_idx and b.enabled then
        cv:rect(bx, bby, bw, bh, b.primary and COL_GOLD or C.accent, false, btn_rad)
      elseif b.enabled then
        cv:rect(bx, bby, bw, bh, 0x2B3642, false, btn_rad)
      end

      local tcol = not b.enabled and COL_TEXT_DIM or (b.primary and COL_GOLD or C.text)
      local lbl = b.label
      local tw = text_w(lbl, btn_fsz)
      -- Strip hotkey suffix if button is too narrow for full label
      if tw > bw - 6 and lbl:find(" %[") then
        lbl = lbl:gsub(" %[.*%]", "")
        tw = text_w(lbl, btn_fsz)
      end
      local tx = bx + math.max(2, I((bw - tw) / 2))
      local ty = bby + math.max(1, I((bh - btn_lh) / 2))
      cv:text(tx, ty, lbl, tcol, btn_fsz)
    end
  end
end

-- Move focus among enabled buttons
local function cycle_focus(dir)
  local n = #active_btns
  if n == 0 then return end
  for _ = 1, n do
    focus_idx = ((focus_idx - 1 + dir) % n) + 1
    if active_btns[focus_idx] and active_btns[focus_idx].enabled then
      break
    end
  end
  draw_screen()
end

local function trigger_focused()
  local b = active_btns[focus_idx]
  if b and b.enabled and b.fn then
    b.fn()
  end
end

-- WADAMESH Lifecycle Callbacks
function app.on_open(w, h)
  local brd = (sys.board and sys.board()) or {}
  W = I((w and w > 0) and w or ((brd.w and brd.w > 0) and brd.w or 320))
  H = I((h and h > 0) and h or ((brd.h and brd.h > 0) and brd.h or 208))

  compute_layout()

  bankroll  = I(tonumber(store.get("bank", START_BANK)) or START_BANK)
  best_bank = I(tonumber(store.get("best", START_BANK)) or START_BANK)
  base_bet  = I(tonumber(store.get("bet", 25)) or 25)
  wins      = I(tonumber(store.get("wins", 0)) or 0)
  rounds    = I(tonumber(store.get("rounds", 0)) or 0)

  if bankroll < MIN_BET then bankroll = START_BANK end
  base_bet = I(clamp(base_bet, MIN_BET, math.min(MAX_BET, bankroll)))

  cv = create_screen_canvas(W, H)

  build_shoe()
  state = "betting"
  rebuild_buttons()
  draw_screen()
end

function app.on_tick(dt)
  if state ~= "dealer" then
    timer.stop()
    return
  end
  local d_tot = eval_hand(dealer_hand)
  if d_tot < 17 then
    sfx()
    dealer_hand[#dealer_hand + 1] = draw_card()
    if eval_hand(dealer_hand) >= 17 then
      settle_round()
    else
      draw_screen()
    end
  else
    settle_round()
  end
end

function app.on_input(ev)
  if not ev or state == "dealer" then return end

  if ev.type == "down" then
    local ex, ey = tonumber(ev.x) or 0, tonumber(ev.y) or 0
    -- Hit-test bottom bar buttons
    for i = 1, #active_btns do
      local b = active_btns[i]
      if b.x and ex >= b.x and ex <= b.x + b.w and ey >= b.y and ey <= b.y + b.h then
        if b.enabled and b.fn then
          focus_idx = i
          b.fn()
        end
        return
      end
    end
    -- Tap top-right area to toggle sound
    if ey <= L.top_h and ex >= W - I(W * 0.28) then
      sound_on = not sound_on
      sys.toast(sound_on and "Sound ON" or "Sound OFF")
      draw_screen()
      return
    end
    -- Tap felt after hand finishes to deal immediately
    if state == "done" and ey > L.top_h + 2 and ey < H - L.bot_h - 2 then
      start_deal()
      return
    end

  elseif ev.type == "swipe" then
    local d = ev.dir
    if d == "left" then
      cycle_focus(-1)
    elseif d == "right" then
      cycle_focus(1)
    elseif d == "up" then
      if state == "betting" or state == "done" then
        adjust_bet(25)
      elseif state == "player" then
        do_hit()
      end
    elseif d == "down" then
      if state == "betting" or state == "done" then
        adjust_bet(-25)
      elseif state == "player" then
        do_stand()
      end
    end

  elseif ev.type == "key" then
    local k = tostring(ev.key or ""):lower()
    if k == "left" or k == "a" then
      cycle_focus(-1)
    elseif k == "right" or k == "l" then
      cycle_focus(1)
    elseif k == "up" or k == "w" or k == "+" or k == "=" then
      if state == "betting" or state == "done" then
        adjust_bet(10)
      end
    elseif k == "down" or k == "z" or k == "-" then
      if state == "betting" or state == "done" then
        adjust_bet(-10)
      end
    elseif k == "enter" or k == " " then
      trigger_focused()
    elseif k == "m" then
      sound_on = not sound_on
      sys.toast(sound_on and "Sound ON" or "Sound OFF")
      draw_screen()
    elseif state == "player" then
      if k == "h" then do_hit()
      elseif k == "s" then do_stand()
      elseif k == "d" then do_double()
      elseif k == "p" then do_split()
      end
    elseif state == "betting" or state == "done" then
      if k == "d" or k == "h" then start_deal()
      elseif k == "b" and state == "done" then
        state = "betting"
        rebuild_buttons()
        draw_screen()
      elseif k == "c" then
        base_bet = MIN_BET
        draw_screen()
      elseif k == "x" then
        base_bet = math.min(MAX_BET, math.max(MIN_BET, bankroll))
        draw_screen()
      end
    end
  end
end

function app.on_close()
  save_stats()
end

return app
