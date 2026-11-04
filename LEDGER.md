# Ledger

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

## How a feature lands

1. The agent merges `main` into its branch and rebuilds, then passes `tools/check-changed`, the WERROR
   build and its spec's acceptance tests, and reports the commit.
2. The integrator reads the diff against the spec, fetches origin, and merges origin into `main` if it
   moved.
3. `git merge --no-ff` the branch. If `main` moved in files the branch touches, the agent re-merges and
   re-gates first.
4. Push `main`, delete the server worktrees, update this file.

A sharded full MCP pass runs on `main` after each wave, and a Mac run after each wave. Failures become
fixes at once.

## Status

### Fixes and editor work

| Spec | What | Branch | Server | Status |
|---|---|---|---|---|
| Mac | Final `main` on macOS | `fix/mac-final-main` | M | In progress |
| S41 | Curve window Delete and F; 5 rebindable keys | `feat/s41-window-commands` | H | In progress |
| S42 | Recorder full disk and readback | `feat/s42-recorder-integrity` | A | In progress |
| S43 | Sprite frames and weather map saves | `feat/s43-saved-output-determinism` | A | In progress |
| S44 | Shader lists for closed scenes | `feat/s44-shader-lists` | B | In progress |
| S45 | Partial model loads, OBJ materials | `feat/s45-model-capacity` | H | In progress |
| S46 | Crash leftovers | `feat/s46-crash-leftovers` | H | In progress |
| S47 | Android and iOS packaging, bundletool | `feat/s47-packaging-tests` | A, M | In progress |
| S48 | Linked folders, one file one GUID | `feat/s48-linked-folders` | B | In progress |
| S49 | Dialogs through `EditorModal` | `feat/s49-editor-modal` | B | In progress |
| S50 | Auto-sync settles before the reply | `feat/s50-autosync-settle` | B | In progress |
| S51 | A-3, Mesh update flags | `feat/s51-mesh-update-flags` | H | In progress |
| S52 | 521-file header fan-out; sanitizer installs | `feat/s52-build-hygiene` | H | In progress |
| S53 | `AssetRefSchema`, oracle, `keptText`, `createdShaders` | `feat/s53-code-cleanups` | B | In progress |

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

- 2026-11-04: S48: a link leading out of the project is a normal folder, as in Unity. The old cleanup
  left those assets untracked.
- 2026-11-04: S49: `respond_dialog` also fills fields and paths, so an agent can complete dialogs, not
  just cancel them.
- 2026-11-04: S45: no fixed capacity silently loses geometry in any build; the loading thread gives the
  same model as the main thread.
- 2026-11-04: S46: only folders with our marker are removed automatically; older ones are shown to the
  user, since they could hold data.
- 2026-11-04: S53: the legacy oracle's output is frozen as golden files before it is deleted.

## Done

Nothing yet.

## Log

- 2026-11-04: Fetched origin: nothing new, `main` at `b4f7816ea`. Wrote specs S41-S53 and this ledger.
- 2026-11-04: Started 15 agents, each in its own worktree: the Mac check, S38 M1, S41-S53.
