// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The model is OpenEngine's DragDropManager (MIT; see NOTICE.md): a typed payload, a target's
// feedback (allowed, its CSS class, no ghost), the drop, the end-of-drag report and the
// auto-scroll capability; here it is HTML5's events and DataTransfer.

#include "htmlui/widgets/DragDropSession.h"

#include "htmlui/widgets/ItemsViewElement.h"
#include "htmlui/widgets/TooltipManager.h"
#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/ForkFeatures.h"
#include "htmlui/compat/LuaHooks.h"
#include "object/ui/HtmlUIEventListener.h"
#include "object/ui/UIDataTransfer.h"
#include "HtmlUIClock.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::htmlui;

namespace {

    constexpr const char* kDragEnter = "htmlui-dragenter";
    constexpr const char* kDragOver = "htmlui-dragover";
    constexpr const char* kDragLeave = "htmlui-dragleave";
    constexpr const char* kDrop = "htmlui-drop";

    constexpr const char* kSourceClass = "htmlui-drag-source";
    constexpr const char* kDraggingClass = "htmlui-dragging";
    constexpr const char* kAllowedClass = "htmlui-drop-allowed";
    constexpr const char* kDeniedClass = "htmlui-drop-denied";
    constexpr const char* kGhostClass = "htmlui-drag-ghost";
    constexpr const char* kRowType = "htmlui/row";

    Rml::Property px(float value){
        return Rml::Property(value, Rml::Unit::PX);
    }

    bool effectAllows(const std::string& allowed, const std::string& effect){
        if (effect == "none")
            return false;
        if (allowed == "all" || allowed.empty() || allowed == "uninitialized")
            return true;
        if (allowed == "none")
            return false;
        if (allowed == effect)
            return true;
        if (allowed == "copyMove")
            return effect == "copy" || effect == "move";
        if (allowed == "copyLink")
            return effect == "copy" || effect == "link";
        if (allowed == "linkMove")
            return effect == "link" || effect == "move";
        return false;
    }

    // HTML5's default dropEffect for an effectAllowed
    std::string defaultEffect(const std::string& allowed){
        if (allowed == "none")
            return "none";
        if (allowed == "link")
            return "link";
        if (allowed == "move" || allowed == "linkMove")
            return "move";
        return "copy";
    }

    bool isDragListenerType(const std::string& type){
        return type == "dragenter" || type == "dragover" || type == "dragleave" || type == "drop" || type == "dragdrop";
    }

    // where drag feedback goes: the nearest element with a dropzone, a drop listener or a virtual view
    Rml::Element* dropTargetOf(Rml::Element* element){
        for (Rml::Element* each = element; each; each = each->GetParentNode()){
            if (rmlui_dynamic_cast<Rml::ElementDocument*>(each))
                return nullptr;
            if (each->HasAttribute("dropzone") || rmlui_dynamic_cast<ItemsViewElement*>(each))
                return each;
            for (HtmlUIEventListener* listener : HtmlUIEventListener::listenersOf(each)){
                if (isDragListenerType(listener->getDomType()))
                    return each;
            }
        }
        return nullptr;
    }

    // dropzone="item weapon move": the nearest one's verdict for the types (its operation in effect)
    bool dropzoneAccepts(Rml::Element* element, const UIDataTransferState& transfer, std::string& effect){
        for (Rml::Element* each = element; each; each = each->GetParentNode()){
            const Rml::Variant* value = each->GetAttribute("dropzone");
            if (!value)
                continue;
            Rml::StringList words;
            Rml::StringUtilities::ExpandString(words, value->Get<Rml::String>(), ' ');
            bool typed = false;
            std::string operation;
            for (Rml::String word : words){
                word = Rml::StringUtilities::StripWhitespace(word);
                if (word == "copy" || word == "move" || word == "link"){
                    operation = word;
                    continue;
                }
                // HTML's old syntax: string:text/plain
                if (word.rfind("string:", 0) == 0 || word.rfind("file:", 0) == 0)
                    word = word.substr(word.find(':') + 1);
                if (!word.empty() && transfer.find(word))
                    typed = true;
            }
            if (typed && !operation.empty())
                effect = operation;
            return typed;
        }
        return false;
    }

