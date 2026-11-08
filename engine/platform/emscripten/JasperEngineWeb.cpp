// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "JasperEngineWeb.h"
#include "WebKeyMap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <set>

#include <emscripten/emscripten.h>
#include <emscripten/heap.h>

#include "Engine.h"
#include "device/Device.h"
#include "Input.h"
#include "Screen.h"
#include "System.h"
#include "Log.h"
#include "PlatformConfig.h"
#include "player/RunControl.h"
#include "util/FrameLimiter.h"
#include "util/PlatformRules.h"

#include <algorithm>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wmissing-braces"
#endif

std::string JasperEngineWeb::canvas;

int JasperEngineWeb::syncWaitTime;
bool JasperEngineWeb::enabledIDB;
bool JasperEngineWeb::loadedIDB;
bool JasperEngineWeb::projectStarted = false;
double JasperEngineWeb::startTime = 0.0;

int JasperEngineWeb::screenWidth;
int JasperEngineWeb::screenHeight;

double JasperEngineWeb::mousePosX;
double JasperEngineWeb::mousePosY;

int JasperEngineWeb::sampleCount = 0;

bool JasperEngineWeb::quitRequested = false;

bool JasperEngineWeb::pageHidden = false;
bool JasperEngineWeb::adPlaying = false;
bool JasperEngineWeb::contextLost = false;
bool JasperEngineWeb::windowFocused = true;
double JasperEngineWeb::lastFrameTime = 0.0;
std::vector<double> JasperEngineWeb::frameIntervals;
float JasperEngineWeb::measuredRefreshRate = 0.0f;

namespace {
    // Engine.targetFrameRate: animation frames that are not due draw nothing
    jasperengine::FrameLimiter frameLimiter;

    // S41 R16: while a Pixel Perfect Camera or an integer Scaling draws, the canvas backing store
    // is its CSS size times devicePixelRatio and the browser never resamples it (pixelated): the
    // integer scale is over physical pixels. Input arrives in CSS pixels and is scaled by it.
    double webCanvasPixelRatio = 1.0;
    bool webCanvasPixelated = false;
    // a canvas the page does not size (its CSS size follows its width and height attributes, as
    // the player's shell has it) gets its CSS size pinned while the backing store is larger:
    // otherwise every frame would grow it by the ratio again
    bool webCanvasCssPinned = false;
    double webCanvasPinnedWidth = 0.0, webCanvasPinnedHeight = 0.0;

    bool pixelArtView(){
        const jasperengine::Scaling scaling = jasperengine::Engine::getScalingMode();
        return jasperengine::Engine::getPixelPerfectView().active || scaling == jasperengine::Scaling::INTEGER ||
               scaling == jasperengine::Scaling::INTEGER_EXPAND;
    }

    // a string a JavaScript snippet made with _malloc (null: none)
    std::string takeJsString(char* value){
        if (!value) return std::string();
        std::string out = value;
        free(value);
        return out;
    }
}

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
        // the game started after the 3 s wait: data:// writes were refused until now (FileUtils),
        // so the restore replaced nothing the game wrote; what it read before is older
        if (JasperEngineWeb::hasProjectStarted())
            jasperengine::Log::warn("user data restored from IndexedDB after the game started; data:// files can be written from now on");
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
        JasperEngineWeb::setAdPlaying(true);
    }

    EMSCRIPTEN_KEEPALIVE 
    void crazygamesad_finished_callback() {
        JasperEngineWeb::setAdPlaying(false);
    }

    EMSCRIPTEN_KEEPALIVE 
    void crazygamesad_error_callback(const char* err) {
	    if (!err || err[0]) {
		    jasperengine::Log::error("Failed to load CrazyGames ad: %s", err);
	    }
        JasperEngineWeb::setAdPlaying(false);
    }

    // screen.orientation's change event: a half turn changes no canvas size
    EMSCRIPTEN_KEEPALIVE
    void jasper_web_orientation_changed() {
        jasperengine::Engine::systemOrientationChanged();
    }

    // the page's env(safe-area-inset-*) over the canvas, CSS pixels of the canvas (the engine's
    // screen on the web is the canvas's CSS size): jasper_safe_area_read, on a change only
    EMSCRIPTEN_KEEPALIVE
    void jasper_web_safe_area(float left, float top, float right, float bottom) {
        jasperengine::Screen::setPlatformSafeArea(jasperengine::EdgeInsets(left, top, right, bottom));
    }
}

