// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RmlTextInputHandler.h"

#include "Engine.h"
#include "System.h"
#include "util/StringUtils.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/TextInputContext.h>

using namespace jasperengine;

namespace {

    RmlTextInputHandler* activeHandler = nullptr;

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

    // the focused element is the field being activated
    std::string value;
    if (context){
        if (Rml::Element* focus = context->GetFocusElement())
            value = focus->GetAttribute<Rml::String>("value", "");
    }
    bool hadInvalid = false;
    System::instance().showVirtualKeyboard(StringUtils::utf8ToWString(value, hadInvalid));
}

void RmlTextInputHandler::OnDeactivate(Rml::TextInputContext* inputContext){
    if (this->inputContext != inputContext)
        return;

    if (composing)
        cancelComposition();

    this->inputContext = nullptr;
    if (activeHandler == this){
        activeHandler = nullptr;
        System::instance().hideVirtualKeyboard();
    }
}

void RmlTextInputHandler::OnDestroy(Rml::TextInputContext* inputContext){
    if (this->inputContext != inputContext)
        return;

    // the field is going away: nothing to restore in it
    composing = false;
    cursorPosition = -1;
    compositionStart = 0;
    compositionEnd = 0;

    this->inputContext = nullptr;
    if (activeHandler == this){
        activeHandler = nullptr;
        System::instance().hideVirtualKeyboard();
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