    Rml::Element* bodyOf(Rml::ElementDocument* document){
        if (Rml::Element* body = compat::LuaHooks::body(document))
            return body;
        return document;
    }

    bool isVerboseDrag(Rml::Element* element){
        const Rml::Style::Drag drag = element->GetComputedValues().drag();
        return drag == Rml::Style::Drag::DragDrop || drag == Rml::Style::Drag::Clone;
    }

    // the scroll container a drag at `point` scrolls (the pointer within kAutoScrollEdge of its edge)
    Rml::Element* scrollerAt(Rml::Element* element, Rml::Vector2f point, Rml::Vector2f& velocity){
        for (Rml::Element* each = element; each; each = each->GetParentNode()){
            if (rmlui_dynamic_cast<Rml::ElementDocument*>(each))
                break;
            const Rml::ComputedValues& computed = each->GetComputedValues();
            using Rml::Style::Overflow;
            const bool scrollsY = (computed.overflow_y() == Overflow::Auto || computed.overflow_y() == Overflow::Scroll) &&
                                  each->GetScrollHeight() > each->GetClientHeight() + 0.5f;
            const bool scrollsX = (computed.overflow_x() == Overflow::Auto || computed.overflow_x() == Overflow::Scroll) &&
                                  each->GetScrollWidth() > each->GetClientWidth() + 0.5f;
            if (!scrollsX && !scrollsY)
                continue;
            const Rml::Vector2f origin = each->GetAbsoluteOffset(Rml::BoxArea::Padding);
            const float width = each->GetClientWidth(), height = each->GetClientHeight();
            if (point.x < origin.x || point.y < origin.y || point.x > origin.x + width || point.y > origin.y + height)
                continue;
            const float edge = DragDropSession::kAutoScrollEdge;
            const float speed = DragDropSession::kAutoScrollSpeed;
            velocity = Rml::Vector2f(0.0f, 0.0f);
            if (scrollsY){
                const float top = point.y - origin.y, bottom = origin.y + height - point.y;
                if (top < edge && each->GetScrollTop() > 0.0f)
                    velocity.y = -speed * (1.0f - top / edge);
                else if (bottom < edge && each->GetScrollTop() < each->GetScrollHeight() - height)
                    velocity.y = speed * (1.0f - bottom / edge);
            }
            if (scrollsX){
                const float left = point.x - origin.x, right = origin.x + width - point.x;
                if (left < edge && each->GetScrollLeft() > 0.0f)
                    velocity.x = -speed * (1.0f - left / edge);
                else if (right < edge && each->GetScrollLeft() < each->GetScrollWidth() - width)
                    velocity.x = speed * (1.0f - right / edge);
            }
            if (velocity.x != 0.0f || velocity.y != 0.0f)
                return each;
        }
        return nullptr;
    }

    struct Session {
        Rml::Context* context = nullptr;
        Rml::ObserverPtr<Rml::Element> source;
        std::shared_ptr<UIDataTransferState> transfer;
        bool programmatic = false;
        bool ended = false;
        bool dropped = false;
        bool cancelled = false;
        Rml::Vector2f pointer{0.0f, 0.0f};
        Rml::Vector2f grab{0.0f, 0.0f};       // the pointer's offset in the source at the start
        bool moved = true;
        Rml::ObserverPtr<Rml::Element> hovered;
        Rml::ObserverPtr<Rml::Element> target;
        Rml::ObserverPtr<Rml::Element> view;   // the virtual view under the pointer
        int dropIndex = -1;
        int dropBand = 0;
        bool accepted = false;
        std::string effect = "none";
        Rml::ObserverPtr<Rml::Element> ghost;
        bool ghostMade = false;
        Rml::Vector2f ghostOffset{0.0f, 0.0f};
        Rml::ObserverPtr<Rml::Element> body;
        double lastTime = 0.0;
    };

    class DragListener;

    struct Runtime {
        std::unordered_map<Rml::Context*, std::unique_ptr<Session>> sessions;
        std::unordered_map<Rml::Context*, DragListener*> listeners;
        Rml::Context* programmatic = nullptr;   // the context of the UIDrag drag
        bool starting = false;                  // UIDrag::begin is sending dragstart
    };

    Runtime& runtime(){
        static Runtime value;
        return value;
    }

