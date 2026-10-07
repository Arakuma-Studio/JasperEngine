// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RMLTEXTINPUTHANDLER_H
#define RMLTEXTINPUTHANDLER_H

#include "Export.h"

#include "math/Rect.h"

#include <RmlUi/Core/TextInputHandler.h>
#include <RmlUi/Core/Types.h>

#include <functional>
#include <string>

namespace Rml {
    class Context;
    class TextInputContext;
}

namespace jasperengine {

    // The text input handler of one HTML UI context (phase-1 spec §3.6.2): a focused text field
    // opens the virtual keyboard and places the IME at its caret, and IME composition is applied
    // to it. The composition state machine is a port of RmlUi's platform-neutral
    // TextInputMethodEditor_Win32 (Backends/RmlUi_Platform_Win32.cpp, RmlUi 6.3).
    class JASPERENGINE_INTERNAL RmlTextInputHandler : public Rml::TextInputHandler {
    public:
        RmlTextInputHandler();
        ~RmlTextInputHandler() override;

        // the context whose focused element is read on activation (HtmlUIContext::create)
        void setContext(Rml::Context* context);
        // context pixels → canvas pixels, for the caret area (default: identity). The screen
        // context scales by canvas / context size; texture contexts map through their surface.
        void setCanvasMapper(std::function<Rect(const Rect&)> mapper);

        void OnActivate(Rml::TextInputContext* inputContext) override;
        void OnDeactivate(Rml::TextInputContext* inputContext) override;
        void OnDestroy(Rml::TextInputContext* inputContext) override;

        // a text field of this context is active (focused)
        bool isActive() const;
        bool isComposing() const;

        // IME: start or update the composition (cursor in code points from its start, -1
        // selects it all); commit it, replacing it with `utf8`; or drop it, restoring the text
        // it replaced. False when no text field is active.
        bool setComposition(const std::string& utf8, int cursor);
        bool confirmComposition(const std::string& utf8);
        void cancelComposition();

        // RmlSystem::ActivateKeyboard: the caret of the active field, context pixels
        void setCaret(Rml::Vector2f position, float lineHeight);

        // the handler whose field was activated last and is still active; null when none
        static RmlTextInputHandler* getActive();

    private:
        void startComposition();
        void endComposition();
        void setCompositionString(const std::string& utf8);
        void updateCursorPosition();

        Rml::Context* context = nullptr;
        Rml::TextInputContext* inputContext = nullptr;
        std::function<Rect(const Rect&)> canvasMapper;

        bool composing = false;
        int cursorPosition = -1;
        // code points of the field's value the composition occupies
        int compositionStart = 0;
        int compositionEnd = 0;
    };

}

#endif //RMLTEXTINPUTHANDLER_H
