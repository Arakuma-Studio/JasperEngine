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

#include <cstdint>
#include <string>
#include <vector>
#include "Export.h"
#include "math/Vector2.h"

namespace jasperengine {

    // Unity's TouchPhase
    enum class TouchPhase {
        Began,
        Moved,
        Stationary,
        Ended,
        Canceled
    };

    // A finger (Unity's Touch). Input.touches lists them with Unity's semantics (a touch that
    // ended or was cancelled is listed in the frame it ends); numTouches/getTouches list the
    // fingers down now. Positions are canvas px, y down.
    struct Touch {
        // the platform's finger id: an opaque small integer, stable while the finger is down
        // (Android pointer ids from 0, iOS slots from 1, browser identifiers)
        int fingerId = 0;
        TouchPhase phase = TouchPhase::Began;
        Vector2 position;
        Vector2 startPosition;
        // position change in this frame
        Vector2 deltaPosition;
        // unscaled seconds since the touch's last position change
        float deltaTime = 0.0f;
        // 1, then 2, 3... for quick taps at the same place (Input::multiTapTime/Distance)
        int tapCount = 1;

        // the old Touch's name of fingerId
        int pointer() const { return fingerId; }
    };

    // A button's state: held now, and the input frames of its last press and release (the
    // frame that sees them, Input::getInputFrame)
    struct ControlState {
        bool held = false;
        uint64_t downFrame = 0;
        uint64_t upFrame = 0;
    };

    struct Gamepad {
        int id = 0;
        std::string name;
        ControlState buttons[D_GAMEPAD_BUTTON_LAST + 1];
        float axes[D_GAMEPAD_AXIS_LAST + 1] = {0.0f, 0.0f, 0.0f, 0.0f, -1.0f, -1.0f};
    };

    // P7 of the input spec: while typing or using a menu, the controls a document takes do not
    // drive input actions (Input::beginFrame passes it to InputActions::update)
    struct InputSuppression {
        bool keyboard = false;            // a text field has keyboard focus
        bool gamepadNavigation = false;   // a document has gamepad focus (d-pad, left stick, A)
    };

    class JASPERENGINE_API Input {

        friend class Engine;

    public:
        // ---- device state writers: the Engine's system* entry points call them (scripts send
        // input through Engine::system*). Presses and releases are stamped with the input frame
        // that will see them.
        static void addKeyPressed(int key, bool repeat = false);
        static void releaseKeyPressed(int key);

        static void addMousePressed(int button);
        static void releaseMousePressed(int button);
        static void setMousePosition(float x, float y);
        static void setMouseScroll(float xoffset, float yoffset);
        // the pointer moved to (x, y), canvas px of the raw platform position (outside the canvas
        // too, so a captured mouse looks around): adds to this frame's mouse delta
        static void addMouseMovement(float x, float y);
        // a movement that is not the mouse's own (a finger simulating it): adds dx, dy as is
        static void addMouseDelta(float dx, float dy);
        // forgets the last pointer position: the next movement starts the delta from there
        static void resetMouseMovement();

        static void addTouch(int pointer, float x, float y);
        static void setTouchPosition(int pointer, float x, float y);
        // the finger lifted (Ended)
        static void removeTouch(int pointer);
        // the finger was interrupted (Canceled)
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

        // text typed (code points), for inputString
        static void addInputCharacter(uint32_t codepoint);

        // ---- the input frame (Engine::runFrame, only on frames that run the update):
        // beginFrame publishes this frame's deltas, touches and text, updates the input actions
        // (InputActions::update) and fires Engine::onButtonDown/onButtonUp; endFrame drops what
        // was for this frame only and moves to the next input frame
        static void beginFrame(double unscaledDeltaTime);
        static void beginFrame(double unscaledDeltaTime, const InputSuppression& suppression);
        static void endFrame();
        static uint64_t getInputFrame();
        static bool isInFrame();
        // input clock: unscaled seconds the input frames ran (touch times)
        static double getInputTime();

