-- (c) Eduardo Doria and contributors
-- SPDX-License-Identifier: MIT

-- Authors S44's golden corpus (docs/specs/S44-aseprite-import.md §9) with Aseprite's Lua API.
-- make_golden_corpus.py drives it; by hand:
--
--   aseprite -b --script-param out=<dir> [--script-param only=<name>] --script make_golden_corpus.lua
--       writes <dir>/<name>.aseprite for every file and prints one "JE-MANIFEST <json>" line
--       (what make_golden_corpus.py patches in afterwards, see below)
--   aseprite -b --script-param render=<file.aseprite> --script make_golden_corpus.lua
--       draws each frame with composed groups into <file>_composed_<NN>.png (1-based) and
--       prints "JE-GROUP <json>" with the opacity and blend mode Aseprite read for each group
--   aseprite -b --script-param resave=<in.aseprite> --script-param to=<out.aseprite> --script ...
--       opens a file and saves it again
--
-- Two things the API cannot reach in batch mode, so the manifest carries them and the Python
-- driver writes them into the file: a batch save never sets header flag 2 (group opacity and
-- blend valid: FileOpConfig only reads the compose_groups preference on the UI thread), and a
-- slice's keys after frame 0 (Slice.bounds/center/pivot only set the frame-0 key).
--
-- Every pixel is computed, nothing is random: the same Aseprite writes the same bytes.

local outDir = app.params["out"] or "."
local only = app.params["only"]

-- groups can carry opacity and blend mode only with this preference (Layer.opacity getters)
app.preferences.experimental.compose_groups = true

local pc = app.pixelColor

local function rgba(r, g, b, a)
  return pc.rgba(r % 256, g % 256, b % 256, a)
end

local function fill(img, x, y, w, h, value)
  for yy = y, y + h - 1 do
    for xx = x, x + w - 1 do
      img:drawPixel(xx, yy, value)
    end
  end
end

-- An opaque pattern with every channel varying, so blend modes differ visibly
local function patternImage(w, h, seed)
  local img = Image(w, h, ColorMode.RGB)
  for y = 0, h - 1 do
    for x = 0, w - 1 do
      img:drawPixel(x, y, rgba(x * 11 + seed * 37, y * 17 + seed * 13, 255 - (x + y) * 7, 255))
    end
  end
  return img
end

local function newSprite(w, h, mode, name)
  local spr = Sprite(w, h, mode)
  app.activeSprite = spr
  spr.filename = outDir .. "/" .. name .. ".aseprite"
  return spr
end

-- Blend modes by our names (editor/pixelart/aseprite/AsepriteDocument.cpp), which are the file's order
local BLEND = {
  normal = BlendMode.NORMAL, multiply = BlendMode.MULTIPLY, screen = BlendMode.SCREEN,
  overlay = BlendMode.OVERLAY, darken = BlendMode.DARKEN, lighten = BlendMode.LIGHTEN,
  color_dodge = BlendMode.COLOR_DODGE, color_burn = BlendMode.COLOR_BURN,
  hard_light = BlendMode.HARD_LIGHT, soft_light = BlendMode.SOFT_LIGHT,
  difference = BlendMode.DIFFERENCE, exclusion = BlendMode.EXCLUSION, hue = BlendMode.HSL_HUE,
  saturation = BlendMode.HSL_SATURATION, color = BlendMode.HSL_COLOR,
  luminosity = BlendMode.HSL_LUMINOSITY, addition = BlendMode.ADDITION,
  subtract = BlendMode.SUBTRACT, divide = BlendMode.DIVIDE,
}

local function blendName(value)
  for name, v in pairs(BLEND) do
    if v == value then return name end
  end
  return "unknown"
end

local manifest = {}

local function save(spr, name, entry)
  spr:saveAs(outDir .. "/" .. name .. ".aseprite")
  entry = entry or {}
  entry.name = name
  table.insert(manifest, entry)
  spr:close()
