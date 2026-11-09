# S85 — Title animation: named animations in a `.titleart`, field classes per Render Mode, Animation Presets, flipbooks, Lua, clips and the Animation panel

**Wave / Depends on.** Title Art program (spec-first), **wave 6C** (with S84, S87; program graph in
§8, program rules in S80 §6.15). Depends on:
- **S80**: the `.titleart` v1 document and its reserved top-level `animation` member, field paths,
  `TitleArtOps`, the renderer and `TitleRenderJob`, the bake, and the `title_art` tools;
- **S81**: the Title Art Renderer and its companion draw, Render Mode, the scheduler and live
  compose, `library:title_fx`, the Lua `TitleArt` class, the `titleart` property rows and their
  reserved `animationTime`, and the Glyph Atlas cells;
- **S82**: the Title Designer, `addPanel`, and the properties and gizmo edits that Record keys;
- **S83**: the per-parameter animation classes (values of S80's `FxAnim` on `ParamDef::anim`) and
  live kinds.

It also uses upstream's clip machinery (`engine/core/animation/KeyframeSampling.*`,
`PropertyBindings.*`), S21 (the Animator plays property tracks), S14 (cooking of flipbook pages),
S24 (named undo) and S25 (commands and settings). Optional: S37 (`.soundevent` on markers) and S40
(sample-placed marker sounds). Neither is implemented today, and S85 works without them.

Owner area:
- **`engine/core/titleanim/`**: always built (the core part, beside S81's component). It holds
  `TitleAnimTypes.h`, `TitleAnimSampling.{h,cpp}`, `TitleAnimPlayer.{h,cpp}` (Baked playback,
  companion uniforms, markers), and `TitleAnimDataIO.{h,cpp}` (`<file>.anim.json`).
- **`engine/core/titleart/anim/`**: module `titleart` only. It holds `TitleAnimIO.{h,cpp}` (the
  document member), `TitleAnimPlan.{h,cpp}` and `TitleAnimRealtime.{h,cpp}` (live compose items
  and re-render requests for S81's scheduler).
- **`editor/titleart/anim/`**: `TitleAnimOps`, `TitleAnimPresets`, `TitleFlipbookBaker`,
  `TitleAnimChecks`, `TitleAnimationPanel` and its pure part `TitleAnimTimelineOps`,
  `TitleAnimRecord`, `TitleAnimPreview`.
- **Data**: `editor/resources/titleart/animation-presets/*.json`.
- **MCP**: the animation parameters and ops in `editor/mcp/tools/TitleArtTools.cpp`.
- **Docs**: the `### Animation` section of `docs/mcp/guide/title-art.md` and the Animation chapter
  of `docs/title-art.md`.

## 1. Goal

Titles move. A `.titleart` holds named animations (Intro, Idle, Win, Outro, any name) on a time
axis that Pachislop never had. Each animation has a duration, a wrap mode with a loop section, and
tracks on any field of the document. Keys use the engine's easings and Hermite curves. Per-glyph
tracks can stagger into a letter cascade. Markers fire Lua events and sounds. Seeds evolve on a
schedule. A designer animates in the Title Designer's Animation panel the way Unity's Animation
window works: dopesheet, curves, Record, scrubbing. One click applies an Animation Preset (Stamp
In, Letter Cascade, Win Pulse...) to any title.

The runtime cost is plain at authoring time. Every field has a **class**:
- **title live**: moves or fades the whole title on the companion's one draw, for free in any
  Render Mode;
- **layer live**: one layer, through S81's live compose;
- **glyph**: per glyph, through glyph cells;
- **re-render**: needs the renderer.

Each Render Mode has one rule per class. Baked turns what it cannot draw live into flipbooks,
Realtime composes or re-renders within S81's frame budget, and Glyph Atlas animates glyphs. The
Designer, `check_title` and the export report that cost before a game ships. Agents author, look
at a filmstrip, check and export with the six `title_art` tools. S85 adds no tools.

## 2. Scope

**In scope**
- The document's `animation` member: animations, tracks (keys, drives, reseeds), stagger, markers,
  its codec, limits and lenient read (R1-R10).
- Field classes, the per-mode plan, flipbooks, and live compose and re-render items for S81's
  scheduler (R11-R20).
- The Animation Presets library and its JSON format, built-in and user presets (R21-R23).
- Playback: `TitleAnimPlayer`, the Lua members of `TitleArt`, clips and the Animator, markers and
  sounds (R24-R31).
- Baked outputs for animations: flipbook pages and `<file>.anim.json` (R16, R17, R60).
- Editor: the Animation panel in the Title Designer (dopesheet, curves, Record, scrubbing, preview,
  onion skin, presets, cost display) (R41-R49).
- Agents: `edit_title` animation ops, and parameters of `get_title`, `render_title`,
  `check_title`, `export_title` and `list_title_library`, plus the guide section (R50-R57).
- Tests, goldens, benchmarks and red controls (§7).

**Out of scope**
- A new timeline asset or a Unity-style track Timeline window. S40 D1 decided that the `.anim` clip
  is the scene's timeline. A title animation is the title's own time axis, and clips drive it
  (R27).
- Video export (MP4, GIF, APNG) of a title animation. The Recorder (S27) captures the Game view.
  S87 exports sheets and PNG sequences from S85's time evaluation (S87 R42).
- The 3D-only members of S86 beyond the class rule: S86 classes its own members, and rigging
  per-letter 3D motion is S86 R4.
- Deforming glyph outlines over time other than through animated warp and distort members, which
  are re-render fields.
- Blending two base animations other than R26's crossfade. Animator-style layers of base
  animations are out; overlays (R25) cover shake and flash on top of an idle.
- Authoring animations from Lua. Lua plays, seeks and listens (§6.5).
- Do-not-copy list (`docs/specs/README.md`): nothing here touches it.

## 3. Current state (feature/title-art `3ffafdfaa`, read)

- **Title Art exists only as specs.** `engine/core/titleart/` and `editor/titleart/` are absent.
  The sibling specs define what S85 builds on:
  - **S80 §6.1**: the document; the reserved top-level member `animation` (S85); layers with `id`,
    `opacity` and `transform {position, rotation (deg clockwise), scale (uniform), flip_x,
    flip_y}`; `chars.<i>` CharAdjust (offset, scale with reflow, rotation); exposed parameters
    `{name, type, targets: [{layer, field}]}`; the 18 blend modes (§6.4), with `normal` and `add`
    by fixed-function blending.
  - **S80 §6.2**: field paths and shorthands (`face.`, `rim#n.`, `fx:<type>#n.`, `gen.`), and op
    batches as one undo step.
  - **S80 §6.11**: tool caps: `get_title` 700, `edit_title` 1,450, `render_title` 1,450,
    `check_title` 1,300, `export_title` 900 (`dry_run` reports without writing), and
    `list_title_library` 800. The rest of the group's 15,000 is S81-S87's.
  - **S80 R40**: `engine/core/titleart/` compiles only with the module.
  - **S81 R1-R2**: the component draws through a **companion** Image or Sprite, in one draw.
  - **S81 R7-R8**: Baked shows the main output PNG; `logo.titleart.bake.json` is the sidecar.
  - **S81 R9**: S83 live (L) parameters change through a **live compose** of cached splits into
    the display target. It costs at most 0.5 ms (mobile, web) or 1 ms (desktop) per title per
    change, else it re-renders.
  - **S81 R15**: the frame budget is 2.0 ms on Android, iOS and web, 4.0 desktop and 8.0 in Edit
    mode. Cost = pixels × `costWeight` (S80 §6.3) × platform factor (mobile 6, web 4, desktop 1, §6.3).
  - **S81 R26-R28**: Glyph Atlas back and front cells and their compose.
  - **S81 R33-R38**: the Live Effects Sweep, Glow Pulse, Tint and Glints, on `library:title_fx`.
  - **S81 R40**: keyable rows, `overrides.<name>` among them. **S81 R41**: the reserved clip event
    `titleArtPlay`. **S81 R44**: "S85 animations", with `titleart.animationTime` reserved.
  - **S81 R51**: always in core are the component, `TitleArtSystem`, the sidecar reader and the Lua
    class, ≤ 40 KiB arm64.
  - **S81 §6.6**: Lua `play(name, {speed, loop, unscaled})`, `stop()`, `isPlaying`,
    `animations`, `onAnimationFinished`.
  - **S82 §6.8**: `TitleDesignerWindow::addPanel(PanelDef{id, title, region, draw})`. **S82 §6.5**:
    `ShortcutContext::TitleDesigner` (Q/W/E/R/T are its tools).
  - **S80 §6.5 / S83 §6.2**: `FxAnim {None, Live, Rerender, Step, Relayout}` per parameter (S80
    declares it, on params and on `TitleArtSchema` members; S83 sets the values). **S83 D10**: `live.kind`
    `none | scale_split | kernel`, where "none" means a re-render, or a flipbook in S85 Baked.
  - **S83 R16**: no effect reads a clock; strobing effects use S85's reseed track.

  Pachislop has no time axis: `src/model/types.ts:1` reads "Document model (stills only — there
  is no time axis anywhere)".
- **Keyframe maths exists as pure functions.** `engine/core/animation/KeyframeSampling.h` provides:
  - `applyEase`;
  - `locateKey` (a cursor hint; the eased segment factor);
  - `sampleFloatRow` (Hermite when tangents are given);
  - `wrapClipTime` (Once, Loop, PingPong, ClampForever; no loop section);
  - `collectCrossedEvents` (forward, backward, loop and ping-pong crossings, once per crossing;
    it takes an `AnimationClip`, not raw times). Its cases are in
    `engine/tests/core/animation/EventCrossingTest.cpp` (`EventCrossing.*`).

  Related types and formats:
  - `ClipKey` (`AnimationClip.h:65-81`) is `{time, value[], ease, inTangent[], outTangent[],
    tangentMode}`.
  - The `.anim` key grammar is `[t, v]`, `[t, v, ease]` or `{time, value, ease, in, out,
    tangent_mode}` (`docs/animation-clips.md`, "Channels and values").
  - Eases are `EaseType` (`engine/core/action/Ease.h:14-50`). Wraps are `ClipWrapMode`
    (`ClipTypes.h:92-98`).
- **Property tracks animate component values.** `PropertyBindings.h` is "the one binding mechanism
  of clip property tracks". It has parametrized rows (`shaderUniforms.<member>`,
  `PropertyBindings.cpp:494`), and exported builds use the registry with no editor code. The
  Animation Player and S21's Animator play these tracks (S21 §12). There is no string
  `PropertyKind` (`ClipTypes.h:42-53`), so text cannot be keyed from a clip.
- **Events reach Lua two ways.** `AnimationEvent {functionName, args, state, time}`
  (`AnimationEvent.h`) goes to `onEvent` subscribers (`object/AnimationPlayer.h:300-301`). It also
  goes to the script method of the event's name (`LuaBinding::callLuaScriptMethod`,
  `script/LuaBinding.h:234`, called at `subsystem/ActionSystemPlayers.cpp:1326`).
