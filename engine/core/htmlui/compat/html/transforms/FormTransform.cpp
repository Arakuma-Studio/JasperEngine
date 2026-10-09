// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// §3.7.3 step 9: forms and inputs. The HTML `type` stays as written (input[type=email]
// selectors keep matching); the runtime's HtmlInputElement maps it to an RmlUi type.

#include "htmlui/compat/html/HtmlTransform.h"

#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/SupportTables.h"

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::htmlui::compat::html;

namespace {

    bool isControl(const HtmlNode& element){
        return element.is("input") || element.is("button") || element.is("select") || element.is("textarea") ||
               element.is("fieldset") || element.is("optgroup") || element.is("option");
    }

    // HTML: a disabled fieldset disables every descendant control except those inside its
    // first legend
    size_t disableDescendants(HtmlNode& node, const HtmlNode* firstLegend){
        size_t count = 0;
        for (std::unique_ptr<HtmlNode>& child : node.children){
            if (child->kind != HtmlNode::Kind::Element || child.get() == firstLegend)
                continue;
            if (isControl(*child) && !child->is("option") && !child->hasAttr("disabled")){
                child->setAttr("disabled", "").hasValue = false;
                count++;
            }
            count += disableDescendants(*child, nullptr);
        }
        return count;
    }

    void transformInput(TransformContext& context, HtmlNode& input){
        const HtmlAttr* typeAttr = input.findAttr("type");
        const std::string type = toLower(trim(input.attr("type")));
        if (type.empty())
            return;
        const SourceRef where = (typeAttr && typeAttr->valueRef.valid()) ? typeAttr->valueRef : input.ref;
        const InputTypeSupport* row = findInputType(type);
        if (!row){
            context.sink.reportAs(Severity::Info, codes::InputTypeApproximated, where,
                                  "unknown input type '" + type + "' is a text input, as in browsers", "use a standard type", "", type);
            return;
        }
        if (!row->reported)
            return;
        std::string message = "input type '" + type + "': ";
        message += row->note ? row->note : "approximated";
        context.sink.reportAs(row->severity, codes::InputTypeApproximated, where, message, row->suggestion ? row->suggestion : "", "", type);
    }

    void transformForm(TransformContext& context, HtmlNode& form){
        std::string dropped;
        SourceRef first;
        for (const char* name : {"action", "method", "target", "enctype", "novalidate", "accept-charset"}){
            const HtmlAttr* attribute = form.findAttr(name);
            if (!attribute)
                continue;
            if (!first.valid())
                first = attribute->nameRef;
            dropped += std::string(dropped.empty() ? "" : ", ") + name;
            form.removeAttr(name);
        }
        if (!dropped.empty()){
            context.sink.report(codes::FormNavigation, first.valid() ? first : form.ref,
                                "form " + dropped + " ignored: submitting dispatches the submit event with the values, nothing is sent",
                                "handle the submit event in Lua (form:addEventListener('submit', fn))", "", "form");
        }
    }

    void walk(TransformContext& context, HtmlNode& node){
        for (std::unique_ptr<HtmlNode>& owned : node.children){
            HtmlNode& element = *owned;
            if (element.kind != HtmlNode::Kind::Element || element.ns != Namespace::Html)
                continue;

            if (element.tag == "input"){
                transformInput(context, element);
            }else if (element.tag == "form"){
                transformForm(context, element);
            }else if (element.tag == "fieldset" && element.hasAttr("disabled")){
                const HtmlNode* legend = nullptr;
                for (const std::unique_ptr<HtmlNode>& child : element.children){
                    if (child->is("legend")){
                        legend = child.get();
                        break;
                    }
                }
                if (disableDescendants(element, legend) > 0){
                    context.sink.report(codes::FieldsetDisabled, element.ref,
                                        "the disabled fieldset disables its controls (disabled added to each, as browsers do)", "", "", "fieldset");
                }
            }

            const bool textControl = element.tag == std::string_view("input") || element.tag == std::string_view("textarea");
            for (const char* name : {"pattern", "minlength"}){
                const HtmlAttr* attribute = textControl ? element.findAttr(name) : nullptr;
                if (attribute){
                    context.sink.reportAs(Severity::Info, codes::UnsupportedAttribute, attribute->nameRef.valid() ? attribute->nameRef : element.ref,
                                          std::string("'") + name + "' is not checked: there is no constraint validation", "validate the value from Lua", "", name);
                }
            }
            walk(context, element);
        }
    }

}

void jasperengine::htmlui::compat::transformForms(TransformContext& context, HtmlNode& tree){
    walk(context, tree);
}
