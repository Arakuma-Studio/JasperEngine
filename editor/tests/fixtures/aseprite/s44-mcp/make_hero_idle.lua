-- (c) Eduardo Doria and contributors
-- SPDX-License-Identifier: MIT
--
-- The character of the S44 MCP tests (tools/mcp/tests/test_aseprite_import.py,
-- test_aseprite_hot_reload.py), authored in Aseprite's batch mode; make.py runs it, writes the
-- slice keys Aseprite's Lua API cannot (frame 0 only) and has Aseprite re-save the file.
--
--   aseprite -b --script-param out=hero_idle.aseprite --script make_hero_idle.lua
--   aseprite -b --script-param out=hero_idle_recolor.aseprite --script-param recolor=1 --script make_hero_idle.lua
--
-- 32x32 RGBA, 6 frames (100, 200, 100, 200, 300, 300 ms), layers bottom to top:
--   Shadow  20x3 black at alpha 128 at (6, 28) on every frame
--   Body    16x24 at (8, 4), one opaque colour per frame (recolor=1: reds, so frame 0 turns red)
--   events  hidden; on frame 1 a 1x1 cel whose user data text is onStep("left", 1)
-- tags walk (frames 0-3, forward, repeat 0) and Idle (4-5, ping-pong, repeat 3); slice hurt
-- (8, 8, 16, 16) on frame 0 (make.py adds its other keys); sprite user properties
-- light2d.intensity = 2.5 and note = "data only".
local out = app.params["out"] or "hero_idle.aseprite"
local recolor = app.params["recolor"] == "1"

local spr = Sprite(32, 32, ColorMode.RGB)
spr.filename = out

local shadow = spr.layers[1]
shadow.name = "Shadow"
local body = spr:newLayer()
body.name = "Body"
local events = spr:newLayer()
events.name = "events"
events.isVisible = false

local durations = { 0.1, 0.2, 0.1, 0.2, 0.3, 0.3 }
for _ = 2, #durations do
  spr:newEmptyFrame()
end
for f = 1, #durations do
  spr.frames[f].duration = durations[f]
end

local colors = {
  { 40, 120, 220 },   -- 0 blue
  { 40, 200, 90 },    -- 1 green
  { 220, 200, 40 },   -- 2 yellow
  { 200, 60, 200 },   -- 3 magenta
  { 60, 200, 220 },   -- 4 cyan
  { 240, 140, 40 },   -- 5 orange
}
local reds = {
  { 230, 40, 40 }, { 200, 30, 60 }, { 240, 70, 50 }, { 180, 20, 30 }, { 250, 90, 80 }, { 210, 50, 20 },
}
for f = 1, #durations do
  local c = (recolor and reds or colors)[f]
  local img = Image(16, 24, ColorMode.RGB)
  img:clear(Color{ r = c[1], g = c[2], b = c[3], a = 255 })
  spr:newCel(body, f, img, Point(8, 4))

  local s = Image(20, 3, ColorMode.RGB)
  s:clear(Color{ r = 0, g = 0, b = 0, a = 128 })
  spr:newCel(shadow, f, s, Point(6, 28))
end

local marker = Image(1, 1, ColorMode.RGB)
marker:clear(Color{ r = 255, g = 255, b = 255, a = 255 })
local step = spr:newCel(events, 2, marker, Point(0, 0))
step.data = 'onStep("left", 1)'

local walk = spr:newTag(1, 4)
walk.name = "walk"
walk.aniDir = AniDir.FORWARD
walk.repeats = 0
local idle = spr:newTag(5, 6)
idle.name = "Idle"
idle.aniDir = AniDir.PING_PONG
idle.repeats = 3

local hurt = spr:newSlice(Rectangle(8, 8, 16, 16))
hurt.name = "hurt"

spr.properties["light2d.intensity"] = 2.5
spr.properties["note"] = "data only"

spr:saveAs(out)
print("wrote " .. out .. ": " .. #spr.layers .. " layers, " .. #spr.frames .. " frames, " .. #spr.tags .. " tags")
