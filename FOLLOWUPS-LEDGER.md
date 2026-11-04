# Follow-ups and sound layer: ledger

The log of the follow-ups and sound layer push, started 2026-11-04 from `main` at `b4f7816ea`
(origin had nothing newer). Only the integrator edits this file. A status changes here when it
changes for real.

## Goal

Implement the whole list the user picked, 100% complete and correct, one branch per spec. When a
feature is fully done: merge `origin/main` and the branch into `main` with a real merge, and push.
Repeat until everything is pushed.

## Requirements (from the user, 2026-11-04)

- Start from the newest remote code.
- Product decisions are mine, made for what users and agents would expect of a game engine.
- Specs hold the goals and requirements; no implementation plans or extra documents.
- Normal parallel subagents, not workflows; each in its own worktree and branch.
- One branch per spec. Commit progress. Each finished feature goes to `main` and is pushed on its own.
- This file records the goals, what is running, what is left and what is done.

## Other programs on `main`

Two other programs push to `origin/main` at the same time, so fetch before every landing. Whoever lands second merges onto the first.

- Pixel art: specs S41-S52, `PIXEL-ART-LEDGER.md`, `feature/sNN-*`.
- Doriax catch-up: `docs/doriax-catchup/LEDGER.md`, `docs/superpowers/specs/2026-11-04-*`, `dc/*`.

This program's specs are S61-S73, because the pixel art specs took S41-S52 first. Worktree folders keep their old names (`je-wt/s41` holds S61, add 20).

Overlaps: S65 stops at the loading thread, because `dc/mesh-memory` removes the fixed capacities. S67 meets `dc/mobile-shipping`, which retires AdMob entirely. S64 meets `dc/export-pipeline` (shader keys). S68 meets `dc/export-pipeline` and `dc/project-window` (links). S63 meets the pixel art S52 and `dc/mesh-memory` (`framesRect`).

## How a feature lands

1. The agent merges `main` into its branch and rebuilds, then passes `tools/check-changed`, the WERROR
   build and its spec's acceptance tests, and reports the commit.
2. The integrator reads the diff against the spec, fetches origin, and merges origin into `main` if it
   moved.
3. `git merge --no-ff` the branch. If `main` moved in files the branch touches, the agent re-merges and
   re-gates first.
4. Push `main`, delete the server worktrees, update this file.

AGENTS.md (2026-11-04) requires the full suite before a push to `origin main` when the push needs it,
such as a big feature; a small change does not. There is no CI, so nothing else checks a push. Big here:
S65, S67, S68, S69, S70, S72 and S35-S40. Several ready big features may share one full pass, on a local
`main` holding all of them. The Mac runs after each group.

## Status

### Fixes and editor work

