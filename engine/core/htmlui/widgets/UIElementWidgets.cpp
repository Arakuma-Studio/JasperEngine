// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// UIElement and UIDocument members of the S17 widgets: Markdown and tooltips (the views are in
// UIItemsView.cpp).

#include "UIElement.h"
#include "UIElementInternal.h"
#include "UIDocument.h"

#include "htmlui/widgets/MarkdownElement.h"
#include "htmlui/widgets/Markdown/MarkdownHtml.h"
#include "htmlui/widgets/TooltipManager.h"
#include "subsystem/UIDocumentSystem.h"
#include "Scene.h"
#include "Log.h"

#include <RmlUi/Core/ElementDocument.h>

using namespace jasperengine;
using namespace jasperengine::htmlui;

namespace {

    TooltipSpec specOf(const UITooltipOptions& options){
        TooltipSpec spec;
        if (options.placement == "auto"){
            spec.automatic = true;
        }else if (!parsePopupSide(options.placement.c_str(), spec.placement)){
            Log::warn("[HTML UI] setTooltip: unknown placement '%s' (top, bottom, left, right, auto); top is used", options.placement.c_str());
            spec.placement = PopupSide::Top;
        }
        if (options.delay)
            spec.delay = (double)*options.delay;
        spec.arrow = options.arrow;
        spec.interactive = options.interactive;
        return spec;
    }

    Rml::ElementDocument* documentOf(Scene* scene, Entity entity){
        return scene ? scene->getSystem<UIDocumentSystem>()->getDocument(entity) : nullptr;
    }

}

void UIElement::setMarkdown(const std::string& markdown){
    Rml::Element* element = get();
    if (!element){
        Log::warn("[HTML UI] setMarkdown: the element is gone");
        return;
    }
    MarkdownElement::setMarkdown(element, markdown);
}

void UIElement::setTooltip(const std::string& text, const UITooltipOptions& options){
    if (Rml::Element* element = get()){
        TooltipSpec spec = specOf(options);
        spec.text = text;
        TooltipManager::setTooltip(element, std::move(spec));
    }
}

void UIElement::setTooltip(const UIElement& content, const UITooltipOptions& options){
    Rml::Element* element = get();
    Rml::Element* from = content.get();
    if (element && from){
        TooltipSpec spec = specOf(options);
        spec.element = from->GetObserverPtr();
        TooltipManager::setTooltip(element, std::move(spec));
    }
}

void UIElement::setTooltip(std::function<std::string()> text, const UITooltipOptions& options){
    Rml::Element* element = get();
    if (!element || !text)
        return;
    TooltipSpec spec = specOf(options);
    spec.content = [text = std::move(text)](){
        TooltipContent content;
        content.text = text();
        content.none = content.text.empty();
        return content;
    };
    TooltipManager::setTooltip(element, std::move(spec));
}

void UIElement::clearTooltip(){
    if (Rml::Element* element = get())
        TooltipManager::clearTooltip(element);
}

void UIDocument::configureTooltips(const UITooltipConfig& config){
    TooltipConfig value;
    value.enabled = config.enabled;
    value.delay = std::max(0.0f, config.delay);
    value.hopWindow = std::max(0.0f, config.hopWindow);
    TooltipManager::configure(documentOf(scene, entity), value);
}

UITooltipConfig UIDocument::getTooltipConfig() const{
    const TooltipConfig config = TooltipManager::configOf(documentOf(scene, entity));
    return UITooltipConfig{config.enabled, (float)config.delay, (float)config.hopWindow};
}

UITooltipInfo UIDocument::getTooltip() const{
    const TooltipState state = TooltipManager::stateOf(documentOf(scene, entity));
    UITooltipInfo info;
    info.visible = state.visible;
    info.source = UIElementAccess::wrap(state.source);
    info.tooltip = UIElementAccess::wrap(state.tooltip);
    info.text = state.text;
    info.rect = UIRect{state.x, state.y, state.width, state.height, state.y, state.x, state.x + state.width, state.y + state.height};
    info.side = state.side;
    return info;
}

bool UIDocument::showTooltip(const UIElement& source){
    return TooltipManager::show(UIElementAccess::get(source));
}

void UIDocument::hideTooltip(){
    TooltipManager::hide(documentOf(scene, entity));
}

std::string UIDocument::markdownToHtml(const std::string& markdown){
    return markdown::renderToRml(markdown).rml;
}