    Session* sessionOf(Rml::Context* context){
        auto& sessions = runtime().sessions;
        auto it = sessions.find(context);
        return it != sessions.end() ? it->second.get() : nullptr;
    }

    // ---- events ----

    Rml::Dictionary parameters(const Session& session, bool* prevented){
        Rml::Dictionary result;
        result["mouse_x"] = session.pointer.x;
        result["mouse_y"] = session.pointer.y;
        if (prevented)
            result[HtmlUIEventNames::preventedParameter] = (void*)prevented;
        if (session.view.get() && session.dropIndex >= 0){
            result[DragDropSession::kDropIndexParameter] = session.dropIndex + 1;
            result[DragDropSession::kDropPositionParameter] = Rml::String(session.dropBand < 0 ? "before" : (session.dropBand > 0 ? "after" : "on"));
        }
        return result;
    }

    bool dispatch(Session& session, Rml::Element* element, const char* type){
        if (!element)
            return false;
        bool prevented = false;
        element->DispatchEvent(type, parameters(session, &prevented), true, true);
        return prevented;
    }

    void setFeedback(Session& session, Rml::Element* newTarget, bool allowed){
        Rml::Element* old = session.target.get();
        if (old && old != newTarget){
            old->SetClass(kAllowedClass, false);
            old->SetClass(kDeniedClass, false);
        }
        session.target = newTarget ? newTarget->GetObserverPtr() : Rml::ObserverPtr<Rml::Element>();
        if (newTarget){
            newTarget->SetClass(kAllowedClass, allowed);
            newTarget->SetClass(kDeniedClass, !allowed);
        }
    }

    void hideIndicator(Session& session){
        if (auto* view = rmlui_dynamic_cast<ItemsViewElement*>(session.view.get()))
            view->showDropIndicator(-1, 0);
    }

    // the view under the pointer and where among its rows
    void locateInView(Session& session, Rml::Element* hovered){
        ItemsViewElement* view = ItemsViewElement::closestView(hovered);
        if (session.view.get() != view)
            hideIndicator(session);
        session.view = view ? view->GetObserverPtr() : Rml::ObserverPtr<Rml::Element>();
        session.dropIndex = -1;
        session.dropBand = 0;
        if (view){
            int index = -1, band = 0;
            if (view->dropTargetAt(session.pointer.x, session.pointer.y, index, band)){
                session.dropIndex = index;
                session.dropBand = band;
            }
        }
    }

    // the element under the drag changed: dragleave on the old one, dragenter on the new one
    void retarget(Session& session, Rml::Element* hovered){
        locateInView(session, hovered);
        Rml::Element* old = session.hovered.get();
        if (old == hovered)
            return;
        if (old)
            dispatch(session, old, kDragLeave);
        session.hovered = hovered ? hovered->GetObserverPtr() : Rml::ObserverPtr<Rml::Element>();
        if (hovered){
            session.transfer->dropEffect = defaultEffect(session.transfer->effectAllowed);
            dispatch(session, hovered, kDragEnter);
        }
        session.moved = true;
    }

    // dragover on the element under the drag: whether it accepts, with which effect
    void dragOver(Session& session){
        Rml::Element* hovered = session.hovered.get();
        session.moved = false;
        if (!hovered){
            session.accepted = false;
            session.effect = "none";
            setFeedback(session, nullptr, false);
            hideIndicator(session);
            return;
        }
        UIDataTransferState& transfer = *session.transfer;
        transfer.dropEffect = defaultEffect(transfer.effectAllowed);
        const bool prevented = dispatch(session, hovered, kDragOver);
        std::string effect = transfer.dropEffect;
        const bool byZone = !prevented && dropzoneAccepts(hovered, transfer, effect);
        session.accepted = (prevented || byZone) && effectAllows(transfer.effectAllowed, effect);
        session.effect = session.accepted ? effect : "none";
        transfer.dropEffect = session.effect;
        setFeedback(session, dropTargetOf(hovered), session.accepted);
        if (auto* view = rmlui_dynamic_cast<ItemsViewElement*>(session.view.get()))
            view->showDropIndicator(session.accepted ? session.dropIndex : -1, session.dropBand);
    }

