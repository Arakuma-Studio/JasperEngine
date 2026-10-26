// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "InputActions.h"

#include "Input.h"
#include "Log.h"
#include "asset/AssetMap.h"
#include "input/InputControlPath.h"
#include "input/InputRebind.h"
#include "io/Data.h"
#include "io/UserSettings.h"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <map>

using namespace jasperengine;

// The evaluation runs on arrays built when an asset loads: maps, actions and bindings (and the
// parts of composites) in asset order, the maps' priority order, and one sample per binding.
// A frame then makes no heap allocation: the controls a map consumed are a bitset, cleared
// through the list of the ones it set.
namespace jasperengine {

    namespace {

        constexpr int MAX_VIRTUALS = 64;
        constexpr int MAX_SAMPLE_KEYS = 16;

        // the controls a map can consume: Input's controls (keys, mouse and gamepad buttons),
        // then each pad slot's axes, the mouse delta and scroll, the primary touch, the touch
        // count and the virtual controls
        constexpr int KEY_AXIS_BASE = Input::CONTROL_COUNT;
        constexpr int KEY_MOUSE_DELTA = KEY_AXIS_BASE + D_INPUT_MAX_GAMEPADS * Input::GAMEPAD_AXES;
        constexpr int KEY_MOUSE_SCROLL = KEY_MOUSE_DELTA + 1;
        constexpr int KEY_TOUCH = KEY_MOUSE_SCROLL + 1;
        constexpr int KEY_TOUCH_COUNT = KEY_TOUCH + 1;
        constexpr int KEY_VIRTUAL_BASE = KEY_TOUCH_COUNT + 1;
        constexpr int KEY_COUNT = KEY_VIRTUAL_BASE + MAX_VIRTUALS;

        constexpr float DEFAULT_DEADZONE = 0.15f;

        struct Sample {
            Vector2 value;
            bool digital = false;
            bool tapped = false;        // pressed and released in the window, released now
            bool repressed = false;     // released and pressed again in the window, held now
            bool actuated = false;      // held, moved, or pressed in the window
            bool delta = false;         // a motion: neither deadzoned nor clamped
            int mods = 0;               // the modifiers that went with it
            int keys[MAX_SAMPLE_KEYS] = {};
            int keyCount = 0;
            int blockedBy = -1;         // the map that consumed its control

            void addKey(int key){
                if (keyCount < MAX_SAMPLE_KEYS)
                    keys[keyCount++] = key;
            }

            float magnitude() const{
                return std::sqrt(value.x * value.x + value.y * value.y);
            }
        };

        struct RBinding {
            std::string id;
            InputControlPath control;      // the effective path (its override, else the asset's)
            float scale = 1.0f;
            bool invert = false;
            std::optional<float> deadzone;
            int requiredMods = 0;
            int forbiddenMods = 0;
            std::vector<std::string> groups;
            InputCompositeType composite = InputCompositeType::NONE;
            bool normalize = true;
            int firstPart = 0;
            int partCount = 0;
            int virtualIndex = -1;
            // this window
            Sample sample;
            bool suppressed = false;
            bool satisfied = false;        // a modifier binding whose modifiers are held, actuated
        };

        struct Injection {
            bool active = false;
            Vector2 value;
            int windows = 0;               // windows left, -1 until released
            bool tap = false;
        };

        struct RAction {
            InputAction* action = nullptr;
            float pressPoint = 0.5f;
            float holdTime = 0.0f;
            std::optional<float> deadzone;
            int firstBinding = 0;
            int bindingCount = 0;
            Injection injection;
        };

        struct RMap {
            std::string name;
            int priority = 0;
            bool enabled = true;
            bool consume = true;
            bool exclusive = false;
            bool ignoreUI = true;
            int gamepad = -1;
            int firstAction = 0;
            int actionCount = 0;
            bool blocked = false;
            int blockedBy = -1;        // the exclusive map above it that blocked it
        };

        struct VirtualControl {
            std::string name;
            Vector2 value;
        };

    }

    struct InputActionsRuntime {
        bool loaded = false;
        std::string path;
        InputActionAssetDesc asset;
        std::vector<RMap> maps;
        std::vector<RAction> actions;
        std::vector<RBinding> bindings;
        std::vector<RBinding> parts;
        std::vector<int> order;            // map indexes, priority first, then asset order
        std::vector<int> modifierEntries;  // scratch: the satisfied modifier bindings of a level
        // InputAction objects outlive reloads, so scripts keep theirs: "Map/Action" -> object
        std::map<std::string, std::unique_ptr<InputAction>> actionObjects;
        std::map<std::string, std::string> overrides;
        VirtualControl virtuals[MAX_VIRTUALS];
        int virtualCount = 0;
        std::bitset<KEY_COUNT> consumed;
        int consumer[KEY_COUNT];
        int consumedKeys[KEY_COUNT];
        int consumedCount = 0;
        float defaultDeadzone = DEFAULT_DEADZONE;
        InputRebindState rebind;
        std::shared_ptr<InputOverrideStore> store;

        InputActionsRuntime(){
            std::fill(std::begin(consumer), std::end(consumer), -1);
        }

        void consume(int key, int map){
            if (key < 0 || key >= KEY_COUNT || consumed[key])
                return;
            consumed[key] = true;
            consumer[key] = map;
            consumedKeys[consumedCount++] = key;
        }

        void clearConsumed(){
            for (int i = 0; i < consumedCount; i++){
                consumed[consumedKeys[i]] = false;
                consumer[consumedKeys[i]] = -1;
            }
            consumedCount = 0;
        }

        int virtualIndex(const std::string& name, bool create){
            for (int i = 0; i < virtualCount; i++){
                if (virtuals[i].name == name)
                    return i;
            }
            if (!create || virtualCount >= MAX_VIRTUALS)
                return -1;
            virtuals[virtualCount].name = name;
            virtuals[virtualCount].value = Vector2(0.0f, 0.0f);
            return virtualCount++;
        }

        RMap* findMap(const std::string& name){
            for (RMap& map : maps){
                if (map.name == name)
                    return &map;
            }
            return nullptr;
        }

        RAction* findAction(const std::string& path){
            for (RAction& action : actions){
                if (action.action->getPath() == path)
                    return &action;
            }
            return nullptr;
        }

        RBinding* findBinding(const std::string& id){
            for (RBinding& binding : bindings){
                if (binding.id == id)
                    return &binding;
            }
            for (RBinding& part : parts){
                if (part.id == id)
                    return &part;
            }
            return nullptr;
        }

        void sortOrder(){
            order.resize(maps.size());
            for (size_t i = 0; i < maps.size(); i++)
                order[i] = (int)i;
            std::stable_sort(order.begin(), order.end(), [&](int a, int b){ return maps[a].priority > maps[b].priority; });
        }

        InputOverrideStore& overrideStore();

        // the evaluation's steps (members: they reach InputAction's state)
        static void resetActionState(InputAction& action);
        void fire(RAction& action, FunctionSubscribe<void(InputActionEvent&)>& event, InputActionPhase phase, int mapIndex);
        void updateAction(RAction& action, int mapIndex, bool active, float dt);
        void build(bool keepMapState);

        // a load, unload or reload asked for by a callback during the evaluation runs after it
        bool evaluating = false;
        std::vector<std::function<void()>> deferred;
    };

}

namespace {

    class UserSettingsStore : public InputOverrideStore {
    public:
        std::string read(const std::string& key) override{
            return UserSettings::getStringForKey(key.c_str(), "");
        }
        void write(const std::string& key, const std::string& value) override{
            UserSettings::setStringForKey(key.c_str(), value);
        }
        void remove(const std::string& key) override{
            UserSettings::removeKey(key.c_str());
        }
    };

    InputActionsRuntime& rt(){
        static InputActionsRuntime runtime;
        return runtime;
    }

    float actionAxis(float value, int axis){
        // triggers rest at -1: an action sees 0..1; the sticks' y is up-positive, like a d-pad
        if (axis == D_GAMEPAD_AXIS_LEFT_TRIGGER || axis == D_GAMEPAD_AXIS_RIGHT_TRIGGER)
            return (value + 1.0f) * 0.5f;
        if (axis == D_GAMEPAD_AXIS_LEFT_Y || axis == D_GAMEPAD_AXIS_RIGHT_Y)
            return -value;
        return value;
    }

