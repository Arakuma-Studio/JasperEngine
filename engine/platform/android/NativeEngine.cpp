// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// ---------------------------------------
// Based on samples: https://github.com/android/games-samples
// ---------------------------------------

#include "NativeEngine.h"
#include "AndroidTextInput.h"
#include "HttpBackendAndroid.h"
#include "MediaCodecBackend.h"
#include "NativePlatformAndroid.h"

#include <game-activity/native_app_glue/android_native_app_glue.h>
#include <android/log.h>
#include <android/window.h>
#include <linux/input-event-codes.h>
#include <swappy/swappyGL.h>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstddef>
#include <memory>
#include <cstring>
#include <string>
#include <vector>
#include <assert.h>
#include <math.h>

#include "JasperEngineAndroid.h"
#include "Engine.h"
#include "System.h"
#include "Input.h"
#include "Screen.h"
#include "Log.h"
#include "render/GpuWorkerContext.h"
#include "util/StringUtils.h"
#include "../common/PlatformDefaults.h"
#include "player/PlayerLaunch.h"
#include "device/Device.h"

#include <sys/system_properties.h>
#include <cstdlib>
#include <sstream>

#define MAX_GL_ERRORS 200

namespace {

// These logical keycodes postdate API 33. Keep their stable platform values so older
// NDK headers can still recognize them on newer Android releases.
constexpr int32_t ANDROID_KEYCODE_F13 = 326;
constexpr int32_t ANDROID_KEYCODE_F24 = 337;

constexpr int getJasperEngineKey(int32_t key, int32_t scanCode) {
    if (key == AKEYCODE_SPACE)
        return D_KEY_SPACE;
    if (key == AKEYCODE_APOSTROPHE)
        return D_KEY_APOSTROPHE;
    if (key == AKEYCODE_COMMA)
        return D_KEY_COMMA;
    if (key == AKEYCODE_MINUS)
        return D_KEY_MINUS;
    if (key == AKEYCODE_PERIOD)
        return D_KEY_PERIOD;
    if (key == AKEYCODE_SLASH)
        return D_KEY_SLASH;

    if (key >= AKEYCODE_0 && key <= AKEYCODE_9)
        return key + D_KEY_0 - AKEYCODE_0;

    if (key == AKEYCODE_SEMICOLON)
        return D_KEY_SEMICOLON;
    if (key == AKEYCODE_EQUALS)
        return D_KEY_EQUAL;

    if (key >= AKEYCODE_A && key <= AKEYCODE_Z)
        return key + D_KEY_A - AKEYCODE_A;

    if (key == AKEYCODE_LEFT_BRACKET)
        return D_KEY_LEFT_BRACKET;
    if (key == AKEYCODE_BACKSLASH)
        return D_KEY_BACKSLASH;
    if (key == AKEYCODE_RIGHT_BRACKET)
        return D_KEY_RIGHT_BRACKET;
    if (key == AKEYCODE_GRAVE)
        return D_KEY_GRAVE_ACCENT;

    if (key == AKEYCODE_ESCAPE)
        return D_KEY_ESCAPE;
    if (key == AKEYCODE_ENTER)
        return D_KEY_ENTER;
    if (key == AKEYCODE_TAB)
        return D_KEY_TAB;
    if (key == AKEYCODE_DEL)
        return D_KEY_BACKSPACE;
    if (key == AKEYCODE_INSERT)
        return D_KEY_INSERT;
    if (key == AKEYCODE_FORWARD_DEL)
        return D_KEY_DELETE;
    if (key == AKEYCODE_DPAD_RIGHT)
        return D_KEY_RIGHT;
    if (key == AKEYCODE_DPAD_LEFT)
        return D_KEY_LEFT;
    if (key == AKEYCODE_DPAD_DOWN)
        return D_KEY_DOWN;
    if (key == AKEYCODE_DPAD_UP)
        return D_KEY_UP;
    // a TV remote's centre (select) key: Enter, which submits a selected Button
    if (key == AKEYCODE_DPAD_CENTER)
        return D_KEY_ENTER;
    if (key == AKEYCODE_PAGE_UP)
        return D_KEY_PAGE_UP;
    if (key == AKEYCODE_PAGE_DOWN)
        return D_KEY_PAGE_DOWN;
    if (key == AKEYCODE_MOVE_HOME)
        return D_KEY_HOME;
    if (key == AKEYCODE_MOVE_END)
        return D_KEY_END;
    if (key == AKEYCODE_CAPS_LOCK)
        return D_KEY_CAPS_LOCK;
    if (key == AKEYCODE_SCROLL_LOCK)
        return D_KEY_SCROLL_LOCK;
    if (key == AKEYCODE_NUM_LOCK)
        return D_KEY_NUM_LOCK;
    if (key == AKEYCODE_SYSRQ)
        return D_KEY_PRINT_SCREEN;
    if (key == AKEYCODE_BREAK)
        return D_KEY_PAUSE;

    if (key >= AKEYCODE_F1 && key <= AKEYCODE_F12)
        return key + D_KEY_F1 - AKEYCODE_F1;

    if (key >= ANDROID_KEYCODE_F13 && key <= ANDROID_KEYCODE_F24)
        return key + D_KEY_F13 - ANDROID_KEYCODE_F13;

    if (key >= AKEYCODE_NUMPAD_0 && key <= AKEYCODE_NUMPAD_9)
        return key + D_KEY_KP_0 - AKEYCODE_NUMPAD_0;
    if (key == AKEYCODE_NUMPAD_DOT)
        return D_KEY_KP_DECIMAL;
    if (key == AKEYCODE_NUMPAD_COMMA)
        return D_KEY_KP_DECIMAL;
    if (key == AKEYCODE_NUMPAD_DIVIDE)
        return D_KEY_KP_DIVIDE;
    if (key == AKEYCODE_NUMPAD_MULTIPLY)
        return D_KEY_KP_MULTIPLY;
    if (key == AKEYCODE_NUMPAD_SUBTRACT)
        return D_KEY_KP_SUBTRACT;
    if (key == AKEYCODE_NUMPAD_ADD)
        return D_KEY_KP_ADD;
    if (key == AKEYCODE_NUMPAD_ENTER)
        return D_KEY_KP_ENTER;
    if (key == AKEYCODE_NUMPAD_EQUALS)
        return D_KEY_KP_EQUAL;

    if (key == AKEYCODE_SHIFT_LEFT)
        return D_KEY_LEFT_SHIFT;
    if (key == AKEYCODE_CTRL_LEFT)
        return D_KEY_LEFT_CONTROL;
    if (key == AKEYCODE_ALT_LEFT)
        return D_KEY_LEFT_ALT;
    if (key == AKEYCODE_META_LEFT)
        return D_KEY_LEFT_SUPER;
    if (key == AKEYCODE_SHIFT_RIGHT)
        return D_KEY_RIGHT_SHIFT;
    if (key == AKEYCODE_CTRL_RIGHT)
        return D_KEY_RIGHT_CONTROL;
    if (key == AKEYCODE_ALT_RIGHT)
        return D_KEY_RIGHT_ALT;
    if (key == AKEYCODE_META_RIGHT)
        return D_KEY_RIGHT_SUPER;
    if (key == AKEYCODE_MENU)
        return D_KEY_MENU;

    // Last resort: before Android 16 there are no logical keycodes for F13..F24,
    // so physical keyboards only expose them through their Linux evdev scan codes.
    // There is no standard Android or evdev representation for the engine's F25.
    if (scanCode >= KEY_F13 && scanCode <= KEY_F24)
        return scanCode + D_KEY_F13 - KEY_F13;

    return D_KEY_UNKNOWN;
}

constexpr int getJasperEngineModifiers(int32_t mods) {
    int modifiers = 0;
    if (mods & AMETA_CTRL_ON) modifiers |= D_MODIFIER_CONTROL;
    if (mods & AMETA_SHIFT_ON) modifiers |= D_MODIFIER_SHIFT;
    if (mods & AMETA_ALT_ON) modifiers |= D_MODIFIER_ALT;
    if (mods & AMETA_META_ON) modifiers |= D_MODIFIER_SUPER;
    if (mods & AMETA_CAPS_LOCK_ON) modifiers |= D_MODIFIER_CAPS_LOCK;
    if (mods & AMETA_NUM_LOCK_ON) modifiers |= D_MODIFIER_NUM_LOCK;

    return modifiers;
}

// a pointer's pen pressure (Input::setPenPressure): a stylus's or an eraser's, -1 for a finger or
// a mouse
float stylusPressureOf(const GameActivityPointerAxes& pointer) {
    const int32_t tool = GameActivityPointerAxes_getToolType(&pointer);
    if (tool != AMOTION_EVENT_TOOL_TYPE_STYLUS && tool != AMOTION_EVENT_TOOL_TYPE_ERASER)
        return -1.0f;
    return std::clamp(GameActivityPointerAxes_getPressure(&pointer), 0.0f, 1.0f);
}

constexpr int getJasperEngineGamepadButton(int32_t key) {
    if (key == AKEYCODE_BUTTON_A)
        return D_GAMEPAD_BUTTON_A;
    if (key == AKEYCODE_BUTTON_B)
        return D_GAMEPAD_BUTTON_B;
    if (key == AKEYCODE_BUTTON_X)
        return D_GAMEPAD_BUTTON_X;
    if (key == AKEYCODE_BUTTON_Y)
        return D_GAMEPAD_BUTTON_Y;
    if (key == AKEYCODE_BUTTON_L1)
        return D_GAMEPAD_BUTTON_LEFT_BUMPER;
    if (key == AKEYCODE_BUTTON_R1)
        return D_GAMEPAD_BUTTON_RIGHT_BUMPER;
    if (key == AKEYCODE_BUTTON_SELECT)
        return D_GAMEPAD_BUTTON_BACK;
    if (key == AKEYCODE_BUTTON_START)
        return D_GAMEPAD_BUTTON_START;
    if (key == AKEYCODE_BUTTON_MODE)
        return D_GAMEPAD_BUTTON_GUIDE;
    if (key == AKEYCODE_BUTTON_THUMBL)
        return D_GAMEPAD_BUTTON_LEFT_THUMB;
    if (key == AKEYCODE_BUTTON_THUMBR)
        return D_GAMEPAD_BUTTON_RIGHT_THUMB;
    if (key == AKEYCODE_DPAD_UP)
        return D_GAMEPAD_BUTTON_DPAD_UP;
    if (key == AKEYCODE_DPAD_RIGHT)
        return D_GAMEPAD_BUTTON_DPAD_RIGHT;
    if (key == AKEYCODE_DPAD_DOWN)
        return D_GAMEPAD_BUTTON_DPAD_DOWN;
    if (key == AKEYCODE_DPAD_LEFT)
        return D_GAMEPAD_BUTTON_DPAD_LEFT;

    return -1;
}

static_assert(getJasperEngineKey(AKEYCODE_0, 0) == D_KEY_0, "Android digit key mapping is broken");
static_assert(getJasperEngineKey(AKEYCODE_9, 0) == D_KEY_9, "Android digit key range is broken");
static_assert(getJasperEngineKey(AKEYCODE_A, 0) == D_KEY_A, "Android letter key mapping is broken");
static_assert(getJasperEngineKey(AKEYCODE_Z, 0) == D_KEY_Z, "Android letter key range is broken");
static_assert(getJasperEngineKey(AKEYCODE_F1, 0) == D_KEY_F1, "Android function key mapping is broken");
static_assert(getJasperEngineKey(AKEYCODE_F12, 0) == D_KEY_F12, "Android function key range is broken");
static_assert(getJasperEngineKey(ANDROID_KEYCODE_F13, 0) == D_KEY_F13, "Android F13 key mapping is broken");
static_assert(getJasperEngineKey(ANDROID_KEYCODE_F24, 0) == D_KEY_F24, "Android F24 key mapping is broken");
static_assert(getJasperEngineKey(AKEYCODE_UNKNOWN, KEY_F13) == D_KEY_F13, "Android F13 scan-code mapping is broken");
static_assert(getJasperEngineKey(AKEYCODE_UNKNOWN, KEY_F24) == D_KEY_F24, "Android F24 scan-code mapping is broken");
static_assert(getJasperEngineKey(AKEYCODE_NUMPAD_0, 0) == D_KEY_KP_0, "Android numpad mapping is broken");
static_assert(getJasperEngineKey(AKEYCODE_NUMPAD_9, 0) == D_KEY_KP_9, "Android numpad key range is broken");
static_assert(getJasperEngineKey(AKEYCODE_NUMPAD_COMMA, 0) == D_KEY_KP_DECIMAL, "Android localized decimal mapping is broken");
static_assert(getJasperEngineKey(AKEYCODE_BACK, 0) == D_KEY_UNKNOWN, "Android Back must not masquerade as a keyboard key");
static_assert(getJasperEngineKey(AKEYCODE_DPAD_CENTER, 0) == D_KEY_ENTER, "the TV remote's centre key must submit as Enter");
static_assert(getJasperEngineKey(AKEYCODE_UNKNOWN, 0) == D_KEY_UNKNOWN, "Unknown Android keys must stay unknown");
static_assert(getJasperEngineModifiers(0) == 0, "Empty Android modifiers must stay empty");
static_assert(getJasperEngineModifiers(AMETA_CTRL_ON | AMETA_SHIFT_ON | AMETA_ALT_ON | AMETA_META_ON |
                                AMETA_CAPS_LOCK_ON | AMETA_NUM_LOCK_ON) ==
              (D_MODIFIER_CONTROL | D_MODIFIER_SHIFT | D_MODIFIER_ALT | D_MODIFIER_SUPER |
               D_MODIFIER_CAPS_LOCK | D_MODIFIER_NUM_LOCK),
              "Android modifier mapping is broken");
static_assert(getJasperEngineGamepadButton(AKEYCODE_BUTTON_A) == D_GAMEPAD_BUTTON_A,
              "Android gamepad face-button mapping is broken");
static_assert(getJasperEngineGamepadButton(AKEYCODE_BUTTON_THUMBR) == D_GAMEPAD_BUTTON_RIGHT_THUMB,
              "Android gamepad thumb-button mapping is broken");
static_assert(getJasperEngineGamepadButton(AKEYCODE_DPAD_LEFT) == D_GAMEPAD_BUTTON_DPAD_LEFT,
              "Android gamepad D-pad mapping is broken");
static_assert(getJasperEngineGamepadButton(AKEYCODE_UNKNOWN) == -1,
              "Unknown Android gamepad buttons must stay unknown");

} // namespace

