# S27 — Editor tools: colour picker, curve editor, project hub, recorder

Wave 3. Owner area:
- `editor/widget/` (new `ColorField`, `ColorPickerWindow`, `CurveField`, `GradientField`, curve
  presets; S21 owns `CurveEditor` there).
- `editor/platform/ScreenColor*`, `editor/hub/`, `editor/resources/templates/` and
  `editor/recorder/` (all new).
- `editor/mcp/tools/EditorToolsTools.cpp` (new).
- Lua bindings for the S21/S20 curve and gradient types.

## 0. Re-check (oe-port `a57c7844d`, before implementing)

This spec was written before S20, S21, S24, S25, S29, S31 and three upstream rounds landed. What
the tree has now, and what S27 does with it. Where this section and §1–§9 disagree, this section
wins; the rows are repeated in §10 Deviations where they change a requirement.

| Spec assumption | What landed | S27 now |
|---|---|---|
| S21 D15 lands `engine/core/math/Curve.h` (weighted Bézier keys, per-side tangent modes, broken flag, five wraps) and `editor/widget/CurveEditor` (R6, R10) | S21 was re-scoped onto upstream's clips (S21 §0): **no D15, no `CurveEditor`**. "`math/Curve.h` (S20) stays the particle curve." S20's `Curve` is `std::vector<CurveKey{time, value, inTangent, outTangent}>`, cubic Hermite, held flat outside the keys, an infinite tangent is a step; helpers `constant(v)`, `linear(from,to)`, `polyline`, `easeInOut(t0,v0,t1,v1)`, `valueRange`, `derivative`. | S27 **extends S20's `Curve`** (no second type, no second evaluator): `CurveKey` gains a packed `mode` (in/out `TangentMode` Free/Auto/Linear/Constant/ClampedAuto + broken; 0 = Free/Free = tangents as stored, so every existing curve keeps its meaning), `Curve` gains `preWrapMode`/`postWrapMode` (`CurveWrapMode` Clamp/Loop/PingPong/Linear/LoopWithOffset; Clamp = today's held-flat) honoured by `evaluate`. Plus the R7 helpers and `using AnimationCurve = Curve`. **No weighted keys** (D1). S27 writes `editor/widget/CurveEditor` itself, for `Curve`. |
| `Gradient` (S20 D5) stores linear keys | S20's `Gradient` stores **sRGB-encoded** colour keys and a `linear` flag (blend space); `evaluate` returns linear RGBA. ≤ 8 + 8 keys, Blend/Fixed. | Used as is; the gradient field and editor show and edit sRGB keys; R12's HDR Intensity per colour key is dropped (D2: the type holds 0–1 sRGB keys). |
| `MinMaxCurve`/`MinMaxGradient` move into `Curve.h` (R6) | In `engine/core/particles/ParticleValues.h` (namespace `particles`), with particle driver/remap fields and `evaluate(input, random)` already. | Not moved (moving changes their namespace and they carry particle-only drivers, D3). Widgets and Lua use them where they are. |
| S27 moves the curve encoding out of S21's AnimYaml into `editor/stream/CurveCodec` (R45) | No AnimYaml. The one curve/gradient encoding is S20's `ParticleEffectCodec` (anonymous `curveToJson`/`gradientToJson`): a curve is `[[t, v, in, out], …]` (`null` tangent = step), a gradient `{colorKeys:[[t,r,g,b]], alphaKeys:[[t,a]], mode?, linear?}` (sRGB). | That encoding is the one codec: S27 moves it into the engine as `engine/core/math/CurveJson.{h,cpp}` (the particle codec calls it), extends it backward-compatibly (a key gets an optional 5th element, the mode token; a curve with a non-Clamp wrap becomes `{keys, preWrapMode?, postWrapMode?}`), and `editor/stream/CurveCodec` maps it to YAML, MCP JSON and the Factory literal. `Constant` tangents stay `null` (∞), as `.particles` files already write (D4). |
| Animation window gains R10's `CurveEditor` additions (R10, R14) | The Animation window's curves view is upstream's, over clip tracks (`AnimationWindowClipCurves`), with S21's `ClipTangentMode` (`tangent_mode` tokens `clampedAuto`, `auto`, `free`, `linear`, `constant`, `flat`, `broken`). | Out of S27: a different data model (clip tracks, quaternion tangents, per-segment eases). S27's tangent-mode tokens reuse S21's names (`tangentModeName`) for the shared modes (D5). |
| Particle inspector interim previews (R14) | `editor/window/InspectorParticles.cpp`: previews + key tables, a five-preset menu, and `ImGui::ColorEdit` for gradient keys and colours; `ForceFieldComponent.falloff` is an `ops::Opaque<Curve>` S08 field. | Replaced by `MinMaxCurveField`/`MinMaxGradientField`/`CurveField`/`GradientField` (R14); `falloff` gets the real `curveOps`. |
| Nine `ColorEdit` sites (R1, AC28) | **14 sites in 10 files**: `InspectorWindow.cpp` ×4 (rows + scene-property inputs), `InspectorParticles.cpp` ×2, `InspectorShaderLibrary.cpp`, `InspectorPostProcessing.cpp`, `InspectorFluid.cpp`, `TerrainEditWindow.cpp`, `NavigationWindow.cpp`, `dialog/ProjectSettingsWindow.cpp`, `dialog/EditorSettingsWindow.cpp` ×2. S08's `ColorLinear`/`ColorSRGB`/`Hdr` flags exist and drive `RowPropertyType::Color3L/Color4L`. | All 14 become `colorField`; AC28's grep covers every file. |
| S24 `InteractiveEdit` (R3) | Landed: `CommandHistory::beginInteractive(name)` → `InteractiveEdit{commit, cancel}`; App's `beforeRecord` hook already merges one ImGui widget drag into one step (S24 R11), keyed on the active ImGui id. | The Color/Curve/Gradient windows hold one session-wide `InteractiveEdit`; the hook skips while one is open (otherwise each picker slider would split the step). |
| S25 catalog/settings (R26, §8) | `shortcuts/DefaultCommands.cpp` + `AppMenuCommand`, `settings/SettingsRows.cpp` (SettingsRegistry), shortcut lint in `CommandCatalogTest`. F10 is free. | Commands `file.projectHub`, `file.saveProjectAsTemplate`, `window.recorder`, `window.colorPicker`, `recorder.toggle` (F10) and the settings `preferences.project_hub_at_startup`, swatch/curve library paths go there. |
| S29 `ProcessExec` (R33) | Landed: `editor/vcs/ProcessExec.{h,cpp}` `runProcess` (argv only, absolute exe, process group / job object kill, cancel token, never on the main thread), `findExecutable`, and Preferences `vcs.gitPath`. | S27 adds `ProcessStream` (stdin writer, line callbacks, `wait`, `kill`) beside it on the same spawn code; Git import uses `findExecutable("git", {configured = gitPath})`. |
| S31 tool groups and `call` (R42, R44) | Landed; the `editor_tools` group is already declared ("Colour picker, curve editor, project templates and movie recording"). BF8: the guide keeps ≥ 1,500 bytes free. | Tools go in `editor_tools` (core keeps `create_project`/`list_project_templates`/`import_project`); one guide line points to `jasper://docs/editor-tools`. |
| FFmpeg via `VideoImporter::findFfmpeg` (R25) | Unchanged: `JASPER_FFMPEG`, Preferences > External Tools `ffmpeg_path`, PATH. | The recorder uses it and the user's FFmpeg executable over a pipe: no libav linked, no FFmpeg shipped, no GPL code in the engine or editor. |
| Start-up, recent projects, zip (R15–R22) | Unchanged upstream: `App::loadStartupProject` still falls back to `createTempProject`; recents are a path list capped at 10. No hub, templates, recorder or zip reader exist (basisu's embedded miniz has no zip API). | Implemented as specified; miniz 3.0.2 (MIT) is vendored to `libs/miniz` from OpenEngine's `Engine/Source/ThirdParty/miniz` with its LICENSE and a `NOTICE.md` entry. |
| Export/scan exclusions (R31) | Now 9 sites (`Exporter.cpp` ×5, `Project.cpp` ×3, `ProjectChecks.cpp`); S30 (in flight) also touches `Exporter.cpp`. | All go through `Project::isNonAssetTopLevelDir`. |
| S28 crash handling | Editor crash recovery runs after a project loads. | Unaffected: the hub runs before any project load; recovery runs when the chosen project opens. |
| S08 reflection | Every component is on tables; custom kinds use `TypeOps`. | `TweenComponent.curve` and `ForceFieldComponent.falloff` use `curveOps` (YAML, MCP JSON, schema, Factory literal) with round-trip tests. |

## 1. Goal

This spec adds four editor tools that we lack today and that Unity users expect:
- A Unity-style **Color** picker for every colour field. It has an eyedropper, swatch libraries,
  HDR intensity and RGB/HSV/hex modes.
- An inspector **Curve** field, a floating Curve window, curve presets and a Lua
  `AnimationCurve` API. All of them sit on the one shared curve type and curve editor widget that
  S21 lands (S21 D15). Tweens use it in this spec; S20 and S21 use the same pieces.
- A **Project Hub** at start-up. It shows recent projects and templates with thumbnails, creates
  a project in its final folder, and imports from zip or Git. Today the editor starts in a temp
  project, and the user must use Save As.
- A **Recorder** for the Game view. It writes PNG sequences and H.264/ProRes movies with a fixed
  timestep, so trailers and bug repros come out frame-exact.

Agents get MCP tools for all four. Lua gets the curve and gradient types at runtime.

## 2. Scope

In scope:
- `ColorField` everywhere a colour is edited. The picker has SV square, hue wheel and sliders in
  RGB 0–255, RGB 0–1 and HSV. It also has hex, alpha, HDR intensity in stops, the eyedropper
  (OS screen read-back plus exact viewport read-back) and swatch libraries per user and per project.
- Inspector integration of the shared runtime types: `Curve` (S21 D15, Lua name
  `AnimationCurve`), `Gradient` (S20 D5), `MinMaxCurve` and `MinMaxGradient` (S20 R2). This covers
  their component-field codecs (YAML, MCP JSON, Factory codegen), Lua bindings and helper
  constructors.
- `CurveField`/`MinMaxCurveField`/`GradientField` widgets, the floating **Curve** window that
  hosts S21's `CurveEditor`, the additions to `CurveEditor` listed in R10, the Gradient Editor,
  and built-in and user curve presets.
- Consumers in this spec: every `TweenComponent` (new ease type `CURVE`), and the particle
  inspector fields (R14). S20 and S21 own their runtime consumers.
- The Project Hub: Projects, New Project (templates), Open and Import (zip, Git). Shift held at
  start-up, `--hub` and File > Project Hub… open it. Four built-in templates, user templates and
  Save Project as Template….
- The Recorder window: Image Sequence (PNG) and Movie (MP4/H.264, optional MOV/ProRes) output,
  fixed frame rate, output resolution, recording modes (Manual / Single Frame / Frame Interval /
  Time Interval), audio capture for movies, start on Play, F10. A CLI `record` subcommand.

Out of scope:
- OpenEngine's retained CSS editor UI toolkit and its docking (README do-not-copy list). Every
  widget here is Dear ImGui.
- Their community project catalog fetched over the network (07-editor-ux.md §"skip"). Only local
  and user templates are offered. The slug and path validation is still ported (R33).
- Linking libav, or shipping FFmpeg. We pipe to the user's FFmpeg executable, as the video
  importer already does. Also out: HEVC, 10-bit/HDR video, an end fade, variable frame rate,
  recording cameras other than the Game view, 360/AOV capture, EXR sequences, alpha in recordings,
  and a runtime (in-game) recorder.
- The xdg-desktop-portal colour picker on Wayland (§9).
- Animation track migration to `AnimationCurve` (S21) and the particle module curves (S20).

## 3. Current state

- **Colours.** Every colour edit is stock `ImGui::ColorEdit3/4` with `NoInputs|NoLabel`:
  - `RowPropertyType::Color3L/Color4L` in `editor/window/InspectorWindow.cpp:3846,3888`. It
    converts linear↔sRGB through `Color::linearTosRGB`/`sRGBToLinear` and issues one `PropertyCmd`
    for each ImGui change, so a drag makes many undo steps.
  - `ScenePropertyInputType::ColorRGB/RGBA` in `InspectorWindow.cpp:556,561`.
  - `InspectorShaderLibrary.cpp:361`, `InspectorPostProcessing.cpp:377`,
    `TerrainEditWindow.cpp:2679`, and the particle gradient stops at `InspectorWindow.cpp:10489`.

  There is no eyedropper, no palette and no HDR intensity. `Material.emissiveFactor`
  (`InspectorWindow.cpp:6807`) uses the same LDR row. S08 adds the `ColorLinear`/`ColorSRGB`/`Hdr`
  field flags (`docs/specs/S08-reflection.md` R4).
- **Curves: none.** Tweens hold one `Ease` (`engine/core/component/TweenComponent.h`). `EaseType`
  ends in `CUSTOM`, which is a `std::function` that cannot be serialized (`engine/core/action/Ease.h:14,47`).
  Particles use `{fromTime,toTime,from,to,Ease}` modifiers and a `ParticleColorGradient` stop
  list (`engine/core/component/ParticleSystemComponent.h:116-140`). Animation tracks store
  glTF Hermite `inTangents/outTangents` per key (`TranslateTracksComponent.h:13-18`) and per-segment
  `EaseType`s (`KeyframeTracksComponent.h`). The Animation window keeps tangents mirrored, but it
  has no graph (`editor/window/AnimationWindow.cpp:543-600`).
- **Planned by other specs.** S21 D15 (wave 2) lands `engine/core/math/Curve.h`. Keys are
  `{time, value, inTangent, outTangent, inWeight, outWeight, mode}`. Tangent modes per side are
  Auto/ClampedAuto/Free/Linear/Constant, plus broken. Weighted keys are Bézier and unweighted keys
  Hermite. Wraps are Constant/Linear/Loop/PingPong/CycleWithOffset. S21 also lands
  `editor/widget/CurveEditor.{h,cpp}` (multi-curve, tangent handles, wrap ghosting, reference
  curves) and says "S27 adds presets and the library". S20 D5 adds `Gradient` (≤ 8 colour + ≤ 8
  alpha keys, Blend/Fixed) to the same header. S20 R2 names `MinMaxCurve`/`MinMaxGradient`.
- **Projects.** File > New Project creates `temp_directory_path()/MyJasperEngineProject`
  (`editor/App.cpp:605-622`, `editor/Project.cpp:5072`), which then needs Save Project As.
  - Start-up (`App::loadStartupProject`, `App.cpp:2276-2333`, called from the three `Backend*.cpp`)
    loads the last project or falls back to the temp project.
  - Recent projects are a list of up to 10 paths in `settings.yaml` (`editor/AppSettings.cpp:496-515`),
    shown in File > Recent Projects (`App.cpp:456-490`).
  - `Project::createProjectAt` (`Project.cpp:4199`) already creates a project at its final place,
    and MCP `create_project` uses it (`editor/mcp/tools/ProjectTools.cpp:1977`).
  - There are no templates, no hub, no zip library (`engine/libs`, `libs`) and no stdin-capable
    process helper. `editor/util/CommandRunner.h:24` runs shell command lines and streams stdout only.
  - Editor resources are embedded as generated headers; `CMakeLists.txt:363-383` embeds
    `docs/mcp/guide/html-ui-examples/**`.
- **Capture.** Screenshots exist only via MCP `screenshot` (`editor/mcp/tools/ViewTools.cpp:296-350`).
  - It uses `GraphicUtils::readFramebufferPixels` (`editor/util/GraphicUtils.cpp:68`), a synchronous
    full read-back for GL/Vulkan/Metal/D3D11.
  - The editor-UI read-back is GL-only (`ViewTools.cpp:553-580`).
  - The Game view framebuffer always follows the panel size (`editor/window/GameWindow.cpp:466-476`).
  - Determinism already exists: `Engine::setFixedDeltatime` (`engine/core/Engine.h:421-426`), used by
    MCP `play` `fixed_fps` (`PlayTools.cpp:252-257`) and reset when play stops (`Project.cpp:4515`).
  - FFmpeg is located by `VideoImporter::findFfmpeg`: `JASPER_FFMPEG`, then Preferences > External
    Tools > FFmpeg (`AppSettings::ffmpegPath`, `AppSettings.cpp:163-166`), then PATH
    (`editor/htmlui/VideoImporter.h:46-53`). Encoders are probed with `ffmpeg -encoders`.
  - Audio is SoLoud, initialised with the default device (`engine/core/subsystem/AudioSystem.cpp:32-50`).
    SoLoud has `NULLDRIVER` and a manual `mix()` (`engine/libs/soloud/include/soloud.h:196,455`).
  - The Linux editor is X11 (`editor/backend/BackendLinux.cpp` includes Xlib).
  - Exports and asset scans skip top-level dirs that start with `.` or are named `build`
    (`editor/Exporter.cpp:1759,1828,1895,1995,2029`, `Project.cpp:2213`). The default assets dir is
    the project root (`Project.h:959`).

## 4. Reference (OpenEngine, MIT, © 2026 Gamecrafters Guild Limited and contributors)

Paths are relative to `external/OpenEngine/`. The files have no per-file header. Copied code
gets the MIT notice block in the file and a `NOTICE.md` entry.

| File | Take | Improve |
|---|---|---|
| `Apps/Editor/Source/Panels/ColorPicker.cpp:40-58,682-728` | The palette model: 19 slots per row, up to 4 rows, empty-slot sentinel, default spectrum row, "click the current swatch to add it" | Named libraries per user and per project in files, not one global prefs key |
| `Apps/Editor/Include/InspectorRegistry.h:31-42` `ColorPickerCallbacks` | The `onValueChanging`/`onApply`/`onCancel` triad with `(argb, intensity)` | Linear float RGBA and intensity in stops, not 8-bit ARGB |
| `Engine/Modules/Platform/Source/Window.cpp:1615-1655` | Win32 `GetDC(NULL)`+`GetPixel` and X11 `XGetImage` on the root window, for reading screen pixels | Read a 9×9 block once per frame for the loupe. Do not open an X display per call |
| `Engine/Modules/Platform/Source/ScreenColor_mac.mm` | — (do not copy: it `dlsym`s the obsolete `CGWindowListCreateImage` and needs Screen Recording permission) | Use `NSColorSampler` |
| `Apps/Editor/Include/UI/Controls/CurvePresets.h`, `Source/UI/Controls/CurvePresets.cpp` (299 lines) | Presets stored normalised to [0,1]² and remapped by the host, with the "My Preset N" naming | JSON `.curves` libraries per user and per project |
| `Apps/Editor/Source/UI/Controls/CurveField.cpp` (1189 lines) | The field thumbnail, plus open-on-click with the field's range | ImGui |

The curve maths and graph references (`Mathematics/Curve.h`, `AnimationClip.h:34-53`,
`Panels/CurvesGraphView.cpp`) belong to S21 §4.
| `Apps/Editor/ProjectTemplates/manifest.json` | The fields `id, name, description, accentColor, thumbnail, source` | One `template.json` per template folder |
| `Apps/Editor/Source/Projects/ProjectCatalog.cpp:18-45` | `IsValidCatalogId` and `IsSafeRelativePath`. Copy them verbatim into `editor/hub/HubPaths.cpp` | — |
| `Apps/Editor/Source/Projects/ProjectAcquisition.cpp:33-125`, `ZipArchive.cpp` | Git lookup (PATH plus the Windows default install dirs), argv-only `git clone`, read-only pack-file removal on Windows, miniz extraction | Zip-slip and size caps (R34) |
| `Apps/Editor/Source/MovieRecorderController.cpp:78-90` | Resolution and bitrate presets (1080p/4K × 24/25/30/60), uniquified output names | Unity Recorder naming and modes |
| `Apps/Editor/Source/GameViewMovieCapture.cpp:24-32,611-680` | A bounded read-back queue and a bounded audio buffer | Offline audio mix in step with the fixed timestep (theirs taps live device output, which drifts when the game runs slower than real time) |
| `Engine/Source/Engine/Video/VideoWriterFFmpeg.cpp:61-71` | Encoder preference lists (`h264_mf`/`libx264`/`libopenh264`, `prores_ks`) | Probed through `ffmpeg -encoders`, no libav link |

## 5. Requirements

### Functional — colour

- **R1** `ColorField` replaces all nine call sites in §3. They store linear values.
  `ColorSRGB` fields (S08) and `ScenePropertyInputType::ColorRGB` UI tints keep sRGB storage. The
  field shows a swatch: alpha is drawn as a checker half, and HDR fields get an "HDR" tag. Clicking
  the swatch opens the **Color** window.
- **R2** The Color window has:
  - an SV square, or a hue wheel (toggle);
  - a mode dropdown **RGB 0-255 / RGB 0-1.0 / HSV** with sliders and numeric fields;
  - **Hexadecimal** (`RRGGBB` or `RRGGBBAA`, always sRGB 8-bit);
  - **A** (alpha, when the field has alpha);
  - **Intensity** (HDR fields only, −10 … +10 stops).
  
  Sliders and hex show sRGB-encoded values; the field stores linear values. For an HDR field the
  stored value is `base × 2^intensity`. On open, intensity = `max(0, log2(maxComponent))`, so an
  emissive of (4,2,0) opens as base (1,0.5,0) at intensity 2. A hex/RGB round trip of any 8-bit
  value is lossless.
- **R3** Live edit: opening the window starts an S24 `InteractiveEdit` named "Change <Label>".
  Every change writes the target live, through merging `PropertyCmd`s inside that edit. Closing
  the window or pressing Enter commits **one** undo step. Esc cancels the edit, which restores
  the value from when the window opened and records nothing. Before S24 lands, the same contract
  is met with `setNoMerge()` on commit and a stored original value on cancel. Multi-selection writes
  all selected entities in that one step. Only one Color window exists. Clicking another colour
  field retargets it and commits the previous edit first.
- **R4** Eyedropper (button and `I` while the window is focused):
  - **Windows and Linux/X11.** The editor enters a sampling mode with a crosshair cursor and a
    loupe that magnifies 9×9 pixels around the cursor, shown near it. The loupe reads screen
    pixels: GDI `BitBlt` from the screen DC on Windows, `XGetImage` on the root window on X11, one
    display connection per session. The target updates live. Left-click picks. Esc or right-click
    cancels and restores the old value. Sampling works anywhere on any monitor, including
    detached editor viewports.
  - **Scene and Game views (all platforms).** When the cursor is over one of them, the colour is
    read from the view's framebuffer at that texel through a new 1×1 sub-rect read
    (`GraphicUtils::readFramebufferRect`). This value is exact: no compositor or display-profile
    conversion.
  - **macOS.** Uses `NSColorSampler`, the system loupe. The result is converted with
    `colorUsingColorSpace:[NSColorSpace sRGBColorSpace]`. There is no live preview, and no Screen
    Recording permission is needed.
  - **Wayland session** (`WAYLAND_DISPLAY` set and screen read fails). Only viewports and our
    own X windows can be sampled. Elsewhere the loupe shows "Outside the editor: not available on
    Wayland".
- **R5** Swatches. The window shows the active library as rows of 19 swatches.
  - Click applies a swatch. Clicking "+" (the current colour) appends it.
  - Right-click offers Replace, Rename, Delete, Move Left/Right.
  - The library dropdown offers New Library…, Rename, Delete, Reveal in File Browser.
  - Libraries are JSON files `*.colors` (§6.5). **User** libraries live in `<settings dir>/swatches/`;
    `<settings dir>` is the folder of `settings.yaml` (`AppSettings.cpp:81-104`). **Project**
    libraries live in `<project>/EditorSettings/Swatches/` and go into version control.
  - A user library "Default" is seeded with black, white, grey and a 16-hue spectrum.
  - A swatch keeps its linear value and its HDR flag. Applying an HDR swatch to an LDR field clamps
    it and shows a one-line warning.

### Functional — curves and gradients

- **R6** One type and one evaluator. S27 introduces no curve or gradient type of its own and no
  second evaluator. It uses S21 D15 `Curve` (semantics normative there), S20 D5 `Gradient`, and
  `MinMaxCurve`/`MinMaxGradient`.
  - The four types live in `engine/core/math/Curve.h`. If S20 placed the min-max types under
    `particles/`, S27 moves them into `Curve.h`; the move changes no API.
  - `Curve.h` gets `using AnimationCurve = Curve;` so that C++ can use Unity's name.
  - **Fallback if S27 lands before S21:** S27 implements D15 exactly as S21 specifies it, with
    S21's tests, and S21 then extends it.
- **R7** Helpers added to `Curve.h`, where the owners have not already added them:
  - `Curve::constant(t0,t1,v)`, `linear(t0,v0,t1,v1)` and `easeInOut(t0,v0,t1,v1)` (Unity statics);
  - `fromEase(EaseType, int keys = 16)`, the shared sampler that S20's legacy migration also needs;
  - `smoothTangents(i, weight)`;
  - `bake(std::span<float>, t0, t1)`;
  - `Gradient::bake(std::span<Vector4>)`;
  - `MinMaxCurve::evaluate(t, random01)`, which honours `multiplier` and the four modes (Constant /
    Random Between Two Constants / Curve / Random Between Two Curves);
  - `MinMaxGradient::evaluate(t, random01)` for Color / Gradient / Random Between Two Colors /
    Random Between Two Gradients.
  
  Every helper is pure, allocation-free in `evaluate`, and has no SIMD requirement.
- **R8** Wrap and tangent labels in the UI use Unity names over S21's enum values:
  - Wrap: Clamp (`Constant`), Loop, Ping Pong, Linear, Loop with Offset (`CycleWithOffset`).
  - Key menu: Clamped Auto, Auto, Free Smooth (Free + not broken), Flat (Free, slope 0), Broken,
    and Left/Right/Both Tangents ▸ Free / Linear / Constant / Weighted.
- **R9** `CurveField` shows the curve as a thumbnail in the field's colour on a dark background,
  with the range outline. The polyline is cached and rebuilt only when the curve changes. Clicking
  the field opens the floating **Curve** window (one instance, retargeted like the Color window).
  Right-click offers Copy, Paste, Reset and Presets ▸. `MinMaxCurveField` adds Unity's mode
  dropdown (the small triangle) and a constant/two-constant inline editor. Its curve modes open
  the Curve window with both curves.
- **R10** The Curve window hosts S21's `CurveEditor`. S27 adds the features below to
  `CurveEditor` where S21 has not shipped them, so that the Animation window gains them too:
  - **Min-max band.** A curve binding may name a partner curve. The area between the two is shaded
    in the curve colour at 25 % alpha.
  - **Range clamp.** Unity's `ranges` argument: key times and values are clamped to a rect, and
    the rect is drawn.
  - **Key editing.** Double-click on a curve adds a key on the curve. Double-click on empty space
    adds one at the cursor on the first visible editable curve. Delete removes keys. Ctrl+C/Ctrl+V
    copy and paste keys at the cursor time. The R8 key context menu.
  - **Bottom bar.** Numeric Time and Value of the selected key, pre and post wrap dropdowns, and
    the preset strip (R11).
  - **View.** F frames the selection, or all keys when nothing is selected.
  - **Undo.** Each gesture (drag, menu action, numeric commit) is one S24 `InteractiveEdit`, so it
    becomes one undo step.
- **R11** Presets. The built-in presets are Constant, Linear, Linear Down, Ease In, Ease Out,
  Ease In Out, Ease In Out Down and Bell. They are normalised to [0,1]² and remapped to the field's
  range. "Save Current as Preset" writes to the active `.curves` library. User libraries live in
  `<settings dir>/curves/` and project libraries in `<project>/EditorSettings/Curves/` (format §6.5).
- **R12** `GradientField` and the **Gradient Editor** window (Unity layout):
  - Alpha keys sit above the bar and colour keys below it. Click adds a key, drag moves it, and
    dragging a key off the bar deletes it.
  - The Color or Alpha and Location fields edit the selected key. The colour opens the Color window.
  - Mode is Blend or Fixed. The window has presets, using the swatch libraries' gradient section.
  - Limits are enforced (8 colour keys + 8 alpha keys).
  - The gradient bar is drawn in sRGB from linear keys. HDR gradients get an Intensity field for
    each colour key.
- **R13** Tweens. `EaseType::CURVE` is appended after `CUSTOM`, and `TweenComponent` gains
  `Curve curve` (default `linear(0,0,1,1)`). It is used when the ease is `CURVE`.
  - The ease row shows "Curve" in its dropdown. Selecting it reveals a `CurveField` with range
    [0,1]×[−1,2].
  - It works in the editor, in Play mode and in every export target (Factory codegen).
  - `Ease::getValue` on a `CURVE` ease returns `curve.evaluate(t)`, so every tween action gets it
    unchanged.
- **R14** Other consumers:
  - S20's particle inspector replaces its interim read-only curve and gradient previews (S20 §9
    default) with `MinMaxCurveField` and `GradientField`.
  - If S20 has not landed, the legacy "Color Gradient" row (`InspectorWindow.cpp:10489`,
    `setParticleColorGradient` at `:5447`) becomes a `GradientField` through a
    `ParticleColorGradient` ↔ `Gradient` adapter (colour keys only, alpha 1).
  - The Animation window keeps S21's embedded `CurveEditor` and gains the R10 additions.

