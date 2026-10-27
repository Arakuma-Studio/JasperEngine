// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "JasperEngineWeb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <set>

#include <emscripten/emscripten.h>

#include "Engine.h"
#include "Input.h"
#include "System.h"
#include "Log.h"

// Prefix of the saved keys, set per project by the export
#ifndef JASPERENGINE_STORAGE_ID
#define JASPERENGINE_STORAGE_ID "JasperEngine"
#endif

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wmissing-braces"
#endif

std::string JasperEngineWeb::canvas;

int JasperEngineWeb::syncWaitTime;
bool JasperEngineWeb::enabledIDB;
bool JasperEngineWeb::loadedIDB;

int JasperEngineWeb::screenWidth;
int JasperEngineWeb::screenHeight;

double JasperEngineWeb::mousePosX;
double JasperEngineWeb::mousePosY;

int JasperEngineWeb::sampleCount = 0;

bool JasperEngineWeb::quitRequested = false;

JasperEngineWeb::GamepadState JasperEngineWeb::gamepads[JASPERENGINE_WEB_MAX_GAMEPADS];

extern "C" {
    EMSCRIPTEN_KEEPALIVE 
    int getScreenWidth() {
        return jasperengine::System::instance().getScreenWidth();
    }

    EMSCRIPTEN_KEEPALIVE 
    int getScreenHeight() {
        return jasperengine::System::instance().getScreenHeight();
    }

    EMSCRIPTEN_KEEPALIVE 
    void changeCanvasSize(int nWidth, int nHeight){
        JasperEngineWeb::changeCanvasSize(nWidth, nHeight);
    }

    EMSCRIPTEN_KEEPALIVE 
    void syncfs_enable_callback(const char* err) {
	    if (!err || err[0]) {
		    jasperengine::Log::error("Failed to enable IndexedDB: %s", err);
            JasperEngineWeb::setEnabledIDB(false);
	    }else{
            JasperEngineWeb::setEnabledIDB(true);
        }
    }

    EMSCRIPTEN_KEEPALIVE 
    void syncfs_callback(const char* err) {
	    if (!err || err[0]) {
		    jasperengine::Log::error("Failed to save in iDB file system: %s", err);
	    }
    }

    // ---- the hidden IME textarea (jasper_ime_create) and the clipboard

    EMSCRIPTEN_KEEPALIVE
    void jasper_web_text_input(const char* text) {
        if (text && text[0])
            jasperengine::Engine::systemTextInput(text);
    }

    EMSCRIPTEN_KEEPALIVE
    void jasper_web_ime_composition(const char* text, int cursor) {
        jasperengine::Engine::systemImeComposition(text ? text : "", cursor);
    }

    EMSCRIPTEN_KEEPALIVE
    void jasper_web_ime_cancel() {
        jasperengine::Engine::systemImeCancel();
    }

    // a paste event: the text becomes what getClipboardText returns, and goes in as text
    EMSCRIPTEN_KEEPALIVE
    void jasper_web_paste(const char* text) {
        JasperEngineWeb::rememberClipboardText(text ? text : "");
        if (text && text[0])
            jasperengine::Engine::systemTextInput(text);
    }

    // a soft keyboard key that came only as an input event (Android Chrome sends keyCode 229)
    EMSCRIPTEN_KEEPALIVE
    void jasper_web_soft_key(int key) {
        jasperengine::Engine::systemKeyDown(key, false, 0);
        if (key == D_KEY_BACKSPACE) jasperengine::Engine::systemCharInput('\b');
        if (key == D_KEY_ENTER) jasperengine::Engine::systemCharInput('\r');
        jasperengine::Engine::systemKeyUp(key, false, 0);
    }

    EMSCRIPTEN_KEEPALIVE 
    void crazygamesad_started_callback() {
        jasperengine::Engine::systemPause();
    }

    EMSCRIPTEN_KEEPALIVE 
    void crazygamesad_finished_callback() {
        jasperengine::Engine::systemResume();
    }

    EMSCRIPTEN_KEEPALIVE 
    void crazygamesad_error_callback(const char* err) {
	    if (!err || err[0]) {
		    jasperengine::Log::error("Failed to load CrazyGames ad: %s", err);
	    }
        jasperengine::Engine::systemResume();
    }
}



// A hidden <textarea id="jasper-ime"> receives the text of HTML UI documents: focused while a
// document text field has focus (it opens the mobile keyboard), placed at the caret so the
// IME candidate window follows it. Its value is cleared after each committed input.
EM_JS(void, jasper_ime_create, (), {
    if (document.getElementById("jasper-ime")) return;
    var ta = document.createElement("textarea");
    ta.id = "jasper-ime";
    ta.setAttribute("autocomplete", "off");
    ta.setAttribute("autocorrect", "off");
    ta.setAttribute("autocapitalize", "off");
    ta.setAttribute("spellcheck", "false");
    ta.setAttribute("aria-hidden", "true");
    ta.tabIndex = -1;
    // 16px keeps iOS from zooming in on focus
    ta.style.cssText = "position:fixed;left:0;top:0;width:1px;height:16px;padding:0;border:0;margin:0;" +
        "opacity:0;resize:none;overflow:hidden;outline:none;font-size:16px;caret-color:transparent;" +
        "pointer-events:none;white-space:pre;z-index:0;";
    document.body.appendChild(ta);

    // suppress: jasper_ime_focus(0) is clearing and blurring the textarea, which makes the
    // browser commit a composition the engine already cancelled (compositionend, input)
    var state = { composing: false, softKey: false, suppress: false };
    var away = function() { return state.suppress || document.activeElement !== ta; };
    Module.jasperIme = state;
    // ccall copies strings onto the 4 MB stack
    var MAX_TEXT = 262144;
    var clip = function(text) { return text.length > MAX_TEXT ? text.substring(0, MAX_TEXT) : text; };
    var codepoints = function(text) { return Array.from(text).length; };

    ta.addEventListener("compositionstart", function(e) {
        state.composing = true;
        ccall("jasper_web_ime_composition", null, ["string", "number"], ["", 0]);
    });
    ta.addEventListener("compositionupdate", function(e) {
        var data = e.data || "";
        // the caret is at the end of the composition (browsers do not report it)
        ccall("jasper_web_ime_composition", null, ["string", "number"], [clip(data), codepoints(data)]);
    });
    ta.addEventListener("compositionend", function(e) {
        state.composing = false;
        if (away()) { ta.value = ""; return; }
        var data = e.data || "";
        if (data.length > 0) ccall("jasper_web_text_input", null, ["string"], [clip(data)]);
        else ccall("jasper_web_ime_cancel", null, [], []);
        ta.value = "";
    });
    ta.addEventListener("beforeinput", function(e) {
        if (state.composing || e.isComposing || !state.softKey || away()) return;
        // keys a soft keyboard sends without a key code of their own
        if (e.inputType === "deleteContentBackward") ccall("jasper_web_soft_key", null, ["number"], [259]);
        else if (e.inputType === "insertLineBreak" || e.inputType === "insertParagraph") ccall("jasper_web_soft_key", null, ["number"], [257]);
    });
    ta.addEventListener("input", function(e) {
        if (away()) { ta.value = ""; return; }
        if (state.composing || e.isComposing) return;
        if (e.inputType === "insertText" && e.data) ccall("jasper_web_text_input", null, ["string"], [clip(e.data)]);
        ta.value = "";
    });
    // one listener for the textarea and the page: whatever has focus, the paste is the game's
    document.addEventListener("paste", function(e) {
        if (!e.clipboardData) return;
        var text = e.clipboardData.getData("text/plain") || "";
        if (document.activeElement === ta || document.activeElement === document.body || document.activeElement === Module.canvas) {
            e.preventDefault();
            ccall("jasper_web_paste", null, ["string"], [clip(text)]);
        }
    }, true);
});

