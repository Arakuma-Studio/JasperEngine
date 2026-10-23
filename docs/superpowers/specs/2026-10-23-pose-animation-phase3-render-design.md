# Pose Animation, phase 3: offscreen pose rendering, contact sheets, `render_pose_animation`

Status: approved design, ready for implementation.
Branch: `pose-animation`. Builds on phases 1-2 (read phase 1 sections 3-4 and phase 2 sections
2, 3 and 5).

## 1. Goal

Agents and the editor UI need pictures of poses that do not depend on a visible window, on the
Scene view camera or on the scene's main camera. Today `screenshot` reads a window framebuffer
(`editor/mcp/tools/ViewTools.cpp`, `w3::captureView`). This phase adds:

1. `PoseRender` (`editor/render/preview/PoseRender.{h,cpp}`): a private scene (camera, key and
   fill lights, a floor, a model instance) rendering any `HumanPose` of a model file from any
   camera to an offscreen texture, read back to RGBA.
2. `PoseImage` (`editor/pose/PoseImage.{h,cpp}`): CPU image composition: cells, contact sheets,
   anti-aliased lines, filled circles, a built-in bitmap font for labels, PNG encoding.
3. MCP `render_pose_animation`, returning the image, a caption and the phase 2 metrics.
4. Thumbnails for the phase 4 window (same renderer, small cells).

## 2. PoseRender (contract)

```cpp
struct PoseView { enum Kind { Front, Side, ThreeQuarter, Back, Top, Custom } kind = Side;
                  float yaw = 0, pitch = 0; /* degrees, Custom only; yaw 0 = looking at the character's front */ };
struct PoseRenderStyle { bool skeletonOverlay = true; bool floorLine = true; bool shaded = true;
                         Vector4 background{0.16f, 0.16f, 0.17f, 1}; };
class PoseRender {
public:
    bool setModel(const std::string& modelPath, std::string* error);       // loads off-scene, builds the rig (phase 2 provider)
    const std::shared_ptr<pose::PoseRig>& rig() const;
    // frames all poses of a batch with one shared framing (union bounds), so cells have the same scale
    void beginBatch(const std::vector<pose::HumanPose>& posesForFraming, const PoseView& view);
    bool renderCell(const pose::HumanPose&, const PoseView&, int width, int height, const PoseRenderStyle&, PoseImage& out);
};
```

- Camera: orthographic, looking at the union bounds' centre, framing the union of the posed
  skeleton bounds (skin bounds when a skin exists) of the batch with 8% margin. View directions
  in canonical axes (the direction the camera looks): Front `-F` (sees the face), Side `+L`
  (camera on the character's right side; the character faces image-right), ThreeQuarter = Front
  rotated -40 degrees about U (camera toward the character's front-right), Back `+F`, Top `-U`
  with screen-up = `+F`. Screen-up is `+U` for every other view.
- The model is drawn with its own materials when `shaded`, under a fixed key light (from
  front-left-above) and fill light; the floor is a dark grid quad at the rig's floor height.
- The skeleton overlay is drawn on the CPU after readback by projecting joint positions with the
  same camera: left limbs coral `#FF7A45`, right limbs cyan `#36B3FF`, centre white `#E8E8E8`,
  2 px lines, joint dots 3 px; IK end effectors (hands, feet) 4 px dots. The floor line is a 2 px
  orange `#FFA726` line across the cell at the floor height (Side/Front/ThreeQuarter/Back).
- Must work in the headless editor (`jasperengine-editor mcp --headless`, Xvfb + Mesa) and in a
  normal editor without disturbing the open scenes, the Scene view or the Game view; rendering is
  scheduled on the main thread through the engine's render-to-texture path (as
  `ModelRender`/`MeshPreviewRender` do) and never blocks the UI thread waiting for frames (use the
  MCP `afterFrames` / `waitUntil` pattern).

## 3. Contact sheets (`PoseImage`)

- Layout helpers: a grid of equal cells with 1 px separators `#000000`, a label strip 14 px high
  under each cell (bitmap font 6x10 ASCII, white on `#202022`), a title strip optional.
- Label text: poses mode `"<index> <name>"` (truncated with `..`), motion mode `"t=<seconds>"`
  with 2 decimals, plus `" C"` markers for feet in contact (`"L"`, `"R"`, `"LR"`).
- PNG encoding through the engine's stb_image_write.

## 4. `render_pose_animation` (MCP, read-only)

Arguments:

| argument | meaning |
|---|---|
| `path` | the `.poseanim` (required) |
| `mode` | `poses` (key poses; default) / `motion` (evenly spaced baked frames) / `times` (specific times) |
| `poses` | indices for `poses` mode (default all, max 12 per row, max 24) |
| `frames` | count for `motion` (4..24, default 12) |
| `times` | seconds for `times` mode (max 24) |
| `views` | array of `front`, `side`, `three_quarter`, `back`, `top` (default: `poses` -> `[front, side]`, others -> `[side]`); each view is one row |
| `model` | scene entity whose model file/rig to use instead of the preview model |
| `cell` | `[width, height]` (default `[160, 200]`, each 64..512); the sheet is capped at 2048 px wide (cells shrink to fit) |
| `overlay` | default true: skeleton L/R colours and labels |
| `save_path` | optional project-relative PNG path to also write |

Result: one PNG image content + text caption describing the layout ("columns: poses 0..5
(names); rows: front, side; coral = left limbs, cyan = right") + structured
`{mode, columns: [{index|time, label}], rows: [views], cell: [w, h], metrics: [per column, the
phase 2 metrics shape]}`. Frames in `motion`/`times` modes come from the same `pose::frames`
pipeline as the bake and the runtime (preview == bake == runtime).

Errors: no preview model and no `model` -> "Set a preview model: edit_pose_animation
set_preview_model, or pass model"; a rig that fails validation names the missing slots and the
`set_humanoid` fix.

Budget: stay within the phase 2 reservation (~2.5K chars of tool definition).

## 5. Tests

- `tools/mcp/tests/test_pose_render.py` (fixture project of phase 2): render `poses` mode with 3
  poses and 2 views -> image size matches the layout, non-flat; left/right colours present in the
  expected halves for an arm-raise pose (pixel checks on the overlay colours); `motion` returns the
  frame count asked; `times` labels; `model` override renders the A-pose robot; `save_path` writes a
  PNG; the Scene view camera and the open scene are unchanged afterwards (`get_scene_tree`,
  `set_scene_camera` read-back); works with `--headless`.
- A golden-free determinism check: rendering the same request twice gives identical PNG bytes.
- Full MCP suite and budget test pass.
