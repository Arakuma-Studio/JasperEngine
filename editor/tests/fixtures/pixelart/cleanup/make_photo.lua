-- Authors photo.jpg (S46 AC11, AC15): a picture with no pixel grid, soft gradients and shapes with
-- noise, so the cleanup has nothing to recover. Run from this directory:
--   /var/lib/orquester/workspaces/jaspersito/tools-aseprite/aseprite -b --script make_photo.lua
local width, height = 256, 192
local sprite = Sprite(width, height, ColorMode.RGB)
local image = sprite.cels[1].image

-- A small LCG, so the fixture is the same on every run
local state = 12345
local function nextValue()
    state = (state * 1103515245 + 12345) % 2147483648
    return state / 2147483648
end

local function clamp(v)
    if v < 0 then return 0 end
    if v > 255 then return 255 end
    return math.floor(v + 0.5)
end

for y = 0, height - 1 do
    for x = 0, width - 1 do
        local u, v = x / (width - 1), y / (height - 1)
        -- a sky-like vertical gradient with a slow wave
        local r = 90 + 120 * v + 28 * math.sin(u * 7.3 + v * 3.1)
        local g = 130 + 60 * v + 22 * math.sin(u * 5.1 - v * 4.7)
        local b = 210 - 110 * v + 18 * math.sin(u * 3.7 + v * 6.3)
        -- soft blobs, nothing with a hard edge
        local d1 = math.sqrt((u - 0.3) ^ 2 + (v - 0.6) ^ 2)
        local d2 = math.sqrt((u - 0.72) ^ 2 + (v - 0.4) ^ 2)
        local blob1 = math.exp(-d1 * d1 * 40)
        local blob2 = math.exp(-d2 * d2 * 70)
        r = r + 90 * blob1 - 40 * blob2
        g = g - 30 * blob1 + 70 * blob2
        b = b - 50 * blob1 + 30 * blob2
        -- sensor noise
        local n = (nextValue() - 0.5) * 14
        image:drawPixel(x, y, Color { r = clamp(r + n), g = clamp(g + n), b = clamp(b + n), a = 255 })
    end
end

sprite:saveCopyAs("photo.jpg")
