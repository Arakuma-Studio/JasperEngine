# UI scaling and text: Canvas Scaler, Game view resolutions, text effects, text outline/shadow, particles in UI

Feature `ui-scaling-text` (ledger #21), branch `gaps/ui-scaling-text`. Status: specced 2026-10-31.
Binding conventions: [AGENT-BRIEF.md](../../engine-gaps/AGENT-BRIEF.md). Gap: [GAP-REPORT.md](../../engine-gaps/GAP-REPORT.md) §8.

## 1. Goals

1. **Canvas Scaler** for UI, Unity's model: Constant Pixel Size, Scale With Screen Size (reference resolution;
   Match Width Or Height with a 0..1 log-space blend, Expand, Shrink), Constant Physical Size; applied to the native
   UI (UI scenes) and to screen-space HTML UI documents, separately from the game world's `Engine.scalingMode`.
2. Game view **resolution presets** (fixed pixel sizes and DPI) to preview scaling; agents set them too.
3. **Per-character text effects** in both UI systems: wave, bounce, shake, pulse, typewriter, fade-in, grow,
   unfold, rainbow, gradient; configurable in the Inspector (native `Text Effect` component), in CSS
   (`text-effect`) and from Lua; faithful import of the game's TextMesh Pro Effect "Waves" settings.
4. **Outline and shadow (underlay)** on the native `Text` (HTML UI already has `-webkit-text-stroke`,
   `text-shadow`, `font-effect`).
5. **Particles inside UI** in canvas order, clipped, scaled with the canvas: native UI (a Particle System under a
   UI element) and HTML UI (a `<particle-system>` element).
6. Text stays crisp when the canvas scales it up.

## 2. Non-goals

- World-space canvases and Screen Space - Camera (the native UI is a UI scene drawn over the view).
- TMP rich-text tags (`<wave>`, `<color>`) in the native Text; ranges are by character index / words instead
  (HTML gets ranges from `<span>`s).
- Text auto-size (TMP Auto Size) and SDF for plain text (plain native Text keeps its bitmap atlas).
- Masking particles by arbitrary image masks (native UI has rectangular scissors only; HTML has clip-path/overflow).
- A Unity ParticleSystem importer (feature `particles`).
- Notch/safe-area insets (feature `safe-area`, boundary in §15).

## 3. Users and stories

- Porting: every canvas of the game is "Scale With Screen Size 1920×1080, match 1" and `CanvasScaleAdjuster`
  switches match between 0 and 1 by aspect; in Unity terms that script is exactly **Expand** (canvas never smaller
  than the reference: 4:3 matches width, 20:9 matches height), so the port sets Screen Match Mode = Expand and
  deletes the script.
- A designer picks "iPhone 15 (2556×1179, 460 dpi)" in the Game view and sees the HUD laid out as on device.
- A menu title waves (Menu scene): add Text Effect, layer Wave (amplitude 0.12 em, period 1.5 s) + Gradient; or in
  HTML `h1 { text-effect: wave(0.12em 1.5s) }`.
- A dialog box types its text with a sound per character (`onCharacterRevealed`).
- Score text gets a 3 px black outline and a soft drop shadow in the Inspector.
- Game Over confetti plays inside the menu panel, above the background, below the buttons, clipped by the panel,
  while `timeScale` is 0.
- Agent: `set_scene_settings {canvas_scaler: {mode: "scale_with_screen_size", reference: [1920,1080],
  match_mode: "expand"}}`, `configure_game_view {resolution: "2400x1080"}`, screenshot; `add_component TextEffect`.

## 4. Research summary

| Source | Take | Leave (why) |
|---|---|---|
| Unity CanvasScaler <https://docs.unity3d.com/Packages/com.unity.ugui@1.0/manual/script-CanvasScaler.html>, source <https://github.com/Unity-Technologies/uGUI/blob/2019.1/UnityEngine.UI/UI/Core/Layout/CanvasScaler.cs> | Modes, names, the formulas (§7.1): log2 blend for Match, min/max for Expand/Shrink, physical units and fallback DPI | Reference Pixels Per Unit / sprite DPI (no world-unit sprites in the UI) |
| Godot multiple resolutions <https://docs.godotengine.org/en/stable/tutorials/rendering/multiple_resolutions.html> | Content scale per viewport separate from the 3D/2D world; `expand` aspect | Stretch mode "viewport" (pixel art upscaling is a world concern, already `Engine.scalingMode`) |
| Unreal UMG DPI scaling <https://dev.epicgames.com/documentation/en-us/unreal-engine/dpi-scaling-in-unreal-engine> | A single scale factor applied to all UI, computed from the viewport size (shortest side curve) | The curve editor (Unity's three modes cover our users) |
| TextMesh Pro <https://docs.unity3d.com/Packages/com.unity.textmeshpro@3.0/manual/index.html> | SDF outline/underlay parameters (Outline width/colour, Underlay offset/softness/dilate/colour), `maxVisibleCharacters`, vertex animation per character | Material presets as assets |
| TextMesh Pro Effect (game: `Assets/TextMesh Pro Effect/TextMeshProEffect.cs`) | Effect types Waves/Grow/Unfold/Bounce/Sketch, `DurationInSeconds`, `Amplitude`, `CharacterDurationRatio`, `Gradient` + `Mix` (multiply/add), `Repeat`, `AutoPlay`, `ForWords`, unscaled clock (`Time.realtimeSinceStartup`); Waves: `y += cos(-2π·t/T + 0.3·i) · 0.3 · pointSize · Amplitude` | Its obfuscated per-material mesh copying |
| Febucci Text Animator <https://www.febucci.com/text-animator-unity/docs/> | The effect vocabulary (wave, bounce, shake, rainbow, fade, typewriter with per-letter events) | Rich-text tag parsing |
| Coffee UIParticle <https://github.com/mob-sakai/ParticleEffectForUGUI> (game `Scripts/UI/UIParticle.cs`) | Particles drawn in canvas order, maskable, scale (default 100) and "ignore canvas scaler" | Mesh baking through CanvasRenderer (our UI scene draws particles directly) |

## 5. Engine today

- `Engine.scalingMode` FITWIDTH/FITHEIGHT/LETTERBOX/CROP/STRETCH/NATIVE (`engine/core/Engine.h:175-182`) defines the
  **game canvas** (`computeViewLayout`, `Engine.cpp:1090-1160`) used by 2D cameras, UI scenes, HTML screen
  documents and input coordinates (`transformCoordPos`). FITWIDTH = Unity match 0, FITHEIGHT = match 1, NATIVE ≈
  Constant Pixel Size 1, CROP/LETTERBOX fixed canvases; no blend, no Expand/Shrink, no DPI.