    void makeGhost(Session& session){
        session.ghostMade = true;
        Rml::Element* source = session.source.get();
        UIDataTransferState& transfer = *session.transfer;
        if (!source || session.programmatic || (transfer.imageSet && !transfer.image.get()))
            return;
        // drag: clone keeps RmlUi's own ghost
        if (source->GetComputedValues().drag() == Rml::Style::Drag::Clone)
            return;
        Rml::Element* from = transfer.imageSet ? transfer.image.get() : source;
        Rml::ElementDocument* document = source->GetOwnerDocument();
        if (!from || !document)
            return;
        Rml::ElementPtr clone = from->Clone();
        if (!clone)
            return;
        clone->RemoveAttribute("id");
        clone->SetClass(kGhostClass, true);
        clone->SetClass(kSourceClass, false);
        const Rml::Vector2f size = from->GetBox().GetSize(Rml::BoxArea::Border);
        const bool fixed = compat::ForkFeatures::present().has(compat::ForkFeature::Fixed);
        clone->SetProperty(Rml::PropertyId::Position, Rml::Property(fixed ? Rml::Style::Position::Fixed : Rml::Style::Position::Absolute));
        clone->SetProperty(Rml::PropertyId::BoxSizing, Rml::Property(Rml::Style::BoxSizing::BorderBox));
        clone->SetProperty(Rml::PropertyId::Width, px(size.x));
        clone->SetProperty(Rml::PropertyId::Height, px(size.y));
        clone->SetProperty(Rml::PropertyId::PointerEvents, Rml::Property(Rml::Style::PointerEvents::None));
        // .rml documents have no user-agent sheet to give it its look
        if (!compat::DocumentLoader::compiledOf(document))
            clone->SetProperty(Rml::PropertyId::Opacity, Rml::Property(DragDropSession::kGhostOpacity, Rml::Unit::NUMBER));
        session.ghostOffset = transfer.imageSet ? Rml::Vector2f(transfer.imageX, transfer.imageY) : session.grab;
        Rml::Element* ghost = bodyOf(document)->AppendChild(std::move(clone));
        session.ghost = ghost->GetObserverPtr();
    }

    void moveGhost(Session& session){
        Rml::Element* ghost = session.ghost.get();
        if (!ghost)
            return;
        Rml::Vector2f origin(0.0f, 0.0f);
        if (ghost->GetComputedValues().position() == Rml::Style::Position::Absolute){
            if (Rml::Element* block = ghost->GetOffsetParent())
                origin = block->GetAbsoluteOffset(Rml::BoxArea::Padding);
        }
        ghost->SetProperty(Rml::PropertyId::Left, px(session.pointer.x - session.ghostOffset.x - origin.x));
        ghost->SetProperty(Rml::PropertyId::Top, px(session.pointer.y - session.ghostOffset.y - origin.y));
    }

    // the drag is over: feedback, ghost and the transfer's values go
    void finish(Rml::Context* context){
        Runtime& r = runtime();
        auto it = r.sessions.find(context);
        if (it == r.sessions.end())
            return;
        std::unique_ptr<Session> session = std::move(it->second);
        r.sessions.erase(it);
        if (r.programmatic == context)
            r.programmatic = nullptr;
        setFeedback(*session, nullptr, false);
        hideIndicator(*session);
        if (Rml::Element* ghost = session->ghost.get()){
            if (ghost->GetParentNode())
                ghost->GetParentNode()->RemoveChild(ghost);
        }
        if (Rml::Element* source = session->source.get())
            source->SetClass(kSourceClass, false);
        if (Rml::Element* body = session->body.get())
            body->SetClass(kDraggingClass, false);
        session->transfer->valid = false;
        session->transfer->entries.clear();
        session->transfer->image.reset();
        session->transfer->rowView.reset();
    }

