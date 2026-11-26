// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// UIDocumentSystem input (phase 1, p1-input, spec §3.6.2): HtmlUIInputState, pointer ids,
// touch, wheel, keys, text and IME, and the consume rules. Gamepad keys: phase 4, p4-input.

#include "UIDocumentSystem.h"

#include "Scene.h"
#include "Input.h"
#include "htmlui/HtmlUIContext.h"
#include "htmlui/RmlKeyMap.h"
#include "htmlui/RmlSystem.h"
#include "htmlui/RmlTextInputHandler.h"
#include "htmlui/compat/runtime/HtmlDialogElement.h"
#include "htmlui/compat/VirtualFiles.h"
#include "htmlui/TextEntryTraits.h"
#include "htmlui/widgets/ItemsViewElement.h"
#include "util/StringUtils.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/ScrollTypes.h>

#include <bitset>
#include <climits>
#include <set>
#include <vector>

namespace jasperengine{

    struct HtmlUIInputState {
        // mouse buttons and touch pointers whose down a document consumed: their moves and
        // up stay the document's until the release
        std::set<int> capturedButtons;
        std::set<int> capturedTouches;
        // keys whose down a document consumed: their up is consumed too
        std::bitset<D_KEY_LAST + 1> consumedKeys;
        // Control characters (\b \r \n \t ESC) are never inserted as text. One whose key down
        // reached the documents in the same frame is dropped; one without (Android's soft
        // backspace) is sent as that key at the next update. Counted per key, both ways round:
        // the editor sends the character before the key down.
        std::vector<int> pendingControlKeys;
        std::vector<int> controlKeyDowns;
        // p4-input: the gamepad keys whose down this system dispatched, until their up
        struct GamepadKey {
            int gamepad;
            GamepadUIKey key;
            Rml::Input::KeyIdentifier rmlKey;          // what the controls saw on the down; the up repeats it
            bool consumed;
            Rml::ObserverPtr<Rml::Element> focus;      // the focused element of the down
            Rml::ObserverPtr<Rml::Element> active;     // A: the element held :active until the up
        };
        std::vector<GamepadKey> gamepadKeys;
    };

}

using namespace jasperengine;

namespace {

    // a mouse button is down (Input's level): one no document captured is the drag of whoever
    // got its press (the game, the component UI, a document on a mesh)
    bool anyMouseButtonDown(){
        for (int button = 0; button <= D_MOUSE_BUTTON_LAST; button++){
            if (Input::getMouseButton(button))
                return true;
        }
        return false;
    }

    // engine mouse buttons to RmlUi's (0 left, 1 right, 2 middle); -1 for the others
    int toRmlButton(int button){
        switch (button){
            case D_MOUSE_BUTTON_LEFT: return 0;
            case D_MOUSE_BUTTON_RIGHT: return 1;
            case D_MOUSE_BUTTON_MIDDLE: return 2;
            default: return -1;
        }
    }

    // the key a control character stands for; D_KEY_UNKNOWN for other characters
    int controlCharacterKey(uint32_t codepoint){
        switch (codepoint){
            case '\b': return D_KEY_BACKSPACE;
            case '\r': case '\n': return D_KEY_ENTER;
            case '\t': return D_KEY_TAB;
            case 0x1B: return D_KEY_ESCAPE;
            default: return D_KEY_UNKNOWN;
        }
    }

    bool isControlKey(int key){
        return key == D_KEY_BACKSPACE || key == D_KEY_ENTER || key == D_KEY_KP_ENTER || key == D_KEY_TAB || key == D_KEY_ESCAPE;
    }

    // KP_ENTER produces '\r' too
    int controlKeyClass(int key){
        return (key == D_KEY_KP_ENTER) ? D_KEY_ENTER : key;
    }

    // takes one occurrence of key out of keys; false when there was none
    bool takeKey(std::vector<int>& keys, int key){
        for (size_t i = 0; i < keys.size(); i++){
            if (keys[i] == key){
                keys.erase(keys.begin() + i);
                return true;
            }
        }
        return false;
    }

    void appendUtf8(std::string& out, uint32_t codepoint){
        if (codepoint < 0x80){
            out += (char)codepoint;
        }else if (codepoint < 0x800){
            out += (char)(0xC0 | (codepoint >> 6));
            out += (char)(0x80 | (codepoint & 0x3F));
        }else if (codepoint < 0x10000){
            out += (char)(0xE0 | (codepoint >> 12));
            out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
            out += (char)(0x80 | (codepoint & 0x3F));
        }else{
            out += (char)(0xF0 | (codepoint >> 18));
            out += (char)(0x80 | ((codepoint >> 12) & 0x3F));
            out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
            out += (char)(0x80 | (codepoint & 0x3F));
        }
    }

    // an element of a shown document (focus can stay on an element of a hidden one)
    bool isInShownDocument(Rml::Element* element){
        Rml::ElementDocument* document = element ? element->GetOwnerDocument() : nullptr;
        return document && document->GetParentNode() && document->IsVisible();
    }

    // <input> of a text type or <textarea>; withSelect adds <select>, which takes keys but no text
    bool inputIsTextControl(Rml::Element* element, bool withSelect){
        if (!element)
            return false;

        const Rml::String& tag = element->GetTagName();
        if (tag == "textarea")
            return true;
        if (withSelect && tag == "select")
            return true;
        if (tag != "input")
            return false;

        const Rml::String type = element->GetAttribute<Rml::String>("type", "text");
        return type == "text" || type == "password" || type == "search" || type == "email" ||
               type == "url" || type == "tel" || type == "number";
    }