EM_JS(void, jasper_ime_focus, (int focus), {
    var ta = document.getElementById("jasper-ime");
    if (!ta) return;
    if (focus) {
        if (document.activeElement !== ta) ta.focus({ preventScroll: true });
    } else if (document.activeElement === ta) {
        var state = Module.jasperIme;
        if (state) { state.composing = false; state.suppress = true; }
        try {
            ta.value = "";
            ta.blur();
        } finally {
            if (state) state.suppress = false;
        }
    }
});

EM_JS(int, jasper_ime_is_focused, (), {
    var ta = document.getElementById("jasper-ime");
    return (ta && document.activeElement === ta) ? 1 : 0;
});

EM_JS(void, jasper_ime_set_soft_key, (int soft), {
    if (Module.jasperIme) Module.jasperIme.softKey = soft !== 0;
});

// x, y, height in canvas pixels (the space of the mouse events)
EM_JS(void, jasper_ime_set_area, (float x, float y, float height), {
    var ta = document.getElementById("jasper-ime");
    var canvas = Module.canvas;
    if (!ta || !canvas) return;
    var rect = canvas.getBoundingClientRect();
    var sx = canvas.width > 0 ? rect.width / canvas.width : 1;
    var sy = canvas.height > 0 ? rect.height / canvas.height : 1;
    ta.style.left = (rect.left + x * sx) + "px";
    ta.style.top = (rect.top + y * sy) + "px";
    ta.style.height = Math.max(1, height * sy) + "px";
});

// navigator.clipboard needs a user activation (a key or a click is one) and a secure context;
// the fallback copies through a selected textarea
EM_JS(void, jasper_clipboard_write, (const char* utf8), {
    var text = UTF8ToString(utf8);
    var fallback = function() {
        var active = document.activeElement;
        var area = document.createElement("textarea");
        area.value = text;
        area.setAttribute("readonly", "");
        area.style.cssText = "position:fixed;left:-9999px;top:0;opacity:0;";
        document.body.appendChild(area);
        area.select();
        try { document.execCommand("copy"); } catch (err) {}
        document.body.removeChild(area);
        if (active && active.focus) active.focus({ preventScroll: true });
    };
    if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text).catch(fallback);
    } else {
        fallback();
    }
});

void JasperEngineWeb::rememberClipboardText(const std::string& text){
    static_cast<JasperEngineWeb&>(jasperengine::System::instance()).clipboardText = text;
}

void JasperEngineWeb::setClipboardText(const std::string& utf8){
    clipboardText = utf8;
    jasper_clipboard_write(utf8.c_str());
}

std::string JasperEngineWeb::getClipboardText(){
    // the browser clipboard is asynchronous: the text of the last copy or paste event
    return clipboardText;
}

void JasperEngineWeb::setTextInputArea(float x, float y, float width, float height){
    jasper_ime_set_area(x, y, height);
}

void JasperEngineWeb::showVirtualKeyboard(std::wstring text){
    jasper_ime_focus(1);
}

void JasperEngineWeb::hideVirtualKeyboard(){
    jasper_ime_focus(0);
}

JasperEngineWeb::JasperEngineWeb(){

}

void JasperEngineWeb::setEnabledIDB(bool enabledIDB){
    JasperEngineWeb::enabledIDB = enabledIDB;
    // the restore from IndexedDB is over, whether it worked or not
    JasperEngineWeb::loadedIDB = true;
}

bool JasperEngineWeb::isUserDataLoaded(){
    return loadedIDB;
}

int JasperEngineWeb::getScreenWidth(){
    return screenWidth;
}

int JasperEngineWeb::getScreenHeight(){
    return screenHeight;
}

int JasperEngineWeb::getSampleCount(){
    return sampleCount;
}

bool JasperEngineWeb::isTouchDevice(){
    // a finger is the primary pointer of phones and tablets, not of touch laptops
    return EM_ASM_INT({
        return (window.matchMedia && window.matchMedia('(pointer: coarse)').matches) ? 1 : 0;
    });
}

int JasperEngineWeb::init(int argc, char **argv){

    canvas = "#canvas";

    int sWidth = DEFAULT_WINDOW_WIDTH;
    int sHeight = DEFAULT_WINDOW_HEIGHT;
    if (argv[1] != NULL && argv[2] != NULL){
        sWidth = atoi(argv[1]);
        sHeight = atoi(argv[2]);
    }

    emscripten_set_keypress_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, true, key_callback);
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, true, key_callback);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, true, key_callback);
    emscripten_set_mousedown_callback(canvas.c_str(), 0, true, mouse_callback);
    emscripten_set_mouseup_callback(canvas.c_str(), 0, true, mouse_callback);
    emscripten_set_mousemove_callback(canvas.c_str(), 0, true, mouse_callback);
    emscripten_set_mouseenter_callback(canvas.c_str(), 0, true, mouse_callback);
    emscripten_set_mouseleave_callback(canvas.c_str(), 0, true, mouse_callback);
    emscripten_set_wheel_callback(canvas.c_str(), 0, true, wheel_callback);
    emscripten_set_touchstart_callback(canvas.c_str(), 0, true, touch_callback);
    emscripten_set_touchmove_callback(canvas.c_str(), 0, true, touch_callback);
    emscripten_set_touchend_callback(canvas.c_str(), 0, true, touch_callback);
    emscripten_set_touchcancel_callback(canvas.c_str(), 0, true, touch_callback);
    emscripten_set_webglcontextlost_callback(canvas.c_str(), 0, true, webgl_context_callback);
    emscripten_set_webglcontextrestored_callback(canvas.c_str(), 0, true, webgl_context_callback);
    emscripten_set_pointerlockchange_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, 0, true, pointerlock_callback);
    // not in the capture phase: the IME textarea's own focus and blur (which do not bubble) stay out
    emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, false, focus_callback);
    emscripten_set_focus_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, false, focus_callback);

    syncWaitTime = 0;
    enabledIDB = false;
    loadedIDB = false;
    EM_ASM(
		FS.mkdir('/datafs');
		FS.mount(IDBFS, {}, '/datafs');
		FS.syncfs(true, function(err) {
			ccall('syncfs_enable_callback', null, ['string'], [err ? err.message : ""]);
		});
	);

    jasperengine::Engine::systemInit(argc, argv, new JasperEngineWeb());

    jasper_ime_create();

    sampleCount = 4;

    EmscriptenWebGLContextAttributes attr;
    emscripten_webgl_init_context_attributes(&attr);
    attr.alpha = true;
    attr.depth = true;
    attr.stencil = true;
    attr.antialias = true;
    attr.preserveDrawingBuffer = false;
    attr.failIfMajorPerformanceCaveat = false;
    attr.enableExtensionsByDefault = true;
    attr.premultipliedAlpha = true;
    attr.majorVersion = 2;
    attr.minorVersion = 0;
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_create_context(canvas.c_str(), &attr);
    
    emscripten_webgl_make_context_current(ctx);

    JasperEngineWeb::screenWidth = sWidth;
    JasperEngineWeb::screenHeight = sHeight;
    jasperengine::Engine::systemViewLoaded();
    changeCanvasSize(sWidth, sHeight);

    emscripten_request_animation_frame_loop(renderLoop, 0);

    return 0;
}

