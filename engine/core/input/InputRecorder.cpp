// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Only with the input scripts module (input/InputScriptSwitch.h); InputScriptsOff.cpp answers the
// engine without it
#include "input/InputScriptSwitch.h"
#if JASPERENGINE_INPUT_SCRIPTS_ENABLED

#include "input/InputRecorder.h"

#include "Engine.h"
#include "Input.h"
#include "Log.h"
#include "Screen.h"
#include "input/StateHash.h"
#include "math/Rect.h"
#include "util/PlatformRules.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace jasperengine::input {

std::atomic<int> InputRecorder::active{0};

namespace {

    constexpr int ACTIVE_RECORDING = 1;
    constexpr int ACTIVE_GATE = 2;
    constexpr size_t CHUNK_RECORDS = 64 * 1024 / sizeof(RecordedEvent);
    constexpr size_t MAX_CHECKPOINT_VALUES = 64;

    struct Gate {
        uint64_t id = 0;
        bool block = false;
        std::function<void()> onRealPress;
    };

    struct RecorderState {
        std::mutex mutex;
        bool recording = false;
        RecordOptions options;
        std::shared_ptr<RecordedInput> buffer;
        uint64_t startFrames = 0;
        uint32_t truncatedFrame = 0;     // the frame the cap stopped it on: where its replay ends
        bool pointerKnown = false;       // where the last recorded mouse event left the pointer
        float pointerX = 0.0f, pointerY = 0.0f;
        size_t bytes = 0;
        bool truncated = false;
        std::vector<std::string> probePaths;
        std::vector<Checkpoint> checkpoints;
        ScriptJson values = ScriptJson::object();
        std::vector<Gate> gates;
        uint64_t nextGate = 1;
    };

    RecorderState& recorder(){
        static RecorderState s;
        return s;
    }

    void updateActive(RecorderState& s, std::atomic<int>& active){
        active.store((s.recording ? ACTIVE_RECORDING : 0) | (s.gates.empty() ? 0 : ACTIVE_GATE), std::memory_order_relaxed);
    }

    bool isLifecycle(RecordKind kind){
        switch (kind) {
            case RecordKind::Pause: case RecordKind::Resume: case RecordKind::FocusLost: case RecordKind::FocusGained:
            case RecordKind::LowMemory: case RecordKind::Orientation: case RecordKind::Hide: case RecordKind::Show:
            case RecordKind::GamepadConnect: case RecordKind::GamepadDisconnect:
                return true;
            default:
                return false;
        }
    }

    bool isPress(RecordKind kind){
        return kind == RecordKind::KeyDown || kind == RecordKind::MouseDown || kind == RecordKind::TouchStart ||
               kind == RecordKind::GamepadButtonDown || kind == RecordKind::Back;
    }

    bool hasPoint(RecordKind kind){
        switch (kind) {
            case RecordKind::MouseDown: case RecordKind::MouseUp: case RecordKind::MouseMove:
            case RecordKind::TouchStart: case RecordKind::TouchMove: case RecordKind::TouchEnd:
                return true;
            default:
                return false;
        }
    }

    // engine (view) pixels -> 0..1 of the canvas
    void normalizePoint(float& x, float& y){
        const Rect view = Engine::getViewRect();
        if (view.getWidth() <= 0 || view.getHeight() <= 0)
            return;
        x = (x - view.getX()) / view.getWidth();
        y = (y - view.getY()) / view.getHeight();
    }

