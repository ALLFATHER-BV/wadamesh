-- SPDX-License-Identifier: GPL-3.0-or-later
-- Ebook (Lua): reads .epub books from the app's own folder (wada.fs).
--
-- Put books in /apps/ebook.d/ (internal flash) or /meshcomod/apps/ebook.d/ (SD
-- card). wada.fs is flat, so a file name is at most 32 characters of
-- A-Z a-z 0-9 . _ - and the book must be UNCOMPRESSED ("stored") inside: there is
-- no inflate in the SDK. scripts/ebook-prep.py repacks any .epub that way.
--
-- Everything is done in small steps on the tick (a callback has an instruction
-- budget): the zip directory, the OPF, the table of contents, then one "section" of
-- a chapter at a time (about 12 KB of text), which is paginated with
-- wada.ui.text_lines so the firmware's own font metrics decide where a line breaks.
-- Images: a book's pictures show as an <image: alt text> line. When the firmware has
-- wada.ui.image (not yet released) the Images menu row chooses between "never" (that
-- line only), "on tap" (tap the line, or the menu's View image row, to see the picture
-- fit to the screen; any tap or key returns) and "always" (a picture gets a page of its
-- own). Without wada.ui.image the app behaves as "never" and the row is not shown.
--
-- The call this app expects (proposed; the code that uses it is in draw_image):
--   local img, err = wada.ui.image(file, offset, length, x, y, w, h)
--     decodes the PNG/JPEG/BMP stored in app file `file` at bytes [offset, offset+length)
--     and draws it inside the box x,y,w,h, scaled down to fit, aspect kept, centred.
--     Returns a widget handle (img:remove() takes it off the screen), or nil, reason.
--
-- Input: touch taps the left/right thirds or swipes left/right; a trackball or D-pad
-- swipes up/down (and left/right); keyboard arrows, space, n/p; a jog wheel or
-- encoder walks the on-screen < menu > buttons. A centre tap / Enter opens the menu.
local ui, sys, timer, store, fs = wada.ui, wada.sys, wada.timer, wada.store, wada.fs
local tr = sys.tr or function(x) return x end
local C = ui.colors
local app = {}

local SYM_LEFT, SYM_RIGHT, SYM_LIST = "\xEF\x81\x93", "\xEF\x81\x94", "\xEF\x80\xBA"
local BULLET, INDENT = "\xE2\x80\xA2 ", "  "
local M = 4
local SIZES = { 12, 14, 16 }
local SECT_TEXT = 12000      -- paragraph bytes per section: bounds Lua heap use and the pause at a section edge
local WIN = 6000             -- raw bytes read per window
local MAX_BOOKS = 100
local PAGE_ROWS = 25         -- chapter rows per page: each row is a real button
local MAX_POS = 10           -- remembered reading positions
local HAS_IMG = type(ui.image) == "function"
local MAX_IMGS = 300         -- picture entries kept from the zip directory (each costs heap)
local IMG_EXT = { png = 1, jpg = 1, jpeg = 1, bmp = 1 }   -- what the firmware can decode
local IMG_MODES = { "always", "tap", "never" }
local IMG_NAMES = { always = "always", tap = "on tap", never = "never" }
local DEFAULT_IMAGES = "never"   -- becomes "tap" once firmware with wada.ui.image ships
local POS_BYTES = 180        -- the store reloads a line of at most 191 bytes, "pos=" included

local W, H
local size = 14
local screen = "none"
local task                       -- the one step function the tick runs
local book                       -- the open book, see open_task
local sec                        -- the section on screen: paras, kinds, off, nxt, eu, pp, po
local page = 1
local text_lbl, stat_lbl, maxl, foot_y
local positions = {}             -- name -> { sp, off, p, o }
local pos_order = {}             -- most recent first
local pred = {}                  -- pred[sp][nxt] = off: section starts already seen, to step backwards
local touch_moved = false
local chap_page = 1
local sec_dirty = false          -- text size or image mode changed: repaginate when the page is shown again
local img_mode = DEFAULT_IMAGES
local inline_img                 -- the picture widget on an image page

local function clear() inline_img = nil; if ui.clear then ui.clear() end end
local function images() return HAS_IMG and img_mode or "never" end
-- text_lines does not count the empty line after a trailing newline, but it is drawn.
local function lines(s)
  return ui.text_lines(s, W - 2 * M, size) + (s:byte(-1) == 10 and 1 or 0)
end
local function set_task(fn) task = fn end

-- ---- small helpers ----------------------------------------------------------
local function cont(b) return b and b >= 0x80 and b < 0xC0 end

local ENT = {
  amp = "&", lt = "<", gt = ">", quot = '"', apos = "'", nbsp = " ",
  ndash = "\xE2\x80\x93", mdash = "\xE2\x80\x94", hellip = "\xE2\x80\xA6",
  lsquo = "\xE2\x80\x98", rsquo = "\xE2\x80\x99", ldquo = "\xE2\x80\x9C", rdquo = "\xE2\x80\x9D",
  bull = "\xE2\x80\xA2", middot = "\xC2\xB7", copy = "\xC2\xA9", reg = "\xC2\xAE",
  trade = "\xE2\x84\xA2", deg = "\xC2\xB0", laquo = "\xC2\xAB", raquo = "\xC2\xBB",
  shy = "", zwj = "", zwnj = "",
}

local function u8(cp)
  if cp == 0xA0 or (cp >= 0x2000 and cp <= 0x200A) or cp == 0x202F or cp == 0x3000 then return " " end
  if cp == 0xAD or (cp >= 0x200B and cp <= 0x200F) or cp == 0xFEFF or cp == 0 then return "" end
  if cp < 0x80 then return string.char(cp)
  elseif cp < 0x800 then return string.char(0xC0 | (cp >> 6), 0x80 | (cp & 0x3F))
  elseif cp < 0x10000 then
    return string.char(0xE0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F))
  elseif cp < 0x110000 then
    return string.char(0xF0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3F), 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F))
  end
  return ""