    bool isInputOfType(Rml::Element* element, std::initializer_list<const char*> types){
        if (!element || element->GetTagName() != "input")
            return false;
        const Rml::String type = element->GetAttribute<Rml::String>("type", "text");
        for (const char* candidate : types){
            if (type == candidate)
                return true;
        }
        return false;
    }

    bool isGamepadDirection(GamepadUIKey key){
        return key >= GamepadUIKey::DPAD_UP && key <= GamepadUIKey::LEFT_STICK_RIGHT;
    }

    Rml::NavigationSearchDirection navigationDirection(GamepadUIKey key){
        switch (key){
            case GamepadUIKey::DPAD_UP: case GamepadUIKey::LEFT_STICK_UP: return Rml::NavigationSearchDirection::Up;
            case GamepadUIKey::DPAD_DOWN: case GamepadUIKey::LEFT_STICK_DOWN: return Rml::NavigationSearchDirection::Down;
            case GamepadUIKey::DPAD_LEFT: case GamepadUIKey::LEFT_STICK_LEFT: return Rml::NavigationSearchDirection::Left;
            default: return Rml::NavigationSearchDirection::Right;
        }
    }

    Rml::Input::KeyIdentifier arrowKey(GamepadUIKey key){
        switch (navigationDirection(key)){
            case Rml::NavigationSearchDirection::Up: return Rml::Input::KI_UP;
            case Rml::NavigationSearchDirection::Down: return Rml::Input::KI_DOWN;
            case Rml::NavigationSearchDirection::Left: return Rml::Input::KI_LEFT;
            default: return Rml::Input::KI_RIGHT;
        }
    }

    // The keyboard key the controls of the focused element see for a gamepad key (the event's
    // key_identifier; scripts see the gamepad's key and code). Directions reach only the controls
    // that use them while focused (a range adjusts, an open select moves through its options);
    // everywhere else they navigate, so a text field or a closed select does not keep the d-pad.
    // A is Space on a checkbox or radio (it toggles on release), nothing on a text field (no line
    // break, no submit), else Enter (a click on press); B is Escape (a modal <dialog> closes). Inside
    // a virtual view (S17 R8) the directions are its arrow keys; at its edges it leaves them to the
    // spatial navigation
    Rml::Input::KeyIdentifier gamepadControlKey(GamepadUIKey key, Rml::Element* focus, bool selectOpen){
        if (isGamepadDirection(key))
            return (selectOpen || isInputOfType(focus, {"range"}) || htmlui::ItemsViewElement::closestView(focus)) ? arrowKey(key) : Rml::Input::KI_UNKNOWN;
        if (key == GamepadUIKey::A){
            if (inputIsTextControl(focus, false))
                return Rml::Input::KI_UNKNOWN;
            return isInputOfType(focus, {"checkbox", "radio"}) ? Rml::Input::KI_SPACE : Rml::Input::KI_RETURN;
        }
        if (key == GamepadUIKey::B)
            return Rml::Input::KI_ESCAPE;
        return Rml::Input::KI_UNKNOWN;
    }

    // a keydown/keyup of a gamepad key: key_identifier for the controls, the modifiers of a key
    // event (none), and the gamepad parameters UIEvent reads (key, code, gamepad, repeat); the fork's
    // text widget leaves events with "gamepad" to the document (P4-3)
    bool dispatchGamepadKey(Rml::Element* target, Rml::EventId id, Rml::Input::KeyIdentifier rmlKey, int gamepad, GamepadUIKey key,
                            bool repeat, bool* defaultPrevented){
        static const char* const modifiers[] = {"ctrl_key", "shift_key", "alt_key", "meta_key", "caps_lock_key", "num_lock_key", "scroll_lock_key"};
        Rml::Dictionary parameters;
        parameters["key_identifier"] = (int)rmlKey;
        for (const char* modifier : modifiers)
            parameters[modifier] = 0;
        parameters["gamepad"] = gamepad;
        parameters["gamepad_key"] = (int)key;
        parameters["repeat"] = repeat ? 1 : 0;
        bool prevented = false;
        const bool propagated = target->DispatchEvent(id, parameters, &prevented);
        if (defaultPrevented)
            *defaultPrevented = prevented;
        return propagated;
    }

    // An Enter keydown carrying event.enterKeyHint (the soft keyboard's action key, a field's
    // enterkeyhint); true when no listener prevented it, so the hint's default action runs
    bool dispatchEnterKey(Rml::Element* target, Rml::Input::KeyIdentifier rmlKey, int rmlModifiers, EnterKey hint){
        static const char* const modifiers[] = {"ctrl_key", "shift_key", "alt_key", "meta_key", "caps_lock_key", "num_lock_key", "scroll_lock_key"};
        Rml::Dictionary parameters;
        parameters["key_identifier"] = (int)rmlKey;
        for (int i = 0; i < 7; i++)
            parameters[modifiers[i]] = (int)((rmlModifiers & (1 << i)) != 0);
        parameters["enterKeyHint"] = Rml::String(enterKeyName(hint));
        bool prevented = false;
        target->DispatchEvent(Rml::EventId::Keydown, parameters, &prevented);
        return !prevented;
    }

