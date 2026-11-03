// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_WIDGETS_TOOLTIPMANAGER_H
#define HTMLUI_WIDGETS_TOOLTIPMANAGER_H

// Tooltips of HTML UI documents (S17 R14-R17): the `title` attribute, rich tooltips
// (`tooltip="#id"`: an element or <template> of the document, cloned on show; el:setTooltip from
// Lua: a text, an element or a function evaluated on each show) and their rules, timed on
// HtmlUIClock. A tooltip shows after the source was hovered (or focused from the keyboard or a
// gamepad) for the delay (0.5 s; tooltip-delay, configure); once one was shown, another source
// reached within the hop window (0.5 s) shows at once. The source is the nearest ancestor with a
// tooltip: moving onto its children does not restart the delay. It hides when the pointer leaves
// (unless the tooltip is interactive and the pointer is on it), on a mouse button, the wheel, a
// scroll, a key (modifiers aside), a drag start, the source's removal, its document's hide or
// unload, and on blur for a focus tooltip. The tooltip, div.htmlui-tooltip[role=tooltip] with an
// optional .htmlui-tooltip-arrow, is appended to the source document's body (author CSS styles it)
// and placed by placePopup: a title 16 px below the pointer (flipped above it), a rich tooltip on
// its side of the source's border box (flip, clamp, arrow); it is laid out out of view for one
// frame first, measured, then placed. One tooltip per document shows at a time. The source gets
// `tooltipshow` and `tooltiphide`. Nothing runs while no tooltip source is hovered or focused.

#include "Export.h"
#include "htmlui/widgets/PopupPlacement.h"

#include <RmlUi/Core/ObserverPtr.h>

#include <functional>
#include <optional>
#include <string>

namespace Rml {
    class Context;
    class Element;
    class ElementDocument;
}

namespace jasperengine::htmlui {

    // What a function tooltip gives on a show: a text, or an element to clone (null: no tooltip)
    struct TooltipContent {
        std::string text;
        Rml::Element* element = nullptr;
        bool none = false;
    };

    // A tooltip set from Lua or C++ (el:setTooltip)
    struct TooltipSpec {
        std::string text;                                   // when neither element nor content is set
        Rml::ObserverPtr<Rml::Element> element;             // cloned on show
        std::function<TooltipContent()> content;            // evaluated on show
        PopupSide placement = PopupSide::Top;
        bool automatic = false;                             // placement "auto"
        std::optional<double> delay;                        // seconds
        bool arrow = false;
        bool interactive = false;
    };

    // UIDocument.tooltip.configure
    struct TooltipConfig {
        bool enabled = true;
        double delay = 0.5;                                 // seconds
        double hopWindow = 0.5;                             // seconds
    };

    // what inspect_ui reports (R39)
    struct TooltipState {
        bool visible = false;
        Rml::Element* source = nullptr;
        Rml::Element* tooltip = nullptr;
        std::string text;                                   // the tooltip's text content
        float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;   // its border box, document px
        std::string side;                                   // where it went
    };

    class JASPERENGINE_INTERNAL TooltipManager {
    public:
        static constexpr double kDefaultDelay = 0.5;
        static constexpr double kDefaultHopWindow = 0.5;
        static constexpr float kTitleGap = 16.0f;           // below the pointer
        static constexpr float kEdgePad = 4.0f;
        static constexpr float kRichGap = 6.0f;
        static constexpr float kArrowSize = 8.0f;

        // a source's tooltip from Lua or C++ (replaces title/tooltip=); clear removes it
        static void setTooltip(Rml::Element* source, TooltipSpec spec);
        static void clearTooltip(Rml::Element* source);
        static bool hasTooltip(const Rml::Element* source);

        static void configure(Rml::ElementDocument* document, const TooltipConfig& config);
        static TooltipConfig configOf(const Rml::ElementDocument* document);

        // shows the tooltip of `source` now (MCP, tests), hides the document's one
        static bool show(Rml::Element* source);
        static void hide(Rml::ElementDocument* document);
        static TooltipState stateOf(const Rml::ElementDocument* document);

        // every frame before the context updates (WidgetsRuntime::update)
        static void update(Rml::Context* context);
        // the context goes, or RmlUi shuts down
        static void contextDestroyed(Rml::Context* context);
        static void documentUnloaded(Rml::ElementDocument* document);
        static void elementDestroyed(Rml::Element* element);
        static void releaseAll();
        // a drag started in the context: its tooltip hides (DragDropSession)
        static void hideForDrag(Rml::Context* context);
    };

}

#endif //HTMLUI_WIDGETS_TOOLTIPMANAGER_H
