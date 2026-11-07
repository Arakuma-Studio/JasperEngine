# Profiling and render diagnostics

How to measure a scene with the editor's tools (spec S15). The same data is in **Window >
Analysis > Profiler** and **Window > Analysis > Frame Debugger** for a human.

## The tools

They are the `profiling` tool group: `tool_groups` lists it in the session's tools, and each
tool is callable by name without it.

| Tool | What it answers |
|---|---|
| `get_render_stats` | the last frame of a view: draws, instances, triangles, passes, pipelines and bindings applied, uniform and upload bytes, alive resources, pipeline cache, `cpu_ms`, `fps`, `gpu {available, reason, frame_ms, semantics}` (also `jasper://stats`) |
| `get_frame_times` | the always-on history of 600 frames: median/p95/p99/max of `frame_ms`, `cpu_ms`, `gpu_ms`, `fps`; spikes over `min_frame_ms` with `spike_interval_frames`; the newest frames; monitor series |
| `capture_profile` | records N frames (views redraw meanwhile), then: `capture_id`, summaries, per-category ms, `worst_frame`, `overhead_note`, `path` when `save_path` is given |
| `get_cpu_profile` | one frame of a capture (or of the live recording): the scope tree of a thread, flat rows over every thread, or each thread's busy % |
| `get_gpu_passes` | the ordered passes of a view's frame: targets, load/store actions, draws, triangles, `cpu_ms`, `gpu_ms`; with `capture_id`, per-label GPU ms over the capture |
| `capture_pass_output` | an image of what one pass wrote (colour, alpha, depth, one channel) |
| `get_vram_report` | the estimated bytes of every GPU image and buffer by category or owner, the largest ones, the peak, the driver's own figure where it reports one |

The view (`view`) is omitted for the engine's frame (the running game while playing, else the
Scene view), `"game"` or `"scene"` by name, or a view tag.

## Workflows

**Is it fast enough?** `get_frame_times` (window 300). Compare `frame_ms` p95 with the budget
(16.7 ms at 60 fps). `cpu_ms` is the main thread's work without presenting and waiting; `gpu_ms`
is the game frame's GPU time while GPU timing runs.

**Find the worst frame and why.** `capture_profile frames=120` then `get_cpu_profile
capture_id=<id> frame=worst` (tree), `view=flat` (what costs most over every thread) and
`view=threads` (are the job workers busy?). `filter` narrows rows by name.

**Which pass costs the GPU time?** `get_gpu_passes` arms GPU timing, waits until the times of a
frame arrive (a few frames late) and lists every pass with `gpu_ms`. Over many frames:
`capture_profile frames=300` then `get_gpu_passes capture_id=<id>` gives median/p95/p99/max per
pass label.

**What did a pass draw?** `capture_pass_output pass=<index or label> attachment=color0|depth
mode=color|depth|alpha|r|g|b`. Depth is normalised to the range it holds. Swapchain passes and
MSAA depth without a resolve image reply `preview_unavailable` with the reason.

**Where does the VRAM go?** `get_vram_report` (by category), `group_by=owner` (by asset or
subsystem), `filter=shadow`. Bytes are estimates from sizes, formats, mips, slices and samples
(drivers pad, tile and compress); `driver` is what the driver reports (Vulkan
VK_EXT_memory_budget, D3D11, Metal, GL NVX/ATI meminfo), null where it reports nothing.

## Caveats

- One frame is one sample: compare medians and percentiles, not single frames.
- `pipelines_applied` is Unity's SetPass calls. A draw with its pass's last pipeline applies
  none, and one with the last bindings binds nothing. Far below `draw_calls`: materials and meshes
  are shared. Close to it in a 3D view: too many distinct materials, shaders or meshes.
- Recording costs time: `overhead_note` estimates it (the measured cost of a scope pair times the
  events). The first frame after arming is partial and is not in a capture.
- GPU time semantics differ per backend:
  - `pipeline_point` (Vulkan, D3D11): timestamps at each pass's start and end; the frame time is
    the first pass begin to the last pass end.
  - `elapsed` (OpenGL, OpenGL ES, WebGL2): one elapsed-time query per pass; the frame time is the
    sum of the passes (`frame_is_sum`). WebGL2 needs EXT_disjoint_timer_query_webgl2 (Chrome),
    Firefox and Safari report `unavailable: extension_missing`. A disjoint event drops the frame.
  - `encoder_span` (Metal): stage-boundary samples of each render encoder. Encoders overlap on
    the GPU: pass times are upper bounds, never sum them. The frame time is the command buffer's.
- GPU timing reasons: `backend` (WebGPU), `extension_missing`, `device_unsupported` (no
  timestamp bits, no Vulkan hostQueryReset, no Metal counters: then only the frame time),
  `disabled` (`JASPERENGINE_GPU_TIMER=off`).
- The editor's own ImGui rendering is not sokol's: only its CPU time is measured (`Editor Render`).
- Edit mode draws only when something changes; the profiling tools keep the views drawing while
  they wait.

## From the command line

- The editor (normal, `mcp` or `--mcp-port`) and development builds accept `--profile` (record
  from the start), `--profile-capture=<path>` and `--profile-frames=N` (default 300). The capture
  starts once every scene is loaded, runs with GPU timing, and logs `Profiler capture written:
  <path>`.
- `jasperengine-editor benchmark --project <dir> --scene <scene> --frames 300 --gpu-passes
  --out result.json` adds a `gpu` object to the result: the device, the semantics, the frame's GPU
  ms (median, p95, mean) and per pass label `perFrame`, `medianMs`, `p95Ms`, `meanMs`;
  `--profile-capture <file>` also writes the measured frames as a capture.
- A `.jetrace.json` opens in Perfetto (ui.perfetto.dev) and chrome://tracing as it is, and in the
  Profiler (Load).
