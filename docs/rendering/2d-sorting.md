# 2D sorting: Sorting Layers, Order in Layer, Sorting Groups

Unity's 2D draw order controls (spec: [S52](../specs/S52-sprite-instancing.md)). With every
renderer at its defaults the order is the Hierarchy's, exactly as before.

## The order

A camera without depth test (every 2D camera) draws its renderers by:

1. **Sorting Layer**: its place in **Project Settings > Tags and Layers > Sorting Layers**, the
   first drawn first (under the others).
2. **Order in Layer**: lower first.
3. With **Project Settings > Graphics > Transparency Sort Mode** set to **Custom Axis**: a larger
   `dot(Transparency Sort Axis, point)` first. The default axis `(0, 1, 0)` draws higher sprites
   first, so a character lower on the screen covers one above it (top-down games). The point is the
   Sprite's bounds' centre (**Sprite Sort Point: Center**, the default) or its pivot (**Pivot**);
   other renderers use their pivot.
4. The Hierarchy order.

UI (a RectTransform and everything under it) is never reordered: it keeps its place in the
Hierarchy, and renderers sort within the stretches of Hierarchy between UI subtrees.

A camera with depth test (3D) keeps its opaque queue as it was. Its transparent queue draws by
Sorting Layer, Order in Layer, then Sort Priority, then far to near.

## Sorting Layers

**Tags and Layers > Sorting Layers** lists them: **+** adds one (it draws after the others),
**−** removes one, the name field renames it, and dragging a row (or the arrows) reorders. Default
always exists and keeps its name; you can move it. Each edit is one undo step.

A renderer stores its layer's id, which never changes: renaming or reordering rewrites no scene.
Removing a layer that renderers of the open scenes use asks first; those renderers draw on
Default until the layer comes back (a scene naming an unknown layer says so once in its load
report).

Renderers with the two rows **Sorting Layer** and **Order in Layer**: Sprite, Mesh Polygon, Mesh
(Additional Settings), Particle System, Point Renderer, Line Renderer, Trail Renderer. The Sorting
Layer dropdown ends with **Add Sorting Layer...**, which opens Tags and Layers.

## Sorting Group

**Add Component > Rendering > Sorting Group** makes the GameObject and everything under it sort as
one item at the group's Sorting Layer and Order in Layer (and its pivot along the axis). Inside,
its renderers sort among themselves by the order above. Groups nest; a disabled group sorts as if
it were not there.

## Batching

Dynamic Batching joins draws only within one Sorting Layer, Order in Layer and Sorting Group: a
change is a batch break (`sort_bucket`). Put sprites that share a sheet in one layer and order to
keep them in one draw.

## Scripts

```lua
sprite.sortingLayerName = "Foreground"
sprite.sortingOrder = 5
print(SortingLayer.getLayerValue(sprite.sortingLayerId))
for _, layer in ipairs(SortingLayer.layers) do print(layer.id, layer.name, layer.value) end
local group = SortingGroup(scene, entity)
group.sortingLayerName = "Characters"
```

`sortingLayerId`, `sortingLayerName` and `sortingOrder` are on Sprite and Mesh, ParticleSystem,
Points, LineRenderer, TrailRenderer and SortingGroup; an unknown layer raises an error. C++ has
the same fields on the components and `SortingLayers` (`engine/core/registry/SortingLayers.h`).

## Exports

The layer table and the Transparency Sort settings go with the game: a compiled game sets them in
its generated `init()`, a prebuilt one reads them from `game.json`.
