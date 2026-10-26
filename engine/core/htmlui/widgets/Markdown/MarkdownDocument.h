// Copyright (c) 2026 Gamecrafters Guild Limited and contributors (OpenEngine, MIT; see NOTICE.md)
// Modified for JasperEngine by Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::htmlui::markdown {

/// A run of inline text with one look. Whitespace inside Text is the source's, with soft line
/// breaks already turned into spaces; a '\n' left in Text is a hard line break.
struct MarkdownSpan
{
    std::string Text;
    bool Strong = false;
    bool Emphasis = false;
    /// Inline code: shown in the monospace face, its text taken literally.
    bool Code = false;
    /// An image: Text is its alternative text (plain, its looks resolved away) and Source its
    /// address as written. An image inside a link also has the link's Url.
    bool Image = false;
    std::string Source;
    /// The link's destination; empty when the span is not a link.
    std::string Url;

    bool operator==(const MarkdownSpan&) const = default;
};

/// The inline content of one block, in reading order.
using MarkdownInlines = std::vector<MarkdownSpan>;

enum class MarkdownBlockKind : uint8_t
{
    Paragraph,
    Heading,
    ListItem,
    CodeBlock,
    Quote,
    Rule,
    Table,
};

/// One block of a Markdown document, in reading order.
struct MarkdownBlock
{
    MarkdownBlockKind Kind = MarkdownBlockKind::Paragraph;
    /// Heading: its level, 1 to 6.
    uint8_t HeadingLevel = 0;
    /// ListItem: its nesting depth, 0 for a top-level item, at most one more than the item
    /// before it, below MarkdownParser::kMaxListDepth. Paragraph and CodeBlock with
    /// InListItem: the depth of the item they continue.
    uint8_t ListDepth = 0;
    /// Paragraph and CodeBlock: the block is indented under the list item at ListDepth (the
    /// last item at that depth) and is part of it: its second paragraph, a fence inside it.
    bool InListItem = false;
    /// ListItem: numbered rather than bulleted.
    bool Ordered = false;
    /// ListItem on an ordered list: the number its marker shows.
    uint32_t Number = 0;
    /// Quote: the block is the next paragraph of the quote before it (a `>` line with nothing
    /// else on it separates them); otherwise it starts a quote.
    bool QuoteContinues = false;
    /// Paragraph, Heading, ListItem and Quote: the inline content.
    MarkdownInlines Inlines;
    /// CodeBlock: the code, its lines joined by '\n', without a trailing newline.
    std::string Code;
    /// CodeBlock: the first word of the fence's info string ("cpp" for a fence opened with
    /// three backticks and cpp); empty when the fence names none.
    std::string Language;
    /// Table: every row's cells, the header row first; every row has the header's cell count.
    std::vector<std::vector<MarkdownInlines>> Rows;

    bool operator==(const MarkdownBlock&) const = default;
};

/// A parsed Markdown document: its blocks in reading order.
struct MarkdownDocument
{
    std::vector<MarkdownBlock> Blocks;
    /// The inline text held something that reads as HTML markup (`<b>`, `</b>`,
    /// `<img src=x>`, `<!-- -->`, `<!DOCTYPE ...>`): it is shown as its text, never
    /// interpreted. Code spans and code blocks do not count.
    bool RawHtml = false;
};

} // namespace jasperengine::htmlui::markdown
