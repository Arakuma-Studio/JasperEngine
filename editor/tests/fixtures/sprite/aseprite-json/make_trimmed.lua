-- S45 R10 fixture: a 24 x 16 sprite of 3 frames whose opaque pixels sit at a different place in
-- each frame (so --trim gives each frame its own offset), and a 9-patch slice "panel" with a pivot
-- whose bounds are not the whole frame (Aseprite's Lua API keys a slice at frame 0 only; the tests
-- add a later key to the JSON for the key ranges).
-- Run from this folder:
--   aseprite -b --script make_trimmed.lua
--   aseprite -b trimmed.aseprite --sheet trimmed.png --data trimmed.json --format json-hash --trim --list-slices --sheet-type horizontal
--   aseprite -b trimmed.aseprite --sheet trimmed-array.png --data trimmed-array.json --format json-array --trim --list-slices --sheet-type horizontal
local spr = Sprite(24, 16, ColorMode.RGB)
spr.filename = "trimmed.aseprite"
local boxes = {{2, 3, 10, 8}, {9, 1, 12, 12}, {4, 6, 16, 9}}   -- x, y, w, h per frame
for i = 2, 3 do spr:newFrame() end
local layer = spr.layers[1]
for f, b in ipairs(boxes) do
  local img = Image(24, 16, ColorMode.RGB)
  for y = b[2], b[2] + b[4] - 1 do
    for x = b[1], b[1] + b[3] - 1 do
      img:drawPixel(x, y, app.pixelColor.rgba((x * 9 + f * 40) % 256, (y * 13) % 256, f * 70, 255))
    end
  end
  spr:newCel(layer, f, img, Point(0, 0))
end
local slice = spr:newSlice(Rectangle(1, 2, 20, 12))
slice.name = "panel"
slice.center = Rectangle(3, 2, 12, 7)
slice.pivot = Point(10, 12)
spr:saveAs("trimmed.aseprite")