    void appendUtf8(std::string& out, uint32_t cp){
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    double recorderRound4(double v){
        return std::round(v * 10000.0) / 10000.0;
    }

    // the record after the last one, a chunk added when the last is full
    RecordedEvent* appendRecord(std::vector<std::unique_ptr<RecordedEvent[]>>& chunks, size_t& count){
        if (count % CHUNK_RECORDS == 0)
            chunks.emplace_back(new RecordedEvent[CHUNK_RECORDS]);
        RecordedEvent* record = &chunks.back()[count % CHUNK_RECORDS];
        count++;
        return record;
    }

}

// ================================================================== the recorded buffer

size_t RecordedInput::events() const{
    return count;
}

uint32_t RecordedInput::lastFrame() const{
    if (count == 0)
        return 0;
    return chunks[(count - 1) / CHUNK_RECORDS][(count - 1) % CHUNK_RECORDS].frame;
}

std::shared_ptr<ParsedScript> RecordedInput::toScript() const{
    auto script = std::make_shared<ParsedScript>();
    script->header = header;
    script->header.kind = ScriptKind::Recording;
    script->header.space = Space::Normalized;
    auto textAt = [this](uint32_t offset) -> std::string {
        if (offset == UINT32_MAX || offset + sizeof(uint32_t) > textPool.size())
            return std::string();
        uint32_t length = 0;
        std::memcpy(&length, textPool.data() + offset, sizeof(length));
        return textPool.substr(offset + sizeof(length), length);
    };
    auto addString = [&](const std::string& text) {
        script->strings.push_back(text);
        return static_cast<int32_t>(script->strings.size() - 1);
    };
    auto point = [](const RecordedEvent& e) {
        ScriptPoint p;
        p.given = true;
        p.space = Space::Normalized;
        p.x = recorderRound4(e.x);
        p.y = recorderRound4(e.y);
        return p;
    };
    uint32_t event = 0;
    for (size_t i = 0; i < count; i++) {
        const RecordedEvent& e = chunks[i / CHUNK_RECORDS][i % CHUNK_RECORDS];
        ScriptAction a;
        a.frame = e.frame;
        a.event = event;
        std::string label;
        switch (e.kind) {
            case RecordKind::KeyDown: a.type = ActionType::KeyDown; a.code = e.code; a.mods = e.mods; label = "key down"; break;
            case RecordKind::KeyUp: a.type = ActionType::KeyUp; a.code = e.code; a.mods = e.mods; label = "key up"; break;
            case RecordKind::Char: {
                // the units of a frame, joined; a surrogate pair is one character
                std::string text;
                size_t k = i;
                for (; k < count; k++) {
                    const RecordedEvent& c = chunks[k / CHUNK_RECORDS][k % CHUNK_RECORDS];
                    if (c.kind != RecordKind::Char || c.frame != e.frame)
                        break;
                    uint32_t unit = static_cast<uint32_t>(c.code);
                    if (unit >= 0xD800 && unit <= 0xDBFF && k + 1 < count) {
                        const RecordedEvent& low = chunks[(k + 1) / CHUNK_RECORDS][(k + 1) % CHUNK_RECORDS];
                        const uint32_t lowUnit = static_cast<uint32_t>(low.code);
                        if (low.kind == RecordKind::Char && low.frame == e.frame && lowUnit >= 0xDC00 && lowUnit <= 0xDFFF) {
                            appendUtf8(text, 0x10000 + ((unit - 0xD800) << 10) + (lowUnit - 0xDC00));
                            k++;
                            continue;
                        }
                    }
                    appendUtf8(text, unit >= 0xD800 && unit <= 0xDFFF ? 0xFFFD : unit);
                }
                i = k - 1;
                a.type = ActionType::TextUnits;
                a.string = addString(text);
                label = "text";
                break;
            }
            case RecordKind::Text: a.type = ActionType::Text; a.string = addString(textAt(e.text)); label = "text"; break;
            case RecordKind::Ime: a.type = ActionType::Ime; a.string = addString(textAt(e.text)); a.count = e.code; label = "ime"; break;
            case RecordKind::ImeCancel: a.type = ActionType::ImeCancel; label = "ime_cancel"; break;
            case RecordKind::MouseDown: a.type = ActionType::MouseDown; a.code = e.code; a.mods = e.mods; a.point = point(e); label = "mouse down"; break;
            case RecordKind::MouseUp: a.type = ActionType::MouseUp; a.code = e.code; a.mods = e.mods; a.point = point(e); label = "mouse up"; break;
            case RecordKind::MouseMove: a.type = ActionType::MouseMove; a.mods = e.mods; a.point = point(e); label = "mouse move"; break;
            case RecordKind::MouseScroll:
                a.type = ActionType::MouseScroll;
                a.mods = e.mods;
                a.value2 = e.x;
                a.value = e.y;
                label = "mouse scroll";
                break;
            case RecordKind::MouseEnter: a.type = ActionType::MouseEnter; label = "mouse_enter"; break;
            case RecordKind::MouseLeave: a.type = ActionType::MouseLeave; label = "mouse_leave"; break;
            case RecordKind::TouchStart: a.type = ActionType::TouchStart; a.device = e.device; a.point = point(e); label = "touch start"; break;
            case RecordKind::TouchMove: a.type = ActionType::TouchMove; a.device = e.device; a.point = point(e); label = "touch move"; break;
            case RecordKind::TouchEnd: a.type = ActionType::TouchEnd; a.device = e.device; a.point = point(e); label = "touch end"; break;
            case RecordKind::TouchCancel: a.type = ActionType::TouchCancel; a.device = e.device; label = "touch cancel"; break;
            case RecordKind::GamepadConnect:
                a.type = ActionType::GamepadConnect;
                a.device = e.device;
                a.string = addString(textAt(e.text));
                label = "gamepad connect";
                break;
            case RecordKind::GamepadDisconnect: a.type = ActionType::GamepadDisconnect; a.device = e.device; label = "gamepad disconnect"; break;
            case RecordKind::GamepadButtonDown: a.type = ActionType::GamepadButtonDown; a.device = e.device; a.code = e.code; label = "gamepad down"; break;
            case RecordKind::GamepadButtonUp: a.type = ActionType::GamepadButtonUp; a.device = e.device; a.code = e.code; label = "gamepad up"; break;
            case RecordKind::GamepadAxis:
                a.type = ActionType::GamepadAxis;
                a.device = e.device;
                a.code = e.code;
                a.value = e.value;
                label = "gamepad axis";
                break;
            case RecordKind::Pause: a.type = ActionType::AppPause; label = "app pause"; break;
            case RecordKind::Resume: a.type = ActionType::AppResume; label = "app resume"; break;
            case RecordKind::FocusLost: a.type = ActionType::AppFocusLost; label = "app focus_lost"; break;
            case RecordKind::FocusGained: a.type = ActionType::AppFocusGained; label = "app focus_gained"; break;
            case RecordKind::LowMemory: a.type = ActionType::AppLowMemory; label = "app low_memory"; break;
            case RecordKind::Hide: a.type = ActionType::AppHide; label = "app hide"; break;
            case RecordKind::Show: a.type = ActionType::AppShow; label = "app show"; break;
            case RecordKind::Back: a.type = ActionType::AppBack; a.code = e.code; label = "app back"; break;
            case RecordKind::Orientation:
                a.type = ActionType::AppOrientation;
                a.string = addString(textAt(e.text));
                label = "app orientation";
                break;
            case RecordKind::Forced:
                a.type = static_cast<ActionType>(e.code);
                a.string = addString(textAt(e.text));
                a.value = e.x;
                a.value2 = e.y;
                a.count = e.extra;
                a.code = e.flags;
                label = actionTypeName(a.type);
                break;
        }
        script->actions.push_back(a);
        script->eventLabels.push_back(label);
        event++;
    }
    script->events = event;
    return script;
}

// ================================================================== recording

bool InputRecorder::begin(const RecordOptions& options, std::string& error){
    RecorderState& s = recorder();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.recording) {
        error = "an input recording is already running: stop it first";
        return false;
    }
    s.options = options;
    s.buffer = std::make_shared<RecordedInput>();
    s.buffer->header = options.header;
    s.buffer->header.kind = ScriptKind::Recording;
    s.buffer->textPool.reserve(64 * 1024);
    s.startFrames = Engine::getGameFrames();
    s.bytes = 0;
    s.truncated = false;
    s.pointerKnown = false;
    s.checkpoints.clear();
    // the probes, chosen now (R17)
    const std::vector<Scene*> scenes = Engine::getSceneLayers();
    const std::vector<StateProbe> probes = options.probes.empty() ? chooseProbes(scenes) : findProbes(scenes, options.probes);
    s.probePaths.clear();
    for (const StateProbe& probe : probes)
        s.probePaths.push_back(probe.path);
    s.buffer->header.probes = s.probePaths;
    s.recording = true;
    updateActive(s, active);
    return true;
}

