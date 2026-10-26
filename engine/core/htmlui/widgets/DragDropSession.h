// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_WIDGETS_DRAGDROPSESSION_H
#define HTMLUI_WIDGETS_DRAGDROPSESSION_H

// HTML5 drag and drop on RmlUi's drags (S17 R18-R23). An element with `drag: drag-drop` (or
// clone; draggable="true" compiles to drag-drop) starts a session at RmlUi's dragstart; the
// session owns the drag's UIDataTransfer and turns the drag into the HTML5 events: dragenter /
// dragleave on the element under the pointer as it changes, dragover on it at most once per frame
// while the pointer moves (and on entry), drop on release over an accepting target, dragend with
// the final dropEffect. A target accepts when a dragover listener prevents the default or the
// nearest `dropzone="type ... [copy|move|link]"` lists one of the types; the drop target (the
// nearest element with a dropzone or a drag listener) gets htmlui-drop-allowed / -denied, the
// source htmlui-drag-source, its body htmlui-dragging. The ghost is a clone of the source (or of
// setDragImage's element) at the pointer; Escape (gamepad B) cancels; within 32 px of a scroll
// container's edge the container scrolls, up to 600 px/s. A drag from a virtual view's row carries
// "htmlui/row" ({view, index, key}); over a virtual view, the drag events say where among its rows
// the pointer is (drop_index, drop_position before/after/on) and the view draws its indicator.
// UIDrag runs the same sequence without a pointer. Engine-internal (RmlUi types).

#include "Export.h"
#include "object/ui/HtmlUIScriptInternal.h"

#include <RmlUi/Core/ObserverPtr.h>

#include <memory>
#include <string>
#include <vector>

namespace Rml {
    class Context;
    class Element;
    class ElementDocument;
}

namespace jasperengine{

    // behind UIDataTransfer handles
    struct UIDataTransferState {
        struct Entry {
            std::string type;
            std::string text;          // a C++ value, or a Lua string
            HtmlUILuaRef lua;          // a Lua value (when set)
        };
        std::vector<Entry> entries;
        std::string effectAllowed = "all";
        std::string dropEffect = "none";
        // the virtual view row the drag started from ("htmlui/row"): its view element, item (0-based) and key
        Rml::ObserverPtr<Rml::Element> rowView;
        int rowIndex = -1;
        std::string rowKey;
        // setDragImage: the element to clone (null with imageSet: no ghost) and the pointer's offset in it
        Rml::ObserverPtr<Rml::Element> image;
        bool imageSet = false;
        float imageX = 0.0f, imageY = 0.0f;
        bool valid = true;             // false once the drag ended

        Entry* find(const std::string& type);
        const Entry* find(const std::string& type) const;
    };

    struct UIDataTransferAccess {
        static UIDataTransfer make(const std::shared_ptr<UIDataTransferState>& state);
        static UIDataTransferState* state(const UIDataTransfer& transfer);
    };

}

namespace jasperengine::htmlui {

    // what inspect_ui reports (R39)
    struct DragState {
        bool active = false;
        Rml::Element* source = nullptr;
        Rml::Element* target = nullptr;            // the drop target under the drag
        std::vector<std::string> types;
        bool allowed = false;
        std::string effect;                        // the dropEffect it would end with
        bool programmatic = false;
    };

    class JASPERENGINE_INTERNAL DragDropSession {
    public:
        static constexpr float kAutoScrollEdge = 32.0f;       // px from a scroll container's edge
        static constexpr float kAutoScrollSpeed = 600.0f;     // px/s at the edge
        static constexpr float kGhostOpacity = 0.7f;

        // the DOM name of a drag event -> the RmlUi event behind it ("" when it is not one of the
        // synthesized ones: dragstart, drag, dragend, dragmove, dragdrop and dragout keep their names)
        static const char* rmlEventOf(const std::string& domType);

        // the data transfer of the drag in progress in the element's context (null without one)
        static std::shared_ptr<UIDataTransferState> transferOf(Rml::Element* element);
        // the parameters of the synthesized events: where among a virtual view's rows the pointer
        // is (drop_index 1-based, drop_position "before"/"after"/"on"; absent outside a view)
        static constexpr const char* kDropIndexParameter = "drop_index";
        static constexpr const char* kDropPositionParameter = "drop_position";

        static DragState stateOf(Rml::Context* context);

        // UIDrag
        static bool begin(Rml::Element* source, std::vector<UIDataTransferState::Entry> entries, const std::string& effectAllowed);
        static bool over(Rml::Element* target);
        static bool drop(Rml::Element* target);
        static void cancel();
        static bool programmaticActive();
        static std::shared_ptr<UIDataTransferState> programmaticTransfer();

        // the source's view keeps its rows while a drag from one of them runs (a recycled source row does not cancel it)
        static bool isDragSourceIn(const Rml::Element* view);

        // every frame before the context updates (WidgetsRuntime::update)
        static void update(Rml::Context* context);
        static void contextDestroyed(Rml::Context* context);
        static void releaseAll();
    };

}

#endif //HTMLUI_WIDGETS_DRAGDROPSESSION_H