- **Per-character animation exists for plain text.** `util/TextEffects.h` (Wave, Bounce, Shake,
  Pulse, Typewriter, FadeIn, Grow, Unfold, Rainbow, Gradient) staggers with `spread` ("TMP Effect
  CharacterDurationRatio"), on SDF text, not styled title glyphs.
- **The shine of `library:ui_fx` runs on engine time** (`shaderlib/library/ui_fx.frag:30`, `:89`),
  so a clip cannot place or scrub it (report e4 §3.3). S81's Sweep fixes that with a keyable
  `sweepPosition`.
- **Flipbooks today** are sprite frames: `SpriteComponent::framesRect`, a `HybridArray` of at most
  `MAX_SPRITE_FRAMES` = 128 (`component/SpriteComponent.h:38`, `Engine.h:105-106`). `stb_rect_pack`
  is vendored (`engine/libs/stb/stb_rect_pack.h`). `util/Fnv.h` has `fnv1a64`.
- **Texture cooking** (S14, `docs/textures.md:25-41`): `ui` at quality `normal` cooks to BC7
  (desktop, 1 B/px), ASTC 6x6 (mobile, 0.445 B/px), ETC2 RGBA8 (`android_etc2`, 1 B/px) and UASTC
  RDO 1 (web, transcoded to 1 B/px formats). UI defaults to no mips.
- **Editor widgets and windows.**
  - `editor/widget/CurveEditor.h` edits `Curve`s (`math/Curve.h`: Hermite keys, per-side tangent
    modes, no per-segment ease). It reports gestures as began / changed / ended / cancelled.
  - `Curve::fromEase(EaseType, keys)` turns an ease into keys within 0.005.
  - The Animation window's clip mode has a dopesheet, curves and Record
    (`editor/window/AnimationWindowClip*.cpp`, Record in `AnimationWindowClipRecord.cpp`).
  - Tangent editing on `ClipTrackData` is `editor/animation/ClipTangents.h`.
  - The Animation shortcuts are `animation.*` in `ShortcutContext::Animation`
    (`editor/shortcuts/DefaultCommands.cpp:339-354`: Space, K, `,` `.`, Alt+`,` Alt+`.`, Delete,
    Ctrl+C/V/D, F, A, R). The context enum is in `CommandCatalog.h:36`.
- **Contact sheets.** `render_pose_animation` takes `mode poses|motion|times`, `frames` and `times`
  (`docs/mcp/TOOLS.md`), the shape `render_title`'s filmstrip follows.
- **Audio.** S37 and S40 are not implemented: `engine/core/animation/ClipAudio.h` is absent and
  `ANIM_VERSION` is still 2 (`AnimationClipIO.h:55`). What exists:
  `AudioSystem::playOneShot(AudioSourceComponent&, filename, volumeScale)` and
  `playClipAtPoint(filename, position, volume, group)` (`subsystem/AudioSystem.h:196-200`).
  Since the merge of origin/main into feature/title-art, S37 and S40 have landed:
  `engine/core/animation/ClipAudio.h` holds the clip cues, `ANIM_VERSION` is 3, Sound Events
  play through `AudioSystem::playFree` (Lua `Audio.play`), and a cue starts through
  `AudioSystem::playFreeFromTimeline(eventOrClip, options, delayFrames)` with the delay from
  `TimelineAudio::delayFrames(fraction)` (S40 R5, R6); the editor previews cues with
  `TimelineAudioPreview` (Deviation 6).
- **Expressions.** `editor/util/NumericExpression.h` evaluates tinyexpr text for numeric fields,
  with no variables API.

## 4. Reference

### 4.1 Pachislop (`/tmp/pachislop-analysis/repo`, used with the author's permission)

Pachislop has nothing to port for time. What S85 takes is the set of parameters that become the
natural animation handles (report p4 §1.1-1.2, "Drive to animate"). S83 decides their classes.

| Pachislop | Where | S85 |
|---|---|---|
| Sweep band `position` of Gradient Overlay, the glint shine | `src/effects/a/stylize.ts:73-95` | Idle Shimmer uses S81's Sweep, which is free in every mode. The document's own `gradient_overlay` sweep animates with S83's class |
| Shatter `strength`, `rotation`, `seed`; layout once, pieces animated from `t` in a vertex shader (p4 §4.11 "Runtime") | `src/effects/b/shatter.ts:514-530` | Shatter In and Out. S83's live kernel in Realtime; a flipbook in Baked |
| `phase` of fire, smoke, corona and wave | `src/generators/energy/fire.ts:66`, `smoke.ts`, `corona.ts`, `src/effects/a/distort.ts` | A `drive` track (R6); `cycles` makes loops seamless |
| `seed` of grain, glitch, lightning and speed lines ("reseed every 2-3 frames", p4) | `src/effects/a/finish.ts:45,104`, `src/generators/energy/lightning.ts:419` | A `reseed` track (R7), as S83 R16 asks |
| Shockwave `radius`, `flash`; Sunburst `rotation` | `src/generators/rings/shockwave.ts`, `src/generators/lines/sunburst.ts` | Stamp In and Win Pulse options |

What we improve: time exists, and every random element stays a function of document data and time.
Two renders at one time are identical (decision 8).

### 4.2 Engine precedents reused

| Precedent | Use |
|---|---|
| `ClipKey`, the `.anim` key grammar, `EaseType` names | Title keys are `ClipKey`s in the same grammar |
| `locateKey`, `applyEase`, `sampleFloatRow` | The sampler calls them unchanged, so eases and tangents behave as clip tracks do |
| `collectCrossedEvents` rules, `EventCrossing.*` cases | `collectCrossedMarkers` keeps the rules and adds loop sections (D4) |
| `editor/animation/ClipTangents.h` | Gains overloads on `(std::vector<ClipKey>&, size_t width)`; the `ClipTrackData` versions forward to them with no behaviour change |
| `CurveEditor` | The panel's Curves view, one `Curve` per component (D12) |
| `AnimationWindowClipRecord.cpp` | Record semantics (R45) |
| `TextEffects` `spread` | The stagger's `over`; `order` adds what TMP lacks |
| `render_pose_animation` | `render_title`'s filmstrip (R53) |
| S81's live compose, scheduler and Glyph Atlas cells (R9, R13-R17, R26-R28) | Realtime and Glyph Atlas animation items are requests to them, not a second runtime |

Nothing is copied from another project, so `NOTICE.md` gets no S85 entry. Pachislop's entry is
S80's.

## 5. Requirements

### Functional — the document time model

- **R1. Animations.** The document's top-level `animation` member (§6.1) holds up to 32 named
  animations and a document `fps` (1..120, default 30: the snapping grid and the default flipbook
  rate). Each animation has:
  - `name`: unique, 1..32 characters, any Unicode except `/` and control characters;
  - `id`: 8 hex, in S80's id space;
  - `role`: `intro`, `idle`, `emphasis`, `win`, `outro` or `custom` (default). It sets the
    duration bounds of R54's checks and the panel's grouping;
  - `duration`: seconds, 0.01..60, or −1 for automatic (the last key time with stagger delays and
    the last marker time);
  - `wrap`: `once`, `loop` or `ping_pong`;
  - `loop_start`: 0 ≤ `loop_start` < `duration`;
  - `next`: an animation that starts when a `once` animation ends;
  - `overlay` (R25);
  - `speed`: 0.01..10;
  - the Realtime and Baked settings of R17 and R18.

  `animation.default` names what a Title Art Renderer plays on start when the component names
  nothing.
- **R2. Tracks.** A track targets one field. `layer` is a layer id, or `""` for the title (R5).
  `field` is an S80 field path, stored long form; ops accept the shorthands. A track holds exactly
  one of:
  - `keys`: `ClipKey`s, time-sorted, at most 4,096;
  - `drive` (R6);
  - `reseed` (R7).

  At most one track exists per (layer, field) per animation. A whole value and one of its
  components (`tint` and `tint.r`) cannot both have tracks; the ops refuse it with
  `component_conflict`, naming the existing track.
- **R3. Values and interpolation.**
  - Numbers, Vec2 and Color interpolate with the key's ease or its tangents. Tangents win over the
    ease, except `step`, as in `.anim`.
  - Colours are keyed `"#rrggbb"` and interpolate in sRGB-encoded channels, which is S80's
    gradient `rgb` rule.
  - Bools, ints, enums, strings (`text`) and AssetRefs (`font`, `image`) are stepped, with
    KeyframeSampling's step slack. An ease on them is dropped with a warning, as `.anim` does.
  - Sampled values are clamped to the field's range. A clamp is reported once per track
    (`key_clamped`, with the key index).
- **R4. Relative tracks.** `relative: true` combines the sampled value with the field's rest value,
  the document's value that the still shows:
  - **Added:** position, rotation, offsets, plain numbers, and `flash.amount` (its rest is 0).
  - **Multiplied:** scale, stretch, opacity, tint, and any field whose registry entry says
    `combine: multiply` (glow `intensity`, for example).
  - **Refused:** stepped fields (`relative_stepped`).

  Absolute tracks replace the rest value. Presets write relative tracks, so they apply to any
  title.
- **R5. Title-level and child tracks.**
  - **Title-level tracks** (`layer: ""`) target virtual fields of the whole title. Their rest
    value is the identity, and they are never written to the document:
    - `transform.position` (px, +y up);
    - `transform.rotation` (deg clockwise);
    - `transform.scale`, `transform.stretch` (Vec2);
    - `transform.pivot` (px from the frame centre; rest `[0, 0]`);
    - `opacity`;
    - `flash` (`{color, amount}`: `rgb += color × amount × alpha`);
    - S81's Live Effects: `live.sweep_position`, `live.sweep_angle`, `live.sweep_width`,
      `live.glow_pulse`, `live.tint_color`, `live.tint_amount`, `live.glint_amount` (track field
      paths in the document's snake_case; each writes S81's component value of the same meaning,
      `sweepPosition`, `sweepAngle`, `sweepWidth`, `glowPulse`, `tintColor`, `tintAmount`,
      `glintAmount`). Their rests are the instance's values.
  - **Child tracks** on a group target `child.<field>` of each child layer, with stagger unit
    `child` (R8).
- **R6. Drives** (continuous evolution: `phase`, rotations, offsets). A drive is
  `{from, to, cycles | rate, ease, wrap}`:
  - **`cycles` (> 0):** the value runs from `from` to `to` that many times over the loop span
    (`loop_start..duration`, or the whole duration of a `once` animation), shaped by `ease` within
    each cycle. The value at the span's end equals its start, so a looped drive has no seam.
  - **`rate` (units per second):** `value = from + rate × t`. With `wrap: true` the value wraps
    into `[from, to)`.
  - A drive on a relative track adds to the rest (`from: 0`).
- **R7. Reseeds.** A `reseed` track on an integer seed field (an effect, generator, mask, erode or
  texture `seed`, or the document `seed`) is `{rate, offset}`. The seed is
  `seed(t) = rest + floor(t × rate + offset)`, where `t` is the animation's wrapped local time.
  - Consumers mix seeds through `tfx::Stream` (S83 R15), so consecutive seeds look unrelated and
    one time always gives one seed.
  - A loop repeats its sequence every pass. Without a track, seeds are constant.
  - Stepped keys on a seed field are allowed too.
- **R8. Stagger.** A track on `glyph.<f>` (text layers) or `child.<f>` (groups) may have
  `stagger: {each | over, order, unit, seed, ease}`.
  - **`unit`:** `glyph` (grapheme clusters with ink; whitespace skipped), `word`, `line` (line or
    column) or `child`.
  - **`order`:** `forward` (logical reading order, also for RTL and vertical text), `reverse`,
    `center_out`, `edges_in`, or `random` (a Fisher-Yates permutation from SplitMix64 of the
    document `seed` and the stagger `seed`).
  - **Delays:** `delay_i = o_i × each`, where `o_i` is the unit's rank in that order. With `over`,
    `delay_i = over × ease(o_i / (n − 1))`; a single unit has delay 0.
  - **Reading the keys:** the keys describe one unit's motion. Unit `i` reads them at
    `t − delay_i`, holding the first key before it and the last after it.
- **R9. Glyph fields.** Text layers have per-glyph virtual fields with identity rests:
  - `glyph.offset` (Vec2, % of the layer `size`, +y up);
  - `glyph.rotation` (deg);
  - `glyph.scale`, `glyph.stretch` (Vec2);
  - `glyph.opacity`, `glyph.tint`, `glyph.flash`.

  They apply after layout, with no reflow (unlike S80's `chars.N.scale`), and on top of `chars`
  adjustments. They pivot per the track's `pivot`: the glyph's ink box `center` (default),
  `baseline`, `bottom` or `top`. The layer's `glyph_mode` (R15) decides how they render.
- **R10. Markers.** An animation has up to 64 markers `{time, name, args, sound, volume_db, group}`:
  - `name` is a Lua identifier. Names may repeat, and the ops address duplicates by `index`.
  - `args` holds at most 8 JSON scalars (`MAX_ANIMATION_EVENT_ARGS`).
  - `sound` is an AssetRef to an audio clip (`.wav .ogg .mp3 .flac`) or a `.soundevent` (S37).
  - `volume_db` is −60..12 (default 0). `group` is a mixer group (default `""`: the source's
    group, else Master).

  A marker past `duration` extends an automatic duration and is reported against an explicit one
  (`marker_after_end`).

### Functional — field classes and the Render Modes

- **R11. Classes.** S80's `TitleArtSchema` carries the `FxAnim` attribute per member (`live`,
  `rerender`, `step`, `relayout`), and S83's `ParamDef` already does. S85 derives a runtime class
  per track:

  | Class | From | Fields |
  |---|---|---|
  | **Title live** | S85's virtual title fields | R5's title-level list |
  | **Layer live** | S80 members marked `live`; S83 params marked L whose effect has `live.kind` `scale_split` or `kernel` | §6.2 |
  | **Glyph** | S85's virtual glyph fields; `child.*` of layer-live fields | R9 |
  | **Glyph Atlas live** | `text` | |
  | **Re-render** | `rerender`, `relayout` and `step` members (stepped values), and L params falling back per S83 D10 | everything else animatable |
  | **None** | identity and structure | ids, `type`, `name`, `locked`, `guide`, `collapsed`, `look`, `link`, `children`, `canvas.*`, `outputs`, `exposed`, `material_links`, `runtime`, list structure |

  - A track on a None field is refused with `not_animatable`, listing the layer's animatable
    fields.
  - A member registered without the attribute is re-render (safe: it costs, but it plays).
- **R12. What each mode does with each class.**

  | Class | Baked | Realtime | Glyph Atlas |
  |---|---|---|---|
  | Title live | companion draw (`title_fx` ANIM feature, R13; S81's Live Effects) | same | same |
  | Layer live | flipbook (R16) | S81 live compose (R14) | the glyph layer's: per-cell compose; plate layers: not played |
  | Glyph | flipbook | glyph-cell compose (R15) | native (S81 R44) |
  | Glyph Atlas live (`text`) | flipbook | re-render | live: cells swap (S81 R28) |
  | Re-render | flipbook | rate-limited re-render (R18), or `prerender` | plates: rate-limited re-render; the glyph layer's style: not played (R19) |

  The plan (`buildAnimPlan(doc, animations, mode)`, pure, in the module) records the result per
  track, with the reason for each fallback. The no-module Baked path needs no plan: its data file
  (R60) already says which tracks play on the companion and which animations have flipbooks.
- **R13. The companion draw.** S81's `library:title_fx` and `title_fx_sprite` gain the feature
  `ANIM` (S81 R35 defines it), with uniforms:
  - `animAffine0`, `animAffine1`: a 2×3 affine in frame-normalised space about the pivot;
  - `animColor`: the opacity, and the flash colour × amount.

  `TitleAnimPlayer` writes them from the title-level tracks each frame an animation with such
  tracks plays. S81's Live Effects are written as runtime values that are never saved, as a clip
  would write them (S81 R34: a keyed `sweepPosition` turns `sweepAuto` off while it plays). While a
  `live.*` track plays, its Live Effect is enabled at runtime with the instance's other settings.
  When nothing plays, the feature is off and S81 R48 (an idle title costs an Image) holds.
- **R14. Layer live in Realtime.** A layer with layer-live tracks becomes a **layer split**: S80's
  renderer keeps the layer's own composite as a cached split, and S81's live compose draws it
  every frame with the sampled transform, opacity, tint and flash, in z order with the S83 L
  kernels. The split is exact only when nothing else renders from the layer's pixels:
  - no layer mask names it;
  - no generator `follow`s it;
  - no clipped layer uses it as base;
  - no adjustment layer is above it at its level or an enclosing one (S80: an adjustment changes
    everything below it);
  - no enclosing group has an enabled effect.

  When a rule fails, the layer's tracks play as re-render class, and the plan says which rule.
  Composing parts costs against S81's per-title live budget (0.5 ms mobile and web, 1 ms
  desktop). Over it, the frame's change falls back to a re-render, as S81 R9 does.
- **R15. Glyph cells.** A text layer with glyph tracks and `glyph_mode: cells` (the default)
  animates through S81's Glyph Atlas cells:
  - each glyph is rendered once as a back cell and a front cell (S81 R26);
  - the compose (S81 R28) draws them per frame with the sampled per-glyph values, back cells then
    front cells, so rims stay behind every face;
  - the other layers of the title are its back and front plates.

  In Realtime, the title shows the merged render at rest and switches to the cell compose while a
  glyph animation plays, crossfading over 0.1 s each way (S81's CROSSFADE). The `anim_glyph_merge`
  check measures the difference at rest. With `glyph_mode: merged`, glyph transform fields are
  re-render class and keep cross-glyph merging; `glyph.opacity`, `.tint` and `.flash` are then
  refused (`glyph_color_needs_cells`).
- **R16. Baked flipbooks.** An animation with any track that is not title live bakes a
  **flipbook**: whole-title frames that the companion shows by texture rect (S81 R44). Title-live
  tracks still play on the companion over it.
  - **Frame times.** Frames sample at `k / flipbook.fps` (default the document fps, at most 60). A
    `once` animation gets frames 0..⌈D × fps⌉ inclusive. A loop gets frames covering `[0, D)`, and
    its loop section wraps with no duplicate frame. At most 240 frames.
  - **Frame size.** It is the union of the frames' alpha bounds (1 px padding) at
    `flipbook.scale` (0.25..1, default 1) of the main output's scale. Every frame has that size,
    and the ANIM affine maps the still's frame onto it, so the companion's RectTransform stays
    as authored.
  - **Storage.** Frames identical to an earlier one share a rect: `fnv1a64` over the pixels, then
    a byte compare. Frames are packed by `stb_rect_pack` on 4-px-aligned positions with 4-px
    gutters, so block compression never mixes two frames. Pages are
    `<file>.<animation id>.flip<N>.png`, at most `flipbook.max_page` (1,024, 2,048 or 4,096;
    default 2,048) per side, each side a multiple of 4.
  - **Import settings.** Pages are ordinary Textures with a `.meta` written by the bake: S14
    `type: ui`, `mipmaps: false`, `quality: normal`, `alphaIsTransparency: true`,
    `maxSize: 4096`. They carry S80's recipe chunk plus the animation's hash, so a changed
    animation marks them stale.
  - **Playback.** The companion shows frame `⌊t × fps⌋`. With `flipbook.frame_blend: true`,
    `title_fx` mixes two frames by the fraction (the ANIM feature's second texture rect, one more
    sample).
  - **Live Effects over frames.** Sweep, Tint and Glints apply over flipbook frames. Glow Pulse
    needs a glow output, which frames do not have, so a `live.glow_pulse` track in such an
    animation is reported (`anim_mode_support`) and holds 0 while frames play.
- **R17. Flipbook settings per animation:** `flipbook {fps, scale, max_page, frame_blend}` (§6.1).
- **R18. Realtime re-render.** Per animation, `rerender` is `live` (default) or `prerender`.
  - **`live`.** Re-render tracks become one S81 request per sample time `k / rerender_fps`
    (`rerender_fps` 1..60, default 30). S81's scheduler runs them double-buffered and time-sliced
    (S81 R12-R17). A request that would start while the previous one is under half done replaces
    it (S81 R12), so frames drop and the clock never stretches. Title-live, layer-live and glyph
    tracks still update every display frame.
  - **Budget.** While it animates, a title may spend its **share** of S81's frame budget each
    frame: the budget divided by the number of animating visible titles, at least 25 % of it.
    This is S81 R44's "per-title share".
  - **`prerender`.** R16's flipbook is rendered at runtime into RGBA8 pages: on `prepare`, and
    again after a text or parameter change. It is time-sliced like any request. Its pages count
    against S81's Cache Budget (16 MiB mobile and web, 64 MiB desktop, S81 R20) at 4 B/px. When
    the pages cannot fit, the animation plays `live` and warns once.
- **R19. Glyph Atlas rules.**
  - Title-live, glyph and `text` tracks play. Layer-live tracks on the glyph layer apply to all
    its cells.
  - Plate layers' re-render tracks re-render that plate per R18's `live` rule (S81 R25 renders
    plates per state).
  - Re-render tracks on the glyph layer's style are not played, because they would rebuild the
    atlas every sample. They hold their rest values, draw `rerender_in_glyph_atlas` in the
    Designer, and fail `anim_mode_support` with fix ops (switch the Render Mode, or remove the
    track).
  - Glyphs missing from the Characters set are reported as S81 R55 reports any text change.
- **R20. One answer to "what does it cost".** `estimateAnimCost(animation, mode, family)` returns
  what the panel badge, `get_title`, `check_title` and the export dry run show:
  - per class, the tracks that play, fall back, or are not played;
  - Baked: flipbook frames (unique and total), pages, and GPU bytes at the family's cook format;
  - Realtime: layer splits and their compose estimate against the live budget; re-render cost per
    sample (S81 §6.3's model: pixels × `cost_weight` × platform factor) and the re-render fps it
    reaches under R18's share; prerender bytes;
  - Glyph Atlas: cells and compose quads.

  It lives in the editor, beside the plan. `<file>.anim.json` stores the Baked figures so the
  Inspector shows them without the module.

### Functional — Animation Presets

- **R21. The library.** At least the 28 presets of §6.4 ship in
  `editor/resources/titleart/animation-presets/`, in four groups: In, Emphasis, Loop and Out. Each
  preset has:
  - an `id`, a `name`, a `group`, a `role` and a default duration;
  - parameters (`number`, `bool`, `enum`, `color`, `sound`) with ranges;
  - a target rule: `title`, `text_layers`, `main_text` (the first target of the exposed `text`,
    else the topmost text layer), or `layers` given by the caller;
  - tracks whose times and values may be tinyexpr expressions over `D` (duration), `n` (units),
    `fps` and the parameters;
  - key generators (`shake`, `flicker`, `beats`, `ticks`) and markers;
  - `adds`: the effects or generator layers it creates when missing (S83 ids);
  - the class each track will have per mode, which `list_title_library` shows.

  A preset whose S83 type is not registered applies without that part, with problem
  `preset_part_unavailable`.
- **R22. Applying.** `apply_animation_preset` writes ordinary tracks and markers into an
  animation:
  - each track it writes carries `from_preset`, and the animation keeps `preset_source {id,
    params}`;
  - **Edit Preset Parameters** (the panel, or the same op again) regenerates only that preset's
    tracks, as one undo step. Manual edits to them are replaced, and the panel asks first;
  - `mode: merge` (default) leaves other tracks alone, while `mode: replace` first removes every
    track on the preset's targets;
  - `at` (seconds) offsets it, so presets chain in one animation.
- **R23. User presets.** **Save Animation as Preset** converts the current animation:
  - absolute tracks become relative against the rest values, or are refused with a reason;
  - times become `D`-relative;
  - targets become `title`, `main_text` or explicit names.

  Presets are saved in `<config dir>/title-library/animation-presets/` (user) or
  `<project>/EditorSettings/TitleLibrary/AnimationPresets/` (project), S84's library scopes (S80
  §6.15). The
  format is §6.8.

### Functional — playback and integration

- **R24. The player.** `TitleAnimPlayer` (always built, owned by S81's `TitleArtSystem` instance
  state) runs each frame in S81's system, after the ActionSystem and the AnimatorSystem, so clip
  values apply in the same frame. It:
  - samples the base animation and the overlays;
  - writes the companion uniforms and Live Effect values (R13) and the flipbook frame (R16);
  - hands Realtime and Glyph Atlas items to the module (R14, R15, R18);
  - collects crossed markers.

  `play` applies the time-0 pose before the frame draws, so an intro that starts invisible never
  flashes the still.
- **R25. Base and overlays.**
  - At most one base animation plays, and `play` replaces it.
  - Overlay animations (`overlay: true`, or `playOverlay`) play on top in any number. Their tracks
    combine as relative tracks over the base pose, whatever their own flag.
  - On one absolute field the later-started overlay wins.
  - Overlays end on their own (`once`) or with `stop(name)`.
  - In Baked mode an overlay plays its title-live tracks only. Its other tracks would need frames
    composited over the base's frames, which one companion draw cannot do. The ops warn
    (`overlay_needs_title_live`) and `anim_mode_support` reports it.
- **R26. Ends, chains, seeks and fades.**
  - **End.** A `once` animation that reaches its end holds its last pose and fires
    `onAnimationFinished(name)`. With `next`, the next animation starts in the same frame at the
    overflow time `t − D`.
  - **Stop and reset.** `stop` keeps the pose (Unity's rule). `reset()` returns to the rest
    values, where the title is idle again. Setting `animationTime` seeks and fires no markers.
  - **Fades.** `play(name, {fade = s})` blends title-live, layer-live and glyph values linearly
    from a snapshot of the current values over `s` seconds. Re-render values and flipbooks switch
    at once.
  - **Clock.** It is the scaled delta, or unscaled with `unscaled` / `animationUnscaledTime`.
- **R27. Clips and the Animator.** S81 reserved the row name `titleart.animationTime`. S85
  registers it with `animationSpeed` (Float), next to S81's `overrides.<name>` and Live Effect
  rows:
  - **`animationTime`** (Float) reads as the base animation's time. A write seeks.
  - **`driveAnimation`** (a component field, §6.5). When it names an animation, the component
    does not advance time itself: it shows that animation at `animationTime`, so a clip or an
    Animator state scrubs the title. The Animation window's preview then previews it exactly.
    Markers fire when the driven time moves forward between frames, never backward.
  - **`titleArtPlay`**: S81 R41's reserved clip event starts an animation without a script.
  - **Exposed parameters** take the strictest class of their targets. A clip on a re-render
    override of a Baked title does nothing, as S81 R7 says.
- **R28. Markers to Lua.** After sampling, crossed markers of the base and the overlays fire in
  time order as `AnimationEvent {functionName = marker name, args, state = animation name, time}`:
  - C++ subscribers, then `TitleArt.onAnimationEvent`, then the GameObject's script method of
    that name (`LuaBinding::callLuaScriptMethod`, as the Animation Player does);
  - then `onAnimationFinished`.

  Calls inside them take effect next frame. The crossing rules are D4's: once per crossing per
  pass, `includeStart` at a start, none on seeks.
- **R29. Marker sounds.** A crossed marker with `sound` plays:
  - **With an Audio Source** on the GameObject: `AudioSystem::playOneShot` on it, with its group,
    2D/3D settings and `volume_db`.
  - **Without one:** `playClipAtPoint` at the scene camera's position, on `group`.
  - **Timing.** Sounds start through S40's `AudioSystem::playFreeFromTimeline` with S40 R5's
    sub-frame placement (`TimelineAudio::delayFrames` of the marker's crossing fraction).
  - **`.soundevent` refs** play through S37's runtime (`AudioSystem::playFree`, Lua `Audio.play`).
  - An unresolved sound warns once per marker per Play.
- **R30. Unknown names.** `play`, `playOverlay`, `stop(name)`, `sample`, `prepare` and
  `driveAnimation` with an unknown animation warn once per name per component (S21's rule). They
  return false and change nothing.
- **R31. Determinism.** For one document, mode and time, sampling is bit-identical on one
  platform. The player never reads wall-clock time. Under `--fixed-fps` runs show the same frames
  and fire the same markers in the same frames every time.

### Platform & backends

- **R32.** Every path runs on GL 3.3, GLES3/WebGL2, Metal, D3D11 and Vulkan, and on the six export
  targets. That covers the ANIM uniforms, flipbook frames by texture rect, layer splits and glyph
  cells in S81's compose, re-render requests and prerender pages. Nothing uses compute, SSBOs or
  features WebGL2 lacks. The ANIM feature adds 3 `vec4` uniforms within `title_fx`'s GLES3 limits
  (S81 R46).
- **R33.** On S80's RGBA8 path (no renderable float), plans and frame times are the same. Frames
  match the float path within S80's stated tolerance, tested by forcing the fallback.
- **R34.** Web without threads: the bake is editor-only; the player allocates nothing per frame and
  needs no worker. Realtime items run inside S81's main-thread slices.

### Performance (server E Release unless said; tiers as S81 §6.8)

- **R35. Sampling.** `TitleAnimBench.Sample64` is a 64-glyph title with one animation of 40 tracks
  (8 staggered glyph tracks, 4 drives, 2 reseeds) and 8 markers. Sampling plus applying the pose
  takes ≤ 25 µs median, with 0 heap allocations after warm-up (600 frames).
- **R36. Baked scene.** `TitleAnimBench.Scene30` is 30 Baked titles on screen, each playing a
  flipbook plus title-live tracks plus one overlay. `TitleArtSystem` plus the player take
  ≤ 0.5 ms p95 on the main thread, with +0 draws versus 30 Images: the ANIM feature and the frame
  rect live in the companion's one draw.
- **R37. Realtime.**
  - A live rim-width pulse of S81's `reference.titleart` (1,024 × 512) at `rerender_fps` 30
    keeps every frame within S81's frame budget plus 0.5 ms (S81 R15). It is measured on the GPU
    lane (server H) and as the static model on server A.
  - Layer splits and glyph-cell compose of a 9-glyph cascade fit the per-title live budget of S81
    R9 on the mobile tier (static model).
  - A glyph cascade on the `counter` fixture in Glyph Atlas meets S81 §6.8's Glyph Atlas update
    row.
- **R38. Flipbook memory.** A document's flipbook pages fit
  `title_art.animation.flipbook_budget_mib` per cook family: desktop 32 MiB, mobile 8,
  android_etc2 8, web 8. Bytes are counted at R16's import settings: BC7 1 B/px, ASTC 6x6
  0.445 B/px, ETC2 RGBA8 1 B/px, web 1 B/px, no mips. The still is not counted (S81 counts it).
- **R39. Editor.**
  - The panel draws 200 tracks × 20 keys in ≤ 1 ms per frame.
  - Scrubbing shows title-live, layer-live and glyph values in the same frame. Re-render values
    follow through S80's time-sliced renderer, keeping the previous image (contract decision 11).
  - **Preview As Baked** plays from a preview flipbook cache. It is built on the background lane,
    the visible animation first, and keyed by content hash and engine version.
- **R40. Bake.** 60 frames of `reference.titleart` bake in ≤ 6 s on server H and ≤ 60 s on
  llvmpipe. The bake is an `McpJobs` job; cancelling it leaves no partial files.

### Editor UX (human)

- **R41. The Animation panel.** It is registered with `TitleDesignerWindow::addPanel(PanelDef{
  "animation", "Animation", bottom, ...})` (S82 §6.8), and opens by default once a document has
  animations.
  - **Toolbar:**
    - animation dropdown (+ New Animation, Duplicate, Rename, Delete, Save as Preset...);
    - Play/Pause, Stop, Loop Preview, Go to Start/End, Previous/Next Frame and Key;
    - time field (seconds, or frames with the Frames toggle);
    - Record (red);
    - Add Preset (§6.4 by group, with a hover preview on the document);
    - Onion Skin;
    - **Preview As** (Exact | Baked | Realtime | Glyph Atlas);
    - the cost badge (R44).
  - **Settings row:** duration (Auto), wrap, loop start, next, overlay, speed, role, fps, the
    flipbook fps and scale, and the Realtime policy.
- **R42. Dopesheet.**
  - **Track list:** grouped by layer in Layers-panel order, with Title first. Each row has:
    - the field's label from the registry ("Position", "Rim 1 Width", "Outer Glow Intensity");
    - its class icon;
    - a mute toggle;
    - the value at the playhead. In Record, editing it keys; otherwise it edits the key at the
      playhead, or the rest value when there is none.
  - **Keys:** diamonds, with each segment's ease drawn as a curve glyph. Stagger shows as a fan of
    faded ghost keys per unit.
  - **Lanes:** a marker lane above (flags, a speaker icon on sound markers), a ruler, the loop
    section bar, and a draggable end handle.
  - **Key editing:**
    - double-click adds a key with the sampled value;
    - drag moves keys, snapped to the fps (Alt: free; Shift: lock time or row);
    - box select; dragging a box edge scales time about the opposite edge;
    - Delete, Ctrl+C, Ctrl+V at the playhead, Ctrl+D.
  - **Key menu:** Ease (Linear, Step, In/Out/In Out × Quad..Bounce), Tangents (Clamped Auto, Auto,
    Free, Flat, Broken, Linear, Constant), Apply to Selection.
  - **Track menu:** Relative, Stagger..., Pivot, Convert to Drive, Remove.
- **R43. Curves.** The Dopesheet | Curves toggle shows the selected tracks in `CurveEditor`, one
  curve per component (D12). Eased segments other than linear and step show read-only, with
  **Convert Eases to Curves**: one undo step, `Curve::fromEase` keys within 0.005. Tangent edits go
  through the `ClipTangents` overloads.
- **R44. Class and cost, visible.**
  - **Class icons:**
    - title or layer live: `ICON_FA_BOLT`, green;
    - glyph: `ICON_FA_FONT`, green;
    - re-render: `ICON_FA_ROTATE`, amber;
    - not played in the Preview As mode: `ICON_FA_BAN`, red.

    The tooltip says why, for example: "Rim 1 Width re-renders the layer. Baked: flipbook frames
    (1.9 MiB on Mobile). Realtime: 30 re-renders per second, 3.1 ms each on Mobile."
  - **Cost badge:** R20 for the selected instance's Render Mode, or every mode, coloured by R37
    and R38. Clicking it runs the animation checks into S82's lint panel.
- **R45. Record** (Unity's record mode).
  - With Record on, edits of animatable fields key them at the playhead in the selected animation
    instead of changing the rest value. That covers the properties panel, the canvas gizmo and the
    value column. `TitleAnimRecord::rewrite(batch)` turns the Designer's `set` ops into `set_key`
    ops before `TitleArtOps` commits.
  - One gesture is one undo step, named "Record Key: Title/Main Scale" (S24).
  - A field without a track gets one: a key at the playhead and, when the playhead is past 0, a
    key at 0 with the rest value, so the motion starts from the still.
  - Keyed fields show a red tint during Record and a blue one when animated outside it.
  - Non-animatable edits during Record change the document and say so in the status line.
  - Record has its own context (R48's keys), so the Designer's R tool key is untouched.
- **R46. Scrub and preview.**
  - The ruler scrubs, and the canvas shows the sampled pose. Play runs in real time, looping with
    Loop Preview.
  - **Preview As** shows what the runtime shows:
    - Baked plays the preview flipbook at its fps and scale, with stepping and softness visible;
    - Realtime plays re-render at its rate;
    - Glyph Atlas omits unplayed tracks.
  - **Sound markers** play during preview through an editor preview voice outside the mixer groups
    (S40's `TimelineAudioPreview`), muted by the
    panel's speaker toggle. Scrubbing is silent.
- **R47. Onion skin** (Preferences > Title Designer; off by default): 2 frames before and 2 after
  at 1/fps, drawn as ghosts (red-tinted before, green-tinted after, 25 %). Ghosts use title-live,
  layer-live and glyph poses, plus cached re-render frames where present. They never trigger
  renders.
- **R48. Undo and keys.**
  - Every gesture (move keys, scale time, ease, preset, rename, record) commits one `TitleArtOps`
    batch: one named project undo step.
  - Drags preview live on a working copy and commit on release. Esc reverts (contract decision 10).
  - Undo while playing pauses the preview.
  - The keys are §6.10's, in `ShortcutContext::TitleAnimation` (active while the panel has focus).
- **R49. Inspector and Project window.**
  - The `.titleart` asset summary (S80) lists animations with duration, wrap and the cost per mode.
  - S81's Title Art Renderer section gains rows: Play Automatically, Default Animation (dropdown),
    Animation Speed, Animation Update (Scaled / Unscaled), Drive Animation (dropdown, "None").
  - An edit-mode **Preview** button plays the default animation in the Scene and Game views and
    restores on stop, with `PreviewOwner` semantics like the Animation window.
  - When markers carry sounds and the GameObject has no Audio Source, the section warns: "Marker
    sounds play 2D on Master without an Audio Source [Add Audio Source]".

### Agent UX (MCP, Lua, CLI)

- **R50. `edit_title` ops** (§6.7.1) go through `TitleAnimOps`, with S80's all-or-nothing batches,
  `dry_run` and `expect_rev`.
  - **Errors** list valid alternatives:
    - unknown fields: did-you-mean and `valid_here`;
    - unknown presets: the nearest ids;
    - `not_animatable`: the layer's animatable fields.
  - **Results** add `animation_changes: [{animation, track, keys, class}]` and the R20 cost of each
    animation the batch touched.
- **R51. `get_title`** gains `animation` (a name, or `"*"`): that animation in full, with keys in the
  compact grammar. The outline carries `animations: [{name, role, duration, wrap, tracks, markers,
  classes, cost}]` whenever the document has animations.
- **R52. `list_title_library`** gains `kind: "animation_presets"`. It has the tiers of S80's listing:
  rows `{id, name, group, role, duration, classes, use}`; `id` for the parameters as lines; and,
  with S84's `sheet`, a 5-frame filmstrip of the preset on the caller's text.
- **R53. `render_title`** gains `options.animation` (a key of S80's `options`), an object `{name, time | times (≤ 24) | frames
  (2..24), as}`. `as` is `exact` (default), `baked`, `realtime` or `glyph_atlas`.
  - It renders one frame, or a filmstrip in S80's sheet layout with `columns`, each cell labelled
    with its time.
  - `frames` spreads evenly over the duration, or over the loop span for loops.
  - Each cell carries `{time, alpha_bounds, coverage_pct, changed_pct}`, where `changed_pct` is
    against the previous cell. With `image: "none"`, an agent reads the motion as numbers.
  - `try` and `compare` work at that time.
- **R54. `check_title`** runs the animation checks of §6.7.2 by id (`checks`), and with `anim` for
  all of them. A new `animation` argument restricts them to one animation. Results follow S80
  §6.12: PASS/WARN/FAIL with `measured`, `threshold`, `at` (times and crops) and fix ops. A check
  never claims aesthetic approval.
- **R55. `export_title`** gains `animations` (names, default all).
  - With `dry_run`, it reports per animation the frames (unique and total), pages and bytes per
    family against R38, and writes nothing.
  - Otherwise it bakes the flipbook pages, their `.meta` and `<file>.anim.json` with the outputs
    through S80's bake writer: derived files, not an undo step (S80 §6.9), all or nothing, and as a
    job past 1 s.
  - Flipbook settings are document members set with `set_animation`, the way S80 configures
    outputs with ops.
- **R56. Guide.** `docs/mcp/guide/title-art.md` gets `### Animation` (`jasper://docs/title-art`):
  - the ops and their fields, the key grammar, the class and mode table, stagger and drives, the
    presets, the check ids;
  - the agent loop: preset, then filmstrip with `image: "none"`, then `check_title {checks:
    ["anim"]}`, then `export_title {dry_run: true}`, then export;
  - a Lua recipe.

  The agent guide itself gets no new bytes; S80's `title art (edit_title)` entry covers the area.
- **R57. CLI.** Every tool works through `jasperengine-editor call`, and Lua through `eval_lua`.
  `jasperengine-editor check --titles` runs the document-kind animation checks without a GPU.

### Data & compatibility

- **R58. No version bump.** A document without `animation` loads and renders exactly as before.
  `animation` and S85's additions to S80's model are optional members with identity defaults, so
  `TITLE_ART_VERSION` is unchanged (S80 migration rule 1). The additions are:
  - `transform.stretch` `[1, 1]`;
  - layer `tint` `"#ffffff"`;
  - layer `flash` `{color: "#ffffff", amount: 0}`;
  - text `glyph_mode` `"cells"`.

  `glyph_mode` matters only for a layer with glyph tracks.
- **R59. Lenient read**, following S80's codec rules:
  - canonical text; unknown members kept; limits clamped (R1, R2, R10);
  - a bad track, key or marker is dropped with a warning and its JSON path
    (`animation.animations[1].tracks[3].keys[2]: expected [time, value]`);
  - a track whose layer id or field is gone is kept, marked `orphan`, not played, and listed by
    `get_title`. An undo of the layer's removal brings it back to life.
- **R60. `<file>.anim.json`** (format `jasperengine.titleanim`, version 1) is written by the bake
  for a document with animations:
  - per animation: the name, settings, compiled title-live tracks (keys as authored), markers,
    flipbook index (pages with GUIDs, frame rects, the frame rect in frame space) and R20's Baked
    figures;
  - `source_hash` and `bake_version` like S81's sidecar.

  Baked mode reads it with S81's sidecar and the PNGs. Realtime and Glyph Atlas read animations
  from the shipped `.titleart` and use the file only for its Baked fallback. A newer version is
  refused with a message naming the build. A stale one plays and is reported (S80 `bake_fresh`).
  It ships with the title in every mode.
- **R61.** Animation ids, preset ids and marker names in the library never use franchise or brand
  names. `test_title_animation_presets.py` checks S84's denied-terms list.
- **R62.** Renames and moves of sound assets relink marker refs through S04 (dependency field
  `animation.animations[i].markers[j].sound`), in the rename's undo step.

## 6. Design constraints & decisions

- **D1. The animation lives in the document.** It is not a separate `.anim`. A title's motion is
  part of its design, and presets, checks and the bake need document fields. Clips drive it from
  outside through `animationTime` (R27), so S40 D1 (clips are the scene's timeline) holds.
- **D2. Keys are `ClipKey`s.** Sampling calls `locateKey`, `applyEase` and `sampleFloatRow`
  unchanged, and the grammar is `.anim`'s. Rejected alternatives:
  - `Curve` per component: it has no per-segment ease, and "back_out on this key" is what
    designers and agents ask for;
  - a new key type: two maths to keep equal.
- **D3. Wrap with a loop section.** `wrapAnimationTime(t, D, loop_start, wrap)`. For `t ≥ D`:
  - `loop` gives `loop_start + fmod(t − loop_start, D − loop_start)`;
  - `ping_pong` runs back and forth over `[loop_start, D]`;
  - `once` clamps to `D`.

  The first pass always plays `0..D`, so "punch, then pulse forever" is one animation (Win with
  `loop_start` after the punch). `wrapClipTime` is unchanged.
- **D4. Marker crossing.** `collectCrossedMarkers(compiled, from, to, includeStart, out)` applies
  `collectCrossedEvents`' rules on the unwrapped time line, split at the end of the first pass and
  at each later pass of the loop section:
  - forward fires `from < t ≤ to`, backward `to ≤ t < from`;
  - each marker fires at most once per pass;
  - a marker before `loop_start` fires in the first pass only.

  The `EventCrossing.*` cases are the oracle.
- **D5. Classes are data on fields.** S80's `FxAnim` on every S80 member and S83 param, not a
  hand-kept list. S85 adds only its virtual title and glyph fields, and the mode rule (R12).
- **D6. The companion stays one draw.** S81's design (one Image or Sprite draw, idle = Image)
  decides the shape here. Whole-title motion is uniforms on that draw. Anything finer is S81's
  compose in Realtime and Glyph Atlas, or whole-title flipbook frames in Baked.
  Rejected: shipping per-layer parts for Baked titles. It would need a compose pass, a display
  target and the module's shaders in Baked players, which contract decision 4 and S81 R51 keep
  out. The cost readout and the checks point heavy Baked animations at Realtime or Glyph Atlas
  instead.
- **D7. Most motion is title live.** With default parameters, 19 of the 28 built-in presets are
  title live only: whole-title transforms, opacity, flash, and S81's Sweep, Glow Pulse and
  Glints. They cost +0 draws and no memory in every mode. That is the design target: the presets
  a game uses most never cost a flipbook.
- **D8. Glyph cells for glyph animation.** S81's back/front cells keep rims behind every face, so a
  cascade stays close to the merged look without per-frame re-rendering. `merged` remains for
  titles where the difference shows (`anim_glyph_merge`).
- **D9. Realtime re-render is rate-limited.** Contract decision 2 (re-render on change,
  time-sliced) holds: an animated re-render field is a change every `1/rerender_fps`, run by S81's
  scheduler under R18's share. Display-rate motion comes from the live classes. That is the only
  way a rim-width pulse runs on a phone without breaking the frame.
- **D10. Overlays instead of layers.** Shake and flash over an idle are what titles need. Relative
  combination needs no weights or masks.
- **D11. Title-level virtual fields.** Presets move "the title", whatever its layers. Wrapping
  layers in a group would change the document, while virtual fields with identity rests are free
  on the companion draw.
- **D12. Curves view adapter.** `TitleAnimTimelineOps::toCurve(track, component)` copies tangents,
  turns linear segments into linear tangents and step into Constant. `fromCurve` writes keys with
  tangents and `tangentMode`. Eased segments are read-only until converted (R43), so `CurveEditor`
  learns nothing about eases.
- **D13. Presets are data with expressions.** `NumericExpression` gains `evaluateWith(text,
  variables)` (tinyexpr `te_variable`s). Key generators (`shake`, `flicker`, `beats`, `ticks`) are
  the only preset code. They are seeded and deterministic.
- **D14. Text keys where they are cheap or honest.** `text` is Glyph Atlas live (counters and
  countdowns). It becomes frames in Baked, which fits a known countdown, and re-renders in
  Realtime. Clips cannot key text (no string `PropertyKind`); S81's `titleArtSetText` event or Lua
  does it.
- **D15. Sounds wait for S40 for sample accuracy.** Frame quantization (≤ 16.7 ms at 60 fps) is
  acceptable first (report e4 §3.4). Markers already hold what S40 needs, so the switch changes no
  documents.

### 6.1 The `animation` member (inside `.titleart` v1)

```json
"animation": {
  "fps": 30,
  "default": "Intro",
  "animations": [
    {
      "id": "7c1e0a42", "name": "Intro", "role": "intro",
      "duration": 0.8, "wrap": "once", "loop_start": 0, "next": "Idle", "overlay": false, "speed": 1,
      "rerender": "live", "rerender_fps": 30,
      "flipbook": {"fps": 30, "scale": 1, "max_page": 2048, "frame_blend": false},
      "preset_source": {"id": "stamp_in", "params": {"from_scale": 2.6, "shockwave": false}},
      "tracks": [
        {"layer": "", "field": "transform.scale", "relative": true, "pivot": "origin", "mute": false,
         "from_preset": "stamp_in", "keys": [[0, 2.6, "quad_in"], [0.48, 0.94, "sine_out"], [0.8, 1]]},
        {"layer": "", "field": "live.sweep_position", "relative": false, "pivot": "origin", "mute": false,
         "keys": [[0.5, 0, "sine_in_out"], [0.8, 1]]},
        {"layer": "3fa0b2c1", "field": "glyph.offset", "relative": true, "pivot": "baseline", "mute": false,
         "stagger": {"each": 0.04, "order": "forward", "unit": "glyph", "seed": 1, "ease": "linear"},
         "keys": [[0, [0, 60], "back_out"], [0.3, [0, 0]]]},
        {"layer": "9b0d4e11", "field": "effects[0].params.phase", "relative": false, "pivot": "origin", "mute": false,
         "drive": {"from": 0, "to": 10, "cycles": 1, "rate": 0, "wrap": false, "ease": "linear"}},
        {"layer": "9b0d4e11", "field": "effects[1].params.seed", "relative": false, "pivot": "origin", "mute": false,
         "reseed": {"rate": 12, "offset": 0}}
      ],
      "markers": [
        {"time": 0.48, "name": "onImpact", "args": [], "sound": {"path": "sounds/stamp.wav", "guid": "…"},
         "volume_db": 0, "group": ""}
      ]
    }
  ]
}
```

- **Writing.** Members appear in the order shown. Every member is written, defaults included
  (S80's canonical rule), except `stagger`, `drive`, `reseed`, `preset_source`, `from_preset` and
  `sound`, which are written only when present.
- **Addressing.** Track `layer` is a layer id or `""`, as in S80 exposed targets, and `field` is
  the long form.
- **Order.** Tracks sort by (layer order in the document, field), markers by (time, name).
- **`pivot`.** On layer and title tracks: `origin` (the transform origin, or the title pivot),
  `center`, `bottom`, `top`, `left` or `right` of the rest ink bounds. On glyph tracks: `center`,
  `baseline`, `bottom` or `top`.
- **Limits:** 32 animations; 512 tracks per animation; 4,096 keys per track; 64 markers; duration
  ≤ 60 s; stagger `each` ≤ 2 s and `over` ≤ 30 s.

### 6.2 Classes of S80's members (the `FxAnim` values S80's `TitleArtSchema` carries)

| `FxAnim` | S80 members |
|---|---|
| `live` (layer live) | `transform.position`, `.rotation`, `.scale`, `.stretch` (new), `.flip_x`, `.flip_y` (stepped); `opacity`; `tint` (new, multiply); `flash` (new); `visible` (stepped) |
| `step` | `font`, `image`, `svg`, enums (`blend`, `fill.kind`, ...), bools other than `visible`, ints that do not interpolate |
| `relayout` | `text`, `size`, `tracking`, `line_height`, `kerning`, `align`, `vertical`, `proportional`, `h_scale`, `v_scale`, `shear`, `chars.*`, `lines.*`, `warp.*`, `distort.*`, `weight`, `variations.*`, shape geometry, image and vector `size` |
| `rerender` | every other number, colour, gradient and curve: `style.*` (face, rims, materials, bevel, erode), mask members, adjustment members, document `seed`, `global_light.*` |

S83's parameters keep the classes their entries state. An L param animates live in Realtime only
when its effect's `live.kind` is `scale_split` or `kernel` and S83 D10's suffix rule holds;
otherwise it is re-render, with D10's reason. `text` is `relayout`, and S85 plays it as Glyph
Atlas live in Glyph Atlas mode.

### 6.3 One title, three modes

Rays (sunburst, `gen.rotation` drive) · Plate (no tracks) · Title (text, glyph cascade in Intro,
Rim 1 Width pulse in Win) · Idle Shimmer uses `live.sweep_position`.

| Animation | Baked | Realtime | Glyph Atlas (Title is the glyph layer) |
|---|---|---|---|
| Intro (title scale, glyph cascade) | flipbook (cascade) + scale on the companion | glyph-cell compose + scale on the companion | cells + scale |
| Idle (sweep, rays rotation) | companion Sweep; rays: flipbook | Sweep on the companion; rays: S83 kernel in the live compose | Sweep; rays (plate): rate-limited plate re-render |
| Win (Rim 1 Width pulse, title flash) | flipbook + flash on the companion | rate-limited re-render + flash | rim pulse not played (FAIL with fixes) + flash |

The panel shows each row's class per mode. `TitleAnimPlanTest.ThreeModes` asserts this table.

### 6.4 The Animation Presets library (built-in)

"TL" means title live (+0 draws and no memory in every mode). Other classes follow R12.

| id (Name) | Group / role | Default | Parameters | What it writes | Class |
|---|---|---|---|---|---|
| `stamp_in` (Stamp In) | In / intro | 0.5 s | `from_scale` 2.6, `overshoot` 0.94, `shake_px` 12, `shockwave` false, `sound` | title scale `from_scale → overshoot (0.6 D) → 1`, quad_in then sine_out; opacity 0→1 by 0.25 D; `shake` keys on title position after 0.6 D (0.25 s decay); marker `onImpact` at 0.6 D; option: a `shockwave` generator layer, `radius` 10→600 px (S83's minimum is 10), layer opacity 1→0 | TL (+ shockwave per R12) |
| `sweep_in` (Sweep In) | In / intro | 0.6 s | `angle` 0, `feather` 30 % | adds a gradient mask to `main_text`, its `position` from −(ink extent) to +; title `live.sweep_position` 0→1 trailing by 0.1 s | mask: re-render; sweep: TL |
| `shatter_in` (Shatter In) | In / intro | 0.7 s | `pieces` 60, `pattern` glass, `spin` 90 | adds `shatter` to `main_text`, `progress` 1→0 cubic_out, `rotation` `spin`→0; opacity 0→1 by 0.2 D; flash 0.8→0 over the last 0.1 D; marker `onAssemble` | S83 kernel (Realtime), flipbook (Baked) |
| `glow_up` (Glow Up) | In / intro | 0.8 s | `peak` 2.5 | opacity 0→1 (0.3 D); title `live.glow_pulse` 0→`peak` (0.6 D)→0; flash 0.6→0 | TL (needs a glow output) |
| `letter_cascade` (Letter Cascade) | In / intro | auto | `each` 0.05, `rise` 60 %, `tilt` −20, `order` forward, `unit` glyph | glyph offset `[0, rise]→0` back_out, glyph opacity 0→1, glyph rotation `tilt`→0, 0.3 s each, staggered | Glyph |
| `typewriter` (Typewriter) | In / intro | auto | `cps` 20, `tick` (sound) | glyph opacity steps 0→1 at each delay (`each = 1/cps`); with `tick`, `ticks` writes a sound marker per glyph | Glyph |
| `bounce_in` (Bounce In) | In / intro | 0.8 s | `height` 150 %, `squash` 0.25, `per_glyph` false | title (or glyph, staggered) position `+height→0` bounce_out; stretch `[1+squash, 1−squash]→[1,1]` elastic_out at landing; pivot bottom | TL / Glyph |
| `flip_in` (Flip In) | In / intro | 0.5 s | `axis` y | title stretch on the axis 0→1 back_out; flash −0.3→0 (darkening read as shading); S86 3D layers: `rotation_y` −90→0 | TL |
| `zoom_punch` (Zoom Punch) | In / intro | 0.35 s | `from_scale` 0.2, `blur` false | title scale `from_scale→1.15→1`; with `blur`, `radial_blur.amount` 0.6→0 on `main_text` (S83's 0..0.95); marker `onPunch` | TL (+ blur per R12) |
| `slide_in` (Slide In) | In / intro | 0.5 s | `from` left/right/top/bottom, `distance` 120 % | title position from the side, back_out; opacity 0→1 | TL |
| `pop_in` (Pop In) | In / intro | 0.4 s | `per_glyph` false, `each` 0.03 | scale 0→1.12→1 elastic_out (glyph scale when `per_glyph`) | TL / Glyph |
| `fade_in` (Fade In) | In / intro | 0.4 s | | opacity 0→1 sine_out | TL |
| `glitch_in` (Glitch In) | In / intro | 0.5 s | `rate` 15 | adds `glitch`: `seed` reseed at `rate`, `amount` 1→0; `flicker` keys on title opacity | glitch per R12; flicker TL |
| `neon_flicker` (Neon Flicker) | In / intro | 0.9 s | `seed` 1, `flickers` 5 | `flicker` generator: stepped title opacity and `live.glow_pulse` settling on | TL |
| `halftone_in` (Halftone Dissolve) | In / intro | 0.6 s | `size` 12 px | adds `halftone` (preset `dissolve`), `fade` 60→0 px | per R12 |
| `shake` (Shake) | Emphasis, overlay | 0.4 s | `amplitude` 16 px, `frequency` 25 Hz, `rotation` 2°, `decay` 1, `seed` 1 | `shake` keys on title position and rotation | TL |
| `flash` (Flash) | Emphasis, overlay | 0.15 s | `color` #ffffff, `amount` 1.5, `count` 1 | title flash 0→`amount`→0 per count, at most 2 per second | TL |
| `heartbeat` (Heartbeat) | Emphasis | 0.6 s | `scale` 1.12 | scale 1→`scale`→1→1.06→1 | TL |
| `jelly` (Jelly) | Emphasis | 0.6 s | `amount` 0.2 | stretch `[1+a,1−a]→[1−a/2,1+a/2]→[1,1]` elastic | TL |
| `idle_shimmer` (Idle Shimmer) | Loop / idle | 3 s, loop | `sweep_time` 0.8 s, `angle` 30, `glints` true | `live.sweep_position` 0→1 over `sweep_time` then held; `live.sweep_angle` = `angle`; with `glints`, `live.glint_amount` drive 0.6..1 `cycles` 1 | TL |
| `win_pulse` (Win Pulse) | Loop / win | 2 s, loop | `bpm` 120, `scale` 1.06, `glow` 0.8, `rays` true | per beat: scale 1→`scale`→1 sine_in_out, `live.glow_pulse` 0→`glow`→0, flash 0→0.25→0; `beats` markers `onBeat`; with `rays` and a sunburst layer, its `rotation` drive `cycles` 0.25 | TL (+ rays per R12) |
| `float_loop` (Float) | Loop / idle | 2.4 s, loop | `height` 4 % | title position y drive sine ±`height`, `cycles` 1 | TL |
| `breathe` (Breathe) | Loop / idle | 3 s, loop | `scale` 1.03 | title scale drive 1↔`scale`, `cycles` 1, sine_in_out | TL |
| `wave_loop` (Wave) | Loop / idle | 1.6 s, loop | `height` 8 %, `each` 0.08 | glyph offset y drive ±`height`, `cycles` 1, staggered (the TextEffects Wave) | Glyph |
| `color_cycle` (Color Cycle) | Loop / idle | 4 s, loop | | `gradient_overlay.offset` drive −1→1 `cycles` 1 on `main_text` | per R12 |
| `zoom_out` (Zoom Out) | Out / outro | 0.4 s | `to_scale` 1.4 | title scale 1→`to_scale`, opacity 1→0 | TL |
| `shatter_out` (Shatter Out) | Out / outro | 0.7 s | `pieces` 60 | shatter `progress` 0→1 cubic_in, title opacity 1→0 over the last 0.3 D | per R12 |
| `fade_out` (Fade Out) | Out / outro | 0.3 s | | title opacity 1→0 | TL |

Key generators (D13):
- `shake`: noise keys at `frequency` with amplitude × `(1 − u)^decay`, SplitMix64 of `seed` and the
  document seed;
- `flicker`: a stepped on/off pattern of `flickers` drops, settling on;
- `beats`: a marker every `60/bpm` s;
- `ticks`: a marker per stagger unit at its delay.

### 6.5 Lua API (S81's `TitleArt`; S85 defines the play semantics and adds the members marked +)

```lua
local t = TitleArt(self.scene, self.entity)              -- S81; or GameObject:getTitleArt()
t:play("Intro")                                          -- S81 signature; bool, false + one warning when unknown
t:play("Win", {speed = 1, loop = nil, unscaled = false, fade = 0.15, time = 0})   -- + fade, time
t:playOverlay("Shake")                                   -- + bool
t:stop()                     -- S81: every animation; the pose stays (Unity)
t:stop("Shake")              -- + one animation by name
t:reset()                    -- + back to the still; the title is idle again
t.paused = true              -- + rw
t:isAnimationPlaying("Idle") -- + one animation; t.isPlaying (S81, ro) is "any"
t:prepare("Win")             -- + Realtime prerender / glyph cells; true when ready now
t:isAnimationReady("Win")    -- +
t:sample("Outro", 0.25)      -- + show that time without playing (pauses)
t:getDuration("Idle")        -- +; t.animations (S81, ro) lists the names
t.animation                  -- + ro: the base animation, "" none
t.animationTime              -- + rw: the base's time; a write seeks (no markers)
t.animationSpeed             -- + rw, multiplies every animation
t.onAnimationEvent           -- + FunctionSubscribe (AnimationEvent): ev.functionName, ev.state, ev.time, ev:getNumberArg(0)...
t.onAnimationFinished        -- S81 FunctionSubscribe (name): a once animation reached its end (before `next` starts)
-- and a script method named like a marker runs on the GameObject's scripts: function Logo:onImpact() end
```

`loop` overrides the animation's wrap for that play: `true` is `loop`, `false` is `once`, `nil`
keeps the document's. Component fields (S08 reflected, `OmitIfDefault`; prefab overrides and
`set_properties` work):

| Field | Default | Inspector | Meaning |
|---|---|---|---|
| `playAutomatically` | true | Play Automatically | play `defaultAnimation`, else `animation.default`, when enabled |
| `defaultAnimation` | `""` | Default Animation | a name |
| `animationSpeed` | 1 | Animation Speed | 0..10 |
| `animationUnscaledTime` | false | Animation Update (Scaled / Unscaled) | |
| `driveAnimation` | `""` | Drive Animation | R27 |
| `animationTime` | 0 | (runtime) | R27, keyable |

### 6.6 C++ API sketch

```cpp
namespace jasperengine::titleanim {                       // engine/core/titleanim (always built)
    enum class AnimWrap : uint8_t { Once, Loop, PingPong };
    enum class AnimRole : uint8_t { Custom, Intro, Idle, Emphasis, Win, Outro };
    enum class TrackClass : uint8_t { None, TitleLive, LayerLive, Glyph, GlyphAtlasLive, Rerender };
    enum class StaggerOrder : uint8_t { Forward, Reverse, CenterOut, EdgesIn, Random };
    enum class StaggerUnit : uint8_t { Glyph, Word, Line, Child };
    enum class TrackPivot : uint8_t { Origin, Center, Bottom, Top, Left, Right, Baseline };
    enum class RerenderPolicy : uint8_t { Live, Prerender };

    struct TitleStagger { float each = 0.05f, over = -1.0f; StaggerOrder order = StaggerOrder::Forward;
                          StaggerUnit unit = StaggerUnit::Glyph; uint32_t seed = 1; EaseType ease = EaseType::LINEAR; };
    struct TitleDrive   { std::vector<float> from, to; float cycles = 0.0f, rate = 0.0f; bool wrap = false; EaseType ease = EaseType::LINEAR; };
    struct TitleReseed  { float rate = 12.0f, offset = 0.0f; };
    struct TitleTrack   { std::string layer, field, fromPreset; bool relative = false, mute = false; TrackPivot pivot = TrackPivot::Origin;
                          std::vector<ClipKey> keys; std::optional<TitleStagger> stagger; std::optional<TitleDrive> drive;
                          std::optional<TitleReseed> reseed; };
    struct TitleMarker  { float time = 0; std::string name; std::vector<AnimationEventArg> args; AssetRef sound; float volumeDb = 0; std::string group; };
    struct FlipbookSettings { float fps = 30, scale = 1; int maxPage = 2048; bool frameBlend = false; };
    struct TitleAnimation { std::string id, name, next; AnimRole role = AnimRole::Custom; float duration = -1, loopStart = 0, speed = 1;
                            AnimWrap wrap = AnimWrap::Once; bool overlay = false; RerenderPolicy rerender = RerenderPolicy::Live;
                            float rerenderFps = 30; FlipbookSettings flipbook; std::vector<TitleTrack> tracks; std::vector<TitleMarker> markers; };

    double wrapAnimationTime(double time, const CompiledTitleAnimation&);
    void sampleAnimation(const CompiledTitleAnimation&, double time, TitleAnimPose& out);   // allocation-free once sized
    void collectCrossedMarkers(const CompiledTitleAnimation&, double from, double to, bool includeStart, std::vector<CrossedMarker>& out);
    bool readAnimData(std::string_view json, TitleAnimData& out, std::vector<std::string>* warnings);    // <file>.anim.json
    class TitleAnimPlayer;      // play / overlay / stop / reset / seek / drive; writes CompanionAnim and flipbook frames
}
namespace jasperengine::titleart {                        // engine/core/titleart/anim (module)
    CompiledTitleAnimation compileAnimation(const TitleArtDoc&, const titleanim::TitleAnimation&, const TitleTextLayout&, std::vector<TitleProblem>*);
    TitleAnimPlan buildAnimPlan(const TitleArtDoc&, std::span<const CompiledTitleAnimation>, TitleArtRenderMode);
    // Realtime / Glyph Atlas: layer splits and glyph cells for S81's live compose; re-render requests at sample times
    void submitAnimItems(const TitleAnimPlan&, const titleanim::TitleAnimPose&, TitleArtInstance&);
}
```

S80's renderer provides these (S80 R18a, §6.3): a render at a pose (`RenderRequest::overrides`
with the sampled values, applied to a copy of the state, with no document edit), a kept layer
split (`keepSplits`), and glyph cells of any text layer (`glyphCell`).

### 6.7 MCP tools and the budget (S31)

S85 adds no tool. S85's row of S80 §6.11's program budget table (≤ 160 characters):

| Tool | Change | Characters (target) |
|---|---|---|
| `edit_title` | ops §6.7.1 (free-form op objects, validated by `TitleAnimOps`; fields in the guide doc) | 0 (S80's description names the animation op family) |
| `get_title` | `animation` (string) | ≤ 50 |
| `render_title` | `options.animation` `{name, time, times, frames, as}`, a key of S80's free-form `options`, keys in `jasper://docs/title-art` | 0 |
| `check_title` | `animation` (string); the check ids go through the existing `checks` list | ≤ 50 |
| `export_title` | `animations` (array of strings) | ≤ 60 |
| `list_title_library` | `kind` value `animation_presets` (in S80's program enum) | 0 |
| **Total** | | **≤ 160; 0 tools; no `budget.json` entry** |

`render_title`'s feature arguments all go into S80's `options` object (S80 §6.11), the precedent
of S21 deviation 10. Annotations are unchanged.

#### 6.7.1 `edit_title` animation ops

| Op | Members |
|---|---|
| `add_animation` | `name`, `role?`, `duration?`, `wrap?`, `loop_start?`, `next?`, `overlay?`, `preset?`, `params?`, `layers?` |
| `set_animation` | `animation` (name or `id:`), `values: {name, role, duration, wrap, loop_start, next, overlay, speed, rerender, rerender_fps, "flipbook.fps", "flipbook.scale", "flipbook.max_page", "flipbook.frame_blend"}` |
| `remove_animation`, `duplicate_animation` | `animation`, `name?` |
| `set_default_animation` | `animation` (`""` none) |
| `set_track` | `animation`, `layer?` (omitted = title), `field`, one of `keys` / `drive` / `reseed`, `relative?`, `stagger?`, `pivot?`, `mute?`, `mode?` (`replace` default; `merge` merges keys by time within 1e-4 s) |
| `set_key` | `animation`, `layer?`, `field`, `time`, `value?` (default: the sampled value), `ease?`, `in?`, `out?`, `tangent_mode?` |
| `remove_track` | `animation`, `layer?`, `field` |
| `remove_keys` | `animation`, `layer?`, `field?`, `times` or `range: [a, b]` |
| `retime` | `animation`, `layer?`, `field?`, `range?`, `shift?`, `scale?`, `pivot_time?`: moves or stretches keys and markers |
| `apply_animation_preset` | `animation`, `preset`, `params?`, `layers?`, `at?`, `mode?` (`merge` default, `replace`) |
| `set_marker` | `animation`, `name`, `time`, `args?`, `sound?`, `volume_db?`, `group?`, `index?` |
| `remove_marker` | `animation`, `name` or `index` |

#### 6.7.2 Animation checks (`check_title`; `checks: ["anim"]` runs them all)

| id | Kind | Measures | Default threshold | Status and fix |
|---|---|---|---|---|
| `anim_loop_seamless` | document + pixels | loops: values at `D − 1e-4` against `loop_start`, slope jumps, and the renders at both times | values within 1e-4 of the range; pixels max 2/255, ≤ 0.05 % changed; a slope jump > 50 % warns | FAIL; fix: `cycles` instead of `rate`, a closing key equal to the loop-start value |
| `anim_duration` | document | duration (loop span for loops) for the role | intro 0.2-3 s, outro 0.15-2, emphasis 0.1-1.5, idle span 0.5-12, win 0.3-6, custom 0.05-60 | WARN; FAIL at 0 or past 60 |
| `anim_end_state` | pixels | a `once` intro, emphasis or win ends on the still; an outro ends hidden | ≤ 0.5 % of ink changed, max 8/255; outro coverage ≤ 0.1 % | WARN; fix: a key at `D` with the rest value, or opacity 0 at `D` |
| `anim_bounds` | pixels | ink outside the canvas or the safe area at 60 samples per second | none | WARN with times and sides |
| `anim_static` | pixels | renders at 12 times all within 0.01 % of each other | any change | FAIL: the animation does nothing |
| `anim_flash` | pixels | general and red flashes at 60 samples/s (WCAG 2.3.1: > 3 flashes in any 1 s, each a pair of opposite changes of ≥ 10 % relative luminance with the darker below 0.8, over ≥ 25 % of the title's ink or 21,824 px, whichever is smaller) | ≤ 3 per second | FAIL; fix: fewer repeats or a longer period. The result says it is not a certification |
| `anim_frame_budget` | estimate | Realtime: compose against S81's live budget; re-render fps reached under R18's share per tier. Glyph Atlas: cells and quads | re-render fps ≥ `rerender_fps` (WARN), ≥ 10 (FAIL) on every target tier | fix: `prerender`, a lower `rerender_fps`, or a live field instead |
| `anim_flipbook_memory` | estimate | Baked: flipbook bytes per family (R38) | the budgets | FAIL; fix: `flipbook.fps` and `flipbook.scale` computed to fit, or Render Mode Realtime / Glyph Atlas named with its cost |
| `anim_mode_support` | document | tracks not played in the component's (or every) mode; fallbacks with their reason; Baked overlays with frames; Glow Pulse over frames; merged glyph colour | none | FAIL for unplayed tracks, WARN for fallbacks |
| `anim_markers` | document | Lua identifiers, times within the duration, sounds that resolve to audio assets, duplicates at one time | valid | WARN |
| `anim_glyph_merge` | pixels | `cells` glyph layers: the cell compose at rest against the merged still | ≤ 1 % of ink changed | WARN; fix: `glyph_mode: merged` (costs re-render), or more tracking |
| `anim_orphans` | document | tracks whose layer or field is gone | none | WARN; fix: `remove_track` |

### 6.8 File formats

| File | Format id, version | Codec |
|---|---|---|
| `*.titleart` `animation` member | inside `jasperengine.titleart` v1 (no bump, R58) | `TitleAnimIO`, called by S80's `TitleArtIO` (module and editor) |
| `<file>.anim.json` | `jasperengine.titleanim` v1 | `TitleAnimDataIO` (always built; players read it) |
| `<file>.<anim id>.flip<N>.png` | PNG + S80 recipe chunk with `animation_hash` | S80's bake writer; `.meta` via `AssetFileWrite` |
| Animation Presets `*.json` | `jasperengine.titleanimpreset` v1 | `TitleAnimPresets` (editor) |

```json
{"format": "jasperengine.titleanim", "version": 1, "source_hash": "…", "bake_version": 3, "default": "Intro",
 "animations": [{"id": "7c1e0a42", "name": "Intro", "duration": 0.8, "wrap": "once", "loop_start": 0, "next": "Idle",
   "overlay": false, "speed": 1,
   "title_tracks": [{"field": "transform.scale", "relative": true, "keys": [[0, 2.6, "quad_in"], [0.48, 0.94, "sine_out"], [0.8, 1]]}],
   "markers": [{"time": 0.48, "name": "onImpact", "args": [], "sound": {"path": "sounds/stamp.wav", "guid": "…"}, "volume_db": 0, "group": ""}],
   "flipbook": {"fps": 30, "frame": [-760, 360, 1520, 720], "blend": false,
     "pages": [{"file": "logo.titleart.7c1e0a42.flip0.png", "guid": "…", "size": [2048, 2048]}],
     "frames": [[0, 0, 0], [0, 1524, 0], [0, 1524, 0], [1, 0, 0]]},
   "baked_cost": {"frames": 25, "unique": 18, "pages": 2, "bytes": {"desktop": 8388608, "mobile": 3732480, "android_etc2": 8388608, "web": 8388608}}}]}
```

`frames` entries are `[page, x, y]` (the frame size is constant). Animations without a flipbook
have `"flipbook": null`.

### 6.9 Settings (S25)

| Key | Scope | Default |
|---|---|---|
| `title_art.animation.flipbook_budget_mib` | Project Settings > Title Art > Animation | `{desktop: 32, mobile: 8, android_etc2: 8, web: 8}` |
| `title_art.animation.default_fps` | Project | 30 (new documents' `animation.fps`) |
| `preferences.title_designer.onion_skin` | Preferences > Title Designer | `{enabled: false, before: 2, after: 2, opacity: 0.25}` |
| `preferences.title_designer.preview_as` | Preferences | `exact` |
| `preferences.title_designer.preview_sounds` | Preferences | true |

The frame budget and the live budget are S81's settings; S85 adds none.

### 6.10 Editor commands (S25 CommandCatalog)

`ShortcutContext::TitleAnimation` (key `title_animation`) is appended after S82's
`TitleDesigner`. It is active while the Animation panel has focus and uses the Animation window's
keys:

| Id | Name | Key |
|---|---|---|
| `titleAnimation.playPause` | Play / Pause Preview | Space |
| `titleAnimation.stop` | Stop Preview | Shift+Space |
| `titleAnimation.addKey` / `addKeyModified` | Add Key / Key Modified Fields | K / Shift+K |
| `titleAnimation.previousFrame` / `nextFrame` | Previous / Next Frame | `,` / `.` |
| `titleAnimation.previousKey` / `nextKey` | Previous / Next Key | Alt+`,` / Alt+`.` |
| `titleAnimation.delete`, `copy`, `paste`, `duplicate` | | Delete, Ctrl+C, Ctrl+V, Ctrl+D |
| `titleAnimation.frameSelected` / `frameAll` | | F / A |
| `titleAnimation.record` | Record | R |
| `titleAnimation.toggleCurves` | Dopesheet / Curves | Tab |
| `titleAnimation.addPreset` | Add Animation Preset... | none |
| `titleAnimation.saveAsPreset` | Save Animation as Preset... | none |

`check-hardcoded-shortcuts.py` passes. S82 AC36's context parse test covers the new value.

### 6.11 Player size (S34)

- **Always built** (`engine/core/titleanim/`, beside S81's core part): the sampler, the Baked
  player, `TitleAnimDataIO`, and the ANIM uniforms. It adds ≤ 20 KiB stripped arm64 and
  ≤ 12 KiB wasm gzip, measured with `tools/players/lib_size.py`. S81's 40 KiB core target becomes
  60 KiB with S85 (S81 is told, §8).
- **Module only** (`engine/core/titleart/anim/`): the document member codec, the plan, layer
  splits, glyph cells for Realtime, re-render and prerender requests.
- Baked-only projects keep their smallest player variant (S81 R59). Presets, the panel, the
  baker and the checks are editor-only.

## 7. Acceptance criteria

Unit tests live in `engine/tests/core/titleanim/` (always built) and
`engine/tests/core/titleart/anim/` (module). Editor tests are in `editor/tests/titleart/anim/` and
MCP tests in `tools/mcp/tests/test_title_animation*.py`. Fixtures in `tools/fixtures/title-art-anim/` (S81's `tools/fixtures/title-art-*` naming):
- a two-word Latin title;
- a vertical kanji title;
- an adjustment layer above an animated layer;
- a loop with a seam;
- a strobe;
- an over-budget flipbook;
- S81's `reference.titleart` and `counter.titleart`.

Goldens go in `tools/mcp/tests/golden/title/anim/` (class `llvmpipe` in `gpu_lane.json`); the
GPU-lane test is class `gpu`.

| AC | Verification |
|---|---|
| AC1 (R1-R3, R59) | `TitleAnimIOTest.RoundTrip` (§6.1's example: write → parse → write identical), `.UnknownMembersKept`, `.LenientDropsWithJsonPath`, `.LimitsClamped`, `.OrphanTrackKept`, `.ComponentConflictRefused`, `.NoAnimationByteIdentical` (an S80 fixture without `animation` re-saves unchanged), `.FuzzNoCrash` under ASan |
| AC2 (R3, D2) | `TitleAnimSamplingTest.MatchesClipSampler`: 200 random tracks (eases, tangents, steps) sample bit-identically to the same keys in an `AnimationClip` through `sampleTrack`; `.ColorsSrgbMidpoint`, `.SteppedSlack` |
| AC3 (R4, R5) | `TitleAnimSamplingTest.RelativeCombine` (add or multiply per field), `.TitleFieldsIdentityRest`; `TitleAnimPlayerTest.CompanionUniforms` (title scale, rotation, pivot, opacity and flash give the expected `animAffine*` and `animColor`; nothing playing turns ANIM off) |
| AC4 (R6, R7, D3) | `TitleAnimSamplingTest.DriveCyclesSeamless` (span end equals start ± 1e-6), `.DriveRateWraps`, `.ReseedSteps` (the seed changes exactly at `k/rate`), `.WrapLoopSection` (once, loop, ping_pong with `loop_start`) |
| AC5 (R8, R9) | `TitleAnimStaggerTest.Orders` (forward, reverse, center_out, edges_in on 7 and 8 units; random is a permutation stable per seed), `.OverWithEase`, `.WhitespaceSkipped`, `.WordAndLineUnits`, `.VerticalColumns`, `.RtlLogicalOrder` (written red first: visual-index ordering fails it) |
| AC6 (R10, R28, D4) | `TitleAnimMarkersTest.CrossingCases` (the `EventCrossing.*` cases applied to markers give the events' results), `.LoopSectionOncePerPass`, `.PreLoopMarkerFirstPassOnly`, `.SeekFiresNone`. Red control: with the pass split removed (test flag), a marker at `loop_start` fires twice per pass |
| AC7 (R11, R12, §6.2, §6.3) | `TitleAnimPlanTest.ClassTable` (every S80 member's class is §6.2's; a member without the attribute is re-render), `.LiveSuffixFallback` (an L param followed by a non-`scale_split` effect falls back with D10's reason), `.ThreeModes` (§6.3's table) |
| AC8 (R14) | GPU suite `TitleAnimGpu.LayerSplitEqualsRender` (core module suite, `GRAPHIC_BACKEND=vulkan` on lavapipe and glcore): for 5 fixtures × 5 times, the live compose with layer splits equals a full render at the sampled pose. Identity transforms: max 2/255, ≤ 0.05 %. Transforms: PSNR ≥ 35 dB. Both pixel paths (forced RGBA8). **Red control:** with the adjustment rule disabled (`TitleAnimPlan::TestFlags::ignoreAdjustments`), the adjustment fixture fails |
| AC9 (R15) | `TitleAnimGpu.GlyphCellsCascade`: a Realtime cascade's compose at rest equals S81's Glyph Atlas compose of the same text within S81's approximate-class tolerance; the crossfades in and out last 0.1 s; `merged` with a glyph opacity track is refused |
| AC10 (R16, R60) | `TitleFlipbookTest.FrameTimes` (once includes `D`, loop excludes it), `.ConstantFrameSize`, `.DedupeIdenticalFrames` (a held section stores one rect), `.FourPixelGutters`, `.PackDeterministic` (two bakes give byte-identical `.anim.json` and page pixels), `.EstimateEqualsWritten` (R20's bytes equal the pages' cooked sizes per family); `TitleAnimDataIOTest.RoundTrip`, `.NewerRefused`, `.StaleReported` |
| AC11 (R13, R16, R31) | `test_title_animation_export.py` (`JASPER_SLOW_BUILD=1`): a Baked animated title in a prebuilt Linux player, headless with `--fixed-fps 30`. A script calls `TitleArt:sample` at 0, 0.25 and 0.8 s and captures the Game view; the captures equal the editor's `render_title {options: {animation: {as: "baked"}}}` at those times (max 2/255, ≤ 0.1 %). The `.titleart` is not in the content manifest, and the player variant stays `lite2d` (S81 R59). `get_render_stats` while it plays shows +0 draws and passes against an Image twin |
| AC12 (R18, R37, D9) | `test_title_animation_perf.py` (GPU lane, class `gpu`): Realtime live rim pulse of `reference.titleart` at `rerender_fps` 30, no frame over S81's budget + 0.5 ms (`get_frame_times`, `harness.over_budget`); re-render sample times are multiples of 1/30, and title-live tracks update every frame while re-renders drop. Server A runs the static-model half (S81 AC17's harness) |
| AC13 (R19) | `test_title_animation.py::test_glyph_atlas_rules`: a `text` track counts 1-2-3 live in Glyph Atlas, a plate track re-renders the plate, a glyph-layer rim track is not played, and `check_title` fails `anim_mode_support` with a fix that passes once applied |
| AC14 (R21-R23, R61) | `editor/tests/titleart/anim/TitleAnimPresetsTest.cpp`: every built-in preset parses, evaluates and applies to three fixtures; the result passes `anim_static`, `anim_end_state` (intro presets) and `anim_loop_seamless` (loops); §6.4's class column matches the plan for each; Edit Preset Parameters regenerates only its tracks; Save as Preset round trip. `test_title_animation_presets.py`: `list_title_library {kind: "animation_presets"}` rows, the `id` tier, a 5-frame sheet of `stamp_in`, and no denied term in ids or names |
| AC15 (R24-R30) | `test_title_animation_lua.py` (Play, `eval_lua`, Console): `play` returns true; `onAnimationEvent` receives `onImpact` once with the animation name; the script method `onImpact` runs; `onAnimationFinished`, then `next` starts Idle in the same frame (no frame where neither plays); `playOverlay("Shake")` over Idle keeps Idle's sweep; `stop` keeps the pose and `reset` restores the still (`get_render_stats` back to idle cost); `play("Nope")` returns false and warns once over 100 calls; unscaled update runs at time scale 0; a marker sound with an Audio Source starts a voice (`getPlayingVoiceCount`; S35 capture lines when S35 is in) |
| AC16 (R27) | `test_title_animation_clips.py`: an `.anim` keying `titleart.animationTime` 0→1 with `driveAnimation: Intro` scrubs the title in Play and in the Animation window preview (`preview_animation` seek gives the `render_title` image at that time within 2/255); an Animator state plays the same clip; a `titleArtPlay` clip event starts Win |
| AC17 (R41-R48) | `test_title_animation_designer.py` (xvfb, isolated editor): create an animation; Add Preset Stamp In; scrub (the canvas screenshot changes); play and stop; Record a scale key by gizmo drag (one step "Record Key: …"; Esc during the drag reverts); R in the panel toggles Record while R on the canvas picks S82's tool; box-scale keys (one step); Curves shows the converted curve; the class icons and the cost badge show for a rim-width track; Ctrl+Z undoes each in order; screenshots in the evidence. `TitleAnimTimelineOpsTest.*`: snapping, box scale, record keying with the time-0 rest key, `toCurve`/`fromCurve` round trip |
| AC18 (R50-R55) | `test_title_animation.py`: every §6.7.1 op with undo and `dry_run`; all-or-nothing on a bad op (nothing applied, its index, did-you-mean on `fx:outer_glow.intensty`, `not_animatable` listing alternatives); `get_title` outline `animations`; `render_title {options: {animation: {name: "Intro", frames: 6}}, image: "none"}` returns 6 cells with rising times and non-zero `changed_pct`; `export_title {dry_run: true}` reports bytes per family and writes nothing; a real export writes every file or none and adds no undo step |
| AC19 (R54, §6.7.2) | `test_title_animation_checks.py`, each seeded failure beside its fixed twin (red controls): a loop with `rate` FAILs and `cycles` PASSes; a 10 Hz strobe flash FAILs and 2 Hz PASSes; a static animation FAILs and one moving key PASSes; an over-budget flipbook FAILs and PASSes after the fix ops; an intro ending off the still WARNs and a closing key PASSes; a visible outro end WARNs; a Glyph Atlas style track FAILs; an orphan WARNs. Every fix applies through `edit_title` |
| AC20 (R35, R36) | `je-bench-titleanim` (`TitleAnimBench.Sample64`, `.Scene30`) on E within budget (median of N in `docs/perf/title-animation.md`); `TitleAnimPlayerTest.ZeroAllocSteadyState` (600 frames) |
| AC21 (R32-R34) | The Android render gate gets scene `titleanim`: a Baked flipbook title and a Realtime glyph cascade, frozen by `TitleArt:sample` at 0.3 s, against a golden, run with and without `debug.jasperengine.titleart_rgba8=1`. The web gate runs the same scene (simd128 and scalar). Metal and D3D11 through the per-platform smoke exports |
| AC22 (§6.7, §6.11) | `test_tool_budget.py` (the group within limits, no new entry), `test_tool_conventions.py`, `test_tool_coverage.py`, `gen_tool_docs.py --check`, `test_docs.py` (the guide's Lua block runs under `lua run`); `lib_size.py --modules` shows the core part within §6.11 |
| AC23 (R58, R62) | `TitleAnimIOTest.NewMembersIdentity`: a document with S85's new members at defaults renders identical to one without them (golden). `test_title_animation.py::test_sound_relink`: renaming the sound relinks the marker in the same undo step |

## 8. Dependencies & conflicts

**The Title Art program graph and waves** (the same in S80-S87; S80 §8 is the source, S80 §6.15
the shared rules; an arrow means "needs"):

| Wave | Specs | Needs |
|---|---|---|
| 6 (title art) | S80 core | merged work only |
| 6B | S81 runtime, S82 Title Designer, S83 effects and generators, S86 3D | S80 (every hook of S80 §6.15); within the wave, by owner: S81 owns the component, scheduler, live compose, `title_fx`, the module wiring and the 3D and mesher sub-options; S82 owns the window, its §6.8 hooks, `DocRow` and the 3D gizmo; S83 owns the catalogue values of the registry members (glyph safety, live variants, classes, cost model), `Glint3D` and the light encoding; S86 owns 3D layers, `Gizmo3DTarget`, `quality`/`debug_view`, environments and the compressed-light encoding |
| 6C | S84 library, S85 animation, S87 localisation and interchange | S80 and wave 6B; within the wave, by owner: S84 owns `PachislopPort`, `pachislop-map.json`, `TitleLibraryApply` (and `lookFromStyle`), the denied terms; S85 owns sampling, `TitleAnimPlan`, flipbooks and `<file>.anim.json`; S87 owns the sidecar `languages` table and the per-language bake sets |

Edges: S81 → S80. S82 → S80, S81 (Preview As Glyph Atlas, create menus). S83 → S80, S81 (live
compose runs its kernels; sidecar anchors). S86 → S80, S81 (scheduler, sub-options, component
fields), S82 (sections, gizmo), S83 (`Glint3D`). S84 → S80, S82 (Library panel), S83, S86. S85 →
S80, S81, S82, S83 (S86 optional). S87 → S80, S81, S82, S83, S84, S85, S86. One exception crosses a
wave: S82's Library panel milestone (its M6) draws S84's data and lands with S84 in wave 6C; the
rest of S82 lands in 6B.

- **Depends on.**
  - **S80:** the document and `FxAnim` on every member (`TitleArtSchema`); `TitleArtOps`; `TitleRenderJob` with
    pose renders, layer splits and glyph cells (A1); the bake writer and recipe chunk; the tools.
  - **S81:** the component and its system order; the companion draw and `title_fx` with the ANIM
    feature (A2); the scheduler, live compose and live budget; Glyph Atlas cells and compose;
    Live Effects; the rows and `titleArtPlay`; the Lua class; the core-size target (A3).
  - **S82:** `addPanel`, the properties panel and gizmo batches that Record rewrites, the lint
    panel, and the context enum.
  - **S83:** the `FxAnim` values of its params, `live.kind`, D10's fallback reasons, `tfx::Stream` seeds.
  - **S84:** the denied-terms list; `list_title_library`'s `sheet`.
  - **Engine and editor:** upstream's KeyframeSampling and PropertyBindings, S14, S21 property
    tracks, S24, S25, S31.
- **Optional.** S37 (`.soundevent`), S40 M2 (sound placement), S35 (captures in AC15), S86 (3D
  members' classes; S86 R4 rigs `chars.<i>` tracks).
- **Blocks.** S87 R42 evaluates S85 animations for sheets and sequences (`sampleAnimation` + a pose
  render). S87's atlas writer and S85's flipbook packer should be one function; whichever lands
  second reuses the first's.
- **Merge hotspots.**
  - `editor/animation/ClipTangents.*` (overloads only) and `editor/util/NumericExpression.*`
    (`evaluateWith`);
  - `editor/shortcuts/CommandCatalog.h` (context) and `DefaultCommands.cpp`;
  - `engine/core/animation/PropertyBindings.cpp` (S81's `titleart` rows block);
  - S80's `TitleArtIO.cpp`, `TitleArtOps.cpp` and `TitleArtTools.cpp`; S81's `title_fx.*` and
    `TitleArtSystem.cpp`;
  - `docs/mcp/guide/title-art.md`, `docs/mcp/TOOLS.md` (regenerated), `tools/mcp/gpu_lane.json`,
    the Xcode project.

## 9. Risks & defaults

- **Whole-title flipbooks are expensive in Baked.** Defaults: budgets of 8 MiB on mobile and web,
  the FAIL check with computed fix ops naming Realtime or Glyph Atlas and their costs, and the
  export dry run. Most presets are title live and never need frames (D7).
- **Glyph cells differ from the merged look on heavy-rim titles.** Default `cells`, with the
  `anim_glyph_merge` warning and `merged` available. The panel shows both side by side when a glyph
  track is first added.
- **Mobile GPU figures are S81's static model.** No phone is in CI. S81 AC18's manual device run
  records S85's cascade and rim-pulse cases too.
- **Frame-quantized marker sounds before S40.** Accepted (D15).
- **S83 types the presets name may be missing.** The preset part is skipped with
  `preset_part_unavailable`, and the class column says so.
- **Preview As Baked needs a preview flipbook.** It is built on the background lane. Until it is
  ready the panel shows "Building Baked preview (12/30)" and plays Exact.

**What the sibling specs provide (A1-A5, all now in their owners' text)**
- **A1 (S80 R18a, §6.3):** `TitleRenderJob` renders at a pose (`RenderRequest::overrides`), keeps a
  layer split (`keepSplits`) and renders glyph cells for any text layer (`glyphCell`).
- **A2 (S81 R35):** `title_fx` and `title_fx_sprite` have the ANIM feature (3 `vec4`: affine and
  colour; `frameRect`, `frameRect2`, `frameBlend`), and the companion shows a flipbook page by
  texture rect.
- **A3 (S81 R51, S80 §6.15):** the always-built core part is ≤ 60 KiB, S85's `engine/core/titleanim/`
  ≤ 20 KiB of it.
- **A4 (S80 R9a):** `TitleArtSchema` carries `FxAnim` per member, with §6.2's assignment.
- **A5 (S80 §6.11):** `list_title_library`'s kind is `animation_presets`.

**Resolved questions (decided here)**
- Where animations live: in the `.titleart` (D1).
- Base animation layers: no; overlays instead (D10).
- Colour key interpolation: sRGB-encoded, matching S80 gradients and `.anim` (R3).
- Record on a new track at t > 0: it adds a time-0 key with the rest value (R45).
- Text keys: Glyph Atlas live, Baked frames, Realtime re-render; never from clips (D14).
- Baked per-layer parts: rejected (D6).

## Milestones

- **M1.** Data, sampling, stagger, drives, reseeds, wrap, markers (R1-R10, R58, R59); AC1-AC6,
  AC23.
- **M2.** Classes and plan (R11, R12, R20); AC7. Needs A4.
- **M3.** Baked: the companion uniforms, flipbooks and `.anim.json`; the player, Lua, clips,
  markers and sounds (R13, R16, R17, R24-R31, R60-R62); AC3, AC10, AC11, AC15, AC16. Needs A2
  and A3.
- **M4.** Realtime and Glyph Atlas: layer splits, glyph cells, re-render and prerender (R14, R15,
  R18, R19, R37); AC8, AC9, AC12, AC13. Needs A1.
- **M5.** Presets (R21-R23); AC14.
- **M6.** MCP and guide (R50-R57); AC18, AC19, AC22.
- **M7.** The Animation panel, Record, curves, preview, onion skin and Inspector rows (R41-R49);
  AC17.
- **M8.** Benchmarks and platform gates (R32-R36, R38-R40); AC20, AC21.

## Deviations

1. **Engine code outside `engine/core/titleart/`.** The contract puts engine code in
   `engine/core/titleart/` behind `JASPERENGINE_TITLE_ART`. S80 R40 compiles that directory only
   with the module, but Baked animated titles must play without it (contract decision 4, S81 R51).
   So the always-built sampler and Baked player live in `engine/core/titleanim/`, as S81 placed
   its core part outside that directory (`engine/core/util/TitleArtBakeInfo`).

   **Reconciled: accepted program-wide.** S80 §6.15 makes this the program's rule: the module is
   `engine/core/titleart/` and nothing under it is built without `JASPERENGINE_TITLE_ART`; the
   always-built part is S81's component, system, object and binding files, S81's
   `engine/core/util/TitleArtBakeInfo.*`, S87's `engine/core/util/TitleArtLanguage.*` and S85's
   `engine/core/titleanim/` (namespace `jasperengine::titleanim`), ≤ 60 KiB together.
2. **Reconciled: `FxAnim` is S80's enum** `{None, Live, Rerender, Step, Relayout}`, carried by
   `TitleArtSchema` and `ParamDef::anim`; S83 sets the param values (it was "S83's", and S83 called
   it "S85's").
3. **Reconciled: preset values in S83's units** (`radial_blur.amount` 0.6, `halftone.fade` 60 px,
   `shockwave.radius` from its minimum 10 with layer opacity).
4. **Reconciled: `render_title`'s `animation` is `options.animation`** and the budget row is ≤ 160
   (S80 §6.11).
5. **Reconciled: flipbook bakes are not undo steps** (S80 §6.9's rule for derived files); user
   presets live in S84's library folders.
6. **S37 and S40 landed before S85 (origin/main merge).** R10, R29 and R46 name the landed API:
   marker sounds start through `AudioSystem::playFreeFromTimeline` with
   `TimelineAudio::delayFrames` (the `AudioSystem::playFromTimeline` the spec first named does not
   exist), `.soundevent` refs play through `AudioSystem::playFree` (no `sound_event_unavailable`
   refusal), and the marker sounds' editor preview voice is S40's `TimelineAudioPreview`. D15's
   frame-quantized interim and the risk "Frame-quantized marker sounds before S40" no longer
   apply.

7. **Drives bounce unless `wrap`.** A drive's cycle runs from→to→from and is seamless by
   construction; `wrap: true` is the ramp (rate or cycles) that wraps. The preset `win_pulse` rays
   use cycles 1 and range -180..180 so the wrap is invisible.
8. **Scale is a uniform number; `stretch` a Vec2.** `transform.scale` is the document's scalar;
   non-uniform title motion goes through `transform.stretch` (new, written only off its default).
   Layer `flash` is `flash.color` and `flash.amount`.
9. **Vertex motion, not a UV affine (R13).** Titles move on their quad's vertices
   (`title_fx.vert`, `title_fx_sprite.vert` with `animVertex0/1`, `animSpace`), so a moved title
   is not clipped at its rectangle. The fragment affine stays identity and carries opacity and
   blend; flash is additive (was a mix).
10. **Grid packing instead of `stb_rect_pack` (R16).** Flipbook cells are equal-sized, so a
    4-px-aligned grid with 4-px gutters packs them exactly; `stb_rect_pack` would add nothing and its
    symbols are hidden in this build.
11. **Markers carry `from_preset`** (a member the player ignores) so re-applying a preset can
    replace its own markers without touching the user's.
12. **`anim_loop_seamless` passes when values differ but pixels match**: the check measures what
    the player shows; a re-render value crossing a seam by less than one 8-bit step is seamless.
13. **`render_title` refuses `compare` with `options.animation`**: a filmstrip already compares
    its own frames.
14. **Preset sheet awaits S84**: `list_title_library {kind: "animation_presets"}` lists ids, group,
    role, params and description; the contact sheet (`sheet`) is S84's library surface and is not
    wired in this stream. Generators `shake`, `flicker`, `beats`, `ticks`, and the members `repeat`
    and `vars`, were added to the preset format (§6.4 asked for expressions only).
15. **R14 layer live is re-rendered, not composed live.** S81's layer-split compose is not in the
    runtime, so Realtime layer-live tracks play through the rate-limited re-render (R18), with
    code `no_layer_compose` and a plan reason `layer-live tracks re-render: the runtime has no
    layer split compose`; `build_title_plan`-style reports say so. The split-blocker reasons are
    still reported.
16. **R15 cells at runtime need the Glyph Atlas plan.** Glyph tracks compose through the Glyph
    Atlas path (new program `GLYPH_COMPOSE_ANIM`, 43: moved cell quads with opacity, tint and
    flash, 7 quads per pass); Realtime titles switch to it while the animation plays and back at
    rest, using the plan S81 computes for the document. A document whose plan has a `fallback`, or
    a second text layer with glyph tracks, keeps its merged render for those tracks.
17. **R39's time slicing does not apply to the canvas pose.** The Animation panel renders poses
    synchronously at a capped side (640 px while playing, 960 px scrubbing), and caches 48 of them
    per document change, instead of S82's slice-budgeted job. A pose is a full title render at
    that scale; slower documents will drop preview frames, not freeze the window for long.
18. **Prerender (R18's policy) renders on demand.** `rerender: prerender` keeps the plan and cost
    figures and stores nothing: `prepareAnimation` only loads flipbook pages. The `prerender` how
    still plays as rate-limited re-render.
19. **The Designer preferences of §6.9 are session-only.** `onion_skin`, `preview_as` and
    `preview_sounds` live in the panel for the session; the project-level
    `title_art.animation.{flipbook_budget_mib, default_fps}` are the S25 settings.
20. **R49's edit-mode Preview button and the marker-sound Audio Source warning are not in the
    Inspector.** The rows are Play Automatically, Default Animation, Animation Speed, Unscaled Time
    (the spec's Animation Update) and Drive Animation.
21. **R43 tangent edits replace the keys (not `ClipTangents`).** The Curves tab edits Hermite keys
    through `CurveEditor` and writes the whole track on release as Free tangents.
23. **Test scope differs from §7.** Documents are built inside the tests instead of
    `tools/fixtures/title-art-anim/`; no image goldens at fixed times (filmstrips are checked as
    numbers, the panel and poses by screenshots a person looked at); `TitleAnimBench.Sample64` uses
    title-level tracks and drives (no staggered glyph tracks or reseeds), `.Scene30` and AC8's
    `LayerSplitEqualsRender` do not exist (Deviation 15), and AC16's clip tracks have no test of
    their own.
24. **S81/S86 fix:** the Glyph Atlas job asks for all its draw states with
    `TitleFirstDraws::allowGroup` before drawing. S86's one-new-draw-a-frame rule reports a skipped
    pass through `lastRunDeferred()`, which this job never read, so a process's first Glyph Atlas
    render could be blank and a page copy could cache a blank cell.
22. **S80 additions this spec needed:** `SchemaMember::omitDefault`, `TitleArtSchema::setFinish`,
    `estimatedTotalUs`, `TitleArtRenderHost::runToEnd`/`readImage`, the checks' `renderAnimation`,
    `renderMode` and `assetExists` context members, and `CanvasPoseSource` in S82's hooks (the
    canvas shows the panel's pose while it scrubs or plays).
