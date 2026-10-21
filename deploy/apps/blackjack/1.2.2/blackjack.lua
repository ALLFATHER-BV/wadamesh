-- Blackjack 21 (ver 1.2.2) — WADAMESH Lua app for LilyGo T-Deck, T-Display P4 (AMOLED/LCD), & MeshCore boards
local ui, sys, store, timer = wada.ui, wada.sys, wada.store, wada.timer
local C = ui.colors
local app = {}

local W, H, cv = 320, 208, nil
local dev_name, is_p4, portrait, s2 = "T-Deck", false, false, false
local top_h, bot_h, tbl_h, cw, ch, ov, s_ov = 20, 30, 158, 34, 46, 20, 16
local p_sm, p_lg, f_sm, f_md, f_lg, f_xl, f_rk = 3, 5, 12, 12, 14, 16, 12
local lh_sm, lh_md, lh_lg, lh_xl, pad, gap = 16, 16, 18, 20, 6, 4

local K_FELT, K_FDK, K_FLN, K_BAR = 0x0B2B1E, 0x082016, 0x164A35, 0x0E1318
local K_CBG, K_CSH, K_CBK, K_CTR = 0xF5F7FA, 0x06150E, 0x1B4F8A, 0x4A8FE7
local K_RED, K_BLK, K_GLD = 0xD9383A, 0x181C24, 0xF2C14E
local K_BTN, K_BACT, K_BDIS, K_DIM = 0x1A222B, 0x1E3A3A, 0x11161C, 0x525A65

local MIN_BET, MAX_BET, START_BANK = 10, 1000, 500
local RANKS = { "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K" }

local bank, best_bank, base_bet = START_BANK, START_BANK, 25
local wins, rounds, sound_on = 0, 0, true
local state, d_hide = "betting", true
local shoe, d_hand, hands, btns = {}, {}, {}, {}
local shoe_top, cur_hand, f_idx = 0, 1, 1
local b_tit, b_sub, b_col = "", "", 0x15B6A6