end

local function entity(name)
  if name:byte(1) == 35 then   -- "#"
    local cp
    local x = name:sub(2, 2)
    if x == "x" or x == "X" then cp = tonumber(name:sub(3), 16) else cp = tonumber(name:sub(2)) end
    return cp and u8(cp) or ""
  end
  return ENT[name] or ""
end

local function decode(s)
  if s:find("&", 1, true) then s = s:gsub("&(#?%w+);", entity) end
  return s
end

local function attrs(s)
  local t = {}
  for k, _, v in s:gmatch("([%w:_%-]+)%s*=%s*([\"'])(.-)%2") do t[k:lower()] = v end
  return t
end

local function plain(s)   -- markup -> one trimmed line of text
  s = decode((s:gsub("<[^>]*>", " "))):gsub("%s+", " ")
  return (s:gsub("^ ", ""):gsub(" $", ""))
end

-- Resolve href against a directory inside the zip ("OEBPS/"), the way a browser would.
local function resolve(dir, href)
  href = href:gsub("#.*$", ""):gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end)
  local out = {}
  for part in (dir .. href):gmatch("[^/]+") do
    if part == ".." then out[#out] = nil elseif part ~= "." then out[#out + 1] = part end
  end
  return table.concat(out, "/")
end

local function dirname(path) return path:match("^(.*/)") or "" end

local function human(n)
  if n >= 1048576 then return string.format("%d.%d MB", n // 1048576, (n % 1048576) * 10 // 1048576) end
  return string.format("%d KB", (n + 1023) // 1024)
end

-- ---- reading positions (wada.store, flushed when the app closes) -------------
-- store.get of a missing key returns the store's own table on some firmware
-- (beta_101), not nil, so only a string or number counts as a stored value.
local function kv(key)
  local v = store.get(key)
  if type(v) == "string" or type(v) == "number" then return v end
  return nil
end

local function load_positions()
  positions, pos_order = {}, {}
  for rec in tostring(kv("pos") or ""):gmatch("[^;]+") do
    local name, sp, off, p, o = rec:match("^([^|]+)|(%d+)|(%d+)|(%d+)|(%d+)$")
    if name then
      positions[name] = { tonumber(sp), tonumber(off), tonumber(p), tonumber(o) }
      pos_order[#pos_order + 1] = name
    end
  end
end

local function save_positions()
  local out, len = {}, 0
  for i = 1, math.min(#pos_order, MAX_POS) do
    local n = pos_order[i]
    local r = positions[n]
    local rec = string.format("%s|%d|%d|%d|%d", n, r[1], r[2], r[3], r[4])
    len = len + #rec + 1
    if len > POS_BYTES then break end
    out[#out + 1] = rec
  end
  store.set("pos", table.concat(out, ";"))
end

local function remember()
  if not (book and sec and sec.pp and sec.pp[page]) then return end
  local name = book.name
  if positions[name] then
    for i, n in ipairs(pos_order) do if n == name then table.remove(pos_order, i); break end end
  end
  table.insert(pos_order, 1, name)
  while #pos_order > MAX_POS do positions[table.remove(pos_order)] = nil end
  positions[name] = { book.sp, sec.off, sec.pp[page], sec.po[page] }
  store.set("last", name)
end

-- ---- the zip container (stored entries only) ----------------------------------
local function zip_open(name)
  local _, total = fs.read(name, 0, 0)
  if not total then return nil, tr("Cannot read this file") end
  if total < 22 then return nil, tr("Not an epub") end
  local n = total < 32768 and total or 32768
  local tail = fs.read(name, total - n, n)
  if not tail then return nil, tr("Cannot read this file") end
  local pos, i = nil, 1
  while true do
    local s = tail:find("PK\5\6", i, true)
    if not s then break end
    pos, i = s, s + 1
  end
  if not pos or pos + 21 > #tail then return nil, tr("Not an epub") end
  local _, _, _, _, cnt, cdsize, cdoff = string.unpack("<I4I2I2I2I2I4I4", tail, pos)
  if cdoff < 0 or cdsize < 0 or cdoff + cdsize > total then return nil, tr("Not an epub") end
  return { name = name, total = total, cnt = cnt, cdoff = cdoff, cdsize = cdsize,
           rd = 0, buf = "", bp = 1, i = 1, ents = {}, nimg = 0 }
end

local ZIP_KEEP = { xhtml = 1, html = 1, htm = 1, xml = 1, opf = 1, ncx = 1, txt = 1 }
local CD_FMT = "<I4I2I2I2I2I2I2I4I4I4I2I2I2I2I2I4I4"

-- Parse the central directory a batch at a time. Returns true when done, false to
-- come back next tick, or nil, err.
local function zip_step(Z)
  for _ = 1, 120 do
    if Z.i > Z.cnt then return true end
    local avail = #Z.buf - Z.bp + 1
    local need = 46
    if avail >= 46 then
      local nlen, elen, clen = string.unpack("<I2I2I2", Z.buf, Z.bp + 28)
      need = 46 + nlen + elen + clen
    end
    if avail < need then
      local want = Z.cdsize - Z.rd
      if want <= 0 then return nil, tr("Not an epub") end
      if want > 8192 then want = 8192 end
      local piece = fs.read(Z.name, Z.cdoff + Z.rd, want)
      if not piece or #piece == 0 then return nil, tr("Cannot read this file") end
      Z.buf, Z.bp, Z.rd = Z.buf:sub(Z.bp) .. piece, 1, Z.rd + #piece
    else
      local sig, _, _, _, method, _, _, _, csize, usize, nlen, elen, clen, _, _, _, lho =
        string.unpack(CD_FMT, Z.buf, Z.bp)
      if sig ~= 0x02014B50 then return nil, tr("Not an epub") end
      local name = Z.buf:sub(Z.bp + 46, Z.bp + 45 + nlen)
      local ext = name:match("%.(%w+)$")
      local x = ext and ext:lower()
      if x and (ZIP_KEEP[x] or (HAS_IMG and IMG_EXT[x] and Z.nimg < MAX_IMGS)) then
        if IMG_EXT[x] then Z.nimg = Z.nimg + 1 end
        Z.ents[name] = { m = method, c = csize, u = usize, h = lho }
      end
      Z.bp = Z.bp + need
      Z.i = Z.i + 1
    end
  end
  return false
end

local function entry_start(Z, e)
  if e.d then return e.d end
  local h = fs.read(Z.name, e.h, 30)
  if not h or #h < 30 then return nil end
  local sig, _, _, _, _, _, _, _, _, nlen, elen = string.unpack("<I4I2I2I2I2I2I4I4I4I2I2", h)
  if sig ~= 0x04034B50 then return nil end
  e.d = e.h + 30 + nlen + elen
  return e.d
end

local function entry_read(Z, e, off, len)
  if e.m ~= 0 then return nil end
  local d = entry_start(Z, e)
  if not d then return nil end
  if off >= e.u then return "" end
  if len > e.u - off then len = e.u - off end
  return fs.read(Z.name, d + off, len)
end

local function entry_all(Z, e, cap)   -- the whole entry (small XML files), or nil, why
  if e.m ~= 0 then return nil, "compressed" end
  if e.u > cap then return nil, "too big" end
  local parts, off = {}, 0
  while off < e.u do
    local piece = entry_read(Z, e, off, 32768)
    if not piece or #piece == 0 then return nil, "read" end
    parts[#parts + 1] = piece
    off = off + #piece
  end
  return table.concat(parts)
end

-- ---- XHTML -> paragraphs (streamed, one window of tags per step) --------------
local BLOCK = {
  p = 1, div = 1, h1 = 1, h2 = 1, h3 = 1, h4 = 1, h5 = 1, h6 = 1, li = 1, tr = 1, td = 1, th = 1,
  blockquote = 1, section = 1, article = 1, ul = 1, ol = 1, table = 1, pre = 1, hr = 1, dd = 1,
  dt = 1, dl = 1, figure = 1, figcaption = 1, aside = 1, header = 1, footer = 1, nav = 1, body = 1,
}
local SKIP = { head = 1, style = 1, script = 1, title = 1, svg = 1 }
local ALT_MAX = 100   -- bytes of alt text kept in a placeholder

-- "<image: alt text>", or "<image>" when the book gives no description.
local function placeholder(a)
  local alt = decode(a.alt or "")
  alt = alt:gsub("%s+", " "):gsub("^ ", ""):gsub(" $", "")
  if #alt > ALT_MAX then
    local cut = ALT_MAX
    while cut > 1 and cont(alt:byte(cut + 1)) do cut = cut - 1 end
    alt = alt:sub(1, cut) .. "\xE2\x80\xA6"
  end
  if alt == "" then return "<" .. tr("image") .. ">" end
  return "<" .. tr("image") .. ": " .. alt .. ">"
end

-- A section ends at a block boundary once it holds SECT_TEXT bytes ("armed"), but only
-- if more text follows: the next paragraph to arrive sets L.cut, and the section
-- stops at the remembered mark (that paragraph is parsed again in the next section).
local function ld_push(L, s, kind, img)
  if L.armed then L.cut = true; return end
  local n = #L.paras + 1
  L.paras[n], L.kinds[n] = s, kind
  if img then L.imgs[n] = img end
  L.bytes = L.bytes + #s
end

local function ld_flush(L)
  local s = table.concat(L.tb)
  L.tb = {}
  s = s:gsub("^%s+", ""):gsub("%s+$", "")
  if s == "" then return false end
  if L.kind == "l" then s = BULLET .. s end
  ld_push(L, s, L.kind)
  return true
end

local function ld_text(L, t)
  if L.skip > 0 or t == "" then return end
  t = t:gsub("%s+", " ")
  L.tb[#L.tb + 1] = decode(t)
end

local function ld_tag(L, tag)
  local c = tag:byte(1)
  if c == 33 or c == 63 then return end                       -- <! and <?
  local closing = c == 47
  local name = tag:match("^/?%s*([%w:_%-]+)")
  if not name then return end
  name = name:lower():gsub("^.*:", "")
  local selfclose = tag:byte(#tag) == 47
  if name == "body" and not closing then L.skip, L.svg = 0, 0 end
  if SKIP[name] then
    if closing then
      if L.skip > 0 then L.skip = L.skip - 1 end
      if name == "svg" and L.svg > 0 then L.svg = L.svg - 1 end
    elseif not selfclose then
      L.skip = L.skip + 1
      if name == "svg" then L.svg = L.svg + 1 end
    end
    return
  end
  -- an <image> inside <svg> is how many books wrap their cover; the svg's text stays hidden
  if L.skip > 0 and not (name == "image" and L.skip == L.svg) then return end
  if name == "br" then L.tb[#L.tb + 1] = "\n"
  elseif name == "img" or name == "image" then
    if not closing then
      ld_flush(L)
      local a = attrs(tag)
      local src = a.src or a["xlink:href"] or a.href
      ld_push(L, placeholder(a), "i", src and resolve(L.dir, src) or nil)
    end
  elseif BLOCK[name] then
    local pushed = ld_flush(L)
    if closing then
      L.kind = "p"
      if pushed and not L.armed and L.bytes >= SECT_TEXT then L.arm = true end
    elseif name:match("^h%d$") then L.kind = "h"
    elseif name == "li" then L.kind = "l"
    else L.kind = "p" end
  end
end

local function ld_fill(L)
  local piece = entry_read(L.Z, L.e, L.fo, WIN) or ""
  L.buf = L.buf:sub(L.bp) .. piece
  L.bo, L.bp = L.bo + (L.bp - 1), 1
  L.fo = L.fo + #piece
  if #piece == 0 or L.fo >= L.e.u then L.eof = true end
end

-- One step of loading a section. Returns true when the section is complete (L.sec).
local function ld_step(L)
  if #L.buf - L.bp < 3000 and not L.eof then ld_fill(L) end
  local n = 0
  while n < 150 do
    local buf = L.buf
    local lt = buf:find("<", L.bp, true)
    if not lt then
      if L.eof then ld_text(L, buf:sub(L.bp)); L.bp = #buf + 1; break end
      if #buf - L.bp > 2000 then   -- a long run of text: take it up to the last space
        local win = buf:sub(L.bp, L.bp + 2400)
        local take = win:match("^.*() ") or #win   -- no space at all (CJK, a long URL): cut on a character
        while take > 1 and cont(buf:byte(L.bp + take)) do take = take - 1 end
        ld_text(L, buf:sub(L.bp, L.bp + take - 1))
        L.bp = L.bp + take
      end
      break
    end
    local gt
    if buf:sub(lt, lt + 3) == "<!--" then
      local _, e = buf:find("-->", lt + 4, true)
      gt = e
    else
      gt = buf:find(">", lt, true)
    end
    if not gt then
      if L.eof then ld_text(L, buf:sub(L.bp, lt - 1)); L.bp = #buf + 1
      elseif #buf - lt > 2900 then   -- never closes within a window: treat the "<" as text
        ld_text(L, buf:sub(L.bp, lt)); L.bp = lt + 1
        n = n + 1
        goto continue
      end
      break
    end
    ld_text(L, buf:sub(L.bp, lt - 1))
    ld_tag(L, buf:sub(lt + 1, gt - 1))
    L.bp = gt + 1
    n = n + 1
    if L.arm then L.arm, L.armed, L.mark = false, true, L.bo + gt end
    if L.cut then break end
    ::continue::
  end
  local fin = L.eof and L.bp > #L.buf
  if fin and not L.cut then ld_flush(L) end
  if L.cut or fin then
    if #L.paras == 0 then L.paras[1], L.kinds[1] = tr("(empty)"), "p" end
    L.sec = { paras = L.paras, kinds = L.kinds, imgs = L.imgs, off = L.start, nxt = L.cut and L.mark or nil }
    return true
  end
  return false
end

local function ld_new(Z, e, start, dir)
  return { Z = Z, e = e, dir = dir, imgs = {}, start = start, fo = start, bo = start, buf = "", bp = 1, eof = false,
           tb = {}, paras = {}, kinds = {}, bytes = 0, kind = "p", skip = 0, svg = 0, cut = false, arm = false, armed = false }
end

-- ---- pagination ---------------------------------------------------------------
-- A placeholder that fits on one line is centred with leading spaces (the page is one
-- left-aligned label, so spaces are the only way to place a single line). Centred, it
-- sits under the middle third of the screen, the only place a tap opens a picture.
local function centred(t)
  local room, sp = W - 2 * M - ui.text_w(t, size), ui.text_w(" ", size)
  if room <= 0 or sp <= 0 then return t end
  return string.rep(" ", room // 2 // sp) .. t
end

local function seg(s, p, o, e)    -- text of paragraph p from byte o to byte e
  local t = s.paras[p]:sub(o, e)
  if o == 1 then
    local k = s.kinds[p]
    if k == "p" then t = INDENT .. t
    elseif k == "i" then   -- a blank line above a picture's placeholder, and below it
      if e >= #s.paras[p] then t = centred(t) .. "\n" end
      t = "\n" .. t
    end
  elseif s.kinds[p] == "i" and e >= #s.paras[p] then t = t .. "\n" end
  return t
end

-- The longest prefix of paragraph p (from byte o) that fits in `left` lines, cut at a
-- word boundary. Binary search over text_lines, so the firmware's wrapping decides.
local function fit(s, p, o, left)
  local str = s.paras[p]
  local lo, hi = o - 1, #str
  while hi - lo > 1 do
    local mid = (lo + hi) // 2
    while mid > lo and cont(str:byte(mid + 1)) do mid = mid - 1 end   -- never cut inside a UTF-8 sequence
    if mid <= lo then break end
    if lines(seg(s, p, o, mid)) <= left then lo = mid else hi = mid end
  end
  if lo < o then return nil end
  if str:byte(lo + 1) ~= 32 then
    local sp = str:sub(o, lo):match("^.*()%s")
    if sp and sp > 1 then lo = o + sp - 2 end
  end
  return lo
end

-- Can paragraph p be shown as a picture? Only a stored (uncompressed) entry can be read.
local function img_ok(s, p)
  local e = s.imgs[p] and book and book.Z.ents[s.imgs[p]]
  return e ~= nil and e.m == 0
end

-- Where the page that starts at (p, o) ends: returns the start of the next page.
-- With `always`, a picture takes a page to itself.
local function build_page(s, p, o, always)
  local left, first = maxl, true
  while p <= #s.paras do
    if always and img_ok(s, p) then
      if first then p, o = p + 1, 1 end
      break
    end
    local n = lines(seg(s, p, o, #s.paras[p]))
    if n <= left then
      left, p, o, first = left - n, p + 1, 1, false
      if left == 0 then break end
    else
      if left >= 2 or first then
        local k = fit(s, p, o, left)
        if not k and first then   -- nothing fits even on an empty page: take one character
          k = o
          while cont(s.paras[p]:byte(k + 1)) do k = k + 1 end
        end
        if k then
          o = k + 1
          while s.paras[p]:byte(o) == 32 do o = o + 1 end
          if o > #s.paras[p] then p, o = p + 1, 1 end
        end
      end
      break
    end
  end
  return p, o
end

local function page_str(s, i)
  local p, o = s.pp[i], s.po[i]
  local ep, eo = s.pp[i + 1], s.po[i + 1]
  if not ep then ep, eo = #s.paras + 1, 1 end
  local out = {}
  for q = p, math.min(ep, #s.paras) do
    local so = q == p and o or 1
    local se = q == ep and eo - 1 or #s.paras[q]
    if se >= so then out[#out + 1] = seg(s, q, so, se) end
  end
  return table.concat(out, "\n")
end

-- The picture whose placeholder is on page i: the one at screen row y, or the first
-- when y is nil. Returns its zip path.
local function page_image(s, i, y)
  local p, o = s.pp[i], s.po[i]
  local ep, eo = s.pp[i + 1], s.po[i + 1]
  if not ep then ep, eo = #s.paras + 1, 1 end
  local row, lh = 0, ui.text_h(size)
  for q = p, math.min(ep, #s.paras) do
    local so = q == p and o or 1
    local se = q == ep and eo - 1 or #s.paras[q]
    if se >= so then
      local n = lines(seg(s, q, so, se))
      if s.imgs[q] and (y == nil or (y >= M + row * lh and y < M + (row + n) * lh)) then return s.imgs[q] end
      row = row + n
    end
  end
end

local function find_page(s, p, o)
  local at = 1
  for i = 1, #s.pp do
    if s.pp[i] < p or (s.pp[i] == p and s.po[i] <= o) then at = i else break end
  end
  return at
end

-- ---- screens --------------------------------------------------------------------
local show_library, show_menu, show_chapters, show_reading, show_error, show_image

-- Draw a picture from the open book, or nil if it cannot be (missing, compressed,
-- or no wada.ui.image). Never raises: a book must stay readable.
local function draw_image(path, x, y, w, h)
  local e = HAS_IMG and book and book.Z.ents[path]
  local d = e and e.m == 0 and entry_start(book.Z, e)
  if not d then return nil end
  local ok, handle = pcall(ui.image, book.Z.name, d, e.u, x, y, w, h)
  return ok and handle or nil
end

local function show_page()
  if screen ~= "reading" or not text_lbl then return end
  if not (sec and sec.pp and sec.pp[page]) then return end
  if inline_img then inline_img:remove(); inline_img = nil end
  local whole = sec.always and sec.po[page] == 1 and img_ok(sec, sec.pp[page]) and sec.imgs[sec.pp[page]]
  if whole then inline_img = draw_image(whole, M, M, W - 2 * M, foot_y - 2 * M) end
  text_lbl:set(inline_img and "" or page_str(sec, page))
  local span = (sec.nxt or sec.eu) - sec.off
  local pct = (sec.off + (page - 1) * (span // #sec.pp)) // ((sec.eu // 100) + 1)
  if pct > 99 then pct = 99 end
  if page == #sec.pp and not sec.nxt and book.sp == #book.spine then pct = 100 end
  stat_lbl:set(string.format("%d/%d  %d%%", book.sp, #book.spine, pct))
  remember()
end

-- Paginate the section a few pages per tick, then call done(). The page list is
-- swapped in only at the end, so the section on screen stays usable meanwhile.
local function paginate(s, done)
  local always = images() == "always"
  local p, o = 1, 1
  local pp, po = {}, {}
  set_task(function()
    for _ = 1, 3 do
      if p > #s.paras then
        set_task(nil)
        if #pp == 0 then pp[1], po[1] = 1, 1 end
        s.pp, s.po, s.always = pp, po, always
        done()
        return
      end
      pp[#pp + 1], po[#po + 1] = p, o
      local p2, o2 = build_page(s, p, o, always)
      if p2 == p and o2 == o then p2, o2 = p + 1, 1 end   -- always advance
      p, o = p2, o2
    end
  end)
end

-- Load (and paginate) a section. want = "this": the section starting at `off`;
-- "last": the final section of the chapter; "before": the one whose end is `stop`.
-- `at` says where to land: "first", "last" or { p, o }.
local function goto_section(sp, off, want, stop, at)
  local item = book.spine[sp]
  local e = item and book.Z.ents[item.path]
  if not e then show_error(tr("A chapter is missing from this book")); return end
  if e.m ~= 0 then show_error(tr("This book is compressed. Repack it with scripts/ebook-prep.py")); return end
  if screen == "reading" and text_lbl then text_lbl:set(tr("Loading...")); stat_lbl:set("") end
  local dir = dirname(item.path)
  local L = ld_new(book.Z, e, off, dir)
  book.sp = sp
  set_task(function()
    if not ld_step(L) then return end
    local s = L.sec
    s.eu = e.u
    if s.nxt then
      pred[sp] = pred[sp] or {}
      pred[sp][s.nxt] = s.off
    end
    if want == "last" and s.nxt then L = ld_new(book.Z, e, s.nxt, dir); return end
    if want == "before" and s.nxt and s.nxt ~= stop then L = ld_new(book.Z, e, s.nxt, dir); return end
    set_task(nil)
    paginate(s, function()
      sec = s
      if at == "last" then page = #s.pp
      elseif type(at) == "table" then page = find_page(s, at[1], at[2])
      else page = 1 end
      show_page()
    end)
  end)
end

local function next_page()
  if not (book and sec and sec.pp) or task then return end
  if page < #sec.pp then page = page + 1; show_page(); return end
  if sec.nxt then goto_section(book.sp, sec.nxt, "this", nil, "first")
  elseif book.sp < #book.spine then goto_section(book.sp + 1, 0, "this", nil, "first")
  else sys.toast(tr("End of book"), 1200) end
end

local function prev_page()
  if not (book and sec and sec.pp) or task then return end
  if page > 1 then page = page - 1; show_page(); return end
  if sec.off > 0 then
    local before = pred[book.sp] and pred[book.sp][sec.off]
    if before then goto_section(book.sp, before, "this", nil, "last")
    else goto_section(book.sp, 0, "before", sec.off, "last") end
  elseif book.sp > 1 then goto_section(book.sp - 1, 0, "last", nil, "last")
  else sys.toast(tr("Start of book"), 1200) end
end

local function relayout()   -- after a text size change: keep the position, repaginate
  if not (sec and sec.pp) then return end
  local s, p, o = sec, sec.pp[page], sec.po[page]
  text_lbl:set(tr("Loading..."))
  stat_lbl:set("")
  paginate(s, function() page = find_page(s, p, o); show_page() end)
end

function show_reading()
  clear()
  screen = "reading"
  local gap = 4
  local bh = math.max(34, ui.text_h(16) + 12)
  local bw = math.max(40, bh)
  foot_y = H - bh - M
  maxl = math.max(1, (foot_y - 2 * M) // ui.text_h(size))
  -- The next button is created first so a jog wheel or trackball focus lands on it.
  ui.button(SYM_RIGHT, W - M - bw, foot_y, bw, bh, next_page)
  ui.button(SYM_LEFT, M, foot_y, bw, bh, prev_page)
  ui.button(SYM_LIST, M + bw + gap, foot_y, bw, bh, function() show_menu() end)
  stat_lbl = ui.label("", M + 2 * (bw + gap) + gap, foot_y + (bh - ui.text_h(12)) // 2, 12, C.sub)
  stat_lbl:width(W - 3 * bw - 4 * gap - 2 * M, "center")
  text_lbl = ui.label(tr("Opening..."), M, M, size, C.text)
  text_lbl:width(W - 2 * M)
  if sec_dirty then
    sec_dirty = false
    relayout()
  elseif sec and sec.pp then
    show_page()
  end
end

-- One picture, fit to the screen; any tap or key goes back to the page.
function show_image(path)
  clear()
  screen = "image"
  text_lbl, stat_lbl = nil, nil
  if not draw_image(path, 0, 0, W, H) then
    show_reading()
    sys.toast(tr("Cannot show this image"), 1500)
  end
end

function show_error(msg)
  set_task(nil)
  clear()
  screen = "error"
  text_lbl, stat_lbl = nil, nil
  local l = ui.label(tr("Cannot open this book"), M, M, 16, C.bad, true)
  l:width(W - 2 * M)
  local d = ui.label(msg, M, M + ui.text_h(16) + 8, 14, C.text)
  d:width(W - 2 * M)
  ui.button(tr("Books"), M, H - 40 - M, W - 2 * M, 40, function() show_library() end)
end

-- ---- opening a book -------------------------------------------------------------
-- The table of contents (EPUB3 nav or EPUB2 NCX), read in windows and parsed 25
-- entries per tick: a real book has hundreds. T.titles maps a file path to the label
-- of the first entry that points at it.
local function toc_new(Z, item, is_nav)
  local e = Z.ents[item.path]
  if not e or e.m ~= 0 then return nil end
  return { Z = Z, e = e, dir = dirname(item.path), nav = is_nav, fo = 0, buf = "", bp = 1,
           eof = false, intoc = false, titles = {} }
end

local function toc_add(T, href, label)
  local path = resolve(T.dir, href)
  if not T.titles[path] then T.titles[path] = plain(label) end
end

-- Returns true when the table of contents is finished.
local function toc_step(T)
  if #T.buf - T.bp < 4000 and not T.eof then
    local piece = entry_read(T.Z, T.e, T.fo, 8192) or ""
    T.buf = T.buf:sub(T.bp) .. piece
    T.bp = 1
    T.fo = T.fo + #piece
    if #piece == 0 or T.fo >= T.e.u then T.eof = true end
  end
  local buf = T.buf
  for _ = 1, 25 do
    if T.nav and not T.intoc then   -- find the <nav> that carries epub:type="toc"
      local s, f = buf:find("<nav[^>]*>", T.bp)
      if not s then
        if T.eof then return true end
        T.bp = math.max(T.bp, #buf - 3)
        return false
      end
      T.intoc = buf:sub(s, f):find("toc", 1, true) ~= nil
      T.bp = f + 1
    else
      local s, f, a, label
      if T.nav then
        local close = buf:find("</nav>", T.bp, true)
        s, f, a, label = buf:find("<a%s+([^>]*)>(.-)</a>", T.bp)
        if close and (not s or close < s) then return true end
        if s then local href = attrs(a).href; if href then toc_add(T, href, label) end end
      else
        local c
        s, f, label, c = buf:find("<navLabel[^>]*>(.-)</navLabel>%s*<content([^>]*)>", T.bp)
        if s then local src = attrs(c).src; if src then toc_add(T, src, label) end end
      end
      if s then
        T.bp = f + 1
      elseif T.eof then
        return true
      else   -- the entry continues in the next window: keep it
        local keep = buf:find(T.nav and "<a[%s>]" or "<navLabel", T.bp)
        T.bp = keep or math.max(T.bp, #buf - 12)
        if keep and #buf - keep >= 4000 then T.bp = keep + 2 end   -- too long to be one entry: skip it
        return false
      end
    end
  end
  return false
end

local COMPRESSED = "This book is compressed. Repack it with scripts/ebook-prep.py"

local function open_task(name)
  local phase = "zip"
  local Z, opf, opf_dir, mpos, spos, spine, toc
  local want, items, spine_ids, meta = {}, {}, {}, {}
  return function()
    if phase == "zip" then
      local z, err = zip_open(name)
      if not z then show_error(err); return end
      Z, phase = z, "dir"
    elseif phase == "dir" then
      local done, err = zip_step(Z)
      if done == nil then show_error(err) elseif done then phase = "container" end
    elseif phase == "container" then
      local ce = Z.ents["META-INF/container.xml"]
      if ce and ce.m ~= 0 then show_error(tr(COMPRESSED)); return end
      local xml = ce and entry_all(Z, ce, 16384)
      local full = xml and xml:match("full%-path%s*=%s*[\"']([^\"']+)[\"']")
      if not full then show_error(tr("Not an epub")); return end
      full = resolve("", full)
      local oe = Z.ents[full]
      if not oe then show_error(tr("Not an epub")); return end
      if oe.m ~= 0 then show_error(tr(COMPRESSED)); return end
      local doc, why = entry_all(Z, oe, 81920)
      if not doc then show_error(why == "too big" and tr("The book index is too large") or tr("Not an epub")); return end
      opf, opf_dir = doc, dirname(full)
      meta.title = plain(opf:match("<dc:title[^>]*>(.-)</dc:title>") or "")
      meta.author = plain(opf:match("<dc:creator[^>]*>(.-)</dc:creator>") or "")
      spos = opf:find("<spine", 1, true) or 1
      phase = "spine"
    elseif phase == "spine" then
      for _ = 1, 150 do
        local s, f, body = opf:find("<itemref%s+([^>]*)>", spos)
        if not s then
          local toc = opf:match("<spine[^>]-toc%s*=%s*[\"']([^\"']+)[\"']")
          if toc then want[toc] = true; meta.toc_id = toc end
          phase, mpos = "manifest", opf:find("<manifest", 1, true) or 1
          break
        end
        local id = attrs(body).idref
        if id then spine_ids[#spine_ids + 1] = id; want[id] = true end
        spos = f + 1
      end
    elseif phase == "manifest" then
      for _ = 1, 150 do
        local s, f, body = opf:find("<item%s+([^>]*)>", mpos)
        if not s then phase = "toc"; break end
        local a = attrs(body)
        local is_nav = (a.properties or ""):find("nav", 1, true) ~= nil
        if a.id and a.href and (want[a.id] or is_nav) then
          items[a.id] = { path = resolve(opf_dir, a.href), nav = is_nav }
        end
        mpos = f + 1
      end
    elseif phase == "toc" then
      spine = {}
      for _, id in ipairs(spine_ids) do
        local it = items[id]
        if it and Z.ents[it.path] then spine[#spine + 1] = { path = it.path } end
      end
      if #spine == 0 then show_error(tr("No readable chapters")); return end
      local nav_item
      for _, it in pairs(items) do if it.nav then nav_item = it; break end end
      local ncx_item = meta.toc_id and items[meta.toc_id]
      toc = nav_item and toc_new(Z, nav_item, true) or ncx_item and toc_new(Z, ncx_item, false)
      opf, items = nil, nil
      phase = toc and "titles" or "start"
    elseif phase == "titles" then
      if toc_step(toc) then
        for _, sp in ipairs(spine) do sp.title = toc.titles[sp.path] end
        phase = "start"
      end
    elseif phase == "start" then
      book = { name = name, Z = Z, spine = spine, title = meta.title ~= "" and meta.title or name,
               author = meta.author, sp = 1 }
      pred = {}
      sec = nil
      set_task(nil)
      local r = positions[name]
      if r and r[1] <= #spine then goto_section(r[1], r[2], "this", nil, { r[3], r[4] })
      else goto_section(1, 0, "this", nil, "first") end
    end
  end
end

local function open_book(name)
  book, sec = nil, nil
  show_reading()
  text_lbl:set(tr("Opening..."))
  set_task(open_task(name))
end

-- ---- library, menu, chapters ------------------------------------------------------
local README = "Put .epub books in this folder.\n\n" ..
  "File names: up to 32 characters of A-Z a-z 0-9 . _ - (no spaces).\n" ..
  "Books must be stored uncompressed inside the .epub file. Convert one with\n" ..
  "scripts/ebook-prep.py from the wadamesh repository.\n"

function show_library()
  clear()
  screen = "library"
  text_lbl, stat_lbl = nil, nil
  ui.label(tr("Books"), M, M, 16, C.text, true)
  local y = M + ui.text_h(16) + 6
  local list = ui.list(M, y, W - 2 * M, H - y - M - ui.text_h(12) - 4)
  local hint = ui.label("", M, H - M - ui.text_h(12), 12, C.sub)
  hint:width(W - 2 * M)
  if book then list:add(SYM_LEFT .. "  " .. tr("Back to book"), function() show_reading() end) end
  local names = {}
  for _, f in ipairs(fs.list() or {}) do
    if f.name:lower():sub(-5) == ".epub" and #names < MAX_BOOKS then names[#names + 1] = f end
  end
  table.sort(names, function(a, b) return a.name:lower() < b.name:lower() end)
  for _, f in ipairs(names) do
    list:add(f.name:sub(1, -6) .. "  \xC2\xB7  " .. human(f.size), function() open_book(f.name) end)
  end
  if #names == 0 then
    hint:set(tr("No books. Add .epub files to this app's folder."))
    if not fs.read("README.txt", 0, 0) then fs.write("README.txt", README) end   -- creates the folder too
  else
    hint:set(string.format(tr("%d book(s)"), #names))
  end
end

local function size_label() return tr("Text size") .. ": " .. size end
local function images_label() return tr("Images") .. ": " .. tr(IMG_NAMES[img_mode]) end

function show_menu()
  remember()
  clear()
  screen = "menu"
  text_lbl, stat_lbl = nil, nil
  local t = ui.label(book and book.title or "", M, M, 16, C.text, true)
  t:width(W - 2 * M)
  local y = M + ui.text_h(16) + 4
  if book and book.author ~= "" then
    ui.label(book.author, M, y, 12, C.sub):width(W - 2 * M)
    y = y + ui.text_h(12) + 2
  end
  local list = ui.list(M, y + 4, W - 2 * M, H - y - 2 * M - 4)
  local size_row, img_row
  list:add(SYM_LEFT .. "  " .. tr("Continue reading"), function() show_reading() end)
  local pic = images() == "tap" and sec and sec.pp and sec.pp[page] and page_image(sec, page)
  if pic then list:add(tr("View image"), function() show_image(pic) end) end
  list:add(tr("Chapters"), function() chap_page = (book.sp - 1) // PAGE_ROWS + 1; show_chapters() end)
  size_row = list:add(size_label(), function()
    for i, v in ipairs(SIZES) do
      if v == size then size = SIZES[i % #SIZES + 1]; break end
    end
    store.set("sz", size)
    list:set(size_row, size_label())
    sec_dirty = true
  end)
  if HAS_IMG then
    img_row = list:add(images_label(), function()
      for i, v in ipairs(IMG_MODES) do
        if v == img_mode then img_mode = IMG_MODES[i % #IMG_MODES + 1]; break end
      end
      store.set("img", img_mode)
      list:set(img_row, images_label())
      sec_dirty = true
    end)
  end
  list:add(tr("Books"), function() show_library() end)
end

function show_chapters()
  clear()
  screen = "chapters"
  local n = #book.spine
  local first = (chap_page - 1) * PAGE_ROWS + 1
  local last = math.min(n, first + PAGE_ROWS - 1)
  ui.label(string.format("%s  %d-%d / %d", tr("Chapters"), first, last, n), M, M, 16, C.text, true)
  local y = M + ui.text_h(16) + 6
  local list = ui.list(M, y, W - 2 * M, H - y - M)
  list:add(SYM_LEFT .. "  " .. tr("Back"), function() show_menu() end)
  if first > 1 then
    list:add(tr("Earlier chapters"), function() chap_page = chap_page - 1; show_chapters() end)
  end
  local sel
  for i = first, last do
    local title = book.spine[i].title or (tr("Section") .. " " .. i)
    local row = list:add(i .. ".  " .. title, function()
      sec = nil
      show_reading()
      goto_section(i, 0, "this", nil, "first")
    end)
    if i == book.sp then sel = row end
  end
  if last < n then
    list:add(tr("Later chapters"), function() chap_page = chap_page + 1; show_chapters() end)
  end
  if sel then list:select(sel) end
end

-- ---- lifecycle ---------------------------------------------------------------------
function app.on_open(w, h)
  W, H = w, h
  if not (fs and ui.clear and ui.text_lines) then
    ui.label(tr("Ebook needs a newer firmware"), M, M, 14, C.bad):width(w - 2 * M)
    return
  end
  local sz = tonumber(kv("sz"))
  for _, v in ipairs(SIZES) do if v == sz then size = sz end end
  local im = kv("img")
  for _, v in ipairs(IMG_MODES) do if v == im then img_mode = im end end
  load_positions()
  local last = kv("last")
  if last and fs.read(tostring(last), 0, 0) then open_book(last) else show_library() end
  timer.every(40)
end

function app.on_tick()
  if task then task() end
end

function app.on_input(ev)
  local t = ev.type
  if screen == "image" then
    if t == "up" or t == "key" then show_reading() end
    return
  end
  if screen ~= "reading" then return end
  if t == "down" then touch_moved = false
  elseif t == "swipe" then
    touch_moved = true
    local d = ev.dir
    if d == "left" or d == "down" then next_page()
    elseif d == "right" or d == "up" then prev_page() end
  elseif t == "up" then
    if touch_moved or (ev.y or 0) >= foot_y then return end
    local x = ev.x or W // 2
    if x < W // 3 then prev_page()
    elseif x > W - W // 3 then next_page()
    else
      -- a tap on a picture's placeholder views it ("on tap") or does nothing ("never")
      local pic = images() ~= "always" and sec and sec.pp and page_image(sec, page, ev.y or 0)
      if not pic then show_menu()
      elseif images() == "tap" then show_image(pic) end
    end
  elseif t == "key" then
    local k = ev.key
    if k == "right" or k == "down" or k == " " or k == "n" then next_page()
    elseif k == "left" or k == "up" or k == "p" or k == "backspace" then prev_page()
    elseif k == "i" and images() == "tap" and sec and sec.pp then
      local pic = page_image(sec, page)
      if pic then show_image(pic) end
    elseif k == "enter" or k == "m" then show_menu()
    elseif k == "c" and book then chap_page = (book.sp - 1) // PAGE_ROWS + 1; show_chapters() end
  end
end

function app.on_close()
  remember()
  save_positions()
end

return app