bool InputRecorder::isRecording(){
    RecorderState& s = recorder();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.recording;
}

namespace {
    std::atomic<int> engineMadeDepth{0};
}

InputRecorder::EngineMade::EngineMade(){
    engineMadeDepth.fetch_add(1, std::memory_order_relaxed);
}

InputRecorder::EngineMade::~EngineMade(){
    engineMadeDepth.fetch_sub(1, std::memory_order_relaxed);
}

bool InputRecorder::record(const RecordedEvent& event, std::string_view text){
    if (engineMadeDepth.load(std::memory_order_relaxed) > 0)
        return true;
    RecorderState& s = recorder();
    std::function<void()> stopReplay;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        // a replay's gate: what the interpreter makes passes, a real press may end an attract loop
        if (!s.gates.empty() && !ScriptPlayer::isInjecting() && !isLifecycle(event.kind)) {
            bool blocked = false;
            for (auto it = s.gates.begin(); it != s.gates.end();) {
                if (it->onRealPress && isPress(event.kind)) {
                    stopReplay = it->onRealPress;
                    it = s.gates.erase(it);
                    continue;
                }
                blocked = blocked || it->block;
                ++it;
            }
            updateActive(s, active);
            if (blocked && !stopReplay)
                return false;
        }
        // a move to where the pointer already is (the editor's Game view sends one each frame it is
        // hovered) changes nothing a replay needs; captured, a move is motion wherever it lands
        const bool unmoved = event.kind == RecordKind::MouseMove && s.pointerKnown && event.x == s.pointerX &&
                             event.y == s.pointerY && Engine::getMouseMode() != MouseMode::CAPTURED;
        if (s.recording && (event.kind == RecordKind::MouseMove || event.kind == RecordKind::MouseDown || event.kind == RecordKind::MouseUp)) {
            s.pointerKnown = true;
            s.pointerX = event.x;
            s.pointerY = event.y;
        }
        if (s.recording && !unmoved && !(event.kind == RecordKind::KeyDown && (event.flags & 1))) {
            RecordedInput& buffer = *s.buffer;
            const uint32_t frame = static_cast<uint32_t>(Engine::getGameFrames() - s.startFrames);
            // consecutive mouse moves of a frame: the last one (all of them while the pointer is captured)
            RecordedEvent* last = buffer.count ? &buffer.chunks.back()[(buffer.count - 1) % CHUNK_RECORDS] : nullptr;
            RecordedEvent* slot = nullptr;
            if (event.kind == RecordKind::MouseMove && last && last->kind == RecordKind::MouseMove && last->frame == frame &&
                Engine::getMouseMode() != MouseMode::CAPTURED) {
                slot = last;
            } else if (s.bytes + sizeof(RecordedEvent) + (text.empty() ? 0 : text.size() + sizeof(uint32_t)) > s.options.maxBytes) {
                s.recording = false;
                s.truncated = true;
                s.truncatedFrame = frame;
                buffer.header.truncated = true;
                updateActive(s, active);
                Log::warn("Input recording stopped at frame %u: it reached its cap of %zu bytes (the file is marked truncated)",
                          frame, s.options.maxBytes);
            } else {
                slot = appendRecord(buffer.chunks, buffer.count);
                s.bytes += sizeof(RecordedEvent);
            }
            if (slot) {
                *slot = event;
                slot->frame = frame;
                if (hasPoint(event.kind))
                    normalizePoint(slot->x, slot->y);
                if (!text.empty() || event.kind == RecordKind::Text || event.kind == RecordKind::Ime ||
                    event.kind == RecordKind::GamepadConnect || event.kind == RecordKind::Orientation) {
                    const uint32_t length = static_cast<uint32_t>(text.size());
                    slot->text = static_cast<uint32_t>(buffer.textPool.size());
                    buffer.textPool.append(reinterpret_cast<const char*>(&length), sizeof(length));
                    buffer.textPool.append(text.data(), text.size());
                    s.bytes += sizeof(length) + text.size();
                }
            }
        }
    }
    if (stopReplay)
        stopReplay();
    return true;
}

