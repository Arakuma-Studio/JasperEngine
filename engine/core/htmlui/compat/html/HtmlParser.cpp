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

    struct Converter {
        Buffer buffer;
        DiagnosticSink* sink;
        bool depthReported = false;

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
        SourceRef startTagRange(const lxb_dom_node_t* node, const lxb_dom_element_t* element, bool& selfClosing) const{
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
                    out->ref = startTagRange(node, element, selfClosing);
                    out->implied = !out->ref.valid();
                    out->selfClosingSyntax = selfClosing;
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

    lxb_status_t status = lxb_html_document_parse(document, reinterpret_cast<const lxb_char_t*>(text.data()), text.size());

    Converter converter{Buffer{text.data(), text.size(), file, 0}, sink};
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
        lxb_dom_node_t* fragment = lxb_html_document_parse_fragment(document, context, reinterpret_cast<const lxb_char_t*>(text.data()), text.size());
        Converter converter{Buffer{text.data(), text.size(), file, baseOffset}, sink};
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
