# Curves and gradients

Curve and gradient values (particle modules, the Force Field falloff, the Tween ease "Curve")
are edited with Unity's Curve and Gradient Editors (docs/specs/S27-editor-tools.md R6-R14). The
curve type is the engine's `Curve` (cubic Hermite keys with tangent modes and wrap modes); the
same values are in scene files, `.particles` files, MCP and Lua.

## The Curve field

- The field draws the curve over its range. Click it: the **Curve** window opens on it (another
  curve field moves the window there).
- Right-click: **Copy**, **Paste**, **Reset**, and **Presets** (the built-in shapes and your
  own), applied fitted to the field's range. Each is one undo step.
- Min-max values (particles) add Unity's mode dropdown: Constant, Curve, Random Between Two
  Constants, Random Between Two Curves. The two-curve mode shows both curves and the band between.

## The Curve window

- **Keys**: double-click on the curve adds a key there (or **Add Key** in the right-click menu);
  drag keys to move them; Shift- or Ctrl-click adds to the selection and a box drag selects
  several; **Delete** (or Backspace) removes them. Ctrl+C / Ctrl+V copy and paste keys.
- **Tangents**: a selected key shows its tangent handles. The key menu sets **Clamped Auto**
  (the default for new keys: smooth, never overshooting), **Auto**, **Free Smooth**, **Flat**,
  **Broken** (the two sides move separately) and, per side, **Free**, **Linear** and **Constant**
  (a step).
- **Wrap Mode** before and after the keys: Clamp, Loop, Ping Pong, Linear and Loop with Offset.
- **F** frames the curve (**Frame All**); the wheel zooms (Ctrl: time only, Shift: values only), the middle mouse or Alt+drag pans.
- The fields at the bottom type the selected key's time and value.
- The presets bar applies a shape; **+** saves the curve as a preset in your library
  ("My Presets", in `<settings dir>/curves/`, or a project library in
  `<project>/EditorSettings/Curves/`).
- Undo: every gesture (a drag, a menu action, a typed number, a preset) is one step. Esc during a
  drag cancels it.
- Keys: while the window has focus, F, Delete, Ctrl+C and Ctrl+V act on the curve and only on it,
  and so do **Edit > Frame Selected**, **Delete**, **Copy** and **Paste** (with nothing selected,
  Delete does nothing; it never reaches the scene). Rebind them in **Edit > Shortcuts...**, group
  Curve (`curve.frame`, `curve.delete`, `curve.copy`, `curve.paste`).

## Gradients

- The **Gradient** field shows the gradient over a checkerboard; click opens the **Gradient
  Editor**: alpha keys over the bar, colour keys under it. A click beside the bar adds a key with
  the gradient's value there, dragging moves it, dragging it far off the bar removes it. The
  selected key's **Color** (a colour field: the Color window) or **Alpha**, and its **Location**
  (percent), are below.
- **Mode**: Blend or Fixed. Up to 8 colour and 8 alpha keys.
- The presets come from the swatch libraries' gradients (color-picker.md).
- Gradient colour keys are sRGB, 0-1 (no HDR keys).

## Tween ease "Curve"

A Tween (Position, Rotation, Scale, Color, Alpha) with **Ease** = Curve eases along its own
curve: the value at a normalized time 0-1. The field's view spans -1 to 2, so overshoots are
editable. Scenes saved before this load a linear curve.

## Files

Curves are JSON (and the same structure in scene YAML):
`[[time, value, inTangent, outTangent, mode?], ...]`, with `null` for a step (Constant) tangent,
or `{"keys": [...], "preWrapMode": "loop", "postWrapMode": "pingPong"}` when a wrap is not clamp.
`mode` is `clampedAuto`, `auto`, `free`, `linear`, `constant`, or `in:<mode>,out:<mode>[,broken]`.
Gradients are `{"colorKeys": [[t, r, g, b]], "alphaKeys": [[t, a]], "mode": "fixed", "linear": true}`.

## Lua

```lua
local curve = AnimationCurve.linear(0, 0, 1, 1)
curve:addKey(0.5, 0.8)
curve.postWrapMode = WrapMode.PingPong
local y = curve:evaluate(0.25)
```

`Keyframe`, `AnimationCurve` (1-based key indices), `Gradient`, `MinMaxCurve` and
`MinMaxGradient` are bound; see `jasper://docs/lua-api`.

## Manual checks

Double-click adds a key on the curve; Delete removes it; the key menu sets each mode; the range
clamp holds while dragging; the min-max band is drawn; a preset applies fitted to the range; each
gesture is one undo step; Esc during a drag restores.
