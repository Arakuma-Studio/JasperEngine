// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RMLSYSTEM_H
#define RMLSYSTEM_H

#include "Export.h"
#include "System.h"

#include <RmlUi/Core/SystemInterface.h>

#include <cstddef>
#include <functional>
#include <string>

namespace jasperengine {

    // RmlUi's system interface: time, logging into the engine Log, document-relative paths
    // and the clipboard.
    class JASPERENGINE_INTERNAL RmlSystem : public Rml::SystemInterface {
    public:
        double GetElapsedTime() override;                 // HtmlUIClock::now() (seconds of UI time)
        bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;
        void JoinPath(Rml::String& translated_path, const Rml::String& document_path, const Rml::String& path) override;
        void SetMouseCursor(const Rml::String& cursor_name) override;      // Engine::setMouseCursor(cursorFromCss(name))
        void SetClipboardText(const Rml::String& text) override;           // System::setClipboardText
        void GetClipboardText(Rml::String& text) override;                 // System::getClipboardText
        // the caret of the focused text field moved: IME position (RmlTextInputHandler::setCaret)
        void ActivateKeyboard(Rml::Vector2f caret_position, float line_height) override;
        // nothing: the text input handler hides the keyboard when the field loses focus
        void DeactivateKeyboard() override {}

        // a CSS cursor value to the engine cursor: the first keyword of a comma list the
        // engine knows (default/auto → ARROW, pointer, text, crosshair, move, not-allowed,
        // wait, progress, help, grab, grabbing, the resize family); ARROW when none is known
        static CursorType cursorFromCss(const std::string& value);

        // tests: every message also goes here; counters reset by resetCounters()
        void setLogListener(std::function<void(Rml::Log::Type, const std::string&)> listener);
        size_t getErrorCount() const;    // LT_ERROR + LT_ASSERT
        size_t getWarningCount() const;  // LT_WARNING
        void resetCounters();

        // JoinPath without the RmlUi types: `path` relative to the directory of
        // `documentPath`, keeping its asset:// or data:// scheme
        static std::string joinPath(const std::string& documentPath, const std::string& path);

    private:
        std::function<void(Rml::Log::Type, const std::string&)> logListener;
        size_t errorCount = 0;
        size_t warningCount = 0;
    };

}

#endif //RMLSYSTEM_H
