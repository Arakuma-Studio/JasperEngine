// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UILottieElement.h"

#include "HtmlUIClock.h"
#include "HtmlUITexture.h"
#include "Log.h"
#include "render/SystemRender.h"
#include "object/ui/HtmlUIEventListener.h"
#include "object/ui/UIValue.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/PropertyIdSet.h>
#include <RmlUi/Core/RenderManager.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/Texture.h>

#include <thorvg.h>
#include <thorvg_lottie.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

using namespace jasperengine;

namespace {

    std::unique_ptr<Rml::ElementInstancerGeneric<UILottieElement>>& lottieInstancer(){
        static std::unique_ptr<Rml::ElementInstancerGeneric<UILottieElement>> value;
        return value;
    }

    const char* const playbackAttributes[] = {"speed", "direction", "mode", "loop", "segment", "marker"};

    std::string toLower(std::string text){
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return text;
    }

    std::string trim(const std::string& text){
        const size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return std::string();
        const size_t last = text.find_last_not_of(" \t\r\n");
        return text.substr(first, last - first + 1);
    }

    bool parseFloat(const std::string& text, float& value){
        const std::string trimmed = trim(text);
        if (trimmed.empty())
            return false;
        char* end = nullptr;
        const float parsed = std::strtof(trimmed.c_str(), &end);
        if (end != trimmed.c_str() + trimmed.size() || !std::isfinite(parsed))
            return false;
        value = parsed;
        return true;
    }

    // HTML boolean attribute: present means true, except an explicit "false"
    bool booleanAttribute(const std::string& value){
        return toLower(trim(value)) != "false";
    }

    std::string extensionOf(const std::string& path){
        const size_t slash = path.find_last_of("/\\");
        const size_t dot = path.find_last_of('.');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
            return std::string();
        return toLower(path.substr(dot + 1));
    }

    std::string directoryOf(const std::string& path){
        const size_t slash = path.find_last_of("/\\");
        return slash == std::string::npos ? std::string() : path.substr(0, slash);
    }

    const char* imageMimeType(const std::string& path){
        const std::string extension = extensionOf(path);
        if (extension == "png")
            return "png";
        if (extension == "jpg" || extension == "jpeg")
            return "jpg";
        return nullptr;
    }

    const char* eventName(LottiePlayback::Event event){
        switch (event){
            case LottiePlayback::Event::PLAY: return "play";
            case LottiePlayback::Event::PAUSE: return "pause";
            case LottiePlayback::Event::COMPLETE: return "complete";
            case LottiePlayback::Event::LOOP: return "loop";
        }
        return "";
    }

}

RMLUI_RTTI_Define(UILottieElement)

UILottieElement::UILottieElement(const Rml::String& tag): Rml::Element(tag){
}

UILottieElement::~UILottieElement(){
    releaseTexture();
    unloadAnimation();
}

void UILottieElement::registerInstancer(){
    if (!lottieInstancer())
        lottieInstancer() = std::make_unique<Rml::ElementInstancerGeneric<UILottieElement>>();
    Rml::Factory::RegisterElementInstancer("lottie", lottieInstancer().get());
}

void UILottieElement::releaseInstancer(){
    lottieInstancer().reset();
}

// ---------------------------------------------------------------- loading

void UILottieElement::unloadAnimation(){
    // the canvas holds a reference to the picture the animation owns
    canvas.reset();
    animation.reset();
    lottie = nullptr;
    loaded = false;
    canvasSize = Rml::Vector2i(0, 0);
    rasterizedFrame = -1.0f;
    frameDirty = true;
}

void UILottieElement::fail(const std::string& message){
    error = message;
    const std::string source = GetAttribute<Rml::String>("src", "");
    Log::error("[HTML UI] <lottie src=\"%s\">: %s", source.c_str(), message.c_str());
    queueEvent("error", message);
    holdPending();
}

void UILottieElement::ensureLoaded(){
    if (sourceDirty)
        load();
}