- Both UI systems already accept an override of their layout size: `UISystem::setAnchorReferenceSize`
  (`UISystem.cpp:1612-1628`, used by `getAnchorReferenceRect`, `:1557`) and `UIDocumentSystem::setReferenceSize`
  (`UIDocumentSystem.cpp:290-300`, `getLayoutSize` `:118`, pointer mapping `toContext` `:261`). UI cameras resize
  with `RenderSystem::updateCameraSizeForCanvas(entity, w, h)` (`RenderSystem.cpp:11557`). HTML text rasterises at
  the physical pixel ratio (`HtmlUIContextRender.cpp:106-116`).
- Game view: aspect presets only, no fixed resolutions (`editor/window/GameWindow.cpp:22-39`).
- Native `Text`: stb_truetype bitmap glyph atlas (`engine/core/util/STBText.cpp:178-290`), HarfBuzz shaping, quads
  with POSITION + TEXCOORD1 (`UISystem.cpp:335-409`), colour as a uniform (`shaderlib/ui.frag`); the UI shader
  already supports a vertex colour (`HAS_VERTEX_COLOR_VEC4`). No outline/shadow, no per-glyph animation.
- HTML: `text-shadow`, `-webkit-text-stroke`, `font-effect` (`html-ui.md:530-539`); the engine implements RmlUi's
  font engine (`engine/core/htmlui/RmlFontEngine.h:71`, `GenerateString` with a `TextShapingContext`); inline
  formatting contexts already link their texts (fork P6-L2, `ElementText::LinkInlineFormattingContext`).
- 2D/UI cameras draw in Hierarchy order without depth test (`lua-api.md:697`); a Points/particle entity under a
  RectTransform is drawn in that order and inherits the parent's scissor (`RenderSystem.cpp:15490-15600`).
  Particles are simulated in `ActionSystem` on scaled time.

## 6. Product decisions

- **S1. Two concerns, two settings.** `Engine.scalingMode` stays the game-world canvas (2D cameras, input
  coordinates, the world). The new **Canvas Scaler** sets the UI's own coordinate space. UI scenes (scenes whose
  camera is `CAMERA_UI`) and screen-space HTML documents of any scene use it. Rationale: Unity separates camera
  framing from CanvasScaler; the game needs "world FITHEIGHT, UI Expand".