    float scalarDeadzone(float value, float deadzone){
        const float magnitude = std::fabs(value);
        if (magnitude <= deadzone || deadzone >= 1.0f)
            return 0.0f;
        const float scaled = std::min(1.0f, (magnitude - deadzone) / (1.0f - deadzone));
        return value < 0.0f ? -scaled : scaled;
    }

    Vector2 radialDeadzone(Vector2 value, float deadzone){
        const float length = std::sqrt(value.x * value.x + value.y * value.y);
        if (length <= deadzone || deadzone >= 1.0f || length <= 0.0f)
            return Vector2(0.0f, 0.0f);
        const float scaled = std::min(1.0f, (length - deadzone) / (1.0f - deadzone));
        return value * (scaled / length);
    }

    // the slots a map reads: its pad, or every slot with a pad (or a pad's last edges)
    template<typename F>
    void forEachSlot(const RMap& map, F&& visit){
        if (map.gamepad >= 0){
            const int slot = Input::getGamepadSlot(map.gamepad);
            if (slot >= 0)
                visit(slot);
            return;
        }
        for (int slot = 0; slot < D_INPUT_MAX_GAMEPADS; slot++){
            if (Input::getGamepadSlotId(slot) >= 0)
                visit(slot);
        }
    }

    void sampleDigital(Sample& s, const InputControlEdges& edges, bool ignoreUI, int key){
        const int presses = (int)edges.presses - (ignoreUI ? (int)edges.uiPresses : 0);
        const bool level = edges.level && !(ignoreUI && edges.uiHeld);
        const bool actuated = level || presses > 0;
        if (!actuated)
            return;
        s.digital = true;
        if (level)
            s.value = Vector2(1.0f, 0.0f);
        if (presses > 0 && !level)
            s.tapped = true;
        if (presses > 0 && level && edges.releases > 0)
            s.repressed = true;
        s.actuated = true;
        // a press in the window counts with the modifiers it was made with (a same-window
        // Ctrl+S tap), a held control with the ones held now (sampleControl adds them)
        if (presses > 0)
            s.mods |= edges.pressMods;
        s.addKey(key);
    }

    // the frame's primary finger: the first one down, else the first one lifted in the window
    bool primaryTouch(Touch& out){
        const size_t count = Input::getTouchCount();
        for (size_t i = 0; i < count; i++){
            const Touch touch = Input::getTouch(i);
            if (touch.phase != TouchPhase::ENDED && touch.phase != TouchPhase::CANCELED){
                out = touch;
                return true;
            }
        }
        if (count > 0){
            out = Input::getTouch(0);
            return true;
        }
        return false;
    }

    Sample sampleControl(InputActionsRuntime& r, const RBinding& binding, const RMap& map, int heldMods){
        Sample s;
        const InputControlPath& control = binding.control;
        auto blocked = [&](int key){
            if (r.consumed[key]){
                s.blockedBy = r.consumer[key];
                return true;
            }
            return false;
        };
        switch (control.kind){
            case InputControlKind::KEY:
            case InputControlKind::MOUSE_BUTTON: {
                const int key = control.kind == InputControlKind::KEY ? Input::keyControl(control.index) : Input::mouseControl(control.index);
                if (key < 0 || blocked(key))
                    break;
                sampleDigital(s, Input::getControlEdges(key, false), map.ignoreUI, key);
                break;
            }
            case InputControlKind::GAMEPAD_BUTTON: {
                forEachSlot(map, [&](int slot){
                    const int key = Input::gamepadControl(slot, control.index);
                    if (key < 0 || blocked(key))
                        return;
                    Sample one;
                    sampleDigital(one, Input::getControlEdges(key, false), map.ignoreUI, key);
                    if (!one.actuated)
                        return;
                    // buttons OR across pads
                    if (one.value.x > s.value.x)
                        s.value = one.value;
                    s.digital = true;
                    s.tapped = s.tapped || one.tapped;
                    s.repressed = s.repressed || one.repressed;
                    s.actuated = true;
                    s.addKey(key);
                });
                if (s.value.x > 0.0f)
                    s.tapped = false;
                break;
            }
            case InputControlKind::GAMEPAD_AXIS: {
                float best = 0.0f;
                int bestKey = -1;
                forEachSlot(map, [&](int slot){
                    const int key = KEY_AXIS_BASE + slot * Input::GAMEPAD_AXES + control.index;
                    if (blocked(key))
                        return;
                    const float value = actionAxis(Input::getGamepadSlotAxis(slot, control.index, map.ignoreUI), control.index);
                    if (std::fabs(value) > std::fabs(best)){
                        best = value;
                        bestKey = key;
                    }
                });
                if (bestKey >= 0){
                    s.value = Vector2(best, 0.0f);
                    s.actuated = true;
                    s.addKey(bestKey);
                }
                break;
            }
            case InputControlKind::GAMEPAD_STICK: {
                const int axisX = control.index == 0 ? D_GAMEPAD_AXIS_LEFT_X : D_GAMEPAD_AXIS_RIGHT_X;
                const int axisY = control.index == 0 ? D_GAMEPAD_AXIS_LEFT_Y : D_GAMEPAD_AXIS_RIGHT_Y;
                float bestLength = 0.0f;
                forEachSlot(map, [&](int slot){
                    const int keyX = KEY_AXIS_BASE + slot * Input::GAMEPAD_AXES + axisX;
                    const int keyY = KEY_AXIS_BASE + slot * Input::GAMEPAD_AXES + axisY;
                    if (blocked(keyX) || blocked(keyY))
                        return;
                    const Vector2 value(actionAxis(Input::getGamepadSlotAxis(slot, axisX, map.ignoreUI), axisX),
                                        actionAxis(Input::getGamepadSlotAxis(slot, axisY, map.ignoreUI), axisY));
                    const float length = std::sqrt(value.x * value.x + value.y * value.y);
                    if (length > bestLength){
                        bestLength = length;
                        s.value = value;
                        s.keyCount = 0;
                        s.addKey(keyX);
                        s.addKey(keyY);
                        s.actuated = true;
                    }
                });
                break;
            }
            case InputControlKind::MOUSE_DELTA:
            case InputControlKind::MOUSE_SCROLL: {
                const bool delta = control.kind == InputControlKind::MOUSE_DELTA;
                const int key = delta ? KEY_MOUSE_DELTA : KEY_MOUSE_SCROLL;
                s.delta = true;
                if (blocked(key))
                    break;
                Vector2 value = delta ? Input::getMouseDelta()
                                      : (map.ignoreUI ? Input::getMouseScrollDeltaForGame() : Input::getMouseScrollDelta());
                if (control.component == 0) value = Vector2(value.x, 0.0f);
                else if (control.component == 1) value = Vector2(value.y, 0.0f);
                if (value.x != 0.0f || value.y != 0.0f){
                    s.value = value;
                    s.actuated = true;
                    s.addKey(key);
                }
                break;
            }
            case InputControlKind::TOUCH_PRESS:
            case InputControlKind::TOUCH_POSITION:
            case InputControlKind::TOUCH_DELTA: {
                s.delta = control.kind == InputControlKind::TOUCH_DELTA;
                Touch touch;
                if (blocked(KEY_TOUCH) || !primaryTouch(touch))
                    break;
                const InputControlEdges edges = Input::getTouchEdges(touch.pointer);
                const bool uiTouch = map.ignoreUI && (edges.uiHeld || (edges.presses > 0 && edges.uiPresses >= edges.presses));
                if (control.kind == InputControlKind::TOUCH_PRESS){
                    sampleDigital(s, edges, map.ignoreUI, KEY_TOUCH);
                }else if (!uiTouch){
                    s.value = control.kind == InputControlKind::TOUCH_POSITION ? touch.position : touch.delta;
                    s.actuated = control.kind == InputControlKind::TOUCH_POSITION || touch.delta.x != 0.0f || touch.delta.y != 0.0f;
                    if (s.actuated)
                        s.addKey(KEY_TOUCH);
                }
                break;
            }
            case InputControlKind::TOUCH_COUNT: {
                if (blocked(KEY_TOUCH_COUNT))
                    break;
                int count = 0;
                for (size_t i = 0; i < Input::getTouchCount(); i++){
                    const TouchPhase phase = Input::getTouch(i).phase;
                    if (phase != TouchPhase::ENDED && phase != TouchPhase::CANCELED)
                        count++;
                }
                if (count > 0){
                    s.value = Vector2((float)count, 0.0f);
                    s.actuated = true;
                    s.addKey(KEY_TOUCH_COUNT);
                }
                break;
            }
            case InputControlKind::VIRTUAL: {
                if (binding.virtualIndex < 0)
                    break;
                const int key = KEY_VIRTUAL_BASE + binding.virtualIndex;
                if (blocked(key))
                    break;
                const Vector2 value = r.virtuals[binding.virtualIndex].value;
                if (value.x != 0.0f || value.y != 0.0f){
                    s.value = value;
                    s.actuated = true;
                    s.addKey(key);
                }
                break;
            }
            case InputControlKind::NONE:
                break;
        }
        if (!s.digital)
            s.mods = heldMods;
        else
            s.mods |= heldMods;
        return s;
    }

