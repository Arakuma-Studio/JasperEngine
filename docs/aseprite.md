# Aseprite files

Save `.aseprite` (or `.ase`) files into the project's assets and use them as they are: the editor
imports each one into a sprite sheet (packed pages and frames with pivots and 9-slice borders),
one Animation Clip per tag, named slices and the file's user data. Saving the file again in
Aseprite updates every sprite, clip and running preview within a moment. Exports ship the import
only, never the `.aseprite` file. Agents have the same through MCP (`jasper://docs/sprites`).

## Using a file

- **Drop it on the Scene view or the Hierarchy**: a GameObject named after the file with a Sprite
  showing frame 0 (pivot: the frame's own), an Animation Player listing the tag clips (Default
  Clip: a tag named `idle`, any case, else the first; Play Automatically on), the file's sprite
  properties applied, and in **Individual Layers** mode one child Sprite per layer. One undo step.
  Dropped on an existing Sprite it replaces its sheet; on the Animation window or a GameObject
  with an Animation Player it adds the tag clips.
- **Sprite > Sprite Sheet** in the Inspector: any Sprite can show a sheet (an `.aseprite` file or a
  texture). **Frame** picks a frame by name; Width and Height 0 draw the frame at its own size
  (pixels per unit: the import's). The inline Sprite Frames are not used while a sheet is set.
- **Open in Aseprite** (the Sprite Inspector, the Aseprite Importer, the Project window's context
  menu, double-click): opens the file in Aseprite. The editor finds Aseprite in the usual places
  (Steam, the installers, `PATH`, `/Applications`, `/opt`) or where **Preferences > External
  Tools > Aseprite** says (**Detect**, **Browse…**); when there is none it says so, offers
  **Locate…**, and double-click opens the Sprite Preview instead.
- **Window > 2D > Sprite Preview** plays the file read-only: tags, frame stepping (`,` `.`),
  onion skin, layers, slices, pivots, trim, the source next to what the game draws, and **Compare
  at 1×** against another sheet's frame.

## Import settings

Select the file in the Project window: the **Aseprite Importer** Inspector edits its settings
(several files at once; Apply / Revert; one undo step). They live in the file's `.meta`:

```yaml
import:
  asepriteImporter:
    version: 1
    importMode: sprite_sheet          # sprite_sheet (Merge Frames) | layers_as_sprites (Individual Layers)
    layers:
      includeHidden: false
      ignorePrefixes: []              # layers whose names start with these are left out
      include: []                     # names or paths ("Body/Arm"); empty: the rules above
      exclude: []
    pixelsPerUnit: project            # the project's value, or a number
    pivot: {source: slice, slice: "", preset: bottom_center, custom: [0.5, 0.0]}
    border: {slice: ""}               # the 9-patch slice ("" = the first; "none")
    trim: true                        # transparent borders cut (alpha 0 only)
    clips: {generate: true, tags: [], events: {layer: events}}
    texture: {layout: packed, padding: 1, extrude: 1, maxSize: 2048, compression: none}
    palette: {asset: true}
```

- Layers: hidden layers, reference layers and layers under a hidden group are left out unless
  `includeHidden`; `include` / `exclude` name layers or groups. Blend modes, layer and cel
  opacity, groups and cel z-index are composed as Aseprite draws them.
- Pivot `source: slice` uses a slice named `pivot` (else the first slice with a pivot, else the
  preset); `custom` is a normalized point (0, 0 bottom-left).
- `texture.compression: none` keeps the pages lossless (pixel art); `project` cooks them with the
  project's sprite texture rules (S14).
- A newer `version` than this editor knows is shown read-only and imported with the keys it knows;
  unknown keys are kept.

## What an import makes

| Output | Reference | Notes |
|---|---|---|
| the sheet | `sprites/hero.aseprite` | frames `hero 0`, `hero 1`… (`Body/0`… per layer in Individual Layers) |
| a tag's clip | `sprites/hero.aseprite#walk` | read-only; direction and repeat unrolled; `loop` when repeat is 0, else `once` |
| the clip of a file without tags | `sprites/hero.aseprite#hero` | all frames |
| slices | by name | per-frame keys, 9-patch centre, pivot, user data |
| palette, tilesets | `#palette#0`, `#tileset:<name>` | for the palette and tileset tools |

Every output has a stable GUID: renaming the file, adding frames or renaming a tag keeps the
references. Text in cel user data on a layer named `events` (the `clips.events.layer` setting)
becomes a clip event at that frame (`onFootstep("left")`).

To edit a tag's clip, extract it (**Extract Clips…** in the importer, or the Animation window's
Extract to .anim) and play the `.anim` instead.

## Slices and user data

