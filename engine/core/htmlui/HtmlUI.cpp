// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUI.h"

#include "HtmlUIContext.h"
#include "RmlFile.h"
#include "RmlFontEngine.h"
#include "RmlSystem.h"
#include "vector/UILottieElement.h"
#include "compat/runtime/RuntimeRegistry.h"

#include "Engine.h"
#include "Log.h"
#include "LunaSVGRuntime.h"
#include "ThorVGRuntime.h"

#include <RmlUi/Core.h>
#ifdef JASPERENGINE_HTMLUI_DEBUGGER
#include <RmlUi/Debugger.h>
#endif

#include <algorithm>
#include <vector>

using namespace jasperengine;

namespace {

    const char* viewDestroyedTag = "jasperengine::HtmlUI_onViewDestroyed";

    int referenceCount = 0;

#ifdef JASPERENGINE_HTMLUI_DEBUGGER
    bool debuggerInitialised = false;
#endif
    HtmlUIContext* debuggerContext = nullptr;
    bool debuggerVisible = false;

    std::vector<HtmlUIContext*>& registeredContexts(){
        static std::vector<HtmlUIContext*> contexts;
        return contexts;
    }

    // The view (and every GPU object with it) goes away before the scenes are destroyed:
    // contexts drop their textures and buffers, RmlUi rebuilds them lazily once it is back
    void onViewDestroyed(){
        if (referenceCount == 0)
            return;

        // a copy: a context may unregister while releasing
        std::vector<HtmlUIContext*> contexts = registeredContexts();
        for (HtmlUIContext* context : contexts){
            context->releaseGpuResources();
        }

        Rml::ReleaseFontResources();
    }

    void subscribeViewDestroyed(){
        // Engine::clearAllSubscriptions (the editor stopping play mode) can drop it,
        // so every acquire puts it back (add() replaces a subscription with the same tag)
        Engine::onViewDestroyed.add(viewDestroyedTag, std::function<void()>(onViewDestroyed));
    }

}

bool HtmlUI::acquire(){
    if (referenceCount > 0){
        referenceCount++;
        subscribeViewDestroyed();
        return true;
    }

    // Rml::Shutdown clears the interfaces, so they are set again on every initialisation
    Rml::SetSystemInterface(&getSystemInterface());
    Rml::SetFileInterface(&getFileInterface());
    Rml::SetFontEngineInterface(&getFontEngine());

    if (!Rml::Initialise()){
        Log::error("[HTML UI] RmlUi could not be initialised");
        return false;
    }

    // Vector graphics: Rml::Initialise already registered the SVG plugin (RMLUI_SVG_PLUGIN,
    // LunaSVG), which needs the engine's fonts for <text>; <lottie> runs on ThorVG. RmlUi
    // forgets element instancers at shutdown, so the lottie one is registered every time.
    LunaSVGRuntime::ensureFonts();
    ThorVGRuntime::acquire();
    UILottieElement::registerInstancer();
    // the compat layer's runtime emulations (phase-2 hook H1), before any document loads
    htmlui::compat::registerRuntime();

    referenceCount = 1;
    subscribeViewDestroyed();

    return true;
}

void HtmlUI::release(){
    if (referenceCount <= 0){
        Log::warn("[HTML UI] HtmlUI::release called without a matching acquire");
        return;
    }

    referenceCount--;
    if (referenceCount > 0)
        return;

    if (!registeredContexts().empty()){
        Log::error("[HTML UI] %u HTML UI context(s) still alive when RmlUi shuts down", (unsigned int)registeredContexts().size());
    }

#ifdef JASPERENGINE_HTMLUI_DEBUGGER
    if (debuggerInitialised){
        Rml::Debugger::Shutdown();
        debuggerInitialised = false;
    }
#endif
    debuggerContext = nullptr;
    debuggerVisible = false;

    Rml::Shutdown();

    // after Rml::Shutdown: the documents (and their ThorVG animations) are gone
    UILottieElement::releaseInstancer();
    htmlui::compat::releaseRuntime();
    ThorVGRuntime::release();

    Engine::onViewDestroyed.remove(viewDestroyedTag);
}