    // the focused element when it is a real element of a shown document; null for none, the
    // context root or a document
    Rml::Element* focusedElement(Rml::Context* context){
        Rml::Element* focus = context->GetFocusElement();
        if (!focus || focus == context->GetRootElement() || rmlui_dynamic_cast<Rml::ElementDocument*>(focus))
            return nullptr;
        return isInShownDocument(focus) ? focus : nullptr;
    }


    // The element a gamepad navigates from: a focused element Tab reaches (a button, a link, a
    // form control, tabindex="0"). An element a click or a script focused that is not in the Tab
    // order (a clicked panel, tabindex="-1") has the keys but not the gamepad, so a click on a HUD
    // does not take the d-pad from the game. A virtual view's rows have it too: the view is the Tab
    // stop, its active row the focus (S17 R8)
    Rml::Element* gamepadFocusElement(Rml::Context* context){
        Rml::Element* focus = focusedElement(context);
        if (!focus)
            return nullptr;
        return (focus->GetComputedValues().tab_index() == Rml::Style::TabIndex::Auto || htmlui::ItemsViewElement::closestView(focus)) ? focus : nullptr;
    }

}

HtmlUIInputState& UIDocumentSystem::getInputState(){
    if (!inputState)
        inputState = std::make_shared<HtmlUIInputState>();
    return *inputState;
}

bool UIDocumentSystem::eventOnPointerDown(int pointer, int button, float x, float y, int mods, bool touch, bool world){
    // what the event handlers destroy goes when the dispatch is over
    StructuralDeferScope deferScope(*scene);
    if (isWorldPointerCaptured(pointer, button, touch)) return dispatchWorldPointer(pointer, button, x, y, mods, PointerPhase::DOWN, touch);
    HtmlUIInputState& state = getInputState();
    bool consumed = false;

    if (hasVisibleDocument()){
        float cx = x, cy = y;
        toContext(cx, cy);
        const int rmlMods = RmlKeyMap::toRmlModifiers(mods);

        if (touch){
            // each finger its own pointer: over content (or its press) it is the documents'
            consumed = getContext()->processTouchStart(pointer, Rml::Vector2f(cx, cy), rmlMods);
            if (consumed) state.capturedTouches.insert(pointer);
            else state.capturedTouches.erase(pointer);
        }else{
            const int rmlButton = toRmlButton(button);
            if (rmlButton >= 0){
                getContext()->processMouseMove(Rml::Vector2f(cx, cy), rmlMods);
                getContext()->processMouseButtonDown(rmlButton, rmlMods);
                consumed = getContext()->isPointerOverContent();
                if (consumed) state.capturedButtons.insert(button);
                else state.capturedButtons.erase(button);
            }
        }
    }

    // a press on a text field (re)opens the virtual keyboard at the next update
    if (consumed && hasFocusedTextInput())
        getContext()->getTextInputHandler().requestKeyboard();

    // p1-rtt: texture-mode documents get the event here when the screen documents did not take it
    if (consumed) textureFocusScreen(); else if (world) consumed = dispatchWorldPointer(pointer, button, x, y, mods, PointerPhase::DOWN, touch);
    return consumed;
}

bool UIDocumentSystem::eventOnPointerUp(int pointer, int button, float x, float y, int mods, bool touch, bool world){
    // what the event handlers destroy goes when the dispatch is over
    StructuralDeferScope deferScope(*scene);
    if (isWorldPointerCaptured(pointer, button, touch)) return dispatchWorldPointer(pointer, button, x, y, mods, PointerPhase::UP, touch);
    HtmlUIInputState& state = getInputState();
    std::set<int>& captures = touch ? state.capturedTouches : state.capturedButtons;
    const int captureKey = touch ? pointer : button;
    // the press decided the owner (S93 R1): a release is the documents' only when they captured
    // its press, wherever it lands; the release of a pointer the game (or the component UI) got
    // still reaches the context, for its hover and pointerup, but goes on to its owner
    const bool captured = captures.erase(captureKey) > 0;
    bool consumed = captured;

    // a release after a consumed press always reaches the context, so nothing stays active
    if (getContext() && (captured || hasVisibleDocument())){
        float cx = x, cy = y;
        toContext(cx, cy);
        const int rmlMods = RmlKeyMap::toRmlModifiers(mods);

        if (touch){
            if (getContext()->hasTouch(pointer))
                getContext()->processTouchEnd(pointer, Rml::Vector2f(cx, cy), rmlMods);
        }else{
            const int rmlButton = toRmlButton(button);
            if (rmlButton >= 0){
                getContext()->processMouseMove(Rml::Vector2f(cx, cy), rmlMods);
                getContext()->processMouseButtonUp(rmlButton, rmlMods);
            }
        }
    }

    if (!consumed && world) consumed = dispatchWorldPointer(pointer, button, x, y, mods, PointerPhase::UP, touch);
    return consumed;
}