void InputRecorder::recordForced(const ScriptAction& action, const ParsedScript& script){
    RecordedEvent e;
    e.kind = RecordKind::Forced;
    e.code = static_cast<int32_t>(action.type);
    e.flags = static_cast<uint8_t>(action.code);
    e.x = action.value;
    e.y = action.value2;
    e.extra = static_cast<int16_t>(std::clamp(action.count, -32768, 32767));
    record(e, script.stringAt(action.string));
}

void InputRecorder::afterUpdate(){
    RecorderState& s = recorder();
    std::vector<std::string> probes;
    uint32_t frame = 0;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.recording || s.options.checkpointFrames <= 0)
            return;
        frame = static_cast<uint32_t>(Engine::getGameFrames() - s.startFrames);
        if (frame == 0 || frame % static_cast<uint32_t>(s.options.checkpointFrames) != 0)
            return;
        probes = s.probePaths;
    }
    Checkpoint checkpoint = sampleCheckpoint(frame, probes);
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.recording)
        s.checkpoints.push_back(std::move(checkpoint));
}

std::shared_ptr<RecordedInput> InputRecorder::end(){
    RecorderState& s = recorder();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.buffer)
        return nullptr;
    std::shared_ptr<RecordedInput> buffer = std::move(s.buffer);
    buffer->header.checkpoints = std::move(s.checkpoints);
    buffer->header.truncated = s.truncated;
    buffer->header.frames = s.truncated ? s.truncatedFrame : static_cast<uint32_t>(Engine::getGameFrames() - s.startFrames);
    s.checkpoints.clear();
    s.recording = false;
    s.buffer.reset();
    updateActive(s, active);
    return buffer;
}

