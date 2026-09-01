-- Hello World — minimal Lua app to verify the app host is working.
local ui, sys = wada.ui, wada.sys
local C = ui.colors
local app = {}

local lbl, sub

function app.on_open(w, h)
  lbl = ui.label(0, h / 2 - 20, w, 30)
  lbl:set("Hello, World!")
  lbl:color(C.accent)
  lbl:align("center")

  sub = ui.label(0, h / 2 + 16, w, 20)
  sub:set("Lua app host is working  \xE2\x80\x94  " .. (sys.board() or ""))
  sub:color(C.sub)
  sub:align("center")
end

function app.on_close()
end

return app
