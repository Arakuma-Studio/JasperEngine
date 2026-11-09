# Sprite Batch

Many copies of one Sprite's frames from one GameObject, drawn with GPU instancing: one draw call
per texture page (spec: [S52](../specs/S52-sprite-instancing.md)). Bullets, coins, debris, crowds:
no GameObject per copy, 96 bytes an instance (112 with FX; 8 more in a scene with a Pixel
Perfect Camera, for the instance's snap pivot).

## Setting it up

**GameObject > 2D Object > Sprite Batch**, or **Add Component > Rendering > Sprite Batch** on a
GameObject with a Sprite. The instances draw that Sprite's frames (its texture, sheet or atlas),
its pivot and its material, with every `library:sprite` effect. The Sprite itself is then drawn
only through its instances. A Sprite Batch cannot share its GameObject with an Instanced Mesh.

| Row | |
|---|---|
| Mode | **Retained**: the instances stay until scripts change them. **Clear Each Frame**: what scripts write in a frame is drawn in that frame only. |
| Space | **Local**: instances move with the GameObject. **World**: their positions are world positions. |
| Capacity | instances the GPU buffer holds before it grows (writing more grows it) |
| Sort Instances | draw the instances along the Transparency Sort Axis (Project Settings > Graphics), else in index order |
| Instances | how many there are (read only) |

The instances are runtime data: they are not saved, and a copied GameObject starts empty.

## Filling it from a script

```lua
local batch = SpriteBatch(scene, entity)
local bullet = batch:frameId("bullet_a")           -- the Sprite's frame by name
batch:setPositions(positions)                       -- flat {x1, y1, x2, y2, ...}; triples with stride 3
batch:setFrames(frames)                             -- frame indices or names (default: frame 0)
batch:setRotations(radians)                         -- about z
batch:setScales(scales)                             -- uniform; {sx, sy} pairs with stride 2
batch:setColors(rgba)                               -- flat sRGB {r, g, b, a, ...}, as Sprite colours
batch:setFx("flash", amounts)                       -- "palette", "flash", "fade" or "dissolve"
local i = batch:add(x, y, bullet)
batch:set(i, x, y)
batch:remove(i)                                     -- the last instance takes its index
batch:clear()
print(batch.count)
```

`setPositions` sets the count; the other streams cover the first `count` instances. Each call
checks its whole array once and changes nothing when it fails: a wrong length (`length`), a frame
out of range (`frame_range`) or an unknown frame name (`unknown_frame`) is an error at the call.
C++ scripts have the same `SpriteBatch` class (`engine/core/object/SpriteBatch.h`), whose calls
take spans of floats.

## How it draws

- An instance is a position, a rotation, a scale, a frame, a colour and, once a script wrote an
  FX slot, the four FX amounts. The frame gives its size and pivot: an instance draws exactly what
  a Sprite showing that frame with that transform draws.
- Consecutive instances on one texture page are one instanced draw.
- A Retained batch uploads its instances only when they changed; a static one uploads nothing.
  Clear Each Frame uploads each frame it draws.
- The batch is culled whole by its instances' bounds; the Scene view picks it as one GameObject.
- It sorts as one item at its Sprite's Sorting Layer, Order in Layer and pivot; its instances never
  interleave with other renderers.
- Under a Pixel Perfect Camera each instance snaps as its Sprite would: an instance at a multiple of
  90 degrees snaps every vertex, any other snaps its pivot; with 90 Degree Steps the rotations are
  drawn rounded to 90 degrees.

## FX slots

`library:sprite`'s per-sprite amounts come per instance: `palette` is the Palette Swap row, `flash`
the hit flash amount (added to the material's), `fade` and `dissolve` are kept for the pixel FX
effects. The FX stream exists only for a material whose effects read one.