bool UILottieElement::load(){
    sourceDirty = false;
    unloadAnimation();
    dotLottie.reset();
    error.clear();
    resolvedPath.clear();
    assetDirectory.clear();
    animationId.clear();
    intrinsicSize = Rml::Vector2f(0, 0);
    // a new source starts from the defaults (speed 1, forward, normal, once)
    playback = LottiePlayback();
    playbackEvents.clear();
    releaseTexture();
    geometryDirty = true;

    const std::string source = trim(GetAttribute<Rml::String>("src", ""));
    if (source.empty())
        return false;

    // relative to the document, as <img> and RmlUi's own plugins resolve sources
    std::string path = source;
    if (Rml::ElementDocument* document = GetOwnerDocument()){
        const Rml::String documentUrl = Rml::StringUtilities::Replace(document->GetSourceURL(), '|', ':');
        Rml::GetSystemInterface()->JoinPath(path, documentUrl, source);
    }
    resolvedPath = path;

    Rml::String fileData;
    if (path.empty() || !Rml::GetFileInterface()->LoadFile(path, fileData)){
        fail("could not read '" + path + "'");
        return false;
    }

    std::string json;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(fileData.data());
    if (extensionOf(path) == "lottie" || DotLottie::looksLikeZip(bytes, fileData.size())){
        dotLottie = std::make_unique<DotLottie>();
        std::string zipError;
        if (!dotLottie->open(bytes, fileData.size(), zipError)){
            dotLottie.reset();
            fail("'" + path + "': " + zipError);
            return false;
        }
        const std::string requested = trim(GetAttribute<Rml::String>("animation", ""));
        if (!requested.empty() && !dotLottie->findAnimation(requested)){
            fail("'" + path + "' has no animation '" + requested + "'");
            return false;
        }
        animationId = requested.empty() ? dotLottie->getDefaultAnimationId() : requested;
        if (!dotLottie->readAnimation(animationId, json, zipError)){
            fail("'" + path + "': " + zipError);
            return false;
        }
    }else{
        json = std::move(fileData);
    }

    std::unique_ptr<tvg::LottieAnimation> created(tvg::LottieAnimation::gen());
    if (!created){
        fail("ThorVG could not create a Lottie animation");
        return false;
    }
    tvg::Picture* picture = created->picture();

    // external images (and fonts by name) go through the engine's file layer, then the archive
    assetDirectory = directoryOf(path);
    picture->resolver([](tvg::Paint* paint, const char* src, void* data) -> bool {
        return static_cast<UILottieElement*>(data)->resolveAsset(paint, src);
    }, this);

    if (picture->load(json.data(), (uint32_t)json.size(), "lottie+json", assetDirectory.c_str(), true) != tvg::Result::Success){
        fail("'" + path + "' is not a Lottie animation ThorVG can read");
        return false;
    }

    float width = 0.0f, height = 0.0f;
    picture->size(&width, &height);
    const float totalFrames = created->totalFrame();
    const float duration = created->duration();
    if (!(width > 0.0f) || !(height > 0.0f) || !(totalFrames > 0.0f) || !(duration > 0.0f)){
        fail("'" + path + "' has no size or no frames");
        return false;
    }

    std::vector<LottiePlayback::Marker> markers;
    const uint32_t markerCount = created->markersCnt();
    for (uint32_t i = 0; i < markerCount; i++){
        float begin = 0.0f, end = 0.0f;
        if (const char* name = created->marker(i, &begin, &end))
            markers.push_back({name, begin, end});
    }

    lottie = created.get();
    animation = std::move(created);
    intrinsicSize = Rml::Vector2f(width, height);
    loaded = true;
    stats.loads++;

    playback.setAnimation(totalFrames, totalFrames / duration);
    playback.setMarkers(std::move(markers));

    // manifest defaults first, then the attributes override them
    applyManifestSettings();
    for (const char* name : playbackAttributes){
        if (HasAttribute(name))
            applyAttribute(name, true);
    }

    queueEvent("load");

    bool autoplay = false;
    if (dotLottie){
        DotLottie::Settings settings = dotLottie->getSettings(animationId);
        autoplay = settings.autoplay.value_or(false);
    }
    if (HasAttribute("autoplay"))
        autoplay = booleanAttribute(GetAttribute<Rml::String>("autoplay", ""));
    // queued, not dispatched: loading may run inside a layout pass (GetIntrinsicDimensions);
    // OnUpdate dispatches them
    if (autoplay)
        playback.play(HtmlUIClock::now(), playbackEvents);
    queuePlaybackEvents();
    holdPending();

    DirtyLayout();
    return true;
}