static bool all_motion_filter(const GameActivityMotionEvent* event) {
  // Process all motion events
  return true;
}

// Keys the engine has no use for go back to the system, so volume, media and
// other device keys keep working. Back reaches MainActivity's back callback
// (delivered as Escape), except the IME backspaces some keyboards send as Back.
// (the default filter would also drop gamepad buttons whose source lacks the
// keyboard class)
static bool key_filter(const GameActivityKeyEvent* event) {
  if (event->keyCode == AKEYCODE_BACK)
    return (event->flags & AKEY_EVENT_FLAG_SOFT_KEYBOARD) != 0;
  return getJasperEngineKey(event->keyCode, event->scanCode) != D_KEY_UNKNOWN ||
         getJasperEngineGamepadButton(event->keyCode) >= 0;
}

static void _handle_cmd_proxy(struct android_app *app, int32_t cmd) {
    NativeEngine *engine = (NativeEngine *) app->userData;
    engine->handleCommand(cmd);
}

static void _log_opengl_error(GLenum err) {
    switch (err) {
        case GL_INVALID_ENUM:
            jasperengine::Log::error("*** OpenGL error: GL_INVALID_ENUM");
            break;
        case GL_INVALID_VALUE:
            jasperengine::Log::error("*** OpenGL error: GL_INVALID_VALUE");
            break;
        case GL_INVALID_OPERATION:
            jasperengine::Log::error("*** OpenGL error: GL_INVALID_OPERATION");
            break;
        case GL_INVALID_FRAMEBUFFER_OPERATION:
            jasperengine::Log::error("*** OpenGL error: GL_INVALID_FRAMEBUFFER_OPERATION");
            break;
        case GL_OUT_OF_MEMORY:
            jasperengine::Log::error("*** OpenGL error: GL_OUT_OF_MEMORY");
            break;
        default:
            jasperengine::Log::error("*** OpenGL error: error %d", err);
            break;
    }
}

static NativeEngine *_singleton = NULL;

// workaround for internal bug b/149866792
static NativeEngineSavedState appState = {false};

using jasperengine::android::codepointsToModifiedUtf8;
using jasperengine::android::codepointsToUtf8;
using jasperengine::android::decodeModifiedUtf8;
using jasperengine::android::utf16ToCodepoints;
using jasperengine::android::wideToCodepoints;

// the component InputField's side of the soft keyboard's text
static jasperengine::android::ComponentTextInput componentTextInput;

// Set on the UI thread by MainActivity's back callback; Android 16 no longer sends Back as a key
static std::atomic<bool> backPressed(false);
// the Escape press a Back made, released on the next frame
static bool backEscapeDown = false;

extern "C" JNIEXPORT void JNICALL
Java_com_jaspergameengine_jasperengine_MainActivity_onBackPressedNative(JNIEnv* env, jobject obj) {
    backPressed = true;
}

// HTML UI document text fields (Engine::wantsTextComposition), in code points: imeText is the
// field's text as the engine has it, composition included; imeCaret is where the engine's
// caret is taken to be when nothing is composed; [imeCompStart, imeCompEnd) is the engine's
// composition (empty when none) and imeCompCursor its cursor.
static std::u32string imeText;
static size_t imeCaret = 0;
static size_t imeCompStart = 0;
static size_t imeCompEnd = 0;
static int imeCompCursor = -1;

// The document diff starts over from this text, with the caret at caret and nothing composed
static void resetDocumentTextInput(const std::u32string& text, size_t caret) {
    imeText = text;
    imeCaret = std::min(caret, text.size());
    imeCompStart = imeCompEnd = 0;
    imeCompCursor = -1;
}

static void sendTextInputKey(int key) {
    jasperengine::Engine::systemKeyDown(key, false, 0);
    jasperengine::Engine::systemKeyUp(key, false, 0);
}

// Edits the IME makes away from the caret (a suggestion replacing a word) first move the
// engine's caret there, one code point per arrow key
static void moveTextInputCaret(size_t target) {
    while (imeCaret < target) {
        sendTextInputKey(D_KEY_RIGHT);
        imeCaret++;
    }
    while (imeCaret > target) {
        sendTextInputKey(D_KEY_LEFT);
        imeCaret--;
    }
}