- **S2. Default = Follow Game Canvas** (UI space = game canvas, today's behaviour) for old and new projects, so
  every existing scene, the agent guide's pixel coordinates and the create_ui rules are unchanged. Docs and the
  mobile project checklist recommend Scale With Screen Size.
- **S3. Where it is set.** Project Settings > Graphics "UI Scale" is the project default; each scene can override
  it (Scene Settings > Canvas Scaler: "Use project default" checkbox). Per scene (not per document) because a
  scene's documents share one RmlUi context (`UIDocumentSystem` owns one context). Texture-mode (world) documents
  keep their own size.
- **S4. Screen size = the game view rect** in physical pixels (`Engine::getViewRect()`), not the whole window: the
  UI covers what the game draws (letterbox bars stay empty). Safe-area insets do not change the scale (Unity).
- **S5. Unity formulas exactly** (§7.1), fractional UI sizes allowed.
- **S6. Crisp text.** The native Text rasterises glyphs at `fontSize × scaleFactor` physical pixels and draws them
  at `fontSize` UI units (as the HTML UI does). The atlas is keyed by raster size.
- **S7. Outline/shadow use SDF glyphs, only when enabled.** A Text with outline or shadow uses a signed-distance
  atlas generated by `stbtt_GetGlyphSDF` and a `HAS_FONTATLAS_SDF` shader feature (face, outline, underlay in one
  pass). Plain texts keep the bitmap atlas, so they render pixel-identical to today.
- **S8. Text effects as a separate component** (native) and an inherited CSS property `text-effect` (HTML), with
  the same effect vocabulary and parameters. Up to 4 layers per Text (an ECS entity has one component per type;
  TMP Effect allows several components).
- **S9. Text effects run on the unscaled UI clock by default** (`useUnscaledTime = true`), like TMP Effect
  (`realtimeSinceStartup`) and the HTML UI clock (`Engine.cpp:1357-1359`): menus animate while paused. The setting
  is explicit (brief: time convention) and can be switched to scaled.
- **S10. Particles in the native UI are ordinary Particle Systems** placed under a RectTransform in a UI scene: they
  draw in Hierarchy order, inherit the ancestor scissor, and are in UI units (so they scale with the canvas). Added:
  `ParticleSystemComponent.useUnscaledTime` and a UI-aware Inspector hint. No separate "UIParticle" component (our
  UI is already a scene; Coffee's mesh baking exists only because uGUI cannot draw a ParticleSystemRenderer).
- **S11. HTML UI particles: `<particle-system src="....prefab">`**, a replaced element that simulates the prefab's
  Particle System in the engine and draws its particles as RmlUi geometry in document order. Feasible because
  particles are CPU-simulated and their render data (`PointsComponent` render points) is CPU-visible.
- **S12. `prefers-reduced-motion`**: HTML documents with `doc:setReducedMotion(true)` and native Text Effects with
  `TextEffect.reducedMotion` (global `UI.reducedMotion`, default false) skip motion effects (wave, bounce, shake,
  pulse, grow, unfold) and keep colour and reveal effects.

## 7. Design: runtime C++

### 7.1 Canvas Scaler (new engine/core/ui/CanvasScaler.h/.cpp)

```cpp
enum class UIScaleMode { FollowGameCanvas, ConstantPixelSize, ScaleWithScreenSize, ConstantPhysicalSize };
enum class ScreenMatchMode { MatchWidthOrHeight, Expand, Shrink };
enum class PhysicalUnit { Centimeters, Millimeters, Inches, Points, Picas };
struct CanvasScalerSettings {
    UIScaleMode mode = UIScaleMode::FollowGameCanvas;
    float scaleFactor = 1.0f;                         // ConstantPixelSize
    Vector2 referenceResolution = Vector2(1920, 1080);
    ScreenMatchMode screenMatchMode = ScreenMatchMode::MatchWidthOrHeight;
    float matchWidthOrHeight = 0.0f;                  // 0 width .. 1 height
    PhysicalUnit physicalUnit = PhysicalUnit::Points;
    float fallbackScreenDPI = 96.0f;
};
struct CanvasScalerResult { float uiWidth, uiHeight; float scaleFactor; };   // scaleFactor = view px per UI unit
CanvasScalerResult computeCanvasScale(const CanvasScalerSettings&, float viewW, float viewH,
                                      float gameCanvasW, float gameCanvasH, float screenDpi /* 0 unknown */);
```

Formulas (W, H = view px; Rw, Rh = reference):
- FollowGameCanvas: `ui = gameCanvas`; `scaleFactor = viewW / gameCanvasW` (x; y analogous for STRETCH).
- ConstantPixelSize: `s = max(scaleFactor, 0.01)`.
- ScaleWithScreenSize: MatchWidthOrHeight `s = 2^lerp(log2(W/Rw), log2(H/Rh), clamp(match,0,1))`; Expand
  `s = min(W/Rw, H/Rh)`; Shrink `s = max(W/Rw, H/Rh)`.
- ConstantPhysicalSize: `dpi = screenDpi > 0 ? screenDpi : fallbackScreenDPI`; `s = dpi / k`, k = 2.54 (cm),
  25.4 (mm), 1 (in), 72 (pt), 6 (pc).
- `uiWidth = W / s`, `uiHeight = H / s` (except FollowGameCanvas). Guard: W, H, s > 0, else keep the last result.

Application (`Engine::systemViewChanged`, `Engine.cpp:1224`, and when a scene's settings change or a scene is
added, `Engine::includeScene` `:397`): for each scene, effective settings = scene override or project default;
result stored on the `Scene` (`Scene::getCanvasScale()`); `UISystem::setAnchorReferenceSize(uiW, uiH)` when the
scene camera is `CAMERA_UI`; `RenderSystem::updateCameraSizeForCanvas(camera, uiW, uiH)` for that camera;
`UIDocumentSystem::setReferenceSize(round(uiW), round(uiH))`. Pointer coordinates entering `UISystem`
(`Engine.cpp:1905` ...) convert canvas → UI with `ui = canvas · (uiW / canvasW)` per axis (the HTML path already
does this in `toContext`). `RectTransform` stays in UI units. `UISystem` passes `scaleFactor` to text raster (S6).
The scene's `onCanvasChanged`-style event: `Engine.onCanvasChanged` already fires on view changes; the scaler is
computed before it.

Screen DPI: `System::getScreenDpi()` / Lua `Screen.dpi` is defined by feature `platform-services` (0 = unknown,
per-platform sources in its spec); the scaler only reads it. In the editor it returns the active Game view device
profile's DPI (safe-area), else the selected resolution preset's DPI (§9), else the host's.

### 7.2 Native Text: crisp raster, outline, shadow (TextComponent.h, STBText, UISystem, ui shaders)

New `TextComponent` fields (defaults keep today's look):

| Field | Default | Meaning |
|---|---|---|
| `outlineWidth` | 0 | UI units, drawn outside the glyph edge (TMP "Outline Thickness" is relative; we use units) |
| `outlineColor` | (0,0,0,1) sRGB | |
| `shadowEnabled` | false | TMP "Underlay" |
| `shadowColor` | (0,0,0,0.5) | |
| `shadowOffset` | (2, 2) | UI units, y down (right-down) |
| `shadowSoftness` | 0 | UI units of blur radius |
| `shadowDilate` | 0 | UI units the shadow shape grows |
| `visibleCharacters` | -1 | TMP `maxVisibleCharacters`: glyphs past it are not drawn (-1 all) |

- S6: `STBText` gains a raster scale; glyph cache key `(face, rasterSize, sdf, sdfPadding)`.
- S7: with `outlineWidth > 0 || shadowEnabled`, glyphs come from `stbtt_GetGlyphSDF(info, scale, glyph, padding,
  onedge=128, pixel_dist_scale=128/padding)` with `padding = ceil((outlineWidth + shadowSoftness + shadowDilate +
  max(|offset.x|,|offset.y|)) × scaleFactor) + 2`, packed in a separate SDF atlas page set; quads expand by the
  padding. Shader feature `HAS_FONTATLAS_SDF` (ui.vert/ui.frag) with a uniform block `u_fs_textParams { vec4
  outlineColor; vec4 shadowColor; vec4 params; /* x outline px, y softness px, z dilate px, w sdf px scale */ vec4
  shadowUV; /* xy offset in atlas uv */ }`: `d = sample(uv)`, face alpha = `smoothstep` around 0.5 with a 1-pixel
  AA width from `fwidth`; outline band from `0.5 - outline·k`; shadow sampled at `uv - shadowUV`, dilated and
  softened, composited under. One draw call per Text as today. The variant must be in the export's shader set (the
  exporter collects UI variants from scenes; add this property combination when a Text has outline/shadow, and the
  runtime shader compiler covers scripted changes).
- `visibleCharacters` and effects need vertex colour: such Texts add a `COLOR` (vec4) attribute (white) to their
  buffer, which selects `HAS_VERTEX_COLOR_VEC4` (already in ui.frag).
- `STBText::createText` additionally outputs per quad the cluster (grapheme) index, line index and the glyph's
  advance box (for effect pivots), kept in `TextComponent.glyphInfo` (runtime).

### 7.3 Text Effect (native, new engine/core/component/TextEffectComponent.h)

```cpp
enum class TextEffectType { None, Wave, Bounce, Shake, Pulse, Typewriter, FadeIn, Grow, Unfold, Rainbow, Gradient };
enum class TextEffectMix { Multiply, Add, Replace };
struct TextEffectLayer {
    TextEffectType type = TextEffectType::None; bool enabled = true;
    float amplitude = 0.15f;      // em (Wave, Bounce, Shake), scale delta (Pulse), unused otherwise
    float duration = 1.5f;        // s: period (looping) or length (one-shot)
    float spread = 0.5f;          // 0..1, TMP Effect CharacterDurationRatio (one-shot stagger)
    float phaseStep = 0.3f;       // rad per character (Wave, Rainbow hue as turns·2π)
    float frequency = 12.0f;      // Hz (Shake)
    float charactersPerSecond = 30.0f;   // Typewriter (0 = use duration for the whole text)
    float fadeTime = 0.0f;        // Typewriter per-character fade
    std::vector<GradientStop> gradient;   // Gradient (colour, alpha, position 0..1)
    TextEffectMix mix = TextEffectMix::Multiply;
    float saturation = 0.8f, lightness = 0.6f;   // Rainbow (HSL)
};
struct TextEffectComponent {
    bool enabled = true;
    std::array<TextEffectLayer, 4> layers;
    bool autoPlay = true, loop = true;   // loop: one-shot effects restart after finishing (TMP "Repeat")
    bool useUnscaledTime = true;         // S9
    int startIndex = 0, length = -1;     // character range (grapheme clusters, -1 = to the end)
    std::string forWords;                // TMP "ForWords": whole words, space separated, overrides the range
    // runtime: time, playing, finished, cached base vertices
    FunctionSubscribe<void()> onComplete; FunctionSubscribe<void(int)> onCharacterRevealed;
};
```

Per frame (`UISystem::update`, after layout, for Texts with an enabled Text Effect and playing): restore base
positions/colours, then for each visible glyph `i` (0..N-1 within the range, counting visible glyphs only) with
glyph centre `c`, font size `f` (UI units), time `t`, `p = t/duration`:
- Wave: `y -= amplitude·f·cos(2π·p - phaseStep·i)` (y down; matches TMP Effect with amplitude_em = 0.3 ×
  TMP Amplitude).
- Bounce: `y -= amplitude·f·|sin(π·(p - spreadDelay·i))|`, `spreadDelay = spread / max(N,1)`.
- Shake: offset `amplitude·f·(h(i,k)·2-1, h'(i,k)·2-1)`, `k = floor(t·frequency)`, `h` a deterministic hash in [0,1).
- Pulse: scale about `c` by `1 + amplitude·sin(2π·(p - spreadDelay·i))`.
- One-shot progress per glyph (TMP Effect formula): `start_i = lerp(i/N, 0, spread)`, `len = lerp(1/N, 1, spread)`,
  `q_i = clamp01((min(p,1) - start_i)/len)`; Grow: quad's right edge from left to full by `q_i` (TMP Grow); Unfold:
  quad from its horizontal centre line to full by `q_i`; FadeIn: alpha × `q_i`.
- Typewriter: revealed count `n = floor(t·charactersPerSecond)` (or `N·p` when cps = 0); glyph alpha = 1 when
  `i < n`, ramps over `fadeTime` otherwise 0; `onCharacterRevealed(i)` once per glyph; finished when all revealed.
- Rainbow: colour = HSL(`fract(p + phaseStep·i/(2π))`, saturation, lightness), mixed by `mix`.
- Gradient: colour = gradient(`fract(p)` for looping, `q_i` for one-shot use) per TMP Effect (multiply or add with
  alpha multiplied).
- Layers apply in order (positions accumulate, colours compose). One-shot effects finish when every glyph reached
  1 → `onComplete`; with `loop` they restart. Changing the text restarts typewriter/one-shot layers.
- Upload only the vertex buffer (positions, colours) when changed; static after one-shot completion.

### 7.4 HTML `text-effect` (fork patch rows T21-1..T21-3 in docs/html-ui/FORK.md + engine font engine)

- Property `text-effect` (inherited, initial `none`): a comma list of functions; arguments positional, omitted
  trailing ones default:
  `wave(<length>=0.15em <time>=1.5s <angle>=17deg)`, `bounce(<length>=0.3em <time>=1s <number spread 0..1>=0.5)`,
  `shake(<length>=0.05em <number Hz>=12)`, `pulse(<number>=0.15 <time>=1s <spread>=0.5)`,
  `typewriter(<number cps>=30 <time fade>=0s)`, `fade-in(<time>=0.5s <spread>=0.5)`, `grow(<time> <spread>)`,
  `unfold(<time> <spread>)`, `rainbow(<time>=2s <angle hue-step>=20deg <percentage s>=80% <percentage l>=60%)`,
  `gradient(<time> [multiply|add|replace]? <color-stop>#)`. Same formulas as 7.3 (em = the element's font size).
- `text-effect-iteration: infinite | <integer>` (default `infinite` for looping, applies to one-shots like TMP
  Repeat), `text-effect-time: ui | game` (default `ui` = unscaled UI clock, S9).
- Fork (RmlUi): `TextShapingContext` gets `const TextEffectState* effect` and `int glyph_index_base`; `ElementText`
  fills them when its computed `text-effect` is not none (glyph base from its position in the linked inline
  formatting context, P6-L2, so `Hello <b>World</b>` waves continuously); while an effect animates the text's
  geometry is marked dirty every frame (vertex regeneration only; layout untouched). `RmlFontEngine::GenerateString`
  (engine) applies the per-glyph transform/colour to every layer it emits (base and font-effect layers, so outlines
  and shadows follow the glyph). Origin time = when the element's computed value or its text changed.
- Events: `texteffectend` (bubbles) when one-shot effects finished all iterations. Lua: `el:restartTextEffect()`.
- Reduced motion (S12) skips motion functions.
- Compat layer: `text-effect*` are engine properties (PropertyTable rows, kind "engine extension"), LuaLS stubs
  for `style.textEffect`.

### 7.5 Particles in UI

Native (S10):
- `ParticleSystemComponent.useUnscaledTime` (default false; `ActionSystem` advances that system with the unscaled
  delta). The UI create menu "UI > Particle System" creates one under the selected RectTransform with
  `useUnscaledTime = true`, `localSpace = true`, sizes in UI units (e.g. 24) and a soft-circle texture.
- Rules made explicit and tested: draws between its previous and next siblings (Hierarchy order); clipped by the
  nearest ancestor scissor (Panel/`scissor`); not hit by pointer raycasts; scales with the canvas (UI units); a
  disabled/hidden ancestor hides it; plays while `timeScale` is 0 when unscaled.
- To verify first (not run while specifying) and fix if wrong: particle sizes must be in UI units under a
  `CAMERA_UI` camera (they scale with the canvas), not in framebuffer pixels; the Points path must honour the
  inherited scissor like UI elements (`RenderSystem.cpp:15490-15545` applies it to any entity under a
  RectTransform parent). The tests in §14 pin both.

HTML (S11):
- `<particle-system src="prefabs/fx/confetti.prefab" autoplay loop time="ui|game" scale="1"></particle-system>`,
  a replaced inline-block element (default size 0×0; the emitter is at its content-box centre, local units = CSS
  px × `scale`). The prefab's root must have a Particle System (+ its Points renderer: texture, sprite frames,
  blending); other components are ignored (diagnostic `HTMLUI-HTML-00xx` when missing).
- `UIDocumentSystem` instantiates the prefab's particle components on a hidden runtime entity of the document's
  scene (not listed in the Hierarchy or saved), simulated by the scene's `ActionSystem` (unscaled for `time="ui"`).
  Each frame the element's render emits one `Rml::Mesh` of the visible particles (quads from position, size,
  rotation, colour, sprite frame; additive or alpha per the Points blend mode) with the particle texture, in
  document order, inside the element's clip and transform. Element removal or document unload destroys the entity.
- Lua: `el:play()`, `el:stop()`, `el:pause()`, `el:clear()`, `el:getParticleSystem()` (the engine `ParticleSystem`
  wrapper of the hidden entity: every particle API works); event `complete` (non-looping systems).

### 7.6 Serialization, catalog, generated code

- Project: `project.yaml` `uiScale: {mode, scaleFactor, referenceResolution: [w,h], screenMatchMode,
  matchWidthOrHeight, physicalUnit, fallbackScreenDPI}` (absent = FollowGameCanvas); Generator emits
  `Engine::setDefaultCanvasScaler({...});` next to `setScalingMode` (`Generator.cpp:2303`).
- Scene: `canvasScaler: {useProjectDefault: true, ...}` in the scene node (`Stream.cpp:3756-3790` encode,
  `:3893-3994` decode), applied by `Factory`'s generated scene code (`scene->setCanvasScaler(...)`).
- Components: `TextComponent` new fields, `TextEffectComponent`, `ParticleSystemComponent.useUnscaledTime` in
  `Stream.cpp`, `Catalog.cpp`, `Factory.cpp`; defaults are not written (old files unchanged byte-for-byte when saved
  without edits).

## 8. Lua API

- `scene:getCanvasScaler()` → `CanvasScaler` (rw: `uiScaleMode` (`UIScaleMode.*`), `scaleFactor`,
  `referenceResolution` (Vector2), `screenMatchMode` (`ScreenMatchMode.MatchWidthOrHeight|Expand|Shrink`),
  `matchWidthOrHeight`, `physicalUnit` (`PhysicalUnit.*`), `fallbackScreenDPI`, `useProjectDefault`; read-only
  `canvasSize` (Vector2 UI units), `appliedScaleFactor`). Changes apply next frame.
- `UI.canvasToUI(scene, Vector2)`, `UI.uiToCanvas(scene, Vector2)` (canvas px ↔ the scene's UI units).
- DPI: `Screen.dpi` (feature platform-services).
- `Text` wrapper: rw `outlineWidth`, `outlineColor`, `shadowEnabled`, `shadowColor`, `shadowOffset`,
  `shadowSoftness`, `shadowDilate`, `visibleCharacters`.
- `TextEffect(scene, entity)`: rw `enabled`, `autoPlay`, `loop`, `useUnscaledTime`, `startIndex`, `length`,
  `forWords`; `getLayer(i)` (1-based, returns a `TextEffectLayer` proxy with the fields of 7.3 and `type` as
  `TextEffectType.*`), `setLayer(i, table)`; methods `play()`, `stop()` (back to plain text), `finish()` (jump to the
  end), `restart()`, `isFinished()`; events `onComplete`, `onCharacterRevealed(index)`. `UI.reducedMotion` (rw).
- `ParticleSystem.useUnscaledTime` (rw).
- HTML: `style.textEffect`, `el:restartTextEffect()`, `<particle-system>` methods (7.5).

## 9. Editor UX

- Project Settings > Graphics: "UI Scale" group (mode dropdown, then the relevant fields as Unity's CanvasScaler
  Inspector: Reference Resolution, Screen Match Mode, Match slider labelled Width ↔ Height, Scale Factor, Physical
  Unit, Fallback Screen DPI) with a one-line preview "At 2400×1080: UI 2400×1080 at scale 1.00".
- Scene Settings (Inspector of the scene / `get_scene_settings`): "Canvas Scaler" with "Use project default".
- Scene view of a UI scene: the canvas frame is the UI size computed for the Game view's current resolution.
- **Game view resolution menu** (`GameWindow.cpp:22-39` extended): Free Aspect; the project canvas; aspect presets
  (16:9, 16:10, 4:3, 9:16); **fixed resolutions** with an optional DPI: 1280×720, 1920×1080, 2560×1440, 2400×1080
  (20:9 phone, 400 dpi), 2732×2048 (264 dpi) and their portrait variants; "Add custom..." (name, width, height,
  dpi), stored in the editor workspace (`Workspace.cpp`). A fixed resolution renders the game at that pixel size
  into an offscreen target and fits it into the panel (the "Scale" zoom stays); the toolbar shows
  "1920×1080 (0.48x)" when shrunk. This fixed-resolution rendering is the mechanism the safe-area feature's
  **device profiles** (named phones with resolution, DPI, insets, rotation; owned by safe-area) use: choosing a
  profile selects its resolution through the same path. One menu, two sections ("Resolutions" here, "Devices"
  from safe-area).
- Inspector: Text gets "Outline" and "Shadow" foldouts and "Visible Characters"; Text Effect component with up to 4
  layer rows (type dropdown, then only that type's fields, gradient editor), Play/Restart buttons that preview in
  edit mode (the editor ticks effects in the Scene view at the UI clock, like TMP Effect's ExecuteInEditMode).
- Component add menu: UI > Text Effect; GameObject > UI > Particle System.
- Undo through the command system like every property.

## 10. MCP / agent surfaces

- `set_project_settings` key `graphics.ui_scale` (object); `get/set_scene_settings` setting `canvas_scaler`
  (object, `SceneTools.cpp:626` table, `ScenePropertyCmd` undo).
- `configure_game_view` gains `resolution` (`"1920x1080"`, a preset name, or `{width, height, dpi}`; `"free"`
  returns to aspect mode) and `custom_resolutions` (read). Screenshots then use that resolution.
- Components via `add_component TextEffect` / `set_properties` (catalog names), `create_ui` gains text props
  `outline` `{width, color}` and `shadow` `{offset, color, softness}`.
- Guide: a "UI scaling" section in `docs/mcp/guide/html-ui.md` and a new `docs/mcp/guide/ui.md` (native UI:
  scaling, text effects, outline, particles); one line in `McpInstructions.cpp`: "UI scale: Canvas Scaler (project or
  scene settings); preview with configure_game_view resolution".

## 11. Per-platform notes

DPI per platform: platform-services. SDF glyph generation is CPU (stb) at text build time, all platforms.
Web/WebGL2 and GLES3: `fwidth` is available (shaders are compiled through the engine's shader pipeline for each
backend). No platform code beyond the DPI query.

## 12. Behaviour rules and edge cases

- Rotation / resize on device: `systemViewChanged` recomputes every scene's scale before `onCanvasChanged`
  scripts run; layouts update that frame.
- Extreme aspect: Expand never produces a UI smaller than the reference in either axis; Shrink can cut (Unity).
- FollowGameCanvas with LETTERBOX: unchanged behaviour.
- Constant Physical Size without DPI: fallback DPI; the editor uses the preset DPI or 96.
- Scenes loaded later get the current result in `includeScene`.
- Text: changing `fontSize` or the scale factor rebuilds the atlas pages for that size; SDF and bitmap pages are
  separate; an atlas growth during an effect keeps the effect time.
- Text Effect range past the text end: clamped; empty text: nothing, `onComplete` not fired.
- Time scale 0: effects with `useUnscaledTime` run; scaled ones freeze. Play paused in the editor: the UI clock
  stops (`HtmlUIClock` rule), so both freeze.
- Particles: a UI particle system in a scene being unloaded is destroyed with it; HTML particle entities die with
  their element/document.

## 13. Performance (mobile)

- Scaler: a few float ops per view change.
- SDF glyph generation ~0.2 ms per glyph at 48 px; only for outlined/shadowed texts; cached per size.
- Text effects: CPU per animated glyph (≈ 40 flops) + one dynamic buffer upload per animated Text per frame; 500
  animated glyphs ≈ 0.1 ms on a mid phone. HTML: regenerating glyph geometry each frame for animated texts only.
- HTML particles: one mesh rebuild per frame per element (≤ 1000 quads recommended; documented).

## 14. Tests

Editor unit tests:
- `CanvasScalerTest`: Match 0/0.5/1 at 2400×1080 vs 1920×1080 (s = 1.25 / 1.118 / 1.0), Expand at 1440×1080
  (s = 0.75, UI 1920×1440) and 2400×1080 (s = 1.0, UI 2400×1080), Shrink, Constant Pixel Size 2, Physical Size at
  326 dpi points (s = 4.528), fallback DPI, invalid inputs keep last result, STRETCH game canvas.
- `UIScaleIntegrationTest` (scene in code): anchors fill the UI size; a pointer at the view centre hits an element
  centred in UI units; HTML context dimensions equal the UI size.
- `TextEffectTest`: glyph offsets for Wave at t = 0, T/4 (cos values), Grow/Unfold per-char progress table from
  the TMP Effect formula with spread 0.531, Typewriter reveal count and events, range/forWords selection, layers
  composition, loop restart, unscaled vs scaled time.
- `TextSdfTest`: SDF atlas used only when outline/shadow on; a plain Text's vertex buffer and texture are identical
  to before (regression).
- `GameViewResolutionTest`: preset parsing, custom presets persisted in the workspace, fixed-resolution render
  target size, `Screen.dpi` override in Play.
Golden render tests (GL, xvfb; `engine/tests` or editor render tests): Text with outline 3 px, with shadow offset
(2,2) softness 4, compared to goldens (tolerance as existing goldens); native UI particles clipped by a Panel.
HTML UI tests (`engine/tests/htmlui`): `text-effect` parsing (every function, defaults, invalid values diagnostics),
per-glyph offsets through `GenerateString` for `wave` across `<b>` (continuous indices), `texteffectend` after
`typewriter`, reduced motion; golden of `<particle-system>` with a fixed seed after 1 s; RmlUi unit test for the
property parser.
MCP e2e (`tools/mcp/tests/test_ui_scaling.py`): set Expand 1920×1080 on a UI scene with a stretch-anchored panel,
`configure_game_view resolution 1440x1080` → screenshot: panel fills, `eval_lua scene:getCanvasScaler().canvasSize`
= (1920, 1440); `2400x1080` → (2400, 1080); HTML document `getBoundingClientRect` of `body` matches; text effect
component added via `add_component`, screenshots at two frames differ in the title region only.
Export: Linux export of the scaler scene starts and logs the canvas size; Web/Android compile.

## 15. Acceptance criteria

1. The game's HUD config (1920×1080, Expand) lays out identically to Unity at 4:3, 16:9 and 20:9 (MCP screenshots
   reviewed, numeric checks above).
2. Old projects/scenes render pixel-identical (FollowGameCanvas default; plain Text unchanged).
3. Text effects match the TMP Effect formulas for the game's Menu settings (Waves, 1.5 s, amplitude 0.2/0.4,
   ratio 0.531, gradient Add) in both UI systems; outline/shadow goldens pass.
4. UI particles play in a paused game (timeScale 0) inside a clipped panel; `<particle-system>` renders in document
   order.
5. All settings round-trip (project, scene, components), generate code for exports, have Inspector UI, MCP access,
   Lua bindings in stubs, and docs (`ui.md`, html-ui.md, lua-api.md, unity-glossary.md rows for CanvasScaler,
   TMP outline/underlay, TMP Effect → Text Effect, UIParticle → Particle System under a UI element / `<particle-system>`).

## 16. Integration points

- `engine/core/Engine.h:175-182,453-459`, `Engine.cpp:397,1090-1253` (scaler application), `Scene.h/.cpp`
  (settings + result); DPI from platform-services' `System::getScreenDpi`.
- `engine/core/subsystem/UISystem.cpp:335-409` (text build), `:1557-1628` (reference size), `:2866+` (pointer
  conversion); `RenderSystem.cpp:11557` (UI camera), `:10304` (drawUI SDF uniforms), `:15490-15600` (UI order and
  scissor for Points).
- `engine/core/util/STBText.cpp:178-290` (raster scale, SDF pages, glyph info), `component/TextComponent.h`,
  new `component/TextEffectComponent.h`, `engine/shaderlib/ui.vert`, `ui.frag`.
- `engine/core/subsystem/UIDocumentSystem.cpp:118,261,290-310`, `htmlui/RmlFontEngine.cpp` (GenerateString),
  RmlUi `ElementText`, `TextShapingContext`, property spec (fork rows T21-1 text-effect property, T21-2 per-glyph
  regeneration hook, T21-3 `<particle-system>` instancer is engine-side, no fork row needed).
- `engine/core/component/ParticleSystemComponent.h:234`, `subsystem/ActionSystem.cpp` (unscaled advance).
- Editor: `window/GameWindow.cpp:22-39,304-450`, `Workspace.cpp`, `window/dialog/ProjectSettingsWindow.cpp:1293`
  (Graphics), scene settings UI, `InspectorWindow.cpp` (Text, Text Effect), `Stream.cpp`, `Catalog.cpp`,
  `Factory.cpp`, `Generator.cpp:2301-2311`, `mcp/tools/SceneTools.cpp:626`, `mcp/tools/ViewTools.cpp`
  (configure_game_view), `mcp/tools/WorldTools.cpp` (create_ui text props).

## 17. Cross-feature dependencies

- **safe-area** (boundary): safe-area owns insets (screen px), the safe-area anchoring of RectTransforms and HTML
  `env(safe-area-inset-*)`, and the notch overlay. Canvas scaling owns the UI size and scale factor. Contract:
  `Scene::getCanvasScale()` gives `{uiWidth, uiHeight, scaleFactor}`; safe-area converts insets to UI units as
  `inset_px / scaleFactor` (native) and to document px as `inset_px × documentWidth / viewWidth` (HTML). Safe-area
  owns the Game view device profiles (with insets and rotation) and drives this spec's fixed-resolution rendering
  to apply a profile's size. The scale is computed from the full view rect, never the safe rect (Unity).
- **platform-services**: owns `System::getScreenDpi()` / `Screen.dpi` (used by Constant Physical Size).
- **ui-multitouch**: uses `UI.canvasToUI` for `PointerEventData.uiPosition`; on-screen controls in UI units scale
  with the canvas.
- **particles**: owns particle modules and the Unity importer; `useUnscaledTime` is added here (one field) and
  must be kept by its serialization changes.
- **anim-properties**: clip channels can animate `outlineColor`, Text Effect fields (UI properties).

## Implementation notes

Decisions taken while implementing (branch `gaps/ui-scaling-text`, on `gaps/platform-services`), where the
spec was silent, ambiguous or did not fit the code:

1. **Where the scaler lives.** `engine/core/util/CanvasScaler.{h,cpp}` (the engine has no `ui/` folder):
   `CanvasScaler::compute` with Unity's formulas, names for files and MCP (`follow_game_canvas`,
   `scale_with_screen_size`, `expand`, `points`, ...). The scene's settings are part of `SceneSettings`
   (`canvasScalerUseProjectDefault`, `canvasScaler`); the result is on `Scene` (`getCanvasScale()`,
   `isUIScaled()` for a UI-camera scene, `isDocumentScaled()` for its screen documents). `Scene::updateCameraSize`
   (called by `includeScene`, `systemViewChanged`, `setCamera`) computes it from `Engine::getViewRect()`, the game
   canvas and `Screen::getDpi()`, then sizes the camera (UI size for a UI camera, else the canvas). `UISystem`
   reads it through `getCanvasSize()` (an explicit `setAnchorReferenceSize`, the editor's, still wins; the anchor
   reference is a float now for fractional UI sizes), converts pointer events canvas -> UI units inside
   `eventOnPointerDown/Up/Move` (one place for the Engine, Lua and ui-multitouch), and `UIDocumentSystem`
   lays documents out at the rounded UI size; a scaled document in a 2D scene still covers the game canvas
   (`getFrameSize`).
