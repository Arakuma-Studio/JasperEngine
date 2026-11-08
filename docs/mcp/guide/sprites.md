# Sprites: sheets, frames and Aseprite files

A Sprite shows frames of a **sheet**: an `.aseprite` file's import, a texture (one frame, or the
frames of its **Sprite Mode**), or a **Sprite Atlas** (`.spriteatlas`). Old sprites may still carry
**inline frames** of their texture (legacy). `set_sprite_frames` sets a Sprite's frames;
`set_sprite_import_settings`, `edit_sprite_atlas` and `get_sprite_atlas` are in group `sprites`.
People have the same things as the Sprite's **Sprite** field, the texture importer's **Sprite
Mode**, the **Aseprite Importer** and the **Sprite Atlas** Inspector of a selected file, and
**Window > 2D > Sprite Preview**.

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
| `sheet_conflict` | `convert`: the texture already has another Sprite Mode; `fix` says to keep the sprite inline |
| `frame_too_large` | an atlas frame does not fit `max_size` (`field`: the frame) |
| `missing_member`, `double_packed` | an atlas member is gone; a source is packed by two atlases in use |
| `atlas_version` | the `.spriteatlas` was written by a newer editor (read-only) |
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
  `offset`, `padding`, `count`, `names`, `prefix` (default `frame_`), `sheet_size` when the image is unreadable) or
  `frames` (pixel rects `[x, y, w, h]` from the image's top-left). They write the texture's
  Sprite Mode (Multiple) and make the Sprite show one of its frames, so every sprite of that texture
  can use them; `inline: true` keeps them in the Sprite instead (legacy).
- `convert: true` moves a legacy Sprite's inline frames into its texture's Sprite Mode (names and
  order kept, so clip keys and Sprite Animations keep their indices); `all_users: true` converts
  every sprite of the project using that texture with the same frames (open and closed scenes,
  prefabs). One `undo {scope: "project"}` restores all of it.
- `frame` (default 0) picks the frame shown; `resize` (default true) sizes the Sprite to it (`0` width and
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

## Sheets: a texture's Sprite Mode

A texture is a sheet of its own frames when its import settings say so (`import.spriteSheet` in
its meta), with no atlas needed:

| Mode | Frames |
|---|---|
| `single` (default) | the whole image, named by its stem |
| `grid` | `cell [w, h]`, `offset [x, y]`, `spacing [x, y]`, `count` (0: all), `skip_empty` (default true), `prefix` |
| `multiple` | `frames: [{name, rect: [x, y, w, h], pivot?: [x, y], border?: [l, t, r, b]}]` |

`pivot` is the default of frames without their own: a preset (`bottom_center`, ...) or `[x, y]`
normalized from the bottom-left. A file named `name@WxH.png` (`coin@16x16.png`) is a grid of
`W x H` cells named `coin_0`, `coin_1`... without anything written. `get_asset_info` on a texture
reports `sprite_mode` (the settings, where they come from, the frames) and `packed_in`. A frame
with a `border` can be drawn **Sliced** (`set_properties` `{component: "sprite", property: "drawMode", value: "sliced"}` with a
`width` and `height`): corners keep their size, edges and centre stretch.

## Atlases

A `.spriteatlas` packs textures, `.aseprite` files and folders of them into a few shared pages,
lossless for pixel art. Sprites keep referencing their source (`sheet: assets/items/sword.png`):
once an atlas in use packs it, they draw it from the atlas page, with the same frame names, order,
sizes and pivots. Key clips on the source sheet, not the atlas. Load an atlas by path in Lua:
`SpriteSheet.load("ui/icons.spriteatlas"):getFrame("sword")`.

1. `edit_sprite_atlas {path, create: true, add: ["assets/items", "assets/sprites/hero.aseprite"]}`
   without `output`: a pack in memory (pages, frames, usage, duplicates, warnings, page images
   unless `include_pages: false`). Nothing is written.
2. The same arguments with `output: true`: the file is written (one project undo step) and built.
3. `get_sprite_atlas {path}` reads the build: pages (size, frames, `used_percent`, VRAM), frames
   (`search` by name, `member:<path>`, `page:<n>`; `limit`, `page`), warnings, `build_state`. A
   texture or `.aseprite` path gives the atlas that packs it and its placements.

`remove` drops a member, or excludes a file inside a folder member. `settings` (partial; with
`platform` only that family's `max_size`): `max_size` 32-8192 (2048), `padding` 0-16 (1),
`extrude` 0-4 (1), `trim` (true; bordered frames never), `keep_members_together` (true),
`power_of_two`, `square` (false), `compression` `auto` (lossless when a member is an `.aseprite`
file, a lossless or Sprite-Mode texture) / `lossless` / `platform_default`, `filter` `auto` /
`point` / `bilinear` (bilinear extrudes at least 2), `include_in_build` (true).

Reading waste: `usage_percent` is the trimmed frame area over the page area; `worst_members` lists
the members with the most transparent pixels trimmed away; a page far below the others is a
member that would fit elsewhere with a larger `max_size` or `keep_members_together: false`.

Project setting `sprite_atlas.mode` (Project Settings > 2D): `enabled` (the editor, Play and
builds draw from atlases), `enabled_for_builds` (only builds), `disabled`. Exports build each atlas
for their platform family (`max_size` overrides) and leave out a packed texture that only sprites
read.

## Legacy inline frames

`jasperengine-editor check --project P --sprites` lists the sprites that still carry inline
frames and the atlas problems (exit 1 on an atlas error). Convert them with
`set_sprite_frames {entity, convert: true, all_users: true}`, the Inspector's **Convert to Sprite
Sheet** or **Assets > Convert Inline Sprite Frames...**.
