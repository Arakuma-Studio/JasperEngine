-- (c) Eduardo Doria and contributors
-- SPDX-License-Identifier: MIT
--
-- Authors S43's Aseprite palette fixtures with the real Aseprite (1.3.18.6), from this directory:
--   aseprite -b --script make-aseprite-palettes.lua
-- aseprite-palette-rgba.aseprite: an RGB sprite whose palette has 6 colours, one half transparent;
-- aseprite-palette-indexed.aseprite: an indexed sprite of 4 colours whose transparent index is 2.

local function paletteOf(colors)
  local palette = Palette(#colors)
  for i, c in ipairs(colors) do
    palette:setColor(i - 1, Color{ r = c[1], g = c[2], b = c[3], a = c[4] or 255 })
  end
  return palette
end

local rgba = Sprite(8, 8, ColorMode.RGB)
rgba:setPalette(paletteOf({
  { 26, 28, 44 }, { 93, 39, 93 }, { 177, 62, 83 }, { 239, 125, 87 }, { 255, 205, 117, 128 }, { 167, 240, 112 },
}))
rgba:saveAs("aseprite-palette-rgba.aseprite")
rgba:close()

local indexed = Sprite(8, 8, ColorMode.INDEXED)
indexed:setPalette(paletteOf({ { 0, 0, 0 }, { 255, 255, 255 }, { 255, 0, 255 }, { 41, 173, 255 } }))
indexed.transparentColor = 2
indexed:saveAs("aseprite-palette-indexed.aseprite")
indexed:close()
