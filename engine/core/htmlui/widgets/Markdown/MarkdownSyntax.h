// Copyright (c) 2026 Gamecrafters Guild Limited and contributors (OpenEngine, MIT; see NOTICE.md)
// Modified for JasperEngine by Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Internal to the Markdown parser's translation units: MarkdownParser.cpp reads the blocks,
// MarkdownInlineParser.cpp the inline content of each. Character classes are ASCII; a byte of a
// multi-byte UTF-8 sequence is neither whitespace nor punctuation, so it reads as a letter.

#include "MarkdownDocument.h"

#include <cstddef>
#include <string_view>

namespace jasperengine::htmlui::markdown {

inline bool IsSpaceOrTab(char c)
{
    return c == ' ' || c == '\t';
}

inline bool IsWhitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

inline bool IsAsciiPunctuation(char c)
{
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

inline bool IsAsciiLetter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

inline std::string_view TrimEnd(std::string_view text)
{
    while (!text.empty() && IsWhitespace(text.back()))
        text.remove_suffix(1);
    return text;
}

inline size_t CountRun(std::string_view text, size_t at, char c)
{
    size_t end = at;
    while (end < text.size() && text[end] == c)
        ++end;
    return end - at;
}

/// A web address (http or https): the only addresses the parser makes links of. Any other
/// address shows as its text.
inline bool IsWebAddress(std::string_view text)
{
    return text.starts_with("http://") || text.starts_with("https://");
}

/// An address an image may load: a web address, or a path with no scheme (`icons/a.png`,
/// `/ui/a.png`), which the caller resolves against the document. `javascript:`, `data:`,
/// `file:` and every other scheme are not: such an image shows its alternative text.
inline bool IsImageAddress(std::string_view text)
{
    if (text.empty())
        return false;
    if (IsWebAddress(text))
        return true;
    // RFC 3986: a scheme is a letter, then letters, digits, '+', '-' or '.', then ':'
    if (!IsAsciiLetter(text[0]))
        return true;
    size_t at = 1;
    while (at < text.size() && (IsAsciiLetter(text[at]) || (text[at] >= '0' && text[at] <= '9') || text[at] == '+' ||
                                text[at] == '-' || text[at] == '.'))
        ++at;
    return at == text.size() || text[at] != ':';
}

/// Parses one block's inline content: strong, emphasis, code spans, links, images, autolinks,
/// bare web addresses, escapes and line breaks. Sets `rawHtml` when the text holds HTML
/// markup, which stays text (MarkdownDocument::RawHtml); leaves it as it was otherwise.
[[nodiscard]] MarkdownInlines ParseMarkdownInlines(std::string_view text, bool& rawHtml);

} // namespace jasperengine::htmlui::markdown