    // a binding's own processing: modifiers, scale, invert, deadzone
    Sample sampleBinding(InputActionsRuntime& r, RBinding& binding, const RMap& map, const RAction& action, int heldMods){
        Sample s = sampleControl(r, binding, map, heldMods);
        binding.satisfied = false;
        if (!s.actuated)
            return s;
        if ((binding.forbiddenMods & s.mods) != 0 ||
            (binding.requiredMods != 0 && (binding.requiredMods & s.mods) != binding.requiredMods)){
            const int blockedBy = s.blockedBy;
            s = Sample();
            s.blockedBy = blockedBy;
            return s;
        }
        binding.satisfied = binding.requiredMods != 0;
        const InputControlKind kind = binding.control.kind;
        if (kind == InputControlKind::GAMEPAD_AXIS || kind == InputControlKind::GAMEPAD_STICK ||
            (kind == InputControlKind::VIRTUAL && binding.deadzone)){
            const float deadzone = binding.deadzone ? *binding.deadzone : action.deadzone ? *action.deadzone : r.defaultDeadzone;
            if (kind == InputControlKind::GAMEPAD_STICK || (kind == InputControlKind::VIRTUAL && s.value.y != 0.0f))
                s.value = radialDeadzone(s.value, deadzone);
            else
                s.value = Vector2(scalarDeadzone(s.value.x, deadzone), 0.0f);
            if (s.value.x == 0.0f && s.value.y == 0.0f)
                s.actuated = false;
        }
        s.value = s.value * binding.scale;
        if (binding.invert)
            s.value = -s.value;
        return s;
    }

    Sample sampleComposite(InputActionsRuntime& r, RBinding& binding, const RMap& map, const RAction& action, int heldMods){
        Sample s;
        s.digital = true;
        float magnitudes[4] = {};
        bool anyTapped = false;
        for (int i = 0; i < binding.partCount && i < 4; i++){
            RBinding& part = r.parts[binding.firstPart + i];
            if (!part.control.isValid())
                continue;
            const Sample one = sampleBinding(r, part, map, action, heldMods);
            if (one.blockedBy >= 0 && s.blockedBy < 0)
                s.blockedBy = one.blockedBy;
            if (!one.actuated)
                continue;
            magnitudes[i] = one.magnitude();
            s.digital = s.digital && one.digital;
            anyTapped = anyTapped || one.tapped;
            s.repressed = s.repressed || one.repressed;
            s.delta = s.delta || one.delta;
            s.actuated = true;
            s.mods |= one.mods;
            for (int k = 0; k < one.keyCount; k++)
                s.addKey(one.keys[k]);
        }
        if (binding.composite == InputCompositeType::AXIS_1D){
            s.value = Vector2(magnitudes[1] - magnitudes[0], 0.0f);
        }else{
            s.value = Vector2(magnitudes[3] - magnitudes[2], magnitudes[0] - magnitudes[1]);
            const float length = s.magnitude();
            if (binding.normalize && length > 1.0f)
                s.value = s.value * (1.0f / length);
        }
        s.tapped = anyTapped && s.magnitude() == 0.0f;
        s.value = s.value * binding.scale;
        if (binding.invert)
            s.value = -s.value;
        if (!s.actuated)
            s.digital = false;
        return s;
    }

    struct EventContext {
        InputActionsRuntime* runtime = nullptr;
        RAction* action = nullptr;
        int mapIndex = -1;
    };
    EventContext currentEvent;

}

void InputActionEvent::consume(){
    consumed = true;
    InputActionsRuntime* r = currentEvent.runtime;
    RAction* action = currentEvent.action;
    if (!r || !action || action->action != this->action)
        return;
    for (int b = 0; b < action->bindingCount; b++){
        const Sample& s = r->bindings[action->firstBinding + b].sample;
        for (int k = 0; k < s.keyCount; k++)
            r->consume(s.keys[k], currentEvent.mapIndex);
    }
}

namespace {

    RBinding runtimeBinding(InputActionsRuntime& r, const InputBindingDesc& desc){
        RBinding binding;
        binding.id = desc.id;
        auto it = r.overrides.find(desc.id);
        binding.control = InputControlPath::parse(it != r.overrides.end() ? it->second : desc.path);
        binding.scale = desc.scale;
        binding.invert = desc.invert;
        binding.deadzone = desc.deadzone;
        binding.requiredMods = desc.requiredMods;
        binding.forbiddenMods = desc.forbiddenMods;
        binding.groups = desc.groups;
        binding.composite = desc.composite;
        binding.normalize = desc.normalize;
        if (binding.control.kind == InputControlKind::VIRTUAL)
            binding.virtualIndex = r.virtualIndex(binding.control.name, true);
        return binding;
    }

    std::string readAssetText(const std::string& path, bool& ok){
        Data data;
        ok = data.open(path.c_str()) == FILEDATA_OK;
        if (!ok)
            return std::string();
        return std::string(reinterpret_cast<const char*>(data.getMemPtr()), data.length());
    }

    std::string titleWords(const std::string& name){
        std::string out;
        bool start = true;
        for (char c : name){
            if (c == '_'){
                out += ' ';
                start = true;
                continue;
            }
            out += start ? (char)std::toupper(static_cast<unsigned char>(c)) : c;
            start = false;
        }
        return out;
    }

    std::string keyDisplayName(int key){
        if (key >= D_KEY_A && key <= D_KEY_Z) return std::string(1, (char)('A' + key - D_KEY_A));
        if (key >= D_KEY_0 && key <= D_KEY_9) return std::string(1, (char)('0' + key - D_KEY_0));
        if (key >= D_KEY_KP_0 && key <= D_KEY_KP_9) return "Numpad " + std::to_string(key - D_KEY_KP_0);
        if (key >= D_KEY_F1 && key <= D_KEY_F25) return "F" + std::to_string(key - D_KEY_F1 + 1);
        switch (key){
            case D_KEY_APOSTROPHE: return "'";
            case D_KEY_COMMA: return ",";
            case D_KEY_MINUS: return "-";
            case D_KEY_PERIOD: return ".";
            case D_KEY_SLASH: return "/";
            case D_KEY_SEMICOLON: return ";";
            case D_KEY_EQUAL: return "=";
            case D_KEY_LEFT_BRACKET: return "[";
            case D_KEY_BACKSLASH: return "\\";
            case D_KEY_RIGHT_BRACKET: return "]";
            case D_KEY_GRAVE_ACCENT: return "`";
            case D_KEY_LEFT_CONTROL: return "Left Ctrl";
            case D_KEY_RIGHT_CONTROL: return "Right Ctrl";
            case D_KEY_KP_DECIMAL: return "Numpad .";
            case D_KEY_KP_DIVIDE: return "Numpad /";
            case D_KEY_KP_MULTIPLY: return "Numpad *";
            case D_KEY_KP_SUBTRACT: return "Numpad -";
            case D_KEY_KP_ADD: return "Numpad +";
            case D_KEY_KP_ENTER: return "Numpad Enter";
            case D_KEY_KP_EQUAL: return "Numpad =";
            default: break;
        }
        return titleWords(InputControlNames::keyName(key));
    }

