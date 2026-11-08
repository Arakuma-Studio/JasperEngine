// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S85-title-animation.md R54, §6.7.2)
// - Purpose: check_title's animation checks, group "anim": anim_loop_seamless, anim_duration,
//   anim_end_state, anim_bounds, anim_static, anim_flash, anim_frame_budget,
//   anim_flipbook_memory, anim_mode_support, anim_markers, anim_glyph_merge, anim_orphans. Each
//   measures and reports PASS / WARN / FAIL (SKIP when it does not apply: no animation, or a
//   pixel check without a renderer) with `measured`, `threshold`, `at` and ready fix ops; none
//   claims aesthetic approval (anim_flash says it is not a certification). `options.animation`
//   restricts them to one animation.
// - Thread-safety: main thread.

#pragma once

namespace jasperengine::editor {

    void registerTitleAnimChecks();

}