bool UIDocumentSystem::eventOnPointerMove(int pointer, float x, float y, int mods, bool touch, bool world){
    // what the event handlers destroy goes when the dispatch is over
    StructuralDeferScope deferScope(*scene);
    if (isWorldPointerCaptured(pointer, -1, touch)) return dispatchWorldPointer(pointer, -1, x, y, mods, PointerPhase::MOVE, touch);
    HtmlUIInputState& state = getInputState();
    // the moves of a pointer a document captured at its press are the documents' until its
    // release (S93 R1), also over the game or another document
    bool consumed = touch ? state.capturedTouches.count(pointer) > 0 : !state.capturedButtons.empty();

    if (hasVisibleDocument()){
        float cx = x, cy = y;
        toContext(cx, cy);
        const int rmlMods = RmlKeyMap::toRmlModifiers(mods);

        if (touch){
            // fingers that started outside the documents do not hover; the others hover (an
            // element that reacts to a finger sliding onto it sees it) but a finger the game got
            // at its press stays the game's
            if (getContext()->hasTouch(pointer))
                getContext()->processTouchMove(pointer, Rml::Vector2f(cx, cy), rmlMods);
        }else{
            // hover over content takes a free mouse; a button held since a press the documents
            // did not take keeps its drag with whoever got that press
            const bool overContent = getContext()->processMouseMove(Rml::Vector2f(cx, cy), rmlMods);
            if (overContent && !anyMouseButtonDown())
                consumed = true;
        }
    }

    if (!consumed){ if (world) consumed = dispatchWorldPointer(pointer, -1, x, y, mods, PointerPhase::MOVE, touch); }else if (!touch) textureMouseLeave();
    return consumed;
}

bool UIDocumentSystem::eventOnTouchCancel(int pointer){
    // what the event handlers destroy goes when the dispatch is over
    StructuralDeferScope deferScope(*scene);
    HtmlUIInputState& state = getInputState();
    const bool captured = state.capturedTouches.erase(pointer) > 0;

    if (getContext() && getContext()->hasTouch(pointer))
        getContext()->processTouchCancel(pointer);

    return dispatchWorldPointer(pointer, -1, 0.0f, 0.0f, 0, PointerPhase::CANCEL, true) || captured;
}

bool UIDocumentSystem::eventOnMouseCancel(int button){
    HtmlUIInputState& state = getInputState();
    // one DOM pointer: its cancel lets go of every button
    const bool captured = !state.capturedButtons.empty();
    state.capturedButtons.clear();

    if (getContext())
        getContext()->processMouseCancel();

    bool consumed = captured;
    if (isWorldPointerCaptured(0, button, false))
        consumed = dispatchWorldPointer(0, button, 0.0f, 0.0f, 0, PointerPhase::CANCEL, false) || consumed;
    return consumed;
}

void UIDocumentSystem::cancelAllPointers(){
    HtmlUIInputState& state = getInputState();

    std::set<int> fingers = state.capturedTouches;
    if (getContext()){
        for (int pointer : getContext()->getTouches())
            fingers.insert(pointer);
    }
    for (int pointer : fingers)
        eventOnTouchCancel(pointer);

    state.capturedButtons.clear();
    if (getContext())
        getContext()->processMouseCancel();
    textureCancelAll();
}

bool UIDocumentSystem::eventOnWheel(float dx, float dy, int mods){
    // what the event handlers destroy goes when the dispatch is over
    StructuralDeferScope deferScope(*scene);
    if (!hasVisibleDocument())
        return dispatchWorldWheel(dx, dy, mods);

    // false: an element scrolled or stopped the event
    const bool notScrolled = getContext()->processMouseWheel(Rml::Vector2f(dx, dy), RmlKeyMap::toRmlModifiers(mods));
    return !notScrolled || getContext()->isPointerOverContent() || dispatchWorldWheel(dx, dy, mods);
}

void UIDocumentSystem::eventOnMouseLeave(){
    // what the event handlers destroy goes when the dispatch is over
    StructuralDeferScope deferScope(*scene);
    if (getContext())
        getContext()->processMouseLeave();
    textureMouseLeave();
}

bool UIDocumentSystem::eventOnKeyDown(int key, bool repeat, int mods){
    // what the event handlers destroy goes when the dispatch is over
    StructuralDeferScope deferScope(*scene);
    (void)repeat;   // RmlUi has no repeat flag: every repeat is a key down, as in a browser
    HtmlUIContext* keys = keyContext();   // p1-rtt: the screen's, or the texture document that took the last press
    if (!keys){
        if (inputState && key >= 0 && key <= D_KEY_LAST)
            inputState->consumedKeys.reset(key);
        return false;
    }

    HtmlUIInputState& state = getInputState();

    if (isControlKey(key))
        state.controlKeyDowns.push_back(controlKeyClass(key));

    bool consumed = false;
    {
        Rml::Context* rmlContext = keys->getRmlContext();
        Rml::Element* focus = focusedElement(rmlContext);

        // Enter with a hint on a text field: cancelable, then the hint's default action (text-entry R3, D2)
        if ((key == D_KEY_ENTER || key == D_KEY_KP_ENTER) && htmlui::isTextEntryField(focus)){
            EnterKey hint = EnterKey::Default;
            const bool explicitHint = htmlui::explicitEnterKeyHint(focus, hint);
            const EnterKey* action = htmlui::actionKeyInFlight();
            if (action || explicitHint){
                if (action && (*action != EnterKey::Default || !explicitHint))
                    hint = *action;
                Rml::ObserverPtr<Rml::Element> field = focus->GetObserverPtr();
                if (dispatchEnterKey(focus, RmlKeyMap::toRmlKey(key), RmlKeyMap::toRmlModifiers(mods), hint) && field)
                    htmlui::runEnterKeyAction(field.get(), hint);
                if (key >= 0 && key <= D_KEY_LAST)
                    state.consumedKeys.set(key, true);
                return true;
            }
        }

        // false: propagation was stopped (a text field handling the key) or the default prevented
        const bool propagated = rmlContext->ProcessKeyDown(RmlKeyMap::toRmlKey(key), RmlKeyMap::toRmlModifiers(mods));

        if (inputIsTextControl(focus, true)){
            // a text field takes every key but Escape and the function keys. RmlUi's text
            // widget stops the propagation of every key it sees, so that cannot tell here
            consumed = key != D_KEY_ESCAPE && !(key >= D_KEY_F1 && key <= D_KEY_F25);
        }else if (focus && !propagated){
            consumed = true;
        }
    }

    if (key >= 0 && key <= D_KEY_LAST)
        state.consumedKeys.set(key, consumed);

    return consumed;
}