### Functional — project hub and templates

- **R15** The **Project Hub** is an ImGui window that fills the main window. While it is open,
  `App::show` draws only the hub and does not load a project, in the same way the `projectLoading`
  early return works (`App.cpp:1871-1878`). Its left navigation has:
  - **Projects.** Recent projects as rows with thumbnail, name, path and "Opened 3 days ago", plus a
    pin, a search box, and sort by Last Opened or Name.
    - Missing projects are greyed, with Locate… and Remove from List.
    - Right-click offers Show in Explorer/Finder, Remove from List, and Open Without Last Scene.
  - **New Project.** A template grid (cards 240×135 with thumbnail, name, description, accent
    bar), sectioned into Built-in and My Templates.
    - The Project Name field defaults to "New Project", and Location defaults to the last used
      parent folder, else `~/JasperEngine Projects`. Browse… uses nfd.
    - **Create** is disabled while `<location>/<name>` exists and is not empty. The reason is
      shown inline.
  - **Open.** Open Folder… (nfd). Picking a folder without `project.yaml` shows "Not a JasperEngine
    project".
  - **Import.** From Zip… (file picker) and From Git URL… (URL field). Both have a target parent
    folder, a progress bar with a log tail, and Cancel.
- **R16** The hub is shown when any of these holds:
  - Shift is held at launch: `GetAsyncKeyState(VK_SHIFT)` on Windows, `[NSEvent modifierFlags]` on
    macOS, `XQueryKeymap` on X11.
  - `--hub` is passed.
  - Preferences > General > **Show Project Hub at Startup** is `Always`. The default is
    `When No Project`.
  - The last project is missing or fails to load. This replaces the temp-project fallback in
    `loadStartupProject`.
  
  It is never shown with `--project`, `benchmark`, `record` or `mcp`. Those keep today's
  behaviour, including MCP's temp project.