    bool isPlayStation(const std::string& padName){
        std::string lower = padName;
        for (char& c : lower)
            c = (char)std::tolower(static_cast<unsigned char>(c));
        return lower.find("playstation") != std::string::npos || lower.find("dualshock") != std::string::npos ||
               lower.find("dualsense") != std::string::npos || lower.find("ps4") != std::string::npos ||
               lower.find("ps5") != std::string::npos || lower.find("sony") != std::string::npos;
    }

    std::string gamepadButtonDisplayName(int button, bool playStation){
        if (playStation){
            switch (button){
                case D_GAMEPAD_BUTTON_A: return "Cross";
                case D_GAMEPAD_BUTTON_B: return "Circle";
                case D_GAMEPAD_BUTTON_X: return "Square";
                case D_GAMEPAD_BUTTON_Y: return "Triangle";
                case D_GAMEPAD_BUTTON_LEFT_BUMPER: return "L1";
                case D_GAMEPAD_BUTTON_RIGHT_BUMPER: return "R1";
                case D_GAMEPAD_BUTTON_BACK: return "Share";
                case D_GAMEPAD_BUTTON_START: return "Options";
                case D_GAMEPAD_BUTTON_GUIDE: return "PS";
                case D_GAMEPAD_BUTTON_LEFT_THUMB: return "L3";
                case D_GAMEPAD_BUTTON_RIGHT_THUMB: return "R3";
                default: break;
            }
        }
        switch (button){
            case D_GAMEPAD_BUTTON_A: return "A";
            case D_GAMEPAD_BUTTON_B: return "B";
            case D_GAMEPAD_BUTTON_X: return "X";
            case D_GAMEPAD_BUTTON_Y: return "Y";
            case D_GAMEPAD_BUTTON_LEFT_BUMPER: return "Left Bumper";
            case D_GAMEPAD_BUTTON_RIGHT_BUMPER: return "Right Bumper";
            case D_GAMEPAD_BUTTON_BACK: return "Back";
            case D_GAMEPAD_BUTTON_START: return "Start";
            case D_GAMEPAD_BUTTON_GUIDE: return "Guide";
            case D_GAMEPAD_BUTTON_LEFT_THUMB: return "Left Stick Press";
            case D_GAMEPAD_BUTTON_RIGHT_THUMB: return "Right Stick Press";
            case D_GAMEPAD_BUTTON_DPAD_UP: return "D-Pad Up";
            case D_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-Pad Right";
            case D_GAMEPAD_BUTTON_DPAD_DOWN: return "D-Pad Down";
            case D_GAMEPAD_BUTTON_DPAD_LEFT: return "D-Pad Left";
            default: break;
        }
        return std::to_string(button);
    }

    std::string gamepadAxisDisplayName(int axis, bool playStation){
        switch (axis){
            case D_GAMEPAD_AXIS_LEFT_X: return "Left Stick X";
            case D_GAMEPAD_AXIS_LEFT_Y: return "Left Stick Y";
            case D_GAMEPAD_AXIS_RIGHT_X: return "Right Stick X";
            case D_GAMEPAD_AXIS_RIGHT_Y: return "Right Stick Y";
            case D_GAMEPAD_AXIS_LEFT_TRIGGER: return playStation ? "L2" : "Left Trigger";
            case D_GAMEPAD_AXIS_RIGHT_TRIGGER: return playStation ? "R2" : "Right Trigger";
            default: break;
        }
        return std::to_string(axis);
    }

    // the pad the names follow: the map's, else the first connected one
    std::string displayPadName(int gamepad){
        if (gamepad >= 0 && Input::isGamepadConnected(gamepad))
            return Input::getGamepadName(gamepad);
        if (Input::numGamepads() > 0)
            return Input::getGamepadName(Input::getGamepadId(0));
        return std::string();
    }

    std::string controlDisplayName(const InputControlPath& control, int mods, int gamepad){
        std::string name;
        const bool playStation = isPlayStation(displayPadName(gamepad));
        switch (control.kind){
            case InputControlKind::KEY: name = keyDisplayName(control.index); break;
            case InputControlKind::MOUSE_BUTTON:
                switch (control.index){
                    case D_MOUSE_BUTTON_LEFT: name = "Left Button"; break;
                    case D_MOUSE_BUTTON_RIGHT: name = "Right Button"; break;
                    case D_MOUSE_BUTTON_MIDDLE: name = "Middle Button"; break;
                    default: name = "Mouse Button " + std::to_string(control.index + 1); break;
                }
                break;
            case InputControlKind::MOUSE_DELTA:
                name = control.component == 0 ? "Mouse Delta X" : control.component == 1 ? "Mouse Delta Y" : "Mouse Delta";
                break;
            case InputControlKind::MOUSE_SCROLL:
                name = control.component == 0 ? "Scroll X" : control.component == 1 ? "Scroll Y" : "Scroll";
                break;
            case InputControlKind::GAMEPAD_BUTTON: name = gamepadButtonDisplayName(control.index, playStation); break;
            case InputControlKind::GAMEPAD_AXIS: name = gamepadAxisDisplayName(control.index, playStation); break;
            case InputControlKind::GAMEPAD_STICK: name = control.index == 0 ? "Left Stick" : "Right Stick"; break;
            case InputControlKind::TOUCH_PRESS: name = "Touch"; break;
            case InputControlKind::TOUCH_POSITION: name = "Touch Position"; break;
            case InputControlKind::TOUCH_DELTA: name = "Touch Delta"; break;
            case InputControlKind::TOUCH_COUNT: name = "Touch Count"; break;
            case InputControlKind::VIRTUAL: name = control.name; break;
            case InputControlKind::NONE: return std::string();
        }
        std::string prefix;
        if (mods & D_MODIFIER_CONTROL) prefix += "Ctrl+";
        if (mods & D_MODIFIER_SHIFT) prefix += "Shift+";
        if (mods & D_MODIFIER_ALT) prefix += "Alt+";
        if (mods & D_MODIFIER_SUPER) prefix += "Super+";
        return prefix + name;
    }

    std::string overridesJson(const std::map<std::string, std::string>& overrides){
        InputJson json = InputJson::object();
        json["version"] = 1;
        InputJson list = InputJson::object();
        for (const auto& [id, path] : overrides)
            list[id] = path;
        json["overrides"] = list;
        return json.dump();
    }

    // the map an action of the asset belongs to (by its runtime index)
    int mapOfAction(const InputActionsRuntime& r, int actionIndex){
        for (size_t m = 0; m < r.maps.size(); m++){
            const RMap& map = r.maps[m];
            if (actionIndex >= map.firstAction && actionIndex < map.firstAction + map.actionCount)
                return (int)m;
        }
        return -1;
    }

}

void InputActionsRuntime::fire(RAction& action, FunctionSubscribe<void(InputActionEvent&)>& event, InputActionPhase phase, int mapIndex){
    if (!event.hasSubscribers())
        return;
    InputActionEvent e;
    e.action = action.action;
    e.phase = phase;
    e.vector = action.action->value;
    e.value = action.action->readValue();
    const EventContext saved = currentEvent;
    currentEvent = EventContext{this, &action, mapIndex};
    event.call(e);
    currentEvent = saved;
}

