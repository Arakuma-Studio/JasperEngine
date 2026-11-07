// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT


#ifndef Input_h
#define Input_h

//Based on GLFW modifier bits
#define D_MODIFIER_SHIFT          0x0001
#define D_MODIFIER_CONTROL        0x0002
#define D_MODIFIER_ALT            0x0004
#define D_MODIFIER_SUPER          0x0008
#define D_MODIFIER_CAPS_LOCK      0x0010
#define D_MODIFIER_NUM_LOCK       0x0020

//Based on GLFW key codes
#define D_KEY_UNKNOWN             -1
#define D_KEY_SPACE               32
#define D_KEY_APOSTROPHE          39  /* ' */
#define D_KEY_COMMA               44  /* , */
#define D_KEY_MINUS               45  /* - */
#define D_KEY_PERIOD              46  /* . */
#define D_KEY_SLASH               47  /* / */
#define D_KEY_0                   48
#define D_KEY_1                   49
#define D_KEY_2                   50
#define D_KEY_3                   51
#define D_KEY_4                   52
#define D_KEY_5                   53
#define D_KEY_6                   54
#define D_KEY_7                   55
#define D_KEY_8                   56
#define D_KEY_9                   57
#define D_KEY_SEMICOLON           59 /* ; */
#define D_KEY_EQUAL               61  /* = */
#define D_KEY_A                   65
#define D_KEY_B                   66
#define D_KEY_C                   67
#define D_KEY_D                   68
#define D_KEY_E                   69
#define D_KEY_F                   70
#define D_KEY_G                   71
#define D_KEY_H                   72
#define D_KEY_I                   73
#define D_KEY_J                   74
#define D_KEY_K                   75
#define D_KEY_L                   76
#define D_KEY_M                   77
#define D_KEY_N                   78
#define D_KEY_O                   79
#define D_KEY_P                   80
#define D_KEY_Q                   81
#define D_KEY_R                   82
#define D_KEY_S                   83
#define D_KEY_T                   84
#define D_KEY_U                   85
#define D_KEY_V                   86
#define D_KEY_W                   87
#define D_KEY_X                   88
#define D_KEY_Y                   89
#define D_KEY_Z                   90
#define D_KEY_LEFT_BRACKET        91 /* [ */
#define D_KEY_BACKSLASH           92  /* \ */
#define D_KEY_RIGHT_BRACKET       93  /* ] */
#define D_KEY_GRAVE_ACCENT        96 /* ` */
#define D_KEY_WORLD_1             161 /* non-US #1 */
#define D_KEY_WORLD_2             162 /* non-US #2 */
#define D_KEY_ESCAPE              256
#define D_KEY_ENTER               257
#define D_KEY_TAB                 258
#define D_KEY_BACKSPACE           259
#define D_KEY_INSERT              260
#define D_KEY_DELETE              261
#define D_KEY_RIGHT               262
#define D_KEY_LEFT                263
#define D_KEY_DOWN                264
#define D_KEY_UP                  265
#define D_KEY_PAGE_UP             266
#define D_KEY_PAGE_DOWN           267
#define D_KEY_HOME                268
#define D_KEY_END                 269
#define D_KEY_CAPS_LOCK           280
#define D_KEY_SCROLL_LOCK         281
#define D_KEY_NUM_LOCK            282
#define D_KEY_PRINT_SCREEN        283
#define D_KEY_PAUSE               284
#define D_KEY_F1                  290
#define D_KEY_F2                  291
#define D_KEY_F3                  292
#define D_KEY_F4                  293
#define D_KEY_F5                  294
#define D_KEY_F6                  295
#define D_KEY_F7                  296
#define D_KEY_F8                  297
#define D_KEY_F9                  298
#define D_KEY_F10                 299
#define D_KEY_F11                 300
#define D_KEY_F12                 301
#define D_KEY_F13                 302
#define D_KEY_F14                 303
#define D_KEY_F15                 304
#define D_KEY_F16                 305
#define D_KEY_F17                 306
#define D_KEY_F18                 307
#define D_KEY_F19                 308
#define D_KEY_F20                 309
#define D_KEY_F21                 310
#define D_KEY_F22                 311
#define D_KEY_F23                 312
#define D_KEY_F24                 313
#define D_KEY_F25                 314
#define D_KEY_KP_0                320
#define D_KEY_KP_1                321
#define D_KEY_KP_2                322
#define D_KEY_KP_3                323
#define D_KEY_KP_4                324
#define D_KEY_KP_5                325
#define D_KEY_KP_6                326
#define D_KEY_KP_7                327
#define D_KEY_KP_8                328
#define D_KEY_KP_9                329
#define D_KEY_KP_DECIMAL          330
#define D_KEY_KP_DIVIDE           331
#define D_KEY_KP_MULTIPLY         332
#define D_KEY_KP_SUBTRACT         333
#define D_KEY_KP_ADD              334
#define D_KEY_KP_ENTER            335
#define D_KEY_KP_EQUAL            336
#define D_KEY_LEFT_SHIFT          340
#define D_KEY_LEFT_CONTROL        341
#define D_KEY_LEFT_ALT            342
#define D_KEY_LEFT_SUPER          343
#define D_KEY_RIGHT_SHIFT         344
#define D_KEY_RIGHT_CONTROL       345
#define D_KEY_RIGHT_ALT           346
#define D_KEY_RIGHT_SUPER         347
#define D_KEY_MENU                348
#define D_KEY_LAST                D_KEY_MENU

