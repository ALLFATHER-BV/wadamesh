-- Music (Lua): plays the MP3 and WAV files in the Music folder of the SD card.
-- Previous / play-pause / next, shuffle, repeat (all or one), the track list,
-- and it keeps going through the list with the screen off: the firmware tells
-- the app when a track ends (app.on_audio) even while the display is asleep.
--
-- Folders inside Music are searched three levels deep, so Music/Artist/Album/
-- works. Without a Music folder it plays the audio files in the card's top folder.
-- Boards without card access play the files in the app's own folder instead.
local ui, sys, timer, store = wada.ui, wada.sys, wada.timer, wada.store
local audio, sd, fs = wada.audio, wada.sd, wada.fs
local tr = sys.tr or function(x) return x end
local C = ui.colors
local PANEL = C.panel or 0x15181B
local app = {}

local SYM_PREV, SYM_PLAY, SYM_PAUSE = "\xEF\x81\x88", "\xEF\x81\x8B", "\xEF\x81\x8C"
local SYM_NEXT, SYM_SHUFFLE, SYM_LOOP = "\xEF\x81\x91", "\xEF\x81\xB4", "\xEF\x81\xB9"
local MAX_TRACKS = 200        -- each list row is a real button; keep the page light
local MAX_DEPTH = 3

local caps = sys.caps and sys.caps() or {}
local W, H, LH12, LH16
local title_lbl, sub_lbl, state_lbl, pos_lbl
local b_prev, b_play, b_next, b_shuf, b_rep
local list
local tracks = {}             -- { path = "sd:/Music/a/b.mp3" or "b.mp3", name, folder }
local order = {}              -- play order: indexes into tracks
local cur = 0                 -- position in order (0 = nothing yet)
local playing, paused = false, false
local shuffle = store and store.get("shuffle") == "1"
local rep = tonumber(store and store.get("repeat") or "0") or 0   -- 0 off, 1 all, 2 one
local t_start, t_paused_at, t_paused = 0, 0, 0
local errors = 0
local last_err = nil          -- why the last play() failed, shown until something plays
local scan = nil              -- { queue = {{dir, depth}}, start = n } while scanning
local mode = "sd"             -- "sd", "app" or "none"

local function is_audio(name)
  local l = name:lower()
  return l:sub(-4) == ".mp3" or l:sub(-4) == ".wav"
end
local function strip_ext(name) return (name:gsub("%.[^%.]+$", "")) end

