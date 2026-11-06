# Pixel art

JasperEngine draws pixel art correctly when you ask it to: one base resolution scaled to the screen
by a whole number, one pixel size for the whole world, and every sprite, tile, glyph and particle on
whole pixels. This page covers the Pixel Perfect Camera, Pixels Per Unit, the pixel-snap rule, the
rotation policy, sub-pixel smoothing and the editor's pixel views. The design and its acceptance
record are in `docs/specs/S41-pixel-perfect-camera.md`.

## Pixel Perfect Camera

Add it to the main camera of your scene: Inspector, Add Component > Rendering > Pixel Perfect
Camera (it is offered only on GameObjects with a Camera). On a perspective camera it does nothing
and the Inspector says so.

| Field | What it does |
|---|---|
| Reference Resolution | The base resolution the art is drawn at, for example 320 x 180. Use Project Canvas (the default) takes Project Settings > Graphics > Canvas Width and Height. |
| Upscale Mode | **Integer (Letterbox)**: every base pixel is k x k screen pixels, k the largest whole number that fits; bars fill the rest. **Integer (Expand)**: the same k, and the base grows to fill the screen (bars under k pixels): wide screens see more of the world. **Stretch Fill**: the integer image stretched to the largest fit of its aspect, with a sharp bilinear filter: each base pixel's interior is one flat colour and only the screen pixels on its seams blend. |
| Upscale Render Texture | On (default): the base image is drawn into a target of its own and scaled by the blit, so rotated sprites stay on base pixels. Off: drawn straight at the screen's resolution on the same grid (cheaper; rotations show finer pixels). |
| Pixel Snapping | The camera and every sprite, tilemap, UI image, nine-slice, text and particle drawn by it land on whole base pixels. |
| Rotation | **90 Degree Steps** (default): a rotated sprite is drawn at the nearest multiple of 90 degrees; its Transform, physics and scripts keep the real angle. **Rotate On Grid**: the angle is kept and the pivot snaps; with the render texture the rotation is a nearest-neighbour rotation on the base grid. |
| Subpixel Smoothing | The camera stays on whole pixels, and its remainder moves the final image by whole screen pixels: scrolling is smooth while every base pixel stays k x k. Input follows the moved image. |
| Letterbox Color | The bars' colour. |

