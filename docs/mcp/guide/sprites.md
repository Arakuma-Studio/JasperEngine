# Sprites: sheets, frames and Aseprite files

A Sprite shows frames of a **sheet** (an `.aseprite` file's import, or a texture: one frame), or
the inline frames of its texture (the Texture Slicer). `set_sprite_frames` sets either kind;
`set_sprite_import_settings` (group `sprites`) changes how an `.aseprite` file imports. People
have the same things as the Sprite's **Sprite Sheet** field, the **Aseprite Importer** Inspector
of a selected `.aseprite` file and **Window > 2D > Sprite Preview**.

## Import

An `.aseprite` / `.ase` file in the assets is imported on a background job whenever it changes
(a save from Aseprite, `set_sprite_import_settings {output: true}`, a reimport). The file never
ships: exports carry its import under `assets/je-imported/`.

### Outputs and reference strings

`get_asset_info` on the file gives the summary (frames, layers, tags, slices, no cels), the
stored and resolved settings, the outputs with their GUIDs and reference strings, the warnings
and the cache state. Use the reference strings as given:

| Output | Reference | Used by |
|---|---|---|
| the sheet | `assets/sprites/hero.aseprite` | a Sprite's `sheet`, `SpriteSheet.load` |
| a frame | index `3` or name `hero 3` (`Body/3` in `layers_as_sprites`) | `set_sprite_frames {frame}`, `Sprite.frame`, `Sprite.frameName` |
| a tag's clip | `assets/sprites/hero.aseprite#walk` | Animation Player entries, `get_animation`, `create {from}` |
| the clip of a file without tags | `assets/sprites/hero.aseprite#hero` | as above |
| a slice | its name (`hurt`) | a 2D collider's `slice`, `Sprite:getSlice` |

Tag clips are read-only: `edit_animation` on one fails with `read_only_clip`. To change one,
extract it: `edit_animation {clip: "assets/anim/walk.anim", create: {from: "assets/sprites/hero.aseprite#walk"}}`.

### Settings keys

`set_sprite_import_settings {paths, settings}` merges `settings` into each file's current ones
(a partial update; unknown keys fail with `invalid_settings` naming the key):

| Key | Values (default) |
|---|---|
| `import_mode` | `sprite_sheet` (layers flattened) or `layers_as_sprites` (frames per top-level layer, one child Sprite each) |
| `layers.include_hidden` | `false` |
| `layers.ignore_prefixes` | names starting with these are left out (`[]`) |
| `layers.include`, `layers.exclude` | layer names or paths (`[]`: by the rules above); an unknown one fails with `unknown_layer` listing the layers |
| `pixels_per_unit` | `"project"` or a number |
| `pivot.source` | `slice` (a slice named `pivot`, else the first with a pivot, else the preset), `preset` or `custom` |
| `pivot.slice`, `pivot.preset`, `pivot.custom` | `""`, `bottom_center`, `[0.5, 0.0]` |
| `border.slice` | `""` (the first 9-patch slice) or `"none"` |
| `trim` | `true` (alpha 0 only) |
| `clips.generate`, `clips.tags`, `clips.events.layer` | `true`, `[]` (all), `events` |
| `texture.layout` | `packed` or `grid` |
| `texture.padding`, `texture.extrude`, `texture.max_size` | `1`, `1`, `2048` |
| `texture.compression` | `none` (lossless) or `project` (the project's sprite texture rules) |

### The loop

1. `set_sprite_import_settings {paths, settings}` without `output`: a preview (frames, pages and
   their sizes, clips with lengths, slices, warnings; `include_preview` adds a contact sheet).
   Nothing is written.
2. The same arguments with `output: true`: the metas are written as one undo step
   (`undo {scope: "project"}`) and the files are imported again; the result has the outputs.
3. When a person saves the file in Aseprite, the editor imports it again by itself: read
   `get_asset_info` again for the new outputs. The next call's `external_changes.problems` lists
   what the import warned about.

`view_asset` on the file shows the imported frames as a contact sheet with frame indices and tag
bands. `get_animation {clip: "assets/sprites/hero.aseprite#walk"}` reads a tag clip's keys.

### Errors

Every error has `kind`, `field`, `message` and `fix`:

| Kind | Meaning |
|---|---|
| `not_aseprite`, `truncated`, `corrupt_chunk`, `too_large`, `unsupported_depth` | the file cannot be read (`field`: the chunk and offset) |
| `unknown_layer`, `unknown_tag`, `unknown_slice` | a setting names something the file lacks; `fix` lists what it has |
| `invalid_settings` | a key or value is wrong (`field`: the key) |
| `read_only_clip` | `edit_animation` on a tag clip |
| `unknown_frame`, `not_a_sheet` | `set_sprite_frames` named a frame or sheet that is not there |
| `import_failed` | anything else; the message says what |

## Sprites from sheets: `set_sprite_frames`

- `sheet`: an `.aseprite` file or a texture. The Sprite takes its frames from it (its inline
  frames and texture are then unused). `frame` is an index or a frame name. With an `.aseprite`
  sheet, `clips` (default `true`) lists the tag clips on the GameObject's Animation Player (added
  when missing, Default Clip a tag named `idle`, else the first, Play Automatically on).
- `create` instead of `entity` makes the GameObject; with an `.aseprite` sheet it also gets the
  file's sprite properties (user properties named `<component>.<property>`, such as
  `light2d.intensity`), as dropping the file in the Scene view does.
- Without `sheet`: `texture` with `slice` (row-major cells: `tile_size`, `columns`, `rows`,
  `offset`, `padding`, `count`, `names`, `prefix`, `sheet_size` when the image is unreadable) or
  `frames` (pixel rects `[x, y, w, h]` from the image's top-left).
- `frame` picks the frame shown; `resize` (default true) sizes the Sprite to it (`0` width and
  height for a sheet: the frame's own size); `size` sets it; `pivot` takes a preset or `frame` (the
  frame's own pivot).
- Animate: an Animation Player with the tag clips, `set_sprite_animation`, or scripts:
  `Sprite:setFrame(index or name)`, `Sprite:playTag("walk")`.

## Slice colliders

A Rigidbody 2D collider with `slice: "<name>"` is that slice's box on every frame of a sheet
Sprite (rebuilt when the box changes) and has no shape on frames where the slice is hidden. The
Sprite Inspector's **Create Colliders from Slices** adds one per slice (Is Trigger when the slice's
user data has `sensor: true`). Over MCP set the collider list with `set_properties`
(`rigidbody2d.colliders`) giving each collider a `slice`.