void JasperEngineWeb::unregisterCallbacks(){
    emscripten_set_keypress_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, true, NULL);
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, true, NULL);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, true, NULL);
    emscripten_set_mousedown_callback(canvas.c_str(), 0, true, NULL);
    emscripten_set_mouseup_callback(canvas.c_str(), 0, true, NULL);
    emscripten_set_mousemove_callback(canvas.c_str(), 0, true, NULL);
    emscripten_set_mouseenter_callback(canvas.c_str(), 0, true, NULL);
    emscripten_set_mouseleave_callback(canvas.c_str(), 0, true, NULL);
    emscripten_set_wheel_callback(canvas.c_str(), 0, true, NULL);
    emscripten_set_touchstart_callback(canvas.c_str(), 0, true, NULL);
    emscripten_set_touchmove_callback(canvas.c_str(), 0, true, NULL);
    emscripten_set_touchend_callback(canvas.c_str(), 0, true, NULL);
    emscripten_set_touchcancel_callback(canvas.c_str(), 0, true, NULL);
    emscripten_set_webglcontextlost_callback(canvas.c_str(), 0, true, NULL);
    emscripten_set_webglcontextrestored_callback(canvas.c_str(), 0, true, NULL);
    emscripten_set_pointerlockchange_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, 0, true, NULL);
    emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, false, NULL);
    emscripten_set_focus_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, false, NULL);
}

void JasperEngineWeb::changeCanvasSize(int width, int height){
    emscripten_set_canvas_element_size(canvas.c_str(), width, height);

    JasperEngineWeb::screenWidth = width;
    JasperEngineWeb::screenHeight = height;
    jasperengine::Engine::systemViewChanged();
}

bool JasperEngineWeb::isFullscreen(){
    EmscriptenFullscreenChangeEvent fsce;
    emscripten_get_fullscreen_status(&fsce);

    return fsce.isFullscreen;
}

void JasperEngineWeb::requestFullscreen(){
    EmscriptenFullscreenStrategy strategy;
    strategy.scaleMode = EMSCRIPTEN_FULLSCREEN_SCALE_STRETCH;
    strategy.canvasResolutionScaleMode = EMSCRIPTEN_FULLSCREEN_CANVAS_SCALE_STDDEF;
    strategy.filteringMode = EMSCRIPTEN_FULLSCREEN_FILTERING_DEFAULT;
    strategy.canvasResizedCallback = canvas_resize;
    strategy.canvasResizedCallbackUserData = NULL;

    emscripten_request_fullscreen_strategy(canvas.c_str(), 1, &strategy);
}

void JasperEngineWeb::exitFullscreen(){
    emscripten_exit_fullscreen();
}

void JasperEngineWeb::setWindowTitle(const std::string& title){
    emscripten_set_window_title(title.c_str());
}

void JasperEngineWeb::quit(){
    quitRequested = true;
}

void JasperEngineWeb::setMouseCursor(jasperengine::CursorType type){
    std::string cursor;

    if (type == jasperengine::CursorType::ARROW){
        cursor = "default";
    }else if (type == jasperengine::CursorType::IBEAM){
        cursor = "text";
    }else if (type == jasperengine::CursorType::CROSSHAIR){
        cursor = "crosshair";
    }else if (type == jasperengine::CursorType::POINTING_HAND){
        cursor = "pointer";
    }else if (type == jasperengine::CursorType::RESIZE_EW){
        cursor = "ew-resize";
    }else if (type == jasperengine::CursorType::RESIZE_NS){
        cursor = "ns-resize";
    }else if (type == jasperengine::CursorType::RESIZE_NWSE){
        cursor = "nwse-resize";
    }else if (type == jasperengine::CursorType::RESIZE_NESW){
        cursor = "nesw-resize";
    }else if (type == jasperengine::CursorType::RESIZE_ALL){
        cursor = "all-scroll";
    }else if (type == jasperengine::CursorType::NOT_ALLOWED){
        cursor = "not-allowed";
    }else if (type == jasperengine::CursorType::WAIT){
        cursor = "wait";
    }else if (type == jasperengine::CursorType::PROGRESS){
        cursor = "progress";
    }else if (type == jasperengine::CursorType::HELP){
        cursor = "help";
    }else if (type == jasperengine::CursorType::GRAB){
        cursor = "grab";
    }else if (type == jasperengine::CursorType::GRABBING){
        cursor = "grabbing";
    }else{
        cursor = "auto";
    }

    EM_ASM({Module.canvas.style.cursor = UTF8ToString($0);}, cursor.c_str());

}

void JasperEngineWeb::setMouseMode(jasperengine::MouseMode mode){
    if (mode == jasperengine::MouseMode::CAPTURED){
        emscripten_request_pointerlock(canvas.c_str(), 1);
        return;
    }

    emscripten_exit_pointerlock();

    // The browser can't confine the pointer without pointer lock, so CONFINED
    // behaves as NORMAL (visible cursor).
    if (mode == jasperengine::MouseMode::HIDDEN){
        EM_ASM({Module.canvas.style.cursor = UTF8ToString($0);}, "none");
    }else{
        setMouseCursor(jasperengine::Engine::getMouseCursor());
    }
}

std::string JasperEngineWeb::getCrashReportPath(){
    // the page hooks (crash_pre.js) report a web crash; nothing is written
    return std::string();
}

std::string JasperEngineWeb::getUserDataPath(){
    return "/datafs";
}

bool JasperEngineWeb::syncFileSystem(){
    if (enabledIDB)
        syncWaitTime = 500;

    return true;
}

void JasperEngineWeb::syncFileSystemNow(){
    syncWaitTime = 0;

    EM_ASM(
        FS.syncfs(function(err) {
            ccall('syncfs_callback', null, ['string'], [err ? err.message : ""]);
        });
    );
}

