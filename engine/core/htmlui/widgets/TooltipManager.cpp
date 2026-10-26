// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The rules are OpenEngine's TooltipOverlay (MIT; see NOTICE.md): the hover delay, the hop
// window, the source identity, the out-of-view measuring frame and the hide triggers.

#include "htmlui/widgets/TooltipManager.h"

#include "htmlui/compat/ForkFeatures.h"
#include "htmlui/compat/LuaHooks.h"
#include "HtmlUIClock.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::htmlui;

namespace {

    enum class Trigger : uint8_t { Pointer, Focus, Manual };

    constexpr const char* kTooltipClass = "htmlui-tooltip";
    constexpr const char* kArrowClass = "htmlui-tooltip-arrow";
    // a tooltip is laid out here before it is measured and placed
    constexpr float kParkedOffset = -10000.0f;

    struct Specs {
        std::unordered_map<const Rml::Element*, TooltipSpec> byElement;
    };

    Specs& specs(){
        static Specs value;
        return value;
    }

    std::unordered_map<const Rml::ElementDocument*, TooltipConfig>& configs(){
        static std::unordered_map<const Rml::ElementDocument*, TooltipConfig> value;
        return value;
    }

    Rml::Property px(float value){
        return Rml::Property(value, Rml::Unit::PX);
    }

    // tooltip-delay: "500ms", "0.5s" or milliseconds; nullopt when absent or not a time
    std::optional<double> attributeDelay(const Rml::Element* element){
        const Rml::Variant* value = element->GetAttribute("tooltip-delay");
        if (!value)
            return std::nullopt;
        const Rml::String text = Rml::StringUtilities::StripWhitespace(value->Get<Rml::String>());
        char* end = nullptr;
        const double number = std::strtod(text.c_str(), &end);
        if (end == text.c_str() || !std::isfinite(number) || number < 0.0)
            return std::nullopt;
        const std::string unit = Rml::StringUtilities::StripWhitespace(Rml::String(end));
        if (unit == "s")
            return number;
        if (unit.empty() || unit == "ms")
            return number / 1000.0;
        return std::nullopt;
    }

    bool isModifierKey(int key){
        using namespace Rml::Input;
        return key == KI_LSHIFT || key == KI_RSHIFT || key == KI_LCONTROL || key == KI_RCONTROL || key == KI_LMENU ||
               key == KI_RMENU || key == KI_LMETA || key == KI_RMETA || key == KI_LWIN || key == KI_RWIN || key == KI_CAPITAL ||
               key == KI_NUMLOCK || key == KI_SCROLL;
    }

    bool insideTooltip(const Rml::Element* element){
        for (const Rml::Element* each = element; each; each = each->GetParentNode()){
            if (each->IsClassSet(kTooltipClass))
                return true;
        }
        return false;
    }

    bool isTooltipSource(const Rml::Element* element){
        if (specs().byElement.count(element))
            return true;
        if (const Rml::Variant* title = element->GetAttribute("title")){
            if (!title->Get<Rml::String>().empty())
                return true;
        }
        return element->HasAttribute("tooltip");
    }

    // the nearest ancestor (or the element) with a tooltip, in a document that shows them
    Rml::Element* sourceOf(Rml::Element* element){
        Rml::ElementDocument* document = element ? element->GetOwnerDocument() : nullptr;
        if (!document || !TooltipManager::configOf(document).enabled)
            return nullptr;
        for (Rml::Element* each = element; each && each != document; each = each->GetParentNode()){
            if (isTooltipSource(each))
                return each;
        }
        return nullptr;
    }

    Rml::Element* bodyOf(Rml::ElementDocument* document){
        if (Rml::Element* body = compat::LuaHooks::body(document))
            return body;
        return document;
    }

    // One context's tooltips: a capture listener on its root sees the hover changes, presses,
    // keys, wheel, scrolls and focus moves; update() runs the timers.
    class TooltipSession final : public Rml::EventListener {
    public:
        explicit TooltipSession(Rml::Context* context): context(context){
            Rml::Element* root = context->GetRootElement();
            for (const char* type : kEvents)
                root->AddEventListener(type, this, true);
        }