- **R17** File > **New Project…** opens the hub on New Project. **File > Project Hub…** opens it
  on Projects. File > Open Project… opens the hub on Open. All three first apply today's
  unsaved-changes prompt (`App.cpp:613-621`). Closing the hub with Esc returns to the open project,
  if there is one. The temp project is no longer reachable from the UI.
- **R18** Create from template:
  - Copy the template folder into the target, skipping `template.json` and the thumbnail.
  - Load it with `Project::loadProject`, `setName(<name>)`, `saveProject()`,
    `copyEngineApiToProject()` and the `.gitignore`/`.gitattributes` writer (`Project.cpp:5232-5320`).
  - Open the template's `startScene`.
  - S04 `.meta` GUIDs inside the template are kept. GUIDs are project-scoped, so two projects from
    one template do not conflict.
  - The work runs off the main thread with the LoadingWindow. A failure removes the partly
    created folder.
- **R19** Built-in templates live in `editor/resources/templates/<id>/`. They are embedded in the
  editor binary by a CMake generator modelled on `CMakeLists.txt:363-383`
  (`project_templates.h`), so no install path is involved. Each template is a normal project saved
  by the editor, plus `template.json` and `thumbnail.png` (480×270).
  - `empty` "Empty": one 3D scene `Main` with only a Main Camera.
  - `3d` "3D": `Main` 3D scene with Main Camera, a Directional Light (shadows on), the default
    Daylight skybox, a 20×20 m ground plane, a cube, a `materials/Ground` material, the post
    processing preset "Default", and `scripts/Rotator.lua` attached to the cube.
  - `2d` "2D": `Main` 2D scene with an orthographic camera, `sprites/player.png` (generated
    placeholder art) on a sprite with `scripts/PlayerMove.lua` (arrow keys/WASD), and a 1280×720
    canvas with Letterbox scaling.
  - `ui` "UI": a UI scene with a UI Document `ui/main_menu.html` + `ui/main_menu.css` (title and
    Play/Quit buttons wired in `scripts/MainMenu.lua`) and a 1920×1080 canvas.
