-- SD Scan: finds Windows malware on the SD card and removes it.
--
-- Written for the ThinkNode M9 SD-card worm (Elecrow security advisory,
-- September 2026). Some cards shipped with a hidden autorun.inf and Windows
-- programs disguised as shortcuts. They cannot run on a mesh radio, but they run
-- on the first Windows PC the card is plugged into, which is exactly where you
-- take a card to copy map tiles onto it.
--
-- What counts as a threat is decided by the FIRMWARE (wada.sd.check and
-- wada.sd.remove, SdThreat.h): autorun.inf, Windows programs, scripts and
-- shortcuts, and Windows programs hiding under another name. remove() refuses
-- anything else, so this app cannot delete map tiles, backups or chat history,
-- not even by mistake. On firmware without those two calls it falls back to the
-- same rules by file name, and can report but not remove.
--
-- The card is walked a page at a time from on_tick, so the screen stays live and
-- no callback comes near the instruction budget, however big the card is.
local ui, sys, sd, timer = wada.ui, wada.sys, wada.sd, wada.timer
local C = ui.colors
local tr = sys.tr or function(s) return s end
local app = {}

local caps = sys.caps()
local CAN_LIST  = caps.sd_list and sd and sd.list and true or false
local CAN_CLEAN = CAN_LIST and caps.sd_clean and sd.check and sd.remove and true or false

-- Same lists as SdThreat.h. Only used to decide which files to ask the firmware
-- about, and as the whole rule on firmware that cannot check.
local KIND = {
  exe = "Windows program", scr = "Windows program", com = "Windows program",
  pif = "Windows program", cpl = "Windows program", msi = "Windows program",
  msp = "Windows program", dll = "Windows program", jar = "Windows program",
  bat = "Windows script", cmd = "Windows script", vbs = "Windows script",
  vbe = "Windows script", js = "Windows script", jse = "Windows script",
  wsf = "Windows script", wsh = "Windows script", hta = "Windows script",
  ps1 = "Windows script", reg = "Windows script", msc = "Windows script",
  lnk = "Windows shortcut", url = "Windows shortcut", scf = "Windows shortcut",
}
-- Pictures are never read for a hidden program: Windows opens them in an image
-- viewer whatever is inside, and a map card holds tens of thousands of them.
local MEDIA = { png = true, jpg = true, jpeg = true, webp = true, bmp = true, gif = true }

local function ext_of(name)
  local e = name:match("%.([^.]+)$")
  return e and e:lower() or nil
end

local function by_name(name)
  if name:lower() == "autorun.inf" then return "autorun file" end
  local e = ext_of(name)
  return e and KIND[e] or nil
end

local function join(dir, name)
  if dir == "/" then return "/" .. name end
  return dir .. "/" .. name
end

local function numeric(name) return name:match("^%d+$") ~= nil end

local function fmt_n(n)
  local s = tostring(n)
  while true do
    local t, k = s:gsub("^(%d+)(%d%d%d)", "%1,%2")
    s = t
    if k == 0 then return s end
  end
end

-- ---- scan state --------------------------------------------------------------
local W, H
local S            -- the current scan or clean-up
local screen = {}  -- labels of the screen on show

-- Both queues are consumed from the front and freed as they go, so they keep an
-- explicit end index: `#` on a table with holes at the front is undefined, and a
-- border in the wrong place would drop or overwrite queued work.
local function new_scan(full)
  return {
    full = full,
    dirs = { { path = "/", start = 1, depth = 0, num = false } },
    di = 1, dn = 1,  -- next folder to list, last folder queued
    checks = {},     -- files waiting for the firmware to look at them
    ci = 1, cn = 0,  -- next file to check, last file queued
    found = {},      -- { path, why }
    files = 0, folders = 0, skipped = 0, errors = 0, busy = 0,
  }
end

-- A tile tree is a folder of numbered folders (z/x/y.png). A quick scan lists the
-- first numbered level, so a numbered folder at the top of the card is still
-- looked into, but does not walk every tile below it.
local function add_dir(path, name, parent)
  S.folders = S.folders + 1
  local num = numeric(name)
  if num and parent.num and not S.full then
    S.skipped = S.skipped + 1
    return
  end
  S.dn = S.dn + 1
  S.dirs[S.dn] = { path = path, start = 1, depth = parent.depth + 1, num = num }
end