        ~TooltipSession() override{
            hideTooltip(false);
            if (Rml::Element* root = context->GetRootElement()){
                for (const char* type : kEvents)
                    root->RemoveEventListener(type, this, true);
            }
        }

        void ProcessEvent(Rml::Event& event) override{
            switch (event.GetId()){
                case Rml::EventId::Mouseover:
                case Rml::EventId::Mouseout:
                    hoverDirty = true;
                    readPointer(event);
                    break;
                case Rml::EventId::Mousemove:
                    // only a title tooltip on its way needs where the pointer is
                    if (pending.get())
                        readPointer(event);
                    break;
                case Rml::EventId::Mousedown:
                case Rml::EventId::Mousescroll:
                case Rml::EventId::Dragstart:
                    dismiss();
                    break;
                case Rml::EventId::Scroll:
                    // a scroll of an ancestor of the source (its own content's scroll moves nothing it is anchored to)
                    if (Rml::Element* source = shownSource.get()){
                        for (Rml::Element* each = source->GetParentNode(); each; each = each->GetParentNode()){
                            if (each == event.GetTargetElement()){
                                dismiss();
                                break;
                            }
                        }
                    }
                    break;
                case Rml::EventId::Keydown:
                    if (!isModifierKey(event.GetParameter<int>("key_identifier", 0)))
                        dismiss();
                    break;
                case Rml::EventId::Focus:
                    focusDirty = true;
                    break;
                case Rml::EventId::Blur:
                    if (trigger == Trigger::Focus && shownSource.get() && event.GetTargetElement() == focusTarget.get())
                        hideTooltip(true);
                    if (pendingTrigger == Trigger::Focus && event.GetTargetElement() == focusTarget.get())
                        pending.reset();
                    focusTarget.reset();
                    break;
                default:
                    break;
            }
        }

        void update(){
            const double now = HtmlUIClock::now();
            if (Rml::Element* source = shownSource.get()){
                // gone, hidden, or its document is
                Rml::ElementDocument* document = source->GetOwnerDocument();
                if (!document || !document->IsVisible() || !source->IsVisible(true) || !tooltip.get())
                    hideTooltip(true);
            }else if (tooltip.get()){
                hideTooltip(false);
            }

            if (hoverDirty){
                hoverDirty = false;
                Rml::Element* hover = context->GetHoverElement();
                Rml::Element* source = sourceOf(hover);
                if (insideTooltip(hover) && interactive && shownSource.get())
                    source = shownSource.get();
                if (source != pointerSource.get()){
                    pointerSource = source ? source->GetObserverPtr() : Rml::ObserverPtr<Rml::Element>();
                    suppressed.reset();
                    if (trigger == Trigger::Pointer && shownSource.get() && shownSource.get() != source)
                        hideTooltip(true);
                    if (pendingTrigger == Trigger::Pointer)
                        pending.reset();
                    if (source && source != shownSource.get())
                        schedule(source, Trigger::Pointer, now);
                }
            }

            if (focusDirty){
                focusDirty = false;
                Rml::Element* focus = context->GetFocusElement();
                if (focus && focus->IsPseudoClassSet("focus-visible")){
                    focusTarget = focus->GetObserverPtr();
                    if (Rml::Element* source = sourceOf(focus)){
                        if (source != shownSource.get())
                            schedule(source, Trigger::Focus, now);
                    }
                }
            }

            if (Rml::Element* source = pending.get()){
                if (now - pendingSince >= pendingDelay - 1e-9){
                    const Trigger how = pendingTrigger;
                    pending.reset();
                    showTooltip(source, how);
                }
            }else{
                pending.reset();
            }

            if (tooltip.get() && !placed)
                place();
        }

        bool showNow(Rml::Element* source){
            pending.reset();
            return showTooltip(source, Trigger::Manual);
        }

        void hideDocument(const Rml::ElementDocument* document){
            Rml::Element* source = shownSource.get();
            if (source && (!document || source->GetOwnerDocument() == document))
                hideTooltip(true);
            Rml::Element* waiting = pending.get();
            if (waiting && (!document || waiting->GetOwnerDocument() == document))
                pending.reset();
        }

