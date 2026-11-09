# Title Designer

Window > Title Designer is where you design Title Art (`.titleart`): stylized titles and logos with
rims, bevels, metals, glass, glows and generators, baked to ordinary textures. Double-click a
`.titleart` in the Project window, use the Inspector's **Open in Title Designer**, or
Assets > Create > Title Art.

Nothing is ever unsaved: every change is written to the file as one undo step (Ctrl+Z / Ctrl+Y as
everywhere in the editor). A drag shows live and is written once, when you release it; Esc during a
drag puts everything back and adds nothing.

## The window

- **Toolbar**: the document (drag it into the Hierarchy or the Scene view to place the title; its
  menu lists recent documents), the tools (Q Hand, W Move, E Rotate, R Scale, T Rect, Z Pivot/Center),
  **Add**, **Preview As** (Realtime, Baked, Glyph Atlas: what that Render Mode ships, and Realtime
  High, Medium and Low: 3D layers as each Realtime tier draws them), **Quality** (Auto
  renders Draft while you drag, Full when it settles), **Background**, **Device**, **Guides**,
  **Snap**, zoom, **Bake** with **Auto** Bake, the check badge and **Stats**.
- **Layers** (front at the top) and **History** on the left; parts (Face, Rim 1...; on a 3D layer
  Face, Bevel, Side, Band 1...) and effects are rows under their layer; generators are layers of
  their own. Effects and generator layers show their cost class (C0 cheap to C4 bake only; hover for
  the desktop and mobile estimate); a generator that follows a deleted layer shows a warning.
- **Canvas** in the middle, with rulers (drag one to add a guide), guides, safe areas of the chosen
  device, the gizmo and the effect handles (light angle, gradient line, glow centre...).
- **Properties** on the right: the selection's page, and **Find a setting** at the top. Its results are
  the controls themselves; English and Japanese both work (`フチ`, `ぼかし`, `blur`, `glow`).
- **Library** at the bottom: Looks and Color Schemes drawn on your own text. Point at a tile to
  preview it on the canvas, click to apply (one undo step). Until the full Library is installed it
  holds a starter set (Casino Gold, Chrome, Neon, Fire, Ice and more); Templates come with it.

Below 960 px the side panels become tabs of one column and the Library folds to its header (the
arrow opens it); below 480 px only the canvas shows, with a **Panels** button.

A title in Chinese, Japanese or Korean needs a CJK font on your system for the editor's fields
(Text, layer names, search) to show it: without one the window says so once. Install one (for
example Noto Sans CJK) and restart; **Preferences > General > CJK Font Language**
(`cjk_font_language`) picks the glyph forms. The title itself always renders with its own font.
See [Text and languages](text-and-languages.md#fonts-in-the-editor).

## Editing

- Rows: drag a label or a value to scrub (Shift x10, Alt x0.1), click to type; the wheel only changes
  a focused field. A changed value shows a reset arrow. Several selected layers or rims edit
  together: typing gives them all the value, scrubbing moves each from its own value.
- Inherited values (bevel and satin light, shadow angles) show a dot: hollow takes the Global Light,
  filled is set here. Editing an inherited value sets it here.
- Text: double-click a text layer (or press Enter) to type on the canvas, Japanese input included.
  Enter is a new line, Ctrl+Enter is done, Esc cancels. Selected characters get a small gizmo each:
  drag the dot to move them, the knob above to turn them (Shift: 15 degree steps), the square to
  scale them; every selected character changes together, as one undo step.
- Tab moves the keyboard between the toolbar, Layers, the canvas, Properties and the Library
  (Shift+Tab back); the panel it reaches is outlined.
- Ctrl+D duplicates, F2 renames, Delete deletes, Ctrl+G groups, Ctrl+Alt+G clips, H hides, L locks,
  arrows nudge (Shift x10), F frames the selection, Home fits the canvas.

## Adding effects and generators

**Add** (Ctrl+Shift+N, or **Add Effect** under the Effects) lists layers, generators and effects in
categories with their counts: Glow, Shadow & Depth, Stroke & Edge, Color, Texture & Material,
Stylize, Blur, Distort, then the generators that follow the selected layer. Search in English or
Japanese (`power glow`, `かすれ`, `集中線`); pointing at an entry previews it, Enter or a click adds
it as one undo step. Recipes (Long Shadow, Shine, CRT...) add the effect they are made of; style
settings (Dry Brush) switch the layer's own setting on and open its section. An effect's page has
its presets as chips (point to preview, click to apply), **Randomize Seed** where it has a seed, and
on effects with a live variant a play button that animates what a Realtime title animates without
re-rendering (view only).

## 3D layers

Set a layer's **Dimension** to 3D (or its menu's **Convert to 3D**). The Dimension section then holds
Geometry (depth, bevel with the profile picker; Custom edits the profile curve), Bands (drag to
reorder, Add Band with presets), Lighting (the environment picker; every value shows whether it is
set here or inherited from the document, and its menu's **Inherit** goes back), Glint, Ambient
Occlusion, Camera, Realtime Preview and Status (triangles and whether each tier fits). Face, Bevel,
Side and each band have their own page with the material picker (point at a tile to preview it).
The gizmo of a 3D layer is the fixed schematic: arrows move, arcs and the outer ring turn it, each
drag one undo step; while you type, the selected letters take it ("Tilt Letters"). Alt+drag on the
layer turns its environment.

## Runtime and In Context

The Document page's **Runtime** section sets the characters a Realtime or Glyph Atlas title ships
(presets, your own, a file, the document's text) and **Check Characters** lists what is left out
and the font subset sizes. **Open in Title Designer** on a Title Art Renderer opens the title
**In Context**: its scene behind it, the title where that renderer puts it.

## Fonts

The Font row opens the picker: project fonts (each face of a `.ttc` separately), the bundled
library, Google Fonts and the system's fonts, each drawn with your own text. **Covers My Text**
hides fonts that miss characters you typed. Choosing a system or Google font copies it into
`Assets/TitleArt/Fonts/` first, so the project builds the same on every machine. `.woff` and
`.woff2` files are converted to `.ttf` / `.otf` on import.

## Checks

The badge runs the document checks after each change and the pixel checks on **Check Rendered**.
They are measurements: passing them does not mean the title looks good.

## For agents

Everything the window does is an `edit_title` op batch; `focus_window {window: "title_designer"}`
and `screenshot {view: "title_designer"}` show it (`jasper://docs/title-art`).