void JasperEngineWeb::pollGamepads(){
    if (emscripten_sample_gamepad_data() != EMSCRIPTEN_RESULT_SUCCESS)
        return;

    int numGamepads = emscripten_get_num_gamepads();
    if (numGamepads > JASPERENGINE_WEB_MAX_GAMEPADS)
        numGamepads = JASPERENGINE_WEB_MAX_GAMEPADS;

    for (int i = 0; i < numGamepads; i++){
        GamepadState& state = gamepads[i];

        EmscriptenGamepadEvent ge;
        bool connected = (emscripten_get_gamepad_status(i, &ge) == EMSCRIPTEN_RESULT_SUCCESS) && ge.connected;

        if (connected && !state.connected){
            state = GamepadState();
            state.connected = true;
            // triggers rest at -1; seed so a resting trigger doesn't emit a
            // spurious axis move from 0 to -1 on connect
            state.triggers[0] = -1.0f;
            state.triggers[1] = -1.0f;
            jasperengine::Engine::systemGamepadConnect(i, ge.id);
        }else if (!connected && state.connected){
            state = GamepadState();
            jasperengine::Engine::systemGamepadDisconnect(i);
        }

        if (!connected)
            continue;

        bool standard = (strcmp(ge.mapping, "standard") == 0);

        int numButtons = (ge.numButtons < 64)? ge.numButtons : 64;
        for (int b = 0; b < numButtons; b++){
            // standard mapping: triggers are analog buttons 6 and 7; report them
            // as axes in [-1, 1] to match the GLFW gamepad convention
            if (standard && (b == 6 || b == 7)){
                int trigger = (b == 6)? 0 : 1;
                int axis = (b == 6)? D_GAMEPAD_AXIS_LEFT_TRIGGER : D_GAMEPAD_AXIS_RIGHT_TRIGGER;
                float value = (float)ge.analogButton[b] * 2.0f - 1.0f;
                if (fabsf(value - state.triggers[trigger]) > 0.001f){
                    state.triggers[trigger] = value;
                    jasperengine::Engine::systemGamepadAxisMove(i, axis, value);
                }
                continue;
            }

            bool pressed = ge.digitalButton[b];
            if (pressed != state.buttons[b]){
                state.buttons[b] = pressed;
                int button = (standard)? jasperengine_gamepad_button(b) : b;
                if (button >= 0){
                    if (pressed){
                        jasperengine::Engine::systemGamepadButtonDown(i, button);
                    }else{
                        jasperengine::Engine::systemGamepadButtonUp(i, button);
                    }
                }
            }
        }

        // standard mapping axes match D_GAMEPAD_AXIS_LEFT_X..RIGHT_Y order
        int numAxes = (ge.numAxes < 64)? ge.numAxes : 64;
        for (int a = 0; a < numAxes; a++){
            float value = (float)ge.axis[a];
            if (fabsf(value - state.axes[a]) > 0.001f){
                state.axes[a] = value;
                jasperengine::Engine::systemGamepadAxisMove(i, a, value);
            }
        }
    }
}

EM_BOOL JasperEngineWeb::renderLoop(double time, void* userdata){
    if (quitRequested) {
        // input events would reach destroyed scenes and a closed Lua state
        unregisterCallbacks();

        jasperengine::Engine::systemViewDestroyed();
        jasperengine::Engine::systemShutdown();

        // shutdown also writes files and this loop will not run again
        if (syncWaitTime > 0)
            syncFileSystemNow();

        return EM_FALSE;
    }

    pollGamepads();

    // CSS size is the input coordinate space as well as the visible surface.
    // Update the framebuffer before drawing; Engine applies the project's
    // Scaling mode (including intentional stretching) to this actual size.
    double cssWidth = 0, cssHeight = 0;
    if (emscripten_get_element_css_size(canvas.c_str(), &cssWidth, &cssHeight) == EMSCRIPTEN_RESULT_SUCCESS) {
        const int width = (int)round(cssWidth);
        const int height = (int)round(cssHeight);
        if (width > 0 && height > 0 && (width != screenWidth || height != screenHeight)) {
            changeCanvasSize(width, height);
        }
    }

    jasperengine::Engine::systemDraw();

    if (syncWaitTime > 0) {
		syncWaitTime -= (int)(jasperengine::Engine::getDeltatime()*1000);

        if (syncWaitTime <= 0){
            syncFileSystemNow();
        }
    }

    return EM_TRUE;
}

EM_BOOL JasperEngineWeb::canvas_resize(int eventType, const void *reserved, void *userData){
    int w, h;
    emscripten_get_canvas_element_size(canvas.c_str(), &w, &h);

    JasperEngineWeb::screenWidth = w;
    JasperEngineWeb::screenHeight = h;
    jasperengine::Engine::systemViewChanged();

    return 0;
}

wchar_t JasperEngineWeb::toCodepoint(const std::string &u){
    int l = u.length();
    if (l<1) return -1; unsigned char u0 = u[0]; if (u0>=0   && u0<=127) return u0;
    if (l<2) return -1; unsigned char u1 = u[1]; if (u0>=192 && u0<=223) return (u0-192)*64 + (u1-128);
    if (l<3) return -1; unsigned char u2 = u[2]; if (u0>=224 && u0<=239) return (u0-224)*4096 + (u1-128)*64 + (u2-128);
    if (l<4) return -1; unsigned char u3 = u[3]; if (u0>=240 && u0<=247) return (u0-240)*262144 + (u1-128)*4096 + (u2-128)*64 + (u3-128);
    return -1;
}

std::string JasperEngineWeb::toUTF8(wchar_t cp) {
    char ch[5] = {0x00};
    if(cp <= 0x7F) { 
        ch[0] = cp; 
    }
    else if(cp <= 0x7FF) { 
        ch[0] = (cp >> 6) + 192; 
        ch[1] = (cp & 63) + 128; 
    }
    else if(0xd800 <= cp && cp <= 0xdfff) {}
    else if(cp <= 0xFFFF) { 
        ch[0] = (cp >> 12) + 224; 
        ch[1]= ((cp >> 6) & 63) + 128; 
        ch[2]= (cp & 63) + 128; 
    }
    else if(cp <= 0x10FFFF) { 
        ch[0] = (cp >> 18) + 240; 
        ch[1] = ((cp >> 12) & 63) + 128; 
        ch[2] = ((cp >> 6) & 63) + 128; 
        ch[3]= (cp & 63) + 128; 
    }
    return std::string(ch);
}