// Screen.safeArea on the web: a hidden fixed <div id="jasper-safe-area"> padded by the page's
// env(safe-area-inset-*) (non-zero with viewport-fit=cover on phones with a notch) gives the
// viewport's safe rectangle; intersected with the canvas, it becomes canvas-local insets.
// Read again on resize, orientationchange and visualViewport resize, and with each canvas size.
EM_JS(void, jasper_safe_area_create, (), {
    if (document.getElementById('jasper-safe-area')) return;
    var probe = document.createElement('div');
    probe.id = 'jasper-safe-area';
    probe.setAttribute('aria-hidden', 'true');
    probe.style.cssText = 'position:fixed;left:0;top:0;width:0;height:0;visibility:hidden;pointer-events:none;' +
        'padding:env(safe-area-inset-top,0px) env(safe-area-inset-right,0px) env(safe-area-inset-bottom,0px) env(safe-area-inset-left,0px);';
    (document.body || document.documentElement).appendChild(probe);
    Module.jasperSafeArea = null;
    Module.jasperReadSafeArea = function() {
        var canvas = Module.canvas;
        if (!canvas || !probe.isConnected) return;
        var style = getComputedStyle(probe);
        var pt = parseFloat(style.paddingTop) || 0, pr = parseFloat(style.paddingRight) || 0;
        var pb = parseFloat(style.paddingBottom) || 0, pl = parseFloat(style.paddingLeft) || 0;
        var vw = document.documentElement.clientWidth || window.innerWidth;
        var vh = document.documentElement.clientHeight || window.innerHeight;
        var rect = canvas.getBoundingClientRect();
        var l = Math.max(0, pl - rect.left), t = Math.max(0, pt - rect.top);
        var r = Math.max(0, rect.right - (vw - pr)), b = Math.max(0, rect.bottom - (vh - pb));
        var last = Module.jasperSafeArea;
        if (last && last[0] === l && last[1] === t && last[2] === r && last[3] === b) return;
        Module.jasperSafeArea = [l, t, r, b];
        ccall('jasper_web_safe_area', null, ['number', 'number', 'number', 'number'], [l, t, r, b]);
    };
    var read = function() { Module.jasperReadSafeArea(); };
    window.addEventListener('resize', read);
    window.addEventListener('orientationchange', read);
    if (window.visualViewport) window.visualViewport.addEventListener('resize', read);
});

EM_JS(void, jasper_safe_area_read, (), {
    if (Module.jasperReadSafeArea) Module.jasperReadSafeArea();
});



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

// Browsers start an AudioContext only from a user gesture, and miniaudio resumes its devices on
// touchend and click only, dropping its listeners after the first even when the resume fails. Every
// gesture the browser counts (a key too: keyboard-only players, IME keys the key callback leaves to
// the composition) resumes the devices the engine started; the listeners stay, so a gesture the
// browser does not count (Escape) or a context the system suspended later (a call on iOS) is
// retried on the next one. A device the engine stopped stays stopped.
EM_JS(void, jasper_audio_unlock_install, (), {
    if (Module.jasperAudioUnlock) return;
    var resume = function() {
        var ma = window.miniaudio;
        if (!ma || !ma.devices || !ma.device_state) return;
        for (var i = 0; i < ma.devices.length; ++i) {
            var device = ma.devices[i];
            if (device && device.webaudio && device.state === ma.device_state.started &&
                device.webaudio.state !== 'running' && device.webaudio.state !== 'closed') {
                device.webaudio.resume().catch(function() {});
            }
        }
    };
    Module.jasperAudioUnlock = resume;
    ['keydown', 'mousedown', 'pointerdown', 'pointerup', 'touchend', 'click'].forEach(function(type) {
        window.addEventListener(type, resume, {capture: true, passive: true});
    });
});

int JasperEngineWeb::init(int argc, char **argv){

    canvas = "#canvas";

    // the canvas size the page gives (its arguments), else the game's
    int sWidth = jasperengine::platformConfig().width;
    int sHeight = jasperengine::platformConfig().height;
    if (argc > 2 && argv[1] != NULL && argv[2] != NULL){
        sWidth = atoi(argv[1]);
        sHeight = atoi(argv[2]);
    }

    jasper_audio_unlock_install();

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
    // Unity's OnApplicationFocus / OnApplicationPause: the window's focus and the page's visibility.
    // Not in the capture phase: the IME textarea's own focus and blur (which do not bubble) stay out
    emscripten_set_visibilitychange_callback(0, true, visibility_callback);
    emscripten_set_focus_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, false, focus_callback);
    emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, false, focus_callback);
    EM_ASM({
        if (screen.orientation && screen.orientation.addEventListener) {
            screen.orientation.addEventListener('change', function() { ccall('jasper_web_orientation_changed', null); });
        }
        // an orientation lock only holds in fullscreen: asked again on entering it
        document.addEventListener('fullscreenchange', function() {
            if (document.fullscreenElement && Module.jasperOrientationLock && screen.orientation && screen.orientation.lock) {
                screen.orientation.lock(Module.jasperOrientationLock).catch(function(e) {
                    console.debug('Screen.orientation: ' + e.message);
                });
            }
        });
    });

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

    // the project (main.lua, init(): the first scene) starts once the saved files are restored
    // (renderLoop), so its init reads them
    jasperengine::Engine::systemInit(argc, argv, new JasperEngineWeb(), false);
    projectStarted = false;
    startTime = emscripten_get_now();

    jasper_ime_create();
    jasper_safe_area_create();

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

    // the view is loaded once the project has started (renderLoop): the order of the other
    // platforms, init -> view loaded -> view changed, so Engine.onViewLoaded reaches the game
    JasperEngineWeb::screenWidth = sWidth;
    JasperEngineWeb::screenHeight = sHeight;

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
    emscripten_set_visibilitychange_callback(0, true, NULL);
    emscripten_set_focus_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, false, NULL);
    emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, false, NULL);
}

