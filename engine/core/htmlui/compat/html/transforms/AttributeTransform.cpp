// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// §3.7.3 steps 10-11: attributes (style="" for the CSS session, presentational hints,
// JavaScript handlers, dropped attributes), URLs, duplicate ids and data-binding attributes
// outside a data model.

#include "htmlui/compat/html/HtmlTransform.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/SupportTables.h"
#include "htmlui/compat/UrlResolver.h"

#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::htmlui::compat::html;

namespace {

    struct Walk {
        TransformContext& context;
        std::unordered_map<std::string, SourceRef> ids;
    };

    SourceRef whereOf(const HtmlNode& element, const HtmlAttr& attribute){
        return attribute.nameRef.valid() ? attribute.nameRef : element.ref;
    }

    std::string px(double value, bool percent = false){
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "%g%s", value, percent ? "%" : "px");
        return buffer;
    }

    void addHint(HintDeclarations& hints, const std::string& declarations, SourceRef ref, bool cells = false){
        std::string& target = cells ? hints.cells : hints.own;
        if (!target.empty())
            target += ' ';
        target += declarations;
        if (!hints.ref.valid())
            hints.ref = ref;
    }

    bool isBindingAttribute(const std::string& name){
        static const char* const exact[] = {"data-if", "data-visible", "data-for", "data-value", "data-checked", "data-rml"};
        for (const char* candidate : exact){
            if (name == candidate)
                return true;
        }
        static const char* const prefixes[] = {"data-alias-", "data-attr-", "data-attrif-", "data-class-", "data-style-", "data-event-"};
        for (const char* prefix : prefixes){
            if (startsWith(name, prefix))
                return true;
        }
        return false;
    }

    // attributes kept as they are, which need none of the checks below
    bool isPlainAttribute(const std::string& name){
        static const std::unordered_set<std::string> plain = {
            "class", "type", "name", "value", "for", "href", "alt", "placeholder", "min", "max", "step", "maxlength",
            "size", "rows", "cols", "colspan", "rowspan", "selected", "checked", "disabled", "readonly", "required",
            "multiple", "open", "start", "reversed", "role", "label", "hidden",
        };
        return plain.count(name) > 0;
    }

    bool isEventHandler(const std::string& name){
        return name.size() > 2 && name[0] == 'o' && name[1] == 'n';
    }

    // legacy presentational attributes (Appendix D.2) -> hint declarations
    void legacyHint(TransformContext& context, HtmlNode& element, const HtmlAttr& attribute, HintDeclarations& hints){
        const std::string& tag = element.tag;
        const std::string& name = attribute.name;
        const std::string value = toLower(trim(attribute.value));
        const SourceRef where = whereOf(element, attribute);
        double number = 0.0;
        bool percent = false;

        if (name == "align"){
            if (tag == "table" || tag == "hr"){
                if (value == "center")
                    addHint(hints, "margin-left: auto; margin-right: auto;", where);
                else if (value == "right")
                    addHint(hints, "margin-left: auto;", where);
                else if (value == "left")
                    addHint(hints, "margin-right: auto;", where);
            }else if (tag == "img" || tag == "iframe" || tag == "object" || tag == "embed" || tag == "svg"){
                if (value == "left" || value == "right")
                    addHint(hints, "float: " + value + ";", where);
                else if (value == "top" || value == "middle" || value == "bottom" || value == "baseline")
                    addHint(hints, "vertical-align: " + value + ";", where);
            }else if (value == "left" || value == "right" || value == "center" || value == "justify" || value == "middle"){
                addHint(hints, "text-align: " + std::string(value == "middle" ? "center" : value) + ";", where);
            }
        }else if (name == "bgcolor"){
            const std::string color = legacyColor(attribute.value);
            if (!color.empty())
                addHint(hints, "background-color: " + color + ";", where);
        }else if (name == "valign"){
            if (value == "top" || value == "middle" || value == "bottom" || value == "baseline")
                addHint(hints, "vertical-align: " + value + ";", where);
        }else if (name == "border" && (tag == "table" || tag == "img")){
            const double width = value.empty() ? 1.0 : std::strtod(value.c_str(), nullptr);
            if (width > 0.0){
                if (tag == "table"){
                    addHint(hints, "border-width: " + px(width) + "; border-color: #808080ff;", where);
                    addHint(hints, "border-width: 1px; border-color: #808080ff;", where, true);
                }else{
                    addHint(hints, "border-width: " + px(width) + "; border-color: var(--html-current-color, #000000ff);", where);
                }
            }
        }else if (name == "cellpadding" && tag == "table"){
            if (parseDimension(value, number, percent))
                addHint(hints, "padding: " + px(number, percent) + ";", where, true);
        }else if (name == "cellspacing" && tag == "table"){
            if (parseDimension(value, number, percent))
                addHint(hints, "gap: " + px(number) + ";", where);
        }else if ((name == "width" || name == "height") && (tag == "table" || tag == "td" || tag == "th" || tag == "col" || tag == "hr")){
            if (parseDimension(value, number, percent) && number > 0.0)
                addHint(hints, name + ": " + px(number, percent) + ";", where);
        }
        (void)context;
    }

    void resolveSource(TransformContext& context, HtmlNode& element, const char* kind){
        HtmlAttr* src = element.findAttr("src");
        if (!src)
            return;
        const std::string value = trim(src->value);
        if (value.empty()){
            element.removeAttr("src");
            return;
        }
        const std::string emitted = context.resolveUrl(value, kind, src->valueRef.valid() ? src->valueRef : element.ref);
        if (emitted.empty()){
            element.removeAttr("src");   // the fallback of §3.13.6: nothing is drawn
        }else{
            src->value = emitted;
        }
    }

    void checkLink(TransformContext& context, HtmlNode& anchor){
        const HtmlAttr* href = anchor.findAttr("href");
        if (!href)
            return;
        const std::string value = trim(href->value);
        if (value.empty())
            return;
        const SourceRef where = href->valueRef.valid() ? href->valueRef : anchor.ref;
        const UrlKind kind = UrlResolver::classify(value);
        if (kind == UrlKind::Remote || kind == UrlKind::OtherScheme){
            context.sink.report(codes::LinkNotNavigable, where,
                                "link '" + value + "' only dispatches the htmlnavigate event (there is no browser to open it)",
                                "handle htmlnavigate in Lua (System:openURL is not available)", "", value);
        }else if (kind == UrlKind::Relative || kind == UrlKind::RootRelative){
            std::string path = value;
            const size_t cut = path.find_first_of("?#");
            if (cut != std::string::npos)
                path = path.substr(0, cut);
            const std::string lower = toLower(path);
            if (!path.empty() && !endsWith(lower, ".html") && !endsWith(lower, ".htm") && !endsWith(lower, ".rml")){
                context.sink.report(codes::LinkNotNavigable, where,
                                    "link '" + value + "' does not point at an HTML document: it only dispatches htmlnavigate",
                                    "", "", value);
            }
        }
    }

    void processElement(Walk& walk, HtmlNode& element, bool svgRoot, bool inModel){
        TransformContext& context = walk.context;
        HintDeclarations hints;

        // by index: the loop removes some (removeAttr keeps the order of the others)
        for (size_t index = 0; index < element.attrs.size(); index++){
            HtmlAttr* attribute = &element.attrs[index];
            // most attributes are kept as written: no table lookup for them
            if (isPlainAttribute(attribute->name))
                continue;
            const std::string name = attribute->name;
            const size_t count = element.attrs.size();
            const SourceRef where = whereOf(element, *attribute);
            // what the body below removed: the next attribute now sits at index
            struct Rewind {
                const HtmlNode& element; size_t& index; size_t count;
                ~Rewind(){ if (element.attrs.size() < count) index--; }
            } rewind{element, index, count};

            if (name == "style"){
                if (!trim(attribute->value).empty()){
                    StyleAttributeSource source;
                    source.element = &element;
                    source.text = attribute->value;
                    source.baseOffset = attribute->valueRef.valid() ? attribute->valueRef.offset : 0;
                    context.styleAttributes.push_back(std::move(source));
                }
                continue;
            }
            if (isEventHandler(name)){
                const std::string event = name.substr(2);
                context.sink.report(codes::EventHandlerAttribute, where,
                                    "'" + name + "' (JavaScript) is not run",
                                    "use element:addEventListener('" + event + "', fn) in Lua, or data-event-" + event + " with a data model",
                                    "", name);
                element.removeAttr(name);
                continue;
            }
            if (name == "id"){
                const std::string id = attribute->value;
                if (!id.empty() && id != DocumentLoader::kRootId){
                    auto found = walk.ids.find(id);
                    if (found == walk.ids.end()){
                        walk.ids.emplace(id, element.ref);
                    }else{
                        const SourcePos first = found->second.valid() ? context.files.position(found->second.file, found->second.offset) : SourcePos{};
                        context.sink.report(codes::DuplicateId, where,
                                            "id '" + id + "' is used more than once" + (first.line ? " (first on line " + std::to_string(first.line) + ")" : std::string()) +
                                            "; getElementById and #" + id + " find the first",
                                            "give every element its own id", "", id);
                    }
                }
                continue;
            }
            if (name == "tabindex"){
                const std::string value = trim(attribute->value);
                char* end = nullptr;
                const long index = std::strtol(value.c_str(), &end, 10);
                if (value.empty() || end == value.c_str())
                    continue;
                if (index < 0){
                    addHint(hints, "tab-index: none; focus: auto;", where);
                }else{
                    addHint(hints, "tab-index: auto;", where);
                    if (index > 0){
                        context.sink.report(codes::TabindexOrder, where,
                                            "tabindex=\"" + value + "\": the Tab order follows the document, positive values are not ordered",
                                            "order the elements in the document, use tabindex=\"0\"", "0", "tabindex");
                    }
                }
                continue;
            }
            if (name == "dir"){
                const std::string value = toLower(trim(attribute->value));
                if (value == "rtl" || value == "ltr" || value == "auto")
                    addHint(hints, "-rmlui-direction: " + value + ";", where);
                continue;
            }
            if (name == "lang"){
                std::string value;
                for (char c : trim(attribute->value)){
                    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
                        value += c;
                }
                if (!value.empty())
                    addHint(hints, "-rmlui-language: " + value + ";", where);
                continue;
            }
            if (name == "title" && !svgRoot){
                context.sink.report(codes::TitleTooltip, where, "title tooltips are not shown", "show a tooltip element from Lua on hover", "", "title");
                continue;
            }
            if (name == "draggable"){
                if (equalsIgnoreCase(trim(attribute->value), "true")){
                    addHint(hints, "drag: drag;", where);
                    context.sink.reportAs(Severity::Info, codes::UnsupportedAttribute, where,
                                          "draggable maps to the engine's drag events (dragstart, drag, dragend); there is no HTML5 drop data",
                                          "", "", "draggable");
                }
                continue;
            }
            if (name == "align" || name == "bgcolor" || name == "valign" || name == "border" || name == "cellpadding" ||
                name == "cellspacing" || name == "width" || name == "height"){
                legacyHint(context, element, *attribute, hints);
                continue;
            }
            if (!inModel && name.compare(0, 5, "data-") == 0 && isBindingAttribute(name)){
                context.sink.report(codes::BindingAttributeOutsideModel, where,
                                    "'" + name + "' is read by the engine as a data binding, but no data-model encloses it",
                                    "put it inside an element with data-model=\"name\", or rename the attribute", "", name);
                continue;
            }
            if (const AttributeSupport* row = attributeRow(element.tag, name)){
                if (row->support == Support::Unsupported && (name == "contenteditable" || startsWith(name, "popover"))){
                    context.sink.reportAs(Severity::Warning, codes::UnsupportedAttribute, where,
                                          "'" + name + "' is not supported and is dropped", row->suggestion ? row->suggestion : "", "", name);
                    element.removeAttr(name);
                }else if (row->support == Support::Ignored){
                    context.sink.reportAs(Severity::Info, codes::UnsupportedAttribute, where,
                                          "'" + name + "' has no effect in the engine and is dropped", "", "", name);
                    element.removeAttr(name);
                }
            }
        }

        // URLs (step 11)
        if (element.ns == Namespace::Html){
            if (element.tag == "img" || (element.tag == "input" && equalsIgnoreCase(trim(element.attr("type")), "image")))
                resolveSource(context, element, "image");
            else if (element.tag == "svg")
                resolveSource(context, element, "svg");
            else if (element.tag == "lottie")
                resolveSource(context, element, "lottie");
            else if (element.tag == "a")
                checkLink(context, element);
        }

        if (!hints.empty())
            context.addHints(element, hints);
    }

    void walkTree(Walk& walk, HtmlNode& node, bool inModel){
        for (std::unique_ptr<HtmlNode>& owned : node.children){
            HtmlNode& element = *owned;
            if (element.kind != HtmlNode::Kind::Element)
                continue;
            const bool model = inModel || element.hasAttr("data-model");
            if (element.ns == Namespace::Svg){
                if (element.tag == "svg" && node.ns != Namespace::Svg)
                    processElement(walk, element, true, model);
                // inside the SVG: no JavaScript handlers either
                std::vector<HtmlNode*> stack{&element};
                while (!stack.empty()){
                    HtmlNode* current = stack.back();
                    stack.pop_back();
                    for (std::unique_ptr<HtmlNode>& child : current->children){
                        if (child->kind != HtmlNode::Kind::Element)
                            continue;
                        for (size_t i = 0; i < child->attrs.size();){
                            if (isEventHandler(child->attrs[i].name)){
                                walk.context.sink.report(codes::EventHandlerAttribute, whereOf(*child, child->attrs[i]),
                                                         "'" + child->attrs[i].name + "' (JavaScript) is not run", "", "", child->attrs[i].name);
                                child->attrs.erase(child->attrs.begin() + (std::ptrdiff_t)i);
                            }else{
                                i++;
                            }
                        }
                        stack.push_back(child.get());
                    }
                }
                continue;
            }
            if (element.ns != Namespace::Html)
                continue;
            processElement(walk, element, false, model);
            walkTree(walk, element, model);
        }
    }

}

void jasperengine::htmlui::compat::transformAttributes(TransformContext& context, HtmlNode& tree){
    Walk walk{context, {}};
    // the document root itself (lang, dir, classes); not "#content"/"#fragment"
    if (tree.kind == HtmlNode::Kind::Element && tree.tag != "#content" && tree.tag != "#fragment")
        processElement(walk, tree, false, tree.hasAttr("data-model"));
    walkTree(walk, tree, tree.hasAttr("data-model"));
}
