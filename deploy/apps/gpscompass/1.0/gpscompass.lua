-- GPS Compass — heading dial plus the live GPS fix, in the RF Monitor's look.
--
-- Heading comes from the magnetometer where the board has one (ThinkNode M9:
-- QMC6309, via wada.sys.compass()) and falls back to GPS course-over-ground
-- while moving on every other board. The dial turns so the heading sits
-- under the fixed lubber mark at the top; a contact with a known position
-- can be picked as a target and is drawn on the dial with bearing and range.
--
-- The magnetometer is raw: the firmware hands out x/y/z in Gauss in the
-- sensor's own frame, uncalibrated. This app does the rest —
--   * hard-iron calibration: press C (or Cal), turn the device through every
--     orientation for ~20 s, press C again. Offsets persist in wada.store.
--   * orientation: the sensor's axes vs. the screen are not documented for
--     the M9, so the app works it out. Hold the device flat, point the top
--     edge at north and press A. Which way the sensor's Z axis faces decides
--     whether the heading runs clockwise or anticlockwise, and that follows
--     from the sign of the vertical field: Earth's field dips DOWN in the
--     northern hemisphere and UP in the southern one, so with a GPS fix (or
--     the last known position) the app reads the handedness off the sensor
--     itself and only needs the one press for the rest. Both persist.
-- Magnetic declination is not applied: this is a magnetic compass.
local ui, sys, mesh, store, timer = wada.ui, wada.sys, wada.mesh, wada.store, wada.timer
local C = ui.colors
local AMBER  = 0xE8A33D
local RING   = 0x3A424A        -- dial ring + minor ticks
local RING2  = 0x252C33        -- inner ring
local PANEL  = C.panel or 0x15181B   -- the dial's own surface (older hosts: literal)
local app = {}

local caps, W, H
local landscape
local cv, D, R, CX, CY            -- dial canvas, diameter, radius, centre
local dial_x, dial_y = 0, 0       -- canvas position in the body (tap hit-test)
local sats_cv, SATS_W, SATS_H = nil, 52, 8
local TH12, TH14, TH16 = 15, 17, 19   -- font line heights, replaced from ui.text_h
local L = {}                      -- labels by name

local has_compass = false
local has_accel = false            -- QMI8658 present: gravity for tilt
local acc = nil                    -- last accelerometer sample, g, sensor frame
local cal = nil                   -- { ox, oy, oz } hard-iron offsets
local calib = nil                 -- in-progress: { mn = {x,y,z}, mx = {x,y,z}, t0 }
local align = 0                   -- degrees added to the raw angle so north reads 000
local align_pending_decl = false  -- A was pressed before the model had a fix
local hint_normal = ""            -- bottom row text outside diagnostics
-- Magnetic declination: the angle between magnetic north (what the sensor
-- measures) and true north (what every GPS bearing is relative to). Mixing the
-- two offsets every waypoint by exactly this much -- measured on hardware as
-- all contacts sitting 22 degrees west, including ones whose positions were
-- known to be right. East positive, so true = magnetic + decl.
local decl = 0
local decl_src = "none"           -- "model" | "stale" | "manual" | "none"
local decl_lat, decl_lon = nil, nil   -- where the model was last evaluated
local DECL_REFRESH_KM = 2         -- it moves ~1 deg per 100 km; 2 km is free
local BUILD_YEAR = 2026.6         -- used only when the clock has never been set
local mag_norm = nil              -- |B| after offsets, Gauss (sanity check for the user)
local tilt_deg = nil              -- how far from level, degrees (nil = unknown)
local weak_field = false          -- WMM caution/blackout zone: compass unreliable
local mag_sat = false             -- last magnetometer sample was flagged as saturated

local hv_x, hv_y = 0, 0           -- smoothed heading unit vector
local heading = nil               -- degrees 0..360, or nil
local src = "none"                -- "mag" | "gps" | "none"
local last_mag_ms = 0
local TICK_MS = 100               -- dial update rate
local SMOOTH = 0.5                -- per-tick blend toward the new heading (1 = none)

local targets, target_i = {}, 0   -- contacts with a position; 0 = none
local next_contacts_ms = 0
local CONTACTS_EVERY = 20000      -- positions only change on adverts

-- Units: altitude and speed each carry their own, imperial by default. Up and
-- down move the selection between the two rows; OK (or a tap on the row)
-- switches that row's units.
local UNITS = { alt = true, spd = true }   -- true = imperial
local sel = "alt"
local row_hit = {}                -- name -> { y, h } in body coordinates
local row_y = {}                  -- name -> y, so diagnostics can move rows
local col_x0, col_x1 = 0, 0       -- the stats column's horizontal span
local val_x, val_w = 0, 0         -- where the value column starts, and its width

local diag = false                -- D: show what the app is actually computing
local diag_m = nil                -- last raw sample, for the diagnostic rows

local press = nil                 -- pending touch: { x, y } from the last "down"
local last_swipe_ms = -100000     -- debounce: LVGL's gesture and the hardware swipe
                                  -- detector can both report one finger swipe
local name_max = 16               -- target-name characters that fit the value column
local compact = false             -- narrow column at a big font: short strings
local last_text = {}              -- label text cache: LVGL relayout only on change
local last_dial_key, last_sats_key = nil, nil
local CAL_SECS = 20

local CARD = { "N","NNE","NE","ENE","E","ESE","SE","SSE",
               "S","SSW","SW","WSW","W","WNW","NW","NNW" }
-- Which north the dial is showing. Bearings computed from coordinates are
-- always true; the dial is only true once a declination is known.
local function href()
  return decl_src == "none" and "M" or "T"
end

local function cardinal(deg)
  return CARD[(math.floor((deg + 11.25) / 22.5) % 16) + 1]
end

local function norm360(d)
  d = d % 360
  if d < 0 then d = d + 360 end
  return d
end

