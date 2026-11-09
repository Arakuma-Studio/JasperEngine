# Title animation

A `.titleart` carries named animations in its `animation` member: tracks of keys (or drives and
reseeds), stagger, markers, a loop section and playback settings. The Title Designer's Animation
strip (Dopesheet and Curves), `edit_title` ops, Lua and clips all drive the same data.

How each field plays depends on its class and the instance's Render Mode:

| Class | Baked | Realtime | Glyph Atlas |
|---|---|---|---|
| Title live (position, scale, rotation, opacity, flash, Live Effects) | companion draw | companion draw | companion draw |
| Layer live | flipbook | re-render at `rerender_fps` | the glyph layer's: per cell; plates re-render |
| Glyph | flipbook | glyph cells while playing | native |
| Text (Glyph Atlas live) | flipbook | re-render | cells swap |
| Re-render | flipbook | re-render at `rerender_fps` | plates re-render; the glyph layer's style is not played |

Baked titles play flipbooks: `export_title` writes `<file>.<animation id>.flip<N>.png` pages and
`<file>.anim.json`; a bake past a budget fails the flipbook check (Project Settings > Title Art >
Animation). Runtime control is through `TitleArt:play`, `playOverlay`, `stop`, `reset` and
`onAnimationEvent`; clips key `animationTime` and `animationSpeed`. Spec:
`docs/specs/S85-title-animation.md`.
