# 2D draw order and Sprite Batch

Read before setting draw order in a 2D scene or drawing many sprites from a script.
User docs: `docs/rendering/2d-sorting.md` and `docs/rendering/sprite-batch.md`.

## Draw order

A 2D camera (no depth test) draws renderers by, in this order:

1. Sorting Layer: the layer's place in `tags_and_layers.sorting_layers`, first drawn first.
2. Order in Layer: lower first.
3. With `graphics.transparency_sort_mode: custom_axis`: a larger `dot(graphics.transparency_sort_axis,
   point)` first (axis `[0, 1, 0]`: higher sprites first, so lower ones cover them). The point is
   a Sprite's bounds' centre (`spriteSortPoint: center`) or its pivot (`pivot`).
4. The Hierarchy.

UI (RectTransform subtrees) keeps its place and Hierarchy order. A 3D camera's transparent queue
compares Sorting Layer and Order in Layer before `sortPriority` and the distance.

- Add layers with `set_project_settings {"tags_and_layers.sorting_layers": ["Background",
  "Default", "Characters", "Foreground"]}`. Pass the whole list. A name already listed keeps its
  layer, so renaming in a list makes a new one. `Default` must stay in the list.
- Set a renderer's layer and order with `set_properties`, on the Sprite's or Mesh's `mesh`, or on
  `particlesystem`, `points`, `linerenderer` or `trailrenderer`:
  `{"sortingLayer": "Foreground", "sortingOrder": 5}`. `sortingLayer` takes a name or an id and
  reads back as `{id, name}`.
- `add_component sortinggroup` makes a GameObject's subtree sort as one item at the group's
  `sortingLayer` and `sortingOrder`. Inside it, children sort among themselves.
- Check the order with `screenshot`.

## Sprite Batch

Use a Sprite Batch for many copies of one Sprite's frames (bullets, coins, debris, crowds). It needs
no GameObject per copy and draws one call per texture page.

1. `add_component spritebatch` on a GameObject with a Sprite (not beside an Instanced Mesh).
2. Fill it from a script. Instances are not saved and the Sprite draws only through them.

```lua
local batch = SpriteBatch(scene, entity)
local bullet = batch:frameId("bullet_a")
batch:setPositions({0, 0, 16, 0, 32, 0})
batch:setFrames({bullet, bullet, bullet})
batch:setRotations({0, 1.57, 3.14})
batch:setColors({1, 1, 1, 1, 1, 0.5, 0.5, 1, 1, 1, 1, 0.5})
batch:setFx("flash", {0, 0.5, 1})
print(batch.count)
```

- `mode`: `retained` keeps the instances, `clear_each_frame` draws what a frame wrote once.
- `space`: `local` (they move with the GameObject) or `world`.
- `get_entity` reports `spritebatch.count`, and `get_render_stats` counts its `instances`.
- A bulk call validates once. A wrong length, an out-of-range frame or an unknown frame name raises
  a typed error (`length`, `frame_range`, `unknown_frame`) and changes nothing.

## What batches in pixel art

Dynamic Batching draws these together with sprites of the same sheet and material:

- Sprites under a Pixel Perfect Camera (rotated ones too).
- `library:sprite` sprites. FLASH amounts and Palette Swap rows may differ per sprite. Other values,
  such as the outline colour, must be equal. OUTLINE and OVERLAY batch on Sprites only.

`get_render_stats` `batching.breaks` names why a sprite did not join the batch before it
(`jasper://docs/profiling`).
