// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RmlTextInputHandler.h"

#include "Engine.h"
#include "System.h"
#include "util/StringUtils.h"
#include "htmlui/TextEntryTraits.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/TextInputContext.h>

using namespace jasperengine;

namespace {

    RmlTextInputHandler* activeHandler = nullptr;

    // The soft keyboard, one for the app: shown for a document field (with these traits), or
    // hidden at the next flush once its field lost the focus and no field took it
    bool keyboardShown = false;
    bool hidePending = false;
    TextEntryTraits shownTraits;
    // what the keyboard holds, as far as the engine knows: the text it was given or last
    // reported, and its selection in code points
    std::string keyboardText;
    int keyboardSelectionStart = 0;
    int keyboardSelectionEnd = 0;

    const Rml::String* valueOf(Rml::Element* field){
        const Rml::Variant* value = field->GetAttribute("value");
        return (value && value->GetType() == Rml::Variant::STRING) ? &value->GetReference<Rml::String>() : nullptr;
    }

    void hideKeyboard(){
        hidePending = false;
        keyboardShown = false;
        System::instance().hideVirtualKeyboard();
    }

}

bool RmlTextInputHandler::isKeyboardShown(){
    return keyboardShown;
}

const TextEntryTraits& RmlTextInputHandler::getKeyboardTraits(){
    return shownTraits;
}

void RmlTextInputHandler::noteKeyboardText(const std::string& utf8, int selectionStart, int selectionEnd){
    if (!activeHandler)
        return;
    keyboardText = utf8;
    keyboardSelectionStart = selectionStart;
    keyboardSelectionEnd = selectionEnd;
}

RmlTextInputHandler::RmlTextInputHandler(){
}

RmlTextInputHandler::~RmlTextInputHandler(){
    if (activeHandler == this)
        activeHandler = nullptr;
}

void RmlTextInputHandler::setContext(Rml::Context* context){
    this->context = context;
}

void RmlTextInputHandler::setCanvasMapper(std::function<Rect(const Rect&)> mapper){
    canvasMapper = std::move(mapper);
}

RmlTextInputHandler* RmlTextInputHandler::getActive(){
    return activeHandler;
}

bool RmlTextInputHandler::isActive() const{
    return inputContext != nullptr;
}

bool RmlTextInputHandler::isComposing() const{
    return composing;
}

void RmlTextInputHandler::OnActivate(Rml::TextInputContext* inputContext){
    // another context's field loses the keyboard without a blur of its own
    if (activeHandler && activeHandler != this && activeHandler->composing){
        activeHandler->cancelComposition();
    }

    this->inputContext = inputContext;
    activeHandler = this;
    keyboardPending = true;
    // a field took the keyboard from the one that blurred: it stays open (R5)
    hidePending = false;
}

void RmlTextInputHandler::requestKeyboard(){
    if (inputContext)
        keyboardPending = true;
}

void RmlTextInputHandler::flushKeyboard(){
    if (hidePending && !activeHandler)
        hideKeyboard();
    if (!inputContext || activeHandler != this || !context)
        return;

    // the focused element is the active field now
    Rml::Element* field = context->GetFocusElement();
    if (!field)
        return;
    if (keyboardPending){
        keyboardPending = false;
        showKeyboard(field);
        return;
    }
    // an attribute changed while focused (a script): the new traits, without hiding (R2, R5)
    if (htmlui::textEntryTraitsOf(field) != shownTraits){
        showKeyboard(field);
        return;
    }
    if (keyboardShown && System::instance().keepsVirtualKeyboardText())
        resyncKeyboard(field);
}

void RmlTextInputHandler::showKeyboard(Rml::Element* field){
    shownTraits = htmlui::textEntryTraitsOf(field);
    const Rml::String* value = valueOf(field);
    keyboardText = value ? *value : Rml::String();
    // the caret goes to the end of the text the keyboard gets
    keyboardSelectionStart = keyboardSelectionEnd = (int)Rml::StringUtilities::LengthUTF8(keyboardText);

    // inputmode="none": no soft keyboard, the field still takes hardware keys
    if (shownTraits.keyboard == KeyboardType::None){
        if (keyboardShown)
            hideKeyboard();
        return;
    }
    keyboardShown = true;
    bool hadInvalid = false;
    const std::wstring text = StringUtils::utf8ToWString(keyboardText, hadInvalid);
    Engine::resetTextInputState(keyboardText, (size_t)keyboardSelectionEnd);
    System::instance().showVirtualKeyboard(text, shownTraits);
}