bool UIDocumentSystem::eventOnKeyUp(int key, int mods){
    // what the event handlers destroy goes when the dispatch is over
    StructuralDeferScope deferScope(*scene);
    bool consumed = false;
    if (inputState && key >= 0 && key <= D_KEY_LAST){
        consumed = inputState->consumedKeys.test(key);
        inputState->consumedKeys.reset(key);
    }

    if (HtmlUIContext* keys = keyContext())
        keys->getRmlContext()->ProcessKeyUp(RmlKeyMap::toRmlKey(key), RmlKeyMap::toRmlModifiers(mods));

    return consumed;
}

bool UIDocumentSystem::eventOnTextInput(const std::string& utf8){
    // what the event handlers destroy goes when the dispatch is over
    StructuralDeferScope deferScope(*scene);
    if (!hasFocusedTextInput())
        return false;

    HtmlUIInputState& state = getInputState();

    bool hadInvalid = false;
    std::string text;
    for (uint32_t codepoint : StringUtils::decodeUtf8ToCodepoints(utf8, hadInvalid)){
        const int controlKey = controlCharacterKey(codepoint);
        if (controlKey != D_KEY_UNKNOWN){
            if (!takeKey(state.controlKeyDowns, controlKey))
                state.pendingControlKeys.push_back(controlKey);
            continue;
        }
        // other C0 controls and DEL are never text
        if (codepoint < 0x20 || codepoint == 0x7F)
            continue;
        appendUtf8(text, codepoint);
    }

    if (!text.empty()){
        RmlTextInputHandler& handler = keyContext()->getTextInputHandler();
        if (handler.isComposing()){
            // a commit while composing replaces the composition
            handler.confirmComposition(text);
        }else{
            keyContext()->getRmlContext()->ProcessTextInput(text);
        }
    }

    return true;
}

bool UIDocumentSystem::eventOnImeComposition(const std::string& utf8, int cursor){
    // what the event handlers destroy goes when the dispatch is over
    StructuralDeferScope deferScope(*scene);
    if (!hasFocusedTextInput())
        return false;

    return keyContext()->getTextInputHandler().setComposition(utf8, cursor);
}

void UIDocumentSystem::eventOnImeCancel(){
    // what the event handlers destroy goes when the dispatch is over
    StructuralDeferScope deferScope(*scene);
    if (getContext())
        getContext()->getTextInputHandler().cancelComposition();
    if (HtmlUIContext* keys = keyContext())
        keys->getTextInputHandler().cancelComposition();
}

bool UIDocumentSystem::hasFocusedTextInput() const{
    HtmlUIContext* keys = keyContext();
    if (!keys)
        return false;

    return inputIsTextControl(focusedElement(keys->getRmlContext()), false);
}

bool UIDocumentSystem::hasPointerCapture(int pointer, int button, bool touch) const{
    if (isWorldPointerCaptured(pointer, button, touch)) return true;
    if (!inputState)
        return false;
    return touch ? inputState->capturedTouches.count(pointer) > 0 : inputState->capturedButtons.count(button) > 0;
}

bool UIDocumentSystem::isPointerOverUI(int pointerId) const{
    const bool mouse = pointerId == INT_MIN || pointerId < 0;
    const bool fingers = pointerId == INT_MIN || pointerId >= 0;
    if (mouse){
        if (inputState && !inputState->capturedButtons.empty())
            return true;
        if (hasVisibleDocument() && getContext() && getContext()->isPointerOverContent())
            return true;
    }
    if (fingers && inputState){
        if (pointerId == INT_MIN ? !inputState->capturedTouches.empty() : inputState->capturedTouches.count(pointerId) > 0)
            return true;
    }
    if (fingers && pointerId != INT_MIN && hasVisibleDocument() && getContext() && getContext()->isTouchOverContent(pointerId))
        return true;
    return false;
}

CursorType UIDocumentSystem::getPointerCursor() const{
    if (!getContext() || !getContext()->isPointerOverContent())
        return CursorType::ARROW;

    Rml::Element* hover = getContext()->getRmlContext()->GetHoverElement();
    if (!hover)
        return CursorType::ARROW;

    return RmlSystem::cursorFromCss(hover->GetComputedValues().cursor());
}

