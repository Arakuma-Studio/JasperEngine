// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S83-title-effects-generators.md §6.11, D12, R35, §6.4, §6.5; S81 R9)
// - Purpose: check_title's checks of what effects and generators cost a Realtime title on a
//   device, registered beside S80's built-ins (registerBuiltinTitleChecks calls
//   registerTitleFxRuntimeChecks):
//   - `fx_bake_only_realtime` (WARN): an effect or generator still above C3 on the mobile tier
//     (C4, D12) in a document a Realtime or Glyph Atlas title shows; fixes remove it or say to
//     set Render Mode to Baked;
//   - `fx_live_budget` (WARN): an Exposed parameter of live effect params whose live compose
//     re-renders: over the live budget on the mobile tier (0.5 ms, S81 R9, with the same
//     prediction the runtime makes, TitleLiveCompose.h) or after an effect with no live variant;
//     the fix lowers the param that costs most (e.g. Particles `count`) or bakes;
//   - `fx_realtime_budget` (WARN): a re-render predicted over 15 ms on the mobile tier; the fix
//     lowers the heaviest definition's costliest param to fit;
//   - `fx_hdr_on_8bit` (info, PASS): more than 0.5 % of the document's light above 4.0, which
//     clips on the RGBA8 path (D14's headroom), measured on a render of its light at 1/4.
//   A document no runtime title shows (TitleCheckContext::runtimeInstances known and empty)
//   passes the first three with what it would cost listed.
// - Thread-safety: main thread.

#pragma once

namespace jasperengine::editor {

    void registerTitleFxRuntimeChecks();

}
