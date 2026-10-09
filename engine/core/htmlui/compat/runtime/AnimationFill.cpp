// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/runtime/AnimationFill.h"

#include "htmlui/compat/runtime/CompatProperties.h"

#include <RmlUi/Core/Animation.h>
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/StyleSheet.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <RmlUi/Core/StyleSheetTypes.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace jasperengine::htmlui::compat;

namespace {

    // the element's animation list as text, to notice a change
    std::string animationKey(Rml::Element* element){
        const Rml::Property* property = element->GetProperty(Rml::PropertyId::Animation);
        if (!property || property->unit != Rml::Unit::ANIMATION)
            return std::string();
        std::string key;
        for (const Rml::Animation& animation : property->Get<Rml::AnimationList>()){
            key += animation.name + "|" + std::to_string(animation.duration) + "|" + std::to_string(animation.delay) + "|" +
                   std::to_string(animation.num_iterations) + "|" + (animation.alternate ? "a" : "") + (animation.paused ? "p" : "") + ";";
        }
        return key;
    }

    struct Filled {
        Rml::ObserverPtr<Rml::Element> element;
        Rml::PropertyId id;
        Rml::Property value;
        std::string animation;
    };

    std::vector<Filled>& filled(){
        static std::vector<Filled> list;
        return list;
    }

    // The end value of `id` in the element's animations: the last animation of the list that
    // animates it wins (as in CSS); its last keyframe holding the property, or the first one
    // for an alternate animation that ends going backwards (even iteration count)
    const Rml::Property* endValue(Rml::Element* element, Rml::PropertyId id){
        const Rml::Property* list = element->GetProperty(Rml::PropertyId::Animation);
        const Rml::StyleSheet* sheet = element->GetStyleSheet();
        if (!list || list->unit != Rml::Unit::ANIMATION || !sheet)
            return nullptr;
        const Rml::AnimationList& animations = list->Get<Rml::AnimationList>();
        for (auto it = animations.rbegin(); it != animations.rend(); ++it){
            const Rml::Keyframes* keyframes = sheet->GetKeyframes(it->name);
            if (!keyframes)
                continue;
            const bool backwards = it->alternate && it->num_iterations > 0 && it->num_iterations % 2 == 0;
            const Rml::Property* value = nullptr;
            for (const Rml::KeyframeBlock& block : keyframes->blocks){
                if (const Rml::Property* property = block.properties.GetProperty(id)){
                    value = property;
                    if (backwards)
                        break;
                }
            }
            if (value)
                return value;
        }
        return nullptr;
    }

    class EndListener : public Rml::EventListener {
    public:
        void ProcessEvent(Rml::Event& event) override{
            Rml::Element* target = event.GetTargetElement();
            if (!target)
                return;
            const std::string fill = CompatProperties::text(*target, CompatProperty::AnimationFill);
            if (fill != "forwards" && fill != "both")
                return;
            const Rml::PropertyId id = Rml::StyleSheetSpecification::GetPropertyId(event.GetParameter<Rml::String>("property", ""));
            if (id == Rml::PropertyId::Invalid)
                return;
            const Rml::Property* value = endValue(target, id);
            if (!value)
                return;
            const Rml::Property copy = *value;
            target->SetProperty(id, copy);
            for (Filled& entry : filled()){
                if (entry.element.get() == target && entry.id == id){
                    entry.value = copy;
                    entry.animation = animationKey(target);
                    return;
                }
            }
            filled().push_back(Filled{target->GetObserverPtr(), id, copy, animationKey(target)});
        }
    };

    struct ContextState {
        std::unique_ptr<EndListener> listener;
        Rml::ObserverPtr<Rml::Element> root;
    };

    std::unordered_map<Rml::Context*, ContextState>& contexts(){
        static std::unordered_map<Rml::Context*, ContextState> value;
        return value;
    }

    bool sameValue(const Rml::Property* a, const Rml::Property& b){
        return a && a->unit == b.unit && a->value == b.value;
    }

}

void AnimationFill::update(Rml::Context* context){
    if (!context)
        return;

    // one capture listener on the context's root sees the animationend of every document
    Rml::Element* root = context->GetRootElement();
    ContextState& state = contexts()[context];
    if (root && state.root.get() != root){
        state.listener = std::make_unique<EndListener>();
        root->AddEventListener(Rml::EventId::Animationend, state.listener.get(), true);
        state.root = root->GetObserverPtr();
    }

    std::vector<Filled>& list = filled();
    for (size_t i = 0; i < list.size();){
        Rml::Element* element = list[i].element.get();
        // another context's (a detached element has none: any context drops it)
        if (element && element->GetContext() && element->GetContext() != context){
            i++;
            continue;
        }
        bool drop = !element;
        if (element){
            const bool detached = !element->GetOwnerDocument() || !element->GetParentNode();
            if (detached || animationKey(element) != list[i].animation){
                // only the value compat set: a script may have replaced it since
                if (sameValue(element->GetLocalProperty(list[i].id), list[i].value))
                    element->RemoveProperty(list[i].id);
                drop = true;
            }
        }
        if (drop){
            list.erase(list.begin() + (std::ptrdiff_t)i);
            continue;
        }
        i++;
    }
}

void AnimationFill::reset(){
    for (auto& entry : contexts()){
        if (Rml::Element* root = entry.second.root.get())
            root->RemoveEventListener(Rml::EventId::Animationend, entry.second.listener.get(), true);
    }
    contexts().clear();
    filled().clear();
}

size_t AnimationFill::filledCount(){
    return filled().size();
}

bool AnimationFill::isFilled(Rml::Element* element){
    for (const Filled& entry : filled()){
        if (entry.element.get() == element)
            return true;
    }
    return false;
}