        // a drag, a press: the tooltip goes and stays away until the pointer reaches another source
        void dismiss(){
            if (pending.get() && pendingTrigger == Trigger::Pointer)
                suppressed = pending.get()->GetObserverPtr();
            pending.reset();
            if (shownSource.get()){
                if (trigger == Trigger::Pointer)
                    suppressed = shownSource;
                hideTooltip(true);
            }
        }

        TooltipState state(const Rml::ElementDocument* document) const{
            TooltipState result;
            Rml::Element* source = shownSource.get();
            Rml::Element* element = tooltip.get();
            if (!source || !element || (document && source->GetOwnerDocument() != document))
                return result;
            result.visible = placed;
            result.source = source;
            result.tooltip = element;
            result.text = textOf(element);
            const Rml::Vector2f offset = element->GetAbsoluteOffset(Rml::BoxArea::Border);
            const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
            result.x = offset.x;
            result.y = offset.y;
            result.width = size.x;
            result.height = size.y;
            result.side = popupSideName(side);
            return result;
        }

        Rml::Context* getContext() const { return context; }

    private:
        static constexpr const char* kEvents[] = {"mouseover", "mouseout", "mousemove", "mousedown", "mousescroll", "dragstart",
                                                  "scroll", "keydown", "focus", "blur"};

        static std::string textOf(Rml::Element* element){
            if (Rml::ElementText* text = rmlui_dynamic_cast<Rml::ElementText*>(element))
                return text->GetText();
            std::string text;
            for (int i = 0; i < element->GetNumChildren(); i++){
                Rml::Element* child = element->GetChild(i);
                if (!child->IsClassSet(kArrowClass))
                    text += textOf(child);
            }
            return text;
        }

        void readPointer(Rml::Event& event){
            if (event.GetParameters().count("mouse_x")){
                pointer.x = event.GetParameter<float>("mouse_x", pointer.x);
                pointer.y = event.GetParameter<float>("mouse_y", pointer.y);
            }
        }

        double delayOf(Rml::Element* source) const{
            const TooltipConfig config = TooltipManager::configOf(source->GetOwnerDocument());
            auto it = specs().byElement.find(source);
            if (it != specs().byElement.end() && it->second.delay)
                return std::max(0.0, *it->second.delay);
            if (std::optional<double> delay = attributeDelay(source))
                return *delay;
            return std::max(0.0, config.delay);
        }

        void schedule(Rml::Element* source, Trigger how, double now){
            if (how == Trigger::Pointer && suppressed.get() == source)
                return;
            const TooltipConfig config = TooltipManager::configOf(source->GetOwnerDocument());
            // the hop: right after a tooltip, the next source shows at once
            if (now - hiddenAt <= config.hopWindow || (shownSource.get() && shownSource.get() != source)){
                showTooltip(source, how);
                return;
            }
            pending = source->GetObserverPtr();
            pendingTrigger = how;
            pendingSince = now;
            pendingDelay = delayOf(source);
        }