end

local function wanted(name)
  return only == nil or only == "" or only == name
end

local function rect(x, y, w, h) return { x = x, y = y, w = w, h = h } end

---------------------------------------------------------------------------------------------
-- group_blend: one group per blend mode, each with its own opacity, over an opaque pattern.
-- Two children per group overlap, so composing the group first differs from flattening it.

local function makeGroupBlend()
  local name = "group_blend"
  local modes = {
    { "normal", 128 }, { "multiply", 200 }, { "screen", 255 }, { "overlay", 127 },
    { "difference", 64 }, { "color", 254 }, { "addition", 1 }, { "darken", 180 },
  }
  local spr = newSprite(#modes * 4, 16, ColorMode.RGB, name)
  spr:newEmptyFrame()
  spr.frames[1].duration = 0.1
  spr.frames[2].duration = 0.15
  local base = spr.layers[1]
  base.name = "base"
  for f = 1, 2 do spr:newCel(base, f, patternImage(spr.width, spr.height, f), Point(0, 0)) end
  local groups = {}
  for i, m in ipairs(modes) do
    local g = spr:newGroup()
    g.name = "g_" .. m[1]
    local a = spr:newLayer()
    a.name = "a_" .. m[1]
    a.parent = g
    local b = spr:newLayer()
    b.name = "b_" .. m[1]
    b.parent = g
    b.blendMode = BlendMode.MULTIPLY
    b.opacity = 220
    for f = 1, 2 do
      local ia = Image(4, 12, ColorMode.RGB)
      ia:clear(rgba(255 - i * 25, 40 + i * 20 + f * 9, 200 - i * 10, 200))
      spr:newCel(a, f, ia, Point((i - 1) * 4, 0))
      local ib = Image(4, 10, ColorMode.RGB)
      ib:clear(rgba(30 + i * 20, 220 - i * 15, 90 + f * 40, 230))
      spr:newCel(b, f, ib, Point((i - 1) * 4, 5 + f))
    end
    g.blendMode = BLEND[m[1]]
    g.opacity = m[2]
    groups[g.name] = { blend = m[1], opacity = m[2] }
  end
  save(spr, name, { groups = groups })
end

---------------------------------------------------------------------------------------------
-- nested_groups: three composed levels with their own opacity and blend, a hidden layer and a
-- hidden group inside, two frames with moved cels.

local function makeNestedGroups()
  local name = "nested_groups"
  local spr = newSprite(16, 16, ColorMode.RGB, name)
  spr:newEmptyFrame()
  local base = spr.layers[1]
  base.name = "base"
  for f = 1, 2 do spr:newCel(base, f, patternImage(16, 16, f + 4), Point(0, 0)) end

  local outer = spr:newGroup()
  outer.name = "outer"
  local o1 = spr:newLayer()
  o1.name = "o1"
  o1.parent = outer
  local middle = spr:newGroup()
  middle.name = "middle"
  middle.parent = outer
  local m1 = spr:newLayer()
  m1.name = "m1"
  m1.parent = middle
  m1.blendMode = BlendMode.OVERLAY
  m1.opacity = 200
  local inner = spr:newGroup()
  inner.name = "inner"
  inner.parent = middle
  local i1 = spr:newLayer()
  i1.name = "i1"
  i1.parent = inner
  i1.blendMode = BlendMode.SCREEN
  local i2 = spr:newLayer()
  i2.name = "i2_hidden"
  i2.parent = inner
  i2.isVisible = false
  local hiddenGroup = spr:newGroup()
  hiddenGroup.name = "hidden_group"
  hiddenGroup.parent = outer
  hiddenGroup.isVisible = false
  local h1 = spr:newLayer()
  h1.name = "h1"
  h1.parent = hiddenGroup

  for f = 1, 2 do
    local io = Image(10, 10, ColorMode.RGB)
    io:clear(rgba(240, 120, 30, 190))
    spr:newCel(o1, f, io, Point(f, 1))
    local im = Image(8, 12, ColorMode.RGB)
    im:clear(rgba(40, 90, 250, 220))
    spr:newCel(m1, f, im, Point(6 - f, 3))
    local ii = Image(6, 6, ColorMode.RGB)
    ii:clear(rgba(200, 250, 60, 255))
    spr:newCel(i1, f, ii, Point(4 + f, 6))
    local ih = Image(16, 16, ColorMode.RGB)
    ih:clear(rgba(255, 0, 255, 255))
    spr:newCel(i2, f, ih, Point(0, 0))
    spr:newCel(h1, f, ih, Point(0, 0))
  end

  local groups = {}
  local function setGroup(g, blend, opacity)
    g.blendMode = BLEND[blend]
    g.opacity = opacity
    groups[g.name] = { blend = blend, opacity = opacity }
  end
  setGroup(outer, "screen", 180)
  setGroup(middle, "multiply", 128)
  setGroup(inner, "difference", 255)
  setGroup(hiddenGroup, "normal", 255)
  save(spr, name, { groups = groups })
end

---------------------------------------------------------------------------------------------
-- cel_layer_opacity: layer opacity {1, 127, 254} (rows) x cel opacity {1, 127, 254} (frames)
-- for six blend modes (columns of 4x4 blocks) over an opaque pattern.

local function makeCelLayerOpacity()
  local name = "cel_layer_opacity"
  local modes = { "normal", "multiply", "screen", "overlay", "hue", "addition" }
  local opacities = { 1, 127, 254 }
  local spr = newSprite(#modes * 4, #opacities * 4, ColorMode.RGB, name)
  spr:newEmptyFrame()
  spr:newEmptyFrame()
  local base = spr.layers[1]
  base.name = "base"
  for f = 1, 3 do spr:newCel(base, f, patternImage(spr.width, spr.height, 9), Point(0, 0)) end
  for bi, mode in ipairs(modes) do
    for oi, layerOpacity in ipairs(opacities) do
      local layer = spr:newLayer()
      layer.name = mode .. "_" .. layerOpacity
      layer.blendMode = BLEND[mode]
      layer.opacity = layerOpacity
      for f = 1, 3 do
        local img = Image(4, 4, ColorMode.RGB)
        for y = 0, 3 do
          for x = 0, 3 do
            -- the right half is half transparent: pixel alpha x cel x layer
            img:drawPixel(x, y, rgba(60 + bi * 30 + x * 20, 250 - oi * 60, 30 + y * 50, x < 2 and 255 or 128))
          end
        end
        local cel = spr:newCel(layer, f, img, Point((bi - 1) * 4, (oi - 1) * 4))
        cel.opacity = opacities[f]
      end
    end
  end
  save(spr, name)
end

---------------------------------------------------------------------------------------------
-- tilemap_flips: a 4x4-tile tilemap over an image layer; frame 1 shows tile 1 with all eight
-- combinations of the x, y and diagonal flips, frame 2 tile 2 (and empty tiles) the same way.

local TILE_XFLIP = 0x80000000
local TILE_YFLIP = 0x40000000
local TILE_DFLIP = 0x20000000

local function makeTilemapFlips()
  local name = "tilemap_flips"
  local spr = newSprite(16, 8, ColorMode.RGB, name)
  spr:newEmptyFrame()
  local base = spr.layers[1]
  base.name = "base"
  for f = 1, 2 do spr:newCel(base, f, patternImage(16, 8, f + 2), Point(0, 0)) end
  app.activeLayer = base
  app.command.NewLayer { tilemap = true, gridBounds = Rectangle(0, 0, 4, 4) }
  local tm = app.layer
  tm.name = "tiles"
  local ts = tm.tileset
  ts.name = "flip_tiles"
  -- two asymmetric tiles: every flip gives a different picture
  local t1 = spr:newTile(ts)
  for y = 0, 3 do
    for x = 0, 3 do
      if x == 0 or y == 0 then
        t1.image:drawPixel(x, y, rgba(255 - y * 40, x * 60, 40, 255))
      elseif x == 3 and y == 3 then
        t1.image:drawPixel(x, y, rgba(20, 20, 250, 160))
      end
    end
  end
  local t2 = spr:newTile(ts)
  for y = 0, 3 do
    for x = 0, y do
      t2.image:drawPixel(x, y, rgba(x * 70, 255 - x * 50, y * 80, 255))
    end
  end
  local flags = { 0, TILE_XFLIP, TILE_YFLIP, TILE_DFLIP, TILE_XFLIP | TILE_YFLIP,
                  TILE_XFLIP | TILE_DFLIP, TILE_YFLIP | TILE_DFLIP, TILE_XFLIP | TILE_YFLIP | TILE_DFLIP }
  for f = 1, 2 do
    local map = Image(4, 2, ColorMode.TILEMAP)
    for i = 0, 7 do
      local tile = f
      if f == 2 and i % 3 == 2 then tile = 0 end
      map:drawPixel(i % 4, i // 4, tile | flags[i + 1])
    end
    spr:newCel(tm, f, map, Point(0, 0))
  end
  save(spr, name)
end

---------------------------------------------------------------------------------------------
-- user_data_all: text, colour and properties of every type the Lua API can make on the sprite,
-- a layer, a cel, a tag, a slice and the tileset; nested maps three levels deep; an
-- extension-keyed map. Integers are stored in the smallest type that holds them, so the values
-- pick int8, uint8, int16, uint16, int32, uint32 and int64.

local function allProperties(seed)
  return {
    b_true = true,
    b_false = false,
    i8 = -100 + seed,
    u8 = 200 + seed,
    i16 = -30000 + seed,
    u16 = 60000 + seed,
    i32 = -2000000000 + seed,
    u32 = 4000000000 + seed,
    i64 = -9000000000000 + seed,
    dbl = 3.25 + seed,
    str = "text " .. seed .. " \u{e9}",
    pt = Point(-3, 4 + seed),
    sz = Size(5, 6 + seed),
    rc = Rectangle(1, -2, 3, 4 + seed),
    id = Uuid(string.format("01234567-89ab-cdef-0123-4567890abc%02x", seed)),
    vec_int = { 1, 2, 300 + seed },
    vec_mixed = { 1, "two", Point(3, 3), true, { k = seed } },
    map3 = { level2 = { level3 = { deep = 7 + seed, s = "x" } } },
  }
end

local function putUserData(obj, text, color, seed)
  obj.data = text
  obj.color = color
  for k, v in pairs(allProperties(seed)) do obj.properties[k] = v end
  obj.properties("je_ext").owner = text
  obj.properties("je_ext").n = seed
end

local function makeUserDataAll()
  local name = "user_data_all"
  local spr = newSprite(8, 8, ColorMode.RGB, name)
  spr:newEmptyFrame()
  local layer = spr.layers[1]
  layer.name = "layer"
  for f = 1, 2 do
    local img = Image(8, 8, ColorMode.RGB)
    img:clear(rgba(f * 100, 50, 200, 255))
    spr:newCel(layer, f, img, Point(0, 0))
  end
  putUserData(spr, "sprite text", Color { r = 10, g = 20, b = 30, a = 255 }, 1)
  putUserData(layer, "layer text", Color { r = 200, g = 0, b = 0, a = 255 }, 2)
  putUserData(layer:cel(1), "cel text", Color { r = 0, g = 200, b = 0, a = 128 }, 3)
  local tag = spr:newTag(1, 2)
  tag.name = "tagged"
  putUserData(tag, "tag text", Color { r = 0, g = 0, b = 200, a = 255 }, 4)
  local slice = spr:newSlice(Rectangle(1, 1, 4, 4))
  slice.name = "noted"
  putUserData(slice, "slice text", Color { r = 255, g = 255, b = 0, a = 255 }, 5)
  save(spr, name)
end

---------------------------------------------------------------------------------------------
-- indexed_background: an 8-colour indexed sprite whose transparent index is 3, a background
-- layer that uses index 3 (opaque there) and a layer above it where index 3 is clear.

local function makeIndexedBackground()
  local name = "indexed_background"
  local spr = newSprite(8, 8, ColorMode.INDEXED, name)
  local pal = spr.palettes[1]
  pal:resize(8)
  local colors = {
    { 0, 0, 0 }, { 255, 255, 255 }, { 200, 40, 40 }, { 255, 0, 255 },
    { 40, 200, 40 }, { 40, 40, 200 }, { 240, 200, 20 }, { 20, 200, 220 },
  }
  for i, c in ipairs(colors) do pal:setColor(i - 1, Color { r = c[1], g = c[2], b = c[3], a = 255 }) end
  spr.transparentColor = 3
  spr:newEmptyFrame()
  local bg = spr.layers[1]
  app.activeLayer = bg
  app.command.BackgroundFromLayer()
  bg = spr.backgroundLayer
  local fg = spr:newLayer()
  fg.name = "fg"
  for f = 1, 2 do
    local b = Image(8, 8, ColorMode.INDEXED)
    for y = 0, 7 do
      for x = 0, 7 do b:drawPixel(x, y, (x + y + f) % 8) end
    end
    spr:newCel(bg, f, b, Point(0, 0))
    local t = Image(6, 6, ColorMode.INDEXED)
    for y = 0, 5 do
      for x = 0, 5 do t:drawPixel(x, y, ((x * y + f) % 3 == 0) and 3 or (x + f) % 8) end
    end
    spr:newCel(fg, f, t, Point(1, 1))
  end
  save(spr, name)
end

---------------------------------------------------------------------------------------------
-- tags_directions: eight frames of distinct durations and colours; one tag per direction x
-- repeat {0, 1, 3}, overlapping.

local function makeTagsDirections()
  local name = "tags_directions"
  local spr = newSprite(4, 4, ColorMode.RGB, name)
  for _ = 2, 8 do spr:newEmptyFrame() end
  local layer = spr.layers[1]
  layer.name = "frames"
  for f = 1, 8 do
    spr.frames[f].duration = (f * 25 + 15) / 1000
    local img = Image(4, 4, ColorMode.RGB)
    img:clear(rgba(f * 30, 255 - f * 30, (f * 77) % 256, 255))
    spr:newCel(layer, f, img, Point(0, 0))
  end
  local dirs = {
    { "forward", AniDir.FORWARD }, { "reverse", AniDir.REVERSE },
    { "pingpong", AniDir.PING_PONG }, { "pingpong_reverse", AniDir.PING_PONG_REVERSE },
  }
  local k = 0
  for _, d in ipairs(dirs) do
    for _, rep in ipairs({ 0, 1, 3 }) do
      local from = (k % 5) + 1
      local to = math.min(8, from + 1 + (k % 3))
      local tag = spr:newTag(from, to)
      tag.name = d[1] .. "_r" .. rep
      tag.aniDir = d[2]
      tag.repeats = rep
      tag.color = Color { r = (k * 40) % 256, g = 128, b = 255 - k * 20, a = 255 }
      k = k + 1
    end
  end
  save(spr, name)
end

---------------------------------------------------------------------------------------------
-- slices_keys: a 9-patch slice, a pivot slice, one with both and a plain one, each with keys on
-- later frames and a hidden key (width 0); the later keys come from the manifest.

local function makeSlicesKeys()
  local name = "slices_keys"
  local spr = newSprite(32, 32, ColorMode.RGB, name)
  for _ = 2, 6 do spr:newEmptyFrame() end
  local layer = spr.layers[1]
  layer.name = "art"
  for f = 1, 6 do spr:newCel(layer, f, patternImage(32, 32, f), Point(0, 0)) end

  local nine = spr:newSlice(Rectangle(2, 2, 20, 20))
  nine.name = "nine"
  nine.center = Rectangle(4, 4, 12, 12)
  local pivot = spr:newSlice(Rectangle(8, 8, 8, 16))
  pivot.name = "pivot"
  pivot.pivot = Point(4, 15)
  local both = spr:newSlice(Rectangle(0, 16, 16, 16))
  both.name = "both"
  both.center = Rectangle(3, 3, 10, 10)
  both.pivot = Point(8, 16)
  local plain = spr:newSlice(Rectangle(20, 0, 12, 8))
  plain.name = "plain"

  local hidden = rect(0, 0, 0, 0)
  local slices = {
    nine = {
      { frame = 0, bounds = rect(2, 2, 20, 20), center = rect(4, 4, 12, 12) },
      { frame = 2, bounds = rect(4, 2, 24, 26), center = rect(5, 6, 14, 14) },
      { frame = 4, bounds = hidden, center = hidden },
    },
    pivot = {
      { frame = 0, bounds = rect(8, 8, 8, 16), pivot = { x = 4, y = 15 } },
      { frame = 1, bounds = rect(9, 8, 8, 16), pivot = { x = 3, y = 15 } },
      { frame = 3, bounds = hidden, pivot = { x = 0, y = 0 } },
      { frame = 5, bounds = rect(-4, 20, 8, 16), pivot = { x = -2, y = 20 } },
    },
    both = {
      { frame = 0, bounds = rect(0, 16, 16, 16), center = rect(3, 3, 10, 10), pivot = { x = 8, y = 16 } },
      { frame = 3, bounds = rect(2, 14, 18, 18), center = rect(4, 4, 10, 10), pivot = { x = 9, y = 18 } },
    },
    plain = {
      { frame = 0, bounds = rect(20, 0, 12, 8) },
      { frame = 1, bounds = hidden },
      { frame = 2, bounds = rect(20, 2, 12, 8) },
    },
  }
  save(spr, name, { slices = slices })
end

---------------------------------------------------------------------------------------------

local renderPath = app.params["render"]
local resavePath = app.params["resave"]

if renderPath and renderPath ~= "" then
  local spr = app.open(renderPath)
  if not spr then error("cannot open " .. renderPath) end
  local stem = renderPath:gsub("%.aseprite$", ""):gsub("%.ase$", "")
  for f = 1, #spr.frames do
    local img = Image(spr.width, spr.height, ColorMode.RGB)
    img:drawSprite(spr, f, Point(0, 0))
    img:saveAs(string.format("%s_composed_%02d.png", stem, f))
  end
  local groups = {}
  for _, layer in ipairs(spr.layers) do
    local function walk(l)
      if l.isGroup then
        groups[l.name] = { blend = blendName(l.blendMode), opacity = l.opacity }
        for _, child in ipairs(l.layers) do walk(child) end
      end
    end
    walk(layer)
  end
  print("JE-GROUP " .. json.encode(groups))
elseif resavePath and resavePath ~= "" then
  local spr = app.open(resavePath)
  if not spr then error("cannot open " .. resavePath) end
  spr:saveAs(app.params["to"])
else
  local makers = {
    { "group_blend", makeGroupBlend },
    { "nested_groups", makeNestedGroups },
    { "cel_layer_opacity", makeCelLayerOpacity },
    { "tilemap_flips", makeTilemapFlips },
    { "user_data_all", makeUserDataAll },
    { "indexed_background", makeIndexedBackground },
    { "tags_directions", makeTagsDirections },
    { "slices_keys", makeSlicesKeys },
  }
  for _, m in ipairs(makers) do
    if wanted(m[1]) then m[2]() end
  end
  print("JE-MANIFEST " .. json.encode(manifest))
end
