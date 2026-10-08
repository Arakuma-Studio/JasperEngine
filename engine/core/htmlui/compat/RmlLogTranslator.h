// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_RMLLOGTRANSLATOR_H
#define COMPAT_RMLLOGTRANSLATOR_H

#include "Export.h"
#include "CompatTypes.h"
#include "Diagnostics.h"

#include <RmlUi/Core/Log.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::htmlui::compat {

    class SourceFiles;

    // RmlUi's log messages about compat output, back in the user's terms (phase-2 §3.12.5).
    // RmlSystem::LogMessage calls handle() first (hook H3); when it returns true the message
    // was dealt with here:
    // - the `unset` sentinel ("Invalid substitution, variable '--html-unset' not defined") is
    //   dropped;
    // - inside a Capture (checks, fork probes) every message is collected instead of logged;
    // - a message naming a registered text (a virtual sheet "@htmlui/doc/<key>/ui/menu.css.rcss:12",
    //   an inline style or the RML of a document "ui/menu.html: 40") is mapped through the line
    //   maps, logged as an HTMLUI-GEN-0006 line with the location in the user's file, recorded for
    //   the Inspector and handed to the registration's sink (the runtime diagnostics of the
    //   UIDocuments that loaded that compile).
    // Everything else goes on to RmlSystem's own logging.
    class JASPERENGINE_INTERNAL RmlLogTranslator {
    public:
        // One text RmlUi parses whose lines compat can map back.
        struct LineMapping {
            std::string url;                          // as RmlUi names it (a leading '/' and '|' for ':' are normalized away)
            uint32_t firstLine = 1;                   // the line number RmlUi gives (*lines)[0]
            const std::vector<SourceRef>* lines = nullptr;   // RcssLineMap::lines or RmlSourceMap::lineRefs
        };

        struct Registration {
            uint64_t key = 0;                          // the compile key (for tests and logs)
            std::shared_ptr<const void> owner;         // keeps files and line maps alive
            const SourceFiles* files = nullptr;
            std::vector<LineMapping> mappings;
            std::function<void(const Diagnostic&)> sink;   // may be empty
        };

        // Registers the texts of a loaded compile; returns a handle (> 0) for remove().
        static uint64_t add(Registration registration);
        static void remove(uint64_t handle);
        static size_t registrationCount();

        // Hook H3. LT_ERROR/LT_ASSERT are errors, LT_WARNING warnings, the rest infos.
        static bool handle(Rml::Log::Type type, const Rml::String& message);
        // The same without RmlUi types.
        static bool handleMessage(Severity severity, const std::string& message);

        // While alive, every message handled on this thread (except the sentinel) is
        // collected here and not logged. Captures nest: the innermost one collects.
        class JASPERENGINE_INTERNAL Capture {
        public:
            struct Message {
                Severity severity;
                std::string text;
            };

            Capture();
            ~Capture();
            Capture(const Capture&) = delete;
            Capture& operator=(const Capture&) = delete;

            const std::vector<Message>& messages() const { return collected; }
            size_t count(Severity severity) const;
            // warnings and errors
            size_t problems() const { return count(Severity::Warning) + count(Severity::Error); }
            void clear() { collected.clear(); }

        private:
            friend class RmlLogTranslator;
            Capture* previous;
            std::vector<Message> collected;
        };

        // "Invalid substitution, variable '--html-unset' not defined ..."
        static bool isUnsetSentinel(std::string_view message);

        // The location an RmlUi message names: "at <url>:<line>", "in <url>: <line>",
        // "on line <line> of <url>", "on <url>:<line>". begin/end delimit the location text
        // (with its leading "at"/"in"/"on" word) inside the message.
        struct MessageLocation {
            std::string url;     // normalized: no leading '/', '|' -> ':'
            uint32_t line = 0;
            size_t begin = 0;
            size_t end = 0;
        };
        static std::optional<MessageLocation> findLocation(std::string_view message);
        static std::string normalizeUrl(std::string_view url);

        // A message mapped through the mappings with the same url (the narrowest mapping whose
        // lines hold a valid ref for the line wins): an HTMLUI-GEN-0006 diagnostic located in
        // the user's file, the message without RmlUi's location; nullopt when it does not map.
        static std::optional<Diagnostic> translate(std::string_view message, const SourceFiles& files,
                                                   Span<const LineMapping> mappings);
    };

}

#endif //COMPAT_RMLLOGTRANSLATOR_H
