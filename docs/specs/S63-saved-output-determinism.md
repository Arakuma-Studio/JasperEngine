# S63 — Saved files do not depend on what was drawn or loaded

**Depends on.** BF20 (the same class of bug), S03. Touches `editor/Factory.cpp`, `engine/core/subsystem/AtmosphereRenderer.cpp`,
serialization and reflection.

## 1. Goal
Saving a scene twice gives the same bytes whether or not its textures finished loading or a frame was drawn between
saves. Generated code and scene files hold authored data only. Two known cases remain, and the spec adds a guard so the
next one is caught by a test.

## 2. Requirements
- R1. Sprite frames: `framesRect` is normalised only once the texture has loaded (`Factory.cpp`, `Sprite.cpp`), so generated
  code differs by load timing. After the fix the authored value is what is stored and generated, whatever the load state,
  and rendering gives the same pixels. Old files load unchanged.
- R2. Clouds: `AtmosphereRenderer::weatherTexture` (`:726`) writes `TextureWrap::REPEAT` into the component's texture during a
  draw, so a saved file depends on draw history. After the fix the wrap is applied at bind or sampler level or at decode, so
  a component's saved `weatherMap` never changes because of a draw, while clouds still tile.
- R3. Guard: an MCP or unit test opens each scene of a representative fixture set, saves, draws frames, waits for assets,
  saves again, and compares scene files and generated sources byte for byte. Any further case it finds is fixed here, or
  it is listed with a stated reason in Deviations; nothing is left silent.
- R4. No format bump unless needed; if needed, a migration per S03.

## 3. Acceptance
- AC1. Red-then-green tests for R1 and R2, each failing without its fix.
- AC2. R3's guard runs in the MCP suite and in `tools/check-changed` for serialization and factory changes.
- AC3. Rendering goldens unchanged for sprites and clouds. `tools/check-changed` green.