bool UIDocumentSystem::eventOnGamepadKeyDown(int gamepad, GamepadUIKey key, bool repeat){
    // what the event handlers destroy goes when the dispatch is over
    StructuralDeferScope deferScope(*scene);
    HtmlUIContext* keys = keyContext();   // the context keys go to (a texture document's after its press)
    if (!keys || key == GamepadUIKey::COUNT)
        return false;

    HtmlUIInputState& state = getInputState();
    Rml::Context* rmlContext = keys->getRmlContext();
    Rml::Element* focus = gamepadFocusElement(rmlContext);
    // as a keyboard key: the focused element, else the focused document (or the root)
    Rml::Element* target = rmlContext->GetFocusElement() ? rmlContext->GetFocusElement() : rmlContext->GetRootElement();

    Rml::ElementFormControlSelect* select = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(focus);
    const bool selectOpen = select && select->IsSelectBoxVisible();
    const Rml::Input::KeyIdentifier rmlKey = gamepadControlKey(key, focus, selectOpen);

    // gamepad input shows where the focus is, also on an element a click focused
    if (focus && !focus->IsPseudoClassSet("focus-visible"))
        focus->SetPseudoClass("focus-visible", true);

    // A: :active while held, as a pressed button
    const bool press = key == GamepadUIKey::A && focus && !repeat;
    if (press)
        focus->SetPseudoClass("active", true);

    bool prevented = false;
    const bool propagated = dispatchGamepadKey(target, Rml::EventId::Keydown, rmlKey, gamepad, key, repeat, &prevented);
    if (press && prevented)
        focus->SetPseudoClass("active", false);

    bool consumed = false;
    if (focus){
        // a listener stopped or prevented it, a control used it, or a listener moved the focus
        const bool handled = !propagated || prevented || rmlContext->GetFocusElement() != focus;
        consumed = handled;

        if (isGamepadDirection(key)){
            // a focused document takes the d-pad and the stick, also where nothing is in that direction
            consumed = true;
            if (!handled && !selectOpen){
                Rml::ElementDocument* document = focus->GetOwnerDocument();
                if (Rml::Element* next = document ? document->FindNavigationTarget(focus, navigationDirection(key)) : nullptr){
                    if (next->Focus(true))
                        next->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest, Rml::ScrollAlignment::Nearest));
                }
            }
        }else if (key == GamepadUIKey::A){
            consumed = true;
            // a text field: the virtual keyboard (Android, iOS, Web), as a tap on it
            if (!prevented && inputIsTextControl(focus, false))
                keys->getTextInputHandler().requestKeyboard();
        }else if (key == GamepadUIKey::B){
            // an open select closes, as on Escape in a browser
            if (!handled && selectOpen){
                select->HideSelectBox();
                consumed = true;
            }
        }
    }

    if (!repeat){
        for (auto it = state.gamepadKeys.begin(); it != state.gamepadKeys.end(); ++it){
            if (it->gamepad == gamepad && it->key == key){
                state.gamepadKeys.erase(it);
                break;
            }
        }
        HtmlUIInputState::GamepadKey held{gamepad, key, rmlKey, consumed, Rml::ObserverPtr<Rml::Element>(), Rml::ObserverPtr<Rml::Element>()};
        if (focus)
            held.focus = focus->GetObserverPtr();
        if (press && !prevented)
            held.active = focus->GetObserverPtr();
        state.gamepadKeys.push_back(held);
    }

    return consumed;
}

bool UIDocumentSystem::eventOnGamepadKeyUp(int gamepad, GamepadUIKey key){
    // what the event handlers destroy goes when the dispatch is over
    StructuralDeferScope deferScope(*scene);
    if (!inputState)
        return false;
    HtmlUIInputState& state = *inputState;

    auto it = state.gamepadKeys.begin();
    while (it != state.gamepadKeys.end() && !(it->gamepad == gamepad && it->key == key))
        ++it;
    if (it == state.gamepadKeys.end())
        return false;
    const HtmlUIInputState::GamepadKey held = *it;
    state.gamepadKeys.erase(it);

    if (Rml::Element* active = held.active.get())
        active->SetPseudoClass("active", false);

    if (HtmlUIContext* keys = keyContext()){
        Rml::Context* rmlContext = keys->getRmlContext();
        Rml::Element* target = rmlContext->GetFocusElement() ? rmlContext->GetFocusElement() : rmlContext->GetRootElement();
        // the controls see the up of their down only (a Space up on the next focused button would click it)
        const Rml::Input::KeyIdentifier rmlKey = (target == held.focus.get()) ? held.rmlKey : Rml::Input::KI_UNKNOWN;
        dispatchGamepadKey(target, Rml::EventId::Keyup, rmlKey, gamepad, key, false, nullptr);
    }

    return held.consumed;
}

bool UIDocumentSystem::hasGamepadFocus() const{
    HtmlUIContext* keys = keyContext();
    return keys && gamepadFocusElement(keys->getRmlContext()) != nullptr;
}

namespace {

    // the open modal <dialog> an element sits in, else null
    htmlui::compat::HtmlDialogElement* enclosingModalDialog(Rml::Element* element){
        for (Rml::Element* node = element; node; node = node->GetParentNode()){
            auto* dialog = rmlui_dynamic_cast<htmlui::compat::HtmlDialogElement*>(node);
            if (dialog && dialog->isModal() && dialog->isOpen())
                return dialog;
        }
        return nullptr;
    }

