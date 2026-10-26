// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/widgets/Markdown/MarkdownHtml.h"
#include "htmlui/widgets/Markdown/MarkdownParser.h"

#include <algorithm>
#include <array>
#include <vector>

using namespace jasperengine::htmlui::markdown;

namespace {

    // What a byte of text becomes in the output; nullptr = itself. '{' is written as a
    // character reference: RmlUi reads "{{" in text as a data binding expression (and its XML
    // parser skips markup inside one), so text from players must never hold one. The C0
    // controls XML 1.0 does not allow become U+FFFD.
    constexpr std::array<const char*, 256> makeEntities(){
        std::array<const char*, 256> table{};
        for (int c = 0; c < 0x20; c++){
            if (c != '\t' && c != '\n' && c != '\r')
                table[c] = "\xEF\xBF\xBD";
        }
        table['&'] = "&amp;";
        table['<'] = "&lt;";
        table['>'] = "&gt;";
        table['"'] = "&quot;";
        table['{'] = "&#123;";
        return table;
    }
    constexpr std::array<const char*, 256> entities = makeEntities();

    // appends `text` escaped; with `breaks`, a '\n' (a hard line break) is written as <br/>
    void appendEscaped(std::string& out, std::string_view text, bool breaks = false){
        size_t run = 0;
        for (size_t i = 0; i < text.size(); i++){
            const char c = text[i];
            const char* entity = (breaks && c == '\n') ? "<br/>" : entities[(unsigned char)c];
            if (!entity)
                continue;
            out.append(text.data() + run, i - run);
            out += entity;
            run = i + 1;
        }
        out.append(text.data() + run, text.size() - run);
    }

    class RmlWriter {
    public:
        RmlWriter(const RenderOptions& renderOptions, std::string& output): options(renderOptions), out(output) {}

        void write(const MarkdownDocument& document){
            out += "<div class=\"markdown-body\">";
            for (const MarkdownBlock& block : document.Blocks){
                if (block.Kind == MarkdownBlockKind::ListItem){
                    closeQuote();
                    listItem(block);
                    inlines(block.Inlines);
                    continue;
                }
                if (block.InListItem && block.ListDepth < lists.size()){
                    // inside the open li at its depth, after the lists nested in it
                    closeQuote();
                    closeLists(block.ListDepth + 1);
                    leafBlock(block);
                    continue;
                }
                closeLists(0);
                if (block.Kind == MarkdownBlockKind::Quote){
                    if (!block.QuoteContinues || !quoteOpen){
                        closeQuote();
                        out += "<blockquote>";
                        quoteOpen = true;
                    }
                    out += "<p>";
                    inlines(block.Inlines);
                    out += "</p>";
                    continue;
                }
                closeQuote();
                leafBlock(block);
            }
            closeQuote();
            closeLists(0);
            out += "</div>";
        }

    private:
        const RenderOptions& options;
        std::string& out;
        std::vector<bool> lists;    // the open lists, outermost first (true: ol); each has an open li
        bool quoteOpen = false;
        // resolveUrl's last answer (a link of several spans asks once)
        std::string resolvedFrom;
        bool resolvedImage = false;
        std::string resolved;

        void leafBlock(const MarkdownBlock& block){
            switch (block.Kind){
                case MarkdownBlockKind::Paragraph:
                    out += "<p>";
                    inlines(block.Inlines);
                    out += "</p>";
                    break;
                case MarkdownBlockKind::Heading: {
                    const char level = (char)('0' + std::clamp<int>(block.HeadingLevel, 1, 6));
                    out += "<h";
                    out += level;
                    out += '>';
                    inlines(block.Inlines);
                    out += "</h";
                    out += level;
                    out += '>';
                    break;
                }
                case MarkdownBlockKind::CodeBlock:
                    out += "<pre><code";
                    if (!block.Language.empty()){
                        out += " data-lang=\"";
                        appendEscaped(out, block.Language);
                        out += '"';
                    }
                    out += '>';
                    appendEscaped(out, block.Code);
                    out += "</code></pre>";
                    break;
                case MarkdownBlockKind::Rule:
                    out += "<hr/>";
                    break;
                case MarkdownBlockKind::Table:
                    table(block);
                    break;
                case MarkdownBlockKind::ListItem:
                case MarkdownBlockKind::Quote:
                    break;      // (written by write())
            }
        }

        void table(const MarkdownBlock& block){
            out += "<table>";
            for (size_t row = 0; row < block.Rows.size(); row++){
                if (row == 0)
                    out += "<thead>";
                else if (row == 1)
                    out += "<tbody>";
                out += "<tr>";
                const char* cell = row == 0 ? "th>" : "td>";
                for (const MarkdownInlines& content : block.Rows[row]){
                    out += '<';
                    out += cell;
                    inlines(content);
                    out += "</";
                    out += cell;
                }
                out += "</tr>";
                if (row == 0)
                    out += "</thead>";
            }
            if (block.Rows.size() > 1)
                out += "</tbody>";
            out += "</table>";
        }

