# Pixel art

How to make pixel art draw correctly, and prove it, for agents. User docs: `docs/pixel-art.md`.
Palettes (edit_palette, apply_palette): jasper://docs/palettes. Bitmap fonts (preview_font): jasper://docs/pixel-fonts.
Cleaning AI or loose pixel art, craft lints (fix_pixel_art, lint_pixel_art): jasper://docs/pixel-art-cleanup.
Sprite effects and outline presets, screen transitions, and the contact sheets whose lint checks them (capture_contact_sheet): jasper://docs/pixel-fx.

## The rules

- Pick one base resolution and draw everything into it. 320x180 divides every common 16:9 screen
  exactly (4x at 720p, 6x at 1080p, 8x at 1440p, 12x at 4K). The base resolution is the project
  canvas.
- Scale the base image to the screen by a whole number only; bars fill what is left, or Integer
  Expand shows more of the world instead.
- Sample pixel art with point filtering, without mipmaps, stored uncompressed: block compression
  shifts colours and smears edges.
- One pixel size for everything: a texel is a base pixel, so every sprite keeps the project's
  Pixels Per Unit and a scale of 1.
- The camera and every sprite sit on whole base pixels. Smooth scrolling moves the finished image
  by screen pixels, never the art inside it.
- Rotate by 90 degrees, or draw the rotations; only what must spin rotates on the grid.
- Text uses pixel fonts at their native size (or a whole multiple), never antialiased.
- Particles and effects share the grid: whole-pixel sizes, point-filtered textures, snapped.
- No smoothing (FXAA, MSAA, a full-frame blur) over the pixel-art image.
- Look at every new asset at 1x next to a tile before it goes in.

## Set up

- A new project: `create_project {template: "2d-pixel-art"}` (Pixel Art Mode on, 320x180, a Pixel
  Perfect Camera, point-filtered lossless art).
- An existing one: `check_pixel_art {fix: ["pixel_art_mode"]}` previews the preset's project
  settings; the same call with `output: true` applies them. Then `check_pixel_art {fix: ["all"]}`
  converts cameras, textures and references the same way.
- `set_project_settings {"pixel_art.enabled": true}` alone only changes what new things get (new
  sprite images import as pixel art, new 2D scenes get a Pixel Perfect Camera, the check runs on import
  and at Play); it changes no other setting.
- Mark a texture: `set_texture_import_settings {paths, settings: {pixel_art: true}}` (also
  `filter_mode`, `pixels_per_unit`, `alpha_clip`).

## The loop

1. Import or draw the art, then `check_pixel_art {paths: [...]}`.
2. Fix: the same call with `fix` previews every change; add `output: true` to apply them as one
   undo step (the result checks again). `fix` takes finding keys, `F<n>`, rule ids or `"all"`; a
   second option is `"<key>@<option>"`.
3. Play, `configure_game_view {pixel_scale: 1}` and `screenshot` (one base pixel per screen pixel),
   or `check_pixel_art {image: true}`: the 1x frame with each finding boxed and labelled `F<n>`.
4. Report the verdict and the counts. A finding with `needs_person` has no automatic fix: hand it
   over with its reason; never change a scale to hide a texel_size finding.

The CLI runs the same rules on the saved project: `jasperengine-editor check --pixel-art`.

## Rule reference

| Rule | Measures | Fix |
|---|---|---|
| `pixel_art_mode` | the preset's project settings that differ | the preset's project settings |
| `integer_scale` | per Check Screen: scale, integer, bars; a scene without a Pixel Perfect Camera | add or enable a Pixel Perfect Camera |
| `filtering` | a reference's min/mag filters | both `nearest` |
| `mipmaps_compression` | mipmaps, format per platform, resize | `pixelArt: true` in the import settings |
| `palette` | pixels off the Default Palette (or the Palette Swap source) | apply the palette; lost colour groups need a person |
| `texel_size` | base pixels per texel | scale 1, the project's PPU, or the UI image's texture size |
| `off_grid` | the drawn corner's fraction of a base pixel | move onto whole pixels |
| `rotation` | angles off 90 degrees, a rolled camera | round the angle, or `@on_grid` (Rotate On Grid) |
| `font_antialiasing` | vector or bitmap font, size, alpha | font size; vector fonts need a person |
| `particles_grid` | filter, render mode, sizes, snapping, the Pixel Art module | Default Filter Mode point, the camera's snapping, the Pixel Art module |
| `anti_aliasing` | FXAA, MSAA, blur | turn them off |
| `render_scale` | Device Tier and scene render scales below 1 | render scale 1 |
| `sprite_variants` | library:sprite shader variants (budget 24) | needs a person |

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

## Pitfalls

- Pixels Per Unit above 1 with UI in the same scene: UI stays in camera units. Put the HUD in a
  layer scene.
- A texel_size finding means the art or its Pixels Per Unit is wrong: fix those, not the scale.
- `check_pixel_art` reads the live editor (unsaved edits included); the CLI reads the saved files.
  Scene fixes are unsaved until `save_scene`.