//Mouse
#define D_MOUSE_BUTTON_1    0
#define D_MOUSE_BUTTON_2    1
#define D_MOUSE_BUTTON_3    2
#define D_MOUSE_BUTTON_4    3
#define D_MOUSE_BUTTON_5    4
#define D_MOUSE_BUTTON_6    5
#define D_MOUSE_BUTTON_7    6
#define D_MOUSE_BUTTON_8    7

#define D_MOUSE_BUTTON_LAST     D_MOUSE_BUTTON_8
#define D_MOUSE_BUTTON_LEFT     D_MOUSE_BUTTON_1
#define D_MOUSE_BUTTON_RIGHT    D_MOUSE_BUTTON_2
#define D_MOUSE_BUTTON_MIDDLE   D_MOUSE_BUTTON_3

//Based on GLFW gamepad buttons (standard Xbox-like layout)
#define D_GAMEPAD_BUTTON_A               0
#define D_GAMEPAD_BUTTON_B               1
#define D_GAMEPAD_BUTTON_X               2
#define D_GAMEPAD_BUTTON_Y               3
#define D_GAMEPAD_BUTTON_LEFT_BUMPER     4
#define D_GAMEPAD_BUTTON_RIGHT_BUMPER    5
#define D_GAMEPAD_BUTTON_BACK            6
#define D_GAMEPAD_BUTTON_START           7
#define D_GAMEPAD_BUTTON_GUIDE           8
#define D_GAMEPAD_BUTTON_LEFT_THUMB      9
#define D_GAMEPAD_BUTTON_RIGHT_THUMB     10
#define D_GAMEPAD_BUTTON_DPAD_UP         11
#define D_GAMEPAD_BUTTON_DPAD_RIGHT      12
#define D_GAMEPAD_BUTTON_DPAD_DOWN       13
#define D_GAMEPAD_BUTTON_DPAD_LEFT       14
#define D_GAMEPAD_BUTTON_LAST            D_GAMEPAD_BUTTON_DPAD_LEFT

#define D_GAMEPAD_BUTTON_CROSS           D_GAMEPAD_BUTTON_A
#define D_GAMEPAD_BUTTON_CIRCLE          D_GAMEPAD_BUTTON_B
#define D_GAMEPAD_BUTTON_SQUARE          D_GAMEPAD_BUTTON_X
#define D_GAMEPAD_BUTTON_TRIANGLE        D_GAMEPAD_BUTTON_Y