// One pause, whatever the sources: the page hidden, an ad playing, the GPU context lost.
// Resumes when none is left (Engine::systemPause / systemResume are idempotent)
void JasperEngineWeb::updatePause(){
    // a pause saves the user settings: not before the IndexedDB restore (renderLoop applies it)
    if (!projectStarted) return;
    if (pageHidden || adPlaying || contextLost){
        jasperengine::Engine::systemPause();
    }else{
        jasperengine::Engine::systemResume();
    }
}

void JasperEngineWeb::setAdPlaying(bool playing){
    adPlaying = playing;
    updatePause();
}

void JasperEngineWeb::setContextLost(bool lost){
    contextLost = lost;
    updatePause();
}

EM_BOOL JasperEngineWeb::visibility_callback(int eventType, const EmscriptenVisibilityChangeEvent* e, void* userData){
    pageHidden = e && e->hidden != 0;
    // hiding: onVisibilityChanged(false) before onPause; showing: after onResume
    if (projectStarted && pageHidden)
        jasperengine::Engine::systemVisibilityChanged(false);
    updatePause();
    if (projectStarted && !pageHidden)
        jasperengine::Engine::systemVisibilityChanged(true);

    // A hidden tab may be closed without another frame: files written in the last half second
    // (and the settings the pause saved) go to IndexedDB now
    if (pageHidden && syncWaitTime > 0)
        syncFileSystemNow();

    return EM_FALSE;
}

// the page lost the keyboard (another tab, window or app): held keys and buttons come up at
// once; then Unity's OnApplicationFocus
EM_BOOL JasperEngineWeb::focus_callback(int eventType, const EmscriptenFocusEvent* e, void* userData){
    windowFocused = eventType == EMSCRIPTEN_EVENT_FOCUS;
    jasperengine::Engine::systemFocusChanged(windowFocused);
    // before the project starts (the IndexedDB restore) it is applied at the start
    if (projectStarted)
        jasperengine::Engine::systemFocus(windowFocused);
    return 0;
}

