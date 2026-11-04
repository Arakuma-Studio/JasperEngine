# Pixel art

How to make pixel art draw correctly, for agents. User docs: `docs/pixel-art.md`.

## Camera and snapping

- **Pixel Perfect Camera** (component `pixelperfectcamera`, on the camera GameObject; add it with
  `add_component`, edit it with `set_properties`): one base resolution (`referenceWidth`,
  `referenceHeight`; 0 x 0 = the project canvas) drawn at a whole-number scale k with bars around.
  It does nothing on a perspective camera. Add it to the scene's main camera.
- `upscaleMode`: `integer` (bars), `integer_expand` (the base grows to fill the screen, bars under k
  pixels), `stretch_fill` (the integer image stretched, sharp at its seams).
- `upscaleRenderTexture` (default true): the base image is drawn into a target and scaled; false
  draws straight at the screen's resolution on the same grid (rotations then show mixels).
- `pixelSnapping` (default true): the camera and every sprite, tilemap, UI image, text and particle
  land on whole base pixels, with one rule shared by the CPU and the shaders. Never round
  positions in a script to fix shimmer: the rule already does it.
- `rotationMode`: `steps_90` (default; a rotated sprite is drawn at the nearest 90 degrees, its
  Transform keeps the angle) or `on_grid` (the angle is kept, the pivot snaps). A Sprite's
  `pixelRotation` (`camera`, `steps_90`, `on_grid`) overrides it.
- `subpixelSmoothing`: smooth scrolling by whole screen pixels; the art stays on its grid.
- `letterboxColor`: the bars.
- Read-only, from `get_entity` (for the Game view's size) and `get_runtime_entity` (live):
  `pixelRatio` (k), `baseWidth`/`baseHeight`, `targetWidth`/`targetHeight`, `viewport` (screen
  pixels), `cropped` (the screen is smaller than the reference), `snappedPosition`,
  `subpixelOffset`.
- **Pixels Per Unit**: `set_project_settings {"graphics.pixels_per_unit": 16}` (default 1: a world
  unit is a pixel). A Sprite's or Tilemap's `pixelsPerUnit` (0 = the project's) divides its pixel
  size; `width`/`height` stay pixels. Keep it 1 unless the game uses Unity-style tile units; with
  more than 1, put the HUD in a layer scene (UI stays in camera units).
- Without a component, `graphics.scaling_mode` `integer` or `integer_expand` scales the canvas by
  whole numbers.
- **Check at 1x**: `configure_game_view {"pixel_scale": 1}` (0 = Auto, the largest that fits; up
  to 8), then `screenshot` of the Game view: one base pixel per screen pixel.
- Scene view: `set_scene_camera {"display": {"pixel_zoom": true}}` zooms in whole steps (1/8x to
  32x); `pixel_grid` (lines on art pixels from 6x), `pixel_snap_preview` (sprites snapped as the
  game draws them) and `pixel_move_snap` (the Move and Rect tools on whole pixels) are on by
  default in a scene whose main camera has a Pixel Perfect Camera.