    Session* start(Rml::Element* source, bool programmatic, Rml::Vector2f pointer){
        Rml::Context* context = source->GetContext();
        if (!context)
            return nullptr;
        if (sessionOf(context))
            finish(context);
        auto session = std::make_unique<Session>();
        session->context = context;
        session->source = source->GetObserverPtr();
        session->transfer = std::make_shared<UIDataTransferState>();
        session->programmatic = programmatic;
        session->pointer = pointer;
        session->grab = pointer - source->GetAbsoluteOffset(Rml::BoxArea::Border);
        session->lastTime = HtmlUIClock::now();
        // a virtual view's row: the drag carries where it came from
        if (ItemsViewElement* view = ItemsViewElement::closestView(source)){
            const int item = view->itemOfElement(source);
            if (item >= 0){
                session->transfer->rowView = view->GetObserverPtr();
                session->transfer->rowIndex = item;
                session->transfer->rowKey = view->keyOf(item);
                UIDataTransferState::Entry row;
                row.type = kRowType;
                session->transfer->entries.push_back(std::move(row));
            }
        }
        source->SetClass(kSourceClass, true);
        if (Rml::ElementDocument* document = source->GetOwnerDocument()){
            Rml::Element* body = bodyOf(document);
            body->SetClass(kDraggingClass, true);
            session->body = body->GetObserverPtr();
        }
        TooltipManager::hideForDrag(context);
        Session* result = session.get();
        runtime().sessions[context] = std::move(session);
        return result;
    }

    // RmlUi's dragend (or UIDrag's): the report of the drop, then the cleanup at the next update
    void endDrag(Session& session, bool cancelled){
        if (session.ended)
            return;
        if ((cancelled || !session.dropped) && session.hovered.get()){
            Rml::Element* hovered = session.hovered.get();
            session.hovered.reset();
            dispatch(session, hovered, kDragLeave);
        }
        session.cancelled = cancelled;
        session.transfer->dropEffect = (session.dropped && !cancelled) ? session.effect : "none";
        session.ended = true;
    }

    // RmlUi's dragdrop: the drop on an accepting target, else a dragleave
    void dropOn(Session& session, Rml::Element* element){
        retarget(session, element);
        if (session.moved)
            dragOver(session);
        Rml::Element* hovered = session.hovered.get();
        if (!hovered)
            return;
        if (session.accepted){
            session.transfer->dropEffect = session.effect;
            dispatch(session, hovered, kDrop);
            session.dropped = true;
        }else{
            session.hovered.reset();
            dispatch(session, hovered, kDragLeave);
        }
    }

    class DragListener final : public Rml::EventListener {
    public:
        explicit DragListener(Rml::Context* context): context(context){
        }

        void ProcessEvent(Rml::Event& event) override{
            Runtime& r = runtime();
            switch (event.GetId()){
                case Rml::EventId::Dragstart:{
                    if (r.starting)
                        return;
                    Rml::Element* source = event.GetTargetElement();
                    if (source && isVerboseDrag(source)){
                        const Rml::Vector2f pointer(event.GetParameter<float>("mouse_x", 0.0f), event.GetParameter<float>("mouse_y", 0.0f));
                        start(source, false, pointer);
                    }
                    return;
                }
                case Rml::EventId::Drag:{
                    Session* session = sessionOf(context);
                    if (!session || session->programmatic || session->ended)
                        return;
                    session->pointer = Rml::Vector2f(event.GetParameter<float>("mouse_x", session->pointer.x),
                                                     event.GetParameter<float>("mouse_y", session->pointer.y));
                    session->moved = true;
                    if (!session->ghostMade)
                        makeGhost(*session);
                    moveGhost(*session);
                    return;
                }
                case Rml::EventId::Dragdrop:{
                    Session* session = sessionOf(context);
                    if (!session || session->programmatic || session->ended)
                        return;
                    session->pointer = Rml::Vector2f(event.GetParameter<float>("mouse_x", session->pointer.x),
                                                     event.GetParameter<float>("mouse_y", session->pointer.y));
                    dropOn(*session, event.GetTargetElement());
                    return;
                }
                case Rml::EventId::Dragend:{
                    Session* session = sessionOf(context);
                    if (!session || event.GetTargetElement() != session->source.get())
                        return;
                    endDrag(*session, event.GetParameter<int>("cancelled", 0) != 0);
                    return;
                }
                case Rml::EventId::Keydown:{
                    Session* session = sessionOf(context);
                    if (!session || session->ended || event.GetParameter<int>("key_identifier", 0) != Rml::Input::KI_ESCAPE)
                        return;
                    // Escape (gamepad B) cancels; the key goes no further (a dialog stays open)
                    event.StopPropagation();
                    if (session->programmatic){
                        DragDropSession::cancel();
                    }else{
                        context->CancelDrag();
                    }
                    return;
                }
                default:
                    return;
            }
        }