bool UILottieElement::resolveAsset(void* paintPointer, const char* source){
    tvg::Paint* paint = static_cast<tvg::Paint*>(paintPointer);
    if (!source || !paint || paint->type() != tvg::Type::Picture)
        return false;   // fonts by name: ThorVG falls back to the built-in font
    tvg::Picture* picture = static_cast<tvg::Picture*>(paint);

    const std::string path = source;
    const char* mimeType = imageMimeType(path);

    // next to the animation file, through the engine's file layer
    Rml::String data;
    if (Rml::GetFileInterface()->LoadFile(path, data) && !data.empty()){
        if (picture->load(data.data(), (uint32_t)data.size(), mimeType, nullptr, true) == tvg::Result::Success)
            return true;
    }

    // inside the .lottie archive (images/ or i/)
    if (dotLottie){
        std::string relative = path;
        if (!assetDirectory.empty() && relative.compare(0, assetDirectory.size(), assetDirectory) == 0)
            relative = relative.substr(assetDirectory.size());
        std::string bytes;
        if (dotLottie->readImage(relative, bytes)){
            if (picture->load(bytes.data(), (uint32_t)bytes.size(), mimeType, nullptr, true) == tvg::Result::Success)
                return true;
        }
    }

    Log::warn("[HTML UI] <lottie src=\"%s\">: image '%s' not found", GetAttribute<Rml::String>("src", "").c_str(), path.c_str());
    return false;
}

void UILottieElement::applyManifestSettings(){
    if (!dotLottie)
        return;
    const DotLottie::Settings settings = dotLottie->getSettings(animationId);
    if (settings.speed)
        playback.setSpeed(*settings.speed);
    if (settings.direction)
        playback.setDirection(*settings.direction < 0 ? -1 : 1);
    if (settings.playMode)
        playback.setMode(toLower(*settings.playMode) == "bounce" ? LottiePlayback::Mode::BOUNCE : LottiePlayback::Mode::NORMAL);
    if (settings.loop){
        if (settings.loopCount && *settings.loopCount > 0)
            playback.setLoopCount(*settings.loopCount);
        else
            playback.setLoop(*settings.loop);
    }
}

