// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S81-title-art-runtime.md R69-R71, §6.5, §6.7)
// - Purpose: get_title and render_title with `entity` instead of `path`: a Title Art Renderer's
//   state (ready, pending, overrides, effective values, live layers, Characters, particles), its
//   render with what-if overrides (nothing saved), and rerender of the live instance (Play or
//   Edit). Both `path` and `entity`, or neither, are refused with {kind: "bad_args", fix}.
// - Thread-safety: main thread (tool handlers).

#pragma once

#include "mcp/McpCall.h"

namespace jasperengine::editor::mcp::titletools {

    // true when the call names `entity` (or names both or neither): it was answered here
    bool getTitleInstance(const CallPtr& call);
    bool renderTitleInstance(const CallPtr& call);

}
