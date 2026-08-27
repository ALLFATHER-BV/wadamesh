-- Manual WAV/MP3 transport test for wada.audio.
-- SD test file:  /Music/test.mp3
-- App-local file: /apps/audio_test.d/test.mp3 on the active app storage.
local ui, sys, timer = wada.ui, wada.sys, wada.timer
local audio = wada.audio
local C = ui.colors

local SD_PATH = "sd:/Music/test.mp3"
local APP_PATH = "test.mp3"

local app = {}
local source = APP_PATH
local source_line, state_line, detail_line, command_line

local function show_command(ok, err)
  command_line:set(ok and "Command: accepted" or ("Command: " .. tostring(err or "rejected")))
  command_line:color(ok and C.good or C.bad)
end

local function refresh()
  if not audio then return end
  local status = audio.status()
  state_line:set("State: " .. tostring(status.state))
  state_line:color(status.state == "error" and C.bad or
                   status.state == "playing" and C.good or C.text)
  local detail = "Source: " .. tostring(status.source or "-") ..
                 "  Format: " .. tostring(status.format or "-")
  if status.error then detail = detail .. "  Error: " .. tostring(status.error) end
  detail_line:set(detail)
  detail_line:color(status.error and C.bad or C.sub)
end

function app.on_open(w, h)
  local caps = sys.caps()
  source = APP_PATH
  if caps.audio_sd and wada.sd and wada.sd.list then
    local entries = wada.sd.list("/")
    if entries then source = SD_PATH end
  end

  ui.label("Audio playback test", 6, 4, 14, C.accent)
  ui.label("audio=" .. tostring(caps.audio) ..
           " wav=" .. tostring(caps.audio_wav) ..
           " mp3=" .. tostring(caps.audio_mp3), 6, 24, 12, C.sub)
  source_line = ui.label("File: " .. source, 6, 44, 12, C.text)
  source_line:width(w - 12)
  state_line = ui.label("State: stopped", 6, 64, 12, C.text)
  detail_line = ui.label("Source: -  Format: -", 6, 82, 12, C.sub)
  detail_line:width(w - 12)
  command_line = ui.label("Command: ready", 6, 100, 12, C.sub)
  command_line:width(w - 12)

  if not caps.audio or not audio then
    command_line:set("Command: wada.audio unavailable")
    command_line:color(C.bad)
    return
  end

  local gap = 5
  local button_w = math.max(54, (w - 12 - gap * 3) // 4)
  local x = 6
  ui.button("Play", x, 116, button_w, 30, function()
    local ok, err = audio.play(source)
    show_command(ok, err)
    refresh()
  end)
  x = x + button_w + gap
  ui.button("Pause", x, 116, button_w, 30, function()
    show_command(audio.pause())
    refresh()
  end)
  x = x + button_w + gap
  ui.button("Resume", x, 116, button_w, 30, function()
    show_command(audio.resume())
    refresh()
  end)
  x = x + button_w + gap
  ui.button("Stop", x, 116, button_w, 30, function()
    show_command(audio.stop())
    refresh()
  end)

  if caps.audio_sd then
    ui.button("Switch SD / app storage", 6, 150, math.min(w - 12, 190), 26, function()
      source = source == SD_PATH and APP_PATH or SD_PATH
      source_line:set("File: " .. source)
      command_line:set("Command: source changed")
      command_line:color(C.sub)
    end)
  end

  timer.every(200)
  refresh()
end

function app.on_tick()
  refresh()
end

function app.on_close()
  if audio then audio.stop() end
end

return app