bool HtmlUI::isInitialized(){
    return referenceCount > 0;
}

int HtmlUI::getReferenceCount(){
    return referenceCount;
}

bool HtmlUI::loadFontFace(const std::string& path, bool fallback, const std::string& family, int weight, bool italic){
    if (path.empty()){
        Log::error("[HTML UI] loadFontFace: empty font path");
        return false;
    }

    const Rml::Style::FontWeight fontWeight = (weight > 0) ? (Rml::Style::FontWeight)std::min(weight, 1000) : Rml::Style::FontWeight::Auto;

    // Faces live in the engine's FontRegistry, not in RmlUi, so they can be loaded
    // before the first document initialises RmlUi
    if (family.empty()){
        return getFontEngine().LoadFontFace(path, 0, fallback, fontWeight);
    }

    const Rml::Style::FontStyle style = italic ? Rml::Style::FontStyle::Italic : Rml::Style::FontStyle::Normal;
    return getFontEngine().LoadFontFace(path, 0, family, style, fontWeight, fallback);
}

RmlSystem& HtmlUI::getSystemInterface(){
    // function-local statics: they outlive Rml::Shutdown(), which still logs through them
    static RmlSystem systemInterface;
    return systemInterface;
}

RmlFile& HtmlUI::getFileInterface(){
    static RmlFile fileInterface;
    return fileInterface;
}

RmlFontEngine& HtmlUI::getFontEngine(){
    static RmlFontEngine fontEngine;
    return fontEngine;
}

void HtmlUI::registerContext(HtmlUIContext* context){
    if (!context)
        return;

    std::vector<HtmlUIContext*>& contexts = registeredContexts();
    if (std::find(contexts.begin(), contexts.end(), context) == contexts.end()){
        contexts.push_back(context);
    }
}

void HtmlUI::unregisterContext(HtmlUIContext* context){
    std::vector<HtmlUIContext*>& contexts = registeredContexts();
    contexts.erase(std::remove(contexts.begin(), contexts.end(), context), contexts.end());

    if (context && context == debuggerContext){
        setDebuggerContext(nullptr, false);
    }
}

size_t HtmlUI::getContextCount(){
    return registeredContexts().size();
}

void HtmlUI::setDebuggerContext(HtmlUIContext* context, bool visible){
#ifndef JASPERENGINE_HTMLUI_DEBUGGER
    (void)visible;
    if (context){
        Log::warn("[HTML UI] the RmlUi debugger is not compiled into this build (JASPERENGINE_HTMLUI_DEBUGGER)");
    }
    debuggerContext = nullptr;
    debuggerVisible = false;
#else
    Rml::Context* rmlContext = context ? context->getRmlContext() : nullptr;

    if (!rmlContext){
        if (context){
            Log::warn("[HTML UI] the debugger needs a created HTML UI context");
        }
        if (debuggerInitialised){
            Rml::Debugger::Shutdown();
            debuggerInitialised = false;
        }
        debuggerContext = nullptr;
        debuggerVisible = false;
        return;
    }

    if (!isInitialized()){
        Log::warn("[HTML UI] the debugger needs RmlUi to be initialised");
        return;
    }

    // the debugger is drawn in its host context and inspects that same context; moving it
    // to another one means loading it again there
    if (debuggerInitialised && debuggerContext != context){
        Rml::Debugger::Shutdown();
        debuggerInitialised = false;
    }

    if (!debuggerInitialised){
        if (!Rml::Debugger::Initialise(rmlContext)){
            Log::error("[HTML UI] the RmlUi debugger could not be initialised");
            debuggerContext = nullptr;
            debuggerVisible = false;
            return;
        }
        debuggerInitialised = true;
    }

    debuggerContext = context;
    debuggerVisible = visible;
    Rml::Debugger::SetVisible(visible);
#endif
}

HtmlUIContext* HtmlUI::getDebuggerContext(){
    return debuggerContext;
}

bool HtmlUI::isDebuggerVisible(){
    return debuggerContext && debuggerVisible;
}

bool HtmlUI::isDebuggerCompiled(){
#ifdef JASPERENGINE_HTMLUI_DEBUGGER
    return true;
#else
    return false;
#endif
}