// The GameTextInput state, for a focused HTML UI document text field. Text outside the
// composing region is diffed against what the engine has: removals become backspaces,
// additions systemTextInput. The composing region is the engine's IME composition; when it
// ends, the text that replaced it is committed (a text input while composing confirms the
// composition), or the composition is cancelled when nothing replaced it. A region the IME
// puts over text the engine already has (Gboard re-selecting a word) is not a composition
// for the engine: later edits in it are plain text edits.
static void updateDocumentTextInput(const std::u16string& units, const GameTextInputState* state) {
    std::u32string text;
    std::vector<size_t> unitToCodepoint;
    utf16ToCodepoints(units, text, unitToCodepoint);

    const int32_t unitCount = (int32_t)units.size();
    auto codepointAt = [&](int32_t unit) {
        return unitToCodepoint[std::max(0, std::min(unit, unitCount))];
    };

    // the composing region in code points, none when undefined (-1) or empty
    size_t compStart = 0;
    size_t compEnd = 0;
    int32_t regionStart = state->composingRegion.start;
    int32_t regionEnd = state->composingRegion.end;
    if (regionStart >= 0 && regionEnd >= 0 && regionStart != regionEnd) {
        compStart = codepointAt(std::min(regionStart, regionEnd));
        compEnd = codepointAt(std::max(regionStart, regionEnd));
    }
    const bool hasComp = compEnd > compStart;

    // the IME's caret: the end of its selection
    const int32_t selectionStart = state->selection.start;
    const int32_t selectionEnd = state->selection.end;
    const size_t caret = (selectionEnd >= 0) ? codepointAt(selectionEnd) : text.size();
    const bool caretCollapsed = selectionEnd >= 0 && selectionStart == selectionEnd;
    const int compCursor = hasComp ? (int)(std::max(compStart, std::min(caret, compEnd)) - compStart) : -1;

    if (imeCompEnd > imeCompStart) {
        // the engine is composing: is the text around its composition unchanged?
        const size_t tail = imeText.size() - imeCompEnd;
        const bool anchored = text.size() >= imeCompStart + tail &&
                              text.compare(0, imeCompStart, imeText, 0, imeCompStart) == 0 &&
                              text.compare(text.size() - tail, tail, imeText, imeCompEnd, tail) == 0;
        const size_t replacedEnd = text.size() - tail;

        if (anchored && hasComp && compStart == imeCompStart && compEnd == replacedEnd) {
            // the same composition, updated
            if (text.compare(compStart, compEnd - compStart, imeText, imeCompStart, imeCompEnd - imeCompStart) != 0 ||
                    compCursor != imeCompCursor) {
                jasperengine::Engine::systemImeComposition(codepointsToUtf8(text, compStart, compEnd), compCursor);
            }
            imeText = text;
            imeCompEnd = compEnd;
            imeCompCursor = compCursor;
            return;
        }

        if (anchored) {
            // the composition ended: what replaced it is committed, except a new composition
            // at its end, which follows the commit
            const bool nextComp = hasComp && compStart >= imeCompStart && compEnd == replacedEnd;
            const size_t commitEnd = nextComp ? compStart : replacedEnd;

            if (commitEnd > imeCompStart) {
                jasperengine::Engine::systemTextInput(codepointsToUtf8(text, imeCompStart, commitEnd));
            } else {
                jasperengine::Engine::systemImeCancel();
            }
            imeText = text;
            imeCaret = commitEnd;
            imeCompStart = imeCompEnd = 0;
            imeCompCursor = -1;

            if (nextComp) {
                jasperengine::Engine::systemImeComposition(codepointsToUtf8(text, compStart, compEnd), compCursor);
                imeCompStart = compStart;
                imeCompEnd = compEnd;
                imeCompCursor = compCursor;
                return;
            }
        } else {
            // text outside the composition changed too: drop the composition, then diff the rest
            jasperengine::Engine::systemImeCancel();
            imeText.erase(imeCompStart, imeCompEnd - imeCompStart);
            imeCaret = imeCompStart;
            imeCompStart = imeCompEnd = 0;
            imeCompCursor = -1;
        }
    }

    // one edit, taken as close to the caret as the text allows: the common prefix stops at
    // the caret, backspaces remove before it
    const size_t common = std::min(imeText.size(), text.size());
    size_t prefix = 0;
    while (prefix < std::min(common, imeCaret) && imeText[prefix] == text[prefix])
        prefix++;
    size_t suffix = 0;
    while (suffix < common - prefix && imeText[imeText.size() - 1 - suffix] == text[text.size() - 1 - suffix])
        suffix++;
    const size_t removedEnd = imeText.size() - suffix;
    const size_t insertedEnd = text.size() - suffix;

    if (removedEnd > prefix || insertedEnd > prefix) {
        moveTextInputCaret(removedEnd);
        for (size_t i = prefix; i < removedEnd; i++)
            sendTextInputKey(D_KEY_BACKSPACE);
        imeCaret = prefix;

        // a composing region at the end of the new text is composed, the rest committed
        const bool newComp = hasComp && compStart >= prefix && compEnd == insertedEnd;
        const size_t commitEnd = newComp ? compStart : insertedEnd;
        if (commitEnd > prefix)
            jasperengine::Engine::systemTextInput(codepointsToUtf8(text, prefix, commitEnd));
        imeCaret = commitEnd;

        if (newComp) {
            jasperengine::Engine::systemImeComposition(codepointsToUtf8(text, compStart, compEnd), compCursor);
            imeCompStart = compStart;
            imeCompEnd = compEnd;
            imeCompCursor = compCursor;
        }
    }
    imeText = text;

    // the IME moved its caret (cursor keys on the keyboard, a swipe on the space bar)
    if (imeCompEnd == imeCompStart && caretCollapsed)
        moveTextInputCaret(caret);
}


NativeEngine::NativeEngine(struct android_app *app) {
    mApp = app;
    mHasFocus = mIsVisible = mHasWindow = false;
    mHasGLObjects = false;
    mEglDisplay = EGL_NO_DISPLAY;
    mEglSurface = EGL_NO_SURFACE;
    mEglContext = EGL_NO_CONTEXT;
    mEglReleaseSurface = EGL_NO_SURFACE;
    mEglConfig = 0;
    mSurfWidth = mSurfHeight = 0;
    mDisplayRotation = 0;
    mNaturalPortrait = true;
    mFullscreenGame = true;
    mSafeAreaKnown = false;
    mLastCutoutInsets = ARect{0, 0, 0, 0};
    mLastBarInsets = ARect{0, 0, 0, 0};
    mLastSafeAreaRevision = -1;
    mLastSafeAreaWidth = -1;
    mLastSafeAreaHeight = -1;
    mFallbackSwapInterval = 1;
    mFallbackLimiterFps = 0.0;
    mAppliedSwapInterval = 0;
    mJniEnv = NULL;
    memset(&mState, 0, sizeof(mState));

    mInternalDataPath = mApp->activity->internalDataPath;

    app->motionEventFilter = all_motion_filter;
    app->keyEventFilter = key_filter;

    // only X and Y are copied into motion events unless an axis is enabled
    GameActivityPointerAxes_enableAxis(AMOTION_EVENT_AXIS_VSCROLL);
    GameActivityPointerAxes_enableAxis(AMOTION_EVENT_AXIS_HSCROLL);
    // a stylus's pressure (Input::getPenPressure)
    GameActivityPointerAxes_enableAxis(AMOTION_EVENT_AXIS_PRESSURE);

    // Flags to control how the IME behaves.
    // https://developer.android.com/reference/android/text/InputType
    constexpr int InputType_dot_TYPE_CLASS_TEXT = 1;
    // https://developer.android.com/reference/android/view/inputmethod/EditorInfo
    constexpr int IME_ACTION_NONE = 1;
    constexpr int IME_FLAG_NO_FULLSCREEN = 33554432;
    
    GameActivity_setImeEditorInfo(app->activity, InputType_dot_TYPE_CLASS_TEXT, IME_ACTION_NONE, IME_FLAG_NO_FULLSCREEN);

    if (app->savedState != NULL) {
        // we are starting with previously saved state -- restore it
        mState = *(struct NativeEngineSavedState *) app->savedState;
    }

    // only one instance of NativeEngine may exist!
    assert(_singleton == NULL);
    _singleton = this;

    // initialize Swappy to adjuest swap timing properly. 60 fps until Engine.targetFrameRate
    // says otherwise (JasperEngineAndroid::applyFramePacing)
    SwappyGL_init(getJniEnv(), mApp->activity->javaGameActivity);
    SwappyGL_setSwapIntervalNS(SWAPPY_SWAP_60FPS);

    setupJNI();

    // An app has no command line: the launch options come from a system property instead
    // (adb shell setprop debug.jasperengine.args "--quit-after 600 --fixed-fps 60", as
    // tools/android-emulator-test.sh does), then a player's game.json (S30 R17)
    jasperengine::applyPlatformDefaults();
    std::vector<std::string> launchArgs;
    char property[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.jasperengine.args", property) > 0) {
        std::istringstream words(property);
        for (std::string word; words >> word;) launchArgs.push_back(word);
    }
    // the platform's System is made first: a player reads its content (game.json, the content
    // manifest) inside the APK's assets/game, which only the asset manager behind it can open
    JasperEngineAndroid* system = new JasperEngineAndroid();
    int launched = -1;
    {
        jasperengine::EarlySystemScope early(system);
        launched = jasperengine::player::launch(launchArgs, jasperengine::player::ParseMode::Lenient);
    }
    if (launched >= 0) {
        __android_log_print(ANDROID_LOG_ERROR, "JasperEngine", "exiting with code %d (launch options or content)", launched);
        // frame pacing runs threads of its own: stopped first, it does not crash the exit
        SwappyGL_destroy();
        std::exit(launched);
    }

    jasperengine::Engine::systemInit(0, nullptr, system);
}

NativeEngine::~NativeEngine() {
    // the window is gone by now: what the shutdown releases needs the context current again
    makeContextCurrent();
    jasperengine::Engine::systemShutdown();

    SwappyGL_destroy();

    killContext();

    if (mJniEnv) {
    mApp->activity->vm->DetachCurrentThread();
    mJniEnv = NULL;
    }
    _singleton = NULL;
}

NativeEngine *NativeEngine::getInstance() {
    return _singleton;
}

GameActivity* NativeEngine::getActivity(){
    return mApp->activity;
}

JniData& NativeEngine::getJniData(){
    return mJniData;
}

JavaVM* NativeEngine::getJavaVM(){
    return mApp->activity->vm;
}

JNIEnv* NativeEngine::getJniEnv() {
    if (!mJniEnv) {
        if (0 != mApp->activity->vm->AttachCurrentThread(&mJniEnv, NULL)) {
            jasperengine::Log::error("*** FATAL ERROR: Failed to attach thread to JNI.");
        }
        assert(mJniEnv != NULL);
    }

    return mJniEnv;
}