void UILottieElement::applyAttribute(const std::string& name, bool atLoad){
    const bool present = HasAttribute(name);
    const std::string value = trim(GetAttribute<Rml::String>(name, ""));
    const std::string source = GetAttribute<Rml::String>("src", "");

    if (name == "speed"){
        float speed = 1.0f;
        if (present && (!parseFloat(value, speed) || !(speed > 0.0f))){
            Log::warn("[HTML UI] <lottie src=\"%s\">: speed=\"%s\" is not a number > 0, using 1", source.c_str(), value.c_str());
            speed = 1.0f;
        }
        playback.setSpeed(speed);
    }else if (name == "direction"){
        int direction = 1;
        if (present){
            if (value == "-1"){
                direction = -1;
            }else if (value != "1" && !value.empty()){
                Log::warn("[HTML UI] <lottie src=\"%s\">: direction=\"%s\" must be 1 or -1, using 1", source.c_str(), value.c_str());
            }
        }
        playback.setDirection(direction);
    }else if (name == "mode"){
        const std::string mode = toLower(value);
        if (present && mode != "normal" && mode != "bounce" && !mode.empty()){
            Log::warn("[HTML UI] <lottie src=\"%s\">: mode=\"%s\" must be normal or bounce", source.c_str(), value.c_str());
        }
        playback.setMode(mode == "bounce" ? LottiePlayback::Mode::BOUNCE : LottiePlayback::Mode::NORMAL);
    }else if (name == "loop"){
        if (!present){
            playback.setLoop(false);
        }else{
            int count = 0;
            const std::string lower = toLower(value);
            if (lower.empty() || lower == "true" || lower == "loop"){
                playback.setLoop(true);
            }else if (lower == "false"){
                playback.setLoop(false);
            }else{
                char* end = nullptr;
                const long parsed = std::strtol(value.c_str(), &end, 10);
                if (end == value.c_str() + value.size()){
                    count = (int)std::clamp(parsed, 0L, 1000000L);
                    playback.setLoopCount(count);   // <= 0: forever
                }else{
                    Log::warn("[HTML UI] <lottie src=\"%s\">: loop=\"%s\" must be empty, true, false or a count", source.c_str(), value.c_str());
                    playback.setLoop(true);
                }
            }
        }
    }else if (name == "segment"){
        if (!present || value.empty()){
            if (!atLoad && !HasAttribute("marker"))
                playback.clearSegment();
            return;
        }
        float start = 0.0f, end = 0.0f;
        const size_t split = value.find_first_of(" \t,");
        if (split == std::string::npos || !parseFloat(value.substr(0, split), start) || !parseFloat(value.substr(split + 1), end)){
            Log::warn("[HTML UI] <lottie src=\"%s\">: segment=\"%s\" must be \"start end\" in frames", source.c_str(), value.c_str());
            return;
        }
        playback.setSegment(start, end);
    }else if (name == "marker"){
        if (!present || value.empty()){
            if (!atLoad)
                playback.clearSegment();
            return;
        }
        if (!playback.setMarker(value)){
            std::string names;
            for (const LottiePlayback::Marker& marker : playback.getMarkers())
                names += (names.empty() ? "" : ", ") + marker.name;
            Log::warn("[HTML UI] <lottie src=\"%s\">: no marker \"%s\" (markers: %s)", source.c_str(), value.c_str(),
                      names.empty() ? "none" : names.c_str());
        }
    }
    frameDirty = true;
}

// ---------------------------------------------------------------- events

void UILottieElement::queueEvent(const std::string& type, const std::string& message){
    pendingEvents.push_back({type, message});
}

void UILottieElement::holdPending(){
    // Loading runs inside LoadDocument/Show (the document's own updates) or a layout pass,
    // before the document's scripts could add a listener. Like an <img> load event, the events
    // of a load are dispatched asynchronously: by an OnUpdate of a later engine frame
    // (SystemRender::getFrameCount), or at once by the next control call.
    holding = true;
    holdFrame = SystemRender::getFrameCount();
}

void UILottieElement::dispatchPending(){
    holding = false;
    if (pendingEvents.empty())
        return;
    // listeners may change this element (or remove it): dispatch a copy
    std::vector<PendingEvent> events;
    events.swap(pendingEvents);
    for (const PendingEvent& event : events){
        Rml::Dictionary parameters;
        // the script API's event.detail (p1-lua): { message = ... } for errors
        UIValue detail;
        if (!event.message.empty()){
            parameters["message"] = event.message;
            detail = UIValue::object();
            detail["message"] = UIValue(event.message);
            parameters[HtmlUIEventNames::detailParameter] = (void*)&detail;
        }
        // media-element events: they do not bubble (capture listeners still see them)
        DispatchEvent(event.type, parameters, false, false);
    }
}

void UILottieElement::queuePlaybackEvents(){
    for (LottiePlayback::Event event : playbackEvents)
        queueEvent(eventName(event));
    playbackEvents.clear();
}

void UILottieElement::dispatchPlaybackEvents(){
    queuePlaybackEvents();
    dispatchPending();
}