-- ---------------------------------------------------------------------------
-- Magnetic declination (WMM2025), so the dial and the bearings share a north.
--
-- The magnetometer measures MAGNETIC north; every bearing computed from GPS
-- coordinates is relative to TRUE north. Plotting one against the other offsets
-- every waypoint by the local declination -- measured on hardware as all
-- contacts, including ones whose positions were known to be right, sitting
-- about 22 degrees west of where they belong.
--
-- This is the real World Magnetic Model rather than a lookup table: degree 12,
-- 4.4 KB of coefficients and code, and it reproduces NOAA's own calculator to
-- 0.00005 degrees at seventeen sites worldwide, verified in this exact
-- single-precision interpreter. A grid accurate enough to stay inside a degree
-- would have cost five times the space and still been worse.
--
-- Includes the secular-variation terms, so it is exact across its 2025.0-2030.0
-- window rather than drifting ~0.1 deg/yr from a frozen snapshot. Past expiry
-- it degrades gracefully -- roughly 0.14 deg/yr -- so it stays inside a degree
-- until about 2033 even if nobody reissues it.
--
-- Regenerate or verify: scripts/wmm/README.md. The block below is generated;
-- scripts/wmm/gen_lua.py --check catches it drifting from the coefficients.
-- WMM2025 magnetic declination, degree 12.
-- Data: NOAA/NCEI WMM.COF epoch 2025.0, valid 2025.0-2030.0.
-- Source: https://www.ncei.noaa.gov/sites/default/files/2024-12/WMM2025COF.zip
-- WMM-GEN BEGIN -- generated by scripts/wmm/gen_lua.py; do not edit by hand
-- WMM2025 magnetic declination, degree 12. East positive:
--     true bearing = magnetic bearing + declination
-- Data: NOAA/NCEI WMM.COF epoch 2025.0, valid 2025.0-2030.0.
-- Source: https://www.ncei.noaa.gov/sites/default/files/2024-12/WMM2025COF.zip
-- Everything below is scoped: the tables are named G/H/GD/HD and would
-- otherwise shadow an app's own globals of those names.
local declination
do
local EPOCH,NMAX=2025.0,12
local G={-29351.8,-1410.8,-2556.6,2951.1,1649.3,1361,-2404.1,1243.8,453.6,895,799.5,55.7,-281.1,12.1,
-233.2,368.9,187.2,-138.7,-142,20.9,64.4,63.8,76.9,-115.7,-40.9,14.9,-60.7,79.5,-77,-8.8,59.3,
15.8,2.5,-11.1,14.2,23.2,10.8,-17.5,2,-21.7,16.9,15,-16.8,.9,4.6,7.8,3,-.2,-2.5,-13.1,2.4,8.6,
-8.7,-12.9,-1.3,-6.4,.2,2,-1,-.6,-.9,1.5,.9,-2.7,-3.9,2.9,-1.5,-2.5,2.4,-.6,-.1,-.6,-.1,1.1,-1,
-.2,2.6,-2,-.2,.3,1.2,-1.3,.6,.6,.5,-.1,-.4,-.2,-1.3,-.7,}
local H={4545.4,-3133.6,-815.1,-56.6,237.5,-549.5,278.6,-133.9,212,-375.6,45.4,220.2,-122.9,43,106.1,
-18.4,16.8,48.8,-59.8,10.9,72.7,-48.9,-14.4,-1,23.4,-7.4,-25.1,-2.3,7.1,-12.6,11.4,-9.7,12.7,.7,
-5.2,3.9,-24.8,12.2,8.3,-3.3,-5.2,7.2,-.6,.8,10,3.3,0,2.4,5.3,-9.1,.4,-4.2,-3.8,.9,-9.1,0,2.9,
-.6,.2,.5,-.3,-1.2,-1.7,-2.9,-1.8,-2.3,-1.3,.7,1,-1.4,0,.6,-.1,.8,.1,-1,.1,.2,}
local GD={12,9.7,-11.6,-5.2,-8,-1.3,-4.2,.4,-15.6,-1.6,-2.4,-6,5.6,-7,.6,1.4,0,.6,2.2,.9,-.2,-.4,.9,1.2,
-.9,.3,.9,0,-.1,-.1,.5,-.1,-.8,-.8,.8,-.1,.2,0,.5,-.1,.3,.2,0,.2,0,-.1,.1,.3,-.3,0,.3,-.1,.1,
-.1,.1,0,.1,.1,0,-.3,0,-.1,-.1,0,0,0,0,0,0,0,-.1,0,0,-.1,-.1,-.1,-.1,0,0,0,0,0,0,.1,0,0,0,-.1,0,
-.1,}
local HD={-21.5,-27.7,-12.1,4,-.3,-4.1,-1.1,4.1,1.6,-4.4,-.5,2.2,.4,1.7,1.9,.3,-1.6,-.4,.9,.7,.9,.6,.5,
-.8,0,-1,.6,-.2,-.2,.5,-.4,.4,-.5,-.6,.3,.2,-.3,.3,-.3,.3,.2,-.1,-.2,.4,.1,0,0,-.2,.1,-.1,.1,0,
-.1,.2,0,0,.1,0,.1,0,0,.1,0,0,0,0,0,0,-.1,.1,0,0,0,0,0,0,0,-.1,}

local sqrt,sin,cos,asin,atan=math.sqrt,math.sin,math.cos,math.asin,math.atan
-- Flat (n,m) index = n*(n+1)/2 + m + 1, so every table stays in Lua's array
-- part (no hash lookups in the inner loop).
local OFF={} ; for n=0,NMAX do OFF[n]=n*(n+1)//2 end
local NP=OFF[NMAX]+NMAX+1
-- one-time recursion constants (position independent)
local K,C,E={},{},{}
for n=1,NMAX do
  local k=sqrt((2*n-1)/(2*n)); if n==1 then k=k*sqrt(2) end
  K[n]=k
  for m=0,n-1 do
    local d=sqrt(n*n-m*m); local i=OFF[n]+m+1
    C[i]=(2*n-1)/d
    E[i]=(n>=m+2) and sqrt((n-1)*(n-1)-m*m)/d or 0
  end
end
local P,DP={},{}
for i=1,NP do P[i]=0; DP[i]=0 end
local CM,SM={},{}

-- lat,lon in degrees; year is a decimal year (e.g. 2027.6, default = EPOCH).
-- Returns (1) declination in degrees, EAST positive: true = magnetic + decl,
--         (2) horizontal field intensity H in nT.
-- H gives the caller WMM's own error bar for free:
--     sigma_D = sqrt(0.26^2 + (5417/H)^2)   degrees, 1-sigma
-- and the official reliability zones: H < 2000 nT is the WMM "Blackout Zone"
-- (a magnetic compass is unusable), 2000 <= H < 6000 nT the "Caution Zone".
function declination(lat,lon,year)
  local dt=(year or EPOCH)-EPOCH
  if lat>89.99 then lat=89.99 elseif lat<-89.99 then lat=-89.99 end
  local phi,lam=lat*0.017453292,lon*0.017453292
  local sp,cp=sin(phi),cos(phi)
  -- WGS-84 geodetic -> geocentric spherical
  local rc=6378.137/sqrt(1-0.006694380*sp*sp)
  local p,z=rc*cp,rc*0.993305620*sp
  local r=sqrt(p*p+z*z)
  local pp=asin(z/r)                 -- geocentric latitude
  local ct,st=sin(pp),cos(pp)        -- cos(colatitude), sin(colatitude)
  P[1],DP[1]=1,0                     -- (0,0)
  for n=1,NMAX do
    local o,o1=OFF[n],OFF[n-1]
    local k=K[n]
    local dnn=o1+n                   -- (n-1,n-1)
    P[o+n+1]=k*st*P[dnn]
    DP[o+n+1]=k*(st*DP[dnn]+ct*P[dnn])
    for m=0,n-1 do
      local i,j=o+m+1,o1+m+1
      local c,e=C[i],E[i]
      local pv=c*ct*P[j]
      local dv=c*(ct*DP[j]-st*P[j])
      if e~=0 then local h=OFF[n-2]+m+1; pv=pv-e*P[h]; dv=dv-e*DP[h] end
      P[i],DP[i]=pv,dv
    end
  end
  for m=0,NMAX do CM[m+1]=cos(m*lam); SM[m+1]=sin(m*lam) end
  local ratio=6371.2/r
  local X,Y,Z=0,0,0
  local pw=ratio*ratio
  local gi,hi=0,0
  for n=1,NMAX do
    pw=pw*ratio
    local o=OFF[n]
    local np1=n+1
    for m=0,n do
      gi=gi+1
      local i,m1=o+m+1,m+1
      local gv=G[gi]+dt*GD[gi]
      local cm,sm=CM[m1],SM[m1]
      local a
      if m>0 then
        hi=hi+1
        local hv=H[hi]+dt*HD[hi]
        a=gv*cm+hv*sm
        Y=Y+pw*m*(gv*sm-hv*cm)*P[i]
      else
        a=gv
      end
      X=X+pw*a*DP[i]
      Z=Z-np1*pw*a*P[i]
    end
  end
  Y=Y/st
  local d=pp-phi
  local Xg=X*cos(d)-Z*sin(d)
  return atan(Y,Xg)*57.29577951, sqrt(Xg*Xg+Y*Y)
end
end -- WMM-GEN END

