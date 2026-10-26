// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIDATATRANSFER_H
#define UIDATATRANSFER_H

#include "Export.h"
#include "UIElement.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine{

    struct UIDataTransferState;   // htmlui/widgets/DragDropSession.h

    // The data of a drag (S17 R19, HTML5's DataTransfer): event.dataTransfer of the drag events,
    // valid from dragstart to dragend. Each entry has a type ("item", "text/plain", ...); from Lua
    // any value is kept by reference (a table arrives by identity), from C++ a string. Unlike
    // HTML5, getData works in dragenter/dragover too, so a target can accept by content.
    // effectAllowed: none, copy, move, link, copyMove, copyLink, linkMove, all (default);
    // dropEffect: none, copy, move, link. setDragImage replaces the ghost (a clone of the source at
    // 0.7 opacity) by a clone of another element, offset from the pointer; clearDragImage drops it.
    class JASPERENGINE_API UIDataTransfer {
    public:
        UIDataTransfer();                           // invalid
        bool isValid() const;                       // the drag is still running

        void setData(const std::string& type, const std::string& value);
        std::string getData(const std::string& type) const;   // a string entry ("" otherwise)
        bool hasType(const std::string& type) const;
        std::vector<std::string> getTypes() const;
        void clearData(const std::string& type = std::string());   // "" clears every entry

        std::string getEffectAllowed() const; void setEffectAllowed(const std::string& effect);
        std::string getDropEffect() const; void setDropEffect(const std::string& effect);

        void setDragImage(const UIElement& element, float offsetX, float offsetY);
        void clearDragImage();

    private:
        friend struct UIDataTransferAccess;
        std::shared_ptr<UIDataTransferState> state;
    };

    // Options of a programmatic drag (UIDrag::begin): string entries by type, the allowed effects
    struct UIDragOptions {
        std::vector<std::pair<std::string, std::string>> data;
        std::string effectAllowed = "all";
    };

    // Programmatic drags (S17 R23) for gamepad and scripted flows: the same events in the same
    // order as a pointer drag. begin sends dragstart (and drag) to the source; over(target)
    // dragleave/dragenter as the target changes, then dragover; drop(target) drop when the target
    // accepts (dragleave when not), then dragend; cancel dragleave and dragend with dropEffect none.
    struct JASPERENGINE_API UIDrag {
        static bool begin(const UIElement& source, const UIDragOptions& options = UIDragOptions());
        static bool over(const UIElement& target);
        static bool drop(const UIElement& target);
        static void cancel();
        static bool isActive();
        // the data transfer of the drag in progress (invalid without one)
        static UIDataTransfer getDataTransfer();
    };

}

#endif //UIDATATRANSFER_H