void UILottieElement::advance(){
    // events stay queued until the caller (or the next OnUpdate) dispatches them
    playback.update(HtmlUIClock::now(), playbackEvents);
    if (playback.getCurrentFrame() != rasterizedFrame)
        frameDirty = true;
}

void UILottieElement::afterControl(){
    dispatchPlaybackEvents();
    if (playback.getCurrentFrame() != rasterizedFrame)
        frameDirty = true;
    if (Rml::Context* context = GetContext())
        context->RequestNextUpdate(0.0);
}

// ---------------------------------------------------------------- RmlUi hooks

bool UILottieElement::GetIntrinsicDimensions(Rml::Vector2f& dimensions, float& ratio){
    ensureLoaded();

    dimensions = intrinsicSize;
    if (dimensions.y > 0.0f)
        ratio = dimensions.x / dimensions.y;
    dimensions *= Rml::ElementUtilities::GetDensityIndependentPixelRatio(this);
    return true;
}

void UILottieElement::OnUpdate(){
    ensureLoaded();

    if (loaded){
        advance();
        queuePlaybackEvents();
    }

    // in order; held while the events of a load wait for a later frame (holdPending)
    if (!holding || SystemRender::getFrameCount() != holdFrame)
        dispatchPending();

    if (!pendingEvents.empty()){
        if (Rml::Context* context = GetContext())
            context->RequestNextUpdate(0.0);
    }
    if (loaded && playback.isPlaying() && IsVisible(true)){
        if (Rml::Context* context = GetContext())
            context->RequestNextUpdate(playback.getTimeToNextFrame());
    }
}

void UILottieElement::OnResize(){
    geometryDirty = true;
}

void UILottieElement::OnAttributeChange(const Rml::ElementAttributes& changed_attributes){
    Rml::Element::OnAttributeChange(changed_attributes);

    if (changed_attributes.count("src") || changed_attributes.count("animation")){
        sourceDirty = true;
        DirtyLayout();
        return;
    }
    if (!loaded)
        return;

    advance();
    for (const char* name : playbackAttributes){
        if (changed_attributes.count(name))
            applyAttribute(name, false);
    }
    afterControl();
}

void UILottieElement::OnPropertyChange(const Rml::PropertyIdSet& changed_properties){
    Rml::Element::OnPropertyChange(changed_properties);

    if (changed_properties.Contains(Rml::PropertyId::ImageColor) || changed_properties.Contains(Rml::PropertyId::Opacity))
        geometryDirty = true;
}

void UILottieElement::layoutPicture(){
    tvg::Picture* picture = animation->picture();
    // contain: the largest uniform scale that fits, centred
    const float scale = std::min(canvasSize.x / intrinsicSize.x, canvasSize.y / intrinsicSize.y);
    const float width = intrinsicSize.x * scale;
    const float height = intrinsicSize.y * scale;
    picture->size(width, height);
    picture->translate((canvasSize.x - width) * 0.5f, (canvasSize.y - height) * 0.5f);
}

void UILottieElement::rasterize(){
    if (!canvas){
        canvas.reset(tvg::SwCanvas::gen());
        if (!canvas){
            fail("ThorVG could not create a canvas");
            return;
        }
        canvas->add(animation->picture());
    }

    animation->frame(playback.getCurrentFrame());
    canvas->update();
    canvas->draw(true);
    canvas->sync();

    rasterizedFrame = playback.getCurrentFrame();
    stats.rasterizations++;
}

void UILottieElement::generateGeometry(){
    Rml::RenderManager* renderManager = GetRenderManager();
    if (!renderManager)
        return;

    const Rml::ComputedValues& computed = GetComputedValues();
    const Rml::ColourbPremultiplied colour = computed.image_color().ToPremultiplied(GetDrawOpacity());

    Rml::Mesh mesh = geometry.Release(Rml::Geometry::ReleaseMode::ClearMesh);
    Rml::MeshUtilities::GenerateQuad(mesh, Rml::Vector2f(0, 0), Rml::Vector2f(renderSize), colour, Rml::Vector2f(0, 0), Rml::Vector2f(1, 1));
    geometry = renderManager->MakeGeometry(std::move(mesh));
    geometryDirty = false;
}