| Spec | What | Branch | Server | Status |
|---|---|---|---|---|
| Mac | Final `main` on macOS | `fix/mac-final-main` | M | In progress |
| CI list | Classify 46 test files so `--suite ci` runs (check-changed uses it) | `fix/ci-suite-classify` | B | Done |
| S61 | Curve window Delete and F; 5 rebindable keys | `feat/s61-window-commands` | H | In progress |
| S62 | Recorder full disk and readback | `feat/s62-recorder-integrity` | A | In progress |
| S63 | Sprite frames and weather map saves | `feat/s63-saved-output-determinism` | A | In progress |
| S64 | Shader lists for closed scenes | `feat/s64-shader-lists` | B | In progress |
| S65 | Models on the loading thread (capacities are `dc/mesh-memory`'s) | `feat/s65-model-capacity` | H | In progress |
| S66 | Crash leftovers | `feat/s66-crash-leftovers` | H | In progress |
| S67 | Android and iOS packaging, bundletool | `feat/s67-packaging-tests` | A, M | In progress |
| S68 | Linked folders, one file one GUID | `feat/s68-linked-folders` | B | In progress |
| S69 | Dialogs through `EditorModal` | `feat/s69-editor-modal` | B | In progress |
| S70 | Auto-sync settles before the reply | `feat/s70-autosync-settle` | B | In progress |
| S71 | A-3, Mesh update flags | `feat/s71-mesh-update-flags` | H | In progress |
| S72 | 521-file header fan-out; sanitizer installs | `feat/s72-build-hygiene` | H | In progress |
| S73 | `AssetRefSchema`, oracle, `keptText`, `createdShaders` | `feat/s73-code-cleanups` | B | In progress |

### Sound layer

Order comes from the specs' dependencies. S38 M1 lands first, because S35 and S36 use `AudioMeter`.

| Spec | What | Branch | Needs | Status |
|---|---|---|---|---|
| S38 | Loudness, limiter, new-project defaults | `feat/s38-loudness` | | In progress: M1 |
| S36 | Analysis, generator, library, import settings, credits, search | `feat/s36-sound-library` | S38 M1 | Waiting |
| S37 | Sound Events, triggers, Lua `Audio`, Sound Browser | `feat/s37-sound-events` | M6 needs S36 | Waiting |
| S35 | Audio driver, capture, `audio_session`, QA | `feat/s35-agent-audio` | S38 M1; M5 needs S36 M1 | Waiting |
| S39 | Music | `feat/s39-music` | S35, S36, S37 | Waiting |
| S40 | Timeline audio | `feat/s40-timeline-audio` | S35, S36, S37 | Waiting |

## Decisions

- 2026-11-04: S68: a link leading out of the project is a normal folder, as in Unity. The old cleanup
  left those assets untracked.
- 2026-11-04: S69: `respond_dialog` also fills fields and paths, so an agent can complete dialogs, not
  just cancel them.
- 2026-11-04: S65: no fixed capacity silently loses geometry in any build; the loading thread gives the
  same model as the main thread.
- 2026-11-04: S66: only folders with our marker are removed automatically; older ones are shown to the
  user, since they could hold data.
- 2026-11-04: S73: the legacy oracle's output is frozen as golden files before it is deleted.

- 2026-11-04: S67: Android Vulkan is no longer offered. It could never have compiled: no sokol backend is set for it,
  and Android only creates an EGL context. Android uses GLES3. A real Android Vulkan backend is a separate feature (see Left).

- 2026-11-04: S68 vs `dc/export-pipeline`: that spec's §6.1 and §7 rule 2 say the export walk does not follow links. S68 ships
  linked folders, because that is what users expect and what the editor already reads. Whoever lands second makes `copyAssets`,
  `copyLua` and `copyCustomShaderSources` follow links. `test_linked_folders_export.py` fails if they stop.
- 2026-11-04: MCP eval: agents add cases but cannot run them. The integrator runs new cases at landing, on Orquester accounts.

## Left over, not on the user's list

- Android Vulkan backend (found by S67).
- `editor_status.playing` also reads true while a scene is saving, which can mislead agents (found by S69; already like this before).

## Done

- `fix/ci-suite-classify`: `--suite ci` runs again. 26 files CI = True, 20 CI_EXCLUDED with reasons, a guard in check-changed. Also fixed two tests pinning guide text origin cut. New CI files 112 passed, 0 failed, 0 skipped; both edited files whole 55 passed. Whole check-changed not run: it would select 47 MCP files, some building real players, for a change that only adds a constant to the 20 excluded files.

## Log

- 2026-11-04: Fetched origin: nothing new, `main` at `b4f7816ea`. Wrote specs S61-S73 and this ledger.
- 2026-11-04: Started 15 agents, each in its own worktree: the Mac check, S38 M1, S61-S73.
- 2026-11-04: Merged origin/main (`e8142a81e`, lean MCP surface; someone else is pushing to origin, so fetch before every landing). B's jobs per slot cut from 8 to 4 (load was 28 on 12 cores).
- 2026-11-04: User pointed to new remote changes. Merged origin/main `297562c95`: pixel art specs, Doriax catch-up specs, AGENTS.md rewrite, GitHub workflows removed. Renumbered this program to S61-S73. Told every agent.
- 2026-11-04: S68's red run: 4 of its tests fail on the control editor as intended; one more was fixed so it does too (not yet rerun).
- 2026-11-04: First landing: `fix/ci-suite-classify`. S69 found a real bug: the Loading popup swallows a dialog opened once; fixing with a red test. S63's guard: 42 of 45 fixture scenes differ on the unfixed tree (somewhat inflated).
