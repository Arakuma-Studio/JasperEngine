// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The only file that includes lexbor: its DOM is converted into HtmlTree here.

#include "htmlui/compat/html/HtmlParser.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/Diagnostics.h"

#include <lexbor/html/html.h>
#include <lexbor/html/interfaces/document.h>
#include <lexbor/html/interfaces/template_element.h>
#include <lexbor/html/parser.h>
#include <lexbor/html/tokenizer.h>
#include <lexbor/html/tokenizer/error.h>
#include <lexbor/html/tree.h>
#include <lexbor/html/tree/error.h>
#include <lexbor/dom/interfaces/attr.h>
#include <lexbor/dom/interfaces/character_data.h>
#include <lexbor/dom/interfaces/document_fragment.h>
#include <lexbor/dom/interfaces/element.h>

#include <cstring>
#include <utility>
#include <vector>

using namespace jasperengine::htmlui::compat;

namespace {

    struct Buffer {
        const char* begin;
        size_t size;
        uint32_t file;
        uint32_t baseOffset;

        bool contains(const lxb_char_t* pointer) const{
            const char* p = reinterpret_cast<const char*>(pointer);
            return pointer && p >= begin && p <= begin + size;
        }
        uint32_t offsetOf(const lxb_char_t* pointer) const{
            return baseOffset + (uint32_t)(reinterpret_cast<const char*>(pointer) - begin);
        }
        // a range of the buffer, invalid when either end is not inside it
        SourceRef range(const lxb_char_t* from, const lxb_char_t* to) const{
            if (file == 0 || !contains(from) || !contains(to) || to < from)
                return SourceRef{};
            return SourceRef{file, offsetOf(from), (uint32_t)(to - from)};
        }
    };

    // The content of a <markdown> element is raw text (S17 R24): its bytes, between the start tag
    // and `</markdown`, are Markdown, not HTML (`<https://x>`, `*` and `<b>` are written as they
    // are). Before lexbor parses the text, findMarkdownContent finds those ranges and they are
    // blanked (spaces, newlines kept, so every offset stays), and the converter hands each
    // element its raw content in HtmlNode::text. Comments and the raw text elements are skipped,
    // so a `<markdown` written in them is not one.
    struct RawContent {
        size_t begin = 0;   // the byte after the start tag's `>`
        size_t end = 0;     // the `<` of `</markdown`, or the end of the text
    };

    // `name` (lower case) at `at`, in any case, followed by what ends a tag name
    bool startsWithTag(std::string_view text, size_t at, std::string_view name){
        if (at + name.size() > text.size())
            return false;
        for (size_t i = 0; i < name.size(); i++){
            const char c = text[at + i];
            if ((c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c) != name[i])
                return false;
        }
        if (at + name.size() == text.size())
            return true;
        const char next = text[at + name.size()];
        return next == ' ' || next == '\t' || next == '\n' || next == '\r' || next == '\f' || next == '/' || next == '>';
    }

    // the position of `</name` (case-insensitive) at or after `from`, text.size() when there is none
    size_t findEndTag(std::string_view text, size_t from, std::string_view name){
        for (size_t at = text.find("</", from); at != std::string_view::npos; at = text.find("</", at + 2)){
            if (startsWithTag(text, at + 2, name))
                return at;
        }
        return text.size();
    }

    // the byte after the `>` that ends the start tag at `at` (quoted values may hold `>`)
    size_t startTagEnd(std::string_view text, size_t at){
        char quote = 0;
        for (size_t i = at; i < text.size(); i++){
            const char c = text[i];
            if (quote){
                if (c == quote)
                    quote = 0;
            }else if (c == '"' || c == '\''){
                quote = c;
            }else if (c == '>'){
                return i + 1;
            }
        }
        return text.size();
    }