2. **Follow Game Canvas changes nothing**, crisp raster included: the native Text rasterizes at the scale only
   when the Canvas Scaler scales a UI scene (`UISystem::getTextRasterScale`); under Follow Game Canvas it keeps
   rasterizing at its font size as before (the spec's S6 formula would have changed every old text in the
   Game view, whose size differs from the canvas). Raster sizes are whole pixels (`round(fontSize x scale)`),
   so resizing a window reuses atlases; plain texts keep their exact font id (`font|20`) and code path.
3. **Scene settings over MCP** are one setting, `canvas_scaler` (an object; fields given on a scene that used
   the project default start from that default and turn `use_project_default` off); the Inspector edits it
   through one `ScenePropertyCmd<SceneCanvasScaler>` (a drag is one undo step). The edit-mode Scene view and
   the Game view's edit pass lay a UI scene out at the UI size computed for the Game view's resolution
   (`Project::applyEditCanvasScale`, `setEditViewSize` from the Game view).
4. **Lua `scene:getCanvasScaler()`** returns a `CanvasScaler` reference (`util/CanvasScalerRef.h`): reads give
   what applies (the project default while `useProjectDefault`), and setting a field of such a scene copies
   the default into the scene and overrides it, as an agent or a script expects. Changes apply at once (the
   UI lays out in them from the next frame).