//Gamepad axes: sticks and triggers in [-1, 1] (triggers rest at -1)
#define D_GAMEPAD_AXIS_LEFT_X            0
#define D_GAMEPAD_AXIS_LEFT_Y            1
#define D_GAMEPAD_AXIS_RIGHT_X           2
#define D_GAMEPAD_AXIS_RIGHT_Y           3
#define D_GAMEPAD_AXIS_LEFT_TRIGGER     4
#define D_GAMEPAD_AXIS_RIGHT_TRIGGER    5
#define D_GAMEPAD_AXIS_LAST              D_GAMEPAD_AXIS_RIGHT_TRIGGER

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>
#include <string>
#include "Export.h"
#include "math/Vector2.h"
#include "System.h"

// Gamepads with their own edge and level state at once (more connect and are ignored)
#define D_INPUT_MAX_GAMEPADS             16
// Fingers down at once (more are ignored)
#define D_INPUT_MAX_TOUCHES              32

namespace jasperengine {

    // A finger's phase in the frame's window (Input::getTouch). The Unity spellings (Began...)
    // are the same values (Lua: Input.TOUCH_BEGAN or TouchPhase.Began)
    enum class TouchPhase : int {
        BEGAN = 0,      // touched the screen in the window (and is still down)
        MOVED,          // was down at the window's start and moved in it
        STATIONARY,     // was down at the window's start and did not move
        ENDED,          // lifted in the window
        CANCELED,       // interrupted in the window (systemTouchCancel): no release, no click
        Began = BEGAN,
        Moved = MOVED,
        Stationary = STATIONARY,
        Ended = ENDED,
        Canceled = CANCELED
    };

    // Whether the pointer is locked to the window (MouseMode::CAPTURED, Input::getPointerLockState)
    enum class PointerLockState : int {
        UNLOCKED = 0,   // not requested
        PENDING,        // requested, not active yet: the browser waits for a gesture, the
                        // window or the editor's Game view does not have the focus
        LOCKED          // active: the mouse delta is raw relative motion
    };

    // A finger (Unity's Touch). Positions are canvas pixels, y down. fingerId is pointer and
    // deltaPosition is delta (both spellings are kept: S22's and Unity's)
    typedef struct Touch{
        int pointer;
        Vector2 position;
        TouchPhase phase = TouchPhase::STATIONARY;
        Vector2 delta;              // canvas pixels moved in the window
        Vector2 startPosition;      // where it touched the screen
        float time = 0.0f;          // unscaled seconds since it touched the screen
        bool began = false;         // it touched the screen in the window (also when it ENDED in it)
        // Unity's names: the platform's finger id (an opaque small integer, stable while the
        // finger is down: Android pointer ids from 0, iOS slots from 1, browser identifiers),
        // the position change in the window, the unscaled seconds since the finger last moved
        // (at the window's latch), and 1, then 2, 3... for quick taps at the same place
        // (Input::multiTapTime / multiTapDistance)
        int fingerId = 0;
        Vector2 deltaPosition;
        float deltaTime = 0.0f;
        int tapCount = 1;
    } Touch;

    typedef struct Gamepad{
        int id;
        std::string name;
        std::map<int,bool> buttonPressed;
        std::map<int,float> axisValue;
    } Gamepad;

    // While typing or using a menu, the controls the UI takes do not drive input actions:
    // keyboard controls while a text field has the keyboard focus, the d-pad, left stick and A
    // while a document has the gamepad focus (Input::beginFrame passes it to InputActions; maps
    // that ignore what the UI consumes honour it)
    struct InputSuppression {
        bool keyboard = false;            // a text field has keyboard focus
        bool gamepadNavigation = false;   // a document has gamepad focus or a Button is selected (d-pad, left stick, A)
        bool keyboardNavigation = false;  // a Button is selected (arrows, Tab, Enter, keypad Enter, Space)
    };