        // the content of `source`'s tooltip into `element`; false when it has none
        bool fill(Rml::Element* source, Rml::Element* element, bool& rich){
            rich = false;
            auto it = specs().byElement.find(source);
            if (it != specs().byElement.end()){
                const TooltipSpec& spec = it->second;
                rich = true;
                if (spec.content){
                    TooltipContent content = spec.content();
                    if (content.none)
                        return false;
                    if (content.element)
                        return appendClone(element, content.element);
                    if (content.text.empty())
                        return false;
                    rich = false;
                    element->SetInnerRML(Rml::StringUtilities::EncodeRml(content.text));
                    return true;
                }
                if (Rml::Element* from = spec.element.get())
                    return appendClone(element, from);
                if (spec.text.empty())
                    return false;
                rich = false;
                element->SetInnerRML(Rml::StringUtilities::EncodeRml(spec.text));
                return true;
            }
            if (const Rml::Variant* reference = source->GetAttribute("tooltip")){
                Rml::String id = Rml::StringUtilities::StripWhitespace(reference->Get<Rml::String>());
                if (!id.empty() && id[0] == '#')
                    id.erase(0, 1);
                Rml::ElementDocument* document = source->GetOwnerDocument();
                Rml::Element* from = (document && !id.empty()) ? document->GetElementById(id) : nullptr;
                if (from){
                    rich = true;
                    if (from->HasAttribute(compat::LuaHooks::kTemplateAttribute)){
                        const std::string rml = compat::LuaHooks::templateContent(from);
                        if (rml.empty())
                            return false;
                        element->SetInnerRML(rml);
                        return true;
                    }
                    return appendClone(element, from);
                }
            }
            const Rml::String title = source->GetAttribute<Rml::String>("title", "");
            if (title.empty())
                return false;
            element->SetInnerRML(Rml::StringUtilities::EncodeRml(title));
            return true;
        }

        // a copy of `from` without its id and hidden (so the rules that hide it do not)
        static bool appendClone(Rml::Element* element, Rml::Element* from){
            Rml::ElementPtr clone = from->Clone();
            if (!clone)
                return false;
            clone->RemoveAttribute("id");
            clone->RemoveAttribute("hidden");
            if (clone->GetLocalStyleProperties().count(Rml::PropertyId::Display))
                clone->RemoveProperty(Rml::PropertyId::Display);
            element->AppendChild(std::move(clone));
            return true;
        }

        bool showTooltip(Rml::Element* source, Trigger how){
            hideTooltip(false);
            Rml::ElementDocument* document = source->GetOwnerDocument();
            if (!document)
                return false;
            Rml::Element* body = bodyOf(document);

            Rml::XMLAttributes attributes;
            attributes["class"] = Rml::String(kTooltipClass);
            attributes["role"] = Rml::String("tooltip");
            Rml::ElementPtr created = Rml::Factory::InstanceElement(body, "div", "div", attributes);
            if (!created)
                return false;
            bool rich = false;
            if (!fill(source, created.get(), rich))
                return false;

            auto it = specs().byElement.find(source);
            const TooltipSpec* spec = it != specs().byElement.end() ? &it->second : nullptr;
            interactive = spec && spec->interactive;
            arrow = spec && spec->arrow;
            richPlacement = rich || spec != nullptr || how != Trigger::Pointer;
            preferred = spec ? spec->placement : (richPlacement ? PopupSide::Top : PopupSide::Bottom);
            automatic = spec && spec->automatic;
            if (interactive)
                created->SetClass("htmlui-tooltip-interactive", true);
            if (arrow){
                Rml::XMLAttributes arrowAttributes;
                arrowAttributes["class"] = Rml::String(kArrowClass);
                created->AppendChild(Rml::Factory::InstanceElement(created.get(), "div", "div", arrowAttributes));
            }
            const bool fixed = compat::ForkFeatures::present().has(compat::ForkFeature::Fixed);
            created->SetProperty(Rml::PropertyId::Position, Rml::Property(fixed ? Rml::Style::Position::Fixed : Rml::Style::Position::Absolute));
            created->SetProperty(Rml::PropertyId::Left, px(kParkedOffset));
            created->SetProperty(Rml::PropertyId::Top, px(kParkedOffset));
            created->SetProperty(Rml::PropertyId::Visibility, Rml::Property(Rml::Style::Visibility::Hidden));

            Rml::Element* element = body->AppendChild(std::move(created));
            tooltip = element->GetObserverPtr();
            shownSource = source->GetObserverPtr();
            anchorPointer = pointer;
            trigger = how;
            placed = false;
            source->DispatchEvent("tooltipshow", Rml::Dictionary(), false, true);
            return true;
        }

