// Copyright (c) 2026 Gamecrafters Guild Limited and contributors (OpenEngine, MIT; see NOTICE.md)
// Modified for JasperEngine by Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Export.h"
#include "MarkdownDocument.h"

#include <cstdint>
#include <string_view>

namespace jasperengine::htmlui::markdown {

/// Parses the Markdown subset language models and players write into a MarkdownDocument.
///
/// Blocks: paragraphs, ATX (`#`) and setext headings, bulleted (`-`, `*`, `+`) and numbered
/// (`1.`, `1)`) list items nested to any depth (up to kMaxListDepth), paragraphs and fences
/// indented under an item, fenced code blocks (``` and ~~~), block quotes (their paragraphs
/// separated by `>` lines with nothing else), thematic breaks and pipe tables. Inlines:
/// strong and emphasis (`*` and `_`, with CommonMark's flanking rules, so `snake_case` and
/// `2 * 3` stay as written), code spans, links (`[text](url)`, `<https://...>`), images
/// (`![alt](src)`), backslash escapes and hard line breaks.
///
/// Anything else is text: an unmatched delimiter shows as the character it is, never as a
/// directive the reader has to decode, and HTML markup shows as written (the document notes
/// it in RawHtml). A fence still open at the end of the source closes there, so a reply that
/// streams a code block shows it as code while it arrives.
class JASPERENGINE_API MarkdownParser
{
public:
    /// The deepest list nesting kept: an item indented under an item at the last depth is
    /// shown as its sibling. It bounds what hostile input can build, no real list nests this far.
    static constexpr uint8_t kMaxListDepth = 32;

    [[nodiscard]] static MarkdownDocument Parse(std::string_view source);
};

} // namespace jasperengine::htmlui::markdown