// value, edges, held time, phase and callbacks of one action this window
void InputActionsRuntime::updateAction(RAction& ra, int mapIndex, bool active, float dt){
    InputActionsRuntime& r = *this;
    InputAction& action = *ra.action;
    Vector2 value;
    bool tap = false;
    bool repressed = false;
    bool anyDelta = false;
    action.consumedByMap = (!active && r.maps[mapIndex].blocked) ? r.maps[mapIndex].blockedBy : -1;

    if (ra.injection.active){
        // a tap reads 0 at the window's end: pressed and released in it
        value = ra.injection.tap ? Vector2(0.0f, 0.0f) : ra.injection.value;
        tap = ra.injection.tap;
        // a tap is one window; a count runs out (it reads 0 from the next window); -1 lasts
        if (ra.injection.tap || (ra.injection.windows > 0 && --ra.injection.windows == 0))
            ra.injection.active = false;
    }else{
        for (int b = 0; b < ra.bindingCount; b++){
            RBinding& binding = r.bindings[ra.firstBinding + b];
            Sample& s = binding.sample;
            if (!active || binding.suppressed){
                s = Sample();
                continue;
            }
            if (s.blockedBy >= 0 && action.consumedByMap < 0)
                action.consumedByMap = s.blockedBy;
            if (!s.actuated)
                continue;
            tap = tap || s.tapped;
            repressed = repressed || s.repressed;
            anyDelta = anyDelta || s.delta;
            switch (action.type){
                case InputActionType::BUTTON:
                    value.x = std::max(value.x, s.magnitude());
                    break;
                case InputActionType::AXIS:
                    value.x += s.value.x;
                    break;
                case InputActionType::VECTOR2:
                    value += s.value;
                    break;
            }
        }
        if (action.type == InputActionType::AXIS && !anyDelta)
            value.x = std::clamp(value.x, -1.0f, 1.0f);
        if (action.type == InputActionType::VECTOR2 && !anyDelta){
            const float length = std::sqrt(value.x * value.x + value.y * value.y);
            if (length > 1.0f)
                value = value * (1.0f / length);
        }
    }

    action.value = value;
    const float magnitude = action.readValue();
    const float absMagnitude = std::fabs(magnitude);
    const bool wasPressed = action.pressed;
    const bool pressedNow = absMagnitude >= ra.pressPoint && absMagnitude > 0.0f;
    tap = tap && !pressedNow;

    action.pressedThisFrame = (!wasPressed && pressedNow) || tap || (wasPressed && pressedNow && repressed);
    action.releasedThisFrame = (wasPressed && !pressedNow) || tap || (wasPressed && pressedNow && repressed);
    action.pressed = pressedNow;
    if (action.pressedThisFrame) action.fixedPressed = true;
    if (action.releasedThisFrame) action.fixedReleased = true;

    if (pressedNow && wasPressed && !repressed) action.heldTime += dt;
    else if (pressedNow) action.heldTime = 0.0f;
    else action.heldTime = 0.0f;

    // phases: WAITING -> STARTED -> PERFORMED -> CANCELED -> WAITING; a tap goes all the way
    if (action.phase == InputActionPhase::CANCELED)
        action.phase = InputActionPhase::WAITING;
    const bool moving = absMagnitude > 0.0f || tap;
    if (action.phase == InputActionPhase::WAITING && moving){
        action.phase = InputActionPhase::STARTED;
        fire(ra, action.onStarted, InputActionPhase::STARTED, mapIndex);
    }
    const bool holdReached = ra.holdTime <= 0.0f || action.heldTime >= ra.holdTime;
    if (action.phase == InputActionPhase::STARTED && (pressedNow || tap) && holdReached){
        action.phase = InputActionPhase::PERFORMED;
        fire(ra, action.onPerformed, InputActionPhase::PERFORMED, mapIndex);
    }
    if ((action.phase == InputActionPhase::PERFORMED && !pressedNow) ||
        (action.phase == InputActionPhase::STARTED && (absMagnitude == 0.0f))){
        action.phase = InputActionPhase::CANCELED;
        fire(ra, action.onCanceled, InputActionPhase::CANCELED, mapIndex);
    }
}

void InputActionsRuntime::resetActionState(InputAction& action){
    action.value = Vector2(0.0f, 0.0f);
    action.pressed = false;
    action.pressedThisFrame = false;
    action.releasedThisFrame = false;
    action.fixedPressed = false;
    action.fixedReleased = false;
    action.heldTime = 0.0f;
    action.phase = InputActionPhase::WAITING;
    action.consumedByMap = -1;
}

// the runtime arrays of the asset; maps keep the state they had (enabled, priority, pad)
void InputActionsRuntime::build(bool keepMapState){
    InputActionsRuntime& r = *this;
    std::map<std::string, RMap> previous;
    if (keepMapState){
        for (const RMap& map : r.maps)
            previous[map.name] = map;
    }
    std::map<std::string, Injection> injections;
    for (const RAction& action : r.actions)
        injections[action.action->getPath()] = action.injection;

    r.maps.clear();
    r.actions.clear();
    r.bindings.clear();
    r.parts.clear();
    for (const InputActionMapDesc& mapDesc : r.asset.maps){
        RMap map;
        map.name = mapDesc.name;
        map.priority = mapDesc.priority;
        map.enabled = mapDesc.enabled;
        map.consume = mapDesc.consume;
        map.exclusive = mapDesc.exclusive;
        map.ignoreUI = mapDesc.ignoreWhenUIConsumes;
        auto old = previous.find(map.name);
        if (old != previous.end()){
            map.enabled = old->second.enabled;
            map.priority = old->second.priority;
            map.gamepad = old->second.gamepad;
        }
        map.firstAction = (int)r.actions.size();
        for (const InputActionDesc& actionDesc : mapDesc.actions){
            const std::string path = mapDesc.name + "/" + actionDesc.name;
            std::unique_ptr<InputAction>& object = r.actionObjects[path];
            if (!object){
                object = std::make_unique<InputAction>();
                object->mapName = mapDesc.name;
                object->name = actionDesc.name;
            }
            if (object->type != actionDesc.type){
                object->type = actionDesc.type;
                InputActionsRuntime::resetActionState(*object);
            }
            RAction action;
            action.action = object.get();
            action.pressPoint = actionDesc.pressPoint;
            action.holdTime = actionDesc.holdTime;
            action.deadzone = actionDesc.deadzone;
            auto injection = injections.find(path);
            if (injection != injections.end())
                action.injection = injection->second;
            action.firstBinding = (int)r.bindings.size();
            for (const InputBindingDesc& bindingDesc : actionDesc.bindings){
                RBinding binding = runtimeBinding(r, bindingDesc);
                if (bindingDesc.isComposite()){
                    binding.firstPart = (int)r.parts.size();
                    binding.partCount = (int)bindingDesc.parts.size();
                    for (const auto& part : bindingDesc.parts)
                        r.parts.push_back(runtimeBinding(r, part.second));
                }
                r.bindings.push_back(std::move(binding));
            }
            action.bindingCount = (int)r.bindings.size() - action.firstBinding;
            r.actions.push_back(action);
        }
        map.actionCount = (int)r.actions.size() - map.firstAction;
        r.maps.push_back(map);
    }
    // the actions the asset no longer has read 0
    for (auto& [path, object] : r.actionObjects){
        if (!r.findAction(path))
            InputActionsRuntime::resetActionState(*object);
    }
    r.modifierEntries.reserve(r.bindings.size() + r.parts.size());
    r.sortOrder();
}

InputOverrideStore& InputActionsRuntime::overrideStore(){
    if (!store)
        store = std::make_shared<UserSettingsStore>();
    return *store;
}

// ---- InputAction

float InputAction::readValue() const{
    switch (type){
        case InputActionType::BUTTON:
        case InputActionType::AXIS:
            return value.x;
        case InputActionType::VECTOR2:
            return std::sqrt(value.x * value.x + value.y * value.y);
    }
    return value.x;
}

Vector2 InputAction::readVector2() const{
    return value;
}

bool InputAction::isPressed() const{
    return pressed;
}

bool InputAction::wasPressedThisFrame() const{
    return Input::isFixedPhase() ? fixedPressed : pressedThisFrame;
}

bool InputAction::wasReleasedThisFrame() const{
    return Input::isFixedPhase() ? fixedReleased : releasedThisFrame;
}

float InputAction::getHeldTime() const{
    return pressed ? heldTime : 0.0f;
}

InputActionPhase InputAction::getPhase() const{
    return phase;
}

std::string InputAction::getConsumedBy() const{
    const InputActionsRuntime& r = rt();
    return consumedByMap >= 0 && consumedByMap < (int)r.maps.size() ? r.maps[consumedByMap].name : std::string();
}

// ---- assets