    std::vector<RawContent> findMarkdownContent(std::string_view text){
        std::vector<RawContent> ranges;
        // elements whose content is text for the parser too: a `<markdown` inside one is text
        static constexpr std::string_view rawText[] = {"script", "style", "textarea", "title", "xmp", "noembed", "noframes", "iframe"};
        for (size_t at = text.find('<'); at != std::string_view::npos && at < text.size(); at = text.find('<', at + 1)){
            if (text.compare(at, 4, "<!--") == 0){
                const size_t close = text.find("-->", at + 4);
                if (close == std::string_view::npos)
                    break;
                at = close + 2;
                continue;
            }
            bool skipped = false;
            for (std::string_view name : rawText){
                if (startsWithTag(text, at + 1, name)){
                    const size_t close = findEndTag(text, startTagEnd(text, at), name);
                    at = close >= text.size() ? text.size() : close + 1;
                    skipped = true;
                    break;
                }
            }
            if (skipped || !startsWithTag(text, at + 1, "markdown"))
                continue;
            const size_t begin = startTagEnd(text, at);
            // `<markdown .../>` holds nothing (the transform moves what the parser puts inside after it)
            if (begin >= 2 && text[begin - 2] == '/')
                continue;
            const size_t end = findEndTag(text, begin, "markdown");
            ranges.push_back(RawContent{begin, end});
            at = end >= text.size() ? text.size() : end;
            if (at >= text.size())
                break;
        }
        return ranges;
    }

    // the text lexbor parses: `ranges` blanked
    std::string blankRanges(std::string_view text, const std::vector<RawContent>& ranges){
        std::string blanked(text);
        for (const RawContent& range : ranges){
            for (size_t i = range.begin; i < range.end; i++){
                if (blanked[i] != '\n')
                    blanked[i] = ' ';
            }
        }
        return blanked;
    }

    struct Converter {
        Buffer buffer;
        DiagnosticSink* sink;
        bool depthReported = false;
        // the text before blanking and its <markdown> contents (findMarkdownContent)
        std::string_view original;
        const std::vector<RawContent>* markdown = nullptr;

        static std::string toString(const lxb_char_t* data, size_t length){
            return data ? std::string(reinterpret_cast<const char*>(data), length) : std::string();
        }

        static Namespace namespaceOf(const lxb_dom_node_t* node){
            if (node->ns == LXB_NS_SVG)
                return Namespace::Svg;
            if (node->ns == LXB_NS_MATH)
                return Namespace::MathMl;
            return Namespace::Html;
        }

        // `<` .. `>` of a start tag: lexbor gives the byte after `<` and the end of the name; the
        // tag ends at the first `>` after the name and every attribute (the end of a quoted
        // value stops before its closing quote, which cannot be `>`)
        SourceRef startTagRange(const lxb_dom_node_t* node, const lxb_dom_element_t* element, bool& selfClosing,
                                const char** tagEnd = nullptr) const{
            selfClosing = false;
            const lxb_char_t* begin = node->jasper_src_begin;
            if (!buffer.contains(begin) || reinterpret_cast<const char*>(begin) == buffer.begin)
                return SourceRef{};
            const lxb_char_t* scan = node->jasper_src_end;
            if (!buffer.contains(scan))
                return SourceRef{};
            for (const lxb_dom_attr_t* attribute = element->first_attr; attribute; attribute = attribute->next){
                const lxb_char_t* end = attribute->node.jasper_src_end;
                if (buffer.contains(end) && end > scan)
                    scan = end;
            }
            const char* p = reinterpret_cast<const char*>(scan);
            const char* last = buffer.begin + buffer.size;
            // a value's closing quote
            if (p < last && (*p == '"' || *p == '\''))
                p++;
            while (p < last && *p != '>')
                p++;
            const char* end = (p < last) ? p + 1 : last;
            if (p < last && p > buffer.begin && *(p - 1) == '/')
                selfClosing = true;
            if (tagEnd)
                *tagEnd = end;
            // `<` (an element made from an end tag, `</br>`, starts at `</`)
            const char* open = reinterpret_cast<const char*>(begin) - 1;
            if (*open == '/' && open > buffer.begin && *(open - 1) == '<')
                open--;
            return SourceRef{buffer.file, buffer.offsetOf(reinterpret_cast<const lxb_char_t*>(open)), (uint32_t)(end - open)};
        }