EM_BOOL JasperEngineWeb::key_callback(int eventType, const EmscriptenKeyboardEvent *e, void *userData){
    std::string keyStorage;
    const char* key = e->key;
    if ((!key) || (!(*key))) {
        keyStorage = toUTF8(e->keyCode);
        key = keyStorage.c_str();
    }
    if (!(*key)) {
        keyStorage = toUTF8(e->which);
        key = keyStorage.c_str();
    }

    int modifiers = 0;
    if (e->ctrlKey) modifiers |= D_MODIFIER_CONTROL;
    if (e->shiftKey) modifiers |= D_MODIFIER_SHIFT;
    if (e->altKey)  modifiers |= D_MODIFIER_ALT;
    if (e->metaKey) modifiers |= D_MODIFIER_SUPER;

    int code = jasperengine_input(e->code);
    if (code==0){
        code = jasperengine_legacy_input(e->which);
    }
    if (code==0){
        code = jasperengine_legacy_input(e->keyCode);
    }

    int skey=0;
    if ((!strcmp(key,"Tab"))||(*key=='\t')) skey=1;
    if ((!strcmp(key,"Backspace"))||(*key=='\b')) skey=2;
    if ((!strcmp(key,"Enter"))||(*key=='\r')) skey=4;
    if ((!strcmp(key,"Escape"))||(*key=='\x1b')) skey=8;

    // keys of an IME composition (229) belong to it; a soft keyboard sends them for every key
    // and its text comes through the IME textarea
    if (eventType == EMSCRIPTEN_EVENT_KEYDOWN)
        jasper_ime_set_soft_key(e->keyCode == 229);
    // Chrome gives the real key code to the up of a key whose down was 229: that up is the
    // composition's too, so the engine never sees an up without its down
    static std::set<int> imeKeys;
    if (eventType == EMSCRIPTEN_EVENT_KEYDOWN && e->keyCode == 229){
        if (code != 0)
            imeKeys.insert(code);
        return 0;
    }
    if (eventType == EMSCRIPTEN_EVENT_KEYUP && (e->keyCode == 229 || imeKeys.erase(code) > 0))
        return 0;

    // Ctrl/Cmd+V while a document text field has focus is left to the browser, whose paste
    // event brings the text: RmlUi would ask for the clipboard synchronously, which the
    // asynchronous browser clipboard cannot answer
    static bool pasteKeyDown = false;
    if (code == D_KEY_V && (e->ctrlKey || e->metaKey)){
        if (eventType == EMSCRIPTEN_EVENT_KEYDOWN && jasperengine::Engine::wantsTextComposition()){
            pasteKeyDown = true;
            return 0;
        }
        if (eventType == EMSCRIPTEN_EVENT_KEYUP && pasteKeyDown){
            pasteKeyDown = false;
            return 0;
        }
    }

    if (eventType == EMSCRIPTEN_EVENT_KEYDOWN){
        jasperengine::Engine::systemKeyDown(code, e->repeat, modifiers);
        if (skey==1) jasperengine::Engine::systemCharInput('\t');
        if (skey==2) jasperengine::Engine::systemCharInput('\b');
        if (skey==4) jasperengine::Engine::systemCharInput('\r');
        if (skey==8) jasperengine::Engine::systemCharInput('\x1b');
        // Tab would move the page focus off the IME textarea; documents move their own focus
        if (skey==1 && jasper_ime_is_focused())
            return 1;

    }else if (eventType == EMSCRIPTEN_EVENT_KEYUP){
        jasperengine::Engine::systemKeyUp(code, e->repeat, modifiers);

    }else if (eventType == EMSCRIPTEN_EVENT_KEYPRESS){
        // the IME textarea's input events bring this text
        if (jasper_ime_is_focused())
            return 0;

        wchar_t cp = toCodepoint(std::string(e->key));
        if (cp == 0) cp = e->which;
        if (cp == 0) cp = e->keyCode;

        if (skey == 0 && cp != 0){
            jasperengine::Engine::systemCharInput(cp);
        }
    }

    return 0;
}

EM_BOOL JasperEngineWeb::mouse_callback(int eventType, const EmscriptenMouseEvent *e, void *userData) {

    int width = JasperEngineWeb::screenWidth;
    int height = JasperEngineWeb::screenHeight;
    EmscriptenPointerlockChangeEvent pointerLockStatus = {};
    emscripten_get_pointerlock_status(&pointerLockStatus);
    bool mouseLocked = pointerLockStatus.isActive;

    if (!mouseLocked){
        if (e->targetX < 0) return 0;
        if (e->targetY < 0) return 0;
        if (e->targetX > width) return 0;
        if (e->targetY > height) return 0;

        mousePosX = e->targetX;
        mousePosY = e->targetY;
    }else{
        mousePosX += e->movementX;
        mousePosY += e->movementY;
    }

    int modifiers = 0;
    if (e->ctrlKey) modifiers |= D_MODIFIER_CONTROL;
    if (e->shiftKey) modifiers |= D_MODIFIER_SHIFT;
    if (e->altKey) modifiers |= D_MODIFIER_ALT;
    if (e->metaKey) modifiers |= D_MODIFIER_SUPER;

    jasperengine::Engine::systemMouseMove((float)mousePosX, (float)mousePosY, modifiers);
    // a capture the browser ended (Esc, the tab hidden) locks again at the next click
    if (eventType == EMSCRIPTEN_EVENT_MOUSEDOWN && !mouseLocked &&
        jasperengine::Engine::getMouseMode() == jasperengine::MouseMode::CAPTURED){
        emscripten_request_pointerlock(canvas.c_str(), 0);
    }
    if (eventType == EMSCRIPTEN_EVENT_MOUSEDOWN && e->buttons != 0){
        jasperengine::Engine::systemMouseDown(jasperengine_mouse_button(e->button), (float)mousePosX, (float)mousePosY, modifiers);
        // a press on a document text field: the textarea keeps the focus the press would move
        if (jasperengine::Engine::wantsTextComposition()){
            jasper_ime_focus(1);
            return 1;
        }
    }
    if (eventType == EMSCRIPTEN_EVENT_MOUSEUP){
        jasperengine::Engine::systemMouseUp(jasperengine_mouse_button(e->button), (float)mousePosX, (float)mousePosY, modifiers);
    }
    if (eventType == EMSCRIPTEN_EVENT_MOUSEENTER){
        jasperengine::Engine::systemMouseEnter();
    }
    if (eventType == EMSCRIPTEN_EVENT_MOUSELEAVE){
        jasperengine::Engine::systemMouseLeave();
    }

    return 0;
}

// Input::getPointerLockState: PENDING from setMouseMode(CAPTURED) until the browser locks
EM_BOOL JasperEngineWeb::pointerlock_callback(int eventType, const EmscriptenPointerlockChangeEvent* e, void* userData) {
    jasperengine::Engine::systemPointerLockChanged(e->isActive);
    return 0;
}

// the page lost the keyboard (another tab, window or app): held keys and buttons come up
EM_BOOL JasperEngineWeb::focus_callback(int eventType, const EmscriptenFocusEvent* e, void* userData) {
    jasperengine::Engine::systemFocusChanged(eventType == EMSCRIPTEN_EVENT_FOCUS);
    return 0;
}

EM_BOOL JasperEngineWeb::wheel_callback(int eventType, const EmscriptenWheelEvent *e, void *userData) {
    float scale;
    switch (e->deltaMode) {
        case DOM_DELTA_PIXEL: scale = -0.04f; break;
        case DOM_DELTA_LINE:  scale = -1.33f; break;
        case DOM_DELTA_PAGE:  scale = -10.0f; break;
        default:              scale = -0.1f; break;
    }

    int modifiers = 0;
    if (e->mouse.ctrlKey) modifiers |= D_MODIFIER_CONTROL;
    if (e->mouse.shiftKey) modifiers |= D_MODIFIER_SHIFT;
    if (e->mouse.altKey) modifiers |= D_MODIFIER_ALT;
    if (e->mouse.metaKey) modifiers |= D_MODIFIER_SUPER;

    jasperengine::Engine::systemMouseScroll(scale * (float)e->deltaX, scale * (float)e->deltaY, modifiers);

  return 0;
}

