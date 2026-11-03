// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MARKDOWN_MARKDOWNHTML_H
#define MARKDOWN_MARKDOWNHTML_H

// Markdown to RML (S17 R24-R26): what <markdown>, el:setMarkdown(), data-markdown and
// UIDocument.markdownToHtml() show. The dialect is MarkdownParser's; the output is
// well-formed XML that is also valid HTML (RmlUi reads RML as XML, Lua gets the same string),
// compact (no white space between blocks), with every text and attribute value escaped:
// HTML in the Markdown, whoever wrote it, shows as its text.
//
//   <div class="markdown-body">  h1-h6, p, ul/ol (start="N" when not 1) > li (lists nest
//   inside their parent li), pre > code (data-lang="lua" when the fence names one),
//   blockquote > p, <hr/>, table > thead/tbody > tr > th/td; inline a[href], strong, em,
//   code, <img src="..." alt="..."/>, <br/>  </div>
//
// Links are made of web addresses (http, https) only: any other address shows its link text.
// Images load web addresses and scheme-less paths (resolved by RenderOptions::resolveUrl);
// any other source shows the alternative text.

#include "Export.h"

#include <functional>
#include <string>
#include <string_view>

namespace jasperengine::htmlui::markdown {

    struct RenderOptions {
        // maps a link href / image src as written in the Markdown to what the output gets
        // (the caller resolves asset paths); identity when empty. image = true for <img src>.
        // An empty result drops the address: the link shows its text, the image its alt text.
        std::function<std::string(const std::string& url, bool image)> resolveUrl;
    };

    struct RenderResult {
        std::string rml;            // <div class="markdown-body">...</div>
        bool rawHtml = false;       // the source had something that looks like an HTML tag (<script>, <img ...>,
                                    // </x>, <!-- -->) outside code: shown as text; the compile reports HTMLUI-HTML-0032
    };

    JASPERENGINE_API RenderResult renderToRml(std::string_view markdown, const RenderOptions& options = {});

    // inline <markdown> source: removes the common leading white space of the non-blank lines
    // (in columns: a tab advances to the next multiple of 4, as in the parser; a tab that
    // straddles the cut leaves its remaining columns as spaces), turns lines of only spaces
    // and tabs into empty ones, drops the leading and trailing blank lines and the '\r' of
    // CRLF line ends. Lines are joined by '\n', without a final one.
    JASPERENGINE_API std::string dedent(std::string_view text);

}

#endif //MARKDOWN_MARKDOWNHTML_H