    // The edges of one control in one window (Input::getControlEdges)
    struct InputControlEdges {
        bool level = false;         // held now
        bool uiHeld = false;        // the press that holds it went to the UI (a document or the component UI)
        uint16_t presses = 0;       // presses in the window
        uint16_t releases = 0;      // releases in the window
        uint16_t uiPresses = 0;     // of those presses, the ones the UI consumed
        int pressMods = 0;          // D_MODIFIER_* held at its last press (its event's and the modifier keys')
    };

    // Input state: levels, and edges per frame (docs/mcp/guide/input.md).
    //
    // Platform events (Engine::system*) add to a pending window between frames. A frame that
    // runs its update (not paused, the simulation not held) starts with beginFrame(), which
    // makes the pending window the frame's: getKeyDown/getKeyUp, the mouse and gamepad
    // button edges, the touch phases and the deltas read it until the next running frame. A
    // press and its release in one window are both seen (Down and Up true, the level false).
    // Inside the fixed steps (onFixedUpdate, Scene::fixedUpdate) the edges are those of a fixed
    // window instead: each beginFrame() adds to it and each fixed step that ran clears it, so
    // one fixed step sees an edge exactly once, also after frames that ran no fixed step.
    //
    // The device-state writers below are the platform and engine entry points: Engine::system*
    // calls them (and tests drive Input with them). The editor's Play input and the MCP tools
    // go through Engine::system*, never through these.
    class JASPERENGINE_API Input {

        friend class Engine;

    public:
        // controls by index, for InputActions and the editor's input views: keys
        // 0..D_KEY_LAST, then the mouse buttons, then each gamepad slot's buttons
        static constexpr int KEY_CONTROLS = D_KEY_LAST + 1;
        static constexpr int MOUSE_CONTROLS = D_MOUSE_BUTTON_LAST + 1;
        static constexpr int GAMEPAD_BUTTONS = D_GAMEPAD_BUTTON_LAST + 1;
        static constexpr int GAMEPAD_AXES = D_GAMEPAD_AXIS_LAST + 1;
        static constexpr int MOUSE_CONTROL_BASE = KEY_CONTROLS;
        static constexpr int GAMEPAD_CONTROL_BASE = MOUSE_CONTROL_BASE + MOUSE_CONTROLS;
        static constexpr int CONTROL_COUNT = GAMEPAD_CONTROL_BASE + D_INPUT_MAX_GAMEPADS * GAMEPAD_BUTTONS;

        // ---- device state writers (Engine::system*)
        static void addKeyPressed(int key, bool repeat = false, int mods = 0);
        static void releaseKeyPressed(int key);

        static void addMousePressed(int button, int mods = 0);
        static void releaseMousePressed(int button);
        static void setMousePosition(float x, float y);
        // a systemMouseMove: the position, and the delta from the previous move
        static void moveMouse(float x, float y);
        // the pointer put somewhere else (Engine::setMousePosition, a finger as the mouse): no delta
        static void warpMouse(float x, float y);
        // the same as moveMouse without the position: the pointer moved to (x, y) (outside the
        // canvas too, so a captured mouse looks around), the delta from the previous movement
        static void addMouseMovement(float x, float y);
        // a movement that is not the mouse's own (a finger simulating it): dx, dy as they are
        static void addMouseDelta(float dx, float dy);
        // the next movement starts the delta from where it is (the pointer came back elsewhere)
        static void resetMouseMovement();
        // a wheel event: the deprecated sticky value and the window's sum
        static void setMouseScroll(float xoffset, float yoffset);
        // the part of it the UI did not consume
        static void addMouseScrollForGame(float xoffset, float yoffset);

        static void addTouch(int pointer, float x, float y);
        static void setTouchPosition(int pointer, float x, float y);
        static void removeTouch(int pointer);
        static void cancelTouch(int pointer);
        static void clearTouches();

        static void addGamepad(int id, const std::string& name);
        static void removeGamepad(int id);
        static void addGamepadButtonPressed(int id, int button);
        static void releaseGamepadButtonPressed(int id, int button);
        static void setGamepadAxisValue(int id, int axis, float value);

