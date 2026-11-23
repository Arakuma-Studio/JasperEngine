// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef InputRecorder_h
#define InputRecorder_h

// The input recorder (docs/specs/input-recorder-and-input-scripts.md R10-R19): every
// Engine::system* input entry point starts with
//
//     if (InputRecorder::isActive() && !InputRecorder::record(...)) return;
//
// before it dispatches. Idle that is one relaxed load. Recording, the event is stamped with the
// update frames run since the recording began and kept in 32-byte records (64 KB chunks, text
// pooled); a replay that blocks real input refuses the device events the interpreter did not
// make (lifecycle events always pass). Checkpoints sample the state every N frames after the
// update.

#include "Export.h"
#include "input/InputScript.h"
#include "input/InputScriptSwitch.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::input {

    enum class RecordKind : uint8_t {
        KeyDown, KeyUp, Char, Text, Ime, ImeCancel,
        MouseDown, MouseUp, MouseMove, MouseScroll, MouseEnter, MouseLeave,
        TouchStart, TouchMove, TouchEnd, TouchCancel,
        GamepadConnect, GamepadDisconnect, GamepadButtonDown, GamepadButtonUp, GamepadAxis,
        Pause, Resume, FocusLost, FocusGained, LowMemory, Orientation, Hide, Show,
        Forced,     // an action the interpreter forced (input_action, action, axis): code is its ActionType
        Back,       // Engine::systemBack from a source no other record carries: code is its BackSource
    };

    // One event at an entry point. Points are engine (view) pixels at the site; the recorder keeps
    // them as 0..1 of the canvas.
    struct RecordedEvent {
        uint32_t frame = 0;         // set by the recorder
        RecordKind kind = RecordKind::KeyDown;
        uint8_t flags = 0;
        int16_t mods = 0;
        int32_t code = 0;           // key, mouse button, gamepad button / axis, char unit, ime cursor, ActionType
        int16_t device = 0;         // pointer, gamepad
        int16_t extra = 0;          // a forced press's windows
        float x = 0.0f, y = 0.0f;   // point; scroll dx, dy; a forced value
        float value = 0.0f;         // axis value
        uint32_t text = UINT32_MAX; // the recorder's text pool

        static RecordedEvent of(RecordKind kind, int32_t code = 0, int16_t device = 0, float x = 0.0f, float y = 0.0f, int mods = 0){
            RecordedEvent e;
            e.kind = kind;
            e.code = code;
            e.device = device;
            e.x = x;
            e.y = y;
            e.mods = static_cast<int16_t>(mods);
            return e;
        }
    };
    static_assert(sizeof(RecordedEvent) == 32, "a record is 32 bytes (R32)");

    // a key press (a repeat is flagged: the recorder drops it, the engine regenerates it)
    inline RecordedEvent keyRecord(RecordKind kind, int key, bool repeat, int mods){
        RecordedEvent e = RecordedEvent::of(kind, key, 0, 0.0f, 0.0f, mods);
        e.flags = repeat ? 1 : 0;
        return e;
    }

    inline RecordedEvent axisRecord(int gamepad, int axis, float value){
        RecordedEvent e = RecordedEvent::of(RecordKind::GamepadAxis, axis, static_cast<int16_t>(gamepad));
        e.value = value;
        return e;
    }

    struct RecordOptions {
        ScriptHeader header;            // fromPlayStart, fixedFps, seed, scene, canvas, simulateTouch, platform...
        size_t maxBytes = SCRIPT_MAX_BYTES;
        int checkpointFrames = 60;      // 0: none
        std::vector<std::string> probes;    // GameObject paths; empty: chosen (the first 16 that move)
    };

    // A stopped recording: its records, turned into a script off the main thread (toScript)
    class JASPERENGINE_API RecordedInput {
    public:
        // not copyable: MSVC defines every special member of a dllexport class, and the implicit copy
        // of the chunks' unique_ptrs does not compile
        RecordedInput() = default;
        RecordedInput(const RecordedInput&) = delete;
        RecordedInput& operator=(const RecordedInput&) = delete;

        ScriptHeader header;
        size_t events() const;
        uint32_t lastFrame() const;
        // the recording as actions (points normalized, char units joined per frame)
        std::shared_ptr<ParsedScript> toScript() const;

    private:
        friend class InputRecorder;
        std::vector<std::unique_ptr<RecordedEvent[]>> chunks;
        size_t count = 0;
        std::string textPool;
    };

    class JASPERENGINE_API InputRecorder {
    public:
#if JASPERENGINE_INPUT_SCRIPTS_ENABLED
        static bool isActive() { return active.load(std::memory_order_relaxed) != 0; }
#else
        // without the module nothing records or gates: the entry points' test folds away
        static constexpr bool isActive() { return false; }
#endif

        // starts recording from the next event; false + error while one runs
        static bool begin(const RecordOptions& options, std::string& error);
        static bool isRecording();
        // the site of an entry point: false when the event must not reach the game
        static bool record(const RecordedEvent& event, std::string_view text = std::string_view());
        // events the engine makes from one already recorded (the Escape an unconsumed Back presses):
        // they reach the game unrecorded, as a replay makes them again
        struct JASPERENGINE_API EngineMade {
            EngineMade();
            ~EngineMade();
        };
        // what the interpreter forced (never game code: an on-screen stick re-derives from its touches)
        static void recordForced(const ScriptAction& action, const ParsedScript& script);
        // Engine::systemDraw, after the frame's update: checkpoints
        static void afterUpdate();
        // the recording; nullptr when none runs. The swap is O(1): encode toScript() on a job
        static std::shared_ptr<RecordedInput> end();

        // the frames, events and bytes of the recording that runs (0 when none)
        static uint32_t frame();
        static size_t events();
        static bool truncated();

        // InputRecording.checkpoint(name, value): the latest value of each name (at most 64), sampled
        // by recordings' checkpoints and compared by replays'
        static bool checkpointValue(const std::string& name, const ScriptJson& value, std::string& error);
        static const ScriptJson& checkpointValues();
        static void clearCheckpointValues();

        // A replay's hold on real input (R16, R28): `block` refuses device events the interpreter
        // did not make; `onRealPress` (when set) is called at the first real key, button or touch
        // press, which then reaches the game. Lifecycle events always pass. Returns its id.
        static uint64_t addRealInputGate(bool block, std::function<void()> onRealPress);
        static void removeRealInputGate(uint64_t id);

#if JASPERENGINE_INPUT_SCRIPTS_ENABLED
    private:
        static std::atomic<int> active;     // bit 1: recording, bit 2: a real-input gate
#endif
    };

    // The checkpoint of the state now (R17): the hash of the running scenes and the values, the
    // probes' positions
    JASPERENGINE_API Checkpoint sampleCheckpoint(uint32_t frame, const std::vector<std::string>& probePaths);
    // A replay's checkpoint against the recorded one: false + the differences (probes farther
    // apart than 1/1024, values that differ)
    JASPERENGINE_API bool compareCheckpoint(const Checkpoint& recorded, const Checkpoint& replayed,
                                            const std::vector<std::string>& probePaths, ScriptJson& differences);
    // A recording's seed for the next Lua state: math.randomseed(seed) right after its libraries
    // open, before any script's init (R16). Taken once.
    JASPERENGINE_API void setPendingLuaSeed(uint32_t seed);
    JASPERENGINE_API bool takePendingLuaSeed(uint32_t& seed);

    // "linux-x64", "android-arm64": the platform tag checkpoints compare on (R19)
    JASPERENGINE_API std::string platformTag();

}

#endif