-- ---------------------------------------------------------------------------
-- persistence
local function load_prefs()
  local ox, oy = store.get("cal_ox"), store.get("cal_oy")
  local oz, r = store.get("cal_oz"), store.get("cal_r")
  -- oz is required for tilt compensation, so a calibration saved by the
  -- flat-only version (which had no third offset) is not good enough any more:
  -- take it only when the whole set is there.
  if type(ox) == "number" and type(oy) == "number" and type(oz) == "number" then
    cal = { ox = ox, oy = oy, oz = oz, r = (type(r) == "number" and r > 0) and r or nil }
    local bx, by, bz = store.get("acc_bx"), store.get("acc_by"), store.get("acc_bz")
    if type(bx) == "number" and type(by) == "number" and type(bz) == "number" then
      cal.abx, cal.aby, cal.abz = bx, by, bz
    end
  end
  -- Orientation settings are versioned: the heading formula changed once the
  -- M9's axes were measured, so values saved against the old one would push a
  -- correct default back off north. Anything older is discarded, not migrated.
  if store.get("orient_ver", 0) == 2 then
    align = tonumber(store.get("align", 0)) or 0
  else
    align = 0
    store.set("align", nil)
    store.set("orient", nil); store.set("flip", nil)   -- the even older pair
    store.set("orient_ver", 2)
  end
  local d = store.get("decl")
  if type(d) == "number" then
    decl, decl_src = d, "stale"
    decl_lat, decl_lon = store.get("decl_lat"), store.get("decl_lon")
  end
  align_pending_decl = store.get("align_pd") and true or false
  UNITS.alt = store.get("u_alt", 1) == 1
  UNITS.spd = store.get("u_spd", 1) == 1
end

local function save_align()
  store.set("align", math.floor(align + 0.5))
  store.set("mirror", nil)     -- the old hand-flipped handedness; measured now
  store.set("orient_ver", 2)
end

local function save_cal()
  if cal then
    store.set("cal_ox", cal.ox); store.set("cal_oy", cal.oy); store.set("cal_oz", cal.oz)
    store.set("cal_r", cal.r)
    store.set("acc_bx", cal.abx); store.set("acc_by", cal.aby); store.set("acc_bz", cal.abz)
  else
    store.set("cal_ox", nil); store.set("cal_oy", nil); store.set("cal_oz", nil)
    store.set("cal_r", nil)
    store.set("acc_bx", nil); store.set("acc_by", nil); store.set("acc_bz", nil)
  end
end

-- ---------------------------------------------------------------------------
-- calibration + heading
--
-- MEASURED FRAMES on the ThinkNode M9 (both by holding known attitudes and
-- logging the raw vectors; neither is documented by anyone, and the only other
-- firmware that touches these parts passes them through unverified):
--   magnetometer  +Y = top edge, +X = LEFT edge, +Z = into the screen
--   accelerometer +X = top edge, +Y = RIGHT edge, +Z = into the screen
-- The accelerometer is therefore already in the aerospace body frame (forward,
-- right, down); the magnetometer becomes (fwd, right, down) = (my, -mx, mz).
--
-- WHY TILT MATTERS: at this latitude Earth's field dips about 60 degrees below
-- horizontal, so the vertical component is ~1.6x the horizontal one. A heading
-- taken from two axes assumes the device is level; tip it and some of that
-- large vertical field leaks into the horizontal pair, which is worth roughly
-- 1.5 degrees of heading per degree of tilt. That, not the sensor, is why a
-- hand-held reading wanders.
local function to_body(m)
  return m.y - (cal and cal.oy or 0),        -- forward (top edge)
       -(m.x - (cal and cal.ox or 0)),       -- right
         m.z - (cal and cal.oz or 0)         -- down (into the screen)
end

-- Hard-iron calibration: fit the sphere the samples lie on. Rotating the
-- device sweeps that sphere, whose centre is the offset and whose radius is
-- the true field.
--
-- ROTATE IT IN PLACE. Carrying it around the room while turning it does not
-- just rotate the device, it also TRANSLATES it through the field of the desk,
-- the laptop and anything else ferrous — and that corrupts the fit. Measured
-- here: a centre that moved 0.15 G between hand-tumbled sessions, against a
-- horizontal signal of only 0.26 G. The accelerometer now checks that the
-- device was actually turned every way, which is the part a user cannot see.
local function calib_new()
  return { n = 0, o = nil, t0 = sys.millis(),
           sx = 0, sy = 0, sz = 0, sxx = 0, syy = 0, szz = 0,
           sxy = 0, sxz = 0, syz = 0, sxs = 0, sys_ = 0, szs = 0, ss = 0,
           gmn = { 9, 9, 9 }, gmx = { -9, -9, -9 },
           -- the same sweep traces a 1 g sphere for the accelerometer, so its
           -- own zero-g offset falls out of the same fit for free
           an = 0, ao = nil, ax = 0, ay = 0, az = 0, axx = 0, ayy = 0, azz = 0,
           axy = 0, axz = 0, ayz = 0, axs = 0, ays = 0, azs = 0, ass = 0 }
end

local function calib_add(m, a)
  local c = calib
  if not c.o then c.o = { m.x, m.y, m.z } end
  local x, y, z = m.x - c.o[1], m.y - c.o[2], m.z - c.o[3]
  local s = x * x + y * y + z * z
  c.n = c.n + 1
  c.sx = c.sx + x; c.sy = c.sy + y; c.sz = c.sz + z
  c.sxx = c.sxx + x * x; c.syy = c.syy + y * y; c.szz = c.szz + z * z
  c.sxy = c.sxy + x * y; c.sxz = c.sxz + x * z; c.syz = c.syz + y * z
  c.sxs = c.sxs + x * s; c.sys_ = c.sys_ + y * s; c.szs = c.szs + z * s
  c.ss = c.ss + s
  -- coverage, measured by where gravity pointed rather than by where the field
  -- went: it is the honest test of "was this thing actually turned over?"
  if a then
    local g = { a.x, a.y, a.z }
    for i = 1, 3 do
      if g[i] < c.gmn[i] then c.gmn[i] = g[i] end
      if g[i] > c.gmx[i] then c.gmx[i] = g[i] end
    end
    if not c.ao then c.ao = { a.x, a.y, a.z } end
    local ux, uy, uz = a.x - c.ao[1], a.y - c.ao[2], a.z - c.ao[3]
    local us = ux * ux + uy * uy + uz * uz
    c.an = c.an + 1
    c.ax = c.ax + ux; c.ay = c.ay + uy; c.az = c.az + uz
    c.axx = c.axx + ux * ux; c.ayy = c.ayy + uy * uy; c.azz = c.azz + uz * uz
    c.axy = c.axy + ux * uy; c.axz = c.axz + ux * uz; c.ayz = c.ayz + uy * uz
    c.axs = c.axs + ux * us; c.ays = c.ays + uy * us; c.azs = c.azs + uz * us
    c.ass = c.ass + us
  end
end

local function solve4(M, v)
  for col = 1, 4 do
    local piv, best = col, math.abs(M[col][col])
    for r = col + 1, 4 do
      local a = math.abs(M[r][col])
      if a > best then piv, best = r, a end
    end
    if best < 1e-9 then return nil end
    if piv ~= col then M[col], M[piv] = M[piv], M[col]; v[col], v[piv] = v[piv], v[col] end
    local d = M[col][col]
    for r = col + 1, 4 do
      local f = M[r][col] / d
      if f ~= 0 then
        for k = col, 4 do M[r][k] = M[r][k] - f * M[col][k] end
        v[r] = v[r] - f * v[col]
      end
    end
  end
  local out = {}
  for r = 4, 1, -1 do
    local acc = v[r]
    for k = r + 1, 4 do acc = acc - M[r][k] * out[k] end
    out[r] = acc / M[r][r]
  end
  return out
end