- **R20** User templates. **File > Save Project as Template…** asks for name and description and
  writes `<settings dir>/templates/<slug>/`. It copies the project without `.jasperengine/`,
  `build/`, `Recordings/`, `.trash/` and `.git/`, and writes a thumbnail from the Scene view
  (`encodeFramebufferPng`, 480 wide). Ids are validated with `IsValidCatalogId`.
- **R21** Recent projects become records in `settings.yaml`:
  `recent_projects: [{path, name, last_opened (ISO 8601 UTC), pinned, thumbnail}]`, capped at 30
  (pinned entries are never evicted). The old string list loads as path-only records. The File >
  Recent Projects menu shows the first 10. A project thumbnail (`.jasperengine/user/hub-thumbnail.png`,
  320×180, from the Scene view) is written when the project closes.
- **R22** Zip import:
  - Uses vendored **miniz** (MIT, `NOTICE.md` entry) in `libs/miniz`, editor only.
  - Extracts to `<target>.importing/`. The project root is the shallowest folder (depth ≤ 2) that
    contains `project.yaml`. That folder is moved to `<target>/<name>`.
  - If no `project.yaml` is found, the import fails and removes everything it wrote.
  - Rules are in R34.
- **R23** Git import:
  - Uses the `git` executable (PATH, the Windows default install dirs, or S29's configured
    path if S29 has landed).
  - Runs `git clone --depth 1 --progress -- <url> <dir>` through `ProcessExec` (R33) with an
    argv array, never through a shell.
  - Accepted URLs are `https://`, `ssh://` and `git@host:path`. Anything starting with `-` is
    refused, and `ext::`/`file://` are refused.
  - If `.gitattributes` has `filter=lfs` and `git lfs` exists, `git lfs pull` runs. Otherwise a
    warning is shown.
  - Progress lines come from stderr.
  - Cancel kills the process tree and removes the folder.
  - When S29 has landed, a project created from a template gets `editor.vcs.lfs: true`
    (S29 R21).

### Functional — recorder