uint32_t InputRecorder::frame(){
    RecorderState& s = recorder();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.buffer ? static_cast<uint32_t>(Engine::getGameFrames() - s.startFrames) : 0;
}

size_t InputRecorder::events(){
    RecorderState& s = recorder();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.buffer ? s.buffer->count : 0;
}

bool InputRecorder::truncated(){
    RecorderState& s = recorder();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.truncated;
}

// ================================================================== checkpoint values

bool InputRecorder::checkpointValue(const std::string& name, const ScriptJson& value, std::string& error){
    RecorderState& s = recorder();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (name.empty()) {
        error = "checkpoint needs a name";
        return false;
    }
    if (!s.values.contains(name) && s.values.size() >= MAX_CHECKPOINT_VALUES) {
        error = "at most 64 checkpoint values: reuse a name";
        return false;
    }
    s.values[name] = value;
    return true;
}

const ScriptJson& InputRecorder::checkpointValues(){
    return recorder().values;
}

void InputRecorder::clearCheckpointValues(){
    RecorderState& s = recorder();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.values = ScriptJson::object();
}

// ================================================================== real-input gates

uint64_t InputRecorder::addRealInputGate(bool block, std::function<void()> onRealPress){
    RecorderState& s = recorder();
    std::lock_guard<std::mutex> lock(s.mutex);
    const uint64_t id = s.nextGate++;
    s.gates.push_back({id, block, std::move(onRealPress)});
    updateActive(s, active);
    return id;
}