void RmlTextInputHandler::resyncKeyboard(Rml::Element* field){
    const Rml::String* value = valueOf(field);
    const Rml::String empty;
    const Rml::String& text = value ? *value : empty;
    int start = 0, end = 0;
    inputContext->GetSelectionRange(start, end);

    const bool sameText = text == keyboardText;
    // the caret only when nothing is composed and the keyboard's selection is a caret: the
    // composition and an IME's own selection are its business
    const bool caretMatters = !composing && keyboardSelectionStart == keyboardSelectionEnd;
    if (sameText && (!caretMatters || (start == keyboardSelectionStart && end == keyboardSelectionEnd)))
        return;

    // the field rejected or replaced text the keyboard has (D5): a composition the field did not
    // take is dropped, then the keyboard gets the field's text
    if (!sameText && composing)
        cancelComposition();
    const Rml::String* now = valueOf(field);
    keyboardText = now ? *now : Rml::String();
    inputContext->GetSelectionRange(start, end);
    keyboardSelectionStart = start;
    keyboardSelectionEnd = end;

    bool hadInvalid = false;
    Engine::resetTextInputState(keyboardText, (size_t)end);
    System::instance().updateVirtualKeyboardText(StringUtils::utf8ToWString(keyboardText, hadInvalid), start, end);
}

void RmlTextInputHandler::OnDeactivate(Rml::TextInputContext* inputContext){
    if (this->inputContext != inputContext)
        return;

    if (composing)
        cancelComposition();

    keyboardPending = false;
    this->inputContext = nullptr;
    if (activeHandler == this){
        activeHandler = nullptr;
        // hidden at the flush, unless another field takes the focus first
        hidePending = true;
    }
}

void RmlTextInputHandler::OnDestroy(Rml::TextInputContext* inputContext){
    if (this->inputContext != inputContext)
        return;

    // the field is going away: nothing to restore in it
    keyboardPending = false;
    composing = false;
    cursorPosition = -1;
    compositionStart = 0;
    compositionEnd = 0;

    this->inputContext = nullptr;
    if (activeHandler == this){
        activeHandler = nullptr;
        hideKeyboard();
    }
}

void RmlTextInputHandler::setCaret(Rml::Vector2f position, float lineHeight){
    if (activeHandler != this || !inputContext)
        return;

    Rect area(position.x, position.y, 1.0f, lineHeight);
    if (canvasMapper)
        area = canvasMapper(area);
    Engine::setTextInputArea(area);
}

bool RmlTextInputHandler::setComposition(const std::string& utf8, int cursor){
    if (!inputContext)
        return false;

    if (!composing)
        startComposition();

    setCompositionString(utf8);
    cursorPosition = cursor;
    updateCursorPosition();

    // Editors that work on one character (e.g. Hangul) report no cursor: they get the
    // selection below instead of the composition underline, as in RmlUi's backend
    if (cursorPosition != -1)
        inputContext->SetCompositionRange(compositionStart, compositionEnd);

    return true;
}

bool RmlTextInputHandler::confirmComposition(const std::string& utf8){
    if (!inputContext)
        return false;

    if (!composing)
        startComposition();

    setCompositionString(utf8);
    inputContext->SetCompositionRange(compositionStart, compositionEnd);
    inputContext->CommitComposition(utf8);

    // the caret goes to the end of the committed text
    cursorPosition = compositionEnd - compositionStart;
    updateCursorPosition();
    endComposition();

    return true;
}

void RmlTextInputHandler::cancelComposition(){
    if (!composing)
        return;

    if (inputContext){
        // drop the composition string and put the caret back where it started
        inputContext->SetText(Rml::StringView(), compositionStart, compositionEnd);
        inputContext->SetCursorPosition(compositionStart);
    }
    endComposition();
}

void RmlTextInputHandler::startComposition(){
    composing = true;
    cursorPosition = -1;
    compositionStart = 0;
    compositionEnd = 0;
}

void RmlTextInputHandler::endComposition(){
    if (inputContext)
        inputContext->SetCompositionRange(0, 0);

    composing = false;
    cursorPosition = -1;
    compositionStart = 0;
    compositionEnd = 0;
}

void RmlTextInputHandler::setCompositionString(const std::string& utf8){
    if (!inputContext)
        return;

    // the first string of a composition replaces the selection (or goes in at the caret)
    if (compositionStart == 0 && compositionEnd == 0)
        inputContext->GetSelectionRange(compositionStart, compositionEnd);

    inputContext->SetText(utf8, compositionStart, compositionEnd);
    compositionEnd = compositionStart + (int)Rml::StringUtilities::LengthUTF8(utf8);
}

void RmlTextInputHandler::updateCursorPosition(){
    if (!inputContext || !composing)
        return;

    if (cursorPosition >= 0){
        int position = compositionStart + cursorPosition;
        if (position > compositionEnd)
            position = compositionEnd;
        inputContext->SetCursorPosition(position);
    }else{
        // no cursor from the IME: select the whole composition
        inputContext->SetSelectionRange(compositionStart, compositionEnd);
    }
}