int NativeEngine::getSurfWidth(){
    return mSurfWidth;
}

int NativeEngine::getSurfHeight(){
    return mSurfHeight;
}

void NativeEngine::setFallbackPacing(int swapInterval, double limiterFps){
    mFallbackSwapInterval = std::max(1, swapInterval);
    mFallbackLimiterFps = limiterFps;
}

int NativeEngine::getDisplayRotation(){
    return mDisplayRotation;
}

bool NativeEngine::isNaturalPortrait(){
    return mNaturalPortrait;
}

int NativeEngine::getDeviceDegrees(){
    if (!mJniData.platformClsRef || !mJniData.platformDeviceDegrees) return -1;
    return getJniEnv()->GetStaticIntField(mJniData.platformClsRef, mJniData.platformDeviceDegrees);
}

void NativeEngine::updateDeviceSignals(){
    if (!mJniData.platformClsRef) return;
    JNIEnv* env = getJniEnv();
    if (!env) return;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (mJniData.platformGetThermalState && now - mLastThermalPoll >= 2.0) {
        mLastThermalPoll = now;
        env->CallStaticIntMethod(mJniData.platformClsRef, mJniData.platformGetThermalState, mJniData.gameActivityObjRef);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (mJniData.platformThermalState) {
        const int state = env->GetStaticIntField(mJniData.platformClsRef, mJniData.platformThermalState);
        jasperengine::Device::reportThermalState((jasperengine::ThermalState)std::clamp(state, 0, jasperengine::THERMAL_STATE_COUNT - 1));
    }
    if (mJniData.platformTrimMemoryCount) {
        const int count = env->GetStaticIntField(mJniData.platformClsRef, mJniData.platformTrimMemoryCount);
        if (count != mTrimMemoryCount) {
            mTrimMemoryCount = count;
            jasperengine::Device::reportLowMemory();
        }
    }
}

bool NativeEngine::refreshDisplayRotation(){
    if (!mJniData.platformClsRef || !mJniData.platformDisplayRotation) return false;
    JNIEnv* env = getJniEnv();
    const int rotation = env->GetStaticIntField(mJniData.platformClsRef, mJniData.platformDisplayRotation);
    if (rotation == mDisplayRotation) return false;
    mDisplayRotation = rotation;
    if (mJniData.platformIsNaturalPortrait) {
        mNaturalPortrait = env->CallStaticBooleanMethod(mJniData.platformClsRef, mJniData.platformIsNaturalPortrait,
                                                        mJniData.gameActivityObjRef) == JNI_TRUE;
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            mNaturalPortrait = true;
        }
    }
    return true;
}

void NativeEngine::updateSafeArea(){
    if (!mApp || !mApp->activity || mSurfWidth <= 0 || mSurfHeight <= 0) return;

    // window pixels: the display cutout (waterfall included; zero below API 28) and, for a game
    // that shows them, the system bars (in fullscreen they come and go over the game, Unity's rule)
    ARect cutout{0, 0, 0, 0};
    ARect bars{0, 0, 0, 0};
    GameActivity_getWindowInsets(mApp->activity, GAMECOMMON_INSETS_TYPE_DISPLAY_CUTOUT, &cutout);
    if (!mFullscreenGame) {
        GameActivity_getWindowInsets(mApp->activity, GAMECOMMON_INSETS_TYPE_SYSTEM_BARS, &bars);
    }
    JNIEnv* env = getJniEnv();
    const int revision = (env && mJniData.platformClsRef && mJniData.platformSafeAreaRevision)
        ? env->GetStaticIntField(mJniData.platformClsRef, mJniData.platformSafeAreaRevision) : 0;
    auto same = [](const ARect& a, const ARect& b){
        return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    };
    if (mSafeAreaKnown && same(cutout, mLastCutoutInsets) && same(bars, mLastBarInsets) && revision == mLastSafeAreaRevision &&
        mSurfWidth == mLastSafeAreaWidth && mSurfHeight == mLastSafeAreaHeight) {
        return;
    }
    mSafeAreaKnown = true;
    mLastCutoutInsets = cutout;
    mLastBarInsets = bars;
    mLastSafeAreaRevision = revision;
    mLastSafeAreaWidth = mSurfWidth;
    mLastSafeAreaHeight = mSurfHeight;

    // where the surface lies in the window (0 in fullscreen) and the cutout rectangles
    int offsets[4] = {0, 0, 0, 0};
    std::vector<jasperengine::Rect> cutouts;
    if (env && mJniData.platformClsRef && mJniData.platformSafeAreaInfo) {
        jintArray array = (jintArray)env->GetStaticObjectField(mJniData.platformClsRef, mJniData.platformSafeAreaInfo);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            array = NULL;
        }
        if (array) {
            const jsize length = env->GetArrayLength(array);
            std::vector<jint> values((size_t)length);
            if (length > 0) env->GetIntArrayRegion(array, 0, length, values.data());
            if (length >= 5) {
                for (int i = 0; i < 4; i++) offsets[i] = std::max(0, (int)values[i]);
                const int count = std::max(0, (int)values[4]);
                for (int i = 0; i < count && 5 + i * 4 + 3 < length; i++) {
                    const jint* r = &values[5 + i * 4];
                    // window to surface pixels
                    cutouts.push_back(jasperengine::Rect((float)(r[0] - offsets[0]), (float)(r[1] - offsets[1]),
                                                         (float)(r[2] - r[0]), (float)(r[3] - r[1])));
                }
            }
            env->DeleteLocalRef(array);
        }
    }

    // the window's insets less where the surface already keeps clear of them
    auto edge = [](int cut, int bar, int offset){ return (float)std::max(0, std::max(cut, bar) - offset); };
    const jasperengine::EdgeInsets insets(edge(cutout.left, bars.left, offsets[0]), edge(cutout.top, bars.top, offsets[1]),
                                          edge(cutout.right, bars.right, offsets[2]), edge(cutout.bottom, bars.bottom, offsets[3]));
    jasperengine::Screen::setPlatformSafeArea(insets, cutouts);
}

void NativeEngine::showSoftInput(std::wstring text){
    // GameTextInput takes Java modified UTF-8 and counts the selection in UTF-16 units
    std::u32string codepoints = wideToCodepoints(text);
    componentTextInput.reset(codepoints);
    int32_t utf16Length = 0;
    std::string strInputBuffer = codepointsToModifiedUtf8(codepoints, utf16Length);

    // a document text field starts from its value, caret at the end
    resetDocumentTextInput(codepoints, codepoints.size());

    GameTextInputState inputState{};
    inputState.text_UTF8 = strInputBuffer.data();
    inputState.text_length = strInputBuffer.length();
    inputState.selection.start = utf16Length;
    inputState.selection.end = utf16Length;
    inputState.composingRegion.start = -1;
    inputState.composingRegion.end = -1;

    GameActivity_setTextInputState(mApp->activity, &inputState);
    GameActivity_showSoftInput(mApp->activity, 0);
}

void NativeEngine::hideSoftInput(){
    GameActivity_hideSoftInput(mApp->activity, 0);
}

std::string NativeEngine::getInternalDataPath(){
    return mInternalDataPath;
}

AAssetManager* NativeEngine::getAssetManager(){
    return mApp->activity->assetManager;
}

void NativeEngine::killGLObjects() {
    if (mHasGLObjects) {
        jasperengine::Engine::systemViewDestroyed();
        mHasGLObjects = false;
    }
}

void NativeEngine::gameLoop() {
    mApp->userData = this;
    mApp->onAppCmd = _handle_cmd_proxy;

    // keeping the screen on is Screen.sleepTimeout's (Player > Keep Screen On, applied by
    // Screen::initialize through JasperEngineAndroid::setKeepScreenOn)
    GameActivity_setWindowFlags(mApp->activity,
                                AWINDOW_FLAG_TURN_SCREEN_ON |
                                AWINDOW_FLAG_FULLSCREEN |
                                AWINDOW_FLAG_SHOW_WHEN_LOCKED,
                                0);

    while (1) {
        int events;
        struct android_poll_source *source;

        // If not animating, block until we get an event; if animating, don't block.
        while ((ALooper_pollOnce(isAnimating() ? 0 : -1, NULL, &events, (void **) &source)) >= 0) {
            // process event
            if (source != NULL) {
                source->process(mApp, source);
            }

            // are we exiting?
            if (mApp->destroyRequested) {
                return;
            }
        }

        handleGameActivityInput();

        if (mApp->textInputState) {
            GameActivity_getTextInputState(mApp->activity, [](void *context, const GameTextInputState *state) {
                if (!context || !state) return;

                std::u16string units = decodeModifiedUtf8(state->text_UTF8, state->text_length);
                std::u32string text;
                std::vector<size_t> unitToCodepoint;
                utf16ToCodepoints(units, text, unitToCodepoint);

                if (jasperengine::Engine::wantsTextComposition()) {
                    updateDocumentTextInput(units, state);
                    // the component InputField diff stays in step
                    componentTextInput.reset(text);
                    return;
                }

                // component InputField: '\b' per removed character, then each typed one
                const jasperengine::android::TextEdit edit = componentTextInput.update(units);
                for (size_t i = 0; i < edit.backspaces; i++){
                    jasperengine::Engine::systemCharInput('\b');
                }
                for (char32_t typed : edit.typed){
                    jasperengine::Engine::systemCharInput(typed);
                }

                // the document diff stays in step, for a document field focused later
                int32_t selectionEnd = std::min(state->selection.end, (int32_t)units.size());
                resetDocumentTextInput(text, (selectionEnd >= 0) ? unitToCodepoint[selectionEnd] : text.size());

            }, this);
            mApp->textInputState = 0;
        }

        if (isAnimating()) {
            doFrame();
        }
    }
}

