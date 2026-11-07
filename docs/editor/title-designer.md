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
  **Add**, **Preview As** (Realtime, Baked, and the modes other features add), **Quality** (Auto
  renders Draft while you drag, Full when it settles), **Background**, **Device**, **Guides**,
  **Snap**, zoom, **Bake** with **Auto** Bake, the check badge and **Stats**.
- **Layers** (front at the top) and **History** on the left; parts (Face, Rim 1...) and effects are
  rows under their layer; generators are layers of their own.
- **Canvas** in the middle, with rulers (drag one to add a guide), guides, safe areas of the chosen
  device, the gizmo and the effect handles (light angle, gradient line, glow centre...).
- **Properties** on the right: the selection's page, and **Find a setting** at the top. Its results are
  the controls themselves; English and Japanese both work (`フチ`, `ぼかし`, `blur`, `glow`).
- **Library** at the bottom (when the Looks, Color Schemes and Templates are installed): point at a
  tile to preview it on your own text, click to apply.

Below 960 px the side panels become tabs of one column; below 480 px only the canvas shows, with a
**Panels** button.

## Editing

- Rows: drag a label or a value to scrub (Shift x10, Alt x0.1), click to type; the wheel only changes
  a focused field. A changed value shows a reset arrow. Several selected layers or rims edit
  together: typing gives them all the value, scrubbing moves each from its own value.
- Inherited values (bevel and satin light, shadow angles) show a dot: hollow takes the Global Light,
  filled is set here. Editing an inherited value sets it here.
- Text: double-click a text layer (or press Enter) to type on the canvas, Japanese input included.
  Enter is a new line, Ctrl+Enter is done, Esc cancels.
- Ctrl+D duplicates, F2 renames, Delete deletes, Ctrl+G groups, Ctrl+Alt+G clips, H hides, L locks,
  arrows nudge (Shift x10), F frames the selection, Home fits the canvas.

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
