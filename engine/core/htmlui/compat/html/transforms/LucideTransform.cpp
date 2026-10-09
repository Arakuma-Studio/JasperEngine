// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// §3.7.3 step 7: elements with data-lucide="name" become the inline SVG of that icon, exactly
// as lucide.createIcons() replaces them. The icons come from the engine's built-in pack
// (runtime/LucideIcons, engine/libs/lucide/lucide.pack, p2-runtime).

#include "htmlui/compat/html/HtmlTransform.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/Diagnostics.h"

#include "htmlui/compat/runtime/LucideIcons.h"

#include <algorithm>

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::htmlui::compat::html;

bool jasperengine::htmlui::compat::lucideAvailable(){
    return LucideIcons::available();
}

bool jasperengine::htmlui::compat::findLucideIcon(std::string_view name, LucideIcon& icon){
    // kebab-case, aliases, PascalCase and camelCase names (lucide matches by toPascalCase)
    const std::string canonical = LucideIcons::resolve(trim(name));
    if (canonical.empty())
        return false;
    std::optional<std::string_view> shapes = LucideIcons::find(canonical);
    if (!shapes)
        return false;
    icon.name = canonical;
    icon.shapes = std::string(*shapes);
    return true;
}

std::vector<std::string> jasperengine::htmlui::compat::closestLucideIcons(std::string_view name, size_t count){
    return LucideIcons::closest(trim(name), count);
}

std::vector<std::pair<std::string, std::string>> jasperengine::htmlui::compat::lucideAttributes(
        const std::vector<std::pair<std::string, std::string>>& attributes, const std::string& iconName){
    std::vector<LucideAttribute> element;
    element.reserve(attributes.size());
    for (const auto& pair : attributes)
        element.push_back(LucideAttribute{pair.first, pair.second});
    std::vector<std::pair<std::string, std::string>> out;
    for (const LucideAttribute& attribute : LucideIcons::svgAttributes(iconName, element))
        out.emplace_back(attribute.name, attribute.value);
    return out;
}

namespace {

    void expandIcons(TransformContext& context, HtmlNode& node){
        for (size_t i = 0; i < node.children.size(); i++){
            HtmlNode& element = *node.children[i];
            if (element.kind != HtmlNode::Kind::Element)
                continue;
            const HtmlAttr* nameAttr = element.findAttr("data-lucide");
            if (!nameAttr || trim(nameAttr->value).empty() || element.ns != Namespace::Html){
                if (element.ns == Namespace::Html)
                    expandIcons(context, element);
                continue;
            }

            const std::string iconName = trim(nameAttr->value);
            const SourceRef where = nameAttr->valueRef.valid() ? nameAttr->valueRef : element.ref;
            LucideIcon icon;
            if (!findLucideIcon(iconName, icon)){
                std::string suggestion;
                if (lucideAvailable()){
                    for (const std::string& closest : closestLucideIcons(iconName, 3))
                        suggestion += (suggestion.empty() ? "" : ", ") + closest;
                    context.sink.report(codes::UnknownLucideIcon, where, "Lucide icon '" + iconName + "' does not exist; the element stays empty",
                                        suggestion.empty() ? "" : "did you mean: " + suggestion, "", iconName);
                }else{
                    context.sink.report(codes::UnknownLucideIcon, where, "Lucide icon '" + iconName + "' is not drawn: this build has no Lucide icon pack",
                                        "", "", iconName);
                }
                element.children.clear();
                continue;
            }

            std::vector<std::pair<std::string, std::string>> attributes;
            for (const HtmlAttr& attribute : element.attrs)
                attributes.emplace_back(attribute.name, attribute.value);

            std::unique_ptr<HtmlNode> svg = HtmlNode::makeElement("svg", Namespace::Svg);
            svg->implied = element.implied;
            svg->ref = element.ref;
            for (const auto& pair : lucideAttributes(attributes, iconName)){
                HtmlAttr attribute;
                attribute.name = pair.first;
                attribute.value = pair.second;
                attribute.hasValue = true;
                if (const HtmlAttr* original = element.findAttr(pair.first)){
                    attribute.nameRef = original->nameRef;
                    attribute.valueRef = (original->value == pair.second) ? original->valueRef : SourceRef{};
                }
                svg->attrs.push_back(std::move(attribute));
            }
            svg->appendChild(HtmlNode::makeRaw(icon.shapes));
            svg->parent = &node;
            node.children[i] = std::move(svg);
        }
    }

}

void jasperengine::htmlui::compat::transformLucide(TransformContext& context, HtmlNode& tree){
    expandIcons(context, tree);
}