bool InputActions::load(const std::string& path){
    bool ok = false;
    const std::string text = readAssetText(path, ok);
    if (!ok){
        Log::error("InputActions: cannot read '%s'", path.c_str());
        return false;
    }
    return loadFromText(path, text);
}

bool InputActions::loadFromText(const std::string& path, const std::string& text){
    if (rt().evaluating){
        // from a callback: after the evaluation, which walks the arrays a load rebuilds
        rt().deferred.push_back([path, text](){ loadFromText(path, text); });
        return true;
    }
    InputActionAssetDesc asset;
    std::string error;
    if (!InputActionAssetDesc::parse(text, asset, error)){
        Log::error("InputActions: '%s' is not an input actions asset: %s", path.c_str(), error.c_str());
        return false;
    }
    InputActionsRuntime& r = rt();
    cancelRebind();
    const bool same = r.loaded && r.path == path;
    r.path = path;
    r.asset = std::move(asset);
    r.loaded = true;
    if (!same){
        r.overrides.clear();
        loadOverrides();
    }
    r.build(same);
    return true;
}

bool InputActions::loadDefault(const std::string& path){
    if (path.empty())
        return false;
    return load(path);
}

bool InputActions::reloadText(const std::string& text){
    InputActionsRuntime& r = rt();
    if (!r.loaded)
        return false;
    if (r.evaluating){
        r.deferred.push_back([text](){ reloadText(text); });
        return true;
    }
    InputActionAssetDesc asset;
    std::string error;
    if (!InputActionAssetDesc::parse(text, asset, error)){
        Log::error("InputActions: the edit of '%s' does not parse: %s", r.path.c_str(), error.c_str());
        return false;
    }
    r.asset = std::move(asset);
    r.build(true);
    return true;
}

void InputActions::unload(){
    InputActionsRuntime& r = rt();
    if (r.evaluating){
        r.deferred.push_back([](){ unload(); });
        return;
    }
    cancelRebind();
    r.loaded = false;
    r.path.clear();
    r.asset = InputActionAssetDesc();
    r.overrides.clear();
    r.maps.clear();
    r.actions.clear();
    r.bindings.clear();
    r.parts.clear();
    r.order.clear();
    r.clearConsumed();
    for (auto& [path, object] : r.actionObjects){
        InputActionsRuntime::resetActionState(*object);
        object->onStarted.clear();
        object->onPerformed.clear();
        object->onCanceled.clear();
    }
}

bool InputActions::isLoaded(){
    return rt().loaded;
}

const std::string& InputActions::getAssetPath(){
    return rt().path;
}

const InputActionAssetDesc& InputActions::getAsset(){
    return rt().asset;
}

void InputActions::setDefaultDeadzone(float deadzone){
    rt().defaultDeadzone = std::clamp(deadzone, 0.0f, 0.99f);
}

float InputActions::getDefaultDeadzone(){
    return rt().defaultDeadzone;
}

// ---- actions and maps

InputAction* InputActions::get(const std::string& mapSlashAction){
    InputActionsRuntime& r = rt();
    if (RAction* action = r.findAction(mapSlashAction))
        return action->action;
    // an action the asset no longer has, as a script holds it from before an edit
    auto it = r.actionObjects.find(mapSlashAction);
    return it != r.actionObjects.end() && r.loaded ? it->second.get() : nullptr;
}

std::vector<InputAction*> InputActions::getActions(){
    std::vector<InputAction*> list;
    for (const RAction& action : rt().actions)
        list.push_back(action.action);
    return list;
}

std::vector<std::string> InputActions::getMapNames(){
    std::vector<std::string> names;
    for (const RMap& map : rt().maps)
        names.push_back(map.name);
    return names;
}

void InputActions::enableMap(const std::string& map, bool enabled){
    if (RMap* found = rt().findMap(map))
        found->enabled = enabled;
    else
        Log::warn("InputActions: no action map '%s'", map.c_str());
}

bool InputActions::isMapEnabled(const std::string& map){
    const RMap* found = rt().findMap(map);
    return found && found->enabled;
}

void InputActions::setMapPriority(const std::string& map, int priority){
    InputActionsRuntime& r = rt();
    if (RMap* found = r.findMap(map)){
        found->priority = priority;
        r.sortOrder();
    }else{
        Log::warn("InputActions: no action map '%s'", map.c_str());
    }
}

int InputActions::getMapPriority(const std::string& map){
    const RMap* found = rt().findMap(map);
    return found ? found->priority : 0;
}

void InputActions::setGamepad(const std::string& map, int gamepadId){
    if (RMap* found = rt().findMap(map))
        found->gamepad = gamepadId < 0 ? -1 : gamepadId;
    else
        Log::warn("InputActions: no action map '%s'", map.c_str());
}

int InputActions::getGamepad(const std::string& map){
    const RMap* found = rt().findMap(map);
    return found ? found->gamepad : -1;
}

// ---- script input

void InputActions::setVirtual(const std::string& name, Vector2 value){
    InputActionsRuntime& r = rt();
    const int index = r.virtualIndex(name, true);
    if (index < 0){
        Log::warn("InputActions: more than %i virtual controls, '%s' is ignored", MAX_VIRTUALS, name.c_str());
        return;
    }
    r.virtuals[index].value = value;
}

void InputActions::setVirtual(const std::string& name, float value){
    setVirtual(name, Vector2(value, 0.0f));
}

Vector2 InputActions::getVirtual(const std::string& name){
    InputActionsRuntime& r = rt();
    const int index = r.virtualIndex(name, false);
    return index >= 0 ? r.virtuals[index].value : Vector2(0.0f, 0.0f);
}

void InputActions::inject(const std::string& mapSlashAction, Vector2 value, int windows){
    RAction* action = rt().findAction(mapSlashAction);
    if (!action){
        Log::warn("InputActions: no action '%s' to inject into", mapSlashAction.c_str());
        return;
    }
    action->injection.active = true;
    action->injection.value = value;
    action->injection.tap = windows == 0;
    action->injection.windows = windows < 0 ? -1 : std::max(windows, 1);
}

void InputActions::releaseInjected(const std::string& mapSlashAction){
    if (RAction* action = rt().findAction(mapSlashAction))
        action->injection = Injection();
}

// ---- rebinding

bool InputActions::startRebind(const std::string& mapSlashAction, const std::string& bindingId,
                               const InputRebindOptions& options, std::function<void(const InputRebindResult&)> onComplete){
    InputActionsRuntime& r = rt();
    const RAction* action = r.findAction(mapSlashAction);
    if (!action){
        Log::warn("InputActions: no action '%s' to rebind", mapSlashAction.c_str());
        return false;
    }
    bool owns = false;
    for (int b = 0; b < action->bindingCount && !owns; b++){
        const RBinding& binding = r.bindings[action->firstBinding + b];
        owns = binding.id == bindingId;
        for (int p = 0; p < binding.partCount && !owns; p++)
            owns = r.parts[binding.firstPart + p].id == bindingId;
    }
    if (!owns){
        Log::warn("InputActions: '%s' has no binding '%s'", mapSlashAction.c_str(), bindingId.c_str());
        return false;
    }
    cancelRebind();
    InputRebindState& state = r.rebind;
    state = InputRebindState();
    state.active = true;
    state.action = mapSlashAction;
    state.bindingId = bindingId;
    state.options = options;
    state.callback = std::move(onComplete);
    const RBinding* binding = r.findBinding(bindingId);
    state.wantsStick = binding && binding->control.kind == InputControlKind::GAMEPAD_STICK;
    InputRebind::begin(state);
    return true;
}

void InputActions::cancelRebind(){
    InputRebindState& state = rt().rebind;
    if (!state.active)
        return;
    InputRebindResult result;
    result.status = InputRebindStatus::CANCELED;
    result.action = state.action;
    result.bindingId = state.bindingId;
    std::function<void(const InputRebindResult&)> callback = std::move(state.callback);
    state = InputRebindState();
    if (callback)
        callback(result);
}

bool InputActions::isRebinding(){
    return rt().rebind.active;
}

