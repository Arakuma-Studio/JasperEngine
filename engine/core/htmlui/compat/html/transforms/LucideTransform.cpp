// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// §3.7.3 step 7: elements with data-lucide="name" become the inline SVG of that icon, exactly
// as lucide.createIcons() replaces them. The icons come from the engine's built-in pack
// (runtime/LucideIcons, engine/libs/lucide/lucide.pack, p2-runtime).

#include "htmlui/compat/html/HtmlTransform.h"

#include "htmlui/compat/Diagnostics.h"

#if __has_include("htmlui/compat/runtime/LucideIcons.h")
#include "htmlui/compat/runtime/LucideIcons.h"
#define HTMLUI_COMPAT_HAS_LUCIDE 1
#else
#define HTMLUI_COMPAT_HAS_LUCIDE 0
#endif

#include <algorithm>

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::htmlui::compat::html;

bool jasperengine::htmlui::compat::lucideAvailable(){
    return HTMLUI_COMPAT_HAS_LUCIDE != 0;
}

bool jasperengine::htmlui::compat::findLucideIcon(std::string_view name, LucideIcon& icon){
#if HTMLUI_COMPAT_HAS_LUCIDE
    const std::string written = trim(name);
    if (written.empty())
        return false;
    // kebab-case as written, then the name a camelCase/PascalCase spelling stands for
    // (lucide matches by toPascalCase(name), so "ArrowRight", "arrowRight" and "arrow-right" agree)
    for (const std::string& candidate : {toLower(written), kebabCase(written)}){
        auto found = LucideIcons::find(candidate);
        if (found){
            icon.name = candidate;
            icon.shapes = std::string(*found);
            return true;
        }
    }
    return false;
#else
    (void)name;
    (void)icon;
    return false;
#endif
}

std::vector<std::string> jasperengine::htmlui::compat::closestLucideIcons(std::string_view name, size_t count){
    std::vector<std::string> names;
#if HTMLUI_COMPAT_HAS_LUCIDE
    for (const auto& closest : LucideIcons::closest(kebabCase(trim(name)), count))
        names.emplace_back(closest);
#else
    (void)name;
    (void)count;
#endif
    return names;
}

std::vector<std::pair<std::string, std::string>> jasperengine::htmlui::compat::lucideAttributes(
        const std::vector<std::pair<std::string, std::string>>& attributes, const std::string& iconName){
    std::vector<std::pair<std::string, std::string>> out = {
        {"xmlns", "http://www.w3.org/2000/svg"}, {"width", "24"}, {"height", "24"}, {"viewBox", "0 0 24 24"},
        {"fill", "none"}, {"stroke", "currentColor"}, {"stroke-width", "2"}, {"stroke-linecap", "round"},
        {"stroke-linejoin", "round"}, {"data-lucide", iconName},
    };
    auto set = [&out](const std::string& name, const std::string& value){
        for (auto& pair : out){
            if (pair.first == name){
                pair.second = value;
                return;
            }
        }
        out.emplace_back(name, value);
    };

    std::string elementClasses;
    bool accessible = false;
    for (const auto& attribute : attributes){
        set(attribute.first, attribute.second);
        if (attribute.first == "class")
            elementClasses = attribute.second;
        if (attribute.first == "role" || startsWith(attribute.first, "aria-"))
            accessible = true;
    }

    // combineClassNames: "lucide lucide-<name>" then the element's, without duplicates
    std::vector<std::string> classes = {"lucide", "lucide-" + iconName};
    for (const std::string& name : splitWhitespace(elementClasses)){
        if (std::find(classes.begin(), classes.end(), name) == classes.end())
            classes.push_back(name);
    }
    std::string classText;
    for (const std::string& name : classes)
        classText += (classText.empty() ? "" : " ") + name;
    set("class", classText);

    if (!accessible)
        set("aria-hidden", "true");
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