        void OnAttach(Rml::Element*) override{
            attachments++;
        }

        void OnDetach(Rml::Element*) override{
            if (--attachments == 0){
                auto& listeners = runtime().listeners;
                auto it = listeners.find(context);
                if (it != listeners.end() && it->second == this)
                    listeners.erase(it);
                delete this;
            }
        }

    private:
        Rml::Context* context;
        int attachments = 0;
    };

    void install(Rml::Context* context){
        auto& listeners = runtime().listeners;
        if (!context || !context->GetRootElement() || listeners.count(context))
            return;
        DragListener* listener = new DragListener(context);
        listeners[context] = listener;
        Rml::Element* root = context->GetRootElement();
        for (const char* type : {"dragstart", "drag", "dragdrop", "dragend", "keydown"})
            root->AddEventListener(type, listener, true);
    }

}

// ---- UIDataTransferState ----

UIDataTransferState::Entry* UIDataTransferState::find(const std::string& type){
    for (Entry& entry : entries){
        if (entry.type == type)
            return &entry;
    }
    return nullptr;
}

const UIDataTransferState::Entry* UIDataTransferState::find(const std::string& type) const{
    for (const Entry& entry : entries){
        if (entry.type == type)
            return &entry;
    }
    return nullptr;
}

// ---- DragDropSession ----

const char* DragDropSession::rmlEventOf(const std::string& domType){
    if (domType == "dragenter")
        return kDragEnter;
    if (domType == "dragover")
        return kDragOver;
    if (domType == "dragleave")
        return kDragLeave;
    if (domType == "drop")
        return kDrop;
    return "";
}

std::shared_ptr<UIDataTransferState> DragDropSession::transferOf(Rml::Element* element){
    Session* session = element ? sessionOf(element->GetContext()) : nullptr;
    return session ? session->transfer : nullptr;
}

DragState DragDropSession::stateOf(Rml::Context* context){
    DragState state;
    Session* session = sessionOf(context);
    if (!session || session->ended)
        return state;
    state.active = true;
    state.source = session->source.get();
    state.target = session->target.get();
    for (const UIDataTransferState::Entry& entry : session->transfer->entries)
        state.types.push_back(entry.type);
    state.allowed = session->accepted;
    state.effect = session->effect;
    state.programmatic = session->programmatic;
    return state;
}

bool DragDropSession::begin(Rml::Element* source, std::vector<UIDataTransferState::Entry> entries, const std::string& effectAllowed){
    if (!source || !source->GetContext())
        return false;
    Rml::Context* context = source->GetContext();
    install(context);
    // one drag at a time: a programmatic one before, or a pointer drag in this context, ends cancelled
    if (runtime().programmatic)
        cancel();
    if (Session* existing = sessionOf(context)){
        if (!existing->programmatic && !existing->ended)
            context->CancelDrag();
        finish(context);
    }
    const Rml::Vector2f box = source->GetBox().GetSize(Rml::BoxArea::Border);
    const Rml::Vector2f pointer = source->GetAbsoluteOffset(Rml::BoxArea::Border) + box * 0.5f;
    Session* session = start(source, true, pointer);
    if (!session)
        return false;
    for (UIDataTransferState::Entry& entry : entries){
        if (UIDataTransferState::Entry* existing = session->transfer->find(entry.type))
            *existing = std::move(entry);
        else
            session->transfer->entries.push_back(std::move(entry));
    }
    session->transfer->effectAllowed = effectAllowed.empty() ? "all" : effectAllowed;
    runtime().programmatic = context;

    Rml::Dictionary startParameters = parameters(*session, nullptr);
    bool prevented = false;
    runtime().starting = true;
    source->DispatchEvent(Rml::EventId::Dragstart, startParameters, &prevented);
    runtime().starting = false;
    session = sessionOf(context);
    if (!session)
        return false;
    if (prevented){
        // HTML5: a prevented dragstart starts no drag (no dragend either)
        finish(context);
        return false;
    }
    if (Rml::Element* alive = session->source.get())
        alive->DispatchEvent(Rml::EventId::Drag, parameters(*session, nullptr));
    return sessionOf(context) != nullptr;
}

