// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_WIDGETS_WIDGETSRUNTIME_H
#define HTMLUI_WIDGETS_WIDGETSRUNTIME_H

// The S17 widgets in RmlUi: the element instancers of <list-view>, <grid-view>, <tree-view>,
// <table-view> and <markdown>, the data-markdown view, an Rml::Plugin counting the elements
// RmlUi makes (the views' "no element created while scrolling" gate, the live reload's authored
// set), the per-context tooltip and drag-and-drop sessions, and the runtime diagnostics
// (HTMLUI-HTML-0029, 0031). Registered after every Rml::Initialise (HtmlUI::acquire) and
// dropped after Rml::Shutdown (HtmlUI::release), as the compat runtime is.

#include "Export.h"

#include <cstdint>
#include <string>

namespace Rml {
    class Context;
    class Element;
}

namespace jasperengine::htmlui {

    class JASPERENGINE_API WidgetsRuntime {
    public:
        enum class Code { VirtualViewHeight, VirtualViewTemplate, LargeDataFor, MarkdownRawHtml };

        // HtmlUI::acquire, after the compat runtime
        static void registerAll();
        // HtmlUI::release, after Rml::Shutdown
        static void releaseAll();
        // every frame, every context, before Rml::Context::Update (HtmlUIContext::update)
        static void update(Rml::Context* context);
        // a context goes (its sessions with it)
        static void contextDestroyed(Rml::Context* context);

        // elements RmlUi made since the runtime was registered (Plugin::OnElementCreate)
        static uint64_t elementsCreated();

        // a runtime diagnostic about an element of a compat document: located by its
        // data-htmlui-loc, kept with the document's diagnostics, logged (non-Info) once
        static void reportElement(Rml::Element* element, Code code, const std::string& message, const std::string& suggestion);
    };

}

#endif //HTMLUI_WIDGETS_WIDGETSRUNTIME_H