- **R24** **Window > General > Recorder** opens the **Recorder** window. It has:
  - **Recorder**: Image Sequence or Movie.
  - **Capture**: Game View.
  - **Output Resolution**: Match Game View, 480p, 720p, 1080p, 1440p, 2160p or Custom W×H.
    Width and height are 16 … 8192 and ≤ the device max texture size. Movie output must have even
    dimensions; the UI rounds to even.
  - **Frame Rate**: 24, 25, 30, 50, 60 or Custom (1 … 240). The rate is always constant.
  - **Recording Mode**: Manual, Single Frame (n), Frame Interval [a,b] or Time Interval [s0,s1] s.
    Frame 0 is the first game frame after Play starts.
  - **Exit Play Mode** when done (checkbox).
  - **Output**: a folder, default `<project>/Recordings`, and a file name with the wildcards
    `<Scene>`, `<Take>`, `<Date>`, `<Time>`, `<Resolution>` and (sequence) `<Frame>`. Defaults are
    `<Scene>_<Take>` and `<Scene>_<Take>/frame_<Frame>`. `<Frame>` is 4-digit zero-padded and
    starts at the first recorded frame.
  - **Take** number, which auto-increments after each recording.
  
  The settings persist per project in `.jasperengine/user/recorder.yaml`.
- **R25** Movie options:
  - **Format**: MP4 (H.264), or MOV (ProRes 422 HQ). MOV is offered only when the FFmpeg has
    `prores_ks`.
  - **Quality**: Low, Medium or High. For `libx264` this maps to CRF 28/23/18 with preset
    `medium`. Hardware encoders get bitrate = pixels × fps × {0.05, 0.1, 0.2} bits.
  - **Capture Audio**.
  
  The H.264 encoder is the first one present in `ffmpeg -hide_banner -encoders` from this list:
  `libx264`, `h264_videotoolbox`, `h264_mf`, `h264_nvenc`, `h264_qsv`, `h264_amf`, `libopenh264`.
  Without FFmpeg or any H.264 encoder, Movie is disabled with the reason and a "Open Preferences…"
  link. Image Sequence always works.
- **R26** Start and stop:
  - **Start Recording** (button, toolbar ● next to Play, and F10 through S25's shortcut catalog)
    enters Play if needed and starts at frame 0.
  - **Record on Play** starts automatically when the user presses Play.
  - **Stop Recording**, F10, stopping Play, or reaching the end of the mode finalises the output.
  - Pausing Play pauses capture, and Step captures exactly one frame.
  - The Game view tab shows a red "REC 00:12 · frame 734" overlay while recording. The overlay is
    not part of the recorded image.
- **R27** Fixed timestep and resolution. While recording:
  - `Engine::setFixedDeltatime(1/fps)` is set and VSync is off. It is restored on stop, composing
    with MCP `fixed_fps`.
  - Every editor frame renders exactly one game frame. On-demand redraw is suspended.
  - The Game view renders into a framebuffer of exactly the output resolution. A new
    `GameWindow::setRenderSizeOverride(w,h)` makes the Game view show it scaled to fit.
- **R28** Capture pipeline:
  - After the game frame renders, the main thread reads the Game framebuffer back (RGBA8, as shown).
  - It pushes the frame into a bounded queue of 8 frames. When the queue is full, the main thread
    **waits**, so frames are never dropped.
  - PNG sequence: S06 background jobs (a 3-thread pool before S06) encode PNGs with
    `stbi_write_png` (compression level 3) and write them with S03 `writeFileAtomic`.
  - Movie: one writer thread streams raw frames to `ffmpeg -f rawvideo -pix_fmt rgba -s WxH
    -framerate F -i pipe:0 …` over the stdin of a `ProcessExec::Stream` (R33). Output is BT.709 tagged,
    `yuv420p` for H.264 with `-movflags +faststart`, and `yuv422p10le` for ProRes.
- **R29** Audio capture (decision):
  - Audio is captured **offline in step with the fixed timestep**, not tapped from the device.
  - With Capture Audio on, the recording must start with Play. If it starts mid-play, audio is
    off and a warning is shown.
  - `AudioSystem::beginOfflineCapture(48000, 2)` re-initialises SoLoud with `NULLDRIVER` before
    the scene starts. After each game frame k, the recorder calls `mix` for exactly
    `round((k+1)·48000/fps) − round(k·48000/fps)` frames into a temp WAV.
  - At finalise, a second FFmpeg pass muxes the audio: `-c:v copy` and AAC 192 kb/s (MP4) or
    `pcm_s16le` (MOV).
  - The user hears no audio while capturing. The window shows a banner saying so.
    `endOfflineCapture()` restores the device when Play stops.
- **R30** Finalising closes stdin and waits for FFmpeg (timeout 120 s, then kill). It deletes the
  temp WAV and writes a sidecar `<output>.recording.json`
  `{frames, fps, width, height, encoder, scene, take, startFrame, warnings}`. The Console gets a
  clickable "Recorded N frames to <path>" message. On failure, FFmpeg's last 20 stderr lines are
  logged and the partial file is kept, renamed `*.failed.mp4`.
- **R31** `Recordings/` and `EditorSettings/` join `.` and `build` in one predicate,
  `Project::isNonAssetTopLevelDir`. That predicate is used by the export and scan sites in §3.
  `Recordings/` is added to the generated `.gitignore` (`Project.cpp:5232`).

### Platform & backends

- **R32** Widgets, hub and recorder work in the editor on Windows, Linux (X11) and macOS, with
  every editor backend (`GRAPHIC_BACKEND` gl/vulkan/metal, `CMakeLists.txt:321-327`). The runtime
  types (R6–R8, R13) and their Lua bindings compile and pass tests on every export target,
  including Emscripten and Android. They use no SIMD requirement and no exceptions.
- **R33** Processes go through S29's `editor/vcs/ProcessExec.{h,cpp}` (S29 R2: argv-only,
  absolute exe, no shell, kill-tree). S27 adds `ProcessExec::Stream` next to it, built on the same
  spawn code. It offers `start(spec)`, a blocking `writeStdin(span)`, `closeStdin()`, stdout and
  stderr line callbacks on a reader thread, `wait(timeoutMs)` and `kill()`. If S27 lands first,
  it creates the file with S29's R2 `run` contract and S29's AC1 test, so that S29 only extends it.