void UILottieElement::releaseTexture(){
    // RmlUi gives the handle back to the renderer, which deletes the HtmlUITexture
    texture.Release();
    liveTexture = nullptr;
    textureSize = Rml::Vector2i(0, 0);
}

bool UILottieElement::createTexture(const Rml::CallbackTextureInterface& textureInterface){
    liveTexture = nullptr;
    std::unique_ptr<HtmlUITexture> created = HtmlUITexture::createDynamic("htmlui-lottie", canvasSize.x, canvasSize.y);
    if (!created)
        return false;

    stats.texturesCreated++;
    HtmlUITexture* raw = created.release();
    textureInterface.SetTextureHandle(reinterpret_cast<Rml::TextureHandle>(raw), canvasSize);
    liveTexture = raw;

    // the current frame goes up with the new texture
    if (raw->update(pixels.data(), pixels.size() * sizeof(uint32_t))){
        stats.uploads++;
        uploadPending = false;
    }else{
        uploadPending = true;
    }
    return true;
}

void UILottieElement::OnRender(){
    if (!loaded || !animation)
        return;

    const Rml::Vector2f content = GetBox().GetSize(Rml::BoxArea::Content).Round();
    const Rml::Vector2i size((int)content.x, (int)content.y);
    if (size.x <= 0 || size.y <= 0)
        return;

    if (size != renderSize){
        renderSize = size;
        geometryDirty = true;
    }

    // a new size: a new canvas target and a new texture (the old one goes back to the renderer)
    if (size != canvasSize){
        canvasSize = size;
        pixels.assign((size_t)size.x * (size_t)size.y, 0u);
        if (!canvas){
            canvas.reset(tvg::SwCanvas::gen());
            if (!canvas){
                fail("ThorVG could not create a canvas");
                return;
            }
            canvas->add(animation->picture());
        }
        canvas->target(pixels.data(), (uint32_t)size.x, (uint32_t)size.x, (uint32_t)size.y, tvg::ColorSpace::ABGR8888);
        layoutPicture();
        frameDirty = true;
        releaseTexture();
    }

    if (geometryDirty)
        generateGeometry();

    if (frameDirty || playback.getCurrentFrame() != rasterizedFrame){
        rasterize();
        frameDirty = false;
        uploadPending = true;
    }

    Rml::RenderManager* renderManager = GetRenderManager();
    if (!renderManager)
        return;

    if (!texture){
        texture = renderManager->MakeCallbackTexture([this](const Rml::CallbackTextureInterface& textureInterface) -> bool {
            return createTexture(textureInterface);
        });
        textureSize = canvasSize;
    }

    // Loads the texture if RmlUi has none (first draw, or after view loss gave every texture
    // back): the callback creates it and uploads the current frame. Only then is liveTexture
    // known to be alive.
    Rml::Texture drawTexture = texture;
    if (drawTexture.GetDimensions() != canvasSize || !liveTexture)
        return;

    if (uploadPending && liveTexture->update(pixels.data(), pixels.size() * sizeof(uint32_t))){
        stats.uploads++;
        uploadPending = false;
    }

    geometry.Render(GetAbsoluteOffset(Rml::BoxArea::Content).Round(), drawTexture);
}

void UILottieElement::ProcessDefaultAction(Rml::Event& event){
    Rml::Element::ProcessDefaultAction(event);

    // phase-2 hook H9: playonhover
    if (!HasAttribute("playonhover") || !booleanAttribute(GetAttribute<Rml::String>("playonhover", "")))
        return;
    if (event == Rml::EventId::Mouseover){
        if (!isPlaying())
            play();
    }else if (event == Rml::EventId::Mouseout){
        if (isPlaying())
            stop();
    }
}

