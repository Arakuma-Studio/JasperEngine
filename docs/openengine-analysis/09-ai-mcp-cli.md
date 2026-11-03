# 09 — AI-assisted development: MCP, CLI, in-editor assistant, agent docs

OURS = JasperEngine (`/var/lib/orquester/workspaces/jaspersito/JasperEngine-2`), THEIRS = OpenEngine (`external/OpenEngine`, MIT, Copyright 2026 Gamecrafters Guild Limited and contributors).
I read the code, not just the file names. I ran nothing from THEIRS. Paths are relative to each repo root.

## TL;DR

- **Architecture is inverted.** OURS embeds a full MCP server (JSON-RPC, stdio and Streamable HTTP) inside the editor process (`editor/mcp`, ~37k lines of C++). THEIRS runs an external Node/TypeScript MCP server (`mcp/`, ~5.8k lines). It proxies each tool to a raw ndjson TCP "debug server" in the editor (`Apps/Editor/Source/DebugServer/EditorDebugServer.cpp`, 127.0.0.1:9999, **no auth**). Their method handlers are mostly in the 10.6k-line `DebugHandlers.cpp`.
- **OURS is the better agent API:** wider content coverage (140 tools vs 110), richer protocol support (instructions, resources, prompts, progress, cancel, annotations), atomic batch and undo, jobs, path addressing, deterministic play testing, disk-edit auto-sync, auth, headless Linux, and about 549 tests against the real editor.
- **THEIRS leads in four areas:**
  - (a) A **safety and approval layer** for an agent working in a human's editor: permission modes, per-call Allow/Deny, named undo steps per agent call, "Undo this turn", and a ledger.
  - (b) An **in-editor assistant panel** that drives the user's own `claude` or `codex` CLI with the editor's MCP attached.
  - (c) **One registry feeding both MCP and a shell CLI** (`ge <tool>`), with flags derived from the schema, exit codes, `--dry-run` and `batch`.
  - (d) **Editor-UI automation and profiling/GPU introspection** (UI tree, click/key/drop into editor widgets, CPU/GPU profilers, frame phases, RenderDoc/Nsight/RGP, render-graph resource readback). It also has **contributor-facing AGENTS.md/CLAUDE.md** files, which OURS lacks at repo level.

---

## 1. What THEIRS has

### 1.1 Transport and architecture

```
AI client --stdio MCP--> mcp/src/index.ts --\
terminal  --argv-------> mcp/src/cli.ts   ---+--> mcp/src/tools/* (one registry) --TCP ndjson 127.0.0.1:9999--> Editor DebugServer
```

- **Server shell.** `mcp/src/index.ts` (110 lines) uses `@modelcontextprotocol/sdk` `McpServer` with `StdioServerTransport` only.
  - The output shape is `{data, summary?, image?}`. An image goes out as an MCP image block, placed first.
  - Every answer that reached an editor gets an `[editor host:port]` suffix. The port is taken from the live socket, because several editors share a machine (`editor-call.ts`, `ipc-client.ts`).
  - It registers 3 resources (`gameengine://log|scene|stats`), which are views onto `get_log`, `get_scene_hierarchy` and `get_render_stats`.
  - **No `instructions`, no prompts, no tool annotations, no progress, no cancellation.** A grep of `mcp/src` finds none.
- **Registry.** `mcp/src/registry.ts` defines `ToolDef{name, category, description, schema (zod), ipcMethod?, coversIpcMethods?, actsOnHost?, run}`.
  - `proxyTool()` (`tools/proxy.ts`) forwards the arguments to the method of the same name.
  - Error codes are `EDITOR_UNREACHABLE`, `EDITOR_TIMEOUT`, `TOOL_FAILED` and `BAD_ARGUMENT`.
  - Importing `tools/index.ts` fails if two tools share a name.
  - Helpers: `flexibleObject` (`schema.ts`) accepts JSON-stringified objects that LLMs send. `vec3Tuple()` is a factory so zod does not emit `$ref`s.
- **IPC client** (`mcp/src/ipc-client.ts`, 268 lines):
  - Connects on demand. The default timeout is 30 s (`GE_MCP_IPC_TIMEOUT_MS`).
  - The timeout message mentions that a native modal stalls dispatch.
  - When `GE_ASSISTANT_TOKEN` is set, it sends `assistant_bind {token}` first on every connection, and a bind refusal is sticky.
- **Editor debug server** (`EditorDebugServer.{h,cpp}`, 934 lines):
  - A network thread runs a `select()` loop with up to 64 clients and a loopback-UDP wake socket. Handlers run on the main thread in `FlushPendingRequests()`.
  - Long calls (GPU readback) use `DeferredMarker()` plus `EnqueueDeferredResponse(poll, budget)`. `EnqueueRenderCallback` runs inside the render phase.
  - On Windows the bind is exclusive (`SO_EXCLUSIVEADDRUSE`) so a second editor cannot silently share 9999.
  - Every request passes `Editor::DebugRequestGateRegistry` gates (Before / Handled / After / Update / ClientClosed). A gate can **park** a request, and later requests on that connection queue behind it.
- **Security** (`docs/getting-started/mcp-setup.mdx` §Security): "listens on 127.0.0.1 only and asks for no credentials: any program on this machine can connect … including the ones reachable only through `ge raw`". The assistant gate only constrains *bound* connections; an unbound one is "left alone" (`AssistantGate.h`).
- **Debug server (C++, 133 `RegisterHandler` sites).** 42 of its methods are deliberately raw-only, each with a reason in `mcp/ipc-method-coverage.json`:
  - Raw-only methods: `open_scene`, `spawn_model`, `set_entity_enabled`, `respond_modal`, `open_context_menu`, `get_context_menu`, `invoke_menu_item`, `set_text_field`, `send_text`, `simulate_mouse_drag`, `simulate_double_click_drag`, `set_scroll`, `undock_panel`, `show_universal_search`, `movie_record`, `physics_raycast`, `terrain_sculpt_dab`, `set_terrain_brush`, `get/set_scene_tool`, `set_scene_view_mode`, `get_mesh_registry`, `get_asset_list`, `graph_*`, `debug_crash`, `assistant_bind`, `assistant_authorize`, …
  - `list_debug_methods` (`mcp/src/debug-server-docs.ts`) parses the `//` comments above each C++ `RegisterHandler` as the parameter docs.