        // Opens the li of `item`: a sibling of the open li at its depth (in the same list when
        // it has the same kind, else in a new one), or the first li of a list nested in it.
        // An item deeper than the open lists allow is placed one below the deepest.
        void listItem(const MarkdownBlock& item){
            const size_t depth = std::min<size_t>(item.ListDepth, lists.size());
            closeLists(depth + 1);
            if (lists.size() == depth + 1){
                if (lists.back() == item.Ordered){
                    out += "</li><li>";
                    return;
                }
                closeLists(depth);
            }
            if (item.Ordered){
                if (item.Number == 1){
                    out += "<ol>";
                }else{
                    out += "<ol start=\"";
                    out += std::to_string(item.Number);
                    out += "\">";
                }
            }else{
                out += "<ul>";
            }
            out += "<li>";
            lists.push_back(item.Ordered);
        }

        // closes the lists nested deeper than `depth` (0: every list)
        void closeLists(size_t depth){
            while (lists.size() > depth){
                out += lists.back() ? "</li></ol>" : "</li></ul>";
                lists.pop_back();
            }
        }

        void closeQuote(){
            if (quoteOpen){
                out += "</blockquote>";
                quoteOpen = false;
            }
        }

        // the address the output gets for `url` (empty: none)
        const std::string& resolve(const std::string& url, bool image){
            if (url != resolvedFrom || image != resolvedImage){
                resolvedFrom = url;
                resolvedImage = image;
                resolved = options.resolveUrl ? options.resolveUrl(url, image) : url;
            }
            return resolved;
        }

        // The spans of one block. Links, strong and emphasis are opened over runs of spans and
        // nested in that order (a outermost), so "**bold *both* bold**" is one strong with an
        // em inside; a code span or an image sits inside whatever is open.
        void inlines(const MarkdownInlines& spans){
            std::string link;        // the open a's href ("" = none)
            bool strong = false;
            bool emphasis = false;
            for (const MarkdownSpan& span : spans){
                const std::string href = span.Url.empty() ? std::string() : resolve(span.Url, false);
                if (href != link){
                    closeInline(link, strong, emphasis, 0);
                }else if (strong != span.Strong){
                    closeInline(link, strong, emphasis, 1);
                }else if (emphasis != span.Emphasis){
                    closeInline(link, strong, emphasis, 2);
                }
                if (link.empty() && !href.empty()){
                    out += "<a href=\"";
                    appendEscaped(out, href);
                    out += "\">";
                    link = href;
                }
                if (!strong && span.Strong){
                    out += "<strong>";
                    strong = true;
                }
                if (!emphasis && span.Emphasis){
                    out += "<em>";
                    emphasis = true;
                }
                if (span.Image){
                    const std::string& src = resolve(span.Source, true);
                    if (src.empty()){
                        appendEscaped(out, span.Text);
                    }else{
                        out += "<img src=\"";
                        appendEscaped(out, src);
                        out += "\" alt=\"";
                        appendEscaped(out, span.Text);
                        out += "\"/>";
                    }
                }else if (span.Code){
                    out += "<code>";
                    appendEscaped(out, span.Text);
                    out += "</code>";
                }else{
                    appendEscaped(out, span.Text, true);
                }
            }
            closeInline(link, strong, emphasis, 0);
        }

        // closes the open inline wrappers from the innermost down to `level` (0: a, 1: strong,
        // 2: em)
        void closeInline(std::string& link, bool& strong, bool& emphasis, int level){
            if (emphasis){
                out += "</em>";
                emphasis = false;
            }
            if (level <= 1 && strong){
                out += "</strong>";
                strong = false;
            }
            if (level == 0 && !link.empty()){
                out += "</a>";
                link.clear();
            }
        }
    };

    size_t indentColumns(std::string_view line){
        size_t columns = 0;
        for (const char c : line){
            if (c == ' ')
                columns++;
            else if (c == '\t')
                columns = (columns / 4 + 1) * 4;
            else
                break;
        }
        return columns;
    }

    bool blankLine(std::string_view line){
        return line.find_first_not_of(" \t") == std::string_view::npos;
    }

}

RenderResult jasperengine::htmlui::markdown::renderToRml(std::string_view markdown, const RenderOptions& options){
    const MarkdownDocument document = MarkdownParser::Parse(markdown);
    RenderResult result;
    result.rawHtml = document.RawHtml;
    result.rml.reserve(markdown.size() + markdown.size() / 2 + 64);
    RmlWriter(options, result.rml).write(document);
    return result;
}

std::string jasperengine::htmlui::markdown::dedent(std::string_view text){
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start <= text.size()){
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        lines.push_back(line);
        start = end + 1;
    }

    size_t first = 0;
    while (first < lines.size() && blankLine(lines[first]))
        first++;
    size_t last = lines.size();
    while (last > first && blankLine(lines[last - 1]))
        last--;
    if (first == last)
        return std::string();

    size_t common = (size_t)-1;
    for (size_t i = first; i < last; i++){
        if (!blankLine(lines[i]))
            common = std::min(common, indentColumns(lines[i]));
    }

    std::string out;
    out.reserve(text.size());
    for (size_t i = first; i < last; i++){
        if (i > first)
            out += '\n';
        const std::string_view line = lines[i];
        if (blankLine(line))
            continue;
        size_t columns = 0;
        size_t at = 0;
        while (columns < common){
            columns = line[at] == '\t' ? (columns / 4 + 1) * 4 : columns + 1;
            at++;
        }
        out.append(columns - common, ' ');
        out.append(line.substr(at));
    }
    return out;
}