    bool isOpenSelect(Rml::Element* element){
        auto* select = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(element);
        return select && select->IsSelectBoxVisible();
    }

}

bool UIDocumentSystem::hasBackOverlay() const{
    HtmlUIContext* keys = keyContext();
    if (!keys)
        return false;
    Rml::Element* focus = focusedElement(keys->getRmlContext());
    return focus && (isOpenSelect(focus) || enclosingModalDialog(focus));
}

bool UIDocumentSystem::dispatchBack(){
    // the top menu document: the highest sortOrder, then the one that became a menu last
    Entity top = NULL_ENTITY;
    int topSort = 0;
    uint64_t topOrder = 0;
    for (const auto& entry : menuEntities){
        const UIDocumentComponent* component = scene->findComponent<UIDocumentComponent>(entry.first);
        if (!component)
            continue;
        if (top == NULL_ENTITY || component->sortOrder > topSort || (component->sortOrder == topSort && entry.second > topOrder)){
            top = entry.first;
            topSort = component->sortOrder;
            topOrder = entry.second;
        }
    }
    Rml::ElementDocument* document = (top != NULL_ENTITY) ? getDocument(top) : nullptr;
    if (!document || !document->GetContext())
        return false;
    // a document on a mesh hears Back only while it holds the keys
    if (getTextureContext(top) && getTextureFocus() != top)
        return false;

    // what the event handlers destroy goes when the dispatch is over
    StructuralDeferScope deferScope(*scene);
    Rml::Element* focus = focusedElement(document->GetContext());
    if (focus && focus->GetOwnerDocument() != document)
        focus = nullptr;
    Rml::Element* target = focus ? focus : document;
    bool prevented = false;
    target->DispatchEvent(Rml::RegisterEventType("back", true, true), Rml::Dictionary(), &prevented);
    if (prevented)
        return true;

    // the default action: an open select closes, else the topmost open modal dialog
    if (focus && isOpenSelect(focus)){
        rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(focus)->HideSelectBox();
        return true;
    }
    Rml::ElementList dialogs;
    document->GetElementsByTagName(dialogs, "dialog");
    for (auto it = dialogs.rbegin(); it != dialogs.rend(); ++it){
        auto* dialog = rmlui_dynamic_cast<htmlui::compat::HtmlDialogElement*>(*it);
        if (!dialog || !dialog->isModal() || !dialog->isOpen())
            continue;
        // as Escape does: a cancelable `cancel`, then close
        if (dialog->DispatchEvent("cancel", Rml::Dictionary(), true, false))
            dialog->close();
        return !dialog->isOpen();
    }
    return false;
}

void UIDocumentSystem::cancelPointersOutsideMenus(std::vector<int>& keptFingers){
    HtmlUIContext* screen = getContext();
    if (!screen)
        return;
    auto inMenu = [this](Rml::Element* element){
        Rml::ElementDocument* owner = element ? element->GetOwnerDocument() : nullptr;
        for (const auto& entry : menuEntities){
            if (owner && getDocument(entry.first) == owner)
                return true;
        }
        return false;
    };
    Rml::Context* rmlContext = screen->getRmlContext();
    const std::vector<int> fingers = screen->getTouches();
    for (int pointer : fingers){
        if (inMenu(rmlContext->GetHoverElement(HtmlUIContext::touchPointerId(pointer))))
            keptFingers.push_back(pointer);
        else
            eventOnTouchCancel(pointer);
    }
    HtmlUIInputState& state = getInputState();
    if (!state.capturedButtons.empty() && !inMenu(rmlContext->GetHoverElement())){
        state.capturedButtons.clear();
        screen->processMouseCancel();
    }
    // documents on meshes: unless one of them is a menu, what they hold ends too
    bool textureMenu = false;
    for (const auto& entry : menuEntities)
        textureMenu = textureMenu || getTextureContext(entry.first) != nullptr;
    if (!textureMenu)
        textureCancelAll();
}

// ---- the gamepad cursor (GamepadCursor)

namespace {

    // the built-in cursor graphic: an arrow, hot spot at its top-left (engine-authored, CC0)
    const char* const kCursorArrowPath = "@htmlui/gamepad-cursor/arrow.svg";
    const char* const kCursorArrowSvg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"32\" height=\"32\" viewBox=\"0 0 32 32\">"
        "<path d=\"M2 2 L2 26 L8.5 20 L13 30 L17.5 28 L13 18.5 L22 18.5 Z\" fill=\"#ffffff\" stroke=\"#000000\" "
        "stroke-width=\"2\" stroke-linejoin=\"round\"/></svg>";
    // above every document's sort order; never hit (pointer-events)
    const char* const kCursorOverlayRml =
        "<rml><head><style>"
        "body { position: absolute; left: 0; top: 0; width: 100%; height: 100%; pointer-events: none; }"
        "img { position: absolute; left: 0; top: 0; pointer-events: none; }"
        "</style></head><body><img id=\"cursor\"/></body></rml>";
    constexpr float kCursorOverlayZ = 2000000000.0f;
    uint32_t cursorOverlayLoads = 0;

}