-- Returns { ox, oy, oz, r } or nil plus the reason. Every refusal keeps the
-- previous calibration: silently saving a bad fit is how north broke before.
local function calib_solve()
  local c = calib
  if c.n < 80 then return nil, "too few readings" end
  -- Was it really turned every way? Gravity should have pointed along both
  -- ends of each axis at some point. This is what a flat spin fails.
  if has_accel and c.gmx[1] > -9 then
    local worst, axis = 9, 1
    for i = 1, 3 do
      local span = c.gmx[i] - c.gmn[i]
      if span < worst then worst, axis = span, i end
    end
    if worst < 0.9 then
      return nil, ({ "turn it nose over tail", "turn it side over side",
                     "turn it face up and down" })[axis]
    end
  end
  local M = {
    { 4 * c.sxx, 4 * c.sxy, 4 * c.sxz, 2 * c.sx },
    { 4 * c.sxy, 4 * c.syy, 4 * c.syz, 2 * c.sy },
    { 4 * c.sxz, 4 * c.syz, 4 * c.szz, 2 * c.sz },
    { 2 * c.sx,  2 * c.sy,  2 * c.sz,  c.n },
  }
  local v = { 2 * c.sxs, 2 * c.sys_, 2 * c.szs, c.ss }
  local sol = solve4(M, v)
  if not sol then return nil, "turn it in more directions" end
  local cx, cy, cz, k = sol[1], sol[2], sol[3], sol[4]
  local r2 = k + cx * cx + cy * cy + cz * cz
  if r2 <= 0 then return nil, "turn it in more directions" end
  local r = math.sqrt(r2)
  -- Earth's TOTAL field is 0.25..0.65 G everywhere on the planet.
  if r < 0.22 or r > 0.70 then return nil, string.format("field reads %.2f G", r) end
  -- Roundness, in closed form from the sums. A fit distorted by carrying the
  -- device past nearby metal has a normal-looking radius and a bad residual.
  local mean_r2 = (c.ss - 2 * (cx * c.sx + cy * c.sy + cz * c.sz)) / c.n
                  + cx * cx + cy * cy + cz * cz
  local resid = math.sqrt(math.max(mean_r2 - r * r, 0))
  if resid > 0.15 * r then
    return nil, string.format("too distorted (%.0f%%) - turn it ON THE SPOT", 100 * resid / r)
  end
  local out = { ox = c.o[1] + cx, oy = c.o[2] + cy, oz = c.o[3] + cz, r = r }
  -- Accelerometer zero-g offset, from the same sweep. A MEMS part is commonly
  -- tens of milli-g out of true, and this one reads ~0.08 g on Y lying flat --
  -- about 5 degrees of tilt that is not there, which the compensation would
  -- then apply to the heading. Only taken when the sphere it fits is close to
  -- the 1 g it must be.
  if c.an >= 80 then
    local AM = {
      { 4 * c.axx, 4 * c.axy, 4 * c.axz, 2 * c.ax },
      { 4 * c.axy, 4 * c.ayy, 4 * c.ayz, 2 * c.ay },
      { 4 * c.axz, 4 * c.ayz, 4 * c.azz, 2 * c.az },
      { 2 * c.ax,  2 * c.ay,  2 * c.az,  c.an },
    }
    local av = { 2 * c.axs, 2 * c.ays, 2 * c.azs, c.ass }
    local asol = solve4(AM, av)
    if asol then
      local acx, acy, acz, ak = asol[1], asol[2], asol[3], asol[4]
      local ar2 = ak + acx * acx + acy * acy + acz * acz
      if ar2 > 0 then
        local ar = math.sqrt(ar2)
        if ar > 0.85 and ar < 1.15 then
          out.abx = c.ao[1] + acx
          out.aby = c.ao[2] + acy
          out.abz = c.ao[3] + acz
          out.ar = ar
        end
      end
    end
  end
  return out, r
end

-- Heading. With the accelerometer, the magnetic vector is rotated back into
-- the horizontal plane before the angle is taken, so tipping the device no
-- longer swings the reading (NXP AN4248 / ST AN3192, in the body frame above).
-- Without one, it falls back to the flat-earth two-axis form, which is only
-- honest while the device is held level.
local function mag_heading(m)
  local bx, by, bz = to_body(m)
  if acc then
    -- gravity's DIRECTION in the body frame: the part reads specific force, so
    -- the skyward axis reads +1 and down is the negative of that
    -- gravity's direction, with the sensor's own zero-g offset removed
    local gx = -(acc.x - (cal and cal.abx or 0))
    local gy = -(acc.y - (cal and cal.aby or 0))
    local gz = -(acc.z - (cal and cal.abz or 0))
    local gn = math.sqrt(gx * gx + gy * gy + gz * gz)
    if gn > 0.5 then
      gx, gy, gz = gx / gn, gy / gn, gz / gn
      local pitch = math.atan(-gx, math.sqrt(gy * gy + gz * gz))
      local roll  = math.atan(gy, gz)
      local sp, cp = math.sin(pitch), math.cos(pitch)
      local sr, cr = math.sin(roll), math.cos(roll)
      local xh = bx * cp + by * sp * sr + bz * sp * cr
      local yh = by * cr - bz * sr
      tilt_deg = math.deg(math.acos(math.max(-1, math.min(1, gz))))
      return norm360(math.deg(math.atan(-yh, xh)) + align + decl)
    end
  end
  tilt_deg = nil
  return norm360(math.deg(math.atan(bx, by)) + align + decl)
end

local function smooth_heading(h)
  local r = math.rad(h)
  local sx, sy = math.sin(r), math.cos(r)
  if hv_x == 0 and hv_y == 0 then
    hv_x, hv_y = sx, sy
  else
    hv_x = hv_x + (sx - hv_x) * SMOOTH
    hv_y = hv_y + (sy - hv_y) * SMOOTH
  end
  return norm360(math.deg(math.atan(hv_x, hv_y)))
end

local function update_heading(now)
  -- Gravity first: which way is down decides how much of the vertical field is
  -- leaking into the horizontal pair, and the field dips ~60 degrees here.
  if has_accel then acc = sys.accel() or acc end
  local m = has_compass and sys.compass() or nil
  if m then
    last_mag_ms = now
    diag_m = m
    mag_sat = m.ovfl == true
    if not mag_sat then
      if calib then calib_add(m, acc) end
      -- total field magnitude: with tilt compensation the heading no longer
      -- cares about attitude, so the useful health check is whether the whole
      -- vector still has the length the calibration found. A big departure
      -- means a magnet nearby or a stale calibration, not a tipped device.
      local bx, by, bz = to_body(m)
      mag_norm = math.sqrt(bx * bx + by * by + bz * bz)
      heading, src = smooth_heading(mag_heading(m)), "mag"
    end
    return
  end
  -- No magnetometer (or nothing fresh for a while): GPS course while moving.
  if has_compass and (now - last_mag_ms) < 1500 and heading then return end
  local g = caps.sdk_ext and sys.gps() or nil
  if g and g.course then
    heading, src = smooth_heading(g.course), "gps"
  else
    heading, src = nil, "none"
    hv_x, hv_y = 0, 0
  end
end