void InputRecorder::removeRealInputGate(uint64_t id){
    RecorderState& s = recorder();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.gates.erase(std::remove_if(s.gates.begin(), s.gates.end(), [id](const Gate& g) { return g.id == id; }), s.gates.end());
    updateActive(s, active);
}

// ================================================================== checkpoints

Checkpoint sampleCheckpoint(uint32_t frame, const std::vector<std::string>& probePaths){
    Checkpoint checkpoint;
    checkpoint.frame = frame;
    const std::vector<Scene*> scenes = Engine::getSceneLayers();
    ScriptJson values;
    {
        RecorderState& s = recorder();
        std::lock_guard<std::mutex> lock(s.mutex);
        values = s.values;
    }
    checkpoint.hash = hashState(scenes, values);
    for (const StateProbe& probe : findProbes(scenes, probePaths))
        checkpoint.probes.push_back(probePosition(probe));
    checkpoint.values = values;
    return checkpoint;
}

bool compareCheckpoint(const Checkpoint& recorded, const Checkpoint& replayed, const std::vector<std::string>& probePaths,
                       ScriptJson& differences){
    differences = ScriptJson::array();
    for (size_t i = 0; i < recorded.probes.size() && i < replayed.probes.size(); i++) {
        const Vector3& a = recorded.probes[i];
        const Vector3& b = replayed.probes[i];
        const double distance = std::sqrt(double(a.x - b.x) * (a.x - b.x) + double(a.y - b.y) * (a.y - b.y) + double(a.z - b.z) * (a.z - b.z));
        if (distance > 1.0 / 1024.0) {
            differences.push_back({{"probe", i < probePaths.size() ? probePaths[i] : std::to_string(i)},
                                   {"recorded", {recorderRound4(a.x), recorderRound4(a.y), recorderRound4(a.z)}},
                                   {"replayed", {recorderRound4(b.x), recorderRound4(b.y), recorderRound4(b.z)}},
                                   {"distance", recorderRound4(distance)}});
        }
    }
    for (auto it = recorded.values.begin(); it != recorded.values.end(); ++it) {
        const ScriptJson other = replayed.values.contains(it.key()) ? replayed.values[it.key()] : ScriptJson();
        if (other != it.value())
            differences.push_back({{"value", it.key()}, {"recorded", it.value()}, {"replayed", other}});
    }
    return recorded.hash == replayed.hash && differences.empty();
}

namespace {
    std::atomic<bool> hasPendingSeed{false};
    std::atomic<uint32_t> pendingSeed{0};
}

void setPendingLuaSeed(uint32_t seed){
    pendingSeed.store(seed);
    hasPendingSeed.store(true);
}

bool takePendingLuaSeed(uint32_t& seed){
    if (!hasPendingSeed.exchange(false))
        return false;
    seed = pendingSeed.load();
    return true;
}

std::string platformTag(){
#if defined(__EMSCRIPTEN__)
    const char* os = "web";
#elif defined(__ANDROID__)
    const char* os = "android";
#elif defined(__APPLE__)
    #if TARGET_OS_IPHONE
    const char* os = "ios";
    #else
    const char* os = "macos";
    #endif
#elif defined(_WIN32)
    const char* os = "windows";
#else
    const char* os = "linux";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    const char* arch = "arm64";
#elif defined(__arm__) || defined(_M_ARM)
    const char* arch = "arm";
#elif defined(__x86_64__) || defined(_M_X64)
    const char* arch = "x64";
#elif defined(__i386__) || defined(_M_IX86)
    const char* arch = "x86";
#elif defined(__wasm__)
    const char* arch = "wasm";
#else
    const char* arch = "unknown";
#endif
    return std::string(os) + "-" + arch;
}

}

#endif // JASPERENGINE_INPUT_SCRIPTS_ENABLED
