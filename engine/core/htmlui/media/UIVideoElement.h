// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIVIDEOELEMENT_H
#define UIVIDEOELEMENT_H

#include "Export.h"
#include "media/VideoAudio.h"
#include "media/VideoPlayback.h"
#include "media/VideoStream.h"
#include "media/VideoTypes.h"

#include <RmlUi/Core/CallbackTexture.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/Texture.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {

    struct HtmlUITexture;
    class UIVideoControls;

    // <video src autoplay loop muted controls poster preload> (docs/html-ui/phase-6-video.md): AV1 +
    // Opus WebM decoded by a VideoStream, drawn as a replaced element (object-fit and
    // object-position, rounded content, image-color x opacity, as the fork's <img>), on the UI clock
    // (HtmlUIClock). HTMLMediaElement's states and events, the <source> choice, the poster, the
    // audio through SoLoud in sync with the picture, and the default controls (UIVideoControls).
    //
    // Frames are converted on the GPU: the element uploads a frame's Y/U/V planes (R8 textures owned
    // by its render-target texture) and queues one HTMLUI_POST Yuv pass on its context's RmlRenderer
    // (queueVideoConversion), which runs before the context draws. A context on another render
    // interface (the tests' recording one) gets the frame converted on the CPU into a dynamic RGBA
    // texture instead.
    //
    // Frame-exact mode (the UI clock pinned or a fixed frame delta: tests, goldens, MCP fixed_fps):
    // the frame of the media time is waited for. Real time: never waits (waiting/playing, dropped
    // frames).
    class JASPERENGINE_INTERNAL UIVideoElement : public Rml::Element {
    public:
        RMLUI_RTTI_DeclareWithParent(UIVideoElement, Rml::Element)

        static constexpr int HAVE_NOTHING = 0, HAVE_METADATA = 1, HAVE_CURRENT_DATA = 2, HAVE_FUTURE_DATA = 3, HAVE_ENOUGH_DATA = 4;
        static constexpr int NETWORK_EMPTY = 0, NETWORK_IDLE = 1, NETWORK_LOADING = 2, NETWORK_NO_SOURCE = 3;

        explicit UIVideoElement(const Rml::String& tag);
        ~UIVideoElement() override;

        // the "video" instancer: HtmlUI::acquire registers it after every Rml::Initialise,
        // HtmlUI::release frees it after Rml::Shutdown (as <lottie>'s)
        static void registerInstancer();
        static void releaseInstancer();

        bool GetIntrinsicDimensions(Rml::Vector2f& dimensions, float& ratio) override;

        // HTMLMediaElement (docs/html-ui/phase-6-video.md §3.9)
        void play();
        void pause();
        void load();
        static std::string canPlayType(const std::string& type);
        void fastSeek(double seconds);
        double getCurrentTime();
        void setCurrentTime(double seconds);
        double getDuration();                 // NaN before the metadata
        bool isPaused();
        bool isEnded();
        bool isSeeking();
        bool isMuted() const;
        void setMuted(bool muted);
        float getVolume() const;
        bool setVolume(float volume);         // false (warning) outside [0, 1]
        double getPlaybackRate();
        bool setPlaybackRate(double rate);    // false (warning) outside 0 / [1/16, 16]
        double getDefaultPlaybackRate();
        bool setDefaultPlaybackRate(double rate);
        int getReadyState();
        int getNetworkState();
        int getVideoWidth();
        int getVideoHeight();
        std::string getCurrentSrc() const;
        const MediaError& getError() const;
        // the reflected attributes
        bool getLoop() const;
        void setLoop(bool loop);
        bool getAutoplay() const;
        void setAutoplay(bool autoplay);
        bool getControls() const;
        void setControls(bool controls);
        bool getDefaultMuted() const;
        void setDefaultMuted(bool muted);
        std::string getPoster() const;
        void setPoster(const std::string& poster);
        std::string getPreload() const;       // "none", "metadata" or "auto" (the attribute's state)
        void setPreload(const std::string& preload);

        // for tooling (inspect_ui) and tests
        struct Stats {
            uint64_t framesShown = 0;        // frames uploaded for display
            uint64_t uploads = 0;            // plane (or RGBA) uploads
            uint64_t conversionsQueued = 0;  // GPU conversion jobs
            uint64_t cpuConversions = 0;     // CPU conversions (another render interface)
            uint64_t texturesCreated = 0;
            uint64_t streamsCreated = 0;
        };
        const Stats& getStats() const;
        VideoStream::Stats getStreamStats() const;
        int getShownFrameIndex() const;      // the index of the frame on screen (-1: none)
        int64_t getShownSequence() const;
        bool isShowingPoster() const;
        bool isFrameExact() const;           // the mode of the last update
        HtmlUITexture* getTexture() const;   // the frame's texture (nullptr before the first frame)
        double getDecodeMsPerFrame() const;
        size_t getMemoryBytes() const;       // decoder pictures, queue, read cache, GPU planes and target
        UIVideoControls* getControlsWidget() const;
        // the audio the element plays (nullptr without an audio track or before the metadata)
        std::shared_ptr<VideoAudioRing> getAudioRing() const;
        // the drift of the last sync step (seconds, audio minus picture)
        double getAudioDrift() const;

        // Tests: no SoLoud voice is made; a test reads getAudioRing() as the mixer would, with
        // `latencyFrames` of output latency (the sync controller runs as with a voice)
        static void setAudioConsumerForTesting(bool external, int64_t latencyFrames = 0);
        // elements alive (leak tests)
        static int getLiveCount();

    protected:
        void OnUpdate() override;
        void OnRender() override;
        void OnResize() override;
        void OnLayout() override;
        void OnAttributeChange(const Rml::ElementAttributes& changed_attributes) override;
        void OnPropertyChange(const Rml::PropertyIdSet& changed_properties) override;
        void OnChildAdd(Rml::Element* child) override;
        void OnChildRemove(Rml::Element* child) override;
        void ProcessDefaultAction(Rml::Event& event) override;

    private:
        friend class UIVideoControls;

        struct Candidate {
            std::string path;               // resolved (asset path)
            std::string attribute;          // the src as written
            Rml::ObserverPtr<Rml::Element> source;   // the <source>, null for the src attribute
        };
        struct PendingEvent {
            std::string type;
            std::string message;
            Rml::ObserverPtr<Rml::Element> target;  // null: this element
        };

        // loading
        void ensureLoaded();
        void runLoad();
        void selectCandidates();
        bool startFetch();
        void tryNextCandidate(const std::string& reason);
        void failSource(const std::string& message, MediaErrorCode code);
        void pollStream();
        void onMetadata();
        void resetMedia(bool fireEmptied);
        std::string resolvePath(const std::string& source) const;

        // playback
        bool frameExactNow() const;
        void advance(bool fromUpdate);
        void showFrameForTime();
        void handlePlaybackEvents();
        void afterControl();
        void internalPause();

        // audio
        void updateAudio();
        float effectiveVolume() const;

        // events
        void queueEvent(const std::string& type, const std::string& message = "", Rml::Element* target = nullptr);
        void holdPending();
        void dispatchPending();

        // drawing
        bool ensureTexture();
        void releaseTexture();
        bool createTexture(const Rml::CallbackTextureInterface& textureInterface);
        void uploadFrame();
        void loadPoster();
        void generateGeometry(Rml::Texture texture, Rml::Vector2f intrinsic);

        // controls
        void updateControls();

        // the resource
        bool loadPending = true;           // the load algorithm runs at the next ensureLoaded
        bool fetchStarted = false;         // a stream was asked for (preload none waits)
        bool fetchWanted = false;          // play() or a seek asked for the data (preload none)
        std::vector<Candidate> candidates;
        size_t candidateIndex = 0;
        std::string currentSrc;
        std::unique_ptr<VideoStream> stream;
        bool metadataSent = false;
        bool dataSent = false;
        bool autoplayPending = false;
        bool autoplayFlag = true;          // HTML's "can autoplay flag": play()/pause() clear it
        bool mutedInitialized = false;     // muted takes the attribute once, at the first load
        int networkState = NETWORK_EMPTY;
        int readyState = HAVE_NOTHING;
        MediaError error;
        Rml::Vector2f intrinsicSize{0.0f, 0.0f};   // the video's display size (0 before the metadata)

        // playback
        VideoPlayback playback;
        double pendingSeek = -1.0;         // a seek asked for before the metadata
        bool seekToStart = false;          // play() after the end, or a source swapped
        bool showPoster = true;            // HTML's "show poster flag"
        bool frameExact = false;
        bool muted = false;
        float volume = 1.0f;
        std::vector<VideoPlayback::Event> playbackEvents;
        int64_t stallSequence = -1;        // real time: the frame waited for

        // events
        std::vector<PendingEvent> pendingEvents;
        bool holding = false;
        uint64_t holdFrame = 0;

        // the frame on screen
        VideoFrame frame;
        bool hasPicture = false;           // a frame was converted into the texture
        bool uploadPending = false;        // `frame` is not in the texture yet
        int64_t shownSequence = -1;
        bool hdrWarned = false;            // the HDR warning was logged (once per element)
        uint64_t uploadedFrame = UINT64_MAX;

        // RmlUi resources
        Rml::CallbackTexture texture;
        HtmlUITexture* liveTexture = nullptr;      // owned by the renderer (ReleaseTexture deletes it)
        Rml::Vector2i textureSize{0, 0};
        bool gpuConversion = true;                 // the context's render interface is an RmlRenderer
        Rml::Texture posterTexture;
        std::string posterSource;
        Rml::Vector2f posterSize{0.0f, 0.0f};
        bool posterDirty = true;
        Rml::ReplacedContentGeometry geometry;
        bool geometryDirty = true;
        bool geometryForPoster = false;            // the geometry was made for the poster

        // audio
        std::unique_ptr<VideoAudioSource> audioSource;
        double lastAudioNow = -1.0;
        double audioDrift = 0.0;
        bool audioSynced = false;
        // the drift of the last updates: corrections follow its mean, not one mixer block's jitter
        static constexpr int DRIFT_SAMPLES = 8;
        double driftSamples[DRIFT_SAMPLES] = {};
        int driftCount = 0;

        std::unique_ptr<UIVideoControls> controls;
        Stats stats;
    };

}

#endif //UIVIDEOELEMENT_H
