# Procedural Material

A Procedural Material is a `.procmat` node graph that bakes to ordinary PNGs and a `.material`
(S90). Make one with **Assets > Create > Procedural Material** (a Perlin noise on a Material
Output) and double-click it in the Project window: it opens in the **Procedural Material** window.
The window is the [Graph Editor](graph-editor.md) with what a material adds; the canvas, the
search, the Inspector rows, Copy and Paste, Arrange and Esc work as they do there.

## Node images

Every node shows the image of its first output and what drawing it took on this device ("1.2 ms",
the node and everything that feeds it, at 128²). An image whose node changed stays, dimmed with a
clock, until its new one is drawn. One image is finished per frame, the selected node first, so a
large graph fills in over a few frames. On a software renderer (tier 2), and on a device whose tier
is not detected, only the selected node and the Material Output get images; with no GPU at all (tier 3) only the selected node does, and
selecting another node stops it.

## Preview

The pane on the right follows the selection: the selected node's output, or the material when
none is selected. **Pin A** and **Pin B** keep the output shown in a slot whatever you select;
click the shown slot again to follow the selection, right click a slot to clear it. The pane shows:

- the lit ball of the material;
- the shown output as a 2×2 tile, so seams show;
- one image per output of the Material Output (Base Color, Normal, ORM, Emission, Height).

A value drag (a number, a colour, a ramp with the same keys) updates the images at once with no
recompile. Other edits (an enum, a wire, a node) rebuild 0.2 s after the last one; until then the
previous image stays, marked Out of date.

## Seed, Variations

**Seed** is the graph's seed: every node's random values derive from it and from the node's own
seed. **Reroll** picks a new one. **Variations** shows the material at 9 other seeds; click one to
use its seed; a cell whose image is still of the previous seeds shows a clock and takes no click. **Lock** keeps the seed: the field, Reroll and Variations leave it alone, so a bake
gives the same images. Each change is one undo step, stored in the file.

## Exposed Properties and Presets

Right click a parameter's name in the Inspector and pick **Expose in Procedural Material** to keep
it in the pane's Exposed Properties, where it edits like in the Inspector. **Presets** keep the
exposed values under a name: type one and **Save**; click a preset to set every exposed value to
it (one undo step); the cross deletes it.

## Problems, device and bake

The Problems strip adds why the material has no image (nothing on the Material Output, a value
the generator refuses) and the rows of the material's checks (the agent's
`preview_procedural_material`); **Fix** applies a check's fix when it is a single edit, and a click
selects the node it is about.

The pane always says which device draws: its name, and its tier (`gpu`, `software` or `cpu`) when it
is detected, "tier unknown" otherwise. On a software
renderer or the CPU it says why ("No GPU available, baking on the CPU.") and **Bake** shows its
estimated time at the size picked, from how long the preview took. A graph with errors cannot be
baked.