- **R34** Import safety:
  - Entries are rejected if they are absolute, have a drive letter, contain `..` or `\`, or are
    symlinks or device files.
  - The uncompressed total must be ≤ 8 GiB and ≤ 200,000 entries, and the free disk space is
    checked first.
  - Ids and relative paths go through the ported `IsValidCatalogId`/`IsSafeRelativePath`.

### Performance

- **R35** `bake` of 64 entries from a 16-key curve takes ≤ 3 µs on the CI x86-64 runner. An
  inspector with 20 visible `CurveField`s costs ≤ 0.2 ms CPU per frame because of the cached
  polylines. S21 owns the evaluate budget.
- **R36** The open Color window costs ≤ 0.3 ms CPU per frame. Eyedropper sampling costs ≤ 1 ms per
  frame (one 9×9 screen read, or one 1×1 framebuffer read). A closed picker costs nothing.
- **R37** On the CI Linux GL runner, recording 1080p30 to PNG keeps ≥ 10 recorded frames per wall
  second, and H.264 (libx264) keeps ≥ 20 frames per wall second. Recorder memory is ≤ 8 queued
  frames + 64 MiB.
- **R38** The hub shows the recent list within 150 ms of start-up with 30 entries. Thumbnails load
  asynchronously.

### Editor UX (human)

- **R39** All names follow Unity: Color, Hexadecimal, Intensity, Swatches, Curve, Wrap Mode
  (Clamp/Loop/Ping Pong/Linear), Clamped Auto, Free Smooth, Broken, Recorder, Recording Mode,
  Take, Exit Play Mode, Capture Audio. Every edit is undoable with a named step. Every window
  remembers its size and position in the layout.
- **R40** `docs/editor/color-picker.md`, `curves.md`, `project-hub.md`, `recorder.md` and the MCP
  agent guide sections are added. `docs/unity-glossary.md` gets the new terms.

### Agent UX (MCP, Lua, CLI)

- **R41** MCP values (`editor/mcp/McpValues.cpp`), accepted by `set_properties`, `set_scene_settings`
  and similar tools:
  - **Colours** can be `"#RRGGBB"` / `"#RRGGBBAA"` (sRGB), `[r,g,b(,a)]` (the field's own space,
    unchanged), or `{"srgb":[…]}` / `{"linear":[…]}`, with optional `"intensity"` (stops, HDR only).
  - **Curves, min-max curves and gradients** use the JSON of §6.5. Errors give the path and reason.
  - `get_entity` returns curves in that JSON.
- **R42** New MCP tools in `EditorToolsTools.cpp`, registered in S31's `editor_tools` group.
  `create_project`, `list_project_templates` and `import_project` stay in `core`. Names and
  schemas follow S31's conventions.
  - `record_game {frames | seconds, start_frame=0, fps=30, width, height, format: png|mp4|mov,
    quality, audio=false, output?, exit_play=true, timeout_seconds}`: a job (`McpJobs`) that enters
    Play if needed. It returns the sidecar JSON plus the file list (PNG) or the movie path.
    `cancel_job` stops and finalises.
  - `sample_view_color {view: game|scene, x, y}`: returns the pixel in `{hex, srgb, linear}`, using
    the coordinate convention of `screenshot`.
  - `edit_swatches {action: list|add|remove|create_library|delete_library, scope: user|project,
    library, name?, color?, index?}`.
  - `list_curve_presets {}` and `evaluate_curve {entity, component, property, times[]}`, which
    evaluates through `Curve::evaluate` (or `MinMaxCurve`/`Gradient::evaluate`, with a `random`
    argument).
  - `list_project_templates {}`, `import_project {source, path, timeout_seconds}` (zip path or Git
    URL), and `create_project` gains `template` (default `empty`).
- **R43** Lua (`engine/core/script/binding/MathClassesLua.cpp`, stubs in `stubs/`):
  - `Keyframe(time, value[, inTangent, outTangent])` with fields `time, value, inTangent,
    outTangent, inWeight, outWeight`, plus the properties `inTangentMode, outTangentMode, broken,
    weightedMode` (views onto S21's packed `mode`).
  - `AnimationCurve()`, `AnimationCurve.linear(t0,v0,t1,v1)`, `.easeInOut(...)`,
    `.constant(t0,t1,v)`.
  - Methods `evaluate(t)`, `addKey(t,v)|addKey(Keyframe)` (returns the index), `removeKey(i)`,
    `moveKey(i,key)`, `getKey(i)`, `length`, `smoothTangents(i,w)`, and properties
    `preWrapMode`/`postWrapMode`.
  - `WrapMode`, `TangentMode` and `WeightedMode` tables. They use S21's enum values and also
    accept `WrapMode.Clamp` as an alias of `Constant`.
  - `Gradient` with `evaluate(t) → Vector4`, `setKeys(colors, alphas)` and `mode`.
  - `MinMaxCurve` with `evaluate(t[, r])` and `mode`.
  - `TweenComponent.curve`.
  - **Lua key indices are 1-based.** C++ and MCP indices are 0-based.
  
  S21 binds no curve API itself (S21 D16 covers Animator only), so S27 owns this binding. It is
  documented in the S23 API JSON.
- **R44** CLI: there is no separate subcommand. Scripts and CI record through S31:
  `jasperengine-editor call --project P --wait-job record_game --frames 90 --format mp4 --output out.mp4`.
  S31's exit codes apply. `docs/editor/recorder.md` shows this form.

### Data & compatibility

- **R45** Field codecs:
  - **One codec.** Component fields use the same curve encoding as S21's `.anim`
    (`editor/animation/AnimYaml`). S27 moves the curve and gradient part of that encoding into
    `editor/stream/CurveCodec.{h,cpp}`, which serves YAML, MCP JSON and the Factory C++ literal.
  - **Curves.** `{preWrap, postWrap, keys: [{time, value, inTangent, outTangent, inWeight?,
    outWeight?, mode?}]}`, where `mode` is a token string such as `"clampedAuto"` or
    `"in:free,out:constant,broken,weighted:both"`. Defaults are omitted.
  - **Gradients.** `{mode, colors: [[t, r,g,b]], alphas: [[t, a]]}`, in linear space.
  - **Min-max types.** `{mode, …}` with only the members their mode uses.
  - `Constant` tangents are written as 0, never ∞. Malformed values keep the default and add a
    skip record (S08 R7 / S03).
  - **S08.** Registration is `Kind::Custom` with `TypeOps` (`curveOps`, `minMaxCurveOps`,
    `gradientOps`, `minMaxGradientOps`) and named drawers `curve`/`minMaxCurve`/`gradient`/
    `minMaxGradient`. Before S08, `RowPropertyType::Curve/MinMaxCurve/Gradient` are appended to
    `InspectorWindow.h:30-61`.
- **R46** New fields only, so there is no scene version bump. Old scenes load with
  `TweenComponent::curve` = linear. `EaseType::CURVE` is appended, so existing numeric values do
  not change. A `.colors` or `.curves` file with an unknown `version` is read-only and shows a
  warning. `settings.yaml` with the old recent list migrates (R21).

## 6. Design constraints & decisions

**6.1 Widget API** (`editor/widget/ColorField.h`, `CurveField.h`, `GradientField.h`):
```cpp
namespace jasperengine::editor::ui {
enum class EditPhase : uint8_t { None, Begin, Changing, Committed, Cancelled };
struct ColorFieldOptions { bool alpha = true; bool hdr = false; bool linearStorage = true;
                           bool eyedropper = true; const char* label = nullptr; bool mixed = false; };
EditPhase colorField(const char* id, Vector4& rgba, const ColorFieldOptions& = {});   // + Vector3 overload
struct CurveRange { float t0 = 0, v0 = 0, t1 = 1, v1 = 1; bool clamp = false; };
struct CurveFieldOptions { CurveRange range; ImU32 color = 0; float height = 0; const char* label = nullptr; bool mixed = false; };
EditPhase curveField(const char* id, Curve& curve, const CurveFieldOptions& = {});
EditPhase minMaxCurveField(const char* id, MinMaxCurve& curve, const CurveFieldOptions& = {});
EditPhase gradientField(const char* id, Gradient& gradient, bool hdr = false, bool mixed = false);
EditPhase minMaxGradientField(const char* id, MinMaxGradient& gradient, bool hdr = false, bool mixed = false);
// Curve window: hosts S21 editor::CurveEditor; S27 adds CurveEditor::setRange(const CurveRange*),
// CurveBinding::partner (min-max band) and CurveEditor::drawPresetBar(...)
}
```
The fields edit a copy of the value and report phases. The inspector glue maps the phases:
- `Begin` → `CommandHistory::beginInteractive("Change <Label>")` (S24 R10);
- `Changing` → merging `PropertyCmd`s for each selected entity;
- `Committed` → `commit()`;
- `Cancelled` → `cancel()`.

The widgets never touch `CommandHandle` themselves. S20, S21 and the MCP glue use the same
mapping.

**6.2 Types.** S27 adds no type; the types are S21 D15's `Curve` and S20 D5's `Gradient`, plus
`MinMaxCurve`/`MinMaxGradient`, all in `engine/core/math/Curve.h`. It adds only the R7 helpers
and `using AnimationCurve = Curve;`. `MinMaxCurve` must have
`{mode, constantMin, constantMax, multiplier, curveMin, curveMax}`. `MinMaxGradient` must have
`{mode, colorMin, colorMax, gradientMin, gradientMax}` (linear `Vector4` colours).

**6.3 Picker architecture.**
- `ColorPickerWindow` is a singleton ImGui window. It is a normal floating window and becomes an
  OS window when Detachable Windows is on.
- `ScreenColor` has one interface: `beginSampling()`, `sample9x9(screenPos, out[81])`,
  `endSampling()`. It is implemented in `ScreenColorWin.cpp`, `ScreenColorX11.cpp` and
  `ScreenColorMac.mm`. The macOS implementation is a one-shot async `NSColorSampler` call.
- Sampling mode captures the mouse through `Backend` so that clicks do not reach other widgets.
- Viewport hits are resolved from the Scene/Game window rects before the screen is read.

**6.4 Hub architecture.**
- `editor/hub/ProjectHubWindow`, `TemplateCatalog` (embedded + user), `ProjectImporter` (zip/git
  job with cancel) and `HubPaths`.
- `App` gains `enum class StartupState { Hub, Loading, Ready }`. The backends' call to
  `loadStartupProject()` is unchanged; inside it the hub decision happens first.
- Hub actions go through `requestProjectChange`, which today's menu commands already use.

**6.5 File formats** (JSON, UTF-8, written with S03 `writeFileAtomic`):
- `*.colors`: `{"version":1,"name":"Default","swatches":[{"name":"Sky","linear":[r,g,b,a],"hdr":false}],
  "gradients":[{"name":…, "gradient":{…}}]}`. At most 76 swatches (4 rows × 19).
- `*.curves`: `{"version":1,"name":…,"presets":[{"name":…,"curve":{…normalised [0,1]²…}}]}`.
- `template.json`: `{"schemaVersion":1,"id":"3d","name":"3D","description":…,"category":"3D",
  "order":1,"accentColor":"#5A67A7","thumbnail":"thumbnail.png","startScene":"Main"}`.
- Curve/gradient JSON for MCP is identical to the YAML mapping in R45.

**6.6 Recorder architecture.**
- `editor/recorder/RecorderSettings` (yaml), `RecorderSession` (state machine: Idle → Arming →
  Recording → Finalising → Idle), `FrameSink` (`PngSequenceSink`, `FfmpegMovieSink`) and
  `OfflineAudioCapture`.
- The session hooks `Engine::onPostUpdate` (as `PlayTools.cpp:290` does) to count game frames. It
  reads back after the Game view's render in `App`'s frame.
- The Recorder window and MCP `record_game` (and so S31's `call`) drive the same `RecorderSession`.
- MCP `play` `fixed_fps` conflicts with a recording's fps. The recorder wins, and MCP reports the
  value it overrode.

## 7. Acceptance criteria

| # | Criterion | Verification |
|---|---|---|
| AC1 | `constant`/`linear`/`easeInOut` build the documented keys; `smoothTangents` gives the Unity result on a 3-key fixture | gtest `CurveHelpers.Constructors`, `CurveHelpers.SmoothTangents` (`engine/tests/core/math/`) |
| AC2 | If S27 lands before S21 (R6 fallback), S21's D15 evaluation tests are added and pass. Otherwise this row is satisfied by S21's suite unchanged | S21 `Curve.*` gtests |
| AC3 | `fromEase` of every non-`CUSTOM` EaseType is within 0.01 of `Ease` at 101 points | gtest `CurveHelpers.FromEase` |
| AC4 | All four MinMaxCurve and MinMaxGradient modes evaluate correctly for random 0/0.5/1, with `multiplier`; `bake` equals `evaluate` at LUT points | gtest `MinMaxCurve.*`, `MinMaxGradient.*`, `CurveHelpers.Bake` |
| AC5 | bake-64 ≤ 3 µs; 20 visible CurveFields ≤ 0.2 ms per frame | gtest `CurveHelpers.BakePerf` (Release CI threshold) + `editor/tests/widget/CurveFieldPerf` (offscreen ImGui context, 1,000 frames) |
| AC6 | YAML → load → save is byte-stable for curves/gradients; defaults omitted; old scene with TweenComponent loads with linear curve | gtest `editor/tests/stream/CurveCodec.*` + fixture `tweens_v_old.scene` |
| AC7 | A tween with `EaseType::CURVE` produces the same positions in Play and in an exported Linux build and a Web build | MCP `test_curves.py::test_tween_curve_play` + smoke-test fixture scene `tween_curve` (`tools/smoke-test.sh`) logging 5 sampled positions |
| AC8 | Lua: `AnimationCurve.linear(0,0,1,2):evaluate(0.5) == 1`, 1-based keys, Gradient evaluate, WrapMode tables | MCP `eval_lua` cases in `test_curves.py::test_lua_api`; stubs regenerated (`generate_lua_stubs.py`) |
| AC9 | `set_properties` accepts hex, srgb/linear objects and HDR intensity; `#FF8000` on a linear field stores (1, 0.2159, 0) ± 1e-4; `get_entity` round-trips curve JSON | MCP `test_color_tools.py::test_set_color_forms`, `test_curves.py::test_set_curve` |
| AC10 | One Color-window session = one undo step "Change Color"; Esc restores and adds none; multi-select writes all in one step; same for a Curve-window drag | gtest `editor/tests/command/FieldEditPhases` (drives Begin/Changing/Committed/Cancelled through the inspector glue and checks `CommandHistory`) + manual: drag in picker, Ctrl+Z once restores |
| AC11 | HDR decomposition: (4,2,0) opens as base (1,0.5,0) intensity 2; re-commit without change is bit-identical | gtest `ColorMath.HdrDecompose` |
| AC12 | `sample_view_color` on a Game view showing an unlit (1,0,0) plane returns `#FF0000`; the eyedropper's viewport path reads the same texel | MCP `test_color_tools.py::test_sample_view_color` (GL and Vulkan images) |
| AC13 | Screen eyedropper on Windows and X11 picks a known pixel of the editor UI (Theme colour) | manual, exact: open Color window, press I, click the Hierarchy background; hex equals `Theme` `WindowBg` sRGB hex. macOS: same via NSColorSampler |
| AC14 | Swatch libraries: add/remove/create persist in `<settings dir>/swatches` and `<project>/EditorSettings/Swatches`; unknown version read-only | MCP `test_color_tools.py::test_edit_swatches` + gtest `SwatchLibrary.Io` |
| AC15 | R10 additions: double-click adds a key on the curve, Delete removes, the key menu sets the R8 modes, range clamp holds, min-max band drawn, presets apply remapped to the range, one undo per gesture | gtest `editor/tests/widget/CurveEditorOps.*` on `CurveEditor`'s model layer (hit-test, add/move/delete/tangent ops, clamp, preset remap) + manual checklist in `docs/editor/curves.md` |
| AC16 | Shift at launch, `--hub`, "Always" pref and missing last project each show the hub; `--project`/`mcp`/`benchmark` never do | MCP harness launch tests `test_hub.py::test_hub_conditions` (`--hub`, missing path, pref) + manual Shift check on each OS |
| AC17 | Creating from each built-in template yields a project that loads with zero errors, opens `Main`, builds (Linux) and runs its script | MCP `test_hub.py::test_create_from_templates` (4 templates; `create_project template=…`, `read_console`, `play`, `eval_lua`) + gtest fixture load of each template scene (S02 scene fixture suite) |
| AC18 | Zip import accepts a nested project zip; rejects `../evil`, absolute, symlink and oversize archives leaving nothing on disk | gtest `editor/tests/hub/ZipImport.*` (crafted archives) + MCP `test_hub.py::test_import_zip` |
| AC19 | Git import of a local bare repo served via `file://` is refused; `https://`/`ssh` arguments are passed as argv (no shell); a `-oProxyCommand` URL is refused | gtest `GitImport.UrlValidation`, `ProcessExec.ArgumentsReachChildVerbatim` (S29 AC1), `ProcessExec.StreamStdin` |
| AC20 | Recent list migrates from the old string list; cap 30; pinned survive | gtest `editor/tests/settings/RecentProjects.*` |
| AC21 | `record_game frames=90 fps=30 format=png 640x360` writes exactly 90 PNGs of 640×360, sidecar says 90, frame k corresponds to game time k/30 (scene script logs `Engine.getGameFrames`) | MCP `test_recorder.py::test_png_sequence` |
| AC22 | `format=mp4` with FFmpeg present: `ffprobe` reports h264, 90 frames, 30 fps, 640×360, BT.709; with `audio=true` an AAC stream of 3.0 s ± 1 audio frame; without FFmpeg the tool fails with the Preferences hint | MCP `test_recorder.py::test_mp4`, `test_mp4_audio`, `test_no_ffmpeg` (`JASPER_FFMPEG=/nonexistent`) |
| AC23 | Two recordings of the same scene with fixed fps produce identical PNG bytes (deterministic) | MCP `test_recorder.py::test_deterministic` |
| AC24 | Frame Interval [30,59] records 30 frames named frame_0030…0059; Exit Play Mode stops play | MCP `test_recorder.py::test_frame_interval` |
| AC25 | Throughput ≥ 10 fps PNG / ≥ 20 fps H.264 at 1080p30; queue never exceeds 8; no dropped frames | MCP `test_recorder.py::test_throughput` (asserts sidecar `wallSeconds`) |
| AC26 | `jasperengine-editor call --project /fixture --wait-job record_game --frames 30 --format png` exits 0 and writes 30 files; the `editor_tools` group is hidden by default and listed after `tool_groups` enables it | `tools/smoke-test.sh` new step `record` + MCP `test_recorder.py::test_tool_group` (S31 AC10 pattern) |
| AC27 | Exports and asset scans skip `Recordings/` and `EditorSettings/`; generated `.gitignore` lists `Recordings/` | gtest `Project.NonAssetTopLevelDirs` + `test_build.py` export contents check |
| AC28 | All nine former `ColorEdit` sites use `colorField` | CI grep check in `tools/mcp/tests/test_checks.py`: no `ImGui::ColorEdit` outside `editor/widget/` |

