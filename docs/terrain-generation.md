# Terrain generation

A **Noise Graph** is the recipe of a generated Terrain: a small file you can read, open in a window and
change, with every step visible. The same graph and seed give the same terrain on every platform.

## Make a terrain from a preset

Select a Terrain, open the **Generate** section of the Terrain window, pick a graph from the list, set
**Seed** (or press the dice), and press **Preview** to see a hillshade, the height range and a slope
histogram without writing anything. **Apply** writes it as one undo step, such as *Generate Terrain
(Mountains, seed 7)*. **Apply To** chooses Height, Layers and Foliage Density; **Region** limits the run to
a rectangle, with **Feather** blending it into the terrain around it.

A new terrain can start from a preset too: in **Create Terrain** (GameObject > 3D Object > Terrain, or the
Terrain window with nothing selected), pick it under **Noise Graph**. Its recommended Terrain Size and
Terrain Height fill the fields, and after **Create** it runs as its own undo step, the same terrain the
Generate section makes with that graph and seed.

The presets: Rolling Hills, Mountains, Alpine (eroded, with snow, rock and scree layers), Canyons, Islands,
Dunes, Plateaus and Plains; and two that work on the terrain you have, Erode Existing and Paint By Slope.

A layer the terrain does not have is skipped with a warning when the graph marks it optional, as the
presets do. Add the layer and generate again to get its paint.

## Edit a graph

**Window > Terrain > Noise Graph**, or double-click a `.noisegraph`. **Project window > Create > Noise
Graph** starts one from a preset, or Blank. Right-click, Tab, or dropping a wire on empty space opens the
node search. Each node shows a thumbnail of its output, its observed and analytic range and its time. A
dimmed node reaches no output; a marked one was folded to a constant. An error draws on its node, and
**Apply** stays off until none are left. Every edit is one undo step; a parameter drag is one step when
you let go.

## What it costs a game

Nothing, if it bakes: a project that only generates in the editor ships no noise code and no
`.noisegraph` files. A game whose scripts name `Noise` or `NoiseGraph` carries the `noise` player module
(`docs/scripting/noise.md`) and every `.noisegraph`, since a script can name any of them. Erosion is a
bake: a graph with an erosion node does not load in a game.
