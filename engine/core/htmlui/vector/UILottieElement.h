// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UILOTTIEELEMENT_H
#define UILOTTIEELEMENT_H

#include "Export.h"
#include "LottiePlayback.h"
#include "DotLottie.h"

#include <RmlUi/Core/CallbackTexture.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Geometry.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tvg {
    struct Animation;
    struct LottieAnimation;
    struct SwCanvas;
}

namespace jasperengine {

    struct HtmlUITexture;

    // <lottie src="anim.json|anim.lottie" autoplay loop speed direction mode segment marker
    // animation>: a Lottie animation rendered by ThorVG (CPU) into ONE dynamic RGBA8 texture
    // per element (HtmlUITexture DYNAMIC), updated in place, and only when the shown frame
    // changes or the element is resized. Playback time is the UI clock (HtmlUIClock).
    // Events on the element: load, error (parameter "message"), play, pause, loop, complete.
    // The content is fitted into the content box keeping its aspect ratio, centred, and
    // tinted by image-color x opacity like <img> and <svg>.
    class JASPERENGINE_INTERNAL UILottieElement : public Rml::Element {
    public:
        RMLUI_RTTI_DeclareWithParent(UILottieElement, Rml::Element)

        explicit UILottieElement(const Rml::String& tag);
        ~UILottieElement() override;

        // the "lottie" instancer: HtmlUI::acquire registers it after every Rml::Initialise
        // (RmlUi forgets instancers at shutdown), HtmlUI::release frees it after Rml::Shutdown
        static void registerInstancer();
        static void releaseInstancer();

        bool GetIntrinsicDimensions(Rml::Vector2f& dimensions, float& ratio) override;

        // control (UIElement's Lottie API); every call loads the source if it is pending
        void play();
        void pause();
        void stop();
        void seek(float seconds);
        void setFrame(float frame);
        void setSpeed(float speed);
        void setDirection(int direction);
        void setLoop(bool loop);
        void setLoopCount(int count);
        void setSegment(float startFrame, float endFrame);
        bool setMarker(const std::string& marker);
        bool isPlaying();
        float getCurrentFrame();
        float getTotalFrames();
        float getDuration();

        bool isLoaded();                         // an animation is loaded
        const std::string& getError() const;     // the last load error ("" = none)
        std::string getResolvedSource() const;   // the path the source was loaded from
        const LottiePlayback& getPlayback() const;

        // counters for tests and statistics
        struct Stats {
            uint32_t loads = 0;           // successful animation loads
            uint32_t rasterizations = 0;  // ThorVG frames drawn into the CPU buffer
            uint32_t uploads = 0;         // in-place texture updates (HtmlUITexture::update)
            uint32_t texturesCreated = 0; // HtmlUITexture::createDynamic calls
        };
        const Stats& getStats() const;
        HtmlUITexture* getTexture() const;       // the texture of the last draw (nullptr before)
        Rml::Vector2i getRenderSize() const;     // content box in pixels at the last draw

    protected:
        void OnUpdate() override;
        void OnRender() override;
        void OnResize() override;
        void OnAttributeChange(const Rml::ElementAttributes& changed_attributes) override;
        void OnPropertyChange(const Rml::PropertyIdSet& changed_properties) override;

    private:
        void ensureLoaded();
        bool load();
        void unloadAnimation();
        void fail(const std::string& message);
        void applyAttribute(const std::string& name, bool atLoad);
        void applyManifestSettings();
        // brings the playback to the UI clock's now and dispatches what happened
        void advance();
        void queuePlaybackEvents();
        void dispatchPlaybackEvents();
        void dispatchPending();
        void holdPending();
        void queueEvent(const std::string& type, const std::string& message = "");
        void afterControl();
        void layoutPicture();
        void rasterize();
        void generateGeometry();
        void releaseTexture();
        bool createTexture(const Rml::CallbackTextureInterface& textureInterface);
        bool resolveAsset(void* paint, const char* source);

        LottiePlayback playback;
        std::vector<LottiePlayback::Event> playbackEvents;
        struct PendingEvent {
            std::string type;
            std::string message;
        };
        std::vector<PendingEvent> pendingEvents;
        bool holding = false;            // pending events wait for a frame after holdFrame (holdPending)
        uint64_t holdFrame = 0;

        // source
        bool sourceDirty = true;
        bool loaded = false;
        std::string error;
        std::string resolvedPath;
        std::string assetDirectory;     // the rpath ThorVG prefixes to external asset paths
        std::unique_ptr<DotLottie> dotLottie;
        std::string animationId;
        Rml::Vector2f intrinsicSize{0, 0};

        // ThorVG
        std::unique_ptr<tvg::Animation> animation;
        tvg::LottieAnimation* lottie = nullptr;   // the same object when it is a LottieAnimation
        std::unique_ptr<tvg::SwCanvas> canvas;
        std::vector<uint32_t> pixels;             // ABGR8888: premultiplied R, G, B, A bytes
        Rml::Vector2i canvasSize{0, 0};
        float rasterizedFrame = -1.0f;
        bool frameDirty = true;
        bool uploadPending = false;

        // RmlUi resources
        Rml::CallbackTexture texture;
        HtmlUITexture* liveTexture = nullptr;     // owned by the renderer (ReleaseTexture deletes it)
        Rml::Vector2i textureSize{0, 0};
        Rml::Geometry geometry;
        bool geometryDirty = true;
        Rml::Vector2i renderSize{0, 0};

        Stats stats;
    };

}

#endif //UILOTTIEELEMENT_H
