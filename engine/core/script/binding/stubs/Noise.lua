---@meta _
-- Hand-written shapes of the noise API's arguments (engine/core/script/binding/NoiseClassesLua.cpp,
-- module noise), appended to the generated stubs. Rules: docs/scripting/noise.md.

---@alias NoiseBasis "perlin"|"simplex"|"value"|"cellular"
---@alias NoiseCellularReturn "f1"|"f2"|"f2_minus_f1"|"cell_value"

---What sampleGrid and sampleGridAsync take. A sample sits at the centre of its texel: (round(x / step) + i + 0.5) * step
---@class NoiseGridArgs
---@field x? number # start, in the graph's units (default 0)
---@field z? number # start, in the graph's units (default 0)
---@field width integer
---@field depth integer
---@field step? number # units per sample (default 1)
---@field seed? integer # default 1
---@field output? string # an output's name (default height, else the first)