local function add_file(path, name)
  S.files = S.files + 1
  local why = by_name(name)
  if CAN_CLEAN then
    -- The firmware has the last word, also on names: it is the rule remove()
    -- applies. Pictures are only judged by name.
    if why or not MEDIA[ext_of(name) or ""] then
      S.cn = S.cn + 1
      S.checks[S.cn] = path
    end
  elseif why then
    S.found[#S.found + 1] = { path = path, why = why }
  end
end

local CHECKS_PER_TICK = 4   -- each one opens a file and reads its first bytes
local PAGE = 24             -- folder entries per tick; every entry is a file open

-- One unit of work: some firmware checks, or else one page of one folder.
-- Returns true when the whole card has been seen.
local function scan_step()
  if S.ci <= S.cn then
    local stop = math.min(S.cn, S.ci + CHECKS_PER_TICK - 1)
    for i = S.ci, stop do
      local path = S.checks[i]
      S.checks[i] = nil
      local why, err = sd.check(path)
      if why then S.found[#S.found + 1] = { path = path, why = why }
      elseif err and err ~= "not found" then S.errors = S.errors + 1 end
    end
    S.ci = stop + 1
    return false
  end
  local d = S.dirs[S.di]
  if not d then return true end
  local entries, err = sd.list(d.path, d.start, PAGE)
  if not entries then
    if err == "busy" and S.busy < 200 then S.busy = S.busy + 1; return false end
    if err == "no sd" then S.nocard = true; return true end
    S.errors = S.errors + 1
    S.dirs[S.di] = nil
    S.di = S.di + 1
    return false
  end
  S.busy = 0
  for _, e in ipairs(entries) do
    local p = join(d.path, e.name)
    if e.type == "dir" then add_dir(p, e.name, d) else add_file(p, e.name) end
  end
  if entries.truncated and entries.next then
    d.start = entries.next            -- same folder, next page, next tick
  else
    S.dirs[S.di] = nil                -- done with it; let the table go
    S.di = S.di + 1
  end
  return false
end

-- ---- screens -------------------------------------------------------------------
local M = 6                              -- page margin
-- A wrapped label at y. Returns the label and the y just below it, measured
-- where the firmware can measure so a long line on a narrow screen cannot run
-- into what follows.
local function text(s, y, size, col)
  size = size or 12
  local l = ui.label(s, M, y, size, col or C.text)
  l:width(W - 2 * M)
  local lines = ui.text_lines and ui.text_lines(s, W - 2 * M, size) or 1
  local lh = ui.text_h and ui.text_h(size) or (size + 4)
  return l, y + lines * lh + 4
end

-- Shown on every screen that could leave someone thinking the card is now clean:
-- this app knows the files it recognises, and only a format is certain.
local NOTICE = tr("SD Scan only removes files it recognises. If the card may be infected, the safest fix is to copy off what you need and format it completely: in Files, hold the SD card row.")

-- The y for a screen's buttons: the bottom edge, or just below the text when the
-- text needs more room than that (a small screen, a longer translation). The page
-- then scrolls instead of the text running under the buttons.
local function button_row(y)
  local by = H - 40
  if y + 6 > by then
    by = y + 6
    ui.scroll(true)
  else
    ui.scroll(false)
  end
  return by
end

local function short(path, max)
  if #path <= max then return path end
  return "..." .. path:sub(#path - max + 4)
end

local show_intro, show_results

local function progress_line()
  return string.format(tr("%s files in %s folders"), fmt_n(S.files), fmt_n(S.folders))
end

local function show_scanning()
  ui.clear()
  sys.keep_awake(true)
  local _, y = text(S.full and tr("Full scan...") or tr("Scanning..."), M, 16, C.accent)
  screen.count, y = text(progress_line(), y, 12, C.text)
  screen.where, y = text("/", y, 12, C.sub)
  ui.button(tr("Stop"), M, button_row(y), 90, 32, function()
    S.stopped = true
  end)
end

local function finish_scan()
  sys.keep_awake(false)
  S.done = true
  show_results()
end

local function remove_step()
  local f = S.found[S.ri]
  if not f then return true end
  local why, err = sd.remove(f.path)
  if why then S.removed = S.removed + 1
  else f.err = err or "failed"; S.failed[#S.failed + 1] = f end
  S.ri = S.ri + 1
  return false
end

local function show_removing()
  ui.clear()
  sys.keep_awake(true)
  S.removing, S.ri, S.removed, S.failed = true, 1, 0, {}
  local _, y = text(tr("Removing..."), M, 16, C.accent)
  screen.count = text("", y, 12, C.text)
end

local function show_removed()
  ui.clear()
  sys.keep_awake(false)
  S.removing = false
  local _, y = nil, M
  if #S.failed == 0 then
    _, y = text(string.format(tr("Removed %d files."), S.removed), y, 16, C.good)
    _, y = text(NOTICE, y, 12, C.text)
  else
    _, y = text(string.format(tr("Removed %d, %d could not be removed."), S.removed, #S.failed),
                y, 16, C.bad)
    _, y = text(NOTICE, y, 12, C.text)
    local lh = math.max(60, H - y - 46)
    local l = ui.list(M, y, W - 2 * M, lh)
    for _, f in ipairs(S.failed) do
      l:add(short(f.path, 40) .. "  (" .. f.err .. ")", function() sys.toast(f.path, 3000) end)
    end
    y = y + lh + 4
  end
  ui.button(tr("Scan again"), M, button_row(y), 120, 32, function() show_intro() end)
end

local function show_confirm()
  ui.clear()
  local n = #S.found
  local _, y = text(string.format(tr("Remove %d files?"), n), M, 16, C.bad)
  _, y = text(tr("They are Windows programs, scripts, shortcuts or autorun files. A mesh radio never uses them, and nothing else on the card is touched."),
              y, 12, C.text)
  -- Cancel first: on a keyboard-only board the first button has the focus, and
  -- a stray Enter should not delete anything.
  local by = button_row(y)
  ui.button(tr("Cancel"), M, by, 100, 32, function() show_results() end)
  ui.button(tr("Remove"), M + 110, by, 100, 32, function() show_removing() end)
end

show_results = function()
  ui.clear()
  sys.keep_awake(false)
  local n = #S.found
  local _, y = nil, M
  if S.nocard then
    _, y = text(tr("No SD card found."), y, 16, C.bad)
    _, y = text(tr("Insert the card and scan again."), y, 12, C.text)
    ui.button(tr("Scan again"), M, button_row(y), 120, 32, function() show_intro() end)
    return
  end
  local how = S.stopped and tr("Stopped after ") or tr("Checked ")
  if n == 0 then
    _, y = text(tr("No known Windows malware found."), y, 16, C.good)
  else
    _, y = text(string.format(tr("%d threats found"), n), y, 16, C.bad)
  end
  local sub = how .. progress_line()
  if S.skipped > 0 then
    sub = sub .. string.format(tr(", map tiles skipped (%s folders)"), fmt_n(S.skipped))
  end
  if S.errors > 0 then sub = sub .. string.format(tr(", %d unreadable"), S.errors) end
  _, y = text(sub, y, 12, C.sub)
  if n == 0 then _, y = text(NOTICE, y, 12, C.text) end
  local again_x = M
  if n > 0 then
    local note = not CAN_CLEAN and
      tr("This firmware can find these but not remove them. Update wadamesh, or format the card: in Files, hold the SD card row.")
    local note_h = 0
    if note then
      note_h = (ui.text_lines and ui.text_lines(note, W - 2 * M, 12) or 3) * (ui.text_h and ui.text_h(12) or 16) + 4
    end
    -- The list takes the room that is left, but never less than a couple of rows:
    -- below that the page scrolls instead.
    local lh = math.max(60, H - 46 - y - note_h)
    local l = ui.list(M, y, W - 2 * M, lh)
    for _, f in ipairs(S.found) do
      l:add(f.why .. ": " .. short(f.path, 40), function() sys.toast(f.path, 3000) end)
    end
    y = y + lh + 4
    if note then _, y = text(note, y, 12, C.text) end
  end
  local by = button_row(y)
  if n > 0 and CAN_CLEAN then
    ui.button(tr("Remove all"), M, by, 110, 32, function() show_confirm() end)
    again_x = M + 120
  end
  ui.button(tr("Scan again"), again_x, by, 110, 32, function() show_intro() end)
end

show_intro = function()
  ui.clear()
  sys.keep_awake(false)
  S = nil
  local _, y = text(tr("SD card malware check"), M, 16, C.accent)
  if not CAN_LIST then
    text(tr("This board has no SD card access for apps."), y, 12, C.bad)
    ui.scroll(false)
    return
  end
  _, y = text(tr("Finds and removes Windows programs, scripts, shortcuts and autorun files, like the worm on some ThinkNode M9 cards. A mesh radio never uses these."),
              y, 12, C.text)
  _, y = text(NOTICE, y, 12, C.text)
  if not CAN_CLEAN then
    _, y = text(tr("This firmware can only find them. Update wadamesh to remove them here."), y, 12, C.sub)
  end
  local by = button_row(y)
  ui.button(tr("Scan"), M, by, 100, 32, function()
    S = new_scan(false); show_scanning()
  end)
  ui.button(tr("Full scan"), M + 110, by, 110, 32, function()
    S = new_scan(true); show_scanning()
  end)
end

-- ---- app callbacks -----------------------------------------------------------
function app.on_open(w, h)
  W, H = w, h
  show_intro()
  timer.every(50)
end

function app.on_tick()
  if not S or S.done and not S.removing then return end
  if S.removing then
    if remove_step() then show_removed()
    elseif screen.count then
      screen.count:set(string.format(tr("%d of %d"), S.ri - 1, #S.found))
    end
    return
  end
  if S.stopped then return finish_scan() end
  if scan_step() then return finish_scan() end
  if screen.count then screen.count:set(progress_line()) end
  local d = S.dirs[S.di]
  if screen.where and d then screen.where:set(short(d.path, 44)) end
end

function app.on_close()
  sys.keep_awake(false)
end

return app