uint32_t UIDocumentSystem::getCursorOverlayLoads(){
    return cursorOverlayLoads;
}

Rml::Element* UIDocumentSystem::getMouseHoverElement() const{
    if (!getContext() || !hasVisibleDocument() || !getContext()->isPointerOverContent())
        return nullptr;
    return getContext()->getRmlContext()->GetHoverElement();
}

bool UIDocumentSystem::isMouseOverControl() const{
    return HtmlUIContext::takesPointerInput(getMouseHoverElement());
}

bool UIDocumentSystem::getFocusCenter(float& x, float& y, const void*& focus) const{
    focus = nullptr;
    if (!getContext())
        return false;
    Rml::Element* element = focusedElement(getContext()->getRmlContext());
    if (!element)
        return false;
    const Rml::Vector2f offset = element->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
    x = offset.x + size.x * 0.5f;
    y = offset.y + size.y * 0.5f;
    toCanvas(x, y);
    focus = element;
    return true;
}

bool UIDocumentSystem::focusHoveredElement(){
    if (!getContext() || focusedElement(getContext()->getRmlContext()))
        return false;
    for (Rml::Element* node = getMouseHoverElement(); node && !rmlui_dynamic_cast<Rml::ElementDocument*>(node); node = node->GetParentNode()){
        if (node->GetComputedValues().tab_index() == Rml::Style::TabIndex::Auto)
            return node->Focus(true);
    }
    return false;
}

void UIDocumentSystem::setCursorOverlay(bool show, float x, float y, const std::string& graphic, float size, float hotX, float hotY){
    if (!show){
        if (cursorOverlay && cursorOverlay->IsVisible())
            cursorOverlay->Hide();
        return;
    }
    if (!getContext())
        return;
    if (!cursorOverlay){
        if (!htmlui::compat::VirtualFiles::exists(kCursorArrowPath))
            htmlui::compat::VirtualFiles::add(kCursorArrowPath, kCursorArrowSvg);
        cursorOverlay = getContext()->getRmlContext()->LoadDocumentFromMemory(kCursorOverlayRml, "[gamepad-cursor]");
        if (!cursorOverlay)
            return;
        cursorOverlayLoads++;
        cursorOverlayImage = cursorOverlay->GetElementById("cursor");
        cursorOverlay->SetProperty(Rml::PropertyId::ZIndex, Rml::Property(kCursorOverlayZ, Rml::Unit::NUMBER));
        cursorOverlayGraphic.clear();
        cursorOverlayLook[0] = -1.0f;
    }
    // found once at the load (a lookup by id walks the document with a queue)
    Rml::Element* image = cursorOverlayImage;
    if (!image)
        return;
    // the graphic, its size and hot spot change rarely: set only then; the position each move
    if (graphic != cursorOverlayGraphic || cursorOverlayLook[0] < 0.0f){
        image->SetAttribute("src", graphic.empty() ? std::string("/") + kCursorArrowPath : graphic);
        cursorOverlayGraphic = graphic;
    }
    const bool lookChanged = size != cursorOverlayLook[0] || hotX != cursorOverlayLook[1] || hotY != cursorOverlayLook[2];
    if (lookChanged){
        // CSS px of the document are dp of the context; the hot spot sits on the point
        image->SetProperty(Rml::PropertyId::Width, Rml::Property(size, Rml::Unit::DP));
        image->SetProperty(Rml::PropertyId::Height, Rml::Property(size, Rml::Unit::DP));
        image->SetProperty(Rml::PropertyId::MarginLeft, Rml::Property(-hotX, Rml::Unit::DP));
        image->SetProperty(Rml::PropertyId::MarginTop, Rml::Property(-hotY, Rml::Unit::DP));
        cursorOverlayLook[0] = size;
        cursorOverlayLook[1] = hotX;
        cursorOverlayLook[2] = hotY;
    }
    float cx = x, cy = y;
    toContext(cx, cy);
    // the position (context px) is set only when it moved
    if (lookChanged || cx != cursorOverlayAt[0] || cy != cursorOverlayAt[1]){
        image->SetProperty(Rml::PropertyId::Left, Rml::Property(cx, Rml::Unit::PX));
        image->SetProperty(Rml::PropertyId::Top, Rml::Property(cy, Rml::Unit::PX));
        cursorOverlayAt[0] = cx;
        cursorOverlayAt[1] = cy;
    }
    if (!cursorOverlay->IsVisible())
        cursorOverlay->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
}

void UIDocumentSystem::updateInput(){
    if (getContext())
        getContext()->getTextInputHandler().flushKeyboard();

    if (!inputState)
        return;
    HtmlUIInputState& state = *inputState;

    std::vector<int> pending;
    pending.swap(state.pendingControlKeys);

    // the frame is over: a key down that came after its character in the same frame
    // cancels it, the others become keys of their own
    for (int key : pending){
        if (takeKey(state.controlKeyDowns, key))
            continue;
        if (!hasFocusedTextInput())
            continue;
        Rml::Context* rmlContext = keyContext()->getRmlContext();
        const Rml::Input::KeyIdentifier rmlKey = RmlKeyMap::toRmlKey(key);
        rmlContext->ProcessKeyDown(rmlKey, 0);
        rmlContext->ProcessKeyUp(rmlKey, 0);
    }

    state.controlKeyDowns.clear();
}
