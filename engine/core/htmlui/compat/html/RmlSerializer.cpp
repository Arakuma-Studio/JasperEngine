// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/html/RmlSerializer.h"

#include "htmlui/compat/DocumentLoader.h"

using namespace jasperengine::htmlui::compat;

namespace {

    bool isWhitespaceOnly(const std::string& text){
        for (char c : text){
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f')
                return false;
        }
        return true;
    }

    // whitespace is significant: RmlUi drops whitespace-only text nodes, so they are written
    // as character references there (which it keeps)
    bool isPreformatted(const HtmlNode* node){
        for (const HtmlNode* walk = node; walk; walk = walk->parent){
            if (walk->kind == HtmlNode::Kind::Element && walk->ns == Namespace::Html &&
                (walk->tag == "pre" || walk->tag == "textarea" || walk->tag == "listing" || walk->tag == "xmp" || walk->tag == "plaintext"))
                return true;
        }
        return false;
    }

    // XML names: what the HTML parser accepts in a name and XML does not ("<b<", "a\"b") is
    // replaced (the parse error was reported); "" when nothing is left
    bool isNameCharacter(unsigned char c, bool first){
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' || c >= 0x80)
            return true;
        return !first && ((c >= '0' && c <= '9') || c == '-' || c == '.');
    }

    std::string xmlName(const std::string& name){
        std::string out;
        for (size_t i = 0; i < name.size(); i++){
            const unsigned char c = (unsigned char)name[i];
            out += isNameCharacter(c, out.empty()) ? (char)c : (out.empty() ? 'x' : '-');
        }
        return out;
    }

    std::string referencesFor(const std::string& whitespace){
        std::string out;
        for (char c : whitespace){
            switch (c){
                case '\n': out += "&#10;"; break;
                case '\t': out += "&#9;"; break;
                case '\r': out += "&#13;"; break;
                case '\f': out += "&#12;"; break;
                default: out += "&#32;"; break;
            }
        }
        return out;
    }

    struct Writer {
        std::string out;
        std::vector<SourceRef> lineRefs{SourceRef{}};
        const HtmlNode* root = nullptr;
        const HtmlNode* body = nullptr;

        uint32_t line() const { return (uint32_t)lineRefs.size(); }

        // text that may contain line breaks: each new line maps to `ref`
        void append(const std::string& text, SourceRef ref){
            out += text;
            for (size_t at = text.find('\n'); at != std::string::npos; at = text.find('\n', at + 1))
                lineRefs.push_back(ref);
        }

        void newline(SourceRef ref){
            out += '\n';
            lineRefs.push_back(ref);
        }

        bool afterTag() const{
            return out.empty() || out.back() == '>' || out.back() == '\n';
        }

        std::string nameOf(const HtmlNode& node) const{
            if (&node == root)
                return "body";
            if (&node == body)
                return "htmlbody";
            return xmlName(node.tag);
        }

        void writeAttributes(const HtmlNode& node){
            for (const HtmlAttr& attribute : node.attrs){
                if (attribute.name == "style" && node.hasRewrittenStyle)
                    continue;
                const std::string name = xmlName(attribute.name);
                if (name != attribute.name)
                    continue;   // not an XML name: dropped (HTML-0001 reported the parse error)
                out += ' ';
                out += attribute.name;
                out += "=\"";
                out += RmlSerializer::escapeAttribute(attribute.hasValue ? attribute.value : std::string());
                out += '"';
            }
            if (node.hasRewrittenStyle && !node.rewrittenStyle.empty()){
                out += " style=\"";
                out += RmlSerializer::escapeAttribute(node.rewrittenStyle);
                out += '"';
            }
            if (node.hasLoc){
                out += ' ';
                out += DocumentLoader::kLocationAttribute;
                out += "=\"";
                out += std::to_string(node.loc);
                out += '"';
            }
        }