5. **Game view resolutions.** Built-in presets as listed (landscape and portrait), "Add custom..." (name,
   size, dpi) in the per-user workspace file (`gameResolutions`), the choice in the editor preferences with
   the aspect. A fixed resolution renders the game at its pixels; the image is fitted to the view and the
   toolbar shows `WxH (0.48x)`; the simulated device of platform-services uses the same path (its screen
   is the fixed resolution). `Screen.dpi` is the device's, else the preset's (`Platform::setSimulatedDpi`),
   else the host's. MCP `configure_game_view resolution` takes a preset or custom name, `"WxH"`, `"WxH@dpi"`,
   `{width, height, dpi[, name]}` (a name saves it as custom) or `"free"`, and reports `resolution`,
   `resolutions`, `custom_resolutions`, `screen_dpi`. The safe-area device profiles (owned by safe-area) plug
   into the Device menu that platform-services added, as specified.
6. **Text outline/shadow.** Colours are stored linear like `UIComponent::color` (the Inspector's colour rows
   and MCP treat UI colours as linear); the `Text` Lua setters take sRGB like `Text.color`. The SDF atlas is a
   separate STBText per (font, size, raster size, padding) with its GlyphAtlas padding equal to the SDF
   padding, so a shadow sampled up to that far outside its glyph finds empty texels, never a neighbour. The
   padding is quantized to steps of 4 px (2..64), an SDF glyph is rasterized at 32 px at least (small texts
   keep their corners), and the shader variant bit is `Sdf` (UI property bit 4, `ui_FtxSdf`). Colours,
   offsets and softness reach the shader every frame without rebuilding the text; a width the padding does
   not cover rebuilds the atlas (past the 64 px cap the shader clamps width, softness and offset to the
   padding instead of sampling a neighbour glyph). The vertex colour of a Text Effect multiplies the face, its
   alpha and the Text's alpha also the outline and shadow (a typewriter or a colour fade fades all three), so
   a hollow text (face alpha 0) keeps its outline. The padding is not part of the measured text: an
   auto-sized Text keeps its size when an outline or shadow is turned on (as TMP).
