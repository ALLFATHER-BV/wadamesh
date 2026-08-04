-- Airtime (Lua) — channel utilization from the mesh's own airtime counters.
-- Samples rx/tx air seconds each tick and shows utilization over the window.
local ui, sys, mesh, timer = wada.ui, wada.sys, wada.mesh, wada.timer
local C = ui.colors
local app = {}
local head, sub, cv
local W, H, base, t0
local hist = {}
local HMAX = 60

function app.on_open(w, h)
  W, H = w, h
  head = ui.label("Channel utilization", 4, 4, 14, C.accent)
  sub  = ui.label("sampling...", 4, 24, 12, C.sub)
  cv = ui.canvas(w - 8, h - 70)
  cv:pos(4, 60)
  local st = mesh.stats()
  base = { rx = st.rx_air_s, tx = st.tx_air_s }
  t0 = sys.millis()
  timer.every(1000)
end

function app.on_tick(dt)
  local st = mesh.stats()
  local wall = (sys.millis() - t0) / 1000
  if wall < 1 then return end
  local air = (st.rx_air_s - base.rx) + (st.tx_air_s - base.tx)
  local pct = math.min(100, air * 100 / wall)
  table.insert(hist, pct)
  if #hist > HMAX then table.remove(hist, 1) end
  sub:set(string.format("%.1f%% of the last %ds on air  (budget %dms)",
                        pct, math.floor(wall), st.tx_budget_ms))
  local cw, ch = W - 8, H - 70
  cv:fill(0x101418)
  local bw = math.max(2, math.floor(cw / HMAX))
  for i = 1, #hist do
    local bh = math.max(1, math.floor(hist[i] * ch / 100))
    local col = hist[i] > 50 and C.bad or hist[i] > 20 and 0xE8A33D or C.good
    cv:rect((i - 1) * bw, ch - bh, bw - 1, bh, col, true)
  end
end

return app