local function I(x) return math.floor(tonumber(x) or 0) end
local function clamp(v, lo, hi) return v < lo and lo or (v > hi and hi or v) end
local function sfx() if sound_on and sys.beep then pcall(sys.beep) end end
local function tw(str, sz)
 local s, f = tostring(str or ""), I(sz or f_sm)
 return ui.text_w and I(ui.text_w(s, f)) or I(#s * (f + 4) * 0.48)
end
local function th(sz)
 local f = I(sz or f_sm)
 return ui.text_h and I(ui.text_h(f)) or (f + 4)
end
local function tcx(cx, y, s, col, sz) cv:text(I(cx - tw(s, sz) / 2), y, s, col, sz) end
local function box(x, y, w, h, bg, fg, r)
 cv:rect(x, y, w, h, bg, true, r)
 if fg then cv:rect(x, y, w, h, fg, false, r) end
end

local function compute_layout()
 local s, l = math.min(W, H), math.max(W, H)
 portrait = (H > W)
 local aspect = l / math.max(1, s)
 if l >= 1185 or (W == 568 and H >= 900) or (W >= 1185 and H >= 460) then
  dev_name, is_p4, s2 = "T-Display P4 AMOLED", true, true
 elseif (l >= 1050 and l < 1185) or (W == 540 and H >= 900) or (W >= 500 and W < 560 and H >= 900) then
  dev_name, is_p4, s2 = "T-Display P4 LCD", true, true
 elseif l >= 700 or s >= 460 then
  dev_name, is_p4, s2 = "Large HD", false, true
 elseif (portrait and aspect > 1.9 and s <= 300) or is_p4 then
  dev_name, is_p4, s2 = "T-Display P4", true, false
 else
  dev_name, is_p4, s2 = (W >= 440 and H < 320) and "Wide Compact" or ((portrait and W <= 280) and "Compact Portrait" or "T-Deck"), false, false
 end
 if s2 then f_sm, f_md, f_lg, f_xl, f_rk = 14, 16, 20, 24, 16
 else f_sm, f_md, f_lg, f_xl, f_rk = 12, 12, 14, 16, 12 end
 lh_sm, lh_md, lh_lg, lh_xl = th(f_sm), th(f_md), th(f_lg), th(f_xl)
 if s2 then
  top_h, pad, gap = I(math.max(lh_md + 16, 40)), (portrait or is_p4) and 22 or 14, 8
  if is_p4 and portrait then
   bot_h = 114
  else
   bot_h = portrait and I(math.max(lh_md + 42, 72)) or I(math.max(lh_md + 26, 54))
  end
 else
  top_h, pad, gap = I(math.max(lh_sm + 4, 20)), 6, 4
  if is_p4 and portrait then
   bot_h = 70
  else
   bot_h = I(math.max(lh_sm + 14, 30))
  end
 end
 tbl_h = math.max(80, H - top_h - bot_h)
 if s2 and is_p4 and portrait then
  cw = I(clamp(W * 0.175, 88, 104)); ch = I(clamp(cw * 1.40, 122, 148))
  ov, s_ov, p_sm, p_lg = I(cw * 0.55), I(cw * 0.38), 6, 14
 elseif s2 then
  ch = I(clamp(tbl_h * 0.30, 88, 128)); cw = I(ch * 0.72)
  ov, s_ov, p_sm, p_lg = I(cw * 0.56), I(cw * 0.44), 5, 12
 elseif portrait then
  cw = I(clamp(W * 0.16, 32, 42)); ch = I(clamp(cw * 1.38, 44, 58))
  ov, s_ov, p_sm, p_lg = I(cw * 0.54), I(cw * 0.38), 3, 5
 else
  cw, ch, ov, s_ov, p_sm, p_lg = 34, clamp(I(tbl_h * 0.29), 42, 48), 20, 16, 3, 5
 end
end

local function create_screen_canvas(w, h)
 w, h = I(w), I(h)
 if w <= 480 and h <= 480 then local s = ui.canvas(w, h); s:pos(0, 0); return s end
 local nx, ny = math.ceil(w / 480), math.ceil(h / 480)
 local btw, bth, tiles = I(math.ceil(w / nx)), I(math.ceil(h / ny)), {}
 for ry = 0, ny - 1 do
  local ty, th_t = ry * bth, math.min(bth, h - ry * bth)
  for cx = 0, nx - 1 do
   local tx, tw_t = cx * btw, math.min(btw, w - cx * btw)
   if tw_t > 0 and th_t > 0 then
    local tc = ui.canvas(tw_t, th_t); tc:pos(tx, ty)
    tiles[#tiles + 1] = { c = tc, x = tx, y = ty, x2 = tx + tw_t, y2 = ty + th_t }
   end
  end
 end
 local vcv = { tiles = tiles }
 function vcv:fill(col) col = I(col); for i = 1, #tiles do tiles[i].c:fill(col) end end
 function vcv:rect(x, y, rw, rh, col, filled, r)
  x, y, rw, rh, col = I(x), I(y), I(rw), I(rh), I(col)
  local rad, x2, y2 = r and I(r) or nil, x + rw, y + rh
  for i = 1, #tiles do
   local t = tiles[i]
   if x2 > t.x and x < t.x2 and y2 > t.y and y < t.y2 then
    if rad then t.c:rect(x - t.x, y - t.y, rw, rh, col, filled, rad) else t.c:rect(x - t.x, y - t.y, rw, rh, col, filled) end
   end
  end
 end
 function vcv:line(x1, y1, x2, y2, col, lw)
  x1, y1, x2, y2, col, lw = I(x1), I(y1), I(x2), I(y2), I(col), lw and I(lw) or 1
  local lx1, ly1, lx2, ly2 = math.min(x1, x2) - lw, math.min(y1, y2) - lw, math.max(x1, x2) + lw + 1, math.max(y1, y2) + lw + 1
  for i = 1, #tiles do
   local t = tiles[i]
   if lx2 > t.x and lx1 < t.x2 and ly2 > t.y and ly1 < t.y2 then t.c:line(x1 - t.x, y1 - t.y, x2 - t.x, y2 - t.y, col, lw) end
  end
 end
 function vcv:circle(x, y, r, col, filled)
  x, y, r, col = I(x), I(y), I(r), I(col)
  for i = 1, #tiles do
   local t = tiles[i]
   if x + r + 2 > t.x and x - r - 2 < t.x2 and y + r + 2 > t.y and y - r - 2 < t.y2 then t.c:circle(x - t.x, y - t.y, r, col, filled) end
  end
 end
 local function seg(x, y, s, col, sz)
  x, y, col, sz = I(x), I(y), I(col), I(sz or f_sm)
  local sw, sh = tw(s, sz), th(sz)
  if sw > btw - 10 then
   local mid, sp = math.floor(#s / 2), nil
   for d = 0, mid - 1 do
    if s:sub(mid + d, mid + d) == " " then sp = mid + d; break end
    if mid - d >= 1 and s:sub(mid - d, mid - d) == " " then sp = mid - d; break end
   end
   if sp and sp > 1 and sp < #s then
    local ls = s:sub(1, sp - 1)
    seg(x, y, ls, col, sz); seg(x + tw(ls .. " ", sz), y, s:sub(sp + 1), col, sz); return
   end
  end
  for i = 1, #tiles do
   local t = tiles[i]
   if x + sw + 4 > t.x and x < t.x2 and y + sh + 4 > t.y and y < t.y2 then t.c:text(x - t.x, y - t.y, s, col, sz) end
  end
 end
 function vcv:text(x, y, s, col, sz) seg(x, y, tostring(s or ""), col, sz) end
 return vcv
end

local function save_stats()
 if bank > best_bank then best_bank = bank end
 store.set("bank", bank); store.set("best", best_bank)
 store.set("bet", base_bet); store.set("wins", wins); store.set("rounds", rounds)
 if wada.widget and wada.widget.set then
  local pct = rounds > 0 and I((wins * 100) / rounds) or 0
  pcall(wada.widget.set, {
   title = "Blackjack", value = "$" .. tostring(bank),
   line = string.format("Peak $%d  ·  Win %d%% (%d/%d)", best_bank, pct, wins, rounds)
  })
 end
end

local function build_shoe()
 if #shoe ~= 312 then
  shoe = {}
  local idx = 1
  for _ = 1, 6 do for s = 1, 4 do for r = 1, 13 do shoe[idx] = { r = r, s = s }; idx = idx + 1 end end end
 end
 for i = #shoe, 2, -1 do
  local j = sys.random(1, i)
  if j < 1 or j > i then j = 1 + (math.abs(j) % i) end
  shoe[i], shoe[j] = shoe[j], shoe[i]
 end
 shoe_top = #shoe
end

local function draw_card()
 if shoe_top < 52 then build_shoe(); sys.toast("Shoe reshuffled") end
 local c = shoe[shoe_top]; shoe_top = shoe_top - 1; return c
end

local function card_val(r) return r == 1 and 11 or (r >= 10 and 10 or r) end

local function eval_hand(cards)
 local total, aces = 0, 0
 for i = 1, #cards do
  local r = cards[i].r
  if r == 1 then aces = aces + 1; total = total + 11
  elseif r >= 10 then total = total + 10
  else total = total + r end
 end
 while total > 21 and aces > 0 do total = total - 10; aces = aces - 1 end
 return total, (aces > 0 and total <= 21), (#cards == 2 and total == 21)
end

local function can_split()
 if state ~= "player" or #hands ~= 1 then return false end
 local h = hands[1]
 return #h.cards == 2 and bank >= h.bet and card_val(h.cards[1].r) == card_val(h.cards[2].r)
end

local function can_double()
 if state ~= "player" then return false end
 local h = hands[cur_hand]
 return (h ~= nil) and (#h.cards == 2) and (bank >= h.bet)
end

local build_btns, draw_screen, start_dealer_turn, settle_round

local function next_hand_or_dealer()
 if cur_hand < #hands then
  cur_hand = cur_hand + 1
  local h = hands[cur_hand]
  if eval_hand(h.cards) == 21 then h.status = "stand"; next_hand_or_dealer(); return end
  build_btns(); draw_screen()
 else
  start_dealer_turn()
 end
end

local function do_hit()
 if state ~= "player" then return end
 local h = hands[cur_hand]
 if not h or h.status ~= "play" then return end
 sfx(); h.cards[#h.cards + 1] = draw_card()
 local total = eval_hand(h.cards)
 if total > 21 then h.status = "bust"; next_hand_or_dealer()
 elseif total == 21 then h.status = "stand"; next_hand_or_dealer()
 else build_btns(); draw_screen() end
end

local function do_stand()
 if state ~= "player" then return end
 local h = hands[cur_hand]
 if not h or h.status ~= "play" then return end
 sfx(); h.status = "stand"; next_hand_or_dealer()
end

local function do_double()
 if not can_double() then return end
 local h = hands[cur_hand]
 sfx(); bank = bank - h.bet; h.bet = h.bet * 2
 h.cards[#h.cards + 1] = draw_card()
 h.status = (eval_hand(h.cards) > 21) and "bust" or "doubled"
 next_hand_or_dealer()
end

local function do_split()
 if not can_split() then return end
 sfx()
 local h1 = hands[1]
 local c2 = h1.cards[2]
 bank = bank - h1.bet
 h1.cards[2] = draw_card()
 hands[2] = { cards = { c2, draw_card() }, bet = h1.bet, status = "play" }
 cur_hand = 1
 if eval_hand(h1.cards) == 21 then h1.status = "stand"; next_hand_or_dealer(); return end
 build_btns(); draw_screen()
end

start_dealer_turn = function()
 local alive = false
 for i = 1, #hands do if hands[i].status ~= "bust" then alive = true; break end end
 d_hide = false
 if not alive then settle_round(); return end
 if eval_hand(d_hand) < 17 then state = "dealer"; build_btns(); draw_screen(); timer.every(420)
 else settle_round() end
end

settle_round = function()
 timer.stop(); state = "done"; d_hide = false
 local d_tot, _, d_bj = eval_hand(d_hand)
 local net, bj_win = 0, false
 for i = 1, #hands do
  local h = hands[i]
  local p_tot, _, p_bj = eval_hand(h.cards)
  local nat = (p_bj and #hands == 1 and h.status == "bj")
  if h.status == "bust" then h.result = "BUST"; net = net - h.bet
  elseif nat and not d_bj then
   local w = I((h.bet * 3) / 2)
   bank = bank + h.bet + w; net = net + w; h.result = "BJ!"; bj_win = true
  elseif d_bj and not nat then h.result = "LOSE"; net = net - h.bet
  elseif nat and d_bj then bank = bank + h.bet; h.result = "PUSH"
  elseif d_tot > 21 or p_tot > d_tot then bank = bank + h.bet * 2; net = net + h.bet; h.result = "WIN"
  elseif p_tot == d_tot then bank = bank + h.bet; h.result = "PUSH"
  else h.result = "LOSE"; net = net - h.bet end
 end
 rounds = rounds + 1
 if net > 0 then wins = wins + 1 end
 if bj_win then b_tit, b_sub, b_col = "BLACKJACK!", string.format("+$%d  (3:2 Payout)", net), K_GLD
 elseif net > 0 then b_tit, b_sub, b_col = d_tot > 21 and "DEALER BUSTS!" or "YOU WIN!", string.format("+$%d", net), C.good
 elseif net < 0 then
  b_tit = (#hands == 1 and hands[1].status == "bust") and "BUST!" or (d_bj and "DEALER BLACKJACK" or "DEALER WINS")
  b_sub, b_col = string.format("-$%d", math.abs(net)), C.bad
 else b_tit, b_sub, b_col = "PUSH", "Bet returned", C.accent end
 if bank < MIN_BET then base_bet = MIN_BET elseif base_bet > bank then base_bet = bank end
 save_stats(); sfx(); build_btns(); draw_screen()
end

local function start_deal()
 if bank < MIN_BET then bank, base_bet = START_BANK, 25; sys.toast("Free $500 bailout chips!") end
 base_bet = clamp(base_bet, MIN_BET, math.min(MAX_BET, bank))
 bank = bank - base_bet
 d_hand = { draw_card(), draw_card() }; d_hide = true
 hands = { { cards = { draw_card(), draw_card() }, bet = base_bet, status = "play" } }
 cur_hand, b_tit, b_sub, state = 1, "", "", "player"
 sfx()
 local _, _, p_bj = eval_hand(hands[1].cards)
 local _, _, d_bj = eval_hand(d_hand)
 if p_bj or d_bj then
  if p_bj then hands[1].status = "bj" end
  d_hide = false; settle_round(); return
 end
 build_btns(); draw_screen()
end

local function adjust_bet(delta)
 if bank < MIN_BET then bank = START_BANK; sys.toast("Bailout: +$500 chips") end
 base_bet = I(clamp(base_bet + delta, MIN_BET, math.min(MAX_BET, bank)))
 sfx(); build_btns(); draw_screen()
end

build_btns = function()
 local c = (W < 280) and not is_p4
 if state == "betting" then
  btns = {
   { label = "-50", enabled = true, fn = function() adjust_bet(-50) end, row = 1 },
   { label = "-10", enabled = true, fn = function() adjust_bet(-10) end, row = 1 },
   { label = "+10", enabled = true, fn = function() adjust_bet(10) end, row = 1 },
   { label = "+50", enabled = true, fn = function() adjust_bet(50) end, row = 1 },
   { label = "MAX", enabled = true, fn = function() base_bet = I(math.min(MAX_BET, math.max(MIN_BET, bank))); sfx(); build_btns(); draw_screen() end, row = 2 },
   { label = "DEAL", primary = true, enabled = true, fn = start_deal, row = 2 },
  }
  f_idx = 6
 elseif state == "player" then
  btns = {
   { label = c and "HIT" or "HIT [H]", primary = true, enabled = true, fn = do_hit, row = 1 },
   { label = c and "STAND" or "STAND [S]", enabled = true, fn = do_stand, row = 1 },
   { label = c and "DBL" or "DBL [D]", enabled = can_double(), fn = do_double, row = 2 },
   { label = c and "SPLIT" or "SPLIT [P]", enabled = can_split(), fn = do_split, row = 2 },
  }
  f_idx = 1
 elseif state == "dealer" then
  btns = { { label = "DEALER DRAWING...", enabled = false } }; f_idx = 1
 elseif state == "done" then
  local amt = I(clamp(base_bet, MIN_BET, math.max(MIN_BET, bank)))
  btns = {
   { label = "-10", enabled = bank >= MIN_BET + 10, fn = function() adjust_bet(-10) end, row = 1 },
   { label = "+10", enabled = bank >= base_bet + 10, fn = function() adjust_bet(10) end, row = 1 },
   { label = c and "BET" or "BET [B]", enabled = true, fn = function() state = "betting"; build_btns(); draw_screen() end, row = 2 },
   { label = string.format("DEAL $%d", amt), primary = true, enabled = true, fn = start_deal, row = 2 },
  }
  f_idx = 4
 end
end

local function disk(tc, cx, cy, r, c)
 r = math.max(1, I(r)); local k = math.max(1, I(r * 0.68))
 tc:rect(cx - k, cy - r, k * 2 + 1, r * 2 + 1, c, true)
 tc:rect(cx - r, cy - k, r * 2 + 1, k * 2 + 1, c, true)
end

local P2 = {
 {0,-2,1,1,-1,-1,3,1,-2,0,5,2,0,2,1,2},{-2,-2,2,1,1,-2,2,1,-2,-1,5,2,-1,1,3,1,0,2,1,1},
 {0,-2,1,1,-1,-1,3,1,-2,0,5,1,-1,1,3,1,0,2,1,1},{-1,-2,3,1,0,-1,1,1,-2,0,5,2,0,2,1,2},
}
local P4 = {
 {0,-3,1,1,-1,-2,3,1,-2,-1,5,1,-3,0,7,2,0,2,1,1,-1,3,3,1},
 {-2,-3,2,1,1,-3,2,1,-3,-2,3,1,1,-2,3,1,-3,-1,7,2,-2,1,5,1,-1,2,3,1,0,3,1,1},
 {0,-3,1,1,-1,-2,3,1,-2,-1,5,1,-3,0,7,1,-2,1,5,1,-1,2,3,1,0,3,1,1},
 {-1,-3,3,2,-3,-1,2,2,2,-1,2,2,-1,-1,3,2,0,1,1,2,-1,3,3,1},
}
local P5 = {
 {0,-5,1,1,-1,-4,3,1,-2,-3,5,1,-3,-2,7,1,-4,-1,9,1,-5,0,11,2,-4,2,3,1,0,2,1,1,2,2,3,1,-1,3,3,1,-2,4,5,1,-3,5,7,1},
 {-3,-5,2,1,2,-5,2,1,-4,-4,4,1,1,-4,4,1,-5,-3,11,3,-4,0,9,1,-3,1,7,1,-2,2,5,1,-1,3,3,1,0,4,1,2},
 {0,-5,1,1,-1,-4,3,1,-2,-3,5,2,-3,-1,7,1,-4,0,9,1,-3,1,7,1,-2,2,5,2,-1,4,3,1,0,5,1,1},
 {-1,-5,3,1,-2,-4,5,2,-1,-2,3,1,-4,-1,3,1,0,-1,1,1,2,-1,3,1,-5,0,11,2,-4,2,3,1,0,2,1,1,2,2,3,1,-1,3,3,2,-3,5,7,1},
}

local function draw_pip(x, y, suit, r)
 x, y, r = I(x), I(y), math.max(2, I(r))
 local col = (suit == 2 or suit == 3) and K_RED or K_BLK
 local tc, ox, oy, tiles = cv, 0, 0, cv.tiles
 if tiles then
  local x1, y1, x2, y2 = x - r - 2, y - r - 2, x + r + 2, y + r + 4
  for i = 1, #tiles do
   local t = tiles[i]
   if x1 >= t.x and x2 <= t.x2 and y1 >= t.y and y2 <= t.y2 then tc, ox, oy = t.c, t.x, t.y; break end
  end
 end
 local px, py = x - ox, y - oy
 local tb = (r <= 2) and P2 or ((r <= 4) and P4 or ((r == 5) and P5 or nil))
 if tb then
  local d = tb[suit] or tb[4]
  for i = 1, #d, 4 do tc:rect(px + d[i], py + d[i + 1], d[i + 2], d[i + 3], col, true) end
  return
 end
 if suit == 1 then
  local bt = I(r * 0.25); local sp = math.max(1, r + bt)
  for dy = -r, bt, 2 do local hw = I(((dy + r) * r) / sp); tc:rect(px - hw, py + dy, hw * 2 + 1, 2, col, true) end
  local lr, lx, ly = math.max(2, I(r * 0.36)), math.max(2, I(r * 0.42)), I(r * 0.22)
  disk(tc, px - lx, py + ly, lr, col); disk(tc, px + lx, py + ly, lr, col)
  local sw, st, bw = math.max(1, I(r * 0.22) * 2 + 1), I(r * 0.25), math.max(2, I(r * 0.55))
  tc:rect(px - I(sw / 2), py + st, sw, math.max(2, r - st + 1), col, true)
  tc:rect(px - bw, py + r, bw * 2 + 1, math.max(1, I(r * 0.18)), col, true)
 elseif suit == 2 then
  local lr, lx, ly, td = math.max(2, I(r * 0.46)), math.max(2, I(r * 0.46)), I(r * 0.32), -I(r * 0.18)
  disk(tc, px - lx, py - ly, lr, col); disk(tc, px + lx, py - ly, lr, col)
  local sp = math.max(1, r - td)
  for dy = td, r, 2 do local hw = math.max(0, I(((r - dy) * r) / sp)); tc:rect(px - hw, py + dy, hw * 2 + 1, 2, col, true) end
 elseif suit == 3 then
  for dy = -r, r, 2 do local hw = math.max(0, I((r - math.abs(dy)) * 0.85)); tc:rect(px - hw, py + dy, hw * 2 + 1, 2, col, true) end
 else
  local lr, lx, ty, by = math.max(2, I(r * 0.38)), math.max(2, I(r * 0.48)), I(r * 0.48), I(r * 0.14)
  disk(tc, px, py - ty, lr, col); disk(tc, px - lx, py + by, lr, col); disk(tc, px + lx, py + by, lr, col)
  local sw, bw = math.max(1, I(r * 0.22) * 2 + 1), math.max(2, I(r * 0.52))
  tc:rect(px - I(sw / 2), py - I(r * 0.1), sw, math.max(2, r + I(r * 0.1) + 1), col, true)
  tc:rect(px - bw, py + r, bw * 2 + 1, math.max(1, I(r * 0.18)), col, true)
 end
end

local function draw_card_ui(x, y, w, h, card, hidden, sm_ov, lg_ov)
 x, y, w, h = I(x), I(y), I(w), I(h)
 local rad, shad = w >= 64 and 8 or 4, w >= 64 and 4 or 2
 local sm, lg, rfsz = sm_ov or p_sm, lg_ov or p_lg, (w >= 64) and f_rk or 12
 cv:rect(x + shad, y + shad, w, h, K_CSH, true, rad)
 if hidden then
  local p1, p2 = w >= 64 and 3 or 1, w >= 64 and 6 or 3
  cv:rect(x, y, w, h, K_CBG, true, rad)
  cv:rect(x + p1, y + p1, w - p1 * 2, h - p1 * 2, K_CBK, true, math.max(2, rad - 1))
  if w >= 26 then
   cv:rect(x + p2, y + p2, w - p2 * 2, h - p2 * 2, K_CTR, false, math.max(2, rad - 2))
   local ew, eh = math.max(6, I(w * 0.28)), math.max(6, I(h * 0.22))
   cv:rect(I(x + (w - ew) / 2), I(y + (h - eh) / 2), ew, eh, K_GLD, false, 2)
  end
  return
 end
 box(x, y, w, h, K_CBG, 0x64748B, rad)
 local col, rstr = (card.s == 2 or card.s == 3) and K_RED or K_BLK, RANKS[card.r]
 local px, py = w >= 64 and 6 or 3, w >= 64 and 4 or 2
 cv:text(x + px, y + py, rstr, col, rfsz)
 if w >= 30 then draw_pip(x + w - sm - px - 1, y + py + sm + 2, card.s, sm) end
 if w >= 68 and h >= 96 then
  cv:text(x + w - px - tw(rstr, rfsz), y + h - py - th(rfsz), rstr, col, rfsz)
  draw_pip(x + px + sm + 1, y + h - py - sm - 3, card.s, sm)
 end
 draw_pip(x + I(w / 2), h >= 42 and ((w >= 68) and I(y + h / 2) or (y + I(h / 2) + 4)) or (y + I(h / 2) + 3), card.s, h >= 42 and lg or sm)
end

local function draw_hand(cards, cx, y, w, h, step_x, hide2)
 local n = #cards
 if n == 0 then return end
 local max_span, eff = math.max(w + 12, W - 24), step_x
 if w + (n - 1) * eff > max_span and n > 1 then eff = math.max(12, I((max_span - w) / (n - 1))) end
 local sx = I(cx - (w + (n - 1) * eff) / 2)
 for i = 1, n do draw_card_ui(sx + (i - 1) * eff, y, w, h, cards[i], hide2 and i == 2) end
end

local function draw_pill(cx, y, str, tcol, bcol)
 local lh, w = th(f_md), tw(str, f_md)
 local pw, ph = w + (s2 and 28 or 16), lh + (s2 and 10 or 6)
 box(I(cx - pw / 2), I(y), pw, ph, 0x0D151C, bcol or K_FLN, 6)
 tcx(cx, I(y) + I((ph - lh) / 2), str, tcol or C.text, f_md)
 return ph
end

draw_screen = function()
 local cx, m = I(W / 2), s2 and 8 or 4
 cv:fill(K_FELT)
 cv:rect(m, top_h + m - 1, W - m * 2, tbl_h - (m - 1) * 2, K_FLN, false, s2 and 12 or 8)
 cv:rect(0, 0, W, top_h, K_BAR, true); cv:line(0, top_h, W, top_h, 0x1F2933, 1)

 local tfsz = s2 and f_md or f_sm
 local tty = math.max(1, I((top_h - th(tfsz)) / 2))
 local top_margin = s2 and 22 or pad
 cv:text(top_margin, tty, string.format("BANK $%d", bank), K_GLD, tfsz)

 local abet = base_bet
 if (state == "player" or state == "dealer") and #hands > 0 then
  abet = 0; for i = 1, #hands do abet = abet + hands[i].bet end
 end
 local bstr = string.format("BET $%d", abet)
 local bx0 = I((W - tw(bstr, tfsz)) / 2)
 cv:text(bx0, tty, bstr, C.accent, tfsz)
 local rstr = string.format(portrait and "HI $%d%s" or "PEAK $%d%s", best_bank, sound_on and "" or " [M]")
 local rw = tw(rstr, tfsz)
 cv:text(math.max(bx0 + tw(bstr, tfsz) + 6, W - rw - top_margin), tty, rstr, C.sub, tfsz)

 if state == "betting" then
  local px, py = (portrait or s2) and 18 or 12, s2 and 16 or 8
  local bw, bh, by = W - px * 2, tbl_h - py * 2, top_h + py
  box(px, by, bw, bh, K_FDK, K_FLN, s2 and 12 or 8)
  local wlbl = string.format("WAGER: $%d", base_bet)
  local ww = tw(wlbl, f_xl)
  if portrait and s2 and H >= 800 then
   local cy = by + I(bh * 0.07)
   tcx(cx, cy, "BLACKJACK 21", K_GLD, f_xl); cy = cy + lh_xl + 12
   tcx(cx, cy, "3:2 Natural Payout  ·  Dealer Stands on 17", C.sub, f_sm); cy = cy + lh_sm + 8
   tcx(cx, cy, string.format("%s  (%dx%d)", dev_name, W, H), K_FLN, f_sm); cy = cy + lh_sm + I(bh * 0.05)
   draw_card_ui(cx - cw - 12, cy, cw, ch, { r = 1, s = 1 }, false)
   draw_card_ui(cx + 12, cy, cw, ch, { r = 13, s = 2 }, false)
   cy = cy + ch + I(bh * 0.06)
   local ph, pw = lh_xl + 24, ww + 56
   box(cx - I(pw / 2), cy, pw, ph, K_BACT, C.accent, 10)
   tcx(cx, cy + I((ph - lh_xl) / 2), wlbl, C.text, f_xl); cy = cy + ph + I(bh * 0.05)
   local pct = rounds > 0 and I((wins * 100) / rounds) or 0
   tcx(cx, cy, string.format("Hands: %d   ·   Won: %d (%d%%)   ·   Shoe: %d cards", rounds, wins, pct, shoe_top), C.sub, f_sm)
   tcx(cx, by + bh - lh_sm - 18, "Tap chip buttons below to adjust wager, then tap DEAL", C.sub, f_sm)
  else
   local dw, dh, dsm, dlg = s2 and 64 or 30, s2 and 90 or 42, s2 and 4 or 3, s2 and 9 or 5
   local title_y = by + (portrait and (s2 and 22 or 16) or 10)
   if W >= 280 and (not portrait or s2) then
    draw_card_ui(px + 8, title_y - 2, dw, dh, { r = 1, s = 1 }, false, dsm, dlg)
    draw_card_ui(W - px - dw - 8, title_y - 2, dw, dh, { r = 13, s = 2 }, false, dsm, dlg)
   end
   tcx(cx, title_y, "BLACKJACK 21", K_GLD, f_xl)
   local sy = title_y + lh_xl + (portrait and (s2 and 14 or 10) or 6)
   tcx(cx, sy, "3:2 Payout  ·  Stands on 17", C.sub, f_sm)
   local ph, pw = lh_xl + (s2 and 18 or 8), ww + (s2 and 40 or 28)
   local wy = math.max(sy + lh_sm + (portrait and 20 or 10), I(by + (bh - ph) * 0.52))
   box(cx - I(pw / 2), wy, pw, ph, K_BACT, C.accent, 6)
   tcx(cx, wy + I((ph - lh_xl) / 2), wlbl, C.text, f_xl)
   local hy = by + bh - lh_sm - (portrait and 16 or 8)
   if hy > wy + ph + 4 then
    tcx(cx, hy, W >= 300 and "Tap buttons or [Up/Down] Bet  [D] Deal" or "Tap buttons below to Bet & Deal", C.sub, f_sm)
   end
  end
 else
  local dtot, _, dbj = eval_hand(d_hand)
  local vpad = portrait and (s2 and 22 or 8) or (s2 and 12 or 6)
  local ccx, lx = portrait and cx or I(W * 0.58), s2 and 22 or 12
  local dcy, pcy = top_h + vpad, top_h + tbl_h - ch - vpad
  local phb = pcy - (s2 and 12 or 6)
  if portrait then
   local dstr = d_hide and string.format("DEALER: %d + ?", card_val(d_hand[1].r)) or (dbj and "DEALER: 21 (BLACKJACK)" or string.format("DEALER: %d", dtot))
   dcy = top_h + vpad + draw_pill(cx, top_h + vpad, dstr, C.text, K_FLN) + (s2 and 14 or 6)
  else
   cv:text(lx, dcy + 4, "DEALER", C.sub, f_sm)
   cv:text(lx, dcy + 6 + lh_sm, d_hide and string.format("%d + ?", card_val(d_hand[1].r)) or (dbj and "21 (BJ)" or tostring(dtot)), C.text, f_lg)
  end
  draw_hand(d_hand, ccx, dcy, cw, ch, ov, d_hide)

  if #hands == 1 then
   local h = hands[1]
   local ptot, psoft, pbj = eval_hand(h.cards)
   local pcol = h.status == "bust" and C.bad or (pbj and K_GLD or C.text)
   if portrait then
    local pstr = pbj and string.format("PLAYER: 21 (BJ!)  ·  $%d", h.bet)
     or ((psoft and ptot <= 21 and state == "player") and string.format("PLAYER: %d / %d  ·  $%d", ptot - 10, ptot, h.bet)
     or string.format("PLAYER: %d  ·  $%d", ptot, h.bet))
    draw_pill(cx, phb - (lh_md + (s2 and 10 or 6)), pstr, pcol, h.status == "bust" and C.bad or (pbj and K_GLD or C.accent))
   else
    local pstr = pbj and "21 (BJ!)" or ((psoft and ptot <= 21 and state == "player") and string.format("%d / %d", ptot - 10, ptot) or tostring(ptot))
    cv:text(lx, pcy + 4, "PLAYER", C.sub, f_sm)
    cv:text(lx, pcy + 6 + lh_sm, pstr, pcol, f_lg)
   end
   draw_hand(h.cards, ccx, pcy, cw, ch, ov, false)
  else
   for idx = 1, 2 do
    local h, hcx = hands[idx], idx == 1 and I(W * (portrait and 0.27 or 0.28)) or I(W * (portrait and 0.73 or 0.74))
    local act, bhw = (state == "player" and cur_hand == idx), portrait and I(W * 0.22) or (s2 and 140 or 60)
    if act then cv:rect(hcx - bhw, pcy - 3, bhw * 2, ch + 6, C.accent, false, 5) end
    local tag = h.result or (h.status == "bust" and "BUST" or string.format("$%d", h.bet))
    local lbl = string.format(portrait and "H%d: %d (%s)" or "H%d:%d (%s)", idx, eval_hand(h.cards), tag)
    tcx(hcx, portrait and (phb - lh_sm) or (pcy - lh_sm - 2), lbl, act and C.accent or (h.status == "bust" and C.bad or C.text), f_sm)
    draw_hand(h.cards, hcx, pcy, cw, ch, s_ov, false)
   end
  end

  local mtop = dcy + ch + (portrait and 8 or 3)
  local mbot = portrait and (phb - (lh_md + 16)) or ((#hands > 1) and (pcy - lh_sm - 4) or (pcy - 3))
  local mcy = I((mtop + mbot) / 2)
  if state == "done" and b_tit ~= "" then
   local sf, slh = portrait and f_md or f_sm, portrait and lh_md or lh_sm
   local bw = math.min(W - 24, math.max(s2 and 280 or 164, math.max(tw(b_tit, f_lg), tw(b_sub, sf)) + (s2 and 44 or 32)))
   local bh = lh_lg + slh + (portrait and (s2 and 28 or 12) or (s2 and 14 or 4))
   local bx, by = cx - I(bw / 2), I(mcy - bh / 2)
   box(bx, by, bw, bh, 0x0D131A, b_col, 6)
   local ty = by + (portrait and (s2 and 8 or 3) or 2)
   tcx(cx, ty, b_tit, b_col, f_lg)
   tcx(cx, ty + lh_lg + (portrait and (s2 and 6 or 2) or 0), b_sub, C.text, sf)
  else
   local lm = (portrait and s2) and 48 or 24
   cv:line(lm, mcy, W - lm, mcy, K_FLN, 1)
   if portrait and s2 and (mbot - mtop) >= lh_sm * 2 + 20 then
    tcx(cx, mcy - lh_sm - 8, "BLACKJACK PAYS 3 TO 2  ·  DEALER STANDS ON ALL 17", K_FLN, f_sm)
    tcx(cx, mcy + 8, state == "dealer" and "Dealer is drawing..." or "Choose your action below", C.sub, f_sm)
   end
  end
 end

 local by = H - bot_h
 cv:rect(0, by, W, bot_h, K_BAR, true); cv:line(0, by, W, by, 0x1F2933, 1)
 local nb = #btns
 if nb > 0 then
  local dbl = (is_p4 and portrait and nb > 1)
  local pvt = dbl and (s2 and 8 or 4) or (s2 and 8 or 3)
  local row_gap = s2 and 6 or 4
  local btn_mx = dbl and (s2 and 22 or 12) or (s2 and 18 or gap)
  local avail_w = W - btn_mx * 2
  local row_h = dbl and I((bot_h - pvt - (s2 and 14 or 8) - row_gap) / 2) or (bot_h - pvt * 2)
  local bfsz, brad = s2 and f_md or f_sm, s2 and 6 or 4
  local blh = th(bfsz)
  local rows
  if dbl then
   local r1, r2 = {}, {}
   for i = 1, nb do
    if (btns[i].row or (i <= math.ceil(nb / 2) and 1 or 2)) == 2 then r2[#r2 + 1] = i else r1[#r1 + 1] = i end
   end
   rows = { r1, r2 }
  else
   local r1 = {}; for i = 1, nb do r1[i] = i end; rows = { r1 }
  end
  for r_idx = 1, #rows do
   local rl = rows[r_idx]
   local nr = #rl
   if nr > 0 then
    local r_by = dbl and ((r_idx == 1) and (by + pvt) or (by + pvt + row_h + row_gap)) or (by + pvt)
    local bw = I((avail_w - (nr - 1) * gap) / nr)
    local row_w = nr * bw + (nr - 1) * gap
    local r_sx = I((W - row_w) / 2)
    for k = 1, nr do
     local i = rl[k]
     local b = btns[i]
     local bx = r_sx + (k - 1) * (bw + gap)
     b.x, b.y, b.w, b.h = bx, r_by, bw, row_h
     local bg = not b.enabled and K_BDIS or (b.primary and K_BACT or K_BTN)
     local fg = (i == f_idx and b.enabled) and (b.primary and K_GLD or C.accent) or (b.enabled and 0x2B3642 or nil)
     box(bx, r_by, bw, row_h, bg, fg, brad)
     local lbl = b.label
     if bw < 56 and lbl:find(" %[") then lbl = lbl:gsub(" %[.*%]", "") end
     if bw < 46 and lbl:find(" %$") then lbl = lbl:gsub(" %$", "") end
     if bw < 38 and lbl == "STAND" then lbl = "STND" end
     local tw_l = tw(lbl, bfsz)
     cv:text(bx + math.max(2, I((bw - tw_l) / 2)), r_by + math.max(1, I((row_h - blh) / 2)), lbl, not b.enabled and K_DIM or (b.primary and K_GLD or C.text), bfsz)
    end
   end
  end
 end
end

local function cycle_focus(dir)
 local n = #btns
 if n == 0 then return end
 for _ = 1, n do
  f_idx = ((f_idx - 1 + dir) % n) + 1
  if btns[f_idx] and btns[f_idx].enabled then break end
 end
 draw_screen()
end

function app.on_open(w, h)
 local brd = (sys.board and sys.board()) or {}
 W = I((w and w > 0) and w or ((brd.w and brd.w > 0) and brd.w or 320))
 H = I((h and h > 0) and h or ((brd.h and brd.h > 0) and brd.h or 208))
 local bname = string.lower(tostring(brd.name or brd.model or brd.board or ""))
 if bname:find("p4") then is_p4 = true end
 compute_layout()
 bank      = I(tonumber(store.get("bank", START_BANK)) or START_BANK)
 best_bank = I(tonumber(store.get("best", START_BANK)) or START_BANK)
 base_bet  = I(tonumber(store.get("bet", 25)) or 25)
 wins      = I(tonumber(store.get("wins", 0)) or 0)
 rounds    = I(tonumber(store.get("rounds", 0)) or 0)
 if bank < MIN_BET then bank = START_BANK end
 base_bet = I(clamp(base_bet, MIN_BET, math.min(MAX_BET, bank)))
 cv = create_screen_canvas(W, H)
 build_shoe(); state = "betting"; build_btns(); draw_screen()
end

function app.on_tick()
 if state ~= "dealer" then timer.stop(); return end
 if eval_hand(d_hand) < 17 then
  sfx(); d_hand[#d_hand + 1] = draw_card()
  if eval_hand(d_hand) >= 17 then settle_round() else draw_screen() end
 else settle_round() end
end

function app.on_input(ev)
 if not ev or state == "dealer" then return end
 if ev.type == "down" then
  local ex, ey = tonumber(ev.x) or 0, tonumber(ev.y) or 0
  for i = 1, #btns do
   local b = btns[i]
   if b.x and ex >= b.x and ex <= b.x + b.w and ey >= b.y and ey <= b.y + b.h then
    if b.enabled and b.fn then f_idx = i; b.fn() end
    return
   end
  end
  if ey <= top_h and ex >= W - I(W * 0.28) then
   sound_on = not sound_on; sys.toast(sound_on and "Sound ON" or "Sound OFF"); draw_screen(); return
  end
  if state == "done" and ey > top_h + 2 and ey < H - bot_h - 2 then start_deal(); return end
 elseif ev.type == "swipe" then
  local d = ev.dir
  if d == "left" then cycle_focus(-1)
  elseif d == "right" then cycle_focus(1)
  elseif d == "up" then
   if state == "betting" or state == "done" then adjust_bet(25) elseif state == "player" then do_hit() end
  elseif d == "down" then
   if state == "betting" or state == "done" then adjust_bet(-25) elseif state == "player" then do_stand() end
  end
 elseif ev.type == "key" then
  local k = tostring(ev.key or ""):lower()
  if k == "left" or k == "a" then cycle_focus(-1)
  elseif k == "right" or k == "l" then cycle_focus(1)
  elseif k == "up" or k == "w" or k == "+" or k == "=" then
   if state == "betting" or state == "done" then adjust_bet(10) end
  elseif k == "down" or k == "z" or k == "-" then
   if state == "betting" or state == "done" then adjust_bet(-10) end
  elseif k == "enter" or k == " " then
   local b = btns[f_idx]; if b and b.enabled and b.fn then b.fn() end
  elseif k == "m" then
   sound_on = not sound_on; sys.toast(sound_on and "Sound ON" or "Sound OFF"); draw_screen()
  elseif state == "player" then
   if k == "h" then do_hit() elseif k == "s" then do_stand() elseif k == "d" then do_double() elseif k == "p" then do_split() end
  elseif state == "betting" or state == "done" then
   if k == "d" or k == "h" then start_deal()
   elseif k == "b" and state == "done" then state = "betting"; build_btns(); draw_screen()
   elseif k == "c" then base_bet = MIN_BET; build_btns(); draw_screen()
   elseif k == "x" then base_bet = math.min(MAX_BET, math.max(MIN_BET, bank)); build_btns(); draw_screen() end
  end
 end
end

function app.on_close() save_stats() end

return app