7. **`visibleCharacters`** collapses the quads of later characters (characters = grapheme clusters, from
   HarfBuzz's cluster values) without a vertex colour or a shader change.
8. **Text Effect formulas vs. the spec.** The shared math is `engine/core/util/TextEffects.{h,cpp}` (native and
   HTML). Checked against the game's `TextMeshProEffect.cs`: Wave moves glyphs **down** at t = 0
   (`y_down += A em cos(2 pi t/T - phaseStep i)`; TMP subtracts from y up; the spec's `y -=` had the sign the
   other way); TMP's Grow grows each glyph **up from its bottom edge** (vertices 1, 2 lerp from 0, 3), not
   from its left edge as the spec said; Unfold is from the horizontal centre line as specified. TMP's
   Gradient is evaluated at each glyph's one-shot progress q_i with the period wrapping (also for looping
   effects): our Gradient layer does that (spread 1 = the whole text one colour cycling). Typewriter: glyph i
   appears at (i + 1) / cps and fades in over `fadeTime`. Bounce is the spec's looping hop (TMP's Bounce is a
   one-shot cosine that ends displaced; `fromTMPEffect` maps it to our Bounce). `TextEffects::fromTMPEffect`
   maps a TMP Effect's settings to layers (Waves: amplitude 0.3 x A em, phase 0.3 rad; Sketch: Shake 0.1 x A;
   a non-white gradient: a Gradient layer with TMP's Mix), documented in ui.md and the glossary.
