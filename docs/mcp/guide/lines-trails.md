# Lines and trails

Two components draw ribbons: **Line Renderer** (component `LineRendererComponent`, MCP name
`linerenderer`, aliases `lines`, `line`; Lua/C++ class `LineRenderer`) and **Trail Renderer**
(`TrailRendererComponent`, MCP `trailrenderer`, alias `trail`; Lua/C++ `TrailRenderer`). Both
follow Unity's LineRenderer and TrailRenderer: the same field names, a width curve and a colour
gradient over the line, round corners and caps, four texture modes, View or Transform Z
alignment. They are unlit (Unity's default line materials are too) and cast no shadows.

Create them with `create_entity type=line_renderer` / `type=trail_renderer` (aliases `line`,
`trail`; `parent` puts a trail under its moving object) or GameObject > Effects > Line Renderer /
Trail Renderer (the Trail Renderer menu item adds one to the selected GameObject when it has
none). A Trail Renderer coexists with the GameObject's Mesh, Sprite or other renderer.

## Line Renderer

### Data modes

`mode` picks the data that draws (both lists are kept and saved):

| Mode | Data | Colours | Use |
|---|---|---|---|
| `polyline` (Unity) | `positions` (Vector3 list), `loop` closes it | `colorGradient` over the length | lasers, ropes, outlines, paths |
| `segments` | `lines`: independent A-B segments (`pointA`, `colorA`, `pointB`, `colorB`) | each segment's own | debug drawings, gizmo-like lines |

The Unity API (`positionCount`, `setPosition`, `setPositions`) switches to `polyline`, the segment
API (`addLine`, `updateLine`, `removeLine`, `clearLines`) to `segments`. Over MCP, writing the
whole `positions` list switches to `polyline`; writing the whole `lines` list switches to
`segments` in local space (`useWorldSpace` false), as segment lists always were, unless the same
call sets `mode` / `useWorldSpace`. `set_shape_points component=line_renderer` takes `points`
(local to the GameObject like every shape; a world-space line gets them in world space, so the
line sits where the points say relative to the object; `closed` loops it; `color` / `colors`
make its gradient, up to 8 points) or `lines` (segments, local). With `segments` the width,
curve (over each segment) and caps apply per segment; there are no joints and the gradient is
not used. `useWorldSpace` (both modes): the points are world positions and the transform is
ignored; off, they move, turn and scale with the GameObject (the width does not scale).

New Line Renderers from the editor (menu, Add Component, `create_entity`, `add_component`) are
Unity's: `polyline`, world space, positions (0,0,0)-(0,0,1) and width 0.1 (2D scenes:
(0,0)-(100,0), width 4). A `LineRenderer(scene)` made by a script keeps the old defaults
(`segments`, width 0) so existing scripts draw as before.

### Width

The width is `widthMultiplier` x `widthCurve` evaluated over the normalized length (0 = the
first point, 1 = the last). **`widthMultiplier` 0 is the hairline**: the 1-pixel `LINES`
primitive (a polyline then draws as connected 1-pixel segments with the gradient's colours; no
curve, caps, joints or texture). Old scenes have no width and draw exactly as before.
`startWidth` / `endWidth` (Lua) read the width at the ends; setting one makes the curve linear
between them with the multiplier 1. `widthUnit`: `world` (Unity, world units) or `pixels`
(screen pixels of the render target, the same size at any distance: debug lines; the curve still
shapes it; Transform Z alignment does not apply). Negative curve values are width 0.

### Corners, caps, alignment

- `numCornerVertices` (0..90): 0 is a sharp miter corner, limited to 4 half widths and bevelled
  beyond (no spikes on sharp turns); more makes a round corner of that many vertices.
- `numCapVertices` (0..90): 0 is a flat end at the point; more is a round cap reaching half a
  width past it.
