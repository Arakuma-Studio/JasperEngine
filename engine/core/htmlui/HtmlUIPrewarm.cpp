// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUIPrewarm.h"

#include "Engine.h"
#include "HtmlUI.h"
#include "HtmlUIContext.h"
#include "Log.h"
#include "htmlui/compat/DocumentLoader.h"
#include "render/SystemRender.h"
#include "script/LuaBinding.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/FontEffect.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/RenderManager.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <RmlUi/Core/StyleSheetTypes.h>
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/URL.h>

#include <algorithm>
#include <atomic>
#include <chrono>

using namespace jasperengine;

namespace {

    std::atomic<double> prewarmBudgetMs{2.0};
    double lastUpdateMs = 0.0;

    std::vector<std::shared_ptr<HtmlUIPrewarmState>>& trackedPrewarms(){
        static std::vector<std::shared_ptr<HtmlUIPrewarmState>> states;
        return states;
    }

    // the element's font-effect list as its text draws it (ElementText::UpdateFontEffects)
    Rml::FontEffectsHandle effectsOf(Rml::Element* element, Rml::FontFaceHandle handle){
        static const Rml::FontEffectList none;
        const Rml::FontEffectList* list = &none;
        if (element->GetComputedValues().has_font_effect()){
            if (const Rml::Property* property = element->GetProperty(Rml::PropertyId::FontEffect)){
                if (Rml::FontEffectsPtr effects = property->Get<Rml::FontEffectsPtr>())
                    list = &effects->list;
            }
        }
        return HtmlUI::getFontEngine().PrepareFontEffects(handle, *list);
    }

    void addText(Rml::Element* element, const std::string& text, std::vector<RmlFontEngine::PrewarmText>& texts){
        const Rml::FontFaceHandle handle = element->GetFontFaceHandle();
        if (!handle || text.empty())
            return;
        RmlFontEngine::PrewarmText warm;
        warm.handle = handle;
        warm.effects = effectsOf(element, handle);
        warm.direction = element->GetComputedValues().direction();
        warm.text = text;
        // text-transform (its ASCII letters: the case mappings of RmlUi's text are internal to it)
        switch (element->GetComputedValues().text_transform()){
            case Rml::Style::TextTransform::Uppercase: warm.text = Rml::StringUtilities::ToUpper(text); break;
            case Rml::Style::TextTransform::Lowercase: warm.text = Rml::StringUtilities::ToLower(text); break;
            case Rml::Style::TextTransform::Capitalize: warm.text = text + Rml::StringUtilities::ToUpper(text); break;
            default: break;
        }
        texts.push_back(std::move(warm));
    }

    // styles are computed for every element, displayed or not (RmlUi's update): display: none subtrees have their fonts
    void collect(Rml::Element* element, std::vector<RmlFontEngine::PrewarmText>& texts){
        if (Rml::ElementText* text = rmlui_dynamic_cast<Rml::ElementText*>(element))
            addText(element, text->GetText(), texts);
        if (const Rml::Variant* warm = element->GetAttribute("data-warm"))
            addText(element, warm->Get<Rml::String>(), texts);
        for (int i = 0; i < element->GetNumChildren(); i++)
            collect(element->GetChild(i), texts);
    }

}

size_t HtmlUIPrewarmState::total() const{
    return (glyphs ? glyphs->total : 0) + (images ? images->total : 0);
}

size_t HtmlUIPrewarmState::ready() const{
    size_t ready = glyphs ? std::min(glyphs->ready, glyphs->total) : 0;
    if (images)
        ready += images->done ? images->total : std::min(images->completed, images->total);
    return ready;
}

bool HtmlUIPrewarmState::done() const{
    return ready() >= total();
}

namespace {

    bool isRasterImage(const std::string& path){
        std::string extension;
        const size_t dot = path.find_last_of('.');
        if (dot != std::string::npos)
            extension = Rml::StringUtilities::ToLower(path.substr(dot + 1));
        return extension == "png" || extension == "jpg" || extension == "jpeg" || extension == "webp" || extension == "bmp" ||
               extension == "tga" || extension == "gif" || extension == "ktx2";
    }