        void convertAttribute(const lxb_dom_attr_t* attribute, bool keepCase, HtmlAttr& out) const{
            size_t length = 0;
            const lxb_char_t* name = keepCase ? lxb_dom_attr_qualified_name(attribute, &length) : lxb_dom_attr_local_name(attribute, &length);
            out.name = toString(name, length);
            if (keepCase && out.name.empty()){
                name = lxb_dom_attr_local_name(attribute, &length);
                out.name = toString(name, length);
            }
            size_t valueLength = 0;
            const lxb_char_t* value = lxb_dom_attr_value(const_cast<lxb_dom_attr_t*>(attribute), &valueLength);
            out.hasValue = attribute->value != nullptr;
            out.value = toString(value, valueLength);

            const lxb_char_t* begin = attribute->node.jasper_src_begin;
            const lxb_char_t* end = attribute->node.jasper_src_end;
            if (!buffer.contains(begin) || !buffer.contains(end) || end < begin)
                return;
            // the name runs to '=', whitespace, '/' or '>'
            const char* p = reinterpret_cast<const char*>(begin);
            const char* stop = reinterpret_cast<const char*>(end);
            const char* nameEnd = p;
            while (nameEnd < stop && *nameEnd != '=' && *nameEnd != ' ' && *nameEnd != '\t' && *nameEnd != '\n' &&
                   *nameEnd != '\r' && *nameEnd != '\f' && *nameEnd != '/' && *nameEnd != '>')
                nameEnd++;
            out.nameRef = SourceRef{buffer.file, buffer.offsetOf(begin), (uint32_t)(nameEnd - p)};
            if (!out.hasValue)
                return;
            const char* valueBegin = nameEnd;
            while (valueBegin < stop && *valueBegin != '=')
                valueBegin++;
            if (valueBegin < stop)
                valueBegin++;
            while (valueBegin < stop && (*valueBegin == ' ' || *valueBegin == '\t' || *valueBegin == '\n' || *valueBegin == '\r' || *valueBegin == '\f'))
                valueBegin++;
            if (valueBegin < stop && (*valueBegin == '"' || *valueBegin == '\''))
                valueBegin++;
            if (valueBegin > stop)
                valueBegin = stop;
            out.valueRef = SourceRef{buffer.file, buffer.offsetOf(reinterpret_cast<const lxb_char_t*>(valueBegin)), (uint32_t)(stop - valueBegin)};
        }

