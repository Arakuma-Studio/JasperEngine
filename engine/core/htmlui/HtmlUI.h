// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_H
#define HTMLUI_H

#include "Export.h"

#include <cstddef>
#include <string>
#include <vector>

namespace jasperengine {

    class HtmlUIContext;
    class RmlSystem;
    class RmlFile;
    class RmlFontEngine;

    // Global, reference-counted owner of RmlUi's process-wide state (the interfaces and
    // Rml::Initialise/Shutdown). Only UIDocumentSystem and the tests acquire it.
    class JASPERENGINE_API HtmlUI {
    public:
        // First call: Rml::SetSystemInterface/SetFileInterface/SetFontEngineInterface, then
        // Rml::Initialise(); subscribes to Engine::onViewDestroyed. Returns false (and logs)
        // if Initialise fails; the count is not taken then.
        static bool acquire();
        // Last call: Rml::Debugger::Shutdown() if initialised, Rml::Shutdown(),
        // unsubscribes. Every HtmlUIContext must be destroyed before.
        static void release();
        static bool isInitialized();
        static int getReferenceCount();

        // Public C++ API for fonts until @font-face (phase 2). Path through FileData
        // (project-relative, asset://, data://). family empty = read from the file.
        static bool loadFontFace(const std::string& path, bool fallback = false,
                                 const std::string& family = "", int weight = 0 /*0 = from file*/, bool italic = false);

        static RmlSystem& getSystemInterface();
        static RmlFile& getFileInterface();
        static RmlFontEngine& getFontEngine();

        // HtmlUIContext registers itself so view loss can drop GPU objects of all of them
        static void registerContext(HtmlUIContext* context);
        static void unregisterContext(HtmlUIContext* context);
        static size_t getContextCount();
        static const std::vector<HtmlUIContext*>& getContexts();
        // the context the RmlUi debugger is attached to (nullptr = none)
        static void setDebuggerContext(HtmlUIContext* context, bool visible);
        static HtmlUIContext* getDebuggerContext();
        static bool isDebuggerVisible();     // shown in getDebuggerContext()
        static bool isDebuggerCompiled();    // JASPERENGINE_HTMLUI_DEBUGGER
    };

}

#endif //HTMLUI_H
