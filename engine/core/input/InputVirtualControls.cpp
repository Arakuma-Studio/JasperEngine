// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "InputVirtualControls.h"

#include "Log.h"
#include "input/InputActions.h"
#include "object/ui/UIEvent.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace jasperengine;

namespace {

    struct Bound {
        UIElement element;
        std::string name;
        std::vector<uint64_t> listeners;
    };

    std::vector<Bound>& bound(){
        static std::vector<Bound> list;
        return list;
    }

    // the controls of elements that are gone go too
    void prune(){
        std::vector<Bound>& list = bound();
        list.erase(std::remove_if(list.begin(), list.end(), [](const Bound& entry){ return !entry.element.isValid(); }), list.end());
    }

    Vector2 stickValue(const UIElement& element, float x, float y){
        const UIRect rect = element.getBoundingClientRect();
        const float radius = std::max(1.0f, std::min(rect.width, rect.height) * 0.5f);
        Vector2 value((x - (rect.x + rect.width * 0.5f)) / radius, -(y - (rect.y + rect.height * 0.5f)) / radius);
        const float length = std::sqrt(value.x * value.x + value.y * value.y);
        if (length > 1.0f)
            value = value * (1.0f / length);
        return value;
    }

    bool bind(UIElement element, const std::string& name, bool stick){
        if (!element.isValid() || name.empty() || name.find('/') != std::string::npos){
            Log::warn("InputActions: a virtual control needs a live element and a name without '/'");
            return false;
        }
        InputVirtualControls::unbind(element);
        prune();
        Bound entry;
        entry.element = element;
        entry.name = name;
        // each finger keeps the element it pressed: the moves of the press reach it wherever they go
        const std::shared_ptr<bool> pressed = std::make_shared<bool>(false);
        entry.listeners.push_back(element.addEventListener("mousedown", [element, name, stick, pressed](UIEvent& event){
            *pressed = true;
            if (stick) InputActions::setVirtual(name, stickValue(element, event.getClientX(), event.getClientY()));
            else InputActions::setVirtual(name, 1.0f);
        }));
        if (stick){
            entry.listeners.push_back(element.addEventListener("mousemove", [element, name, pressed](UIEvent& event){
                if (*pressed) InputActions::setVirtual(name, stickValue(element, event.getClientX(), event.getClientY()));
            }));
        }
        entry.listeners.push_back(element.addEventListener("mouseup", [name, pressed](UIEvent&){
            *pressed = false;
            InputActions::setVirtual(name, Vector2(0.0f, 0.0f));
        }));
        bound().push_back(entry);
        return true;
    }

}

bool InputVirtualControls::bindStick(UIElement element, const std::string& name){
    return bind(element, name, true);
}

bool InputVirtualControls::bindButton(UIElement element, const std::string& name){
    return bind(element, name, false);
}

void InputVirtualControls::unbind(UIElement element){
    std::vector<Bound>& list = bound();
    for (auto it = list.begin(); it != list.end();){
        if (it->element.isValid() && element.isValid() && it->element == element){
            for (uint64_t listener : it->listeners)
                it->element.removeEventListener(listener);
            InputActions::setVirtual(it->name, Vector2(0.0f, 0.0f));
            it = list.erase(it);
        }else{
            ++it;
        }
    }
}
