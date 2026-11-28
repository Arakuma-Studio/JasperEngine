// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef InputScript_h
#define InputScript_h

// Input scripts and recordings (docs/specs/input-recorder-and-input-scripts.md R1-R9): the
// `.input.json` format v1 and the one interpreter that applies it through the engine's own input
// entry points (Engine::system*, InputActions), shared by send_input, Test.input, players and Lua.
//
// A file is parsed into actions, each one engine-level operation (a key down, a touch start, a
// wait) with the frames to let pass after it: a composite event (a key press, a tap, a drag)
// becomes several. A recording stamps each event with the update frame it arrived on; a script's
// events follow each other (an event's own frames, then gap_frames), as send_input's do.
//
// A ScriptPlayer applies the actions due before the input latch of a running frame
// (Input::beginFrame), so an event lands in the same pending window as on the recorded run. The
// host (the editor, a player) supplies what differs between them: UI Document targets, the Lua
// condition of `wait until`, shots.

#include "Export.h"
#include "math/Vector2.h"
#include "math/Vector3.h"

#include "json.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::input {

    using ScriptJson = nlohmann::json;

    // the format's name and version (R1)
    inline constexpr const char* SCRIPT_FORMAT = "jasperengine-input";
    inline constexpr int SCRIPT_VERSION = 1;
    // a file larger than this, or with more events, is refused (R6)
    inline constexpr size_t SCRIPT_MAX_BYTES = 64u * 1024u * 1024u;
    inline constexpr size_t SCRIPT_MAX_EVENTS = 2000000u;
    // the frames a file's `wait {until}` waits at most when it gives none (R4)
    inline constexpr int WAIT_UNTIL_DEFAULT_FRAMES = 600;

    enum class ScriptKind : uint8_t { Recording, Script };

    // Where an event's x, y are: canvas units, 0..1 of the canvas, 0..1 of Screen.safeArea, or the
    // editor's Game view pixels (live send_input calls only: a file cannot store it)
    enum class Space : uint8_t { Canvas, Normalized, SafeArea, View };

    JASPERENGINE_API const char* spaceName(Space space);
    JASPERENGINE_API bool parseSpace(std::string_view name, Space& out);

    // A state sample of a recording (R17): stored by the recorder, compared on replay
    struct Checkpoint {
        uint32_t frame = 0;
        uint64_t hash = 0;
        std::vector<Vector3> probes;                    // the header's probes, in order
        ScriptJson values = ScriptJson::object();       // InputRecording.checkpoint(name, value)

        bool operator==(const Checkpoint& other) const;
    };

    struct ScriptHeader {
        ScriptKind kind = ScriptKind::Script;
        bool fromPlayStart = false;     // start: "play" (else "now")
        int fixedFps = 60;
        Space space = Space::Canvas;    // of the events that give none
        int gapFrames = 1;              // scripts: frames after each event
        bool hasSeed = false;
        uint32_t seed = 0;
        std::string scene;
        std::string platform;           // "linux-x64": checkpoints compare only on the same (R19)
        std::string engineVersion;
        Vector2 canvas = Vector2(0, 0);
        bool simulateTouch = false;
        std::vector<std::string> probes;
        std::vector<Checkpoint> checkpoints;
        bool truncated = false;
        uint32_t frames = 0;            // recordings: update frames from start to stop (0: unknown, to the last event)
        ScriptJson extra = ScriptJson::object();   // unknown keys, kept by tools that rewrite a file (R1)
    };

    // One engine-level operation of the interpreter
    enum class ActionType : uint8_t {
        Wait,               // nothing; its frames (and live ms) pass
        WaitUntil,          // until the condition (string) is truthy, at most `count` frames
        Shot,               // a capture named `string`
        KeyDown, KeyUp,                     // code: key, mods
        Text,                               // systemTextInput(string)
        TextUnits,                          // systemCharInput for each UTF-16 unit of string
        Ime,                                // composition string, cursor in `count` (code points)
        ImeCommit,                          // string ("" cancels)
        ImeCancel,
        MouseEnter, MouseLeave,
        MouseMove,                          // to point
        MouseMoveBy,                        // value, value2: dx, dy in point.space units
        MouseDown, MouseUp,                 // code: button; at point, else where the pointer is
        MouseScroll,                        // value2, value: dx, dy; moves to point first when given
        DragStart, DragMove, DragEnd,       // point -> to; DragMove at fraction value
        TouchStart, TouchMove,              // device: pointer
        TouchEnd,                           // at point, else where the pointer is
        TouchCancel,
        SwipeStart, SwipeMove, SwipeEnd,    // as the drag, with a finger
        GamepadConnect,                     // device: gamepad, string: name
        GamepadDisconnect,
        GamepadButtonDown, GamepadButtonUp, // code: button
        GamepadAxis,                        // code: axis, value
        GamepadCursor,                      // the gamepad cursor warped to point (before the event's button or axis)
        ActionDown, ActionUp,               // the Input Manager's action `string` through the source
        Axis,                               // its axis `string` set to value
        AxisDelta,                          // value added this frame
        InputActionPress,                   // InputActions::inject(string, (value, value2), count windows)
        InputActionDown, InputActionUp,
        ReleaseAll,
        AppPause, AppResume, AppFocusLost, AppFocusGained, AppLowMemory, AppHide, AppShow,
        AppViewLost, AppViewRestored,       // the GPU view lost and loaded again (S95 REQ-18; host_.appViewLost)
        AppOrientation,                     // string: portrait, landscape_left...
        AppBack,                            // Back, as Android's Back button (Engine::systemBack)
    };

    JASPERENGINE_API const char* actionTypeName(ActionType type);

    struct ScriptPoint {
        bool given = false;
        Space space = Space::Canvas;
        double x = 0.0, y = 0.0;
        int32_t target = -1;            // a UI Document selector (ParsedScript::targets), resolved when it runs
        bool operator==(const ScriptPoint& other) const;
    };

    struct ScriptAction {
        uint32_t frame = 0;             // recordings: the update frame it is due on
        uint32_t framesAfter = 0;       // scripts: the frames to let pass before the next action
        uint32_t msAfter = 0;           // live send_input: wall-clock milliseconds (no fixed fps)
        uint32_t event = 0;             // the event it came from
        ActionType type = ActionType::Wait;
        int32_t code = 0;
        int32_t device = 0;             // touch pointer, gamepad
        int32_t mods = 0;
        int32_t count = 0;
        int32_t string = -1;            // ParsedScript::strings
        float value = 0.0f, value2 = 0.0f;
        ScriptPoint point, to;
        bool operator==(const ScriptAction& other) const;
    };

    struct ScriptTarget {
        std::string selector;
        ScriptJson document;            // the UI Document to search (null: every one, topmost first)
    };

    struct ParsedScript {
        ScriptHeader header;
        std::vector<ScriptAction> actions;
        std::vector<std::string> strings;
        std::vector<ScriptTarget> targets;
        std::vector<std::string> eventLabels;   // "touch tap" of each event (errors)
        size_t events = 0;
        // the last frame an action is due on (recordings), or the sum of the scripts' frames
        JASPERENGINE_API uint32_t lastFrame() const;
        JASPERENGINE_API const std::string& stringAt(int32_t index) const;
    };

    // How the events of a list are read
    struct EventParseOptions {
        Space space = Space::Canvas;    // of events that give none
        int gapFrames = 1;
        int fixedFps = 60;              // a file's ms -> frames
        bool recording = false;         // events carry `frame` (non-decreasing)
        bool live = false;              // a send_input call: `view` space, wall-clock ms, implicit gamepads
    };

    // false + error ("events[i]: ..." with the fix); `out` is left with no actions (R6)
    JASPERENGINE_API bool parseScript(std::string_view json, ParsedScript& out, std::string& error);
    JASPERENGINE_API bool parseScriptJson(const ScriptJson& root, ParsedScript& out, std::string& error);
    // the events of a file or a call; `errorEvent` gets the failing event's index, `error` its
    // message without the index
    JASPERENGINE_API bool parseEventList(const ScriptJson& events, const EventParseOptions& options, ParsedScript& out,
                                         std::string& error, size_t& errorEvent);
    // a script's events with a file's rules (R2, R3): "events[i]: ..." on error
    JASPERENGINE_API bool parseEvents(const ScriptJson& events, Space defaultSpace, int gapFrames, int fixedFps, ParsedScript& out,
                                      std::string& error);
    // A recording (or any list of single-step actions) as a file: one event per line inside
    // `events`, points with 4 decimals. False when an action is a composite's step (a drag's
    // move): only what a recording holds can be written.
    JASPERENGINE_API bool writeScript(const ParsedScript& script, std::string& out, std::string& error);

    // Input a run holds down, released at its end, at stop and on error. The editor shares one
    // across its send_input calls (a key held by one call is released by a later one).
    struct HeldInput {
        std::set<int> keys;
        std::set<int> mouseButtons;
        Vector2 mousePos = Vector2(0, 0);   // engine (view) pixels
        bool mousePosKnown = false;         // set by an injected event
        std::map<int, Vector2> touches;     // pointer -> engine pixels
        std::set<int> gamepads;             // connected by the run
        std::map<int, std::set<int>> gamepadButtons;
        std::map<int, std::set<int>> gamepadAxes;
        std::set<std::string> actions;      // Input Actions forced down (InputActions::inject)
        std::set<std::string> managerActions;   // the Input Manager's actions held through the source
        std::map<std::string, float> axes;
        JASPERENGINE_API bool empty() const;
    };

    // Releases what `held` holds (Engine::system*, the forced Input Actions, the Input Manager's
    // source of that name) and empties it
    JASPERENGINE_API void releaseHeldInput(HeldInput& held, const std::string& actionSource);

    // What differs between the hosts: the editor and a player
    struct PlayerHost {
        // a UI Document element's centre in canvas units (R8); null: "target needs the htmlui
        // module in this player". `found` describes the document (editor reports)
        std::function<bool(const std::string& selector, const ScriptJson& document, Vector2& canvasPoint, std::string& error,
                           ScriptJson* found)> resolveTarget;
        // the `wait until` condition, in the game's Lua state; null: the engine's Lua state
        std::function<bool(const std::string& condition, bool& truthy, std::string& error)> evalCondition;
        // a `shot` step; null: the shot is skipped
        std::function<bool(const std::string& name, uint64_t frame, std::string& error)> shot;
        // true while a shot is being taken: the player waits
        std::function<bool()> busy;
        // canvas units -> the coordinates of Engine::system*; null: the engine's view rect
        std::function<Vector2(Vector2 canvasPoint)> toEnginePoint;
        // an injected pointer event (the editor's Game view then leaves the pointer to the run), and
        // release_all / the end (it takes it back)
        std::function<void(bool injected)> pointerInjected;
        // app hide / show; null: Engine::systemVisibilityChanged
        std::function<std::string(bool hidden)> appHidden;
        // app view_lost / view_restored (S95 REQ-18): the running scenes' GPU objects released and
        // rebuilt; null: refused (the editor simulates the loss)
        std::function<std::string(bool lost)> appViewLost;
        // app orientation; null: refused (a simulated device turns, in the editor)
        std::function<std::string(const std::string& orientation)> appOrientation;
    };

    struct PlayOptions {
        bool simulateTouch = false;     // the left mouse button is a finger (Simulate Touch)
        int msFixedFps = 0;             // live ms: frames at this rate, 0 = wall clock
        std::string actionSource = "input";     // InputActions::namedSource of action / axis events
        std::shared_ptr<HeldInput> held;        // shared bookkeeping; null: the player's own
        bool releaseAtEnd = true;       // release what it holds when it ends, fails or is destroyed
        bool stopOnDivergence = false;  // a recording's checkpoint that differs fails the run
    };

    struct ResolvedTarget {
        size_t event = 0;
        std::string selector;
        Vector2 canvas;
        ScriptJson document;
    };

    // app back: who took it (Engine::getLastBackRoute: escape, key, document, game)
    struct BackTaken {
        size_t event = 0;
        std::string route;
    };

    struct ShotTaken {
        std::string name;
        uint64_t frame = 0;
    };

    // What a run did (R18, R41)
    struct ReplayReport {
        size_t actionsDone = 0;
        uint64_t frames = 0;                    // frames the run has seen
        bool done = false;
        bool failed = false;
        std::string error;                      // without the event prefix
        size_t failedEvent = 0;
        std::vector<ResolvedTarget> targets;
        std::vector<ShotTaken> shots;
        std::vector<BackTaken> backs;
        // checkpoints (recordings)
        bool comparable = false;
        int compared = 0;
        int64_t divergedAtFrame = -1;
        ScriptJson differences = ScriptJson::array();
    };

    class JASPERENGINE_API ScriptPlayer {
    public:
        ScriptPlayer(std::shared_ptr<const ParsedScript> script, PlayerHost host, PlayOptions options);
        ~ScriptPlayer();
        ScriptPlayer(const ScriptPlayer&) = delete;
        ScriptPlayer& operator=(const ScriptPlayer&) = delete;

        // Applies the actions due, before the input latch of a running frame. frameIndex counts
        // the frames since the run began (0 at the first call; the caller's count, which for a
        // paused app also counts the frames it drew, so a resume step still runs). False when the
        // run ended (done or failed).
        bool beforeFrame(uint64_t frameIndex);
        // after a frame's update: checkpoints (recordings)
        void afterUpdate(uint64_t frameIndex);
        // releases what the run holds (its end, a stop, an error)
        void releaseHeld();
        // stops the run where it is: what it pressed since it began is released (what was held
        // before stays); false when it had already ended
        bool cutShort();
        bool finished() const { return report_.done || report_.failed; }
        const ReplayReport& report() const { return report_; }
        ReplayReport& report() { return report_; }
        const ParsedScript& script() const { return *script_; }
        HeldInput& held() { return *held_; }
        // "Event 7 (touch tap): ..." of a failed run
        std::string errorText() const;

        // the interpreter's own Engine::system* calls are running (the real-input filter, R16)
        static bool isInjecting();

    private:
        bool runAction(const ScriptAction& action, std::string& error);
        // the point in the engine's coordinates (the events'), or in the canvas
        bool resolve(const ScriptAction& action, const ScriptPoint& point, Vector2& out, std::string& error, bool engineSpace = true);
        Vector2 currentPointer();
        void fail(const ScriptAction& action, const std::string& error);

        std::shared_ptr<const ParsedScript> script_;
        PlayerHost host_;
        PlayOptions options_;
        std::shared_ptr<HeldInput> held_;
        HeldInput heldBefore_;
        ReplayReport report_;
        size_t next_ = 0;
        uint64_t lastFrame_ = 0;
        bool orientationSkipped_ = false;   // a recording's turn without a simulated device: warned once
        bool started_ = false;
        uint64_t pendingFrames_ = 0;
        bool msPending_ = false;
        std::chrono::steady_clock::time_point msDeadline_;
        bool waitingUntil_ = false;
        uint64_t untilFrames_ = 0;
        size_t nextCheckpoint_ = 0;
        Vector2 pathFrom_, pathTo_;     // a drag's or a swipe's ends, resolved when it starts
    };

}

#endif