## 8. Dependencies & conflicts

- **Depends on:**
  - S01 (C++20) and S02 (gtest targets `engine/tests/core`, `editor/tests`).
  - S03 (`writeFileAtomic` for swatch, preset, template and recorder files).
  - **S21** (hard): `Curve.h` (D15) and `editor/widget/CurveEditor`. If S27 lands first, use the
    R6 fallback.
  - **S20** (hard for the gradient types): `Gradient`, `MinMaxCurve` and `MinMaxGradient`. If
    S20 is late, S27 adds them with S20's D5/R2 semantics.
  - S29 (soft): `ProcessExec`; if S29 is late, use the R33 fallback.
  - S31 (soft): the tool groups and `call`. Until S31 lands, tools are registered in `core` and
    AC26's CLI half waits for it.
  - S08 (soft): `Hdr`/`ColorLinear` flags, `Kind::Custom` TypeOps, named drawers. Without S08 the
    legacy `RowPropertyType` path from R45 is used.
  - S24 (soft): `InteractiveEdit`; if S24 is late, use the R3 fallback.
  - S25 (soft): F10 and Shift-at-start are listed in the shortcut catalog, and the command palette
    gets Start Recording, Project Hub and Color Picker.
  - S06 (soft): PNG encode jobs.
  - S04 (soft): template `.meta` handling.
- **Feeds:**
  - S20: fields replace its interim previews; it gets `fromEase`.
  - S21: the R10 `CurveEditor` additions and the presets.
  - S11: the swatch widget for Kelvin presets (optional).
  - S13: the recorder's fixed delta gives deterministic grain and VHS.
  - S29: `ProcessExec::Stream`, and the Hub sets `editor.vcs.lfs`.
  - S25: commands Start Recording (F10), Project Hub, Color Picker and Save Project as Template
    are registered in its `CommandCatalog`.