### 1.2 THEIR full tool list (110 tools, `mcp/src/tools/*.ts`)

| Module | Tools |
|---|---|
| editor-state (9) | `get_editor_state`, `get_scene_hierarchy`, `get_log` (count/minLevel/filter/offset), `get_build_status`, `get_build_settings`, `trigger_build`, `execute_command` (named/numeric editor command), `set_play_mode` (enter/exit/pause/resume, activateGameView), `focus_view` |
| scene (10) | `create_entity` (+components map), `set_component` (reflected fields, enums by name, AssetRef by GUID/path, read-back-compatible shape, one undo step), `delete_entity` (undoable subtree), `select_entity`, `get_entity_components`, `query_ecs` (hasComponent), `save_scene` (refuses during play / degraded scene unless force), `undo`, `redo`, `get_undo_stack` |
| ground (2) | `sample_ground_height`, `get_placement_ground_gap` |
| markups (8) | `markup_list`, `markup_get`, `markup_create` (volume/region/path), `markup_update`, `markup_comment` (threads, `[[entity:N]]` links), `markup_contains`, `markup_frame`, `markup_set_visible` |
| animation (2) | `spawn_skinned_model`, `get_skeleton_store_state` |
| ui — editor UI (7) | `get_ui_tree` (maxDepth), `get_panel_tree`, `click_element` (by id or coords; reaches game input in play), `perform_drop` (asset drag-drop), `move_pointer` (hover), `send_key` (shortcut → focused element → game), `input_text` |
| assets (4) | `open_asset`, `select_asset`, `generate_folder_thumbnails`, `get_asset_preview` |
| view (5) | `get_camera`, `set_camera`, `move_camera` (animated), `look_at`, `set_gizmos_visibility` |
| capture (5) | `take_screenshot` (window/viewport/panel/element/rect/asset_preview; render-graph frame, refuses OS window capture unless `allowWindowCapture`; summary warns on `pendingMaterialTextureBinds`), `capture_resource` (any render-graph texture → PNG or buffer bytes, mip/layer/range, pre-exposure aware), `trigger_capture` + `get_capture_status` (RenderDoc), `nsight_capture` |
| render-graph (7) | `get_render_graph_overview`, `_passes`, `_resources`, `_pass_detail`, `_dependencies`, `_validation`, `get_pass_descriptors` |
| render-settings (12) | `get_msaa`, `set_msaa`, `set_aa_mode`, `set_taa_render_scale`, `set_dynamic_resolution`, `set_lod`, `set_shadow_quality`, `set_shadow_pcss`, `set_moments_resolution`, `set_msm_blur_mode`, `get_hdr_output`, `set_hdr_output` (each says PERSISTS vs live-only) |
| profiling (18) | `get_render_stats`, `get_validation_stats` (per-VUID), `get_gpu_tooling`, `rgp_capture`, `get_terrain_stats`, `get_ui_profile_history` (120-frame ring), `get_cpu_profiler` (tree+flat), `get_gpu_profiler` (per-pass), `get_job_system`, `get_memory_stats`, `get_ecs_wave_trace`, `get_frame_phases` (1024-frame ring), `get_navigation_stats`, `find_path`, `get_navigation_sources`, `toggle_shadow_debug`, `set_parallax_steps_view`, `toggle_shadow_thumbnails` |
| window (2) | `get_monitors` (DebugMetrics history), `move_window` |
| game-graph (8, host-side file edits) | `list_game_graph_actions`, `get_game_graph`, `add_game_graph_node`, `update_game_graph_node`, `delete_game_graph_node`, `link_game_graph_nodes`, `write_game_graph_cpp`, `write_game_graph_action_code` |
| ecs-authoring (4, C# codegen) | `create_game_system`, `create_entity_system`, `create_component`, `list_ecs_systems` |
| lifecycle (6, host-side) | `build_engine` (cmake, auto-detects preset), `run_tests` (ctest), `launch_editor` (frees *its* port only; `renderdoc=true`), `wait_for_editor`, `editor_full_cycle` (shutdown→build→launch→wait), `shutdown_editor` (graceful IPC, then kills only the holder of its own port) |
| meta (1) | `list_debug_methods` |

Granularity: about one tool per debug-server method. Entities are addressed by numeric id only. `set_component` takes one component per call. There is **no MCP `batch` tool**; batching exists only as `ge batch`.

### 1.3 CLI (`mcp/ge.mjs`, `mcp/src/cli.ts`, `cli/{args,help,output,raw}.ts`)

- `node mcp/ge.mjs <tool> [--flags | '{json}']`.
  - Flags come from the same JSON Schema that MCP publishes. They are kebab-cased (camelCase also accepted).
  - Shapes: vectors as `1,2,3`, objects as `--values K=V` (repeatable), and a single `{…}` is passed through verbatim.
  - Booleans are **tri-state** (omitted means "no change").
- `ge raw <method> '{…}'` reaches any debug method unvalidated. `ge batch` reads ndjson from stdin over one socket; responses are paired by id.
- Global flags: `--port`, `--host`, `--timeout`, `--json`, `--dry-run`, `--quiet`. Exit codes: 0 ok, 1 tool failed, 2 usage, 3 unreachable, 4 timeout.
- `ge.mjs` re-runs `tsc` when `mcp/src` is newer than `dist`.
- A standalone binary is built with Node SEA (`mcp/scripts/build-sea.mjs`, about 90 MB; CMake target `GameEngineCli`).

### 1.4 In-editor AI Assistant (`Packages/ai-assistant`, about 15.2k lines incl. tests)

- **Providers** (`Editor/Providers/`):
  - `ClaudeSessionProvider` (default) spawns the user's `claude -p --output-format stream-json --verbose --include-partial-messages [--resume id] --permission-prompts none --tools "" --strict-mcp-config --system-prompt-snapshot off --mcp-config <file> --allowedTools mcp__editor_assistant__…` with `--model`/`--effort`/`--append-system-prompt` (`ClaudeSessionProvider.cpp:53-90`). Claude Code's built-in tools are turned off, so the agent can act **only** through the editor's MCP.
  - `CodexSessionProvider` (`codex exec … resume`, read-only sandbox). Before attaching tools, the editor probes whether the Codex sandbox can reach the debug port; if it can, Codex stays "Conversation only".
  - `ClaudeApiProvider` (direct HTTPS + SSE, `ANTHROPIC_API_KEY`, `CanActWithTools=false`: chat only).
  - The editor strips `ANTHROPIC_API_KEY`/`OPENAI_API_KEY`/… from the env of spawned CLIs, so turns run on the user's login and not on a key.
  - Interface: `IAgentProvider.h` (`AgentTurnRequest`, `TurnResult` with cost, tokens, round trips and permission denials; `ProviderCapabilities`).
- **Attachment** (`AssistantAttachment.cpp`): writes a per-turn MCP config into `gameengine_mcp/pid-<pid>/assistant-<conv>.json`. It points at the staged Node server with env `GE_EDITOR_DEBUG_PORT`, `GE_MCP_TOOLS` (the allowed subset) and `GE_ASSISTANT_TOKEN`.
- **Gate** (`AssistantGate.{h,cpp}` 761 lines, `AssistantTools.cpp`, `AssistantMode.h`):
  - Every tool has a class: Read, View, UndoableEdit, Input, Gated or Denied.
  - There are 4 modes: **Read only**, **Auto** (default: reads, view changes, undoable edits and input run; gated actions ask), **Ask before every edit**, and **Edit freely**.
  - A call that asks is **parked** in the debug server until the user picks Allow, Allow for this turn, or Deny on the call's row (10-minute limit).
  - Denied in every mode: `undo`/`redo` ("the assistant cannot see what is on top of the shared undo stack"), `focus_view`/`move_window` (they steal the OS foreground), the lifecycle tools, and `list_debug_methods`.
  - Host-side file-writing tools (`actsOnHost`) call `assistant_authorize` first and are confined to the project root.
  - Edits wait while the user has an interactive drag open (`AssistantGate.cpp:627`).
  - Each admitted undoable edit is wrapped in `UndoRedoService::BeginCompound("Assistant: Set DirectionalLight (Sun)")` (`AssistantGate.cpp:277-302`).
  - Every decided call writes one log line: `AI Assistant: conversation N turn T: <tool> <subject> -> ran|failed|refused|declined|cancelled|parked [undo #id]`.
  - `AssistantActionLedger` records each call's state.
  - Drift observed: 7 newer profiling/navigation tools (`get_frame_phases`, `get_memory_stats`, `get_job_system`, `get_ecs_wave_trace`, `get_navigation_*`, `find_path`) are missing from the `kTools` table. `AttachedNames()` derives the assistant's tool set from that table, so the panel's agent does not get those tools.
- **Undo this turn** (`AssistantTurnUndo.{h,cpp}`): it undoes only when the turn's steps are exactly the top of the undo stack, newest first. Otherwise the button is disabled, and its tooltip names the step on top.
- **UI** (`PromptPanel.cpp` 1395 lines, `AgentCallRow*`, `AgentConversationRow*`):
  - Replies stream as Markdown, prompts queue, and Stop/Esc cancels.
  - Call rows show a mark, the action, the subject and the resulting undo step. Screenshots appear inline; clicking one opens it in Asset View.
  - A footer shows cost and token counts.
  - Also: a model/effort popover, a session picker that resumes the CLI's own sessions (`CliSessionFiles.cpp`), a login-status row, a push-to-talk mic (`PromptMicButton`, `Platform/SpeechInput.h`), and Ctrl+Enter to reach the Allow button.
- **Context gathering** (`MarkupContext.cpp`): `@markup:<tag>` tokens attach structured blocks (shape, area, members, thread), capped at 8 KB per block and 32 KB per turn. "Ask the AI Assistant" is also offered from the Mark-ups panel.
- The system prompt is tiny: one line plus a mode line (`AgentConversationController.cpp:27-50`).
- **Tests** (`Packages/ai-assistant/Tests/`, about 6k lines of gtest): `AssistantGateTests` (652), `AiAssistantPanelTests` (2234), `CliSessionProviderTests` (with `FakeAgentCli.cpp` and recorded `claude-*.stdout`/`codex-*.stdout` fixtures), `SseParserTests`, `MarkupContextTests`, and others.

### 1.5 Automation, headless and tests for the MCP layer

- `Apps/Editor/Source/Automation/UiReplayRunner.{h,cpp}` (3.5k lines): scripted, deterministic UI input replay. It logs per-frame telemetry as JSONL and exits after N frames.
- `EditorSmokeHooks`: environment-driven startup hooks for headless gates (for example `GE_SMOKE_POLYHAVEN`).
- `Tests/Mcp/` (ctest, no editor needed):
  - `ipc-method-drift.mjs` (`EditorIpcMethodDrift`) fails if a handler is neither a tool nor listed raw-only, and on stale or duplicate entries.
  - `mcp-editor-endpoint-check.mjs` checks that every answer names its editor.
  - `rgp-capture-check.mjs` and `mcp-assistant-check.mjs`.
- `Tests/EditorHarness/*.mjs`: run by hand against a live editor. They capture before/after screenshots, compare them with a pure-JS PNG diff (`lib/pngdiff.mjs`), and assert only on the effect. Not in CI (they need a real Vulkan device).
- Headless operation is not documented (Windows-first).

### 1.6 Agent docs

- **`AGENTS.md` (58 lines)** is a map for contributors' coding agents:
  - Build presets, the DebugFast/Release rules and one worktree per change.
  - Focused test suites, with "declared = ran + disabled" reporting and the requirement to be red without the change.
  - Evidence: recorded-pose screenshots from an isolated editor on a verified-free port.
  - **"never synthesise OS input … drive the editor through its debug port"**, and never kill an editor you did not launch.
  - Landing rules: no attribution trailers, no naming other engines.
- **`CLAUDE.md` (85 lines)**: `@AGENTS.md`, engineering values, debugging instruments (cdb, ASan/TSan, RenderDoc, and the MCP tools / `ge` CLI), architecture in one screen, code style, working discipline (tier every claim as verified/inferred/assumed), and reporting headings.
- It refers to `.claude/rules/{rendering,ecs,editor-ui,scripting,documentation}.md` (path-scoped) and `.claude/skills/coordinator`, plus a private "workbench" repo.
- `WHY-OPEN-ENGINE.md` §AI: "an assistant can reach everything a person can" and "bring your own" model, with no engine token and no middleman.
- `docs/getting-started/mcp-setup.mdx`: setup, per-OS `node` resolution, multi-editor targeting, CLI and security. `docs/editor/ai-assistant.mdx`: the panel.

---

## 2. What OURS has

### 2.1 Transport and architecture (`docs/mcp/DESIGN.md`, `editor/mcp/`)

- **In-process MCP server** (`McpServer.cpp` 985 lines).
  - JSON-RPC 2.0, protocol versions 2025-11-25, 06-18, 03-26 and 2024-11-05.
  - Supports `tools`, `resources` (with templates), `prompts`, `logging`, `notifications/cancelled` and `notifications/progress`.
  - `initialize.instructions` carries the agent guide: `McpInstructions.cpp`, capped at 16 KB, with a test that every tool and parameter name it mentions exists.
- **stdio** (`McpTransportStdio.cpp`, `McpLaunch.cpp`): `jasperengine-editor mcp --project|--create [--headless] [--http-port]`.
  - stdout is duplicated for the protocol, and every other stdout write is redirected to stderr.
  - `--headless` spawns `Xvfb -displayfd` on Linux.
- **Streamable HTTP** (`McpTransportHttp.cpp` 634 lines, hand-written): 127.0.0.1 only, Origin check against DNS rebinding, optional bearer token (`McpTransportHttp.cpp:447,534`), `Mcp-Session-Id`, and SSE when progress is requested.
- **Handlers run on the main thread** through `McpCall` (`ok/fail/reply`, `waitUntil`, `afterFrames`, `progress`, `cancelled`).
  - Long operations become jobs (`McpJobs`, `get_job`/`cancel_job`).
  - A busy policy is set per tool (`ToolActivity` in `McpRegistry.h:30-39`): Read, Control, Edit, PlayStart, Build, ScriptCompile, ProjectSwitch.
  - `allowedWhilePlaying` is set on 34 tools.
  - Dialog policy (`McpServer.cpp:838-860`): edits are refused while a modal is open, `respond_dialog` answers it, stale alerts are auto-dismissed, and alerts raised by a tool are folded into its result.
- **Registry** (`McpRegistry.h:41-51`): `ToolDef{name, title, description, inputSchema, outputSchema, annotations{readOnly, destructive, idempotent, openWorld}, allowedWhilePlaying, activity, handler}`.
- **Addressing** (`McpRefs`):
  - Scenes by id, name or path; GameObjects by id or `Player/Gun`/`Enemy#2` paths.
  - Components by catalog name and properties by property path.
  - Values cover every `PropertyType` (`McpValues.cpp` 2.7k lines) and are read back after a write. Unknown arguments are rejected with the list of valid ones.
- **McpAutoSync** (890 lines): before every call it runs a stat scan of the project, reloads changed Lua, C++, shaders, materials, HTML UI and assets, and returns `external_changes{changed, problems[{path,line,message}], ui_reloaded, deferred}`.
- **McpActivityWindow**: server status, the HTTP enable switch and port, connected clients, and a log of recent calls with result and duration. There are no approval controls (`McpActivityWindow.cpp:95-138`).
- The old in-editor AI chat (`editor/ai/*`, `AiChatWindow` 2.86k lines, libcurl, SecretStore) was **removed** in commit `0871abf03` ("MCP server core replacing the in-editor AI agent"). The stated stance is "bring your own agent, model and API account" (`docs/mcp/README.md`).

### 2.2 OUR full tool list (140 tools; `docs/mcp/TOOLS.md`, generated by `tools/mcp/gen_tool_docs.py`)

| Module (`editor/mcp/tools/`) | Tools |
|---|---|
| Core (7) `CoreTools.cpp` | `batch` (≤200 calls, `undo_group` via `CommandHistory::mark/groupSince`, `stop_on_error`, images aggregated), `cancel_job`, `editor_status`, `get_job`, `redo`, `respond_dialog`, `undo` |
| Project (8) | `create_project`, `edit_build_scenes`, `edit_project_build_file`, `get_project_settings`, `get_project_summary`, `open_project`, `save_project`, `set_project_settings` |
| Scenes (14) | `apply_post_processing_preset`, `close_scene`, `create_scene`, `delete_scene`, `get_scene_settings`, `list_post_processing_presets`, `list_scenes`, `open_scene`, `rename_scene`, `save_scene`, `select_scene`, `set_child_scene`, `set_main_camera`, `set_scene_settings` |
| GameObjects (12) | `copy_entities`, `create_entity`, `delete_entities`, `duplicate_entities`, `find_entities`, `get_scene_tree`, `get_selection`, `move_entity`, `paste_entities`, `rename_entity`, `select_entities`, `set_entity_state` |
| Components (10) | `add_component`, `copy_component`, `get_entity`, `list_component_types`, `paste_component`, `remove_component`, `reset_component`, `set_mesh_geometry`, `set_properties` (multi-entity, multi-component, one undo step, read-back), `set_transform` |
| Prefabs (12) | `add_to_prefab`, `create_prefab`, `create_prefab_variant`, `get_prefab_info`, `instantiate_prefab`, `list_prefabs`, `prefab_mode`, `prefab_overrides`, `remove_from_prefab`, `replace_prefab`, `set_standalone_prefab`, `unpack_prefab` |
| Play and input (8) | `eval_lua`, `get_play_state`, `get_runtime_entity`, `list_runtime_entities`, `play` (start/stop/restart/pause/resume/step/set; `time_scale`, `fixed_fps`, `auto_pause`; optional screenshot), `send_input` (key/text/IME/mouse/touch/gamepad, CSS-selector targets, hold frames), `set_runtime_property`, `wait` (seconds/game_seconds/frames/Lua `until`, with input) |
| Views (5) | `configure_game_view`, `focus_window`, `frame_entity`, `screenshot` (game/scene/editor; waits for pending images, models and effects, `ViewTools.cpp:69-78`; flags flat images; `canvas_rect_in_image`), `set_scene_camera` |
| Console (2) | `clear_console`, `read_console` (cursor `since`, types, regex, contains, only_problems) |
| Scripts (10) | `attach_script`, `check_script`, `code_editor`, `compile_scripts`, `create_script`, `detach_script`, `list_scripts`, `set_script_properties`, `sync_files`, `update_script_entry` |
| Assets, materials, shaders (22) | `apply_material_preset`, `check_shader`, `create_folder`, `create_material`, `delete_asset`, `fork_shader`, `generate_placeholder_art`, `get_asset_info`, `get_shader_uniforms`, `import_files`, `link_material`, `list_assets`, `list_material_presets`, `list_shaders`, `move_asset`, `read_file`, `search_files`, `set_shader_uniform`, `set_texture_settings`, `update_material`, `view_asset`, `write_file` |
| Docs (3) | `read_engine_source`, `search_engine_api`, `search_engine_source` |
| Build and export (4) | `build_project`, `generate_shaders`, `get_build_info`, `run_build` |
| World (18) | `create_particles`, `create_ui`, `edit_animation`, `edit_colliders`, `edit_tilemap`, `get_animation`, `get_model_info`, `get_tilemap`, `preview_animation`, `sample_terrain`, `sculpt_terrain`, `set_joint`, `set_model`, `set_rect_layout`, `set_shape_points`, `set_sprite_animation`, `set_sprite_frames`, `set_terrain_maps` |
| HTML/CSS UI (5) | `assert_ui`, `create_ui_document`, `inspect_ui`, `list_ui_effects`, `set_ui_effect_time` |

- **Resources** (`DocsTools.cpp`, `HtmlUiTools.cpp`): `jasper://guide`, `jasper://docs` (index) and `jasper://docs/{name}` (lua-api, cpp-scripting, html-ui, shaders, post-processing, …), `jasper://examples/html-ui/{…}`, `jasper://project/AGENTS.md`, `jasper://project/{path}`.
- **Prompts**: `new_2d_game(idea, language)` and `fix_script_errors` (`DocsTools.cpp:579-633`).

### 2.3 CLI and scripts

- `jasperengine-editor` subcommands (`editor/main.cpp:21-49`, `editor/cli/CommandLine.{h,cpp}` 2.5k lines):
  - `export`, `shaders`, `scene-dump` (with an ops language: create/set/delete/spawn/duplicate/unpack/undo/redo/save…), `check`, `ui-support`, `ui-effects`, `import-ui`, `benchmark`, `mcp`.
  - **`check --hook`** is a Claude Code / Codex PostToolUse hook that checks each edited file and feeds errors back to the agent (`docs/mcp/checks.md`).
  - There is **no generic "call MCP tool X from a shell" command.**
- `tools/editor-shot.sh` takes editor screenshots under Docker + Xvfb, scripted with xdotool steps (click, drag, keys, `shot:TAG`). `tools/smoke-test.sh` runs export → build → run game → screenshot, and fails on error logs.
- `generate_api_suggestions.py` scans the LuaBridge bindings and writes `engine_api_suggestions.h` (code-editor completion). `generate_lua_stubs.py` is the companion for LuaLS stubs.

### 2.4 Agent docs

- `Generator::writeAgentsFile` (`editor/Generator.cpp:1403-1519`) writes a detailed **per-project** `AGENTS.md` once and a `CLAUDE.md` containing `@AGENTS.md`. It covers MCP usage, the Lua/C++ API, check hooks, HTML UI, generated files, VCS and build modes.
- The repo root has `.mcp.json` (jasper stdio server) and `docs/mcp/README.md` §"Developing JasperEngine itself".
- **The repo has no AGENTS.md, CLAUDE.md or `.claude/` for engine contributors.** `git ls-files` finds none; the only one is under `build/fixture-unity/`.

### 2.5 Tests

- `tools/mcp/`:
  - `harness.py` (847 lines) drives the real editor through the official `mcp` SDK, in a scratch copy of a project under Xvfb, over stdio or HTTP.
  - `fake_server.py` and `selftest.py` test the harness itself. `run_tests.py`/`run-tests.sh` support `-k` and `-j`.
  - **64 test files / ~549 `test_*` functions** against the real editor.
  - `tool_budget.py` keeps the tool definitions under about 100K characters (currently about 99.8K). `pngdiff.py`, `htmlui_pixels.py`, `core_smoke.py`.
- "Every tool needs a test" is a stated rule, but no gate enforces it.

---

## 3. Gaps — THEIRS has, OURS lacks

| Feature / tool | Their location | Value | Effort |
|---|---|---|---|
| Permission modes (Read only / Auto / Ask / Edit freely) and per-call **Allow / Allow for this turn / Deny**, with parked calls | `Packages/ai-assistant/Editor/AssistantGate.{h,cpp}`, `AssistantMode.h`, `AssistantTools.cpp`, debug-server gate registry | **H** (needed before we invite agents into a human's live editor over HTTP) | M (we already have `ToolActivity`, annotations and `McpCall::waitUntil` to park) |
| Named, attributed undo step per agent edit ("Assistant: Set X (Sun)") | `AssistantGate.cpp:277-345` (`BeginCompound`) | H | M (our `Command`/`CommandHistory` have no labels and there is no Undo History UI) |
| "Undo this agent turn" (linear: only when the turn's steps are on top) | `AssistantTurnUndo.{h,cpp}` | M | M (depends on the row above; `mark/groupSince` exists) |
| Call ledger plus one log line per decided call, shown in the activity UI with the result and the undo step it left | `AssistantActionLedger.{h,cpp}`, `AssistantActionLogLine` | M | S |
| Shell CLI for every MCP tool: flags from the schema, `{json}` passthrough, tri-state booleans, exit codes 0-4, `--json` envelope, `--dry-run`, stdin `batch` | `mcp/src/cli.ts`, `cli/args.ts`, `cli/output.ts`, `cli/raw.ts` | **H** (CI scripts, hooks, humans, agents without MCP) | S-M |
| In-editor assistant panel driving the user's local `claude`/`codex` CLI with our MCP attached (no keys stored; built-in tools off) | `Packages/ai-assistant/Editor/Providers/ClaudeSessionProvider.cpp:53-90`, `AssistantAttachment.cpp`, `PromptPanel.cpp` | M (fits "bring your own"; lowers the barrier for non-terminal users) | L-XL |
| Editor-UI introspection and injection (UI tree, panel tree, click element, key, text, drop, hover; context menus and modals raw-only) | `mcp/src/tools/ui.ts`, `DebugServer/UITreeDump.cpp`, `InjectedInput.cpp`, `PerformDropRequest.cpp` | M (agents testing editor UX; replaces xdotool in `editor-shot.sh`) | L (ImGui: item-ID registry or ImGui Test Engine) |
| Perf and profiling reads: render stats, CPU profiler tree, frame-phase ring, memory, job system | `tools/profiling.ts`, `DebugServer/FramePhaseReport.cpp`, `JobSystemReport.cpp`, `MemoryDebugHandlers.cpp` | H (perf work by agents; we have `Engine::FrameStats` at `engine/core/Engine.h:226` and the `benchmark` CLI, neither exposed to MCP) | S (stats) to M (CPU scopes) |
| GPU capture: RenderDoc trigger and status | `DebugServer/RenderDocCapture.cpp` (175 lines), `tools/capture.ts` | M | S-M |
| Readback of a named render target or buffer | `capture_resource` (`tools/capture.ts`) | M (shadow maps, depth, post buffers) | M (no render graph: we would expose framebuffers by name) |
| Screenshot crop targets (panel / element / rect) | `take_screenshot` targets | L-M | S |
| Restricting the served tool subset (`GE_MCP_TOOLS`; unknown name fails at start) | `mcp/src/assistant.ts:servedTools` | M (our tool budget is at about 99.8K / 100K) | S |
| Lifecycle tools for engine developers (build editor, run tests, launch on its own port, full rebuild-relaunch cycle, stop only your own editor) | `tools/lifecycle.ts`, `build/editor-process.ts`, `build/debug-port.ts` | M (dogfooding) | S-M |
| Coverage/drift gate (every handler exposed or justified) | `Tests/Mcp/ipc-method-drift.mjs`, `mcp/ipc-method-coverage.json` | M (our analogue: every tool has a test; TOOLS.md and the guide are current) | S |
| Repo-level contributor AGENTS.md / CLAUDE.md / path-scoped `.claude/rules` | `AGENTS.md`, `CLAUDE.md` | **H** | S |
| World mark-ups (volume/region/path annotations with threads) and `@markup` tokens injected into prompts | `tools/markups.ts`, `DebugServer/MarkupDebugHandlers.cpp`, `MarkupContext.cpp` | M | L |
| Scripted deterministic editor UI replay with JSONL telemetry | `Apps/Editor/Source/Automation/UiReplayRunner.{h,cpp}` | L-M | L |
| Animated camera move and look-at; gizmo visibility toggle for clean shots | `tools/view.ts` | L | S |
| Voice dictation in the prompt | `PromptMicButton.cpp`, `Platform/SpeechInput.h` | L | M |

---

## 4. Things THEIRS does better / things OURS does better

### THEIRS better (evidence)

1. **Safety model for a shared editor.**
   - Tool classes plus modes, enforced in the editor rather than by the client (`AssistantGate::Before` is a debug-server gate). Parked calls hold every later request on the same connection, which keeps them in order.
   - A 10-minute wait limit. Edits wait until the user's drag ends (`AssistantGate.cpp:627`).
   - Host-side writes are re-authorized via `assistant_authorize` and confined to the project root.
   - Undo/redo are deliberately denied to the agent, with a reasoned explanation (it cannot see the shared stack).
   - OURS lets any connected client run every tool, including `undo` and `delete_asset`. The only checks are the busy, play and dialog policies.
2. **Attribution in the undo history.** Each admitted agent edit is a compound step named for the action and its subject, and "Undo this turn" refuses out of order. OURS: commands have no names (`editor/command/Command.h`); `batch undo_group` is the only grouping.
3. **One registry, two front-ends.** A shell user gets every tool with schema-accurate flags, `--help` and `--dry-run`. OURS: only `harness.py` (Python, test-oriented) and the purpose-built CLI subcommands.
4. **Editor-UI automation through the app, not the OS.** THEIRS: `click_element`/`send_key`/`perform_drop` take the real input path. Their AGENTS.md forbids OS input synthesis. OURS: `tools/editor-shot.sh` uses xdotool on Xvfb, and MCP can only press dialog buttons and focus windows.
5. **Introspection depth for rendering and perf.** CPU and GPU profilers, frame phases, per-VUID validation, RenderDoc, Nsight and RGP, and render-graph resource readback with pre-exposure handled.
6. **Screenshot honesty in the summary text.**
   - `take_screenshot` puts the pending-texture-bind and "this is an OS window capture" warnings into the *text* block, not only into data.
   - It refuses OS window capture by default (`tools/capture.ts`).
   - OURS has a comparable but different safeguard: it waits for loads and flags a flat colour.
7. **Contributor agent docs.** Concrete, enforceable rules: isolated editor on a free port, never kill an editor you did not launch, red-before-green evidence, worktree discipline, verified/inferred/assumed tiers. OURS has nothing equivalent at repo level.
8. **Assistant test rigor.** A fake agent CLI plus recorded stream-json fixtures for Claude and Codex, and 2.2k lines of panel tests.

### OURS better (evidence)

1. **No external runtime and a smaller attack surface.**
   - OURS: an in-process server; stdio opens no port; HTTP checks Origin and supports a bearer token (`McpTransportHttp.cpp:447,534`).
   - THEIRS: Node ≥20 plus an unauthenticated TCP port that any local process, including a sandboxed agent shell, can use to call any method, raw ones included. Their own docs say so; they work around it by probing the Codex sandbox.
2. **Full MCP protocol surface.**
   - OURS has instructions (a 16 KB guide, tested), resources and templates (guide, docs, examples, project files, AGENTS.md), prompts, progress, cancellation, logging, annotations and an outputSchema option.
   - THEIRS: 3 resources and nothing else.
3. **Agent ergonomics.**
   - Path addressing (`Player/Gun`) vs numeric ids only.
   - Multi-entity `set_properties` with read-back vs one component per call.
   - An atomic `batch` with `undo_group` in MCP vs no MCP batch.
   - Jobs with progress (`get_job`/`cancel_job`) vs polling `get_build_status`.
   - Modal handling (`respond_dialog`, auto-dismiss) vs "a native popup … stalls the debug server" (`ipc-client.ts` timeout text).
4. **Play-mode testing.**
   - A deterministic clock (`fixed_fps`, `auto_pause`, `step`).
   - Game input across key, text, IME, mouse, touch and gamepad, with CSS-selector targets.
   - `wait` until a Lua condition, `eval_lua`, and runtime entity read/write.
   - A screenshot can be attached to `play`, `wait` and `send_input` results.
   - THEIRS: `set_play_mode` plus editor-path input, with no runtime state reads and no scripting eval.
5. **Code loop.** OURS: auto-sync of disk edits with `external_changes.problems`, `check --hook` for PostToolUse, generated LuaLS stubs, `.luarc.json`, `.clangd`, and a per-project AGENTS.md/CLAUDE.md. THEIRS has no equivalent feedback channel for agent-edited files.
6. **Content breadth.** Prefabs and variants (12), project and settings (8), scenes (14), assets, materials and shaders (22), world (18), HTML UI with `assert_ui` (5). THEIRS skews toward renderer diagnostics.
7. **Headless and CI.** OURS: `--headless` spawns Xvfb, works with Mesa software GL, and `smoke-test.sh`/`editor-shot.sh` run in Docker. THEIRS: the Windows desktop is primary, and the live harness is not in CI.
8. **Tests for the MCP layer.** OURS: about 549 tests over 64 files against the real editor, plus a fake-server selftest, a tool-size budget and a generated TOOLS.md. THEIRS: 4 static/faked gates plus manual harness scripts.
9. **Context-budget discipline.** OURS: `tool_budget.py` and shared conventions kept in the instructions. THEIRS: very long tool descriptions (`save_scene` and `set_component` are each about 1-2 KB) and no budget check.

---

## 5. Copy/port candidates (ranked by value/effort)

License: THEIRS is MIT (`external/OpenEngine/LICENSE`). Verbatim code needs their copyright and permission notice kept (for example in NOTICE.md and file headers). Most items below are **design ports**: we reimplement in our idioms and copy no text.

1. **Repo-level contributor `AGENTS.md` + `CLAUDE.md` (`@AGENTS.md`) + `.claude/rules/*.md`.**
   - Source for structure: `AGENTS.md`, `CLAUDE.md`.
   - Our content: `tools/build.sh` targets, `tools/mcp/run-tests.sh -k`, `smoke-test.sh`/`editor-shot.sh` evidence, a "use `JASPER_PROJECT` scratch copies, never the fixture" rule, "drive the editor via MCP, not xdotool on a real display", own working directory and `settings.yaml` per editor, never kill an editor you did not launch, and the `tool_budget.py` and `gen_tool_docs.py` regeneration duties.
   - Effort **S**. No dependencies. Write our own text; do not copy theirs.
2. **Generic tool CLI: `jasperengine-editor call <tool> ['{json}' | --flag v…] [--url http://127.0.0.1:47615/mcp | --project <dir>]`.**
   - Sources: `mcp/src/cli/args.ts` (kebab flags from JSON Schema, `a,b,c` vectors, `K=V` objects, tri-state `--x/--no-x`, lone `{…}` passthrough), `cli/output.ts` (exit codes 0/1/2/3/4, `--json` `{ok,code,error}`, PNG written to a file and never printed as base64 on stdout), `cli/raw.ts` (`batch` from stdin, `--dry-run`).
   - Adaptation: C++ in `editor/cli`. It speaks our Streamable HTTP, or spawns `mcp` stdio for a one-shot call, and reuses `McpSchema` for `--help`. A faster interim option is `tools/mcp/jcall.py` on top of `harness.py`.
   - Effort **S-M**.
3. **Approval gate plus permission modes, applied to MCP sessions.**
   - Sources: `AssistantGate.{h,cpp}` (`DecideAssistantCall`, the park/answer/turn model), `AssistantMode.h`, `AssistantTools.cpp` (the class table), `AssistantActionLedger.h`, `Tests/AssistantGateTests.cpp` (test ideas).
   - Adaptation:
     - Derive the class from our `annotations` and `ToolActivity`: readOnly → Read; Control → View; Edit + undoable → UndoableEdit; `send_input` → Input; save/build/write_file/delete_asset/`eval_lua allow_edit_mode` → Gated.
     - Store the mode per MCP session (HTTP `Mcp-Session-Id`; stdio = owner, default Edit freely).
     - In `McpServer::execute`, park through `call->waitUntil(...)` and show Allow / Allow for this session / Deny rows in `McpActivityWindow`. Write one log line per decided call.
     - Keep `undo`/`redo` callable over stdio, but deny them by default for HTTP sessions (their reasoning applies).
   - Effort **M**. No new dependencies.
4. **Named agent undo steps plus "Undo this agent call/turn".**
   - Sources: `AssistantGate.cpp:277-345` (`UndoStep` RAII around `BeginCompound(name)`; `AssistantUndoStepName` patterns), `AssistantTurnUndo.{h,cpp}` (linear guard).
   - Adaptation:
     - Add `std::string label` to `editor/command/Command.h`.
     - In `McpServer::execute`, wrap each non-readOnly call in `mark()`/`groupSince()` (already used by `batch`) and label it "MCP: Set Properties (Player/Gun)".
     - Add an undo history list and "Undo last agent call" to `McpActivityWindow`. It is enabled only when the call's group is the top of its `CommandHistory`.
   - Effort **M**.
5. **Perf read tools: `get_render_stats` and `get_frame_times`.**
   - Sources: `mcp/src/tools/profiling.ts` (`get_render_stats`, `get_frame_phases` contract: a ring of the newest N frames, attribution after the fact), `DebugServer/FramePhaseReport.cpp` (298 lines).
   - Adaptation: expose `Engine::getFrameStats()` (draw calls, instances, triangles), `framerate` and a 1024-entry ring of frame and update times recorded in the editor loop. Add a `stats: true` option to `play`, `wait` and `benchmark` results. Mind the tool budget (about 1-2K characters).
   - Effort **S**.
6. **Served-tool subset (`--tools a,b,c` / `JASPER_MCP_TOOLS`, plus named profiles such as `--tools-profile core|ui|world`).**
   - Source: `mcp/src/assistant.ts:servedTools`. An unknown name is a hard error at startup.
   - Adaptation: filter in `McpRegistry` at `tools/list` and `tools/call`. This relieves the about-100K budget.
   - Effort **S**.
7. **RenderDoc in-app capture tools (`trigger_capture`, `get_capture_status`).**
   - Source: `DebugServer/RenderDocCapture.cpp` (`renderdoc_app.h` API: `RENDERDOC_GetAPI`, `TriggerCapture`, capture path template, wait for the file to be written, recover on timeout).
   - Adaptation: an OpenGL/Vulkan editor build under `renderdoc` on Linux/Windows. `renderdoc_app.h` is MIT.
   - Effort **S-M**.
8. **Coverage gate for our MCP layer.**
   - Source: the idea behind `Tests/Mcp/ipc-method-drift.mjs` (fail on undeclared, stale or redundant entries).
   - Ours: a pytest that lists `tools/list` and fails when a tool name appears in no `tools/mcp/tests/*.py` and is not in an allow-list with reasons. Add a `gen_tool_docs.py --check` so a stale TOOLS.md fails. Fix the "jasperengine-editor unknown" version in the TOOLS.md header.
   - Effort **S**.
9. **Screenshot improvements.**
   - Source: `take_screenshot` in `tools/capture.ts`. It has `panel`/`rect` crop targets, and puts warnings into the result *text*, not only into structured data.
   - Ours: add `view:"panel", panel:"inspector"` (crop from the editor capture using ImGui window rects) and `rect`.
   - Effort **S**.
10. **Later: in-editor "Agent" panel driving the user's CLI.**
    - Sources: `ClaudeSessionProvider.cpp:53-90` (argument set: `-p --output-format stream-json --include-partial-messages --resume`, `--tools ""`, `--strict-mcp-config --mcp-config <per-turn file> --allowedTools mcp__<srv>__…`), `CliSessionProvider.cpp` (process, cancel, stream parsing), `AssistantAttachment.cpp` (per-turn config file under a per-PID temp dir), the key-stripping env policy, and `Tests/FakeAgentCli.cpp` plus fixtures.
    - Adaptation: an ImGui panel in which the CLI connects to our HTTP endpoint with a per-turn bearer token, so our gate (item 3) binds the session. This is compatible with our "bring your own agent" stance because no key is stored.
    - Verify Claude Code flag names against the current CLI before relying on them. `--permission-prompts` and `--system-prompt-snapshot` are their claims, unverified by me.
    - Effort **L-XL**. Do it only after items 3 and 4.

---

## 6. Not worth copying (and why)

- **External Node MCP server plus raw TCP debug socket.** It adds a Node runtime and an unauthenticated port, and it creates a two-layer drift problem that needs its own gate. Our in-process design avoids all of that.
- **The `ge raw` passthrough and `list_debug_methods`.** We have no second method layer; every capability is a typed tool.
- **The `[editor host:port]` attribution suffix.** Our stdio sessions own their editor, and HTTP sessions name the URL explicitly. The value is low; at most, `editor_status` could add pid and cwd.
- **Render-graph introspection (7 tools), render-settings A/B tools (MSAA/TAA/MSM/PCSS/HDR/LOD, 12), and Nsight/RGP capture.** They are specific to their Vulkan render-graph renderer and to vendor tooling. We have no render graph, and our settings go through `set_project_settings`/`set_scene_settings`.
- **Game-graph and C# ECS codegen tools.** Those systems have no equivalent here; `create_script`/`check_script` already cover our scripting.
- **Node SEA standalone CLI binary.** Our CLI is the editor binary itself.
- **ClaudeApiProvider (direct API chat without tools).** We removed exactly this kind of chat (`0871abf03`); it adds key handling for little value.
- **Codex sandbox port probe, session picker over CLI session files, login-status row, dictation.** These are polish that depends on the panel; consider them only if item 10 happens.
- **Their process rules (AB-LOCK, private workbench, Windows `C:/` path rules, lane coordination).** They are team- and machine-specific. Copy only the generic principles in item 1.
- **Their very long per-tool descriptions.** They contradict our tool budget; keep shared conventions in `McpInstructions.cpp`.
