// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ClipPropertyNames.h"

#include "Catalog.h"
#include "registry/EntityRegistry.h"

#include <algorithm>
#include <cctype>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace{

    std::string clipPropertyLowered(const std::string& text){
        std::string out;
        for (char c : text){
            if (c != ' ' && c != '_') out += (char)std::tolower((unsigned char)c);
        }
        return out;
    }

    // the Catalog type of a component key ("ui" -> UIComponent); false when none
    bool typeOfKey(const std::string& key, ComponentType& out){
        for (int i = 0; i < ComponentTypeCount; i++){
            const ComponentType type = static_cast<ComponentType>(i);
            if (Catalog::getComponentName(type, true) == key){
                out = type;
                return true;
            }
        }
        return false;
    }

    bool rowsOf(const std::string& component){
        for (const anim::AnimatableProperty& row : anim::animatableProperties()){
            if (component == row.component) return true;
        }
        return false;
    }

    // the UI elements whose graphic is the UIComponent (its colour is ui.color)
    const ComponentType uiGraphics[] = {ComponentType::ImageComponent, ComponentType::TextComponent, ComponentType::ButtonComponent,
                                        ComponentType::PanelComponent, ComponentType::PolygonComponent, ComponentType::InputFieldComponent,
                                        ComponentType::ScrollbarComponent, ComponentType::ProgressbarComponent};

}

std::string ClipPropertyNames::groupLabel(EntityRegistry* registry, Entity entity, const std::string& component){
    if (component == "gameobject") return "GameObject";
    if (component.compare(0, 7, "script:") == 0) return component.substr(7) + " (Script)";
    const bool bound = registry && entity != NULL_ENTITY && registry->isEntityCreated(entity);
    if (bound && (component == "ui" || component == "mesh")){
        const std::vector<ComponentType> types = Catalog::findComponents(registry, entity);
        auto has = [&](ComponentType type){ return std::find(types.begin(), types.end(), type) != types.end(); };
        if (component == "ui"){
            for (ComponentType type : uiGraphics){
                if (has(type)) return Catalog::getComponentDisplayName(type);
            }
        }else if (has(ComponentType::SpriteComponent)){
            return Catalog::getComponentDisplayName(ComponentType::SpriteComponent);
        }
    }
    ComponentType type;
    if (typeOfKey(component, type)) return Catalog::getComponentDisplayName(type);
    return component;
}

std::string ClipPropertyNames::propertyLabel(const std::string& component, const std::string& property){
    anim::PropertyRef ref;
    if (!anim::parsePropertyRef(component, property, ref)) return property;
    std::string label;
    if (ref.script){
        label = ref.params.member;
    }else if (ref.entry){
        label = ref.entry->label;
        if (ref.params.submesh >= 0){
            label = (std::string(ref.entry->property).find("textureRect") != std::string::npos ? "Submesh [" : "Material [") +
                    std::to_string(ref.params.submesh) + "] : " + label;
        }
        if (!ref.params.member.empty() && !ref.script) label = (ref.params.submesh >= 0 ? "Material [" + std::to_string(ref.params.submesh) + "] : " : "Shader : ") +
                                                                 ref.params.member;
    }
    if (ref.swizzle >= 0){
        static const char* const colorNames[] = {"Red", "Green", "Blue", "Alpha"};
        static const char* const vectorNames[] = {"x", "y", "z", "w"};
        const bool colour = ref.kind == PropertyKind::Color3 || ref.kind == PropertyKind::Color4 || ref.display == PropertyDisplay::SRGB;
        const char suffix = property.empty() ? 0 : property.back();
        const bool colourLetter = suffix == 'r' || suffix == 'g' || suffix == 'b' || suffix == 'a';
        label += colour || colourLetter ? std::string(" (") + colorNames[ref.swizzle] + ")" : std::string(".") + vectorNames[ref.swizzle];
    }
    return label;
}

std::string ClipPropertyNames::trackLabel(EntityRegistry* registry, Entity entity, const ClipTrackData& track){
    return groupLabel(registry, entity, track.component) + "." + propertyLabel(track.component, track.property);
}

std::string ClipPropertyNames::listProperties(const std::string& component){
    std::string list;
    for (const anim::AnimatableProperty& row : anim::animatableProperties()){
        if (component != row.component) continue;
        list += (list.empty() ? "" : ", ") + std::string(row.property);
    }
    return list;
}

bool ClipPropertyNames::resolve(const std::string& component, const std::string& property, std::string& outComponent,
                                std::string& outProperty, std::string& error){
    outComponent = component;
    outProperty = property;
    error.clear();
    // "<Class> (Script)" (the Inspector's name) is script:<Class>
    const std::string scriptSuffix = " (Script)";
    if (component.size() > scriptSuffix.size() && component.compare(component.size() - scriptSuffix.size(), scriptSuffix.size(), scriptSuffix) == 0){
        outComponent = "script:" + component.substr(0, component.size() - scriptSuffix.size());
    }else if (clipPropertyLowered(component) == "gameobject"){
        outComponent = "gameobject";
    }else if (component.compare(0, 7, "script:") != 0 && !rowsOf(component)){
        // a display name or another spelling of a key ("Image", "Light 2D", "RectTransform")
        const std::string wanted = clipPropertyLowered(component);
        for (int i = 0; i < ComponentTypeCount; i++){
            const ComponentType type = static_cast<ComponentType>(i);
            if (clipPropertyLowered(Catalog::getComponentDisplayName(type)) != wanted && Catalog::getComponentName(type, true) != wanted &&
                clipPropertyLowered(Catalog::getComponentName(type, false)) != wanted) continue;
            const std::string key = Catalog::getComponentName(type, true);
            if (std::find(std::begin(uiGraphics), std::end(uiGraphics), type) != std::end(uiGraphics)){
                // a UI element's graphic: ui (a Button's state colours stay button.*)
                anim::PropertyRef own;
                outComponent = rowsOf(key) && anim::parsePropertyRef(key, property, own) ? key : "ui";
            }else if (type == ComponentType::SpriteComponent){
                // Sprite Renderer : Color is the sprite's mesh material colour
                anim::PropertyRef own;
                if (anim::parsePropertyRef(key, property, own)){
                    outComponent = key;
                }else if (property == "color" || property.compare(0, 6, "color.") == 0){
                    outComponent = "mesh";
                    outProperty = "submeshes[0].material.baseColorFactor" + property.substr(5);
                }else{
                    outComponent = "mesh";
                }
            }else{
                outComponent = key;
            }
            break;
        }
    }
    if (outComponent == Catalog::getComponentName(ComponentType::SpriteComponent, true) &&
        (outProperty == "color" || outProperty.compare(0, 6, "color.") == 0)){
        // the key itself ("sprite" "color", as Lua's Sprite.color): the mesh material colour
        anim::PropertyRef own;
        if (!anim::parsePropertyRef(outComponent, outProperty, own)){
            outComponent = "mesh";
            outProperty = "submeshes[0].material.baseColorFactor" + outProperty.substr(5);
        }
    }
    anim::PropertyRef ref;
    if (anim::parsePropertyRef(outComponent, outProperty, ref)) return true;
    error = ref.error;
    const std::string list = listProperties(outComponent);
    if (!list.empty()) error += "; animatable on '" + outComponent + "': " + list;
    return false;
}