        std::unique_ptr<HtmlNode> convertOne(const lxb_dom_node_t* node) const{
            switch (node->type){
                case LXB_DOM_NODE_TYPE_ELEMENT: {
                    const lxb_dom_element_t* element = lxb_dom_interface_element(const_cast<lxb_dom_node_t*>(node));
                    std::unique_ptr<HtmlNode> out(new HtmlNode());
                    out->kind = HtmlNode::Kind::Element;
                    out->ns = namespaceOf(node);
                    size_t length = 0;
                    const lxb_char_t* name = (out->ns == Namespace::Html)
                        ? lxb_dom_element_local_name(const_cast<lxb_dom_element_t*>(element), &length)
                        : lxb_dom_element_qualified_name(element, &length);
                    out->tag = toString(name, length);
                    bool selfClosing = false;
                    const char* tagEnd = nullptr;
                    out->ref = startTagRange(node, element, selfClosing, &tagEnd);
                    if (buffer.file == 0)
                        out->ref = SourceRef{};
                    out->implied = !out->ref.valid();
                    out->selfClosingSyntax = selfClosing;
                    if (markdown && tagEnd && out->ns == Namespace::Html && out->tag == "markdown"){
                        const size_t begin = (size_t)(tagEnd - buffer.begin);
                        for (const RawContent& range : *markdown){
                            if (range.begin == begin){
                                out->text = std::string(original.substr(range.begin, range.end - range.begin));
                                break;
                            }
                        }
                    }
                    const bool keepCase = out->ns != Namespace::Html;
                    size_t attributes = 0;
                    for (const lxb_dom_attr_t* attribute = element->first_attr; attribute; attribute = attribute->next)
                        attributes++;
                    out->attrs.reserve(attributes);
                    for (const lxb_dom_attr_t* attribute = element->first_attr; attribute; attribute = attribute->next){
                        HtmlAttr& converted = out->attrs.emplace_back();
                        convertAttribute(attribute, keepCase, converted);
                        if (converted.name.empty())
                            out->attrs.pop_back();
                    }
                    return out;
                }
                case LXB_DOM_NODE_TYPE_TEXT:
                case LXB_DOM_NODE_TYPE_CDATA_SECTION:
                case LXB_DOM_NODE_TYPE_COMMENT: {
                    const lxb_dom_character_data_t* data = lxb_dom_interface_character_data(const_cast<lxb_dom_node_t*>(node));
                    std::unique_ptr<HtmlNode> out(new HtmlNode());
                    out->kind = (node->type == LXB_DOM_NODE_TYPE_COMMENT) ? HtmlNode::Kind::Comment : HtmlNode::Kind::Text;
                    out->text = toString(data->data.data, data->data.length);
                    out->ref = buffer.range(node->jasper_src_begin, node->jasper_src_end);
                    out->implied = !out->ref.valid();
                    return out;
                }
                case LXB_DOM_NODE_TYPE_DOCUMENT_TYPE: {
                    std::unique_ptr<HtmlNode> out(new HtmlNode());
                    out->kind = HtmlNode::Kind::Doctype;
                    out->implied = true;
                    return out;
                }
                default:
                    return nullptr;   // processing instructions (not produced by the HTML parser)
            }
        }

        // the children of `from` (and, for templates, their content) under `to`, iteratively:
        // an explicit stack keeps hostile nesting off the C++ stack
        void convertChildren(const lxb_dom_node_t* from, HtmlNode* to, int depth){
            struct Frame {
                const lxb_dom_node_t* next;   // the next lexbor child to convert
                HtmlNode* parent;
                int depth;
            };
            std::vector<Frame> stack;
            stack.push_back(Frame{from->first_child, to, depth});
            while (!stack.empty()){
                Frame& frame = stack.back();
                const lxb_dom_node_t* node = frame.next;
                if (!node){
                    stack.pop_back();
                    continue;
                }
                frame.next = node->next;
                HtmlNode* parent = frame.parent;
                const int nodeDepth = frame.depth + 1;

                std::unique_ptr<HtmlNode> converted = convertOne(node);
                if (!converted)
                    continue;
                HtmlNode* added = parent->appendChild(std::move(converted));

                if (added->kind != HtmlNode::Kind::Element)
                    continue;

                if (added->is("template")){
                    const lxb_html_template_element_t* element = lxb_html_interface_template(const_cast<lxb_dom_node_t*>(node));
                    added->templateContent = HtmlNode::makeElement("#content");
                    added->templateContent->parent = added;
                    if (element->content){
                        // the content gets its own stack (it is a separate tree)
                        convertChildren(lxb_dom_interface_node(element->content), added->templateContent.get(), nodeDepth);
                    }
                }

                if (!node->first_child)
                    continue;
                if (nodeDepth >= HtmlParser::kMaximumDepth){
                    // deeper elements go to this ancestor, as Chromium's parser does
                    if (!depthReported && sink){
                        sink->report(codes::HtmlParseError, added->location(),
                                     "elements are nested deeper than " + std::to_string(HtmlParser::kMaximumDepth) +
                                     " levels; deeper elements are attached to the element at that depth, as browsers do",
                                     "flatten the markup");
                        depthReported = true;
                    }
                    stack.push_back(Frame{node->first_child, parent, frame.depth});
                }else{
                    // (the children's list is allocated once)
                    size_t count = 0;
                    for (const lxb_dom_node_t* child = node->first_child; child; child = child->next)
                        count++;
                    added->children.reserve(count);
                    stack.push_back(Frame{node->first_child, added, nodeDepth});
                }
            }
        }