std::vector<InputBindingInfo> InputActions::getConflicts(const std::string& mapSlashAction, const std::string& bindingId){
    InputActionsRuntime& r = rt();
    std::vector<InputBindingInfo> conflicts;
    const InputBindingDesc* desc = r.asset.findBinding(bindingId);
    const RBinding* binding = r.findBinding(bindingId);
    if (!desc || !binding || !binding->control.isValid())
        return conflicts;
    // a composite part's groups are its composite's
    std::vector<std::string> groups = binding->groups;
    for (const RBinding& top : r.bindings){
        for (int p = 0; p < top.partCount; p++){
            if (r.parts[top.firstPart + p].id == bindingId)
                groups = top.groups;
        }
    }
    auto sharesGroup = [&](const std::vector<std::string>& other){
        if (groups.empty() || other.empty())
            return true;
        return std::any_of(other.begin(), other.end(), [&](const std::string& g){
            return std::find(groups.begin(), groups.end(), g) != groups.end();
        });
    };
    for (size_t m = 0; m < r.maps.size(); m++){
        const RMap& map = r.maps[m];
        if (!map.enabled)
            continue;
        for (int a = map.firstAction; a < map.firstAction + map.actionCount; a++){
            const RAction& action = r.actions[a];
            for (int b = action.firstBinding; b < action.firstBinding + action.bindingCount; b++){
                const RBinding& other = r.bindings[b];
                auto check = [&](const RBinding& candidate, const std::vector<std::string>& candidateGroups){
                    if (candidate.id == bindingId || !(candidate.control == binding->control) || !sharesGroup(candidateGroups))
                        return;
                    conflicts.push_back({action.action->getPath(), candidate.id, candidate.control.toString(), candidateGroups});
                };
                if (other.partCount > 0){
                    for (int p = 0; p < other.partCount; p++)
                        check(r.parts[other.firstPart + p], other.groups);
                }else{
                    check(other, other.groups);
                }
            }
        }
    }
    (void)mapSlashAction;
    return conflicts;
}

std::string InputActions::getBindingPath(const std::string& bindingId){
    const RBinding* binding = rt().findBinding(bindingId);
    return binding ? binding->control.toString() : std::string();
}

void InputActions::setOverride(const std::string& bindingId, const std::string& path){
    InputActionsRuntime& r = rt();
    const InputControlPath control = InputControlPath::parse(path);
    if (!control.isValid()){
        Log::warn("InputActions: '%s' is not a control path", path.c_str());
        return;
    }
    r.overrides[bindingId] = control.toString();
    if (RBinding* binding = r.findBinding(bindingId)){
        binding->control = control;
        binding->virtualIndex = control.kind == InputControlKind::VIRTUAL ? r.virtualIndex(control.name, true) : -1;
    }
}

void InputActions::removeOverride(const std::string& bindingId){
    InputActionsRuntime& r = rt();
    r.overrides.erase(bindingId);
    if (RBinding* binding = r.findBinding(bindingId)){
        if (const InputBindingDesc* desc = r.asset.findBinding(bindingId)){
            binding->control = InputControlPath::parse(desc->path);
            binding->virtualIndex = binding->control.kind == InputControlKind::VIRTUAL ? r.virtualIndex(binding->control.name, true) : -1;
        }
    }
}

std::string InputActions::getOverridesKey(){
    InputActionsRuntime& r = rt();
    const Guid guid = AssetMap::findGuid(r.path);
    return "jasperengine.input." + (guid.isNull() ? r.path : guid.str());
}

bool InputActions::saveOverrides(){
    InputActionsRuntime& r = rt();
    if (!r.loaded)
        return false;
    r.overrideStore().write(getOverridesKey(), overridesJson(r.overrides));
    return true;
}

bool InputActions::loadOverrides(){
    InputActionsRuntime& r = rt();
    if (!r.loaded)
        return false;
    const std::string text = r.overrideStore().read(getOverridesKey());
    if (text.empty())
        return true;
    InputJson json = InputJson::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object() || !json.contains("overrides") || !json["overrides"].is_object()){
        Log::warn("InputActions: the stored rebinding of '%s' is not readable; it is ignored", r.path.c_str());
        return false;
    }
    // ids the asset does not have are kept (a newer build may have them) and ignored
    for (auto it = json["overrides"].begin(); it != json["overrides"].end(); ++it){
        if (it.value().is_string() && InputControlPath::parse(it.value().get<std::string>()).isValid())
            r.overrides[it.key()] = it.value().get<std::string>();
    }
    for (RBinding& binding : r.bindings){
        auto found = r.overrides.find(binding.id);
        if (found != r.overrides.end())
            binding.control = InputControlPath::parse(found->second);
    }
    for (RBinding& part : r.parts){
        auto found = r.overrides.find(part.id);
        if (found != r.overrides.end())
            part.control = InputControlPath::parse(found->second);
    }
    return true;
}

void InputActions::resetOverrides(){
    InputActionsRuntime& r = rt();
    const std::vector<std::pair<std::string, std::string>> previous(r.overrides.begin(), r.overrides.end());
    for (const auto& entry : previous)
        removeOverride(entry.first);
    r.overrides.clear();
    if (r.loaded)
        r.overrideStore().remove(getOverridesKey());
}

void InputActions::setOverrideStore(std::shared_ptr<InputOverrideStore> store){
    rt().store = std::move(store);
}

// ---- display

std::string InputActions::getBindingDisplayName(const std::string& bindingId){
    InputActionsRuntime& r = rt();
    const RBinding* binding = r.findBinding(bindingId);
    if (!binding)
        return std::string();
    int gamepad = -1;
    for (size_t a = 0; a < r.actions.size(); a++){
        const RAction& action = r.actions[a];
        for (int b = action.firstBinding; b < action.firstBinding + action.bindingCount; b++){
            const RBinding& top = r.bindings[b];
            bool mine = &top == binding;
            for (int p = 0; p < top.partCount && !mine; p++)
                mine = &r.parts[top.firstPart + p] == binding;
            if (mine){
                const int m = mapOfAction(r, (int)a);
                if (m >= 0) gamepad = r.maps[m].gamepad;
            }
        }
    }
    if (binding->partCount > 0){
        // "W/A/S/D": up, left, down, right; "Q/E": negative, positive
        std::vector<int> order;
        if (binding->composite == InputCompositeType::DPAD_2D) order = {0, 2, 1, 3};
        else order = {0, 1};
        std::string name;
        for (int index : order){
            if (index >= binding->partCount)
                continue;
            const RBinding& part = r.parts[binding->firstPart + index];
            if (!name.empty()) name += "/";
            name += controlDisplayName(part.control, part.requiredMods, gamepad);
        }
        return name;
    }
    return controlDisplayName(binding->control, binding->requiredMods, gamepad);
}

std::string InputActions::getControlDisplayName(const std::string& path, int mods){
    return controlDisplayName(InputControlPath::parse(path), mods, -1);
}

bool InputActions::isControlBound(const std::string& path){
    InputActionsRuntime& r = rt();
    const InputControlPath control = InputControlPath::parse(path);
    if (!control.isValid() || !r.loaded)
        return false;
    for (const RMap& map : r.maps){
        if (!map.enabled)
            continue;
        for (int a = map.firstAction; a < map.firstAction + map.actionCount; a++){
            const RAction& action = r.actions[a];
            for (int b = action.firstBinding; b < action.firstBinding + action.bindingCount; b++){
                const RBinding& binding = r.bindings[b];
                if (binding.control == control)
                    return true;
                for (int p = 0; p < binding.partCount; p++){
                    if (r.parts[binding.firstPart + p].control == control)
                        return true;
                }
            }
        }
    }
    return false;
}

// ---- Engine

void InputActions::evaluate(float dt){
    InputActionsRuntime& r = rt();
    if (!r.loaded)
        return;
    r.evaluating = true;
    evaluateWindow(dt);
    r.evaluating = false;
    if (!r.deferred.empty()){
        std::vector<std::function<void()>> deferred;
        deferred.swap(r.deferred);
        for (const std::function<void()>& run : deferred)
            run();
    }
}

