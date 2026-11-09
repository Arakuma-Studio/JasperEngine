// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S84-title-library.md §6.16, R40)
// - Purpose: S84's one registration call, made with the Title Art MCP tools at editor start: the
//   library ops, the checks (style_lint, color_scheme_match, metal_range), the Title Designer's
//   Library provider (replacing S82's starter set) and font list, and the project hook that
//   keeps the library on the open project. Registering builds no index: the library loads on its
//   first use (R40), so editor start time is unchanged.
// - Thread-safety: main thread, once.

#pragma once

namespace jasperengine::editor::titlelibrary {

    void registerTitleLibrary();

}