void NativeEngine::setupJNI(){
    JNIEnv* env = getJniEnv();

    mJniData.gameActivityObjRef = mApp->activity->javaGameActivity;
    mJniData.gameActivityClsRef = env->GetObjectClass(mJniData.gameActivityObjRef);

    mJniData.getUserSettingsRef = env->GetMethodID(mJniData.gameActivityClsRef, "getUserSettings", "()Lcom/jaspergameengine/jasperengine/UserSettings;");

    mJniData.userSettingsObjRef = env->CallObjectMethod(mJniData.gameActivityObjRef, mJniData.getUserSettingsRef);
    mJniData.userSettingsClsRef = env->GetObjectClass(mJniData.userSettingsObjRef);

    mJniData.getBoolForKeyRef = env->GetMethodID(mJniData.userSettingsClsRef, "getBoolForKey", "(Ljava/lang/String;Z)Z");
    mJniData.getIntegerForKeyRef = env->GetMethodID(mJniData.userSettingsClsRef, "getIntegerForKey", "(Ljava/lang/String;I)I");
    mJniData.getLongForKeyRef = env->GetMethodID(mJniData.userSettingsClsRef, "getLongForKey", "(Ljava/lang/String;J)J");
    mJniData.getFloatForKeyRef = env->GetMethodID(mJniData.userSettingsClsRef, "getFloatForKey", "(Ljava/lang/String;F)F");
    mJniData.getDoubleForKeyRef = env->GetMethodID(mJniData.userSettingsClsRef, "getDoubleForKey", "(Ljava/lang/String;D)D");
    mJniData.getStringForKeyRef = env->GetMethodID(mJniData.userSettingsClsRef, "getStringForKey", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");

    mJniData.setBoolForKeyRef = env->GetMethodID(mJniData.userSettingsClsRef, "setBoolForKey", "(Ljava/lang/String;Z)V");
    mJniData.setIntegerForKeyRef = env->GetMethodID(mJniData.userSettingsClsRef, "setIntegerForKey", "(Ljava/lang/String;I)V");
    mJniData.setLongForKeyRef = env->GetMethodID(mJniData.userSettingsClsRef, "setLongForKey", "(Ljava/lang/String;J)V");
    mJniData.setFloatForKeyRef = env->GetMethodID(mJniData.userSettingsClsRef, "setFloatForKey", "(Ljava/lang/String;F)V");
    mJniData.setDoubleForKeyRef = env->GetMethodID(mJniData.userSettingsClsRef, "setDoubleForKey", "(Ljava/lang/String;D)V");
    mJniData.setStringForKeyRef = env->GetMethodID(mJniData.userSettingsClsRef, "setStringForKey", "(Ljava/lang/String;Ljava/lang/String;)V");

    mJniData.removeKeyRef = env->GetMethodID(mJniData.userSettingsClsRef, "removeKey", "(Ljava/lang/String;)V");
    mJniData.hasKeyRef = env->GetMethodID(mJniData.userSettingsClsRef, "hasKey", "(Ljava/lang/String;)Z");
    mJniData.removeAllKeysRef = env->GetMethodID(mJniData.userSettingsClsRef, "removeAllKeys", "()V");
    mJniData.saveUserSettingsRef = env->GetMethodID(mJniData.userSettingsClsRef, "save", "()Z");

    // an activity without them (an older MainActivity) keeps the base in-process clipboard
    mJniData.setClipboardText = env->GetMethodID(mJniData.gameActivityClsRef, "setClipboardText", "(Ljava/lang/String;)V");
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        mJniData.setClipboardText = NULL;
    }
    mJniData.getClipboardText = env->GetMethodID(mJniData.gameActivityClsRef, "getClipboardText", "()Ljava/lang/String;");
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        mJniData.getClipboardText = NULL;
    }
    if (!mJniData.setClipboardText || !mJniData.getClipboardText)
        jasperengine::Log::warn("NativeEngine: activity has no clipboard methods, using an in-process clipboard");

    // the HTTP client's Java side (JasperHttp) and its native callbacks
    jasperengine::httpAndroidSetup(env, mJniData.gameActivityObjRef);
    jasperengine::mediaCodecSetAssetManager(getAssetManager());
    // the native bridge's Java side (JasperNative) and its natives
    jasperengine::nativeBridgeAndroidSetup(env, mJniData.gameActivityObjRef);
    setupPlatformJNI(env);
}

// JasperPlatform through the activity's class loader: the game thread is attached natively, so
// FindClass would only see the system's classes
void NativeEngine::setupPlatformJNI(JNIEnv* env){
    memset(&mJniData.platformClsRef, 0, sizeof(JniData) - offsetof(JniData, platformClsRef));

    jclass activityClass = env->GetObjectClass(mJniData.gameActivityObjRef);
    jmethodID getClassLoader = env->GetMethodID(activityClass, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader = getClassLoader ? env->CallObjectMethod(mJniData.gameActivityObjRef, getClassLoader) : NULL;
    jclass platformClass = NULL;
    if (loader && !env->ExceptionCheck()) {
        jclass loaderClass = env->GetObjectClass(loader);
        jmethodID loadClass = env->GetMethodID(loaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
        jstring name = env->NewStringUTF("com.jaspergameengine.jasperengine.JasperPlatform");
        platformClass = loadClass ? (jclass)env->CallObjectMethod(loader, loadClass, name) : NULL;
        env->DeleteLocalRef(name);
        env->DeleteLocalRef(loaderClass);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        platformClass = NULL;
    }
    if (loader) env->DeleteLocalRef(loader);
    env->DeleteLocalRef(activityClass);
    if (!platformClass) {
        jasperengine::Log::warn("NativeEngine: no JasperPlatform class: openURL, locale, orientation and device info use the defaults");
        return;
    }
    mJniData.platformClsRef = (jclass)env->NewGlobalRef(platformClass);
    env->DeleteLocalRef(platformClass);

    auto method = [&](const char* name, const char* signature) -> jmethodID {
        jmethodID id = env->GetStaticMethodID(mJniData.platformClsRef, name, signature);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            jasperengine::Log::warn("NativeEngine: JasperPlatform.%s is missing", name);
            return NULL;
        }
        return id;
    };
    // (Activity, ...) signatures: rest is what follows the activity parameter
    auto withActivity = [&](const char* name, const char* rest) {
        return method(name, (std::string("(Landroid/app/Activity;") + rest).c_str());
    };
    mJniData.platformOpenURL = withActivity("openURL", "Ljava/lang/String;)Z");
    mJniData.platformGetPreferredLocales = method("getPreferredLocales", "()[Ljava/lang/String;");
    mJniData.platformGetRegion = method("getRegion", "()Ljava/lang/String;");
    mJniData.platformGetAppVersion = withActivity("getAppVersion", ")Ljava/lang/String;");
    mJniData.platformGetAppBuild = withActivity("getAppBuild", ")J");
    mJniData.platformGetAppIdentifier = withActivity("getAppIdentifier", ")Ljava/lang/String;");
    mJniData.platformGetAppName = withActivity("getAppName", ")Ljava/lang/String;");
    mJniData.platformGetOsRelease = method("getOsRelease", "()Ljava/lang/String;");
    mJniData.platformGetApiLevel = method("getApiLevel", "()I");
    mJniData.platformGetBuildDisplay = method("getBuildDisplay", "()Ljava/lang/String;");
    mJniData.platformGetBuildIncremental = method("getBuildIncremental", "()Ljava/lang/String;");
    mJniData.platformGetDeviceModel = method("getDeviceModel", "()Ljava/lang/String;");
    mJniData.platformGetSystemMemorySize = withActivity("getSystemMemorySize", ")I");
    mJniData.platformGetInternetReachability = withActivity("getInternetReachability", ")I");
    mJniData.platformVibrate = withActivity("vibrate", "I)Z");
    mJniData.platformHapticFeedback = withActivity("hapticFeedback", "I)Z");
    mJniData.platformGetScreenDpi = withActivity("getScreenDpi", ")I");
    mJniData.platformGetRefreshRate = withActivity("getRefreshRate", ")F");
    mJniData.platformIsNaturalPortrait = withActivity("isNaturalPortrait", ")Z");
    mJniData.platformSetOrientationPolicy = withActivity("setOrientationPolicy", "I)V");

    mJniData.platformDisplayRotation = env->GetStaticFieldID(mJniData.platformClsRef, "displayRotation", "I");
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        mJniData.platformDisplayRotation = NULL;
    }
    mJniData.platformDeviceDegrees = env->GetStaticFieldID(mJniData.platformClsRef, "deviceDegrees", "I");
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        mJniData.platformDeviceDegrees = NULL;
    }
    mJniData.platformSafeAreaRevision = env->GetStaticFieldID(mJniData.platformClsRef, "safeAreaRevision", "I");
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        mJniData.platformSafeAreaRevision = NULL;
    }
    mJniData.platformSafeAreaInfo = env->GetStaticFieldID(mJniData.platformClsRef, "safeAreaInfo", "[I");
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        mJniData.platformSafeAreaInfo = NULL;
    }
    mJniData.platformThermalState = env->GetStaticFieldID(mJniData.platformClsRef, "thermalState", "I");
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        mJniData.platformThermalState = NULL;
    }
    mJniData.platformTrimMemoryCount = env->GetStaticFieldID(mJniData.platformClsRef, "trimMemoryCount", "I");
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        mJniData.platformTrimMemoryCount = NULL;
    }
    mJniData.platformGetThermalState = withActivity("getThermalState", ")I");

    // Player > Android > Fullscreen as the export wrote it into MainActivity (true without it)
    jmethodID isFullscreenGame = env->GetMethodID(mJniData.gameActivityClsRef, "isFullscreenGame", "()Z");
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        isFullscreenGame = NULL;
    }
    mFullscreenGame = true;
    if (isFullscreenGame) {
        mFullscreenGame = env->CallBooleanMethod(mJniData.gameActivityObjRef, isFullscreenGame) == JNI_TRUE;
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            mFullscreenGame = true;
        }
    }

    // the orientation the app starts in
    mDisplayRotation = -1;
    refreshDisplayRotation();
}

