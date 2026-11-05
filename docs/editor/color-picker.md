# Color picker

Every colour field of the editor (the Inspector, Scene Settings, Project Settings, Preferences,
particles, terrain, navigation, the shader library) is one widget: a swatch that opens the
**Color** window (docs/specs/S27-editor-tools.md R1-R5). Agents set colours through MCP
(`jasper://docs/editor-tools`).

## The field

- Click the swatch: the Color window opens on that field. Clicking another colour field moves the
  window to it and finishes the first edit.
- An HDR field shows "HDR" on the swatch; an alpha field shows the alpha as a bar under it.
- Hovering shows the field's name and its hexadecimal value.
- Several selected GameObjects with different values show a dash over the swatch; an edit sets
  them all.

## The Color window

- **SV square** with a hue bar, or the **hue wheel** (the Wheel / Square button).
- **RGB 0-255**, **RGB 0-1.0** or **HSV** sliders, and **Hexadecimal** (`RRGGBB` or `RRGGBBAA`,
  sRGB; **Copy** puts `#RRGGBB` on the clipboard). The A slider when the field has alpha.
- **Intensity** for HDR fields: stops (powers of two) over the colour; the RGB values stay 0-1.
- The bar at the bottom compares the original (left; click to go back to it) and the new colour.
- **Eyedropper** (the pipette button, or I, `colorPicker.eyedropper` in **Edit > Shortcuts...**): click anywhere on the screen to take that colour, in
  any editor window or outside the editor. A loupe magnifies the pixels under the pointer. Esc or
  a right-click stops without a change. Inside the Scene and Game views it reads the rendered
  image itself, so the value is exact.
- Every change is live. **Enter** or closing the window keeps it as one undo step
  ("Change <Field>"); **Esc** gives the field its original value and records nothing.
- The window remembers its size and position.

### Linear and sRGB

Colours are edited in sRGB, as you see them. A field that stores linear values (lights, fog,
materials: most 3D colours) is converted when it is written; the hex and the 0-255 values are
always sRGB. HDR values above 1 keep their ratio and show the stops in Intensity.

## Swatches

The **Swatches** section under the picker holds libraries of colours:

- A user library (`<settings dir>/swatches/*.colors`, on this computer) and project libraries
  (`<project>/EditorSettings/Swatches/*.colors`, in version control). The library menu switches
  between them, makes new ones (**New Library...**, **New Project Library...**) and reveals the
  file.
- Click a swatch to use it; **+** adds the current colour. Right-click a swatch: **Replace**
  (with the current colour), **Rename...**, **Move Left**/**Move Right**, **Delete**.
- A library also keeps the gradient presets of the Gradient Editor (curves.md).
- A library written by a newer editor (an unknown `version`) is shown read-only.

The `.colors` format is JSON:
`{"version": 1, "name": "...", "swatches": [{"name": "Sky", "linear": [r, g, b, a], "hdr": false}], "gradients": [{"name": "...", "gradient": {...}}]}`.

## Manual checks (per platform)

The eyedropper's screen read is platform code: X11 (`XGetImage`), Windows (`BitBlt` of the
desktop DC) and macOS (`NSColorSampler`). On a new platform or backend check: the picker opens
from an Inspector colour, a drag in the square is one undo step, Esc restores, the eyedropper
takes a colour from the Scene view, from another editor window and from outside the editor.