// ---------------------------------------------------------------- control

void UILottieElement::play(){
    ensureLoaded();
    if (!loaded)
        return;
    advance();
    playback.play(HtmlUIClock::now(), playbackEvents);
    afterControl();
}

void UILottieElement::pause(){
    ensureLoaded();
    if (!loaded)
        return;
    advance();
    playback.pause(HtmlUIClock::now(), playbackEvents);
    afterControl();
}

void UILottieElement::stop(){
    ensureLoaded();
    if (!loaded)
        return;
    advance();
    playback.stop(HtmlUIClock::now(), playbackEvents);
    afterControl();
}

void UILottieElement::seek(float seconds){
    ensureLoaded();
    if (!loaded)
        return;
    advance();
    playback.seek(seconds, HtmlUIClock::now());
    afterControl();
}

void UILottieElement::setFrame(float frame){
    ensureLoaded();
    if (!loaded)
        return;
    advance();
    playback.setFrame(frame, HtmlUIClock::now());
    afterControl();
}

void UILottieElement::setSpeed(float speed){
    ensureLoaded();
    if (!(speed > 0.0f) || !std::isfinite(speed)){
        Log::warn("[HTML UI] <lottie>: setSpeed(%g) needs a speed > 0", speed);
        return;
    }
    if (!loaded)
        return;
    advance();
    playback.setSpeed(speed);
    afterControl();
}

void UILottieElement::setDirection(int direction){
    ensureLoaded();
    if (direction != 1 && direction != -1){
        Log::warn("[HTML UI] <lottie>: setDirection(%i) needs 1 or -1", direction);
        return;
    }
    if (!loaded)
        return;
    advance();
    playback.setDirection(direction);
    afterControl();
}

void UILottieElement::setLoop(bool loop){
    ensureLoaded();
    if (!loaded)
        return;
    advance();
    playback.setLoop(loop);
    afterControl();
}

void UILottieElement::setLoopCount(int count){
    ensureLoaded();
    if (!loaded)
        return;
    advance();
    playback.setLoopCount(count);
    afterControl();
}

void UILottieElement::setSegment(float startFrame, float endFrame){
    ensureLoaded();
    if (!loaded)
        return;
    advance();
    playback.setSegment(startFrame, endFrame);
    afterControl();
}

bool UILottieElement::setMarker(const std::string& marker){
    ensureLoaded();
    if (!loaded)
        return false;
    advance();
    const bool found = playback.setMarker(marker);
    if (!found)
        Log::warn("[HTML UI] <lottie src=\"%s\">: no marker \"%s\"", GetAttribute<Rml::String>("src", "").c_str(), marker.c_str());
    afterControl();
    return found;
}

bool UILottieElement::isPlaying(){
    ensureLoaded();
    if (loaded)
        advance();
    return loaded && playback.isPlaying();
}

float UILottieElement::getCurrentFrame(){
    ensureLoaded();
    if (loaded)
        advance();
    return loaded ? playback.getCurrentFrame() : 0.0f;
}

float UILottieElement::getTotalFrames(){
    ensureLoaded();
    return loaded ? playback.getTotalFrames() : 0.0f;
}

float UILottieElement::getDuration(){
    ensureLoaded();
    return loaded ? playback.getDuration() : 0.0f;
}

bool UILottieElement::isLoaded(){
    ensureLoaded();
    return loaded;
}

const std::string& UILottieElement::getError() const{
    return error;
}

std::string UILottieElement::getResolvedSource() const{
    return resolvedPath;
}

const LottiePlayback& UILottieElement::getPlayback() const{
    return playback;
}

const UILottieElement::Stats& UILottieElement::getStats() const{
    return stats;
}

HtmlUITexture* UILottieElement::getTexture() const{
    return liveTexture;
}

Rml::Vector2i UILottieElement::getRenderSize() const{
    return renderSize;
}