bool DragDropSession::over(Rml::Element* target){
    Rml::Context* context = runtime().programmatic;
    Session* session = sessionOf(context);
    if (!session || session->ended)
        return false;
    if (target){
        const Rml::Vector2f box = target->GetBox().GetSize(Rml::BoxArea::Border);
        session->pointer = target->GetAbsoluteOffset(Rml::BoxArea::Border) + box * 0.5f;
    }
    if (Rml::Element* source = session->source.get())
        source->DispatchEvent(Rml::EventId::Drag, parameters(*session, nullptr));
    retarget(*session, target);
    dragOver(*session);
    return session->accepted;
}

bool DragDropSession::drop(Rml::Element* target){
    Rml::Context* context = runtime().programmatic;
    Session* session = sessionOf(context);
    if (!session || session->ended)
        return false;
    if (target && target != session->hovered.get())
        over(target);
    session = sessionOf(context);
    if (!session)
        return false;
    dropOn(*session, session->hovered.get());
    const bool dropped = session->dropped;
    Rml::Element* source = session->source.get();
    endDrag(*session, false);
    if (source){
        Rml::Dictionary endParameters = parameters(*session, nullptr);
        source->DispatchEvent(Rml::EventId::Dragend, endParameters);
    }
    finish(context);
    return dropped;
}

void DragDropSession::cancel(){
    Runtime& r = runtime();
    if (Rml::Context* context = r.programmatic){
        Session* session = sessionOf(context);
        if (session && !session->ended){
            Rml::Element* source = session->source.get();
            endDrag(*session, true);
            if (source){
                Rml::Dictionary endParameters = parameters(*session, nullptr);
                endParameters["cancelled"] = 1;
                source->DispatchEvent(Rml::EventId::Dragend, endParameters);
            }
        }
        finish(context);
        return;
    }
    // a pointer drag: the one of any context
    for (auto& pair : r.sessions){
        if (!pair.second->ended){
            pair.first->CancelDrag();
            return;
        }
    }
}

bool DragDropSession::programmaticActive(){
    Session* session = sessionOf(runtime().programmatic);
    return session && !session->ended;
}

std::shared_ptr<UIDataTransferState> DragDropSession::programmaticTransfer(){
    Session* session = sessionOf(runtime().programmatic);
    return session ? session->transfer : nullptr;
}

bool DragDropSession::isDragSourceIn(const Rml::Element* view){
    for (const auto& pair : runtime().sessions){
        for (const Rml::Element* each = pair.second->source.get(); each; each = each->GetParentNode()){
            if (each == view)
                return true;
        }
    }
    return false;
}

void DragDropSession::update(Rml::Context* context){
    install(context);
    Session* session = sessionOf(context);
    if (!session)
        return;
    if (session->ended){
        finish(context);
        return;
    }
    if (session->programmatic)
        return;
    // RmlUi dropped the drag without a dragend: a dragstart listener prevented it, or the source went
    if (!context->IsDragging() || !session->source.get()){
        session->transfer->dropEffect = "none";
        finish(context);
        return;
    }

    const double now = HtmlUIClock::now();
    const float dt = (float)std::clamp(now - session->lastTime, 0.0, 0.1);
    session->lastTime = now;

    if (session->moved){
        Rml::Element* hovered = context->GetElementAtPoint(session->pointer, session->source.get());
        retarget(*session, hovered);
        dragOver(*session);
    }

    // auto-scroll near a scroll container's edge (the content moves under the pointer: a dragover again)
    Rml::Vector2f velocity(0.0f, 0.0f);
    if (Rml::Element* scroller = scrollerAt(session->hovered.get(), session->pointer, velocity)){
        if (dt > 0.0f){
            if (velocity.y != 0.0f)
                scroller->SetScrollTop(scroller->GetScrollTop() + velocity.y * dt);
            if (velocity.x != 0.0f)
                scroller->SetScrollLeft(scroller->GetScrollLeft() + velocity.x * dt);
            session->moved = true;
        }
    }
}

void DragDropSession::contextDestroyed(Rml::Context* context){
    Runtime& r = runtime();
    r.sessions.erase(context);
    if (r.programmatic == context)
        r.programmatic = nullptr;
}

