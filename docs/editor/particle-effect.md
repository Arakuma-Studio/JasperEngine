# The Particle Effect window

**Window > Visual Effects > Particle Effect** shows one effect as a whole: every Particle System it is
made of, when each one starts and stops, a preview with its own clock, and the modules of the system
you pick. It is Unity's Particle Effect panel grown into a window, with ideas from Effekseer's editor
(the timing view, editing that keeps the frame). Spec: `docs/specs/S91-effect-authoring.md`.

## Opening it

- **Double-click a `.particles` file** in the Project window (or select it and use Assets > Open). A
  single click still shows the file in the Inspector; **Open as Text** in its context menu shows the JSON.
- **Right-click a GameObject in the Hierarchy > Open in Particle Effect**, or **Open in Particle Effect**
  in a Particle System's Inspector: every Particle System at or below that GameObject, placed as they
  are under it.
- Agents: `focus_window` with `window: "particle_effect"` brings it to the front; `editor_status`
  reports what it shows under `ui.particle_effect` (the file or subtree root, the clock, the seed, the
  hidden nodes).

## Outline and timing

The left side lists the systems and, under each, its sub-emitter children down the tree (Birth, Death,
Collision, Trigger), each with a bar on the timeline:

- a thin light span: the start delay's range;
- the solid bar: when it emits (to the right edge with an arrow when it loops); orange when it emits on
  Collision or Trigger events, whose times are only a bound;
- the light tail: its particles living on after the last emission;
- white diamonds: bursts.

The bars are computed from the documents (start delay, Duration, Looping, the emission and the start
lifetime), not from a run, so they are the same everywhere; MCP gets the same rows. The red line is the
playhead: click or drag on the ruler or on any bar to scrub. A scrub pauses the clock.

The **eye** beside a node hides it in the preview only. Its children keep playing (it still simulates,
it just draws nothing). The eye never writes the file, adds no undo step and never reaches an export.

## Preview Stage

The preview plays the effect in a scene of its own, with its own clock:

- **Play / Pause**, **Step** (one tick, paused), **Restart** (back to the start, same seed) and the
  playback speed.
- **Seed**: the seed every system plays with. **Reroll** picks another one at the same time.
  **Per-Loop Seed** gives each loop a new seed; otherwise the seed changes only when you ask.
- **Loop Range**: the clock loops between two times. Without it, a one-shot effect starts again shortly
  after its last particle dies, and a looping effect plays on.
- **Background**: Dark, Light, Checker (to judge alpha), or Scene Sky (the open scene's sky).
- **Moving Emitter**: the effect circles the stage's centre, for trails, Rate over Distance and inherited
  velocity.
- **Overdraw**: every particle adds the same heat, untextured. Bright areas are where many layers cover a
  pixel, which costs fill rate on mobile and web.
- Drag the image to orbit, use the wheel to zoom, double-click to frame it again. The corner shows the live
  particles against their capacity and the simulate and render times.

### Editing holds the frame

When you change a value while playing or paused, the preview simulates again to the **same time with the
same seed**. The work is spread over frames (at most 4 ms each, starting from the checkpoints) and a bar
under the image shows its progress. So a paused frame shows exactly what your change does to that frame.

## Modules

The right side has the selected node's modules, the same rows as the Inspector. Edits write the file
(an inline effect: its GameObject) and every Particle System that uses it updates. Each drag is one undo
step, a click that changes nothing is none, and **Esc** during a drag puts the value back with no step.

- A value that differs from its default has a bar beside its label. **Right-click the label > Reset**
  puts it back.
- Each module header has a **?** link to its section below and a right-click menu: **Reset**, **Copy
  Module**, **Paste Module** (adds the copied module when the effect lacks it) and **Paste Values** (onto
  the same module, keeping its checkbox). **Add Module** also offers a copied module the effect lacks.

**Exposed Parameters** has a slider per exposed parameter and a button per trigger name (Trigger
sub-emitters). They drive the preview only; the effect keeps its values.

## Curve window additions

When the Curve window is opened from these rows, it shows the window's playhead (the clock as a fraction
of the system's Duration, the axis of curves over the emitter's time). Its key menu adds:

- **Align**: the selected keys take their mean value;
- **Smooth**: each selected key gets joined Free tangents along its neighbours' mean slope (Unity's
  SmoothTangents);
- **Sharpen**: Linear tangents on both sides, a corner at each key;
- **Paste at Playhead** (also on the graph's right-click menu): the copied keys at the playhead.

## Module reference

The **?** link on each module header opens its section here.

### Main
Duration, Looping, Prewarm, Start Delay, the start values (lifetime, speed, size, rotation, colour), gravity,
simulation space and speed, Max Particles, the seed, culling and bounds.

### Emission
Rate over Time, Rate over Distance and bursts (time, count, cycles, interval, probability).

### Shape
Where particles are born and their first direction: sphere, hemisphere, cone, box, circle, donut, edge,
rectangle or a mesh.

### Start Velocity
A velocity added at birth, on top of Start Speed along the shape's direction.

### Velocity over Lifetime
Linear, orbital and radial velocity over each particle's life, and a speed modifier.

### Limit Velocity over Lifetime
Caps the speed (per axis or overall) and adds drag.

### Force over Lifetime
A force (acceleration) over each particle's life, in local or world space.

### Noise
Curl noise that turbulates position, and optionally rotation and size.

### Orbit
Particles circle an axis through the emitter.

### External Forces
Which Force Fields act on the system (by layer), and how strongly.

### Color over Lifetime
A gradient over each particle's life, multiplied with its start colour.

### Color by Speed
A gradient over the particle's speed.

### Size over Lifetime
A curve over each particle's life that scales its size.

### Size by Speed
A curve over the particle's speed that scales its size.

### Rotation over Lifetime
An angular velocity over each particle's life.

### Rotation by Speed
An angular velocity over the particle's speed.

### Texture Sheet Animation
Frames of a sprite sheet (a grid or sprites) over the particle's life, at a frame rate, or by speed.

### Collision
Particles bounce off planes, the world's colliders (3D or 2D) or terrain, and can die or send events.

### Trails
A ribbon behind each particle, or ribbons through the particles.

### Sub Emitters
Other `.particles` effects started when a particle is born, dies, collides or a trigger is sent. Each is a
child in the Outline.

### Custom Data
Values per particle that a custom shader reads.

### Renderer
How particles are drawn: billboard modes, meshes, the material and texture, blending, sorting, lighting.

### Set over Age
Sets a property over age windows (the older from-to modifiers).