        void place(){
            Rml::Element* element = tooltip.get();
            Rml::Element* source = shownSource.get();
            if (!element || !source)
                return;
            const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
            if (size.x <= 0.0f && size.y <= 0.0f)
                return;   // laid out at the next update

            PlaceRequest request;
            const Rml::Vector2i dimensions = context->GetDimensions();
            request.viewport = PopupRect{0.0f, 0.0f, (float)dimensions.x, (float)dimensions.y};
            request.popup = PopupSize{size.x, size.y};
            request.pad = TooltipManager::kEdgePad;
            if (richPlacement){
                const Rml::Vector2f offset = source->GetAbsoluteOffset(Rml::BoxArea::Border);
                const Rml::Vector2f box = source->GetBox().GetSize(Rml::BoxArea::Border);
                request.anchor = PopupRect{offset.x, offset.y, box.x, box.y};
                request.preferred = preferred;
                request.automatic = automatic;
                request.gap = TooltipManager::kRichGap;
                request.arrow = arrow ? TooltipManager::kArrowSize : 0.0f;
            }else{
                request.anchor = PopupRect{anchorPointer.x, anchorPointer.y, 1.0f, 1.0f};
                request.preferred = PopupSide::Bottom;
                request.gap = TooltipManager::kTitleGap;
            }
            const PlaceResult result = placePopup(request);
            side = result.side;

            // left/top are the containing block's: the body's (absolute) or the viewport's (fixed)
            Rml::Vector2f origin(0.0f, 0.0f);
            if (element->GetComputedValues().position() == Rml::Style::Position::Absolute){
                if (Rml::Element* block = element->GetOffsetParent())
                    origin = block->GetAbsoluteOffset(Rml::BoxArea::Padding);
            }
            // whole pixels: text at a fractional offset blurs, and draws differently as the offset moves
            element->SetProperty(Rml::PropertyId::Left, px(std::round(result.rect.x - origin.x)));
            element->SetProperty(Rml::PropertyId::Top, px(std::round(result.rect.y - origin.y)));
            element->RemoveProperty(Rml::PropertyId::Visibility);
            for (const PopupSide each : {PopupSide::Top, PopupSide::Bottom, PopupSide::Left, PopupSide::Right})
                element->SetClass(std::string("htmlui-tooltip-") + popupSideName(each), each == result.side);

            if (arrow){
                for (int i = 0; i < element->GetNumChildren(); i++){
                    Rml::Element* child = element->GetChild(i);
                    if (!child->IsClassSet(kArrowClass))
                        continue;
                    const float half = TooltipManager::kArrowSize * 0.5f;
                    const bool vertical = result.side == PopupSide::Top || result.side == PopupSide::Bottom;
                    const float along = result.arrowOffset - half;
                    child->SetProperty(vertical ? Rml::PropertyId::Left : Rml::PropertyId::Top, px(along));
                    child->RemoveProperty(vertical ? Rml::PropertyId::Top : Rml::PropertyId::Left);
                    child->RemoveProperty(Rml::PropertyId::Bottom);
                    child->RemoveProperty(Rml::PropertyId::Right);
                    switch (result.side){
                        case PopupSide::Top: child->SetProperty(Rml::PropertyId::Bottom, px(-half)); break;
                        case PopupSide::Bottom: child->SetProperty(Rml::PropertyId::Top, px(-half)); break;
                        case PopupSide::Left: child->SetProperty(Rml::PropertyId::Right, px(-half)); break;
                        case PopupSide::Right: child->SetProperty(Rml::PropertyId::Left, px(-half)); break;
                    }
                }
            }
            placed = true;
        }

        void hideTooltip(bool notify){
            Rml::Element* element = tooltip.get();
            Rml::Element* source = shownSource.get();
            if (element && element->GetParentNode())
                element->GetParentNode()->RemoveChild(element);
            tooltip.reset();
            if (shownSource.get() || element){
                hiddenAt = placed ? HtmlUIClock::now() : hiddenAt;
            }
            shownSource.reset();
            placed = false;
            if (notify && source)
                source->DispatchEvent("tooltiphide", Rml::Dictionary(), false, true);
        }

        Rml::Context* context;
        bool hoverDirty = false;
        bool focusDirty = false;
        Rml::Vector2f pointer{0.0f, 0.0f};
        Rml::ObserverPtr<Rml::Element> pointerSource;
        Rml::ObserverPtr<Rml::Element> focusTarget;
        Rml::ObserverPtr<Rml::Element> suppressed;   // dismissed: not again until the pointer leaves it

