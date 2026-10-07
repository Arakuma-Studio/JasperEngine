// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIDATAMODEL_H
#define UIDATAMODEL_H

#include "Export.h"
#include "UIValue.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    class UIEvent;
    struct UIDataModelState;   // defined by p1-lua (UIDataModel.cpp / HtmlUIScriptInternal.h)

    // RmlUi data binding (data-model, data-for, {{ }}, data-event-*) over one UIValue tree
    // per model. A weak handle: once the model is removed every method is a no-op returning
    // defaults (defined by p1-lua)
    class JASPERENGINE_API UIDataModel {
    public:
        bool isValid() const; std::string getName() const;
        UIValue get(const std::string& path) const;                 // "items[2].name"
        void set(const std::string& path, const UIValue& value);    // dirties the top-level variable
        void dirty(const std::string& topLevelName); void dirtyAll();
        void bindEvent(const std::string& name, std::function<void(UIEvent&, const std::vector<UIValue>& args)> fn);
        uint64_t onChange(const std::string& topLevelName, std::function<void(const UIValue&)> fn); // after a data-value write
        void removeOnChange(uint64_t id);

    private:
        friend struct HtmlUIScriptAccess;   // p1-lua internals (HtmlUIScriptInternal.h) create and read the state
        std::shared_ptr<UIDataModelState> state;
    };

}

#endif //UIDATAMODEL_H