EM_BOOL JasperEngineWeb::touch_callback(int emsc_type, const EmscriptenTouchEvent* emsc_event, void* user_data) {
    bool retval = true;

    // Emscripten lists every finger. isChanged is the one this event is about.
    // Ending a finger that is still held releases its button, so a jump tap stops a run.
    // Cancel is not a release: pointer-up can click the control under the finger.
    switch (emsc_type) {
        case EMSCRIPTEN_EVENT_TOUCHSTART:
            for (int i = 0; i < emsc_event->numTouches; i++) {
                const EmscriptenTouchPoint* src = &emsc_event->touches[i];
                if (!src->isChanged) continue;
                jasperengine::Engine::systemTouchStart((int)src->identifier, src->targetX, src->targetY);
            }
            break;
        case EMSCRIPTEN_EVENT_TOUCHMOVE:
            for (int i = 0; i < emsc_event->numTouches; i++) {
                const EmscriptenTouchPoint* src = &emsc_event->touches[i];
                if (!src->isChanged) continue;
                jasperengine::Engine::systemTouchMove((int)src->identifier, src->targetX, src->targetY);
            }
            break;
        case EMSCRIPTEN_EVENT_TOUCHEND:
            for (int i = 0; i < emsc_event->numTouches; i++) {
                const EmscriptenTouchPoint* src = &emsc_event->touches[i];
                if (!src->isChanged) continue;
                jasperengine::Engine::systemTouchEnd((int)src->identifier, src->targetX, src->targetY);
            }
            // touchend is a user activation, touchstart is not: the mobile keyboard opens
            // only for a focus made here
            if (jasperengine::Engine::wantsTextComposition())
                jasper_ime_focus(1);
            break;
        case EMSCRIPTEN_EVENT_TOUCHCANCEL: {
            bool anyHeld = false;
            for (int i = 0; i < emsc_event->numTouches; i++) {
                if (!emsc_event->touches[i].isChanged) {
                    anyHeld = true;
                    break;
                }
            }
            if (!anyHeld) {
                // Clear all touches before notifying listeners of each cancellation.
                jasperengine::Engine::systemTouchCancel();
            } else {
                for (int i = 0; i < emsc_event->numTouches; i++) {
                    const EmscriptenTouchPoint* src = &emsc_event->touches[i];
                    if (src->isChanged)
                        jasperengine::Engine::systemTouchCancel((int)src->identifier);
                }
            }
            break;
        }
        default:
            retval = false;
            break;
    }

  return retval;
}

EM_BOOL JasperEngineWeb::webgl_context_callback(int emsc_type, const void* reserved, void* user_data) {
    switch (emsc_type) {
        case EMSCRIPTEN_EVENT_WEBGLCONTEXTLOST:     jasperengine::Engine::systemPause(); break;
        case EMSCRIPTEN_EVENT_WEBGLCONTEXTRESTORED: jasperengine::Engine::systemResume(); break;
        default:                                    break;
    }

    return 0;
}

int JasperEngineWeb::jasperengine_mouse_button(int button){
    if (button == 0) return D_MOUSE_BUTTON_1;
    if (button == 2) return D_MOUSE_BUTTON_2;
    if (button == 1) return D_MOUSE_BUTTON_3;
    if (button == 3) return D_MOUSE_BUTTON_4;
    if (button == 4) return D_MOUSE_BUTTON_5;
    if (button == 5) return D_MOUSE_BUTTON_6;
    if (button == 6) return D_MOUSE_BUTTON_7;
    if (button == 7) return D_MOUSE_BUTTON_8;

    return -1;
}

//W3C standard gamepad mapping (buttons 6 and 7 are handled as trigger axes)
int JasperEngineWeb::jasperengine_gamepad_button(int button){
    if (button == 0) return D_GAMEPAD_BUTTON_A;
    if (button == 1) return D_GAMEPAD_BUTTON_B;
    if (button == 2) return D_GAMEPAD_BUTTON_X;
    if (button == 3) return D_GAMEPAD_BUTTON_Y;
    if (button == 4) return D_GAMEPAD_BUTTON_LEFT_BUMPER;
    if (button == 5) return D_GAMEPAD_BUTTON_RIGHT_BUMPER;
    if (button == 8) return D_GAMEPAD_BUTTON_BACK;
    if (button == 9) return D_GAMEPAD_BUTTON_START;
    if (button == 10) return D_GAMEPAD_BUTTON_LEFT_THUMB;
    if (button == 11) return D_GAMEPAD_BUTTON_RIGHT_THUMB;
    if (button == 12) return D_GAMEPAD_BUTTON_DPAD_UP;
    if (button == 13) return D_GAMEPAD_BUTTON_DPAD_DOWN;
    if (button == 14) return D_GAMEPAD_BUTTON_DPAD_LEFT;
    if (button == 15) return D_GAMEPAD_BUTTON_DPAD_RIGHT;
    if (button == 16) return D_GAMEPAD_BUTTON_GUIDE;

    return -1;
}