- **Naming conflict:** S01's "exported project templates" are the CMake/player scaffolding in
  `engine/project`. S27's "templates" are starter projects. The code says `ProjectTemplate` only
  in `editor/hub`.
- **Merge hotspots:**
  - `editor/window/InspectorWindow.cpp` (S08, S20, S21, S24 and most streams).
  - `editor/widget/CurveEditor.{h,cpp}` (S21 owns it).
  - `engine/core/math/Curve.h` (S20, S21).
  - `editor/vcs/ProcessExec.{h,cpp}` (S29).
  - `editor/App.cpp` (menus, start-up: S25, S28, S30, S31).
  - `editor/AppSettings.{h,cpp}` (S25).
  - `editor/window/GameWindow.cpp` (S15, S26).
  - `editor/mcp/McpValues.cpp` (S08).
  - `editor/Exporter.cpp` exclusion sites (S30).
  - `CMakeLists.txt` (template embedding, miniz: S02).
  - `engine/core/subsystem/AudioSystem.cpp` (S22).
  - `engine/core/script/binding/MathClassesLua.cpp` (S18, S23).

## 9. Risks & defaults

- **Wayland and macOS screen sampling.** X11 `XGetImage` under XWayland cannot see Wayland
  surfaces, and macOS gives no live preview. *Default:* use the viewport path everywhere and the OS
  path where it works, with a clear message. The xdg portal `PickColor` is a follow-up.
- **Display colour management.** OS-sampled values on wide-gamut or HDR displays are mapped by the
  OS. *Default:* convert to sRGB where the API allows (macOS). Elsewhere use the raw 8-bit values.
  Viewport sampling is exact.
- **Muted audio while capturing (R29).** This is a UX trade-off made for frame-exact sync.
  *Default:* accept it and show a banner. Live monitoring may follow later.
- **User FFmpeg variance.** Builds may lack libx264 or prores_ks. *Default:* fall back through the
  encoder list, show the encoder used in the sidecar, and keep the PNG sequence always available.
- **Synchronous read-back stalls** (`glFinish`/`vkQueueWaitIdle`). These are acceptable because
  recording is offline at a fixed timestep. An async PBO/staging ring is a later optimisation.
- **Hub-without-project state.** Code that assumes a loaded project must not run while the hub is
  shown. *Default:* `App::show` returns early, as during loading. MCP calls during the hub answer
  "no project open: use create_project/open_project". Tests cover the MCP path.
- **Template upkeep.** Templates must be re-saved when the scene format changes. *Default:* the S02
  scene fixture suite loads every template, and S03 version bumps must migrate them in-tree.
- **Ordering with S20/S21.** Both are wave 2, and S27 is wave 3. If one of them is late, S27
  implements that spec's type contract verbatim with that spec's tests (R6 fallback), and must not
  invent a variant. *Default:* the integrator rejects a second curve or gradient type.
- **Packed `mode` token grammar (R45).** S21 has not fixed a text form. *Default:* the grammar in
  R45. If S21's AnimYaml already chose another, that one wins and R45 follows it.

## 10. Deviations

1. **D1 No weighted keys.** S20's `Curve` (the shared type, §0) is unweighted Hermite, and no
   `.particles` file or scene carries weights. Weighted Bézier keys would add a second evaluation
   path to the particle LUT baker for a feature no consumer has. `inWeight`/`outWeight`, Lua
   `weightedMode`/`WeightedMode` and the key menu's "Weighted" items are not implemented.
2. **D2 Gradient keys are sRGB, LDR.** S20's `Gradient` stores sRGB-encoded 0–1 colour keys (its
   `linear` flag picks the blend space). The gradient JSON stays S20's (sRGB), not R45's linear
   form, and R12's per-key HDR Intensity is dropped: the type cannot hold it.
3. **D3 `MinMaxCurve`/`MinMaxGradient` stay in `particles/ParticleValues.h`.** They carry the
   particle drivers (`driver`, `driverMin/Max`, `wrap`) and live in namespace `particles`; moving
   them would change their API. Their fields are S20's (`constant`, `constantMax`, `curve`,
   `curveMax`, `multiplier`; `color`, `colorMax`, `gradient`, `gradientMax`), not §6.2's names.
4. **D4 One codec is S20's JSON, extended.** Curves are `[[t, v, in, out(, mode)]]` with `null`
   for an infinite (Constant / step) tangent, as `.particles` files already write; a curve with
   a non-Clamp wrap is `{keys, preWrapMode, postWrapMode}`. The YAML form is the same structure.
5. **D5 Tangent mode tokens** follow S21's `ClipTangentMode` names where they overlap: a key's
   mode token is `clampedAuto`, `auto`, `free`, `linear` or `constant` when both sides agree and
   it is not broken, else `in:<mode>,out:<mode>[,broken]`; Free/Free unbroken is omitted.
6. **D6 The Animation window keeps its own curves view** (§0): R10's "so that the Animation
   window gains them" and R14's third bullet are out of scope.
7. **D7 `fromEase` adds keys where 16 are not enough.** AC3 asks every named ease within 0.01;
   with cubic Hermite segments Elastic In Out needs about 24 keys (its central oscillations), so
   `fromEase(type, keys = 16)` starts from `keys` and adds keys, up to 64, while the curve is
   further than 0.005 from the ease, each new key placed by a local search around the worst point
   (keys land on the bounces' kinks, whose parabolic arcs then are exact). `fromFunction(f, 16)`
   without a tolerance is S20's legacy sampler exactly (same keys, so converted effects and their
   goldens are unchanged).
8. **D8 `create_project` without `template` keeps today's project.** R42 makes `empty` the
   default. MCP tests and agents rely on `create_project` giving one untitled 3D scene and no
   files; a template is asked for by name (`template: "empty"`). The Project Hub always uses a
   template.
9. **D9 The 3D template keeps a new scene's post-processing.** There is no "Default"
   post-processing preset (S13's presets are None, Mobile, Mobile Adaptive, Console, Stylised,
   CRT Retro, Film and VHS Tape); the template's `Main` has the settings File > New Scene gives a
   3D scene. The 2D template's orthographic camera is the 2D scene's own default camera.
   The UI template is the HTML UI's `main_menu` example (`create_ui_document`): `ui/main_menu.html`
   and `.css` with Play, Settings, Credits and Quit, wired in its `MainMenuUI.lua` (the project's Lua
   folder), not a separate `scripts/MainMenu.lua`.
10. **D10 sRGB conversions use the engine's transfer.** The colour forms (R41), the Color window
    and `sample_view_color` convert with `Color::sRGBToLinear` / `linearTosRGB` (the 2.2 power the
    renderer and the old colour edits use), so a value set by hex equals the same value picked in
    the window. AC9's `#FF8000` on a linear field stores (1, 0.2195, 0), not the piecewise
    sRGB curve's 0.2159.

## 11. Progress

- [x] Re-check (§0) and deviations, committed before implementation.
- [x] Runtime types (R6-R8, R13): tangent and wrap modes, `fromEase`/`fromFunction`, helpers,
      allocation-free `Gradient::evaluate`, `EaseType::CURVE` and `TweenComponent::curve`; the
      one codec (`CurveJson`, `CurveCodec`); S08 TypeOps; Lua bindings (R43).
- [x] Widgets (R1-R5, R9-R12): Color window (eyedropper on X11/Win32/macOS, swatches), Curve and
      Gradient windows, the field edit phases and their undo sessions; the nine colour sites.
- [x] Project Hub (R15-R23): pages, recents, templates (built-in embedded, user, save as
      template), zip and Git import; start-up rules.
- [x] Recorder (R24-R31): window, session, PNG and FFmpeg sinks, offline audio, sidecar,
      `ProcessStream`; non-asset folders.
- [x] MCP (R41, R42), commands (S25), docs (R40), NOTICE.
- [x] Builds on the merged commit (oe-port e3f0c274d): linux GL and Vulkan with warnings as
      errors, web (and the `tween-curve` web gate: PASS), Android (and its `tween-curve` project).
- [x] Built-in templates and the tween-curve fixture made by the editor (`tools/templates/
      make_project_templates.py`, `tools/fixtures/make_tween_curve_fixture.py`).
- [x] Unit tests: core 2366/2366, editor 1427/1427; ASan+UBSan on S27's areas (editor 149, core
      30) and, before the last merge, the whole suites (core 2347, editor 1418 but one basisu
      Debug assertion outside S27). Smoke: fixture-unity and tools/fixtures/tween-curve PASS
      (the exported game logs the curve checks; `record` writes 30 frames). Xcode checks clean.
      TOOLS.md regenerated. The full MCP suite (MCP_SUITE=all, slow builds) on the merged commit.
- Unverified here: Windows and macOS (the screen eyedropper's BitBlt / NSColorSampler, the
  Shift-at-launch readers, D3D11 and Metal rect readback compile paths), manual checks of
  docs/editor/color-picker.md and curves.md, R36-R38 budgets on the benchmark server.