        // Releases every held key, mouse button and gamepad button (their up edges are seen by
        // the next frame) and reports the touches Canceled in Input.touches: app pause, focus
        // loss, editor Play stop. The fingers down now (numTouches) are left to the platform's
        // touch cancel (Engine::systemTouchCancel), which the UI and onTouchCancel hear
        static void releaseAll();
        // releaseAll and a clean slate (frames, deltas, text, touches, taps): editor Play start
        static void reset();

        // ---- held state (the device, whatever the UI consumed)
        static bool isKeyPressed(int key);
        static bool isMousePressed(int button);
        static bool isTouch();
        static bool isMouseEntered();

        // ---- edges: pressed / released in this frame (Unity's GetKeyDown / GetKeyUp)
        static bool getKeyDown(int key);
        static bool getKeyUp(int key);
        static bool getMouseButtonDown(int button);
        static bool getMouseButtonUp(int button);
        // id -1: any gamepad
        static bool getGamepadButtonDown(int id, int button);
        static bool getGamepadButtonUp(int id, int button);
        // any key, mouse button or gamepad button held / pressed in this frame
        static bool isAnyKeyPressed();
        static bool isAnyKeyDown();
        // the first control pressed in this frame, by its control name (keys, mouse buttons,
        // gamepad buttons, gamepad axes past 0.5 as "left_x+" / "left_x-", triggers as
        // "left_trigger"), "" when none: "press a key" screens
        static std::string getPressedControl();
        // UTF-8 text typed in this frame (Unity's inputString: backspace is "\b", return "\n")
        static std::string getInputString();
        // mouse movement in this frame, canvas px, y down
        static Vector2 getMouseDelta();
        // wheel steps in this frame (y positive up, x positive right)
        static Vector2 getMouseScrollDelta();

        static Vector2 getMousePosition();
        // the last wheel event's offsets
        static Vector2 getMouseScroll();
        static Vector2 getTouchPosition(int pointer);

        // the fingers down now (legacy list)
        static std::vector<Touch> getTouches();
        static size_t numTouches();

        // ---- touches with Unity's semantics (published by beginFrame)
        static int getTouchCount();
        // 0-based; a Touch with fingerId -1 when out of range
        static Touch getTouch(int index);
        static const std::vector<Touch>& getFrameTouches();
        static bool findTouchById(int fingerId, Touch& out);
        // false: only the first finger is in the touches above (the legacy list and the UI still get them all)
        static void setMultiTouchEnabled(bool enabled);
        static bool isMultiTouchEnabled();
        // a finger the touches above report (multiTouchEnabled)
        static bool isTouchVisible(int fingerId);
        // a touch that begins within multiTapTime of the last one's end, near its start, adds a tap
        static double multiTapTime;
        static float multiTapDistance;

        static bool isGamepadConnected(int id);
        static std::string getGamepadName(int id);
        static bool isGamepadButtonPressed(int id, int button);
        static float getGamepadAxis(int id, int axis);

        static std::vector<Gamepad> getGamepads();
        static size_t numGamepads();
        // Gamepad ids are sparse (they can have gaps after a disconnect), so
        // iterate connected pads by dense index: getGamepadId(0..numGamepads()-1).
        static int getGamepadId(size_t index);

        static int getModifiers();

        static size_t findTouchIndex(int pointer);
        static size_t findGamepadIndex(int id);

    private:

        static ControlState keyState[D_KEY_LAST + 1];
        static ControlState mouseState[D_MOUSE_BUTTON_LAST + 1];
        static Vector2 mousePosition;
        static Vector2 mouseScroll;
        static std::vector<Touch> touches;
        static std::vector<Gamepad> gamepads;
        static bool mousedEntered;
        static int modifiers;

        static uint64_t stampFrame();
        static void notePressedControl(const std::string& name);
        static void endTouch(int pointer, TouchPhase phase);
    };
}

#endif /* Input_h */