int JasperEngineWeb::jasperengine_input(const char code[32]){
    if (!strcmp(code,"Space")) return D_KEY_SPACE;
    if (!strcmp(code,"Quote")) return D_KEY_APOSTROPHE;  /* ' */
    if (!strcmp(code,"Comma")) return D_KEY_COMMA;  /* , */
    if (!strcmp(code,"Minus")) return D_KEY_MINUS;  /* - */
    if (!strcmp(code,"Period")) return D_KEY_PERIOD;  /* . */
    if (!strcmp(code,"Slash")) return D_KEY_SLASH;  /* / */
    if (!strcmp(code,"Digit0")) return D_KEY_0;
    if (!strcmp(code,"Digit1")) return D_KEY_1;
    if (!strcmp(code,"Digit2")) return D_KEY_2;
    if (!strcmp(code,"Digit3")) return D_KEY_3;
    if (!strcmp(code,"Digit4")) return D_KEY_4;
    if (!strcmp(code,"Digit5")) return D_KEY_5;
    if (!strcmp(code,"Digit6")) return D_KEY_6;
    if (!strcmp(code,"Digit7")) return D_KEY_7;
    if (!strcmp(code,"Digit8")) return D_KEY_8;
    if (!strcmp(code,"Digit9")) return D_KEY_9;
    if (!strcmp(code,"Semicolon")) return D_KEY_SEMICOLON; /* ; */
    if (!strcmp(code,"Equal")) return D_KEY_EQUAL;  /* = */
    if (!strcmp(code,"KeyA")) return D_KEY_A;
    if (!strcmp(code,"KeyB")) return D_KEY_B;
    if (!strcmp(code,"KeyC")) return D_KEY_C;
    if (!strcmp(code,"KeyD")) return D_KEY_D;
    if (!strcmp(code,"KeyE")) return D_KEY_E;
    if (!strcmp(code,"KeyF")) return D_KEY_F;
    if (!strcmp(code,"KeyG")) return D_KEY_G;
    if (!strcmp(code,"KeyH")) return D_KEY_H;
    if (!strcmp(code,"KeyI")) return D_KEY_I;
    if (!strcmp(code,"KeyJ")) return D_KEY_J;
    if (!strcmp(code,"KeyK")) return D_KEY_K;
    if (!strcmp(code,"KeyL")) return D_KEY_L;
    if (!strcmp(code,"KeyM")) return D_KEY_M;
    if (!strcmp(code,"KeyN")) return D_KEY_N;
    if (!strcmp(code,"KeyO")) return D_KEY_O;
    if (!strcmp(code,"KeyP")) return D_KEY_P;
    if (!strcmp(code,"KeyQ")) return D_KEY_Q;
    if (!strcmp(code,"KeyR")) return D_KEY_R;
    if (!strcmp(code,"KeyS")) return D_KEY_S;
    if (!strcmp(code,"KeyT")) return D_KEY_T;
    if (!strcmp(code,"KeyU")) return D_KEY_U;
    if (!strcmp(code,"KeyV")) return D_KEY_V;
    if (!strcmp(code,"KeyW")) return D_KEY_W;
    if (!strcmp(code,"KeyX")) return D_KEY_X;
    if (!strcmp(code,"KeyY")) return D_KEY_Y;
    if (!strcmp(code,"KeyZ")) return D_KEY_Z;
    if (!strcmp(code,"BracketLeft")) return D_KEY_LEFT_BRACKET; /* [ */
    if (!strcmp(code,"Backslash")) return D_KEY_BACKSLASH;  /* \ */
    if (!strcmp(code,"BracketRight")) return D_KEY_RIGHT_BRACKET;  /* ] */
    if (!strcmp(code,"Backquote")) return D_KEY_GRAVE_ACCENT; /* ` */

    if (!strcmp(code,"Escape")) return D_KEY_ESCAPE;
    if (!strcmp(code,"Enter")) return D_KEY_ENTER;
    if (!strcmp(code,"Tab")) return D_KEY_TAB;
    if (!strcmp(code,"Backspace")) return D_KEY_BACKSPACE;
    if (!strcmp(code,"Insert")) return D_KEY_INSERT;
    if (!strcmp(code,"Delete")) return D_KEY_DELETE;
    if (!strcmp(code,"ArrowRight")) return D_KEY_RIGHT;
    if (!strcmp(code,"ArrowLeft")) return D_KEY_LEFT;
    if (!strcmp(code,"ArrowDown")) return D_KEY_DOWN;
    if (!strcmp(code,"ArrowUp")) return D_KEY_UP;
    if (!strcmp(code,"PageUp")) return D_KEY_PAGE_UP;
    if (!strcmp(code,"PageDown")) return D_KEY_PAGE_DOWN;
    if (!strcmp(code,"Home")) return D_KEY_HOME;
    if (!strcmp(code,"End")) return D_KEY_END;
    if (!strcmp(code,"CapsLock")) return D_KEY_CAPS_LOCK;
    if (!strcmp(code,"ScrollLock")) return D_KEY_SCROLL_LOCK;
    if (!strcmp(code,"NumLock")) return D_KEY_NUM_LOCK;
    if (!strcmp(code,"PrintScreen")) return D_KEY_PRINT_SCREEN;
    if (!strcmp(code,"Pause")) return D_KEY_PAUSE;
    if (!strcmp(code,"F1")) return D_KEY_F1;
    if (!strcmp(code,"F2")) return D_KEY_F2;
    if (!strcmp(code,"F3")) return D_KEY_F3;
    if (!strcmp(code,"F4")) return D_KEY_F4;
    if (!strcmp(code,"F5")) return D_KEY_F5;
    if (!strcmp(code,"F6")) return D_KEY_F6;
    if (!strcmp(code,"F7")) return D_KEY_F7;
    if (!strcmp(code,"F8")) return D_KEY_F8;
    if (!strcmp(code,"F9")) return D_KEY_F9;
    if (!strcmp(code,"F10")) return D_KEY_F10;
    if (!strcmp(code,"F11")) return D_KEY_F11;
    if (!strcmp(code,"F12")) return D_KEY_F12;
    if (!strcmp(code,"Numpad0")) return D_KEY_KP_0;
    if (!strcmp(code,"Numpad1")) return D_KEY_KP_1;
    if (!strcmp(code,"Numpad2")) return D_KEY_KP_2;
    if (!strcmp(code,"Numpad3")) return D_KEY_KP_3;
    if (!strcmp(code,"Numpad4")) return D_KEY_KP_4;
    if (!strcmp(code,"Numpad5")) return D_KEY_KP_5;
    if (!strcmp(code,"Numpad6")) return D_KEY_KP_6;
    if (!strcmp(code,"Numpad7")) return D_KEY_KP_7;
    if (!strcmp(code,"Numpad8")) return D_KEY_KP_8;
    if (!strcmp(code,"Numpad9")) return D_KEY_KP_9;
    if (!strcmp(code,"NumpadDecimal")) return D_KEY_KP_DECIMAL;
    if (!strcmp(code,"NumpadDivide")) return D_KEY_KP_DIVIDE;
    if (!strcmp(code,"NumpadMultiply")) return D_KEY_KP_MULTIPLY;
    if (!strcmp(code,"NumpadSubtract")) return D_KEY_KP_SUBTRACT;
    if (!strcmp(code,"NumpadAdd")) return D_KEY_KP_ADD;
    if (!strcmp(code,"NumpadEnter")) return D_KEY_KP_ENTER;
    if (!strcmp(code,"NumpadEqual")) return D_KEY_KP_EQUAL;
    if (!strcmp(code,"ShiftLeft")) return D_KEY_LEFT_SHIFT;
    if (!strcmp(code,"ControlLeft")) return D_KEY_LEFT_CONTROL;
    if (!strcmp(code,"AltLeft")) return D_KEY_LEFT_ALT;
    if (!strcmp(code,"OSLeft")) return D_KEY_LEFT_SUPER;
    if (!strcmp(code,"ShiftRight")) return D_KEY_RIGHT_SHIFT;
    if (!strcmp(code,"ControlRight")) return D_KEY_RIGHT_CONTROL;
    if (!strcmp(code,"AltRight")) return D_KEY_RIGHT_ALT;
    if (!strcmp(code,"OSRight")) return D_KEY_RIGHT_SUPER;
    if (!strcmp(code,"Menu")) return D_KEY_MENU;


    return 0;
}