- A Rigidbody 2D collider whose **Slice** names a slice is that slice's box on every frame of the
  sprite (rebuilt only when the box changes) and has no shape on frames where the slice is hidden.
  **Create Colliders from Slices** (Sprite Inspector) adds one per slice, Is Trigger when the
  slice's user properties have `sensor: true`.
- The sprite's, tags', slices' and layers' user data (text, colour, user properties) are in the
  sheet and readable from Lua. Sprite user properties named `<component>.<property>` (for example
  `light2d.intensity`) are set on the GameObject by a drop and by **Apply Sprite Properties**.

## Lua

`Sprite`: `sheet` (a path; setting it shows frame 0), `frame` (0-based), `frameName`,
`frameCount`, `getSheet()`, `setFrame(index or name)`, `playTag(name, speed)` (the tag at the
sheet's durations, its direction and repeat; no clip events: the Animation Player has them),
`stopTag()`, `tag`, `isTagPlaying`, `getTag(name)`, `getTags()`, `getSlice(name)` (on the shown
frame, in the GameObject's local units), `getSlices()`, `getUserData()`. `SpriteSheet.load(path)`:
`frameCount`, `pageCount`, `frameNames`, `tags`, `slices`, `properties`, `findFrame(name)`,
`getFrame(nameOrIndex)`, `getTag(name)`, `getSlice(name, frame)` in pixels.
`jasper://docs/lua-api` has an example.

## Command line

```sh
jasperengine-editor aseprite info hero.aseprite [--json] [--cels]
jasperengine-editor aseprite render hero.aseprite out.png [--frame N | --tag walk] [--layers Body,Head] [--sheet horizontal|rows] [--scale 4]
jasperengine-editor aseprite resave hero.aseprite copy.aseprite [--expect-identical]
```

`render` draws as the importer does (`--flatten-groups` as Aseprite's own `--sheet` export does);
`resave` writes the file back byte for byte when nothing changed.

## Files the editor reads and writes

The codec reads every file Aseprite 1.0 to 1.3.x writes: RGBA, grayscale and indexed sprites,
layers and groups with every blend mode, linked cels, tilemaps and tilesets, tags (all
directions, repeat), slices, user data with all property types, colour profiles, external file
references and the old palette chunks; chunks it does not know are kept in place. An untouched
file is written back byte for byte, and an edit changes only the chunks it touches. The files it
writes open in the Aseprite version their features need:

| Feature | Aseprite |
|---|---|
| layers, groups, cels, tags, slices, palettes | 1.2 |
| tilemaps and tilesets, layer UUIDs, cel z-index, user data on tags and the sprite, user properties | 1.3 |

Limits (larger files fail with `too_large` instead of exhausting memory): canvas 16384 px a side,
65,535 frames, 1 GiB of decoded pixels, 64 MiB of palettes (frame 0's and one per change), groups
nested 128 deep.

## Agents drawing on your sprites

An agent connected through MCP can draw on the same `.aseprite` file you have open in Aseprite
(`create_sprite`, `read_sprite_pixels`, `edit_sprite`, `render_sprite`, `diff_sprite`, group
`sprites`; the agent guide is `jasper://docs/sprites`). There is no painting canvas in the editor:
Aseprite stays the tool for people.

- Each agent batch is one step of the project's undo history, named after the agent ("Claude Code:
  Edit Sprite (hero.aseprite: line, fill ×2)"). Ctrl+Z undoes it.
- **Window > 2D > Sprite Preview** shows every stroke as it lands: the changed pixels are outlined
  for 3 seconds (**Preferences > Pixel Art > Highlight Agent Edits**), and a line above the canvas
  says who edited it and when, with **Undo** while that step is the newest.
- The Aseprite Importer in the Inspector shows "Last edited by ..." while this editor remembers it.
- **Reopen a file in Aseprite after an agent has edited it.** Aseprite keeps its own copy in memory
  and does not reload: if you save it there, your save wins and the agent's newer edits are gone
  from the file. The Console then warns which agent steps were overwritten, those steps can no
  longer be undone (greyed in the Undo History, with the reason in their tooltip), and the agent's
  next edit is told to read the file again.

## What is verified

The codec's tests (`editor/tests/pixelart/`) render every frame of the fixture corpus
(`editor/tests/fixtures/aseprite/README.md`) against Aseprite's own PNG exports pixel for pixel,
including a corpus made with Aseprite 1.3.18 for group opacity and blend, layer × cel opacity,
composed nested groups, tilemap flips, user data and indexed sprites with a background; compare
`describe` with Aseprite's `--data` for every fixture; re-save every fixture byte for byte;
round-trip documents built in code; and fuzz the reader with malformed files under the address
sanitizer, bounding what it allocates. `tools/pixelart/aseprite_compat.py` (needs
`JASPER_ASEPRITE`) checks the other direction: Aseprite opens the files the writer makes and
exports the same frames.