void DragDropSession::releaseAll(){
    Runtime& r = runtime();
    r.sessions.clear();
    r.programmatic = nullptr;
}

// ---- UIDataTransfer ----

UIDataTransfer UIDataTransferAccess::make(const std::shared_ptr<UIDataTransferState>& state){
    UIDataTransfer transfer;
    transfer.state = state;
    return transfer;
}

UIDataTransferState* UIDataTransferAccess::state(const UIDataTransfer& transfer){
    return (transfer.state && transfer.state->valid) ? transfer.state.get() : nullptr;
}

UIDataTransfer::UIDataTransfer() = default;

bool UIDataTransfer::isValid() const{
    return state && state->valid;
}

void UIDataTransfer::setData(const std::string& type, const std::string& value){
    if (!isValid() || type.empty())
        return;
    UIDataTransferState::Entry* entry = state->find(type);
    if (!entry){
        state->entries.emplace_back();
        entry = &state->entries.back();
        entry->type = type;
    }
    entry->text = value;
    entry->lua.reset();
}

std::string UIDataTransfer::getData(const std::string& type) const{
    if (!isValid())
        return std::string();
    const UIDataTransferState::Entry* entry = state->find(type);
    return (entry && !entry->lua.isSet()) ? entry->text : std::string();
}

bool UIDataTransfer::hasType(const std::string& type) const{
    return isValid() && state->find(type) != nullptr;
}

std::vector<std::string> UIDataTransfer::getTypes() const{
    std::vector<std::string> types;
    if (isValid()){
        for (const UIDataTransferState::Entry& entry : state->entries)
            types.push_back(entry.type);
    }
    return types;
}

void UIDataTransfer::clearData(const std::string& type){
    if (!isValid())
        return;
    auto& entries = state->entries;
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const UIDataTransferState::Entry& entry){
        return entry.type != kRowType && (type.empty() || entry.type == type);
    }), entries.end());
}

std::string UIDataTransfer::getEffectAllowed() const{
    return isValid() ? state->effectAllowed : std::string("none");
}

void UIDataTransfer::setEffectAllowed(const std::string& effect){
    static const char* const allowed[] = {"none", "copy", "move", "link", "copyMove", "copyLink", "linkMove", "all", "uninitialized"};
    if (!isValid())
        return;
    for (const char* name : allowed){
        if (effect == name){
            state->effectAllowed = effect;
            return;
        }
    }
}

std::string UIDataTransfer::getDropEffect() const{
    return isValid() ? state->dropEffect : std::string("none");
}

void UIDataTransfer::setDropEffect(const std::string& effect){
    if (isValid() && (effect == "none" || effect == "copy" || effect == "move" || effect == "link"))
        state->dropEffect = effect;
}

void UIDataTransfer::setDragImage(const UIElement& element, float offsetX, float offsetY){
    if (!isValid())
        return;
    Rml::Element* image = UIElementAccess::get(element);
    state->image = image ? image->GetObserverPtr() : Rml::ObserverPtr<Rml::Element>();
    state->imageSet = true;
    state->imageX = offsetX;
    state->imageY = offsetY;
}

void UIDataTransfer::clearDragImage(){
    if (!isValid())
        return;
    state->image.reset();
    state->imageSet = true;
}

// ---- UIDrag ----

bool UIDrag::begin(const UIElement& source, const UIDragOptions& options){
    std::vector<UIDataTransferState::Entry> entries;
    for (const auto& pair : options.data){
        UIDataTransferState::Entry entry;
        entry.type = pair.first;
        entry.text = pair.second;
        entries.push_back(std::move(entry));
    }
    return DragDropSession::begin(UIElementAccess::get(source), std::move(entries), options.effectAllowed);
}

bool UIDrag::over(const UIElement& target){
    return DragDropSession::over(UIElementAccess::get(target));
}

bool UIDrag::drop(const UIElement& target){
    return DragDropSession::drop(UIElementAccess::get(target));
}

void UIDrag::cancel(){
    DragDropSession::cancel();
}

bool UIDrag::isActive(){
    return DragDropSession::programmaticActive();
}

UIDataTransfer UIDrag::getDataTransfer(){
    return UIDataTransferAccess::make(DragDropSession::programmaticTransfer());
}