local function fmt_time(ms)
  local s = ms // 1000
  if s >= 3600 then return string.format("%d:%02d:%02d", s // 3600, (s // 60) % 60, s % 60) end
  return string.format("%d:%02d", s // 60, s % 60)
end

-- E-paper (T-Deck Pro and Max): every redraw is a panel refresh and every tenth
-- one flashes the whole screen, so no running clock there; the screen changes
-- only when the track or the state does.
local EPAPER = caps.epaper == true

local function elapsed()
  if not playing then return 0 end
  local now = paused and t_paused_at or sys.millis()
  return now - t_start - t_paused
end

local function save_prefs()
  if not store then return end
  store.set("shuffle", shuffle and "1" or "0")
  store.set("repeat", tostring(rep))
end

local function build_order(keep)
  order = {}
  for i = 1, #tracks do order[i] = i end
  if shuffle then
    for i = #order, 2, -1 do
      local j = math.random(i)
      order[i], order[j] = order[j], order[i]
    end
    -- keep the current track where we are, so shuffling does not jump
    if keep then
      for i = 1, #order do
        if order[i] == keep then order[1], order[i] = order[i], order[1]; break end
      end
      cur = 1
    end
  elseif keep then
    cur = keep
  end
end

-- Relabelling a button corrupted memory on firmware before beta_95 (button:set
-- wrote into the button object, not its label). caps().panel arrived with the
-- fix, so older firmware keeps fixed glyphs and shows state by colour alone.
local can_relabel = caps.panel == true
local function btn_set(b, txt) if can_relabel then b:set(txt) end end

local function paint_buttons()
  btn_set(b_play, playing and not paused and SYM_PAUSE or SYM_PLAY)
  b_play:color(playing and not paused and C.accent or C.text)
  b_shuf:color(shuffle and C.accent or C.sub)
  btn_set(b_rep, rep == 2 and (SYM_LOOP .. " 1") or SYM_LOOP)
  b_rep:color(rep > 0 and C.accent or C.sub)
end

local function paint_now()
  local t = cur > 0 and tracks[order[cur]] or nil
  if t then
    title_lbl:set(t.name)
    sub_lbl:set(t.folder ~= "" and t.folder or tr("Music"))
    pos_lbl:set(string.format("%d / %d", order[cur], #tracks))
  end
  if not playing and last_err == "muted" then
    state_lbl:set(tr("Sound is off: Settings > Sound"))   -- apps follow the device's sound switch
    state_lbl:color(C.warn or C.bad)
  elseif not playing and last_err then
    state_lbl:set(tr("Cannot play") .. ": " .. last_err)
    state_lbl:color(C.bad)
  elseif not playing then
    state_lbl:set(#tracks > 0 and tr("Stopped") or "")
    state_lbl:color(C.sub)
  elseif paused then
    state_lbl:set(EPAPER and tr("Paused") or (tr("Paused") .. "  " .. fmt_time(elapsed())))
    state_lbl:color(C.sub)
  else
    state_lbl:set(EPAPER and tr("Playing") or (tr("Playing") .. "  " .. fmt_time(elapsed())))
    state_lbl:color(C.accent)
  end
  paint_buttons()
end

local function play_at(k)
  if #tracks == 0 then return end
  cur = k
  local t = tracks[order[cur]]
  local ok, err = audio.play(t.path)
  last_err = not ok and tostring(err) or nil
  if ok then
    playing, paused = true, false
    t_start, t_paused, t_paused_at = sys.millis(), 0, 0
    if store then store.set("last", t.path) end
    if list and list.select then list:select(order[cur]) end
  else
    playing, paused = false, false
  end
  paint_now()
  return ok
end

local function step(dir)              -- next (1) or previous (-1), wrapping
  if #tracks == 0 then return end
  local k = cur + dir
  if k > #order then k = 1 elseif k < 1 then k = #order end
  play_at(k)
end

local function toggle()
  if #tracks == 0 then return end
  if not playing then play_at(cur > 0 and cur or 1); return end
  if paused then
    if audio.resume() then
      paused = false
      t_paused = t_paused + (sys.millis() - t_paused_at)
    end
  else
    if audio.pause() then paused, t_paused_at = true, sys.millis() end
  end
  paint_now()
end

-- ---- the library ------------------------------------------------------------
local function add_track(path, name, folder)
  if #tracks >= MAX_TRACKS then return end
  tracks[#tracks + 1] = { path = path, name = strip_ext(name), folder = folder }
end

local function finish_scan()
  local no_card = scan and scan.no_card
  scan = nil
  table.sort(tracks, function(a, b) return a.path:lower() < b.path:lower() end)
  list:clear()
  for i, t in ipairs(tracks) do
    list:add(t.folder ~= "" and (t.name .. "  \xC2\xB7  " .. t.folder) or t.name,
             function()
               for k = 1, #order do if order[k] == i then play_at(k); return end end
             end)
  end
  -- start where the last session left off
  local last = store and store.get("last")
  local keep = nil
  for i, t in ipairs(tracks) do if t.path == last then keep = i; break end end
  build_order(keep)
  if #tracks == 0 then
    title_lbl:set(no_card and tr("No SD card") or tr("No music found"))
    sub_lbl:set(mode == "sd" and tr("Put MP3 or WAV files in a Music folder on the SD card.")
                or tr("Add MP3 or WAV files to this app's folder."))
  else
    if cur == 0 then cur = 1 end
    if list.select then list:select(order[cur]) end
    if #tracks >= MAX_TRACKS then sys.toast(string.format(tr("Showing the first %d tracks"), MAX_TRACKS), 2500) end
  end
  paint_now()
  timer.every(500)                    -- from here on the tick only updates the clock
end

-- One directory page per tick: a card folder can be large and a callback has an
-- instruction budget. Sub-folders join the queue up to MAX_DEPTH.
local function scan_step()
  local job = scan.queue[1]
  if not job or #tracks >= MAX_TRACKS then finish_scan(); return end
  local entries, err = sd.list(job.dir, scan.start, 48)
  if not entries then
    table.remove(scan.queue, 1); scan.start = 1
    if err == "busy" then table.insert(scan.queue, 1, job); return end   -- the card is busy: try again next tick
    if err == "no sd" then scan.no_card = true; scan.queue = {}; return end
    if job.dir == "/Music" then
      -- no Music folder: the audio files in the top folder of the card, not its sub-folders
      scan.queue[#scan.queue + 1] = { dir = "/", depth = MAX_DEPTH, root = true }
    end
    return
  end
  local folder = job.dir:gsub("^/Music/?", ""):gsub("^/", "")
  for _, e in ipairs(entries) do
    if e.type == "dir" then
      if job.depth < MAX_DEPTH and not job.root and e.name:sub(1, 1) ~= "." then
        scan.queue[#scan.queue + 1] = { dir = job.dir .. "/" .. e.name, depth = job.depth + 1 }
      end
    elseif is_audio(e.name) and e.name:sub(1, 2) ~= "._" then
      add_track("sd:" .. (job.dir == "/" and "" or job.dir) .. "/" .. e.name, e.name, folder)
    end
  end
  if entries.truncated then scan.start = entries.next
  else table.remove(scan.queue, 1); scan.start = 1 end
  title_lbl:set(string.format(tr("Looking for music... %d"), #tracks))
end

local function start_library()
  if not audio then
    mode = "none"
    title_lbl:set(tr("No speaker"))
    sub_lbl:set(tr("This device cannot play music."))
    return
  end
  if caps.audio_sd and sd and sd.list then
    mode = "sd"
    scan = { queue = { { dir = "/Music", depth = 1 } }, start = 1 }
    title_lbl:set(tr("Looking for music..."))
    timer.every(60)
  elseif fs and fs.list then
    mode = "app"
    for _, name in ipairs(fs.list() or {}) do
      if type(name) == "table" then name = name.name end
      if name and is_audio(name) then add_track(name, name, "") end
    end
    finish_scan()
  else
    mode = "none"
    finish_scan()
  end
end

-- ---- screen -----------------------------------------------------------------
function app.on_open(w, h)
  W, H = w, h
  LH12, LH16 = ui.text_h(12), ui.text_h(16)
  local M, y = 4, 4

  -- now playing
  local ph = LH16 + 2 * LH12 + 14
  if ui.panel then ui.panel(M, y, W - 2 * M, ph, PANEL, 8) end
  title_lbl = ui.label("", M + 8, y + 5, 16, C.text, true); title_lbl:width(W - 2 * M - 16)
  sub_lbl = ui.label("", M + 8, y + 5 + LH16, 12, C.sub); sub_lbl:width(W - 2 * M - 16)
  state_lbl = ui.label("", M + 8, y + 7 + LH16 + LH12, 12, C.sub)
  local pw = ui.text_w and ui.text_w("200 / 200", 12) or 70
  pos_lbl = ui.label("", W - M - 8 - pw, y + 7 + LH16 + LH12, 12, C.sub); pos_lbl:width(pw, "right")
  y = y + ph + 6

  -- controls: previous, play/pause, next, shuffle, repeat
  local gap = 4
  local bw = (W - 2 * M - 4 * gap) // 5
  local bh = math.max(32, LH16 + 12)
  local function btn(i, txt, fn) return ui.button(txt, M + (i - 1) * (bw + gap), y, bw, bh, fn) end
  b_prev = btn(1, SYM_PREV, function() step(-1) end)
  b_play = btn(2, SYM_PLAY, toggle)
  b_next = btn(3, SYM_NEXT, function() step(1) end)
  b_shuf = btn(4, SYM_SHUFFLE, function()
    shuffle = not shuffle
    build_order(cur > 0 and order[cur] or nil)
    save_prefs(); paint_buttons()
  end)
  b_rep = btn(5, SYM_LOOP, function()
    rep = (rep + 1) % 3
    save_prefs(); paint_buttons()
    sys.toast(rep == 0 and tr("Repeat off") or rep == 1 and tr("Repeat all") or tr("Repeat this track"), 1200)
  end)
  y = y + bh + 6

  list = ui.list(M, y, W - 2 * M, H - y - M)
  paint_buttons()
  start_library()
end

function app.on_tick(dt)
  if scan then scan_step(); return end
  if playing and not paused and not EPAPER then paint_now() end
end

-- The firmware calls this when playback changes state, also with the screen off.
function app.on_audio(st)
  if st.state == "ended" then
    errors = 0
    if rep == 2 then play_at(cur)
    elseif cur < #order or rep == 1 then step(1)
    else playing, paused = false, false; paint_now() end
  elseif st.state == "error" then
    errors = errors + 1
    playing = false
    last_err = st.error or "?"
    paint_now()
    if errors < 3 and #tracks > 1 then step(1) end   -- skip a bad file, but never loop on a bad card
  end
end

-- Keyboards: space plays or pauses, n and p (or the arrows) change track.
-- Swiping left and right on the screen changes track too.
function app.on_input(ev)
  if ev.type == "key" then
    if ev.key == " " then toggle()
    elseif ev.key == "n" or ev.key == "right" then step(1)
    elseif ev.key == "p" or ev.key == "left" then step(-1) end
  elseif ev.type == "swipe" then
    if ev.dir == "left" then step(1) elseif ev.dir == "right" then step(-1) end
  end
end

-- Older firmware has no on_audio: fall back to watching the status while the
-- screen is on, so a playlist still advances there.
if not caps.audio_events then
  local base_tick = app.on_tick
  app.on_tick = function(dt)
    base_tick(dt)
    if playing and audio and audio.status then
      local st = audio.status()
      if st and (st.state == "ended" or st.state == "error") then app.on_audio(st) end
    end
  end
end

return app