The Inspector shows the **Current Pixel Ratio** for the Game view's size ("4x - 320x180 in
1280x720, bars 0x0"; the live one in Play) and warns when the camera is not its scene's main
camera, when Rotate On Grid has no render texture, when it overrides the scene's Fixed Resolution
(the scene's setting is kept and unused while the component drives the camera) and when Pixels Per
Unit is above 1 with UI in the same scene.

While it drives the main camera, the component sets the game's canvas to the base resolution and
its view to the integer viewport, so input, the Canvas Scaler (Follow Game Canvas) and layer scenes
follow it. A layer scene's camera with a Pixel Perfect Camera (a HUD) takes the main scene's
layout and draws directly on its grid. A render-to-texture camera with one snaps to its texture's
pixels.

Without a component, Project Settings > Graphics > Scaling Mode **Integer (Letterbox)** and
**Integer (Expand)** give the same whole-number scaling of the canvas to everything the game draws.

## Pixels Per Unit

Project Settings > Graphics > Pixels Per Unit (1 to 256, default 1): how many texels make one world
unit. At 1, a world unit is a pixel and moving 1 unit moves 1 pixel, as every scene made before.
Sprites and Tilemaps have their own Pixels Per Unit (0 = the project's): their quads are their pixel
sizes divided by it, while Width and Height stay in pixels. A texel is one base pixel on screen when
the drawable's Pixels Per Unit equals the project's.

Pixels Per Unit does not change 2D physics: Box2D keeps its 64 world units (points) per metre
(`PhysicsSystem` `pointsToMeterScale2D`), so at Pixels Per Unit 16 one metre is 64 units, 1,024
texels. Set the scene's points-to-meter scale yourself when a body should be one world unit per
metre.

Use a value above 1 for Unity-style tile units (16 texels per unit). UI drawn by the same camera
stays in camera units, so put the HUD in a layer scene then.

## The pixel-snap rule

One rounding, the same on the CPU (`engine/core/pixelart/PixelSnap.h`) and in every vertex stage
(`engine/shaderlib/includes/pixel_snap.glsl`): a position in base pixels of the drawing target
(x right, y up from its bottom-left corner) goes to `floor(p + 0.5 + 1/128)`. The small bias moves
the half-pixel positions a CENTER pivot of an odd size produces away from the CPU's and the GPU's
rounding differences, so both put such a sprite on the same pixel. Draws turned by 0, 90, 180 or
270 degrees (or flipped) snap every vertex; others snap their pivot. Tilemap tiles round their edges
to the texel grid before they are built, so neighbours share their edges exactly and never gap.

Library shaders get the rule through `includes/surface_vs.glsl` and `includes/ui_vs.glsl`; a vertex
shader of its own includes `includes/pixel_snap.glsl` and applies it last, after every displacement.

## The editor's pixel views

- **Game view, Pixel Scale** (Auto, 1x ... 8x; scales that do not fit are greyed out), shown when the
  scene's main camera has a Pixel Perfect Camera: it forces k. **1x** shows the base resolution at
  one physical pixel per pixel: check pixel art at 1x. The view draws its image at one framebuffer
  pixel per physical pixel, its Scale zooms by whole numbers with nearest sampling. The edit-mode
  Game view lays the image out as Play does.
- **Scene view, Pixel Zoom** (2D scenes with a Pixel Perfect Camera): the wheel steps through 1/8x,
  1/4x, 1/2x, 1x, 2x, 3x ... 32x at the cursor; the readout opens the list; **1x** (Alt+1) and
  **Game** (Alt+2, the Game view's k) jump there; panning keeps the pixel grid on whole screen
  pixels. From 6x the **Pixel Grid** draws the art pixels' edges. **Snap Preview** draws the
  sprites snapped as the game draws them. **Pixel Move Snap** makes the Move and Rect tools move
  and size on whole pixels; hold Ctrl to drag freely. The three are on by default and switch in
  the menu beside the Pixel Zoom button. The camera's frame shows the reference resolution, and in
  Integer (Expand) the frame the Game view's size shows (dashed).

## Scripting

```lua
local ppc = PixelPerfectCamera(self.scene, cameraEntity)
ppc.referenceResolution = Vector2(320, 180)
ppc.upscaleMode = PixelUpscaleMode.INTEGER_EXPAND
local onGrid = ppc:roundToPixel(Vector3(10.5, 4, 0))
local k = ppc.pixelRatio
```

The full API is in `docs/mcp/guide/lua-api.md` (PixelPerfectCamera, `Sprite.pixelsPerUnit`,
`Sprite.pixelRotation`, `Tilemap.pixelsPerUnit`, `Engine.getPixelsPerUnit()`).

## Agents

Agents use `add_component` and `set_properties` (component `pixelperfectcamera`),
`set_project_settings` (`graphics.pixels_per_unit`, `graphics.scaling_mode` `integer` and
`integer_expand`), `configure_game_view` (`pixel_scale`), `set_scene_camera` (`display`:
`pixel_zoom`, `pixel_grid`, `pixel_snap_preview`, `pixel_move_snap`) and `screenshot`. The agent
guide is `jasper://docs/pixel-art`.

## Platforms and cost

Everything runs in vertex and fragment shaders on every backend. On the web, while a Pixel Perfect
Camera or an integer Scaling is active, the canvas backing store is its CSS size times
`devicePixelRatio` and the canvas is drawn `image-rendering: pixelated`, so the browser never
resamples the integer image. A scene without a Pixel Perfect Camera and without an integer
Scaling draws exactly what it drew before: no variant, pass or target is added.