        void reportErrors(lxb_html_document_t* document){
            if (!sink)
                return;
            lxb_html_parser_t* parser = static_cast<lxb_html_parser_t*>(document->dom_document.parser);
            if (!parser)
                return;

            lxb_html_tokenizer_t* tokenizer = lxb_html_parser_tokenizer_noi(parser);
            if (tokenizer && tokenizer->parse_errors){
                const size_t count = lexbor_array_obj_length(tokenizer->parse_errors);
                for (size_t i = 0; i < count; i++){
                    const lxb_html_tokenizer_error_t* error =
                        static_cast<const lxb_html_tokenizer_error_t*>(lexbor_array_obj_get(tokenizer->parse_errors, i));
                    size_t length = 0;
                    const lxb_char_t* text = lxb_html_tokenizer_error_to_string(error->id, &length);
                    SourceRef ref = buffer.range(error->pos, error->pos);
                    if (ref.valid())
                        ref.length = 1;
                    sink->report(codes::HtmlParseError, ref, "HTML parse error: " + toString(text, length) + "; recovered as browsers do",
                                 "", "", "html-parse-error");
                }
            }

            lxb_html_tree_t* tree = lxb_html_parser_tree_noi(parser);
            if (tree && tree->parse_errors){
                const size_t count = lexbor_array_obj_length(tree->parse_errors);
                for (size_t i = 0; i < count; i++){
                    const lxb_html_tree_error_t* error =
                        static_cast<const lxb_html_tree_error_t*>(lexbor_array_obj_get(tree->parse_errors, i));
                    size_t length = 0;
                    const lxb_char_t* text = lxb_html_tree_error_to_string(error->id, &length);
                    SourceRef ref = buffer.range(error->begin, error->end);
                    if (ref.valid() && ref.offset > buffer.baseOffset && buffer.begin[ref.offset - buffer.baseOffset - 1] == '<'){
                        // lexbor points after '<' (and '</'): the diagnostic starts at the tag
                        ref.offset--;
                        ref.length++;
                    }
                    std::string message = toString(text, length);
                    if (error->id == LXB_HTML_RULES_ERROR_UNTOININMO)
                        message = "the document has no <!DOCTYPE html>: browsers render it in quirks mode";
                    else
                        message = "HTML parse error: " + message + "; recovered as browsers do";
                    sink->report(codes::HtmlParseError, ref, message,
                                 error->id == LXB_HTML_RULES_ERROR_UNTOININMO ? "start the file with <!DOCTYPE html>" : "",
                                 "", "html-parse-error");
                }
            }
        }
    };

    // self-closing syntax on HTML elements that are not void: the element stays open
    void reportSelfClosing(const HtmlNode& node, DiagnosticSink* sink){
        if (!sink)
            return;
        for (const std::unique_ptr<HtmlNode>& child : node.children){
            if (child->kind != HtmlNode::Kind::Element)
                continue;
            // (the engine's own <lottie/> is void: the element transform moves its content out)
            if (child->selfClosingSyntax && child->ns == Namespace::Html && !HtmlParser::isVoidElement(child->tag) && child->tag != "lottie"){
                sink->report(codes::SelfClosingTag, child->ref,
                             "'<" + child->tag + "/>' does not close the element in HTML: the following content goes inside it until its parent ends",
                             "write <" + child->tag + "></" + child->tag + ">", "", child->tag);
            }
            reportSelfClosing(*child, sink);
            if (child->templateContent)
                reportSelfClosing(*child->templateContent, sink);
        }
    }

    // One lexbor document per thread, cleaned between parses: creating one allocates its
    // arenas and tables, which costs more than parsing a small fragment
    struct ThreadDocument {
        lxb_html_document_t* document = nullptr;
        ~ThreadDocument(){
            if (document)
                lxb_html_document_destroy(document);
        }
    };

    lxb_html_document_t* createDocument(){
        thread_local ThreadDocument holder;
        if (!holder.document)
            holder.document = lxb_html_document_create();
        else
            lxb_html_document_clean(holder.document);
        return holder.document;
    }