void JasperEngineWeb::changeCanvasSize(int width, int height){
    emscripten_set_canvas_element_size(canvas.c_str(), width, height);

    JasperEngineWeb::screenWidth = width;
    JasperEngineWeb::screenHeight = height;
    // the page may resize before the project starts; the view is loaded with this size then
    // (and where the canvas now lies under the page's safe area first)
    jasper_safe_area_read();
    if (projectStarted)
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

bool JasperEngineWeb::flushFileSystem(){
    if (enabledIDB)
        syncFileSystemNow();

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

EM_JS_DEPS(jasper_device, "$stringToNewUTF8,$GL");

// The GPU behind WebGL: WEBGL_debug_renderer_info's unmasked renderer where the browser gives it
// ("ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 (0x00002487) Direct3D11 ...)", "Adreno (TM) 650"),
// else RENDERER (Firefox's sanitized name, Safari's "Apple GPU")
EM_JS(char*, jasper_gpu_renderer, (), {
    var gl = (typeof GLctx !== 'undefined') ? GLctx : null;
    if (!gl) return 0;
    var name = '';
    try {
        var ext = gl.getExtension('WEBGL_debug_renderer_info');
        name = ext ? gl.getParameter(ext.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER);
    } catch (e) {
        name = '';
    }
    return name ? stringToNewUTF8(String(name)) : 0;
});

// performance.memory (Chromium): the JS heap's share of its limit, 0 where the browser has none
EM_JS(double, jasper_js_heap_use, (), {
    var memory = (typeof performance !== 'undefined') ? performance.memory : null;
    return (memory && memory.jsHeapSizeLimit > 0) ? memory.usedJSHeapSize / memory.jsHeapSizeLimit : 0;
});

std::string JasperEngineWeb::getGpuRendererName(){
    char* name = jasper_gpu_renderer();
    if (!name) return "";
    std::string out = name;
    free(name);
    return out;
}

void JasperEngineWeb::pollMemoryPressure(){
    // every 5 s; a warning when the use crosses 90 %, again only once it fell under 80 %
    static double lastPoll = 0.0;
    static bool pressed = false;
    const double now = emscripten_get_now();
    if (now - lastPoll < 5000.0) return;
    lastPoll = now;
    const double heapMax = (double)emscripten_get_heap_max();
    const double wasmUse = heapMax > 0.0 ? (double)emscripten_get_heap_size() / heapMax : 0.0;
    const double use = std::max(wasmUse, jasper_js_heap_use());
    if (!pressed && use >= 0.9) {
        pressed = true;
        jasperengine::Log::warn("Memory: %.0f%% of the page's heap in use, a low-memory warning", use * 100.0);
        jasperengine::Device::reportLowMemory();
    } else if (pressed && use < 0.8) {
        pressed = false;
    }
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

        // a quit before the project started has no view to destroy
        if (projectStarted)
            jasperengine::Engine::systemViewDestroyed();
        jasperengine::Engine::systemShutdown();

        // shutdown also writes files and this loop will not run again
        if (syncWaitTime > 0)
            syncFileSystemNow();

        // a page has no process exit code: the run's (R19) is logged for the browser gate
        const int exitCode = jasperengine::player::RunControl::finish();
        std::printf("JasperEngine exit code %d\n", exitCode);
        std::fflush(stdout);
        return EM_FALSE;
    }

    if (!projectStarted){
        // IndexedDB answers in a few milliseconds; a browser that never does (storage
        // blocked) must not keep the game from starting
        const bool timedOut = emscripten_get_now() - startTime >= 3000.0;
        if (!loadedIDB && !timedOut)
            return EM_TRUE;
        if (!loadedIDB)
            jasperengine::Log::warn("user data not restored after 3 s; saves may be missing");
        projectStarted = true;
        jasperengine::Engine::systemStartProject();
        jasperengine::Engine::systemViewLoaded();
        changeCanvasSize(screenWidth, screenHeight);
        // what the page did while the project waited
        if (!windowFocused)
            jasperengine::Engine::systemFocus(false);
        if (pageHidden)
            jasperengine::Engine::systemVisibilityChanged(false);
        updatePause();
    }

    // the display's rate: the median interval of the first animation frames
    if (lastFrameTime > 0.0 && frameIntervals.size() < 30){
        const double interval = time - lastFrameTime;
        if (interval > 1.0 && interval < 200.0) frameIntervals.push_back(interval);
        if (frameIntervals.size() == 30){
            std::vector<double> sorted = frameIntervals;
            std::sort(sorted.begin(), sorted.end());
            measuredRefreshRate = (float)std::round(1000.0 / sorted[sorted.size() / 2]);
        }
    }
    lastFrameTime = time;

    // Engine.targetFrameRate: a frame that is not due yet draws nothing (2 ms early still counts)
    if (!frameLimiter.isDue(jasperengine::Engine::getFramePacing().limiterFps, 0.002)){
        return EM_TRUE;
    }

    pollGamepads();
    pollMemoryPressure();

    // CSS size is the input coordinate space as well as the visible surface.
    // Update the framebuffer before drawing; Engine applies the project's
    // Scaling mode (including intentional stretching) to this actual size.
    double cssWidth = 0, cssHeight = 0;
    if (emscripten_get_element_css_size(canvas.c_str(), &cssWidth, &cssHeight) == EMSCRIPTEN_RESULT_SUCCESS) {
        // pixel art: physical pixels (S41 R16)
        const bool pixelated = pixelArtView();
        const double ratio = pixelated ? std::max(1.0, emscripten_get_device_pixel_ratio()) : 1.0;
        if (pixelated != webCanvasPixelated) {
            webCanvasPixelated = pixelated;
            EM_ASM({
                var element = document.querySelector(UTF8ToString($0));
                if (element) element.style.imageRendering = $1 ? "pixelated" : "";
            }, canvas.c_str(), pixelated ? 1 : 0);
            if (pixelated && (int)round(cssWidth) == screenWidth && (int)round(cssHeight) == screenHeight) {
                emscripten_set_element_css_size(canvas.c_str(), cssWidth, cssHeight);
                webCanvasCssPinned = true;
                webCanvasPinnedWidth = cssWidth;
                webCanvasPinnedHeight = cssHeight;
            } else if (!pixelated && webCanvasCssPinned) {
                // back to a backing store of the CSS size, then the attributes size it again
                webCanvasCssPinned = false;
                changeCanvasSize((int)round(webCanvasPinnedWidth), (int)round(webCanvasPinnedHeight));
                EM_ASM({
                    var element = document.querySelector(UTF8ToString($0));
                    if (element) { element.style.width = ""; element.style.height = ""; }
                }, canvas.c_str());
                cssWidth = webCanvasPinnedWidth;
                cssHeight = webCanvasPinnedHeight;
            }
        }
        const int width = (int)round(cssWidth * ratio);
        const int height = (int)round(cssHeight * ratio);
        if (width > 0 && height > 0) {
            webCanvasPixelRatio = cssWidth > 0 ? (double)width / cssWidth : 1.0;
            if (width != screenWidth || height != screenHeight)
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
    // before the project starts the view is not loaded yet: it gets this size when it is
    // (and where the canvas now lies under the page's safe area first)
    jasper_safe_area_read();
    if (projectStarted)
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

    int code = jasperengine::web::keyFromCode(e->code);
    if (code==0){
        code = jasperengine::web::keyFromKeyCode((int)e->which, (int)e->location);
    }
    if (code==0){
        code = jasperengine::web::keyFromKeyCode((int)e->keyCode, (int)e->location);
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

        char32_t cp = toCodepoint(std::string(e->key));
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
        // CSS pixels to the backing store's (1 unless pixel art sizes it by the device ratio)
        const double targetX = e->targetX * webCanvasPixelRatio;
        const double targetY = e->targetY * webCanvasPixelRatio;
        if (targetX < 0) return 0;
        if (targetY < 0) return 0;
        if (targetX > width) return 0;
        if (targetY > height) return 0;

        mousePosX = targetX;
        mousePosY = targetY;
    }else{
        // movement comes in CSS pixels too (S41 R16)
        mousePosX += e->movementX * webCanvasPixelRatio;
        mousePosY += e->movementY * webCanvasPixelRatio;
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
                jasperengine::Engine::systemTouchStart((int)src->identifier, src->targetX * webCanvasPixelRatio, src->targetY * webCanvasPixelRatio);
            }
            break;
        case EMSCRIPTEN_EVENT_TOUCHMOVE:
            for (int i = 0; i < emsc_event->numTouches; i++) {
                const EmscriptenTouchPoint* src = &emsc_event->touches[i];
                if (!src->isChanged) continue;
                jasperengine::Engine::systemTouchMove((int)src->identifier, src->targetX * webCanvasPixelRatio, src->targetY * webCanvasPixelRatio);
            }
            break;
        case EMSCRIPTEN_EVENT_TOUCHEND:
            for (int i = 0; i < emsc_event->numTouches; i++) {
                const EmscriptenTouchPoint* src = &emsc_event->touches[i];
                if (!src->isChanged) continue;
                jasperengine::Engine::systemTouchEnd((int)src->identifier, src->targetX * webCanvasPixelRatio, src->targetY * webCanvasPixelRatio);
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
        case EMSCRIPTEN_EVENT_WEBGLCONTEXTLOST:     setContextLost(true); break;
        case EMSCRIPTEN_EVENT_WEBGLCONTEXTRESTORED: setContextLost(false); break;
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


// ---- UserSettings: localStorage. Pages of one site share it, so each game prefixes its keys:
// "<storageId>/<key>". Bare "<key>" entries of older builds are still read, so their saves
// carry over, until this game writes or deletes the key, or deletes all (then no bare key counts
// any more: the "<storageId>/\u0001jasperengine:cleared" marker). A deleted key whose bare entry
// exists is kept as the tombstone value "\u0001jasperengine:deleted" (a bare key may be another
// game's, so it is never removed). Older builds wrote "" as their tombstone; those are turned
// into the new one once ("<storageId>/\u0001jasperengine:v2"), so "" is a value like any other.
// Typed values use System's text format (the getters and setters of the base class).
// localStorage is synchronous: there is nothing to save. It can throw (storage disabled, quota):
// a read then gives the default and a failed write is logged once.

// the JS library functions the store uses (stringToNewUTF8 brings malloc in)
EM_JS_DEPS(jasper_user_settings, "$UTF8ToString,$stringToNewUTF8,$lengthBytesUTF8");

EM_JS(void, jasper_settings_install, (), {
    if (Module.jasperSettings) return;
    var SOH = String.fromCharCode(1);
    var TOMBSTONE = SOH + "jasperengine:deleted";
    var migrated = {};
    function storage() { try { return window.localStorage; } catch (e) { return null; } }
    function marker(id, name) { return id + "/" + SOH + "jasperengine:" + name; }
    function migrate(ls, id) {
        if (migrated[id]) return;
        migrated[id] = true;
        if (ls.getItem(marker(id, "v2")) !== null) return;
        var prefix = id + "/";
        var keys = [];
        for (var i = 0; i < ls.length; i++) {
            var k = ls.key(i);
            if (k !== null && k.indexOf(prefix) === 0) keys.push(k);
        }
        keys.forEach(function(k) {
            if (ls.getItem(k) === "" && ls.getItem(k.substring(prefix.length)) !== null) ls.setItem(k, TOMBSTONE);
        });
        ls.setItem(marker(id, "v2"), "1");
    }
    function open(id) {
        var ls = storage();
        if (!ls) return null;
        try { migrate(ls, id); } catch (e) {}
        return ls;
    }
    Module.jasperSettings = {
        get: function(id, key) {
            try {
                var ls = open(id);
                if (!ls) return null;
                var own = ls.getItem(id + "/" + key);
                if (own !== null) return own === TOMBSTONE ? null : own;
                if (ls.getItem(marker(id, "cleared")) !== null) return null;
                return ls.getItem(key);
            } catch (e) { return null; }
        },
        set: function(id, key, value) {
            try {
                var ls = open(id);
                if (!ls) return 0;
                ls.setItem(id + "/" + key, value);
                return 1;
            } catch (e) { return 0; }
        },
        remove: function(id, key) {
            try {
                var ls = open(id);
                if (!ls) return 0;
                var own = id + "/" + key;
                if (ls.getItem(marker(id, "cleared")) === null && ls.getItem(key) !== null) ls.setItem(own, TOMBSTONE);
                else ls.removeItem(own);
                return 1;
            } catch (e) { return 0; }
        },
        clear: function(id) {
            try {
                var ls = open(id);
                if (!ls) return 0;
                var prefix = id + "/";
                var keep = marker(id, "v2");
                var keys = [];
                for (var i = 0; i < ls.length; i++) {
                    var k = ls.key(i);
                    if (k !== null && k.indexOf(prefix) === 0 && k !== keep) keys.push(k);
                }
                keys.forEach(function(k) { ls.removeItem(k); });
                ls.setItem(marker(id, "cleared"), "1");
                return 1;
            } catch (e) { return 0; }
        }
    };
});

// Values cross with their byte length (UTF8ToString / stringToUTF8 stop at a zero byte), so a
// value with zeros survives as on the other platforms; keys never have one (UserSettings).
EM_JS(char*, jasper_settings_get, (const char* id, const char* key, int* length), {
    var value = Module.jasperSettings.get(UTF8ToString(id), UTF8ToString(key));
    if (value === null) return 0;
    // stringToNewUTF8 encodes U+0000 as a zero byte and does not stop there
    HEAP32[length >> 2] = lengthBytesUTF8(value);
    return stringToNewUTF8(value);
});

EM_JS(int, jasper_settings_has, (const char* id, const char* key), {
    return Module.jasperSettings.get(UTF8ToString(id), UTF8ToString(key)) !== null ? 1 : 0;
});

EM_JS(int, jasper_settings_set, (const char* id, const char* key, const char* value, int length), {
    var text = new TextDecoder().decode(HEAPU8.subarray(value, value + length));
    return Module.jasperSettings.set(UTF8ToString(id), UTF8ToString(key), text);
});

EM_JS(int, jasper_settings_remove, (const char* id, const char* key), {
    return Module.jasperSettings.remove(UTF8ToString(id), UTF8ToString(key));
});

EM_JS(int, jasper_settings_clear, (const char* id), {
    return Module.jasperSettings.clear(UTF8ToString(id));
});

namespace {
    // the JS store (Module.jasperSettings), made on first use
    const char* settingsId(){
        static bool installed = false;
        if (!installed){
            installed = true;
            jasper_settings_install();
        }
        // the export's id (PlatformDefaults) or a player's game.json
        return jasperengine::platformConfig().storageId.c_str();
    }

    void settingsWriteFailed(){
        static bool logged = false;
        if (!logged){
            logged = true;
            jasperengine::Log::error("UserSettings: localStorage refused the write (storage disabled or full)");
        }
    }
}

std::string JasperEngineWeb::getStringForKey(const char *key, const std::string& defaultValue){
    if (!key) return defaultValue;
    int length = 0;
    char* value = jasper_settings_get(settingsId(), key, &length);
    if (value){
        std::string strValue(value, static_cast<size_t>(length));
        free(value); //Each call to _malloc() must be paired with free() or heap memory will leak
        return strValue;
    }

    return defaultValue;
}

void JasperEngineWeb::setStringForKey(const char* key, const std::string& value){
    if (!key || !key[0]){
        jasperengine::Log::error("UserSettings: a key must be a non-empty string");
        return;
    }
    if (!jasper_settings_set(settingsId(), key, value.data(), static_cast<int>(value.size())))
        settingsWriteFailed();
}

void JasperEngineWeb::removeKey(const char *key){
    if (key && !jasper_settings_remove(settingsId(), key))
        settingsWriteFailed();
}

bool JasperEngineWeb::hasKey(const char *key){
    return key && jasper_settings_has(settingsId(), key);
}

void JasperEngineWeb::removeAllKeys(){
    if (!jasper_settings_clear(settingsId()))
        settingsWriteFailed();
}

void JasperEngineWeb::saveUserSettings(){
    // localStorage writes are synchronous
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

// ------------------------------------------------------------------ platform services

bool JasperEngineWeb::platformOpenURL(const std::string& url){
    // a popup the browser blocks (no click or key going on) opens on the next one instead
    return EM_ASM_INT({
        var url = UTF8ToString($0);
        var opened = window.open(url, '_blank');
        if (opened) {
            try { opened.opener = null; } catch (e) {}
            return 1;
        }
        Module.jasperPendingUrl = url;
        if (!Module.jasperPendingUrlListener) {
            Module.jasperPendingUrlListener = function() {
                var pending = Module.jasperPendingUrl;
                if (!pending) return;
                Module.jasperPendingUrl = null;
                var w = window.open(pending, '_blank');
                if (w) { try { w.opener = null; } catch (e) {} }
            };
            (['pointerup', 'keyup', 'touchend']).forEach(function(type) {
                document.addEventListener(type, Module.jasperPendingUrlListener, true);
            });
        }
        console.log('System.openURL: the browser blocked the popup; ' + url + ' opens on the next click or key');
        return 0;
    }, url.c_str()) != 0;
}

std::vector<std::string> JasperEngineWeb::getPreferredLocales(){
    const std::string list = takeJsString((char*)EM_ASM_INT({
        var langs = (navigator.languages && navigator.languages.length) ? navigator.languages : [navigator.language || 'en-US'];
        var text = langs.join('\n');
        var len = lengthBytesUTF8(text) + 1;
        var buf = _malloc(len);
        stringToUTF8(text, buf, len);
        return buf;
    }));
    std::vector<std::string> tags;
    size_t start = 0;
    while (start <= list.size()){
        const size_t end = list.find('\n', start);
        const std::string tag = jasperengine::LocaleRules::normalize(list.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (!tag.empty() && std::find(tags.begin(), tags.end(), tag) == tags.end()) tags.push_back(tag);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return tags.empty() ? System::getPreferredLocales() : tags;
}

std::string JasperEngineWeb::getOperatingSystem(){
    static const std::string value = "Web (" + takeJsString((char*)EM_ASM_INT({
        var platform = (navigator.userAgentData && navigator.userAgentData.platform) ? navigator.userAgentData.platform
                       : (navigator.platform || 'unknown');
        var len = lengthBytesUTF8(platform) + 1;
        var buf = _malloc(len);
        stringToUTF8(platform, buf, len);
        return buf;
    })) + ")";
    return value;
}

std::string JasperEngineWeb::getOperatingSystemVersion(){
    return "";
}

std::string JasperEngineWeb::getDeviceModel(){
    return "Browser";
}

jasperengine::DeviceType JasperEngineWeb::getDeviceType(){
    return isTouchDevice() ? jasperengine::DeviceType::Handheld : jasperengine::DeviceType::Desktop;
}

int JasperEngineWeb::getSystemMemorySize(){
    // navigator.deviceMemory: GiB, rounded and capped by the browser (Chromium only)
    return EM_ASM_INT({
        return navigator.deviceMemory ? Math.round(navigator.deviceMemory * 1024) : 0;
    });
}

jasperengine::NetworkReachability JasperEngineWeb::getInternetReachability(){
    const int online = EM_ASM_INT({ return navigator.onLine ? 1 : 0; });
    return online ? jasperengine::NetworkReachability::ReachableViaLocalAreaNetwork : jasperengine::NetworkReachability::NotReachable;
}

bool JasperEngineWeb::vibrate(int milliseconds){
    return EM_ASM_INT({
        return (navigator.vibrate && navigator.vibrate(Math.max(1, $0))) ? 1 : 0;
    }, milliseconds) != 0;
}

bool JasperEngineWeb::hapticFeedback(jasperengine::HapticFeedback type){
    // the Vibration API's approximation of the iOS feedback kinds (the list in parentheses: a
    // comma outside them would split the EM_ASM macro arguments)
    return EM_ASM_INT({
        var patterns = ([[10], [20], [40], [10], [20, 40, 20], [30, 60, 30], [40, 60, 40, 60, 40]]);
        var pattern = patterns[$0] || [20];
        return (navigator.vibrate && navigator.vibrate(pattern)) ? 1 : 0;
    }, (int)type) != 0;
}

float JasperEngineWeb::getScreenDpi(){
    return (float)EM_ASM_DOUBLE({ return (window.devicePixelRatio || 1) * 96; });
}

float JasperEngineWeb::getRefreshRate(){
    return measuredRefreshRate > 0.0f ? measuredRefreshRate : 60.0f;
}

void JasperEngineWeb::setKeepScreenOn(bool keepOn){
    // the Screen Wake Lock API; the browser drops the lock when the page is hidden, so it is
    // asked again when the page shows
    EM_ASM({
        Module.jasperKeepScreenOn = $0 != 0;
        function acquire() {
            if (!Module.jasperKeepScreenOn || !navigator.wakeLock || document.visibilityState !== 'visible') return;
            // one request at a time: a second one before the first resolves would leak a lock
            if (Module.jasperWakeLock || Module.jasperWakeLockPending) return;
            Module.jasperWakeLockPending = true;
            navigator.wakeLock.request('screen').then(function(lock) {
                Module.jasperWakeLockPending = false;
                Module.jasperWakeLock = lock;
                lock.addEventListener('release', function() { if (Module.jasperWakeLock === lock) Module.jasperWakeLock = null; });
                if (!Module.jasperKeepScreenOn) { Module.jasperWakeLock = null; lock.release(); }
            }).catch(function(e) {
                Module.jasperWakeLockPending = false;
                console.debug('Screen.sleepTimeout: ' + e.message);
            });
        }
        if (!Module.jasperWakeLockListener) {
            Module.jasperWakeLockListener = acquire;
            document.addEventListener('visibilitychange', acquire);
        }
        if (Module.jasperKeepScreenOn) {
            acquire();
        } else if (Module.jasperWakeLock) {
            var lock = Module.jasperWakeLock;
            Module.jasperWakeLock = null;
            lock.release();
        }
    }, keepOn ? 1 : 0);
}

void JasperEngineWeb::applyOrientationPolicy(jasperengine::ScreenOrientation requested, uint8_t allowedMask){
    using jasperengine::PlatformRules::orientationBit;
    using jasperengine::ScreenOrientation;
    const uint8_t mask = jasperengine::PlatformRules::effectiveMask(requested, allowedMask);
    const uint8_t portraits = orientationBit(ScreenOrientation::Portrait) | orientationBit(ScreenOrientation::PortraitUpsideDown);
    const uint8_t landscapes = orientationBit(ScreenOrientation::LandscapeLeft) | orientationBit(ScreenOrientation::LandscapeRight);
    const char* lock = "any";
    if (mask == orientationBit(ScreenOrientation::Portrait)) lock = "portrait-primary";
    else if (mask == orientationBit(ScreenOrientation::PortraitUpsideDown)) lock = "portrait-secondary";
    else if (mask == orientationBit(ScreenOrientation::LandscapeLeft)) lock = "landscape-primary";
    else if (mask == orientationBit(ScreenOrientation::LandscapeRight)) lock = "landscape-secondary";
    else if ((mask & landscapes) == 0) lock = "portrait";
    else if ((mask & portraits) == 0) lock = "landscape";
    // browsers lock only in fullscreen: a refusal is expected and logged verbosely
    EM_ASM({
        var lock = UTF8ToString($0);
        Module.jasperOrientationLock = lock;
        if (!screen.orientation || !screen.orientation.lock) return;
        screen.orientation.lock(lock).catch(function(e) {
            console.debug('Screen.orientation: ' + e.message);
        });
    }, lock);
}

jasperengine::ScreenOrientation JasperEngineWeb::getScreenOrientation(){
    // a desktop browser's screen is the monitor, not the canvas: the canvas size says it there
    if (!isTouchDevice()) return System::getScreenOrientation();
    const int type = EM_ASM_INT({
        if (!screen.orientation || !screen.orientation.type) return 0;
        var types = ({'portrait-primary': 1, 'portrait-secondary': 2, 'landscape-primary': 3, 'landscape-secondary': 4});
        return types[screen.orientation.type] || 0;
    });
    if (jasperengine::PlatformRules::isOrientation(type) && type != (int)jasperengine::ScreenOrientation::AutoRotation){
        return (jasperengine::ScreenOrientation)type;
    }
    return System::getScreenOrientation();
}