    // the <img> sources as RmlUi loads them (ElementImage: joined with the document's directory)
    void collectImages(Rml::Element* element, const std::string& directory, std::vector<std::string>& paths){
        if (element->GetTagName() == "img"){
            const Rml::String source = element->GetAttribute<Rml::String>("src", "");
            if (!source.empty() && source.find(':') == std::string::npos){
                Rml::String joined;
                Rml::GetSystemInterface()->JoinPath(joined, directory, source);
                const std::string file = joined.substr(0, joined.find_first_of("?#"));
                if (isRasterImage(file) && std::find(paths.begin(), paths.end(), joined) == paths.end())
                    paths.push_back(joined);
            }
        }
        for (int i = 0; i < element->GetNumChildren(); i++)
            collectImages(element->GetChild(i), directory, paths);
    }

}

std::shared_ptr<HtmlUIPrewarmState> HtmlUIPrewarm::prewarmImages(const std::vector<std::string>& paths){
    std::shared_ptr<HtmlUIPrewarmState> state = std::make_shared<HtmlUIPrewarmState>();
    state->images = HtmlUIImageCache::get().prewarm(paths);
    track(state);
    return state;
}

std::shared_ptr<HtmlUIPrewarmState> HtmlUIPrewarm::prewarmDocument(Rml::ElementDocument* document, bool images){
    std::shared_ptr<HtmlUIPrewarmState> state = std::make_shared<HtmlUIPrewarmState>();
    if (!document)
        return state;
    // (styles of an element added since the last update are resolved first)
    document->UpdateDocument();
    std::vector<RmlFontEngine::PrewarmText> texts;
    collect(document, texts);
    std::vector<const Rml::RenderManager*> managers;
    if (Rml::Context* context = document->GetContext())
        managers.push_back(&context->GetRenderManager());
    state->glyphs = HtmlUI::getFontEngine().prewarm(texts, managers);
    if (images){
        std::vector<std::string> paths;
        collectImages(document, Rml::URL(document->GetSourceURL()).GetPath(), paths);
        // the images the compile read (an .html document's style sheets: backgrounds, border images, masks)
        if (std::shared_ptr<const htmlui::compat::CompatDocument> compiled = htmlui::compat::DocumentLoader::compiledOf(document)){
            for (const std::string& file : compiled->dependencies){
                if (isRasterImage(file) && std::find(paths.begin(), paths.end(), file) == paths.end())
                    paths.push_back(file);
            }
        }
        if (!paths.empty())
            state->images = HtmlUIImageCache::get().prewarm(paths);
    }
    track(state);
    return state;
}

std::shared_ptr<HtmlUIPrewarmState> HtmlUIPrewarm::prewarmText(const UIPrewarmText& text, std::string* error){
    std::shared_ptr<HtmlUIPrewarmState> state = std::make_shared<HtmlUIPrewarmState>();
    // font-effect is parsed by RmlUi: up for the call when no document has it up (the glyph cache outlives it)
    const bool acquired = !HtmlUI::isInitialized() && HtmlUI::acquire();
    RmlFontEngine& fonts = HtmlUI::getFontEngine();
    const Rml::FontFaceHandle handle = fonts.GetFontFaceHandle(text.family, text.italic ? Rml::Style::FontStyle::Italic : Rml::Style::FontStyle::Normal,
                                                               (Rml::Style::FontWeight)std::max(1, std::min(text.weight, 1000)), std::max(1, text.size));
    Rml::FontEffectsHandle effects = 0;
    bool parsed = true;
    if (!text.effect.empty() && HtmlUI::isInitialized()){
        Rml::PropertyDictionary properties;
        const Rml::Property* property = nullptr;
        if (Rml::StyleSheetSpecification::ParsePropertyDeclaration(properties, "font-effect", text.effect))
            property = properties.GetProperty(Rml::PropertyId::FontEffect);
        Rml::FontEffectsPtr list = property ? property->Get<Rml::FontEffectsPtr>() : nullptr;
        if (list)
            effects = fonts.PrepareFontEffects(handle, list->list);
        else
            parsed = false;
    }
    if (!parsed){
        if (error)
            *error = "'" + text.effect + "' is not a font-effect value: use outline(2px #000), glow(2px 4px 0px 0px #000), blur(3px #fff) "
                     "or shadow(2px 2px #000), comma-separated";
    }else if (handle){
        std::vector<const Rml::RenderManager*> managers;
        for (HtmlUIContext* context : HtmlUI::getContexts()){
            if (Rml::Context* rml = context->getRmlContext())
                managers.push_back(&rml->GetRenderManager());
        }
        RmlFontEngine::PrewarmText warm;
        warm.handle = handle;
        warm.effects = effects;
        warm.text = text.text;
        state->glyphs = fonts.prewarm({warm}, managers);
        track(state);
    }
    if (acquired)
        HtmlUI::release();
    return state;
}