bool NativeEngine::initDisplay() {
    if (mEglDisplay != EGL_NO_DISPLAY) {
        return true;
    }

    mEglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (EGL_FALSE == eglInitialize(mEglDisplay, 0, 0)) {
        jasperengine::Log::error("NativeEngine: failed to init display, error %d", eglGetError());
        return false;
    }
    return true;
}

bool NativeEngine::initSurface() {
    assert(mEglDisplay != EGL_NO_DISPLAY);

    if (mEglSurface != EGL_NO_SURFACE) {
        return true;
    }

    EGLint numConfigs;

    // the swapchain is a depth-stencil target (stencil masks of the shader library)
    const EGLint attribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, // request OpenGL ES 3.0
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_BLUE_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_RED_SIZE, 8,
            EGL_DEPTH_SIZE, 24,
            EGL_STENCIL_SIZE, 8,
            EGL_NONE
    };

    // Pick the first EGLConfig that matches.
    eglChooseConfig(mEglDisplay, attribs, &mEglConfig, 1, &numConfigs);

    if (!numConfigs){
        // without stencil (stencil tests then always pass)
        const EGLint attribs[] = {
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, // request OpenGL ES 3.0
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                EGL_BLUE_SIZE, 8,
                EGL_GREEN_SIZE, 8,
                EGL_RED_SIZE, 8,
                EGL_DEPTH_SIZE, 24,
                EGL_NONE
        };

        eglChooseConfig(mEglDisplay, attribs, &mEglConfig, 1, &numConfigs);
    }

    if (!numConfigs){
        // fall back to 16bit depth buffer
        const EGLint attribs[] = {
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, // request OpenGL ES 3.0
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                EGL_BLUE_SIZE, 8,
                EGL_GREEN_SIZE, 8,
                EGL_RED_SIZE, 8,
                EGL_DEPTH_SIZE, 16,
                EGL_NONE
        };

        eglChooseConfig(mEglDisplay, attribs, &mEglConfig, 1, &numConfigs);
    }

    if (!numConfigs) {
        jasperengine::Log::error("Unable to retrieve EGL config");
        return false;
    }

    // create EGL surface
    mEglSurface = eglCreateWindowSurface(mEglDisplay, mEglConfig, mApp->window, NULL);
    if (mEglSurface == EGL_NO_SURFACE) {
        jasperengine::Log::error("Failed to create EGL surface, EGL error %d", eglGetError());
        return false;
    }

    return true;
}

namespace {