    void releaseDocument(lxb_html_document_t* document){
        // kept for the next parse (cleaned then); the memory of this one is reused
        (void)document;
    }

}

bool HtmlParser::isVoidElement(std::string_view tag){
    static const char* const voids[] = {"area", "base", "basefont", "bgsound", "br", "col", "embed", "frame", "hr",
                                        "img", "input", "keygen", "link", "meta", "param", "source", "track", "wbr"};
    for (const char* name : voids){
        if (tag == name)
            return true;
    }
    return false;
}

HtmlDocument HtmlParser::parseDocument(uint32_t file, std::string_view text, DiagnosticSink* sink){
    HtmlDocument result;
    result.file = file;

    lxb_html_document_t* document = createDocument();
    if (!document){
        result.root = HtmlNode::makeElement("html");
        return result;
    }

    const std::vector<RawContent> markdown = findMarkdownContent(text);
    const std::string blanked = markdown.empty() ? std::string() : blankRanges(text, markdown);
    const std::string_view parsed = markdown.empty() ? text : std::string_view(blanked);
    lxb_status_t status = lxb_html_document_parse(document, reinterpret_cast<const lxb_char_t*>(parsed.data()), parsed.size());

    Converter converter{Buffer{parsed.data(), parsed.size(), file, 0}, sink};
    converter.original = text;
    converter.markdown = &markdown;
    if (status == LXB_STATUS_OK){
        lxb_dom_node_t* top = lxb_dom_interface_node(document);
        for (lxb_dom_node_t* node = top->first_child; node; node = node->next){
            if (node->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE){
                result.hasDoctype = true;
            }else if (node->type == LXB_DOM_NODE_TYPE_ELEMENT && !result.root){
                result.root = converter.convertOne(node);
                converter.convertChildren(node, result.root.get(), 1);
            }
        }
        result.quirks = lxb_dom_interface_document(document)->compat_mode == LXB_DOM_DOCUMENT_CMODE_QUIRKS;
        converter.reportErrors(document);
    }else if (sink){
        sink->report(codes::InternalError, SourceRef{file, 0, 0}, "the HTML parser failed (lexbor status " + std::to_string(status) + ")");
    }

    releaseDocument(document);

    if (!result.root)
        result.root = HtmlNode::makeElement("html");
    reportSelfClosing(*result.root, sink);
    return result;
}

HtmlDocument HtmlParser::parseFragment(uint32_t file, std::string_view text, uint32_t baseOffset,
                                       DiagnosticSink* sink, std::string_view contextTag){
    HtmlDocument result;
    result.file = file;
    result.root = HtmlNode::makeElement("#fragment");

    lxb_html_document_t* document = createDocument();
    if (!document)
        return result;

    const std::string tag(contextTag.empty() ? "body" : contextTag);
    lxb_dom_element_t* context = lxb_dom_document_create_element(lxb_dom_interface_document(document),
                                                                 reinterpret_cast<const lxb_char_t*>(tag.data()), tag.size(), nullptr);
    if (context){
        const std::vector<RawContent> markdown = findMarkdownContent(text);
        const std::string blanked = markdown.empty() ? std::string() : blankRanges(text, markdown);
        const std::string_view parsed = markdown.empty() ? text : std::string_view(blanked);
        lxb_dom_node_t* fragment = lxb_html_document_parse_fragment(document, context, reinterpret_cast<const lxb_char_t*>(parsed.data()), parsed.size());
        Converter converter{Buffer{parsed.data(), parsed.size(), file, baseOffset}, sink};
        converter.original = text;
        converter.markdown = &markdown;
        if (fragment){
            converter.convertChildren(fragment, result.root.get(), 1);
            converter.reportErrors(document);
        }else if (sink){
            sink->report(codes::InternalError, SourceRef{file, baseOffset, 0}, "the HTML parser failed on a fragment");
        }
    }

    releaseDocument(document);
    reportSelfClosing(*result.root, sink);
    return result;
}