int JasperEngineWeb::jasperengine_legacy_input(int code){
    if (code==8) return D_KEY_DELETE;
    if (code==9) return D_KEY_TAB;
    if (code==13) return D_KEY_ENTER;
    if (code==16) return D_KEY_LEFT_SHIFT;
    if (code==17) return D_KEY_LEFT_CONTROL;
    if (code==18) return D_KEY_LEFT_ALT;
    if (code==19) return D_KEY_PAUSE;
    if (code==20) return D_KEY_CAPS_LOCK;
    if (code==27) return D_KEY_ESCAPE;
    if (code==32) return D_KEY_SPACE;
    if (code==33) return D_KEY_PAGE_UP;
    if (code==34) return D_KEY_PAGE_DOWN;
    if (code==35) return D_KEY_END;
    if (code==36) return D_KEY_HOME;
    if (code==37) return D_KEY_LEFT;
    if (code==38) return D_KEY_UP;
    if (code==39) return D_KEY_RIGHT;
    if (code==40) return D_KEY_DOWN;
    if (code==45) return D_KEY_INSERT;
    if (code==46) return D_KEY_DELETE;
    if (code==48) return D_KEY_0;
    if (code==49) return D_KEY_1;
    if (code==50) return D_KEY_2;
    if (code==51) return D_KEY_3;
    if (code==52) return D_KEY_4;
    if (code==53) return D_KEY_5;
    if (code==54) return D_KEY_6;
    if (code==55) return D_KEY_7;
    if (code==56) return D_KEY_8;
    if (code==57) return D_KEY_9;

    if (code==65) return D_KEY_A;
    if (code==66) return D_KEY_B;
    if (code==67) return D_KEY_C;
    if (code==68) return D_KEY_D;
    if (code==69) return D_KEY_E;
    if (code==70) return D_KEY_F;
    if (code==71) return D_KEY_G;
    if (code==72) return D_KEY_H;
    if (code==73) return D_KEY_I;
    if (code==74) return D_KEY_J;
    if (code==75) return D_KEY_K;
    if (code==76) return D_KEY_L;
    if (code==77) return D_KEY_M;
    if (code==78) return D_KEY_N;
    if (code==79) return D_KEY_O;
    if (code==80) return D_KEY_P;
    if (code==81) return D_KEY_Q;
    if (code==82) return D_KEY_R;
    if (code==83) return D_KEY_S;
    if (code==84) return D_KEY_T;
    if (code==85) return D_KEY_U;
    if (code==86) return D_KEY_V;
    if (code==87) return D_KEY_W;
    if (code==88) return D_KEY_X;
    if (code==89) return D_KEY_Y;
    if (code==90) return D_KEY_Z;

    if (code==91) return D_KEY_LEFT_SUPER;
    if (code==92) return D_KEY_RIGHT_SUPER;
    if (code==93) return 0; //SELECT key
    if (code==96) return D_KEY_KP_0;
    if (code==97) return D_KEY_KP_1;
    if (code==98) return D_KEY_KP_2;
    if (code==99) return D_KEY_KP_3;
    if (code==100) return D_KEY_KP_4;
    if (code==101) return D_KEY_KP_5;
    if (code==102) return D_KEY_KP_6;
    if (code==103) return D_KEY_KP_7;
    if (code==104) return D_KEY_KP_8;
    if (code==105) return D_KEY_KP_9;
    if (code==106) return D_KEY_KP_MULTIPLY;
    if (code==107) return D_KEY_KP_ADD;
    if (code==109) return D_KEY_KP_SUBTRACT;
    if (code==110) return D_KEY_KP_DECIMAL;
    if (code==111) return D_KEY_KP_DIVIDE;
    if (code==112) return D_KEY_F1;
    if (code==113) return D_KEY_F2;
    if (code==114) return D_KEY_F3;
    if (code==115) return D_KEY_F4;
    if (code==116) return D_KEY_F5;
    if (code==117) return D_KEY_F6;
    if (code==118) return D_KEY_F7;
    if (code==119) return D_KEY_F8;
    if (code==120) return D_KEY_F9;
    if (code==121) return D_KEY_F10;
    if (code==122) return D_KEY_F11;
    if (code==123) return D_KEY_F12;
    if (code==144) return D_KEY_NUM_LOCK;
    if (code==145) return D_KEY_SCROLL_LOCK;
    if (code==186) return D_KEY_SEMICOLON;
    if (code==187) return D_KEY_EQUAL;
    if (code==188) return D_KEY_COMMA;
    if (code==189) return D_KEY_MINUS;
    if (code==190) return D_KEY_PERIOD;
    if (code==191) return D_KEY_SLASH;
    if (code==192) return D_KEY_GRAVE_ACCENT;
    if (code==219) return D_KEY_LEFT_BRACKET;
    if (code==220) return D_KEY_BACKSLASH;
    if (code==222) return D_KEY_APOSTROPHE;

    return 0;
}

// Pages of one site share localStorage, so each game prefixes its keys. The bare keys
// of older builds are still read, so their saves carry over.
std::string JasperEngineWeb::getStringForKey(const char *key, const std::string& defaultValue){
    char* value = (char*)EM_ASM_INT({
        var key = UTF8ToString($0);
        var val = localStorage.getItem(UTF8ToString($1) + '/' + key);
        if (val === null) val = localStorage.getItem(key);
        if (val) {
            var len = lengthBytesUTF8(val)+1;
            var buf = _malloc(len);
            stringToUTF8(val, buf, len);
            return buf;
        }
        return null;
    }, key, JASPERENGINE_STORAGE_ID);

    if (value){
        std::string strValue = value;
        free(value); //Each call to _malloc() must be paired with free() or heap memory will leak
        return strValue;
    }

    return defaultValue;
}

void JasperEngineWeb::setStringForKey(const char* key, const std::string& value){
    EM_ASM_ARGS({
        var key = UTF8ToString($0);
        var value = UTF8ToString($1);
        localStorage.setItem(UTF8ToString($2) + '/' + key, value);
    }, key, value.c_str(), JASPERENGINE_STORAGE_ID);
}

void JasperEngineWeb::removeKey(const char *key){
    // A bare key may be another game's, so an empty value hides it instead
    EM_ASM_ARGS({
        var key = UTF8ToString($0);
        var own = UTF8ToString($1) + '/' + key;
        if (localStorage.getItem(key) === null) localStorage.removeItem(own);
        else localStorage.setItem(own, "");
    }, key, JASPERENGINE_STORAGE_ID);
}

void JasperEngineWeb::initializeCrazyGamesSDK(){
    EM_ASM(
        function loadJS(FILE_URL, async = true) {
            let scriptEle = document.createElement("script");

            scriptEle.setAttribute("src", FILE_URL);
            scriptEle.setAttribute("type", "text/javascript");
            scriptEle.setAttribute("async", async);

            document.body.appendChild(scriptEle);

            scriptEle.addEventListener("error", (ev) => {
                console.log("Error on loading file", ev);
            });
        }

        loadJS("https://sdk.crazygames.com/crazygames-sdk-v2.js", false);
    );
}

void JasperEngineWeb::showCrazyGamesAd(const std::string& type){
    EM_ASM({
        var adtype = UTF8ToString($0);
        const callbacks = ({
            adFinished: () => ccall('crazygamesad_finished_callback', null),
            adError: (error) => ccall('crazygamesad_error_callback', null, ['string'], [error ? error.message : ""]),
            adStarted: () => ccall('crazygamesad_started_callback', null)
        });
        window.CrazyGames.SDK.ad.requestAd(adtype, callbacks);
    }, type.c_str());
}

void JasperEngineWeb::happytimeCrazyGames(){
    EM_ASM(
        window.CrazyGames.SDK.game.happytime();
    );
}

void JasperEngineWeb::gameplayStartCrazyGames(){
    EM_ASM(
        window.CrazyGames.SDK.game.gameplayStart();
    );
}

void JasperEngineWeb::gameplayStopCrazyGames(){
    EM_ASM(
        window.CrazyGames.SDK.game.gameplayStop();
    );
}

void JasperEngineWeb::loadingStartCrazyGames(){
    EM_ASM(
        window.CrazyGames.SDK.game.sdkGameLoadingStart();
    );
}

void JasperEngineWeb::loadingStopCrazyGames(){
    EM_ASM(
        window.CrazyGames.SDK.game.sdkGameLoadingStop();
    );
}