void InputActions::evaluateWindow(float dt){
    InputActionsRuntime& r = rt();

    // a rebind takes the window's first control; meanwhile every map is suspended
    bool suspended = false;
    if (r.rebind.active){
        suspended = true;
        InputRebindResult result;
        if (InputRebind::scan(r.rebind, dt, result)){
            std::function<void(const InputRebindResult&)> callback = std::move(r.rebind.callback);
            r.rebind = InputRebindState();
            if (result.status == InputRebindStatus::COMPLETED)
                setOverride(result.bindingId, result.path);
            if (callback)
                callback(result);
        }
    }

    r.clearConsumed();
    const int heldMods = Input::getHeldModifierKeys();
    bool blockedAll = false;
    int blockingMap = -1;
    size_t i = 0;
    while (i < r.order.size()){
        const int priority = r.maps[r.order[i]].priority;
        size_t end = i;
        while (end < r.order.size() && r.maps[r.order[end]].priority == priority)
            end++;

        // the level's samples
        for (size_t k = i; k < end; k++){
            RMap& map = r.maps[r.order[k]];
            map.blocked = blockedAll;
            map.blockedBy = blockedAll ? blockingMap : -1;
            const bool active = map.enabled && !blockedAll && !suspended;
            for (int a = map.firstAction; a < map.firstAction + map.actionCount; a++){
                const RAction& action = r.actions[a];
                for (int b = action.firstBinding; b < action.firstBinding + action.bindingCount; b++){
                    RBinding& binding = r.bindings[b];
                    binding.suppressed = false;
                    if (!active){
                        binding.sample = Sample();
                        binding.satisfied = false;
                        continue;
                    }
                    binding.sample = binding.partCount > 0 ? sampleComposite(r, binding, map, action, heldMods)
                                                           : sampleBinding(r, binding, map, action, heldMods);
                }
            }
        }

        // a satisfied modifier binding suppresses the bindings of its key that do not list
        // its modifiers (Ctrl+S does not also fire S)
        r.modifierEntries.clear();
        for (size_t k = i; k < end; k++){
            const RMap& map = r.maps[r.order[k]];
            for (int a = map.firstAction; a < map.firstAction + map.actionCount; a++){
                const RAction& action = r.actions[a];
                for (int b = action.firstBinding; b < action.firstBinding + action.bindingCount; b++){
                    if (r.bindings[b].satisfied && r.bindings[b].sample.actuated && r.bindings[b].partCount == 0)
                        r.modifierEntries.push_back(b);
                }
            }
        }
        if (!r.modifierEntries.empty()){
            for (size_t k = i; k < end; k++){
                const RMap& map = r.maps[r.order[k]];
                for (int a = map.firstAction; a < map.firstAction + map.actionCount; a++){
                    const RAction& action = r.actions[a];
                    for (int b = action.firstBinding; b < action.firstBinding + action.bindingCount; b++){
                        RBinding& binding = r.bindings[b];
                        if (binding.partCount > 0 || !binding.sample.actuated)
                            continue;
                        for (int entry : r.modifierEntries){
                            const RBinding& withMods = r.bindings[entry];
                            if (entry != b && withMods.control == binding.control &&
                                (binding.requiredMods & withMods.requiredMods) != withMods.requiredMods){
                                binding.suppressed = true;
                                break;
                            }
                        }
                    }
                }
            }
        }

        // the actions, in map order; their callbacks may consume controls for lower maps
        for (size_t k = i; k < end; k++){
            const int mapIndex = r.order[k];
            const RMap& map = r.maps[mapIndex];
            const bool active = map.enabled && !map.blocked && !suspended;
            for (int a = map.firstAction; a < map.firstAction + map.actionCount; a++)
                r.updateAction(r.actions[a], mapIndex, active, dt);
        }

        // what the level's consuming maps used is unavailable below; an exclusive map blocks all
        for (size_t k = i; k < end; k++){
            const int mapIndex = r.order[k];
            const RMap& map = r.maps[mapIndex];
            if (!map.enabled || map.blocked || suspended)
                continue;
            if (map.exclusive && !blockedAll){
                blockedAll = true;
                blockingMap = mapIndex;
            }
            if (!map.consume)
                continue;
            for (int a = map.firstAction; a < map.firstAction + map.actionCount; a++){
                const RAction& action = r.actions[a];
                for (int b = action.firstBinding; b < action.firstBinding + action.bindingCount; b++){
                    const RBinding& binding = r.bindings[b];
                    if (binding.suppressed || !binding.sample.actuated)
                        continue;
                    for (int key = 0; key < binding.sample.keyCount; key++)
                        r.consume(binding.sample.keys[key], mapIndex);
                }
            }
        }
        i = end;
    }
}

void InputActions::endFixedStep(){
    for (RAction& action : rt().actions){
        action.action->fixedPressed = false;
        action.action->fixedReleased = false;
    }
}

void InputActions::resetSession(){
    InputActionsRuntime& r = rt();
    cancelRebind();
    for (int i = 0; i < r.virtualCount; i++)
        r.virtuals[i].value = Vector2(0.0f, 0.0f);
    for (auto& [path, object] : r.actionObjects){
        InputActionsRuntime::resetActionState(*object);
        object->onStarted.clear();
        object->onPerformed.clear();
        object->onCanceled.clear();
    }
    if (r.loaded){
        // the maps start again as the asset has them
        r.build(false);
        for (RAction& action : r.actions)
            action.injection = Injection();
    }
    r.clearConsumed();
}

std::vector<InputActions::MapState> InputActions::getMapStates(){
    std::vector<MapState> states;
    InputActionsRuntime& r = rt();
    for (int index : r.order){
        const RMap& map = r.maps[index];
        states.push_back({map.name, map.priority, map.enabled, map.gamepad, map.blocked});
    }
    return states;
}

// ---- handles

std::vector<InputBinding> InputAction::getBindings() const{
    std::vector<InputBinding> list;
    const InputActionsRuntime& r = rt();
    for (const RAction& action : r.actions){
        if (action.action != this)
            continue;
        for (int b = action.firstBinding; b < action.firstBinding + action.bindingCount; b++)
            list.emplace_back(r.bindings[b].id);
    }
    return list;
}

bool InputBinding::isValid() const{
    return rt().findBinding(id) != nullptr;
}

bool InputBinding::isComposite() const{
    const RBinding* binding = rt().findBinding(id);
    return binding && binding->partCount > 0;
}

std::string InputBinding::getPath() const{
    return InputActions::getBindingPath(id);
}

bool InputBinding::hasOverride() const{
    return rt().overrides.count(id) > 0;
}

std::vector<std::string> InputBinding::getGroups() const{
    const RBinding* binding = rt().findBinding(id);
    return binding ? binding->groups : std::vector<std::string>();
}

std::vector<InputBinding> InputBinding::getParts() const{
    std::vector<InputBinding> list;
    InputActionsRuntime& r = rt();
    const RBinding* binding = r.findBinding(id);
    if (!binding)
        return list;
    for (int p = 0; p < binding->partCount; p++)
        list.emplace_back(r.parts[binding->firstPart + p].id);
    return list;
}

std::string InputBinding::getDisplayName() const{
    return InputActions::getBindingDisplayName(id);
}

bool InputActionMap::isValid() const{
    return rt().findMap(name) != nullptr;
}

bool InputActionMap::isEnabled() const{
    return InputActions::isMapEnabled(name);
}

void InputActionMap::setEnabled(bool enabled){
    InputActions::enableMap(name, enabled);
}

int InputActionMap::getPriority() const{
    return InputActions::getMapPriority(name);
}

void InputActionMap::setPriority(int priority){
    InputActions::setMapPriority(name, priority);
}

int InputActionMap::getGamepad() const{
    return InputActions::getGamepad(name);
}

void InputActionMap::setGamepad(int gamepadId){
    InputActions::setGamepad(name, gamepadId);
}

std::vector<InputAction*> InputActionMap::getActions() const{
    std::vector<InputAction*> list;
    InputActionsRuntime& r = rt();
    const RMap* map = r.findMap(name);
    if (!map)
        return list;
    for (int a = map->firstAction; a < map->firstAction + map->actionCount; a++)
        list.push_back(r.actions[a].action);
    return list;
}

InputActionMap InputActions::getMap(const std::string& name){
    return InputActionMap(name);
}

InputBinding InputActions::getBinding(const std::string& id){
    return InputBinding(id);
}