-- ---------------------------------------------------------------------------
-- targets: contacts that have shared a position
local function refresh_contacts()
  local list = {}
  for _, c in ipairs(mesh.contacts()) do
    if (c.lat ~= 0 or c.lon ~= 0) and c.name and c.name ~= "" then
      list[#list + 1] = c
    end
  end
  table.sort(list, function(a, b) return a.name < b.name end)
  -- keep the same target selected across a refresh when it is still there
  local cur = targets[target_i]
  targets = list
  if cur then
    target_i = 0
    for i, c in ipairs(list) do
      if c.name == cur.name then target_i = i break end
    end
  elseif target_i > #targets then
    target_i = 0
  end
end

local function cycle_target(step)
  if #targets == 0 then
    target_i = 0
    sys.toast("No contact has shared a position yet", 1500)
    return
  end
  target_i = (target_i + step) % (#targets + 1)
  if target_i < 0 then target_i = target_i + #targets + 1 end
end

-- great-circle distance (m) and initial bearing (deg) from (lat1,lon1) to (lat2,lon2)
local function geo(lat1, lon1, lat2, lon2)
  local p1, p2 = math.rad(lat1), math.rad(lat2)
  local dl = math.rad(lon2 - lon1)
  local a = math.sin((p2 - p1) / 2) ^ 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ^ 2
  local dist = 2 * 6371000 * math.atan(math.sqrt(a), math.sqrt(1 - a))
  local y = math.sin(dl) * math.cos(p2)
  local x = math.cos(p1) * math.sin(p2) - math.sin(p1) * math.cos(p2) * math.cos(dl)
  return dist, norm360(math.deg(math.atan(y, x)))
end

-- range to a target follows the ALTITUDE row's units (one "how far" setting)
-- Recompute only when the fix has actually moved: the model costs ~8k VM
-- instructions, which is 8% of one tick's budget, and declination changes by
-- about a degree per 100 km.
local function update_decl(lat, lon)
  if not lat or (lat == 0 and lon == 0) then return end
  if decl_src == "model" and decl_lat then
    local dist = geo(decl_lat, decl_lon, lat, lon)
    if dist < DECL_REFRESH_KM * 1000 then return end
  end
  local yr = BUILD_YEAR
  local dt = sys.datetime and sys.datetime()
  if dt and dt.year and dt.year > 2020 then yr = dt.year + (dt.month - 0.5) / 12 end
  local d, hfield = declination(lat, lon, yr)
  if d then
    if align_pending_decl then
      -- north was set against TRUE north while the model was blind, so the
      -- offset the user made is carrying this declination already
      align = align - d
      align_pending_decl = false
      store.set("align_pd", nil); store.set("align", math.floor(align + 0.5))
    end
    decl, decl_src = d, "model"
    decl_lat, decl_lon = lat, lon
    -- keep it across a reboot: a stale value from 50 km away is worth about a
    -- degree, where assuming zero is worth the whole declination
    store.set("decl", d); store.set("decl_lat", lat); store.set("decl_lon", lon)
    -- WMM's own error model: below 6000 nT of horizontal field a magnetic
    -- compass is not to be trusted, and below 2000 nT it is useless
    weak_field = hfield and hfield < 6000
  end
end

local function fmt_dist(m)
  if UNITS.alt then
    local ft = m * 3.28084
    if ft < 1000 then return string.format("%d ft", math.floor(ft + 0.5)) end
    local mi = m / 1609.344
    if mi < 10 then return string.format("%.2f mi", mi) end
    return string.format("%.1f mi", mi)
  end
  if m < 1000 then return string.format("%d m", math.floor(m + 0.5)) end
  if m < 10000 then return string.format("%.2f km", m / 1000) end
  return string.format("%.1f km", m / 1000)
end

local function fmt_alt(m)
  if UNITS.alt then return string.format("%d ft", math.floor(m * 3.28084 + 0.5)) end
  return string.format("%d m", math.floor(m + 0.5))
end

local function fmt_speed(kmh)
  if UNITS.spd then return string.format("%.1f mph", kmh * 0.621371) end
  return string.format("%.1f km/h", kmh)
end

-- ---------------------------------------------------------------------------
-- drawing
local function set_text(name, text, color)
  local l = L[name]
  if not l then return end
  if last_text[name] ~= text then l:set(text); last_text[name] = text end
  if color and last_text[name .. "#"] ~= color then l:color(color); last_text[name .. "#"] = color end
end

local function pt(deg, r)
  local a = math.rad(deg)
  return math.floor(CX + math.sin(a) * r + 0.5), math.floor(CY - math.cos(a) * r + 0.5)
end

-- Canvas text is left-anchored, so centring means knowing the width. Newer
-- firmware can MEASURE it (wada.ui.text_w, gated on caps().measure); older
-- firmware cannot, and then this estimates ~0.55 x line height per CHARACTER —
-- counting characters, not bytes, since "°" is two bytes in UTF-8 and counting
-- those once pushed the centred heading half a glyph off.
local function text_w(str, lh, size)
  if ui.text_w then return ui.text_w(str, size or 12) end
  local n = 0
  for _ in str:gmatch("[%z\1-\127\194-\244]") do n = n + 1 end
  return math.floor(n * lh * 0.55)
end

local function draw_dial(tgt_bearing)
  local h = heading or 0
  local live = heading ~= nil
  -- the page is the firmware's black; the dial sits on its own raised disc so
  -- it reads as an instrument rather than as drawing on the page
  cv:fill(C.bg)
  cv:circle(CX, CY, R, PANEL, true)
  -- rings
  cv:circle(CX, CY, R, live and RING or RING2, false, 2)
  cv:circle(CX, CY, R - 14, RING2, false, 1)

  -- graduations: every 10° a minor tick, every 30° a major one, the four
  -- cardinals as letters. The whole card rotates by -heading so the current
  -- heading sits under the lubber mark.
  for deg = 0, 350, 10 do
    local a = deg - h
    local major = deg % 30 == 0
    local cardinalp = deg % 90 == 0
    local len = cardinalp and 9 or major and 6 or 3
    local x1, y1 = pt(a, R - 3)
    local x2, y2 = pt(a, R - 3 - len)
    local col = (deg == 0) and C.bad or (major and (live and C.text or C.sub) or RING)
    cv:line(x1, y1, x2, y2, col, cardinalp and 2 or 1)
    if cardinalp then
      local lx, ly = pt(a, R - 14 - math.floor(TH12 * 0.6))
      local letter = CARD[math.floor(deg / 90) * 4 + 1]
      cv:text(lx - math.floor(TH12 * 0.3), ly - math.floor(TH12 / 2), letter, col, 12)
    end
  end

  -- lubber mark: a small solid triangle pointing in from the top
  for i = 0, 6 do
    cv:line(CX - 6 + i, i, CX + 6 - i, i, C.accent, 1)
  end
  cv:line(CX, 0, CX, 9, C.accent, 2)

  -- target: dot on the inner ring plus a thin spoke, relative to the card
  if tgt_bearing then
    local mx, my = pt(tgt_bearing - h, R - 14)
    local sx, sy = pt(tgt_bearing - h, R - 26)
    cv:line(CX, CY, sx, sy, RING, 1)
    cv:circle(mx, my, 4, AMBER, true)
  end

  -- centre: heading number with the degree sign hanging off its right edge —
  -- the DIGITS are what should sit centred, so the number does not appear to
  -- shift when the reading crosses 100 or 200
  if live then
    local digits = string.format("%03d", math.floor(h + 0.5) % 360)
    local dw = text_w(digits, TH16, 16)
    local dx = CX - math.floor(dw / 2)
    cv:text(dx, CY - TH16 + 1, digits, C.text, 16)
    cv:text(dx + dw, CY - TH16 + 1, "\194\176", C.sub, 16)
    -- T or M rides with the degree sign, outside the centred digits: the point
    -- of the number is useless without knowing what it is measured from
    local ref = (src == "gps") and "T" or href()
    cv:text(dx + dw + text_w("\194\176", TH16, 16), CY - TH16 + 1, ref,
            ref == "M" and AMBER or C.sub, 12)
    local cd = cardinal(h)
    cv:text(CX - math.floor(text_w(cd, TH12, 12) / 2), CY + 3, cd, src == "mag" and C.accent or AMBER, 12)
  else
    cv:text(CX - math.floor(text_w("--", TH16, 16) / 2), CY - math.floor(TH16 / 2), "--", C.sub, 16)
  end
end

-- satellite meter: ten cells, filled in the status colour up to the count
local function draw_sats(n, col)
  if not sats_cv then return end
  sats_cv:fill(C.bg)
  local cw = math.floor((SATS_W - 9) / 10)
  for i = 0, 9 do
    local x = i * (cw + 1)
    if i < n then sats_cv:rect(x, 0, cw, SATS_H, col, true)
    else sats_cv:rect(x, 0, cw, SATS_H, PANEL, true) end
  end
end

local function refresh(now)
  update_heading(now)

  -- GPS readout
  local g = caps.sdk_ext and sys.gps() or nil
  local me_lat, me_lon
  local sats_n, sats_col = 0, RING
  if g then
    me_lat, me_lon = g.lat, g.lon
    update_decl(g.lat, g.lon)
    sats_n = g.sats or 0
    sats_col = sats_n >= 6 and C.good or AMBER
    set_text("fix", string.format("%d sats", sats_n), C.text)
    set_text("lat", string.format("%.5f", g.lat), C.text)
    set_text("lon", string.format("%.5f", g.lon), C.text)
    set_text("alt", fmt_alt(g.alt_m or 0), C.text)
    if g.speed_kmh then
      local s = fmt_speed(g.speed_kmh)
      if g.course then s = s .. string.format("  %03d\194\176T", math.floor(g.course + 0.5) % 360) end
      set_text("spd", s, C.text)
    else
      set_text("spd", "--", C.sub)
    end
  else
    local me = mesh.self()
    if me and (me.lat ~= 0 or me.lon ~= 0) then
      me_lat, me_lon = me.lat, me.lon
      update_decl(me.lat, me.lon)
      set_text("lat", string.format("%.5f", me.lat), C.sub)
      set_text("lon", string.format("%.5f", me.lon), C.sub)
    else
      set_text("lat", "--", C.sub)
      set_text("lon", "--", C.sub)
    end
    set_text("fix", caps.sdk_ext and "no fix" or "no GPS", C.sub)
    set_text("alt", "--", C.sub)
    set_text("spd", "--", C.sub)
  end
  local sk = sats_n .. "|" .. sats_col
  if sk ~= last_sats_key then draw_sats(sats_n, sats_col); last_sats_key = sk end

  -- heading-source line over the dial (<= 18 glyphs: it spans the dial width)
  if calib then
    local left = CAL_SECS - math.floor((now - calib.t0) / 1000)
    -- coverage, not just a countdown: the span of each axis reaches 2r over a
    -- full turn, so this reads ~100% exactly when the sweep is complete
    -- coverage measured by how far gravity has swung on its worst axis: 2.0
    -- means the device has been fully over in that direction
    local worst = 9
    for i = 1, 3 do
      local span = calib.gmx[i] - calib.gmn[i]
      if span < worst then worst = span end
    end
    local cov = (worst < 8) and math.floor(worst / 1.6 * 100) or 0
    set_text("src", string.format("Turn it all ways  %ds  %d%%", math.max(left, 0), math.min(cov, 99)), AMBER)
  elseif mag_sat then
    set_text("src", "Field saturated", C.bad)
  elseif src == "mag" then
    -- Tilt is the accuracy ceiling for a 2-axis compass: the field dips ~60
    -- degrees at mid latitudes, so tipping the device swaps vertical field
    -- into the horizontal pair and swings the heading. The app cannot correct
    -- that without the IMU, but it CAN notice: held level the horizontal
    -- magnitude equals the calibrated radius, and tilting shrinks or inflates
    -- it. So say when the reading should not be trusted.
    local field_off = cal and cal.r and mag_norm and math.abs(mag_norm - cal.r) > 0.25 * cal.r
    if not cal then
      set_text("src", "Calibrate: press C", AMBER)
    elseif field_off then
      set_text("src", string.format("Field off (%.2f G)", mag_norm or 0), C.bad)
    elseif tilt_deg and tilt_deg > 55 then
      -- past ~55 degrees the horizontal projection is small enough that the
      -- correction stops being trustworthy; say so rather than lie
      set_text("src", "Too steep to read", AMBER)
    elseif tilt_deg then
      -- Which north the dial is showing, in the room the panel actually has.
      -- Amber MAG is a warning: bearings to contacts are TRUE, so while the
      -- declination is unknown every waypoint is off by it.
      if weak_field then
        -- WMM ships a blackout/caution model with its coefficients: under
        -- 6000 nT of horizontal field the direction is not worth trusting,
        -- whatever the calibration says
        set_text("src", "Weak field: heading unreliable", AMBER)
      else
        set_text("src", string.format("%s  tilt %.0f\194\176",
                                      decl_src == "none" and "MAG north" or "TRUE north",
                                      tilt_deg), decl_src == "none" and AMBER or C.good)
      end
    else
      set_text("src", "Hold it level", AMBER)
    end
  elseif src == "gps" then
    set_text("src", "GPS course", AMBER)
  elseif has_compass then
    set_text("src", "Sensor: no data", C.bad)
  else
    set_text("src", "GPS when moving", C.sub)
  end

  -- D: what the app is actually computing, so a wrong heading can be diagnosed
  -- from the screen instead of guessed at. Takes over the target rows.
  if diag then
    local m = diag_m
    -- Rows are short on purpose: the value column is ~22 characters at this
    -- font, and a longer line wraps onto the row below and overprints it.
    -- Diagnostics also take the whole panel width (see diag_layout), which is
    -- the key column back.
    set_text("tgt", cal and string.format("cal %.2f %.2f %.2f", cal.ox, cal.oy, cal.oz or 0)
                        or "cal NONE - press C", cal and C.text or C.bad)
    set_text("tgt2", m and string.format("mag %.2f %.2f %.2f", m.x, m.y, m.z) or "mag --", C.text)
    set_text("rel", acc and string.format("acc %.2f %.2f %.2f", acc.x, acc.y, acc.z) or "acc --", C.text)
    local bits = string.format("|B|%.2f", mag_norm or 0)
    if m then
      -- Vertical field. North of the equator, held flat, this MUST be positive
      -- if the magnetometer's +Z points into the screen -- which is what the
      -- tilt correction assumes. Negative here means that sign is inverted,
      -- and pitching the device would still swing the heading.
      local _, _, bz = to_body(m)
      bits = bits .. string.format(" d%+.2f", bz)
    end
    if cal and cal.r then bits = bits .. string.format(" r%.2f", cal.r) end
    if tilt_deg then bits = bits .. string.format(" t%d", math.floor(tilt_deg + 0.5)) end
    if align ~= 0 then bits = bits .. string.format(" a%d", math.floor(align + 0.5)) end
    set_text("seen", bits, AMBER)
    -- The declination gets the full-width bottom row: it is what separates the
    -- dial's north from every bearing on the panel, and seeing the position it
    -- was computed at is how a wrong marker gets traced to a bad fix rather
    -- than a bad model.
    if decl_src == "none" then
      set_text("hint", "no declination yet - bearings are TRUE, dial is MAGNETIC", AMBER)
    else
      set_text("hint", string.format("decl %+.1f\194\176 %s  @ %.2f, %.2f", decl,
                                     decl_src == "model" and "WMM2025" or "stored",
                                     decl_lat or 0, decl_lon or 0), C.sub)
    end
    draw_dial(nil)
    last_dial_key = nil          -- keep the dial live while diagnosing
    return
  end

  -- target: name, range + bearing, which way to turn, when it was last heard
  local tgt_bearing
  local t = targets[target_i]
  if t then
    set_text("tgt", t.name:sub(1, name_max), AMBER)
    if me_lat then
      local dist, brg = geo(me_lat, me_lon, t.lat, t.lon)
      tgt_bearing = brg
      set_text("tgt2", string.format("%s  %03d\194\176T %s", fmt_dist(dist), math.floor(brg + 0.5) % 360,
                                     cardinal(brg)), C.text)
      if heading then
        local rel = norm360(brg - heading)
        local turn = rel <= 180 and rel or 360 - rel
        local s
        if turn <= 6 then s = "ahead"
        elseif turn >= 174 then s = "behind"
        else s = string.format("%d\194\176 %s", math.floor(turn + 0.5), rel <= 180 and "right" or "left") end
        set_text("rel", s, turn <= 6 and C.good or C.text)
      else
        set_text("rel", "no heading", C.sub)
      end
    else
      set_text("tgt2", compact and "no own position" or "own position unknown", C.sub)
      set_text("rel", "", C.sub)
    end
    local ago = t.ago_s or 0
    if ago <= 0 then set_text("seen", "", C.sub)
    elseif ago < 60 then set_text("seen", "heard just now", C.sub)
    elseif ago < 3600 then set_text("seen", string.format("heard %dm ago", math.floor(ago / 60)), C.sub)
    elseif ago < 86400 then set_text("seen", string.format("heard %dh ago", math.floor(ago / 3600)), C.sub)
    else set_text("seen", string.format("heard %dd ago", math.floor(ago / 86400)), C.sub) end
  else
    set_text("tgt", #targets > 0 and string.format("none  (%d)  <>", #targets) or "none", C.sub)
    set_text("tgt2", "", C.sub)
    set_text("rel", "", C.sub)
    set_text("seen", "", C.sub)
  end

  -- dial: redraw only when what it shows changed. The T/M reference belongs in
  -- the key too: when the first fix lands the heading itself often does not
  -- move, and without this the dial would keep claiming MAGNETIC.
  local key = string.format("%d|%s|%s|%s|%s|%s", heading and math.floor(heading + 0.5) or -1, src,
                            tgt_bearing and math.floor(tgt_bearing + 0.5) or "-", tostring(cal ~= nil),
                            tostring(mag_sat), href())
  if key ~= last_dial_key then
    draw_dial(tgt_bearing)
    last_dial_key = key
  end
end

-- ---------------------------------------------------------------------------
-- actions
local function toggle_cal()
  if not has_compass then sys.toast("No magnetometer on this board", 1500) return end
  if calib then
    local fit, r = calib_solve()
    if fit then
      cal = fit
      save_cal()
      sys.toast(string.format("Calibrated  field %.2f G", r), 2200)
    else
      sys.toast("Not calibrated: " .. tostring(r), 2800)
    end
    calib = nil
    if sys.keep_awake then sys.keep_awake(false) end
  else
    -- Hold the screen and the app's tick for the sweep: the default screen
    -- timeout is the same 20 s as this calibration, and a blanked screen used
    -- to stop the sampling dead half way through and save the partial fit.
    if sys.keep_awake then sys.keep_awake(true) end
    calib = calib_new()
    sys.toast("Turn it every way ON ONE SPOT - do not carry it around", 3000)
  end
  hv_x, hv_y = 0, 0
end

-- One press does the whole orientation job: hold the device flat with the top
-- edge at north and press A.
--   * handedness — whether the heading runs clockwise or anticlockwise depends
--     on which way the sensor's Z axis faces, and that shows up in the sign of
--     the vertical field: Earth's field dips DOWN north of the magnetic
--     equator and UP south of it. With a position (a fix, or the last one the
--     node knows) the sign of `mag_z` therefore says which way Z points.
--   * offset — whatever angle the sensor reports while pointing north becomes
--     the zero.
-- the selected row's key is drawn in the accent colour so it is obvious which
-- one OK will switch
local function paint_selection()
  for _, n in ipairs({ "alt", "spd" }) do
    local kl = L["k_" .. n]
    if kl then kl:color(n == sel and C.accent or C.sub) end
  end
end

local function move_sel(dir)
  sel = (sel == "alt") and "spd" or "alt"
  paint_selection()
end

local function toggle_units(which)
  which = which or sel
  UNITS[which] = not UNITS[which]
  store.set(which == "alt" and "u_alt" or "u_spd", UNITS[which] and 1 or 0)
  sel = which
  paint_selection()
  last_text[which] = nil                      -- force the row to re-render now
  if which == "alt" then
    sys.toast(UNITS.alt and "Altitude and range: feet / miles" or "Altitude and range: metres / km", 1400)
  else
    sys.toast(UNITS.spd and "Speed: mph" or "Speed: km/h", 1200)
  end
end

-- Note on what "north" means here: this is a MAGNETIC compass. Magnetic north
-- and true north differ by the local declination -- about 13 degrees in
-- California, over 15 in parts of the US -- so a dial that disagrees with a
-- phone (which shows true north) by roughly that much is not broken, it is
-- measuring a different north.
--
-- A pressed while pointing at TRUE north folds the local declination into
-- `align` and makes the two agree. That is now its only job: both sensors'
-- axis mappings are measured, so nothing here has to guess at handedness any
-- more (the first version tried to infer it from the dip and had the test
-- inverted, which is what once made the dial turn the wrong way).
local function align_north()
  if not has_compass then sys.toast("No magnetometer on this board", 1500) return end
  if not cal then sys.toast("Calibrate first: press C", 2000) return end
  local m = sys.compass()
  if not m or m.ovfl then sys.toast("No usable reading", 1500) return end
  align = 0
  align = -mag_heading(m)          -- whatever it reads now becomes 000
  -- mag_heading() already added the declination, so `align` is the residual on
  -- top of the model -- mounting error, a stray magnet in the case -- and it
  -- stays correct as the model updates. But with no fix yet the declination in
  -- there was 0, so `align` quietly swallowed the real one; counting it again
  -- when the fix arrives would double it. Remember to take it back out.
  align_pending_decl = (decl_src == "none")
  store.set("align_pd", align_pending_decl and 1 or nil)   -- the store takes numbers, not booleans
  save_align()
  hv_x, hv_y = 0, 0
  sys.toast(align_pending_decl and "North set (magnetic until a fix)" or "North set here", 1600)
end

-- ---------------------------------------------------------------------------
-- layout
local function label(name, x, y, size, color, width)
  local l = ui.label("", x, y, size or 12, color or C.text)
  if width then l:width(width) end
  L[name] = l
end

function app.on_open(w, h)
  W, H = w, h
  caps = sys.caps()
  has_compass = caps.compass == true
  has_accel = caps.accel == true
  load_prefs()
  landscape = w >= h * 1.3
  TH12, TH14, TH16 = ui.text_h(12), ui.text_h(14), ui.text_h(16)

  -- panel rows: { name, key } — a key/value pair per row, keys in the muted
  -- colour at x0, values in the value column. Key-less rows after TGT are the
  -- target's detail lines and sit in the value column. The magnetometer
  -- status line is not a row: it is centred over the dial. The key hint is
  -- not a row either: it sits on the bottom edge of the view.
  local rows = {
    { "fix", "FIX" }, { "lat", "LAT" }, { "lon", "LON" }, { "alt", "ALT" }, { "spd", "SPD" },
    { "tgt", "TGT" }, { "tgt2", false }, { "rel", false }, { "seen", false },
  }
  local gap_before = { tgt = 4 }   -- group spacing
  local x0, y0, colw, line
  local hint_y

  if landscape then
    -- stats column on the left, dial on the right under its status line, the
    -- hint centred along the bottom edge
    hint_y = h - TH12 - 3
    dial_y = TH12 + 4
    D = math.min(hint_y - 4 - dial_y, w - 176)
    cv = ui.canvas(D, D)
    dial_x = w - D - 2
    x0, y0 = 4, 4
    colw = dial_x - x0 - 8
    line = TH12 + 2
    local function total()
      local t = 0
      for _, r in ipairs(rows) do t = t + line + (gap_before[r[1]] or 0) end
      return t
    end
    local room = hint_y - 4 - y0                 -- rows must clear the hint line
    while #rows > 6 and total() > room do table.remove(rows) end
    if total() > room then line = math.floor(room / #rows) end
  else
    D = math.min(w - 8, 200)
    cv = ui.canvas(D, D)
    dial_x, dial_y = math.floor((w - D) / 2), TH12 + 4
    x0, y0 = 6, dial_y + D + 6
    colw = w - 12
    line = TH12 + 2
    if caps.touch then ui.scroll(true) end
  end
  cv:pos(dial_x, dial_y)
  R = math.floor(D / 2) - 2
  CX, CY = math.floor(D / 2), math.floor(D / 2)
  -- magnetometer / heading-source status, centred over the dial
  L.src = ui.label("", dial_x, 2, 12, C.sub)
  L.src:width(D, "center")

  local keyw = math.floor(TH12 * 2.1)           -- "LON" at 12 px is ~24 px; leave a gap
  local valx = x0 + keyw + 6
  local valw = colw - keyw - 6
  col_x0, col_x1 = x0, x0 + colw
  val_x, val_w = valx, valw
  name_max = math.max(8, math.min(18, math.floor((valw - 4) / (TH12 * 0.62))))
  -- Montserrat runs ~0.48 x line height per glyph: the status line over the
  -- dial is the longest (22 glyphs, ~10.6 x); go compact when the dial is
  -- narrower than that
  compact = D < TH12 * 10.7

  local y = y0
  for _, r in ipairs(rows) do
    local name, key = r[1], r[2]
    y = y + (gap_before[name] or 0)
    if key then
      local kl = ui.label(key, x0, y, 12, C.sub)
      if name == "alt" or name == "spd" or name == "tgt" then L["k_" .. name] = kl end
    end
    label(name, valx, y, 12, C.text, valw)   -- key-less rows line up with the values
    row_y[name] = y
    if name == "alt" or name == "spd" then row_hit[name] = { y = y, h = line } end
    if name == "fix" then
      -- The meter sits right after the count. The reserve is sized for the
      -- widest text ("99 sats") so the bars hold still as the number changes,
      -- but measured properly: text_w's 0.55-per-character estimate is for
      -- mixed text, while digits and spaces in Montserrat run nearer 0.39 of
      -- the line height, which left an obvious gap.
      local sats_x = valx + text_w("99 sats", TH12, 12) + 4
      SATS_W = math.max(20, math.min(52, valx + valw - sats_x - 2))
      sats_cv = ui.canvas(SATS_W, SATS_H)
      sats_cv:pos(sats_x, y + math.floor((TH12 - SATS_H) / 2))
      sats_cv:fill(C.bg)
    end
    y = y + line
  end

  -- touch boards in portrait: the same actions as buttons (keys may not exist)
  if caps.touch and not landscape then
    local bw, bh = math.floor((w - 12 - 9) / 4), 30
    local by = y + 2
    local bx = 6
    if has_compass then
      ui.button("Cal", bx, by, bw, bh, toggle_cal);   bx = bx + bw + 3
      ui.button("North", bx, by, bw, bh, align_north); bx = bx + bw + 3
    end
    ui.button("Target", bx, by, bw, bh, function() cycle_target(1) end)
    y = by + bh + 4
  end

  -- key hint: centred across the whole view on the bottom edge in landscape,
  -- below everything else in portrait (where the body scrolls)
  L.hint = ui.label("", 0, hint_y or (y + 2), 12, C.sub)
  L.hint:width(w, "center")
  if caps.keyboard then
    -- "A set north" is deliberately not advertised: the axis mapping is
    -- measured, so a calibrated device points north on its own. A still works
    -- for an unknown board or a stubborn environment.
    hint_normal = has_compass and "C calibrate   up/down + OK units   <> target"
                              or "up/down + OK units   <> target"
  elseif caps.touch then
    hint_normal = "Tap a row for units, the dial for the next target"
  else
    hint_normal = "up/down + OK units   <> target"
  end
  set_text("hint", hint_normal, C.sub)

  paint_selection()
  refresh_contacts()
  next_contacts_ms = sys.millis() + CONTACTS_EVERY
  refresh(sys.millis())
  timer.every(TICK_MS)
end

function app.on_tick(dt)
  local now = sys.millis()
  if (now - next_contacts_ms) >= 0 then     -- wrap-safe: millis is a 32-bit integer here
    refresh_contacts()
    next_contacts_ms = now + CONTACTS_EVERY
  end
  if calib and (now - calib.t0) >= CAL_SECS * 1000 then toggle_cal() end
  refresh(now)
end

local function in_dial(x, y)
  return x >= dial_x and x < dial_x + D and y >= dial_y and y < dial_y + D
end

-- Diagnostics need every pixel of the panel, so the four target rows move out
-- to the key column's left edge and take the full width while it is on. The
-- TGT key label would sit on top of that, so it is blanked and restored.
local function diag_layout(on)
  for _, n in ipairs({ "tgt", "tgt2", "rel", "seen" }) do
    local l = L[n]
    if l then
      l:width(on and (col_x1 - col_x0) or val_w)
      local x, y = on and col_x0 or val_x, row_y[n]
      if y then l:pos(x, y) end
    end
    last_text[n] = nil            -- force a re-render at the new width
  end
  if L.k_tgt then L.k_tgt:set(on and "" or "TGT") end
end

-- which stats row a press landed on, or nil
local function row_at(x, y)
  if x < col_x0 or x > col_x1 then return nil end
  for name, r in pairs(row_hit) do
    if y >= r.y - 2 and y < r.y + r.h + 2 then return name end
  end
  return nil
end

function app.on_input(ev)
  if ev.type == "swipe" then
    press = nil
    -- one finger swipe can arrive twice on touch boards (LVGL's gesture and
    -- the firmware's own swipe detector both report it): take the first only
    local now = sys.millis()
    if (now - last_swipe_ms) < 300 then return end
    last_swipe_ms = now
    -- the M9's d-pad arrives here, not as key events
    if ev.dir == "left" then cycle_target(-1)
    elseif ev.dir == "right" then cycle_target(1)
    elseif ev.dir == "up" or ev.dir == "down" then move_sel(ev.dir)
    end
  elseif ev.type == "down" then
    -- "down" fires at the start of every touch, swipe or scroll drag, so a tap
    -- is only recognised on the matching "up" that landed within 12 px. The
    -- M9's OK key synthesises down+up at the body centre, which passes too.
    press = { x = ev.x or 0, y = ev.y or 0 }
  elseif ev.type == "up" then
    if press then
      local dx, dy = (ev.x or 0) - press.x, (ev.y or 0) - press.y
      if dx * dx + dy * dy <= 144 then
        if caps.touch then
          -- a tap on the ALT or SPD row switches that row's units; on the dial
          -- it steps the target
          local r = row_at(press.x, press.y)
          if r then toggle_units(r)
          elseif in_dial(press.x, press.y) then cycle_target(1) end
        else
          -- no touchscreen: this is the OK key's synthetic press, wherever the
          -- host put it — it switches the selected row's units
          toggle_units()
        end
      end
      press = nil
    end
  elseif ev.type == "key" then
    local k = ev.key
    -- deliberately no "enter" here: the host answers OK with a synthetic
    -- down/up pair AND an enter key event, so acting on both would toggle
    -- twice and look like nothing happened
    if k == "up" or k == "down" then move_sel(k)
    elseif k == "d" or k == "D" then
      diag = not diag
      last_dial_key = nil
      diag_layout(diag)
      if not diag then set_text("hint", hint_normal, C.sub) end
      sys.toast(diag and "Diagnostics on" or "Diagnostics off", 900)
    elseif k == "c" or k == "C" then toggle_cal()
    elseif k == "a" or k == "A" then align_north()
    elseif k == "x" or k == "X" then
      if cal then
        cal = nil; save_cal()
        align = 0; save_align()
        sys.toast("Calibration cleared", 1000)
      end
    end
  end
end

function app.on_close()
  calib = nil
end

return app