        Rml::ObserverPtr<Rml::Element> pending;
        Trigger pendingTrigger = Trigger::Pointer;
        double pendingSince = 0.0;
        double pendingDelay = TooltipManager::kDefaultDelay;

        Rml::ObserverPtr<Rml::Element> shownSource;
        Rml::ObserverPtr<Rml::Element> tooltip;
        Trigger trigger = Trigger::Pointer;
        Rml::Vector2f anchorPointer{0.0f, 0.0f};
        bool placed = false;
        bool interactive = false;
        bool arrow = false;
        bool richPlacement = false;
        bool automatic = false;
        PopupSide preferred = PopupSide::Top;
        PopupSide side = PopupSide::Top;
        double hiddenAt = -std::numeric_limits<double>::infinity();
    };

    std::unordered_map<Rml::Context*, std::unique_ptr<TooltipSession>>& sessions(){
        static std::unordered_map<Rml::Context*, std::unique_ptr<TooltipSession>> value;
        return value;
    }

    TooltipSession* sessionOf(Rml::Context* context, bool create){
        if (!context || !context->GetRootElement())
            return nullptr;
        auto& all = sessions();
        auto it = all.find(context);
        if (it != all.end())
            return it->second.get();
        if (!create)
            return nullptr;
        return all.emplace(context, std::make_unique<TooltipSession>(context)).first->second.get();
    }

}

void TooltipManager::setTooltip(Rml::Element* source, TooltipSpec spec){
    if (!source)
        return;
    specs().byElement[source] = std::move(spec);
    sessionOf(source->GetContext(), true);
}

void TooltipManager::clearTooltip(Rml::Element* source){
    specs().byElement.erase(source);
}

bool TooltipManager::hasTooltip(const Rml::Element* source){
    return source && isTooltipSource(source);
}

void TooltipManager::configure(Rml::ElementDocument* document, const TooltipConfig& config){
    if (!document)
        return;
    configs()[document] = config;
    if (!config.enabled){
        if (TooltipSession* session = sessionOf(document->GetContext(), false))
            session->hideDocument(document);
    }
}

TooltipConfig TooltipManager::configOf(const Rml::ElementDocument* document){
    auto it = configs().find(document);
    return it != configs().end() ? it->second : TooltipConfig();
}

bool TooltipManager::show(Rml::Element* source){
    if (!source || !isTooltipSource(source))
        return false;
    TooltipSession* session = sessionOf(source->GetContext(), true);
    return session && session->showNow(source);
}

void TooltipManager::hide(Rml::ElementDocument* document){
    if (TooltipSession* session = document ? sessionOf(document->GetContext(), false) : nullptr)
        session->hideDocument(document);
}

TooltipState TooltipManager::stateOf(const Rml::ElementDocument* document){
    Rml::Context* context = document ? const_cast<Rml::ElementDocument*>(document)->GetContext() : nullptr;
    TooltipSession* session = sessionOf(context, false);
    return session ? session->state(document) : TooltipState();
}

void TooltipManager::update(Rml::Context* context){
    if (TooltipSession* session = sessionOf(context, true))
        session->update();
}

void TooltipManager::contextDestroyed(Rml::Context* context){
    sessions().erase(context);
}

void TooltipManager::documentUnloaded(Rml::ElementDocument* document){
    configs().erase(document);
    if (TooltipSession* session = document ? sessionOf(document->GetContext(), false) : nullptr)
        session->hideDocument(document);
}

void TooltipManager::elementDestroyed(Rml::Element* element){
    Specs& all = specs();
    if (!all.byElement.empty())
        all.byElement.erase(element);
}

void TooltipManager::releaseAll(){
    sessions().clear();
    specs().byElement.clear();
    configs().clear();
}

void TooltipManager::hideForDrag(Rml::Context* context){
    if (TooltipSession* session = sessionOf(context, false))
        session->dismiss();
}