- `alignment`: `view` (the ribbon faces the camera; every camera sees it full width) or
  `transform_z` (it lies across the GameObject's Z axis, like a flat strip: thin seen edge-on).

### Colour and texture

`colorGradient`: colour keys and alpha keys over the length (see Curves and gradients); Lua
`startColor` / `endColor` key the two ends. Blending: AUTO is alpha blending without depth
writes (blended ribbons sort with the transparent renderers by distance, `sortPriority` first,
lower first); set `blendMode` for additive, premultiplied, multiply or opaque (opaque ribbons draw
in the opaque pass and write depth).

The built-in line shader samples a **base texture** (`texture`; stored as
`shaderTextures.u_baseTexture`, white without one) times the colour, at texture coordinates that
need a width (a hairline has none: `check` warns `line-texture-no-uv`). `textureMode`:

| Mode | u along the line |
|---|---|
| `stretch` | once over the whole length (x `textureScale.x`) |
| `tile` | repeats every 1 / `textureScale.x` world units (Unity) |
| `distribute_per_segment` | once over the line, each segment the same share |
| `repeat_per_segment` | once per segment |

v runs across the ribbon (0 on one side, `textureScale.y` on the other). Trails measure u from
the head.

## Trail Renderer

A ribbon behind a moving GameObject, in world space. Each frame (scaled game time: it freezes at
`Engine.timeScale` 0), after scripts, animation and physics moved the object:

- points older than `time` seconds are removed;
- while `emitting`, a point is added when the object moved more than `minVertexDistance` from the
  newest one (0: one per frame while it moves; at most 4096 points, the oldest dropped);
- while emitting, the live head (where the object is now) is the ribbon's first point; with
  `emitting` false the trail is detached: its stored points stay and fade out, not joined to the
  object (a pooled bullet: set emitting false, move it, `clear()` or set emitting true again).

`widthCurve` and `colorGradient` run by **age**: 0 is the head, 1 a point `time` old (Unity's
fade). `autodestruct`: the GameObject is destroyed once the trail has had no points and the
object has not moved for `time` seconds. A trail whose GameObject becomes inactive, or which is
disabled, is **cleared** at once (a pooled bullet deactivated, moved and reactivated, even in the
same frame, draws no streak from where it was); after teleporting an active object call
`clear()`. In Lua `TrailRenderer(scene)` makes a GameObject with a trail, `object:addTrailRenderer()`
adds one to an existing GameObject (Unity's AddComponent), `object:getTrailRenderer()` (nil
without one) and `object(name, "TrailRenderer")` only look one up. A new trail from the editor: width 0.2
(2D: 8, `minVertexDistance` 2), thinning 1 -> 0 and fading out to the tail. A trail with `time` 0
draws nothing (`check` warns `trail-zero-time`).

Lua positions (`positionCount`, `getPosition(i)`, `setPosition(i, v)`, `getPositions()`) are the
stored points, oldest first (Unity's order); `addPosition(v)` / `addPositions(t)` add points now.
In the editor's edit mode a trail draws only while its GameObject (or a parent) is dragged with
the gizmo, cleared on release.

## Curves and gradients

`widthCurve` is a `Curve` and `colorGradient` a `Gradient`: S27's shared types (Unity's
`AnimationCurve` and `Gradient`; the particle modules, tweens and force fields use them too),
in S27's one form in scene files, `set_properties` and `get_entity`
(docs/specs/S27-editor-tools.md):

- **Curve**: `{"keys": [[time, value, inTangent, outTangent(, mode)], ...], "preWrapMode": "clamp",
  "postWrapMode": "clamp"}`. Tangents are slopes (value per time); `null` is a step (the
  segment holds the key before it). Wraps: `clamp`, `loop`, `pingPong`, `linear`,
  `loopWithOffset`. Writes also take a number (a constant curve) or the keys array alone
  (`[[time, value], ...]`: flat tangents). Between two keys the value is the cubic Hermite of
  their tangents (Unity's non-weighted keys).
- **Gradient**: `{"colorKeys": [[time, r, g, b], ...], "alphaKeys": [[time, alpha], ...], "mode":
  "blend"}`: up to 8 keys each, times 0..1, colours sRGB (as pickers show them). Blending mixes
  two colour keys in sRGB like Unity (a red-to-blue gradient is (0.5, 0, 0.5) sRGB halfway), so
  gradients copied from Unity look the same; alpha mixes linearly; `"linear": true` blends in
  linear space instead. `mode` `fixed` steps to the first key at or after t. More than 8 keys:
  `set_properties` and Lua (`setKeys`, `colorKeys`, `alphaKeys`) refuse; a scene file with more
  loads the first 7 and the last, with a warning. No keys: white, opaque.
- **origin/main's form** reads too (its files, and what older scripts send):
  `{"keys": [[time, value(, in, out)]], "pre", "post"}` curves (`"inf"` / `.inf` steps,
  `ping_pong` / `repeat` wraps) and `{"mode", "colors": [[time, "#RRGGBB" | [r, g, b] linear]],
  "alphas"}` gradients, one colour (`"#RRGGBBAA"`, `[r, g, b, a]` linear) for a constant one;
  scenes saved by it are rewritten in S27's form when they load (scene format 5 -> 6, prefab 6 -> 7),
  and `get_entity` answers in S27's form.
- `set_shape_points` with `colors` on a Line Renderer sets `colorGradient` (one key per colour,
  evenly over the length; linear colours, as the tool takes them, stored sRGB).

In the Inspector both are fields with an inline preview that opens an editor: drag keys,
double-click to add, Delete to remove, tangent handles and Auto / Linear / Constant / Free per key,
presets; the gradient editor has alpha markers above the bar and colour markers below.

In Lua component values of these types are **copies** (Unity semantics): edit, then assign back.

```lua
local line = LineRenderer(scene, entity)
local curve = line.widthCurve
curve:addKey(0.5, 2.0)              -- 1-based index of the new key, -1 if a key is at 0.5
line.widthCurve = curve             -- assign the copy back
local g = Gradient()
g:setKeys({{0, Vector3(1, 0.6, 0)}, {1, Vector3(1, 0.6, 0)}}, {{0, 1}, {1, 0}})  -- sRGB colours; false (and a warning) past 8 keys
line.colorGradient = g
-- origin/main's script names work: AnimationCurve.constant(v), CurveKey (Keyframe), CurveWrap,
-- curve:addKey(t, v, inTangent, outTangent), curve.keys, g:colorKeyCount(), g:getColorKey(i)
```

## Recipes

**Laser / lightning (B scene of the Unity game)**: a 2-point world-space line, width 0.35, an
animated sprite sheet (8 rows), additive.

```json
{"tool": "create_entity", "arguments": {"type": "line_renderer", "name": "Lightning"}}
{"tool": "set_properties", "arguments": {"entity": "Lightning", "values": {"linerenderer": {
  "widthMultiplier": 0.35, "customShader": "library:line_flipbook",
  "shaderTextures": {"u_baseTexture": "textures/lightning.png"},
  "shaderUniforms": {"columns": 1, "rows": 8, "fps": 30}}}}}
```

```lua
-- every frame: from the cannon to the target (setPosition is 0-based, like Unity)
function Cannon:update()
    self.line:setPosition(0, self.head:getWorldPosition())
    self.line:setPosition(1, self.target)
end
```

`library:line_flipbook` (type `lines`): base map, `tint`, `columns`, `rows`, `fps`, `scroll`
(UV per second), the line's colour (`VERTEX_COLOR`, on), additive by default.

**Bullet trail (Invaders of the Unity game)**: on the bullet's child, time 0.3, width 0.223 -> 0,
orange fading out, a point every 0.1.

```json
{"tool": "create_entity", "arguments": {"type": "trail_renderer", "name": "Trail", "parent": "Bullet"}}
{"tool": "set_properties", "arguments": {"entity": "Trail", "values": {"trailrenderer": {
  "time": 0.3, "minVertexDistance": 0.1, "widthMultiplier": 1,
  "widthCurve": [[0, 0.223], [1, 0]],
  "colorGradient": {"colorKeys": [[0, 1, 0.588, 0], [1, 1, 0.588, 0]], "alphaKeys": [[0, 1], [1, 0]]}}}}}
```

**2D rope**: a 20-point polyline in a pixel-unit scene, width 6, round corners (`numCornerVertices`
4); it sorts with sprites by the hierarchy order in a 2D camera.

**Pixel-wide debug lines**: `widthMultiplier` 2, `widthUnit` `pixels`: 2 pixels at any distance.

## Reading back

`get_entity` lists every field and adds `ribbon` for a Line Renderer (`vertices`, `indices`,
`length`, `loaded`; a hairline `{"hairline": true, "segments": n}`) and `trail` for a Trail
Renderer (`points`, `vertices`, `length`; moving in Play). Check the look with `screenshot`.

## Shaders

Both draw with the built-in `lines` shader or a `lines` library shader / fork (`customShader`, an
asset reference with its GUID on both: a renamed fork is followed, and Project > Fix references
lists a missing one; `list_shaders type=lines`). Its variants: the hairline (`Vc4`), the ribbon (`Rib`), its base
texture (`Tex`), pixel widths (`Pxw`). A library or forked lines shader includes
`includes/lines_vs.glsl` in its .vert (the hairline and the ribbon) and
`includes/lines_fs.glsl` in its .frag (`lineVertexColor()`, `lineUV()`); a fork made before widths
draws the hairline only (copy the include into it, or fork again). jasper://docs/shaders.

## Costs (mobile)

- CPU: a line is rebuilt only when it changes (a 100-point polyline with 4 corner vertices is
  about 1,200 vertices); a moving trail rebuilds every frame, at a cost that grows with its point
  count (about Time / frame time with a Min Vertex Distance of 0): for many trails keep Time short
  and Min Vertex Distance above 0, and corner and cap vertices low. 50 trails of 64 points took
  about 0.6 ms of CPU per frame on a busy desktop build machine (not measured on a phone).
  Buffers grow by powers of two and shrink after 5 s at under a quarter.
- GPU: one draw per line or trail; the widths are expanded in the vertex shader; fill is the
  ribbon's area. Thick lines and trails are culled by their bounds (pixel widths are not).