        static void addMouseEntered();
        static void releaseMouseEntered();

        static void setModifiers(int mods);

        static void setPointerLockState(PointerLockState state);

        // text typed (code points), for getInputString (Unity's inputString): backspace is "\b",
        // return "\n"; other control characters are left out
        static void addInputCharacter(uint32_t codepoint);

        // Releases every held key, mouse button and gamepad button (their up edges are in the
        // next window) and reports the fingers down Canceled in the next window's touches: app
        // pause, focus loss, the editor's Play stop. The fingers stay in the fingers-down list
        // (numTouches, getTouches) until the platform's touch cancel or lift removes them, which
        // then adds no second Canceled
        static void releaseAll();
        // releaseAll and a clean slate (levels, windows, deltas, text, touches, taps, the input
        // clock): the editor's Play start and stop
        static void reset();

        // ---- Engine and editor only
        // the press just recorded went to the UI (a document or the component UI consumed it):
        // InputActions maps with ignoreWhenUIConsumes skip it
        static void setPressConsumedByUI(int control);
        static void setTouchConsumedByUI(int pointer);
        // the documents took the axis (the game sees its rest value)
        static void setGamepadAxisConsumedByUI(int id, int axis, bool consumed);
        // the pending window becomes the frame's (and is added to the fixed window)
        static void beginFrame();
        // the input frame of a frame that runs its update (Engine::systemDraw): the latch above,
        // the input clock advanced by the unscaled frame time (touch times, taps, smoothing),
        // then the input actions' evaluation (InputActions::evaluate, with the suppression of a
        // focused text field or gamepad navigation, Engine's when not given) and the actions'
        // Engine::onButtonDown / onButtonUp, before the fixed steps
        static void beginFrame(double unscaledDeltaTime);
        static void beginFrame(double unscaledDeltaTime, const InputSuppression& suppression);
        // the end of that frame (after the update): what was for this frame only goes (the
        // virtual sources' deltas, InputActions::endFrame); the windows stay until the next latch
        static void endFrame();
        // between beginFrame(dt) and endFrame()
        static bool isInFrame();
        // input frames latched so far (1 before the first)
        static uint64_t getInputFrame();
        // the input clock: unscaled seconds the input frames ran
        static double getInputTime();
        // around each fixed step: edge queries read the fixed window between them, which
        // endFixedStep() clears
        static void beginFixedStep();
        static void endFixedStep();
        // clears levels, windows, deltas and touches (the editor's Play start and stop)
        static void resetFrameState();
        // the deltas of the pending window (the window lost the focus)
        static void clearPendingDeltas();
        // inside a fixed step (beginFixedStep .. endFixedStep)
        static bool isFixedPhase();

        // ---- levels (as the device is now)
        static bool getKey(int key);
        static bool getMouseButton(int button);
        static bool isKeyPressed(int key);
        static bool isMousePressed(int button);
        static bool isTouch();
        static bool isMouseEntered();

        // ---- edges of the window (fixed window inside a fixed step)
        static bool getKeyDown(int key);
        static bool getKeyUp(int key);
        static bool getMouseButtonDown(int button);
        static bool getMouseButtonUp(int button);
        static bool getGamepadButtonDown(int id, int button);
        static bool getGamepadButtonUp(int id, int button);
        // a key, a mouse button or a gamepad button was pressed in the window
        static bool anyKeyDown();
        // Unity's anyKey / anyKeyDown: one is held now / anyKeyDown()
        static bool isAnyKeyPressed();
        static bool isAnyKeyDown();
        // the first control pressed in the window, by its control name (InputControlNames: keys,
        // "mouse_left"..., "gamepad_a"..., a gamepad axis past 0.5 as "left_x+" / "left_x-", a
        // trigger past half way as "left_trigger"), "" when none: "press a key" screens
        static std::string getPressedControl();
        // the UTF-8 text typed in the window (Unity's inputString: backspace "\b", return "\n")
        static std::string getInputString();