9. **Text Effect colours** are composed in sRGB (as TMP) from the text's colour and written as linear vertex
   colours with an alpha factor; the draw then takes only the alpha of `UIComponent::color`
   (`vertexColorAbsolute`). `onComplete` fires each time the one-shot layers end (with `loop` they start again
   right away); looping layers keep their phase across cycles. Reduced motion is the global `UI.reducedMotion`
   (the spec's `TextEffect.reducedMotion` read as that global). In edit mode effects hold still
   (`UISystem::setTextEffectsHeld`, set by the editor's pause of edit scenes) unless the Inspector's Preview
   plays them on the wall clock; Play and Stop start them again as a loaded scene does.
10. **Lua `TextEffect`** is a standalone wrapper (not an `EntityHandle`: its `setLayer(i, ...)` would clash
    with `EntityHandle.setLayer(layer)`); `getLayer(i)` returns a `TextEffectLayerRef` whose fields write through
    to the running effect; `setLayer(i, ...)` takes a `TextEffectLayer` or a plain table.
11. **UI particles (native).** Verified: Points sizes under a UI camera are UI units (`computePointsScale`
    uses the camera's ortho height). The scissor was NOT inherited (only entities with a RectTransform took
    the parent's scissor): fixed in `RenderSystem` so Points under UI use the nearest ancestor RectTransform's
    scissor (an empty one skips the draw), pinned by `FullPathGoldenTest.UIPointsClippedByAPanel`. An old scene
    with Points under a UI element now clips them to it (the rule of this spec, like UI children); nothing
    else changes. Effect indices follow reading order (logical clusters), so a right-to-left text types from
    its first character. "UI > Particle System" makes the legacy two-GameObject layout (a Points emitter "UI Particles"
    with a generated `ui/particle_soft.png` and its "Particle System" child: unscaled time, local space, a
    sparkle in UI units, y down) because the single-GameObject form belongs to the particles feature.
    `create_particles` under a UI element or in a UI scene does the same for its presets (y flipped, unscaled
    time, local space, the soft image). `useUnscaledTime` advances by the scene's delta divided by the time
    scale (the unscaled delta at time scale 0), which keeps editor previews (time scale 1) on their own step;
    `Engine::getUnscaledDeltatime()` is declared exactly as the coroutines branch declares it (a trivial merge).
12. **HTML `text-effect`.** Engine properties registered by the compat layer (`CompatProperty::TextEffect*`,
    PropertyTable rows with `RewriteId::TextEffect`, a rewriter that validates with the parser and reports
    `HTMLUI-CSS-0004` for an invalid value). Fork rows T21-1/T21-2: `TextShapingContext::text_effect` +
    `glyph_index_base`, `Rml::TextEffectProvider` (`ElementText::SetTextEffectProvider`), `ElementText::DirtyGeometry`
    and `CountEffectCharacters`; ElementText asks the provider once per geometry generation and advances the
    index per line; the engine's `HtmlTextEffects` finds the element that declares the effect, counts the
    visible characters of its texts in the linked inline formatting context and returns the time and count;
    `RmlFontEngine::GenerateString` applies the shared formulas to the base glyph box and maps every layer's
    quad (outlines, `font-effect` shadows, text-shadow passes) through it; a text-shadow pass
    (`TextShapingContext::text_effect_shadow`) keeps the shadow's colour and takes only the glyph's alpha. A
    `text-effect*` property set at runtime regenerates the text's geometry (`TextEffectProvider::IsTextEffectChange`),
    and new content (new text nodes joining the effect in a later frame) starts the effect again. A `px` amplitude is divided by the
    text's font size. `texteffectend` is dispatched (bubbling) at the declaring element; `el:restartTextEffect()`
    restarts it and its descendants' effects.
13. **`<particle-system>`.** `htmlui/UIParticleSystemElement.{h,cpp}`: the prefab through `PrefabManager`
    (registered in Play and exports; the editor's edit mode has no prefab factories, so the element is empty
    there, documented); the hidden entity is listed by `UIDocumentSystem::addRuntimeEntity` and the editor
    leaves it (and its children) out of the play-created GameObjects; the emitter is made invisible to the
    scene's renderers and simulated by the ActionSystem; the element draws one mesh per frame (premultiplied;
    additive particles with alpha 0). Prefab units are y up (the 2D convention). Exports include every prefab
    (`collectAllPrefabs`), so a prefab used only from a document is in the build. Diagnostic
    `HTMLUI-HTML-0029` (particle-system-source) for a src without a Particle System. The seam for the
    particles feature: `UIParticleSource::read`/`material` read Points render points and InstancedMesh
    instance data (position, scale, colour, textureRect); **when particles merges**: read
    `InstanceData.roll` into `UIParticleQuad::rotation` and, for its single-GameObject systems, take the
    emitter = the system's own entity (already the fallback when `ActionComponent.target` is empty) and the
    built-in particle texture of its quad material.
14. **Merges (platform-services, main with physics-2d, script-lifecycle, lines-trails, persistence,
    ui-canvas-group).** `TextEffectComponent` is appended after the Trail Renderer and the Canvas Group (the
    last ordinal; update flag bit 49). A Text Effect's alpha-only uniform goes through `uiDrawColor`, so a Canvas
    Group fades effect texts and, through the uniform alpha in the SDF shader, their outline and shadow (the
    canvas-group spec's "per-glyph alpha multiplies with the group"). `docs/mcp/guide/ui.md` keeps the Canvas
    Group section first, then this feature's sections. `ScriptLifecycleTest.PauseAndFocusMessages` was adapted
    to platform-services' rule that a pause takes the focus first and a resume does not return it. The HTML
    `text-effect` gradient colours parse with compat's CSS colour parser (no RmlUi needed: `jasperengine-editor
    check --ui` validated the guide's snippet without RmlUi and crashed); stops are `<color> [<percentage>]`
    words, so colours with inner spaces (`rgb(1 2 3)`) are not accepted there: use hex or names.