        // svg: inside an inline SVG (RmlUi reads it as raw text): no line breaks are added
        void writeNode(const HtmlNode& node, bool svg){
            switch (node.kind){
                case HtmlNode::Kind::Text: {
                    if (isWhitespaceOnly(node.text) && !svg && isPreformatted(node.parent)){
                        out += referencesFor(node.text);
                    }else{
                        append(RmlSerializer::escapeText(node.text), node.location());
                    }
                    return;
                }
                case HtmlNode::Kind::Raw:
                    append(node.text, node.location());
                    return;
                case HtmlNode::Kind::Comment:
                case HtmlNode::Kind::Doctype:
                    return;
                case HtmlNode::Kind::Element:
                    break;
            }

            const SourceRef location = node.location();
            if (!svg && afterTag() && !out.empty() && out.back() != '\n')
                newline(location);
            else if (!svg && !out.empty() && out.back() == '\n')
                lineRefs.back() = location;

            const std::string name = nameOf(node);
            out += '<';
            out += name;
            writeAttributes(node);

            bool hasContent = false;
            for (const std::unique_ptr<HtmlNode>& child : node.children){
                if (child->kind != HtmlNode::Kind::Comment && child->kind != HtmlNode::Kind::Doctype){
                    hasContent = true;
                    break;
                }
            }
            if (!hasContent){
                out += "/>";
                return;
            }
            out += '>';
            const bool childrenSvg = svg || node.ns == Namespace::Svg;
            for (const std::unique_ptr<HtmlNode>& child : node.children)
                writeNode(*child, childrenSvg);
            out += "</";
            out += name;
            out += '>';
        }
    };

}

std::string RmlSerializer::escapeText(const std::string& text){
    if (text.find_first_of("&<>") == std::string::npos)
        return text;
    std::string out;
    out.reserve(text.size());
    for (char c : text){
        switch (c){
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string RmlSerializer::escapeAttribute(const std::string& value){
    if (value.find_first_of("&<>\"\n\r\t") == std::string::npos)
        return value;
    std::string out;
    out.reserve(value.size());
    for (char c : value){
        switch (c){
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            // kept on one line, so the RML line count stays the serializer's
            case '\n': out += "&#10;"; break;
            case '\r': out += "&#13;"; break;
            case '\t': out += "&#9;"; break;
            default: out += c; break;
        }
    }
    return out;
}

RmlSerializer::Output RmlSerializer::writeDocument(Document& document){
    Writer writer;
    writer.root = document.root;
    writer.body = document.body;

    writer.out += "<rml>";
    writer.newline(SourceRef{});
    writer.out += "<head>";
    if (!document.title.empty()){
        writer.newline(SourceRef{});
        writer.out += "<title>" + escapeText(document.title) + "</title>";
    }
    for (HeadItem& item : document.head){
        writer.newline(item.ref);
        if (item.kind == HeadItem::Kind::Link){
            writer.out += "<link type=\"text/rcss\" href=\"" + escapeAttribute(item.href) + "\"" + item.attributes + "/>";
            continue;
        }
        // RmlUi numbers an inline sheet from the line of its <style> tag: the RCSS starts on
        // the next one
        writer.out += "<style>";
        std::string rcss = item.rcss;
        // "</style" would end the raw text early; "<\/" is the same in CSS
        for (size_t at = rcss.find("</"); at != std::string::npos; at = rcss.find("</", at + 3))
            rcss.replace(at, 2, "<\\/");
        writer.newline(item.ref);
        item.contentLine = writer.line();
        writer.append(rcss, item.ref);
        if (writer.out.back() != '\n')
            writer.newline(item.ref);
        writer.out += "</style>";
    }
    writer.newline(SourceRef{});
    writer.out += "</head>";

    if (document.root)
        writer.writeNode(*document.root, false);

    writer.newline(SourceRef{});
    writer.out += "</rml>";
    writer.newline(SourceRef{});

    Output output;
    output.rml = std::move(writer.out);
    output.lineRefs = std::move(writer.lineRefs);
    return output;
}

RmlSerializer::Output RmlSerializer::writeFragment(const HtmlNode& container){
    Writer writer;
    for (const std::unique_ptr<HtmlNode>& child : container.children)
        writer.writeNode(*child, container.ns == Namespace::Svg);
    Output output;
    output.rml = std::move(writer.out);
    output.lineRefs = std::move(writer.lineRefs);
    return output;
}

std::string RmlSerializer::writeElement(const HtmlNode& element){
    Writer writer;
    writer.writeNode(element, true);
    return std::move(writer.out);
}
