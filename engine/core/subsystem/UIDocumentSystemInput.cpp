// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// UIDocumentSystem input (phase 1, p1-input, spec §3.6.2): HtmlUIInputState, pointer ids,
// touch, wheel, keys, text and IME, and the consume rules.

#include "UIDocumentSystem.h"

#include "Scene.h"
#include "Input.h"
#include "htmlui/HtmlUIContext.h"
#include "htmlui/RmlKeyMap.h"
#include "htmlui/RmlSystem.h"
#include "htmlui/RmlTextInputHandler.h"
#include "util/StringUtils.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <bitset>
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
    };

}

using namespace jasperengine;

namespace {

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
    bool isTextControl(Rml::Element* element, bool withSelect){
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

    // the focused element when it is a real element of a shown document; null for none, the
    // context root or a document
    Rml::Element* focusedElement(Rml::Context* context){
        Rml::Element* focus = context->GetFocusElement();
        if (!focus || focus == context->GetRootElement() || rmlui_dynamic_cast<Rml::ElementDocument*>(focus))
            return nullptr;
        return isInShownDocument(focus) ? focus : nullptr;
    }

}

HtmlUIInputState& UIDocumentSystem::getInputState(){
    if (!inputState)
        inputState = std::make_shared<HtmlUIInputState>();
    return *inputState;
}

bool UIDocumentSystem::eventOnPointerDown(int pointer, int button, float x, float y, int mods, bool touch){
    if (isWorldPointerCaptured(pointer, button, touch)) return dispatchWorldPointer(pointer, button, x, y, mods, PointerPhase::DOWN, touch);
    HtmlUIInputState& state = getInputState();
    bool consumed = false;

    if (hasVisibleDocument()){
        float cx = x, cy = y;
        toContext(cx, cy);
        const int rmlMods = RmlKeyMap::toRmlModifiers(mods);

        if (touch){
            getContext()->processTouchStart(pointer, Rml::Vector2f(cx, cy), rmlMods);
            consumed = getContext()->isPointerOverContent();
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
    if (consumed) textureFocusScreen(); else consumed = dispatchWorldPointer(pointer, button, x, y, mods, PointerPhase::DOWN, touch);
    return consumed;
}

bool UIDocumentSystem::eventOnPointerUp(int pointer, int button, float x, float y, int mods, bool touch){
    if (isWorldPointerCaptured(pointer, button, touch)) return dispatchWorldPointer(pointer, button, x, y, mods, PointerPhase::UP, touch);
    HtmlUIInputState& state = getInputState();
    std::set<int>& captures = touch ? state.capturedTouches : state.capturedButtons;
    const int captureKey = touch ? pointer : button;
    const bool captured = captures.erase(captureKey) > 0;
    bool consumed = captured;

    // a release after a consumed press always reaches the context, so nothing stays active
    if (getContext() && (captured || hasVisibleDocument())){
        float cx = x, cy = y;
        toContext(cx, cy);
        const int rmlMods = RmlKeyMap::toRmlModifiers(mods);

        if (touch){
            if (getContext()->hasTouch(pointer)){
                if (getContext()->processTouchEnd(pointer, Rml::Vector2f(cx, cy), rmlMods))
                    consumed = true;
            }
        }else{
            const int rmlButton = toRmlButton(button);
            if (rmlButton >= 0){
                getContext()->processMouseMove(Rml::Vector2f(cx, cy), rmlMods);
                if (getContext()->isPointerOverContent())
                    consumed = true;
                getContext()->processMouseButtonUp(rmlButton, rmlMods);
            }
        }
    }

    if (!consumed) consumed = dispatchWorldPointer(pointer, button, x, y, mods, PointerPhase::UP, touch);
    return consumed;
}

bool UIDocumentSystem::eventOnPointerMove(int pointer, float x, float y, int mods, bool touch){
    if (isWorldPointerCaptured(pointer, -1, touch)) return dispatchWorldPointer(pointer, -1, x, y, mods, PointerPhase::MOVE, touch);
    HtmlUIInputState& state = getInputState();
    bool consumed = false;

    if (hasVisibleDocument()){
        float cx = x, cy = y;
        toContext(cx, cy);
        const int rmlMods = RmlKeyMap::toRmlModifiers(mods);

        if (touch){
            // fingers that started outside the documents do not hover
            if (getContext()->hasTouch(pointer)){
                consumed = getContext()->processTouchMove(pointer, Rml::Vector2f(cx, cy), rmlMods) ||
                           state.capturedTouches.count(pointer) > 0;
            }
        }else{
            consumed = getContext()->processMouseMove(Rml::Vector2f(cx, cy), rmlMods) || !state.capturedButtons.empty();
        }
    }

    if (!consumed) consumed = dispatchWorldPointer(pointer, -1, x, y, mods, PointerPhase::MOVE, touch); else if (!touch) textureMouseLeave();
    return consumed;
}

bool UIDocumentSystem::eventOnTouchCancel(int pointer){
    HtmlUIInputState& state = getInputState();
    const bool captured = state.capturedTouches.erase(pointer) > 0;

    if (getContext() && getContext()->hasTouch(pointer))
        getContext()->processTouchCancel(pointer);

    return dispatchWorldPointer(pointer, -1, 0.0f, 0.0f, 0, PointerPhase::CANCEL, true) || captured;
}

bool UIDocumentSystem::eventOnWheel(float dx, float dy, int mods){
    if (!hasVisibleDocument())
        return dispatchWorldWheel(dx, dy, mods);

    // false: an element scrolled or stopped the event
    const bool notScrolled = getContext()->processMouseWheel(Rml::Vector2f(dx, dy), RmlKeyMap::toRmlModifiers(mods));
    return !notScrolled || getContext()->isPointerOverContent() || dispatchWorldWheel(dx, dy, mods);
}

void UIDocumentSystem::eventOnMouseLeave(){
    if (getContext())
        getContext()->processMouseLeave();
    textureMouseLeave();
}

bool UIDocumentSystem::eventOnKeyDown(int key, bool repeat, int mods){
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

        // false: propagation was stopped (a text field handling the key) or the default prevented
        const bool propagated = rmlContext->ProcessKeyDown(RmlKeyMap::toRmlKey(key), RmlKeyMap::toRmlModifiers(mods));

        if (isTextControl(focus, true)){
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
    if (!hasFocusedTextInput())
        return false;

    return keyContext()->getTextInputHandler().setComposition(utf8, cursor);
}

void UIDocumentSystem::eventOnImeCancel(){
    if (getContext())
        getContext()->getTextInputHandler().cancelComposition();
    if (HtmlUIContext* keys = keyContext())
        keys->getTextInputHandler().cancelComposition();
}

bool UIDocumentSystem::hasFocusedTextInput() const{
    HtmlUIContext* keys = keyContext();
    if (!keys)
        return false;

    return isTextControl(focusedElement(keys->getRmlContext()), false);
}

bool UIDocumentSystem::hasPointerCapture(int pointer, int button, bool touch) const{
    if (isWorldPointerCaptured(pointer, button, touch)) return true;
    if (!inputState)
        return false;
    return touch ? inputState->capturedTouches.count(pointer) > 0 : inputState->capturedButtons.count(button) > 0;
}

CursorType UIDocumentSystem::getPointerCursor() const{
    if (!getContext() || !getContext()->isPointerOverContent())
        return CursorType::ARROW;

    Rml::Element* hover = getContext()->getRmlContext()->GetHoverElement();
    if (!hover)
        return CursorType::ARROW;

    return RmlSystem::cursorFromCss(hover->GetComputedValues().cursor());
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