        static Vector2 getMousePosition();
        // canvas pixels the pointer moved in the frame's window (raw relative motion while
        // the pointer is locked), not counting a warp or the first move after entering
        static Vector2 getMouseDelta();
        // the wheel offsets of the frame's window
        static Vector2 getMouseScrollDelta();
        // Deprecated: the last wheel event's offsets, kept until the next (getMouseScrollDelta)
        static Vector2 getMouseScroll();
        // what of the window's wheel offsets the UI did not consume
        static Vector2 getMouseScrollDeltaForGame();
        static Vector2 getTouchPosition(int pointer);

        // the fingers down now
        static std::vector<Touch> getTouches();
        static size_t numTouches();
        // the frame's touches: the fingers down at the latch and the ones lifted or
        // cancelled in the window, each with its phase
        static size_t getTouchCount();
        // a Touch with pointer and fingerId -1 when out of range
        static Touch getTouch(size_t index);
        // the frame's touches as a list (Unity's Input.touches)
        static std::vector<Touch> getFrameTouches();
        // the frame's touch of that finger; false when it has none
        static bool findTouchById(int fingerId, Touch& out);
        // false: only the first finger down is in the frame's touches and the bindings (the
        // fingers-down list and the UI still get them all; Unity's multiTouchEnabled)
        static void setMultiTouchEnabled(bool enabled);
        static bool isMultiTouchEnabled();
        // a finger the frame's touches report (multiTouchEnabled), also one not down
        static bool isTouchVisible(int fingerId);
        // a touch that begins within multiTapTime seconds of the last one's end, and within
        // multiTapDistance canvas pixels of its start, counts on from its tapCount (the last
        // one must have lasted less than multiTapTime)
        static double multiTapTime;
        static float multiTapDistance;
        static bool getTouchDown(int pointer);
        static bool getTouchUp(int pointer);
        // the edges of a finger's presses in the window (InputActions' touch/primary)
        static InputControlEdges getTouchEdges(int pointer);

        static PointerLockState getPointerLockState();

        static bool isGamepadConnected(int id);
        static std::string getGamepadName(int id);
        static bool isGamepadButtonPressed(int id, int button);
        static float getGamepadAxis(int id, int axis);
        // the axis as the game sees it: its rest value while the UI took it
        static float getGamepadAxisForGame(int id, int axis);

        static std::vector<Gamepad> getGamepads();
        static size_t numGamepads();
        // Gamepad ids are sparse (they can have gaps after a disconnect), so
        // iterate connected pads by dense index: getGamepadId(0..numGamepads()-1).
        static int getGamepadId(size_t index);

        static int getModifiers();
        // D_MODIFIER_SHIFT/CONTROL/ALT/SUPER of the modifier keys held now
        static int getHeldModifierKeys();

        // Unity's Input.deviceOrientation: how the device is held (FaceUp/FaceDown on iOS),
        // Unknown where nothing reports it (desktops, flat Android devices)
        static DeviceOrientation getDeviceOrientation();

        static size_t findTouchIndex(int pointer);
        static size_t findGamepadIndex(int id);

        // ---- controls by index (CONTROL_COUNT), for InputActions and the editor
        static int keyControl(int key);                   // -1 outside 0..D_KEY_LAST
        static int mouseControl(int button);              // -1 outside 0..D_MOUSE_BUTTON_LAST
        // a connected (or just disconnected) pad's slot, -1 for none
        static int getGamepadSlot(int id);
        static int getGamepadSlotId(int slot);            // -1 for a free slot
        static bool isGamepadSlotConnected(int slot);
        static int gamepadControl(int slot, int button);  // -1 outside the slots or buttons
        static float getGamepadSlotAxis(int slot, int axis, bool forGame);
        // the window's edges of a control (the fixed window inside a fixed step)
        static InputControlEdges getControlEdges(int control);
        static InputControlEdges getControlEdges(int control, bool fixedWindow);
    };
}

#endif /* Input_h */
