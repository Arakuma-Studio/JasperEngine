# Sprites, sprite sheets and Sprite Atlases

A **Sprite** (Sprite Renderer) shows one frame of a **sheet**. A sheet is an asset:

- an Aseprite file (`.aseprite`, `.ase`; see [Aseprite import](aseprite.md));
- a texture: one frame (the whole image), or the frames of its **Sprite Mode**;
- a **Sprite Atlas** (`.spriteatlas`), which packs many textures and Aseprite files into a few
  shared pages.

A Sprite stores its sheet and its frame (an index and the frame's name: renaming or reordering a
sheet keeps the right frame). Sprites made before sheets existed may still carry their own frame
rectangles (**inline frames**, below); they keep working.

## The Sprite Inspector

| Field | Meaning |
|---|---|
| **Sprite** | the sheet: drop a texture, an `.aseprite` file or a `.spriteatlas` |
| **Draw Mode** | **Simple** stretches the frame over Width x Height; **Sliced** draws a frame with a border as 9 parts, corners at their own size, edges and centre stretched |
| **Native Size** | Width and Height 0: each frame at its own size |
| **Frame** | a dropdown with thumbnails and search, and the frame's index |
| **Pivot** | the nine presets, or **From Frame** (the frame's own pivot) |

A Sliced frame without a border draws Simple, and the Inspector says so.

## A texture's Sprite Mode

Select a texture: its importer has a **Sprite Mode** section.

- **Single**: the whole image is one frame.
- **Grid**: cells of a size, from an offset, with spacing; empty (fully transparent) cells are
  skipped. Frames are named `<prefix><i>`.
- **Multiple**: frames you list, each with a name, a rect, and optionally a pivot and a border.

**Slice...** opens the Texture Slicer on the texture: grid by cell count or cell size, offset,
padding, prefix, Skip Empty, the default pivot, a frame's own pivot and border, and **From JSON**,
which reads the frames from an Aseprite `--data` file next to the texture (`hero.json` beside
`hero.png`, json-hash or json-array, with 9-patch slices as borders). The Sprite Inspector's
**Slicer Tool** opens the same Slicer and also makes the Sprite show the texture's frames, in one
undo step.

A texture named `name@WxH.png` (`coin@16x16.png`) is a grid of W x H cells without any setting:
frames `coin_0`, `coin_1`, ... The importer shows "Grid from file name (16x16)". If the size does
not divide the image, the importer says how many pixels are left over.

## Sprite Atlases

**Assets > Create > 2D > Sprite Atlas** makes an empty atlas; **Sprite Atlas from Selection** (also
in the Project window's context menu) packs the selected textures and Aseprite files. The atlas
Inspector:

- **Objects for Packing**: files and folders (drop them on the Add button, or click it to add the
  Project window's selection), **Exclude** for paths inside folders, **Include in Build**.
- **Packing**: **Max Texture Size** with tabs for Default, Desktop, Mobile, Web and Android ETC2
  (**Override for ...**), **Padding**, **Extrude Edges**, **Trim**, **Keep Members Together** (a
  member's frames share a page when they fit, so an animation never switches texture), **Power
  of Two**, **Square**, **Compression** (Auto, Lossless, Platform Default) and **Filter** (Auto
  from members, Point, Bilinear).
- **Pack Preview** packs in memory; **Apply** writes the atlas (one undo step) and builds it.
- The pages: tabs, zoom (Fit, 1x to 16x), frame outlines, the trim and extrusion, a tooltip per
  frame (name, member, size, what was trimmed, its place). A click selects the frame's file in
  the Project window; a double-click opens an Aseprite file in Aseprite.
- The frame list: search by name, `member:<path>` and `page:<n>`; sort by name, size, page or
  waste; a thumbnail per frame.
- The stats: `3 pages · 1,204 frames · 87 % used · 412 duplicate frames merged · 1.9 MB VRAM`,
  the wasted pixels per page and the members with the most transparent area. Warnings have a Fix
  button where one exists (remove a member packed twice, raise Max Size).

Sprites keep referencing their source: adding a texture to an atlas changes no scene. While an
atlas packs it, a sprite showing `items/sword.png` draws it from the atlas page, with the same
frame names, order, sizes, trim offsets and pivots. Old sprites with inline frames draw from the
page too. Animation clips keyed on a source keep working; key the source, not the atlas.

Packing is lossless for pixel art: page pixels equal the source pixels, transparent texels keep
their colour. **Auto** compression is lossless when a member is an Aseprite file, a texture with
lossless import settings or a texture with a Sprite Mode; otherwise each platform's sprite format,
with every frame aligned to 4-pixel blocks. Builds run in the background (the status bar shows
`Building sprite atlas ui (2/3 pages)` with Cancel) and are incremental: an unchanged member is not
read again, an unchanged page is not written or cooked again. Saving a member (a texture, an
Aseprite file) rebuilds the atlases that pack it. The Console reports
`[Sprite Atlas] Built ui/icons.spriteatlas: 3 pages, 1,204 frames, 87 % (210 ms)`; the Project
window marks an atlas that is building or failed.

**Project Settings > 2D > Sprite Atlas > Mode**:

- **Enabled** (default): the editor, Play and builds draw from atlases.
- **Enabled for Builds**: the editor and Play draw the sources; builds use the atlases.
- **Disabled**: nothing is built or used.

An atlas with **Include in Build** off is never used by builds; a script can still load it by
path. A source packed by two atlases in use is a warning on both; the atlas whose path sorts
first draws it. Exports build each atlas for their platform family (its Max Texture Size), ship
the pages under `assets/je-imported/`, and leave out a packed texture that only sprites use.

## Inline frames (legacy)

A Sprite made before sheets keeps its frame rectangles in itself. The Inspector shows them under
**Inline Frames (legacy)**; **Convert to Sprite Sheet** moves them into the texture's Sprite Mode
(Multiple, names and order kept) and makes the Sprite show the same frame by index, so clips and
Sprite Animations keep working. Every other sprite of the project that uses the texture with the
same frames (open and closed scenes, prefabs) converts too; one undo restores everything. A
texture that already has a different Sprite Mode cannot take them: keep the sprite inline.
**Assets > Convert Inline Sprite Frames...** lists every legacy sprite and converts them texture by
texture. `jasperengine-editor check --project <dir> --sprites` lists them from the command line,
with the atlas problems (exit 1 on an atlas error).

## Scripts

```lua
local sprite = Sprite(self.scene, self.entity)
sprite.sheet = "assets/items/coin@16x16.png"
sprite.frameName = "coin_2"
sprite.drawMode = SpriteDrawMode.SIMPLE
print(sprite.frameCount)

local icons = SpriteSheet.load("assets/ui/icons.spriteatlas")
if icons then
    local sword = icons:getFrame("sword")
    if sword then print(sword.rect.x, sword.rect.y) end
end
```

`SpriteSheet.load` takes any sheet: a texture, an Aseprite file, a `.spriteatlas`. A Sliced
sprite is `sprite.drawMode = SpriteDrawMode.SLICED` with a width and a height.

## Agents

The MCP tools are `set_sprite_frames` (sheets, slicing into a texture's Sprite Mode, `convert`),
`edit_sprite_atlas` and `get_sprite_atlas` (group `sprites`); `get_asset_info` reports a texture's
Sprite Mode and `packed_in`. The agent guide is `jasper://docs/sprites`.