void HtmlUIPrewarm::track(const std::shared_ptr<HtmlUIPrewarmState>& state){
    if (state && !state->notified)
        trackedPrewarms().push_back(state);
}

void HtmlUIPrewarm::update(){
    // once a frame however many scenes update (outside the engine's frame, the tests: every call)
    static uint64_t lastFrame = UINT64_MAX;
    if (Engine::isFrameRunning()){
        const uint64_t frame = SystemRender::getFrameCount();
        if (frame == lastFrame)
            return;
        lastFrame = frame;
    }
    std::vector<std::shared_ptr<HtmlUIPrewarmState>>& states = trackedPrewarms();
    lastUpdateMs = 0.0;
    if (states.empty())
        return;
    const auto start = std::chrono::steady_clock::now();
    HtmlUI::getFontEngine().updatePrewarm(prewarmBudgetMs);
    lastUpdateMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    std::vector<std::shared_ptr<HtmlUIPrewarmState>> finished;
    states.erase(std::remove_if(states.begin(), states.end(), [&finished](const std::shared_ptr<HtmlUIPrewarmState>& state){
        if (!state->done())
            return false;
        finished.push_back(state);
        return true;
    }), states.end());
    // (an onDone may start another prewarm)
    for (const std::shared_ptr<HtmlUIPrewarmState>& state : finished){
        state->notified = true;
        if (!state->onDone.isSet())
            continue;
        lua_State* L = LuaBinding::getLuaState();
        if (L && state->onDone.push(L))
            HtmlUILua::call(L, 0, "prewarm onDone");
        state->onDone.reset();
    }
}

void HtmlUIPrewarm::setBudgetMs(double milliseconds){
    prewarmBudgetMs = std::max(0.0, milliseconds);
}

double HtmlUIPrewarm::getBudgetMs(){
    return prewarmBudgetMs;
}

double HtmlUIPrewarm::getLastUpdateMs(){
    return lastUpdateMs;
}

// ---- UIPrewarm ----

UIPrewarm::UIPrewarm(): state(std::make_shared<HtmlUIPrewarmState>()){
    state->notified = true;
}

UIPrewarm::UIPrewarm(std::shared_ptr<HtmlUIPrewarmState> prewarmState): state(std::move(prewarmState)){
    if (!state){
        state = std::make_shared<HtmlUIPrewarmState>();
        state->notified = true;
    }
}

bool UIPrewarm::isDone() const{
    return state->done();
}

float UIPrewarm::getProgress() const{
    const size_t total = state->total();
    return total == 0 ? 1.0f : (float)state->ready() / (float)total;
}

UIPrewarm UIPrewarm::prewarmImages(const std::vector<std::string>& paths){
    return UIPrewarm(HtmlUIPrewarm::prewarmImages(paths));
}

UIPrewarm UIPrewarm::prewarmText(const UIPrewarmText& text){
    std::string error;
    std::shared_ptr<HtmlUIPrewarmState> made = HtmlUIPrewarm::prewarmText(text, &error);
    if (!error.empty())
        Log::error("[HTML UI] HtmlUI.prewarmText: %s", error.c_str());
    return UIPrewarm(made);
}

void UIPrewarm::setBudgetMs(double milliseconds){
    HtmlUIPrewarm::setBudgetMs(milliseconds);
}

double UIPrewarm::getBudgetMs(){
    return HtmlUIPrewarm::getBudgetMs();
}

const std::shared_ptr<HtmlUIPrewarmState>& UIPrewarm::getState() const{
    return state;
}
