# Editor tools for agents: colours, curves, templates, recording

Served as `jasper://docs/editor-tools`. The user documentation is `docs/editor/color-picker.md`,
`curves.md`, `project-hub.md` and `recorder.md`; the design is
`docs/specs/S27-editor-tools.md`. `record_game`, `sample_view_color`, `edit_swatches`,
`list_curve_presets` and `evaluate_curve` are the `editor_tools` group (`tool_groups` enable
`["editor_tools"]` lists them), with `clear_project_leftovers`; `create_project {template}`,
`list_project_templates` and `import_project` are core.

## Colours

Any colour property (`set_properties`, `create_entity` values, `set_scene_settings`, material
values) takes:

- `[r, g, b(, a)]`: the field's own space, unchanged (most 3D colours are linear; UI and sprite
  colours sRGB: `get_entity` shows what is stored).
- `"#RRGGBB"` / `"#RRGGBBAA"`: sRGB, converted to linear for a linear field.
- `{"srgb": [r, g, b(, a)]}` or `{"linear": [...]}`: say the space; converted to the field's.
- `"intensity": stops` with any of the forms above (`{"hex": "#FF8000", "intensity": 2}`):
  multiplies by 2^stops; HDR fields only (light colours, emission), else an error.

`sample_view_color {view, x, y}` reads one pixel of the Game or Scene view (screenshot's pixel
coordinates, from the top left) as `{hex, srgb, linear}`: what the eyedropper takes. Use it to
check a colour on screen exactly instead of guessing from a screenshot.

`edit_swatches` lists and edits the Color window's swatch libraries: `scope` user (this
computer) or project (`EditorSettings/Swatches/*.colors`, in version control).

## Curves and gradients

A curve value is JSON: `[[time, value, inTangent, outTangent, mode?], ...]`. `null` for a tangent
is a step (Constant). With a wrap: `{"keys": [...], "preWrapMode": "loop", "postWrapMode":
"pingPong"}` (clamp, loop, pingPong, linear, loopWithOffset). `mode` is `clampedAuto`, `auto`,
`free` (the default, omitted), `linear`, `constant`, or `in:<mode>,out:<mode>[,broken]`; Auto
and Clamped Auto keys get their tangents computed, whatever you send. A gradient is
`{"colorKeys": [[t, r, g, b]], "alphaKeys": [[t, a]], "mode": "blend"|"fixed", "linear": bool}`
with sRGB 0-1 colours, at most 8 keys of each. Errors name the path (`keys[2][4]`).

- Tween ease along a curve: `set_properties` `{"tween": {"function": "curve", "curve": [...]}}`
  (the value over the normalized time 0-1; overshoot above 1 is fine).
- Particle modules take the particle effect JSON (`jasper://docs/particles`): min-max values
  `{mode, ...}` with these curves and gradients.
- `evaluate_curve {entity, component, property, times, random}` evaluates the stored value as
  the runtime does (a curve, a min-max curve or gradient at `random`, or a particle field such as
  `sizeOverLifetime.size`): check a curve before playing.
- `list_curve_presets`: the Curve window's presets (over 0-1 x 0-1), to start from.

## Projects from templates

- `list_project_templates`: `empty`, `3d`, `2d`, `ui` and the user's own (File > Save Project as
  Template). `create_project {path, name, template}` copies one, opens it on its start scene and
  returns its scenes. Without `template` the project is empty with one untitled 3D scene, as
  before.
- `import_project {source, path}`: a `.zip` or a Git URL (`https://`, `ssh://`,
  `user@host:path`) into the folder `path`, then opens it. A refused archive (absolute paths,
  `..`, links, too big) or a failed clone leaves nothing behind.
- A stopped editor's `<name>.importing` staging folders go by themselves at the next start, and a
  running editor's are never touched. One with no owner marker may hold a user's files: results list
  it in `leftovers`, and `clear_project_leftovers {remove}` deletes it once the user agrees.
- While the editor shows the Project Hub without a project, project tools answer "No project is
  open"; `create_project`, `open_project`, `import_project` and `clear_project_leftovers` work.

## Recording the game

`record_game` records the Game view as a job: it enters Play when needed, runs the game at
exactly 1/fps per frame (frame k is game time k/fps, the same on every machine) and writes:

- `format: "png"`: a sequence `Recordings/<Scene>_<Take>/frame_0000.png...` (no FFmpeg needed);
- `"mp4"` (H.264) or `"mov"` (ProRes 422 HQ): through the user's FFmpeg (`JASPER_FFMPEG`,
  Preferences > External Tools, or PATH). Without one the call fails saying so: use png.

`frames` or `seconds`, from `start_frame` (0 = Play's first frame); `width`/`height` (default:
the Recorder window's resolution); `audio: true` mixes the game's sound in (from Play's start
only). The result is the sidecar (`frames, fps, width, height, encoder, scene, take,
startFrame, warnings, wallSeconds`) with `files` or `movie`; `<output>.recording.json` holds the
same. `cancel_job` stops early and still finalises. A `play` `fixed_fps` is replaced while
recording and restored after (`overrode_fixed_fps`).

From a shell: `jasperengine-editor call --project P --wait-job record_game --frames 90 --format mp4 --output out.mp4`.

Check a recording by reading back a frame (`view_asset` on a PNG) or the sidecar's frame count;
don't compare movie bytes (encoders differ).