    // The shader creation's worker context (spec S32): a context sharing the render context, over
    // a 1x1 pbuffer of its config or no surface (EGL_KHR_surfaceless_context), current on the
    // creation thread
    class EglWorkerContext : public jasperengine::GpuWorkerContext {
    public:
        static std::unique_ptr<jasperengine::GpuWorkerContext> create(EGLDisplay display, EGLConfig config, EGLContext share){
            const EGLint attribList[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
            EGLContext context = eglCreateContext(display, config, share, attribList);
            if (context == EGL_NO_CONTEXT){
                jasperengine::Log::warn("EGL: no worker context in the render context's share group, EGL error %d", eglGetError());
                return nullptr;
            }
            EGLSurface surface = EGL_NO_SURFACE;
            EGLint surfaceType = 0;
            eglGetConfigAttrib(display, config, EGL_SURFACE_TYPE, &surfaceType);
            if (surfaceType & EGL_PBUFFER_BIT){
                const EGLint attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
                surface = eglCreatePbufferSurface(display, config, attribs);
            }
            if (surface == EGL_NO_SURFACE){
                const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
                if (!extensions || !strstr(extensions, "EGL_KHR_surfaceless_context")){
                    eglDestroyContext(display, context);
                    jasperengine::Log::warn("EGL: no surface for the worker context");
                    return nullptr;
                }
            }
            return std::unique_ptr<jasperengine::GpuWorkerContext>(new EglWorkerContext(display, context, surface));
        }

        ~EglWorkerContext() override {
            if (surface != EGL_NO_SURFACE)
                eglDestroySurface(display, surface);
            eglDestroyContext(display, context);
        }

        bool makeCurrent() override {
            return eglMakeCurrent(display, surface, surface, context) == EGL_TRUE;
        }

        void release() override {
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglReleaseThread();
        }

    private:
        EglWorkerContext(EGLDisplay display, EGLContext context, EGLSurface surface)
            : display(display), context(context), surface(surface) {}

        EGLDisplay display;
        EGLContext context;
        EGLSurface surface;
    };

}

bool NativeEngine::initContext() {
    // need a display
    assert(mEglDisplay != EGL_NO_DISPLAY);

    EGLint attribList[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE}; // OpenGL ES 3.0

    if (mEglContext != EGL_NO_CONTEXT) {
        return true;
    }

    mEglContext = eglCreateContext(mEglDisplay, mEglConfig, NULL, attribList);
    if (mEglContext == EGL_NO_CONTEXT) {
        jasperengine::Log::error("Failed to create EGL context, EGL error %d", eglGetError());
        return false;
    }

    // the engine's shader creation thread (spec S32), made when its render system starts
    const EGLDisplay display = mEglDisplay;
    const EGLConfig config = mEglConfig;
    const EGLContext share = mEglContext;
    jasperengine::setGpuWorkerContextFactory([display, config, share]() {
        return EglWorkerContext::create(display, config, share);
    });
    return true;
}

bool NativeEngine::makeContextCurrent() {
    if (mEglDisplay == EGL_NO_DISPLAY || mEglContext == EGL_NO_CONTEXT) {
        return false;
    }
    if (mEglSurface != EGL_NO_SURFACE) {
        return eglMakeCurrent(mEglDisplay, mEglSurface, mEglSurface, mEglContext) == EGL_TRUE;
    }
    // no window: a 1x1 pbuffer of the context's config, else no surface at all
    // (EGL_KHR_surfaceless_context)
    if (mEglReleaseSurface == EGL_NO_SURFACE) {
        EGLint surfaceType = 0;
        eglGetConfigAttrib(mEglDisplay, mEglConfig, EGL_SURFACE_TYPE, &surfaceType);
        if (surfaceType & EGL_PBUFFER_BIT) {
            const EGLint attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
            mEglReleaseSurface = eglCreatePbufferSurface(mEglDisplay, mEglConfig, attribs);
        }
    }
    if (mEglReleaseSurface != EGL_NO_SURFACE &&
            eglMakeCurrent(mEglDisplay, mEglReleaseSurface, mEglReleaseSurface, mEglContext) == EGL_TRUE) {
        return true;
    }
    const char* extensions = eglQueryString(mEglDisplay, EGL_EXTENSIONS);
    if (extensions && strstr(extensions, "EGL_KHR_surfaceless_context") &&
            eglMakeCurrent(mEglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, mEglContext) == EGL_TRUE) {
        return true;
    }
    jasperengine::Log::warn("NativeEngine: no surface to make the context current without the window, EGL error %d", eglGetError());
    return false;
}

void NativeEngine::killContext() {
    // APP_CMD_TERM_WINDOW unbound the context with its surface: the engine's GL objects are
    // deleted with it current again, not as calls with no context (libEGL logs those)
    if (mHasGLObjects) {
        makeContextCurrent();
    }
    killGLObjects();

    eglMakeCurrent(mEglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (mEglContext != EGL_NO_CONTEXT) {
        jasperengine::setGpuWorkerContextFactory(nullptr);
        eglDestroyContext(mEglDisplay, mEglContext);
        mEglContext = EGL_NO_CONTEXT;
    }
    if (mEglReleaseSurface != EGL_NO_SURFACE) {
        eglDestroySurface(mEglDisplay, mEglReleaseSurface);
        mEglReleaseSurface = EGL_NO_SURFACE;
    }
}

void NativeEngine::killSurface() {
    eglMakeCurrent(mEglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    // a new surface takes the swap interval again
    mAppliedSwapInterval = 0;
    if (mEglSurface != EGL_NO_SURFACE) {
        eglDestroySurface(mEglDisplay, mEglSurface);
        mEglSurface = EGL_NO_SURFACE;
    }
}

void NativeEngine::killDisplay() {
    killContext();
    killSurface();

    if (mEglDisplay != EGL_NO_DISPLAY) {
        eglTerminate(mEglDisplay);
        mEglDisplay = EGL_NO_DISPLAY;
    }
}

void NativeEngine::handleEglError(EGLint error) {
    switch (error) {
        case EGL_SUCCESS:
            return;
        case EGL_CONTEXT_LOST:
            jasperengine::Log::warn("NativeEngine: egl error: EGL_CONTEXT_LOST. Recreating context.");
            killContext();
            return;
        case EGL_BAD_CONTEXT:
            jasperengine::Log::warn("NativeEngine: egl error: EGL_BAD_CONTEXT. Recreating context.");
            killContext();
            return;
        case EGL_BAD_DISPLAY:
            jasperengine::Log::warn("NativeEngine: egl error: EGL_BAD_DISPLAY. Recreating display.");
            killDisplay();
            return;
        case EGL_BAD_SURFACE:
            jasperengine::Log::warn("NativeEngine: egl error: EGL_BAD_SURFACE. Recreating display.");
            killSurface();
            return;
        default:
            jasperengine::Log::warn("NativeEngine: unknown egl error: %d", error);
            return;
    }
}

bool NativeEngine::prepareToRender() {
    if (mEglDisplay == EGL_NO_DISPLAY || mEglSurface == EGL_NO_SURFACE ||
        mEglContext == EGL_NO_CONTEXT) {

        // create display if needed
        if (!initDisplay()) {
            jasperengine::Log::error("NativeEngine: failed to create display.");
            return false;
        }

        // create surface if needed
        if (!initSurface()) {
            jasperengine::Log::error("NativeEngine: failed to create surface.");
            return false;
        }

        // create context if needed
        if (!initContext()) {
            jasperengine::Log::error("NativeEngine: failed to create context.");
            return false;
        }

        // bind them
        if (EGL_FALSE == eglMakeCurrent(mEglDisplay, mEglSurface, mEglSurface, mEglContext)) {
            const EGLint error = eglGetError();
            jasperengine::Log::error("NativeEngine: eglMakeCurrent failed, EGL error %d", error);
            handleEglError(error);
            // Every handle may still be set, which would skip this binding next frame.
            killSurface();
            return false;
        }

    }
    if (!mHasGLObjects) {
        jasperengine::Engine::systemViewLoaded();
        mHasGLObjects = true;
    }

    jasperengine::Engine::systemViewChanged();

    // ready to render
    return true;
}

void NativeEngine::doFrame() {
    // prepare to render (create context, surfaces, etc, if needed)
    if (!prepareToRender()) {
        // not ready
        jasperengine::Log::error("NativeEngine: preparation to render failed.");
        return;
    }

    // how big is the surface? We query every frame because it's cheap, and some
    // strange devices out there change the surface size without calling any callbacks...
    int width, height;
    eglQuerySurface(mEglDisplay, mEglSurface, EGL_WIDTH, &width);
    eglQuerySurface(mEglDisplay, mEglSurface, EGL_HEIGHT, &height);

    // a rotation changes the display rotation; a half turn changes nothing else
    const bool rotated = refreshDisplayRotation();

    if (width != mSurfWidth || height != mSurfHeight) {
        mSurfWidth = width;
        mSurfHeight = height;

        // the insets that go with the new size, then onCanvasChanged, onOrientationChanged and
        // onSafeAreaChanged
        updateSafeArea();
        jasperengine::Engine::systemViewChanged();

        glViewport(0, 0, mSurfWidth, mSurfHeight);
    } else if (rotated) {
        jasperengine::Engine::systemOrientationChanged();
    }
    // insets that changed without a new surface size (a half turn moves the cutout; the
    // event comes at the start of this frame's update). During a quarter turn that waits for its
    // surface size Screen holds them for that size (Engine::isOrientationPending)
    updateSafeArea();

    // the thermal state and memory warnings since the last frame (spec S56)
    updateDeviceSignals();

    jasperengine::Engine::systemDraw();

    // without Swappy, SwappyGL_swap is eglSwapBuffers: pace it with the swap interval
    const bool swappy = SwappyGL_isEnabled();
    if (!swappy && mAppliedSwapInterval != mFallbackSwapInterval) {
        eglSwapInterval(mEglDisplay, mFallbackSwapInterval);
        mAppliedSwapInterval = mFallbackSwapInterval;
    }

    // swap buffers
    if (!SwappyGL_swap(mEglDisplay, mEglSurface)) {        // failed to swap buffers...
        const EGLint error = eglGetError();
        jasperengine::Log::error("NativeEngine: SwappyGL_swap failed, EGL error %d", error);
        handleEglError(error);
        return;
    }
    jasperengine::Engine::systemFramePresented();

    if (!swappy) {
        mFrameLimiter.wait(mFallbackLimiterFps);
    }

    // print out GL errors, if any
    GLenum e;
    static int errorsPrinted = 0;
    while ((e = glGetError()) != GL_NO_ERROR) {
        if (errorsPrinted < MAX_GL_ERRORS) {
            _log_opengl_error(e);
            ++errorsPrinted;
            if (errorsPrinted >= MAX_GL_ERRORS) {
                jasperengine::Log::error("*** NativeEngine: TOO MANY OPENGL ERRORS. NO LONGER PRINTING.");
            }
        }
    }
}

// Visible but unfocused (a permission dialog, the notification shade) keeps rendering, as Unity
bool NativeEngine::isAnimating() {
    return mIsVisible && mHasWindow;
}

// Gamepads are registered lazily on their first event; Android has no NDK-side
// disconnect notification, so slots stay allocated until the app restarts.
int NativeEngine::getGamepadSlot(int32_t deviceId){
    for (int i = 0; i < MAX_GAMEPADS; i++){
        if (mGamepads[i].deviceId == deviceId)
            return i;
    }
    for (int i = 0; i < MAX_GAMEPADS; i++){
        if (mGamepads[i].deviceId == -1){
            mGamepads[i] = GamepadDevice();
            mGamepads[i].deviceId = deviceId;
            // triggers rest at -1; seed so the first stick-only motion event
            // doesn't emit spurious trigger axis moves from 0 to -1
            mGamepads[i].axes[D_GAMEPAD_AXIS_LEFT_TRIGGER] = -1.0f;
            mGamepads[i].axes[D_GAMEPAD_AXIS_RIGHT_TRIGGER] = -1.0f;
            jasperengine::Engine::systemGamepadConnect(i, "Gamepad");
            return i;
        }
    }
    return -1;
}

void NativeEngine::handleGamepadMotion(GameActivityMotionEvent* motionEvent){
    if (motionEvent->pointerCount < 1)
        return;

    int slot = getGamepadSlot(motionEvent->deviceId);
    if (slot < 0)
        return;

    GamepadDevice& pad = mGamepads[slot];
    GameActivityPointerAxes* pointer = &motionEvent->pointers[0];

    auto forwardAxis = [&](int axis, float value){
        if (fabsf(value - pad.axes[axis]) > 0.001f){
            pad.axes[axis] = value;
            jasperengine::Engine::systemGamepadAxisMove(slot, axis, value);
        }
    };

    forwardAxis(D_GAMEPAD_AXIS_LEFT_X, GameActivityPointerAxes_getAxisValue(pointer, AMOTION_EVENT_AXIS_X));
    forwardAxis(D_GAMEPAD_AXIS_LEFT_Y, GameActivityPointerAxes_getAxisValue(pointer, AMOTION_EVENT_AXIS_Y));
    forwardAxis(D_GAMEPAD_AXIS_RIGHT_X, GameActivityPointerAxes_getAxisValue(pointer, AMOTION_EVENT_AXIS_Z));
    forwardAxis(D_GAMEPAD_AXIS_RIGHT_Y, GameActivityPointerAxes_getAxisValue(pointer, AMOTION_EVENT_AXIS_RZ));

    // triggers report 0..1 (some devices use BRAKE/GAS instead); convert to
    // the [-1, 1] engine convention where -1 is released
    float ltrigger = GameActivityPointerAxes_getAxisValue(pointer, AMOTION_EVENT_AXIS_LTRIGGER);
    if (ltrigger == 0.0f)
        ltrigger = GameActivityPointerAxes_getAxisValue(pointer, AMOTION_EVENT_AXIS_BRAKE);
    forwardAxis(D_GAMEPAD_AXIS_LEFT_TRIGGER, ltrigger * 2.0f - 1.0f);

    float rtrigger = GameActivityPointerAxes_getAxisValue(pointer, AMOTION_EVENT_AXIS_RTRIGGER);
    if (rtrigger == 0.0f)
        rtrigger = GameActivityPointerAxes_getAxisValue(pointer, AMOTION_EVENT_AXIS_GAS);
    forwardAxis(D_GAMEPAD_AXIS_RIGHT_TRIGGER, rtrigger * 2.0f - 1.0f);

    // dpad arrives as hat axes; convert to button events
    int hatX = (int)lroundf(GameActivityPointerAxes_getAxisValue(pointer, AMOTION_EVENT_AXIS_HAT_X));
    int hatY = (int)lroundf(GameActivityPointerAxes_getAxisValue(pointer, AMOTION_EVENT_AXIS_HAT_Y));

    auto forwardHat = [&](int& prev, int current, int negButton, int posButton){
        if (current == prev)
            return;
        if (prev < 0)
            jasperengine::Engine::systemGamepadButtonUp(slot, negButton);
        if (prev > 0)
            jasperengine::Engine::systemGamepadButtonUp(slot, posButton);
        if (current < 0)
            jasperengine::Engine::systemGamepadButtonDown(slot, negButton);
        if (current > 0)
            jasperengine::Engine::systemGamepadButtonDown(slot, posButton);
        prev = current;
    };

    forwardHat(pad.hatX, hatX, D_GAMEPAD_BUTTON_DPAD_LEFT, D_GAMEPAD_BUTTON_DPAD_RIGHT);
    forwardHat(pad.hatY, hatY, D_GAMEPAD_BUTTON_DPAD_UP, D_GAMEPAD_BUTTON_DPAD_DOWN);
}

void NativeEngine::handleGameActivityInput(){
    // Back plays as an Escape press, released on the next frame
    if (backEscapeDown) {
        jasperengine::Engine::systemKeyUp(D_KEY_ESCAPE, false, jasperengine::Input::getModifiers());
        backEscapeDown = false;
    }
    if (backPressed.exchange(false)) {
        jasperengine::Engine::systemCharInput('\x1b');
        jasperengine::Engine::systemKeyDown(D_KEY_ESCAPE, false, jasperengine::Input::getModifiers());
        backEscapeDown = true;
    }

    android_input_buffer* inputBuffer = android_app_swap_input_buffers(mApp);

    if (inputBuffer == nullptr) return;

    if (inputBuffer->keyEventsCount != 0) {
        for (uint64_t i = 0; i < inputBuffer->keyEventsCount; ++i) {
            GameActivityKeyEvent* keyEvent = &inputBuffer->keyEvents[i];

            if ((keyEvent->source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD) {
                int gamepadButton = getJasperEngineGamepadButton(keyEvent->keyCode);
                if (gamepadButton >= 0) {
                    int slot = getGamepadSlot(keyEvent->deviceId);
                    if (slot >= 0) {
                        if (keyEvent->action == AKEY_EVENT_ACTION_DOWN && keyEvent->repeatCount == 0) {
                            jasperengine::Engine::systemGamepadButtonDown(slot, gamepadButton);
                        } else if (keyEvent->action == AKEY_EVENT_ACTION_UP) {
                            jasperengine::Engine::systemGamepadButtonUp(slot, gamepadButton);
                        }
                    }
                    continue;
                }
            }

            // metaState works with capslock, numlock and others
            int keyCode = getJasperEngineKey(keyEvent->keyCode, keyEvent->scanCode);
            int modifiers = getJasperEngineModifiers(keyEvent->metaState);
            bool repeat = keyEvent->repeatCount > 0;

            if (keyEvent->action == AKEY_EVENT_ACTION_DOWN) {
                if (keyEvent->keyCode == AKEYCODE_TAB)
                    jasperengine::Engine::systemCharInput('\t');
                if (keyEvent->keyCode == AKEYCODE_DEL)
                    jasperengine::Engine::systemCharInput('\b');
                if (keyEvent->keyCode == AKEYCODE_ENTER)
                    jasperengine::Engine::systemCharInput('\r');
                if (keyEvent->keyCode == AKEYCODE_ESCAPE)
                    jasperengine::Engine::systemCharInput('\x1b');
                // some IMEs deliver the soft-keyboard backspace as BACK instead of DEL.
                // Only take it when the event really came from the software keyboard,
                // otherwise the system back button/gesture would erase a character.
                if (keyEvent->keyCode == AKEYCODE_BACK && (keyEvent->flags & AKEY_EVENT_FLAG_SOFT_KEYBOARD))
                    jasperengine::Engine::systemCharInput('\b');

                // the soft-keyboard Back above has no engine key; don't spam
                // listeners with D_KEY_UNKNOWN
                if (keyCode != D_KEY_UNKNOWN)
                    jasperengine::Engine::systemKeyDown(keyCode, repeat, modifiers);
            } else if (keyEvent->action == AKEY_EVENT_ACTION_UP) {
                if (keyCode != D_KEY_UNKNOWN)
                    jasperengine::Engine::systemKeyUp(keyCode, repeat, modifiers);
            }

        }
        android_app_clear_key_events(inputBuffer);
    }

    if (inputBuffer->motionEventsCount != 0) {
        for (uint64_t i = 0; i < inputBuffer->motionEventsCount; ++i) {
            GameActivityMotionEvent* motionEvent = &inputBuffer->motionEvents[i];

            if ((motionEvent->source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
                handleGamepadMotion(motionEvent);
                continue;
            }

            int pointerAction = motionEvent->action & AMOTION_EVENT_ACTION_MASK;

            // mouse wheel and touchpad scrolling. VSCROLL is positive up, as the engine's y;
            // HSCROLL is positive right, the engine's x is positive left.
            if (pointerAction == AMOTION_EVENT_ACTION_SCROLL) {
                if (motionEvent->pointerCount > 0) {
                    float vscroll = GameActivityPointerAxes_getAxisValue(&motionEvent->pointers[0], AMOTION_EVENT_AXIS_VSCROLL);
                    float hscroll = GameActivityPointerAxes_getAxisValue(&motionEvent->pointers[0], AMOTION_EVENT_AXIS_HSCROLL);
                    if (vscroll != 0.0f || hscroll != 0.0f)
                        jasperengine::Engine::systemMouseScroll(-hscroll, vscroll, getJasperEngineModifiers(motionEvent->metaState));
                }
                continue;
            }

            // the pointer that went down or up, else the first (MOVE, HOVER_*), before the
            // engine hears of the event
            if (motionEvent->pointerCount > 0) {
                const int actionIndex = (motionEvent->action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
                const int penIndex = actionIndex < (int)motionEvent->pointerCount ? actionIndex : 0;
                jasperengine::Input::setPenPressure(stylusPressureOf(motionEvent->pointers[penIndex]));
            }

            // a mouse moving with no button down. Pressed buttons arrive as DOWN/MOVE/UP and
            // stay on the touch path below.
            if (pointerAction == AMOTION_EVENT_ACTION_HOVER_MOVE ||
                    pointerAction == AMOTION_EVENT_ACTION_HOVER_ENTER ||
                    pointerAction == AMOTION_EVENT_ACTION_HOVER_EXIT) {
                if ((motionEvent->source & AINPUT_SOURCE_MOUSE) == AINPUT_SOURCE_MOUSE && motionEvent->pointerCount > 0) {
                    if (pointerAction == AMOTION_EVENT_ACTION_HOVER_EXIT) {
                        jasperengine::Engine::systemMouseLeave();
                    } else {
                        if (pointerAction == AMOTION_EVENT_ACTION_HOVER_ENTER)
                            jasperengine::Engine::systemMouseEnter();
                        float hoverX = GameActivityPointerAxes_getX(&motionEvent->pointers[0]);
                        float hoverY = GameActivityPointerAxes_getY(&motionEvent->pointers[0]);
                        jasperengine::Engine::systemMouseMove(hoverX, hoverY, getJasperEngineModifiers(motionEvent->metaState));
                    }
                }
                continue;
            }

            if (motionEvent->pointerCount > 0) {
                int action = motionEvent->action;
                int actionMasked = action & AMOTION_EVENT_ACTION_MASK;

                // MOVE has no pointer index. Every finger's position is in the array;
                // updating only index 0 freezes the other finger on its button.
                if (actionMasked == AMOTION_EVENT_ACTION_MOVE) {
                    for (int p = 0; p < (int)motionEvent->pointerCount; ++p) {
                        int motionPointerId = motionEvent->pointers[p].id;
                        float motionX = GameActivityPointerAxes_getX(&motionEvent->pointers[p]);
                        float motionY = GameActivityPointerAxes_getY(&motionEvent->pointers[p]);
                        jasperengine::Engine::systemTouchMove(motionPointerId, motionX, motionY);
                    }
                } else if (actionMasked == AMOTION_EVENT_ACTION_CANCEL) {
                    jasperengine::Engine::systemTouchCancel();
                } else {
                    int ptrIndex = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
                    if (ptrIndex >= 0 && ptrIndex < (int)motionEvent->pointerCount) {
                        int motionPointerId = motionEvent->pointers[ptrIndex].id;
                        float motionX = GameActivityPointerAxes_getX(&motionEvent->pointers[ptrIndex]);
                        float motionY = GameActivityPointerAxes_getY(&motionEvent->pointers[ptrIndex]);

                        if (actionMasked == AMOTION_EVENT_ACTION_DOWN || actionMasked == AMOTION_EVENT_ACTION_POINTER_DOWN) {
                            jasperengine::Engine::systemTouchStart(motionPointerId, motionX, motionY);
                        } else if (actionMasked == AMOTION_EVENT_ACTION_UP || actionMasked == AMOTION_EVENT_ACTION_POINTER_UP) {
                            jasperengine::Engine::systemTouchEnd(motionPointerId, motionX, motionY);
                        }
                    }
                }
            }

        }
        android_app_clear_motion_events(inputBuffer);
    }
}

void NativeEngine::handleCommand(int32_t cmd) {
    switch (cmd) {
        case APP_CMD_SAVE_STATE:
            // The system has asked us to save our current state.
            mState.mHasFocus = mHasFocus;
            mApp->savedState = malloc(sizeof(mState));
            *((NativeEngineSavedState *) mApp->savedState) = mState;
            mApp->savedStateSize = sizeof(mState);
            break;
        case APP_CMD_INIT_WINDOW:
            if (mApp->window != NULL) {
                mHasWindow = true;
                SwappyGL_setWindow(mApp->window);
                if (mApp->savedStateSize == sizeof(mState) && mApp->savedState != nullptr) {
                    mState = *((NativeEngineSavedState *) mApp->savedState);
                    mHasFocus = mState.mHasFocus;
                } else {
                    // Workaround APP_CMD_GAINED_FOCUS issue where the focus state is not
                    // passed down from NativeActivity when restarting Activity
                    mHasFocus = appState.mHasFocus;
                }
            }
            break;
        case APP_CMD_TERM_WINDOW:
            // The window is going away -- kill the surface
            killSurface();
            mHasWindow = false;
            break;
        case APP_CMD_GAINED_FOCUS:
            mHasFocus = true;
            mState.mHasFocus = appState.mHasFocus = mHasFocus;
            jasperengine::Engine::systemFocusChanged(true);
            jasperengine::Engine::systemFocus(true);
            break;
        case APP_CMD_LOST_FOCUS:
            mHasFocus = false;
            mState.mHasFocus = appState.mHasFocus = mHasFocus;
            // the fingers and pressed buttons are let go (a notification shade, a call)
            jasperengine::Engine::systemFocusChanged(false);
            jasperengine::Engine::systemFocus(false);
            break;
        case APP_CMD_PAUSE:
            jasperengine::Engine::systemPause();
            break;
        case APP_CMD_RESUME:
            jasperengine::Engine::systemResume();
            break;
        case APP_CMD_STOP:
            mIsVisible = false;
            break;
        case APP_CMD_START:
            mIsVisible = true;
            break;
        case APP_CMD_WINDOW_INSETS_CHANGED:
            // the display cutout or the system bars changed: read in the next doFrame, after the
            // display rotation and the surface size, so the insets go with the size they belong
            // to (a rotation reports the new insets before the surface resizes)
            break;
        case APP_CMD_CONFIG_CHANGED:
            // a rotation or a locale change (configChanges keeps the activity): the surface size
            // follows in a later frame, which reports the rotation after onCanvasChanged
            break;
        case APP_CMD_LOW_MEMORY:
            jasperengine::Engine::systemLowMemory();
            // system told us we have low memory. So if we are not visible, let's
            // cooperate by deallocating all of our graphic resources.
            // killContext makes the context current without a surface to delete the GL objects
            // (EGL_KHR_surfaceless_context); destroying the context frees the GPU memory anyway.
            if (!mHasWindow) {
                killContext();
            }
            break;
        default:
            break;
    }

}
