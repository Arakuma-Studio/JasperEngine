// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VIDEO_PLAYER_SYSTEM_H
#define VIDEO_PLAYER_SYSTEM_H

// Runs the scene's VideoPlayerComponents (docs/superpowers/specs/2026-10-31-video-mp4-design.md
// §6.4): opens their source (media/VideoSourceFactory: WebM AV1 in software, MP4 through the
// platform's decoders, URLs through the media cache), keeps each one's clock (scaled or unscaled
// game time x playbackSpeed, Unity's GameTime / UnscaledGameTime), hands the frame of the clock's
// time to its render texture (VideoTextureConverter, converted by RenderSystem before the frame's
// passes), keeps the sound in sync (a SoLoud voice: direct, or through an AudioSource's 3D
// settings), overrides a material's texture (MaterialOverride, restored when it stops) and fires
// the events (prepareCompleted, started, loopPointReached, errorReceived, seekCompleted,
// frameReady) on the main thread during its update, after every player of the frame moved.
// RenderSystem draws the camera planes (drawCameraPlanes).
//
// playOnAwake: prepared and played at the first update the player is enabled and active (in the
// editor, the first update of Play). A changed clip/url stops the player (isPrepared false);
// playOnAwake does not start it again. An entity destroyed (or its scene unloaded) closes its
// source, stops its voice and joins its decoder thread; a URL's download goes on only for the
// other players that want it (the media cache).

#include "ecs/SubSystem.h"
#include "component/VideoPlayerComponent.h"
#include "math/Rect.h"
#include "render/PipelineState.h"

#include <cstdint>
#include <memory>
#include <string>

namespace jasperengine{

    class CameraRender;
    class VideoTextureConverter;

    class JASPERENGINE_API VideoPlayerSystem : public SubSystem {
    public:
        VideoPlayerSystem(Scene* scene);
        virtual ~VideoPlayerSystem();

        void load() override;
        void draw() override;
        void destroy() override;
        void update(double dt) override;
        void setPaused(bool value) override;

        void onComponentAdded(Entity entity, ComponentId componentId) override;
        void onComponentRemoved(Entity entity, ComponentId componentId) override;

        // control (VideoPlayer); a player without a clip/url does nothing (a warning)
        void prepare(Entity entity);
        void play(Entity entity);           // prepares first when needed: plays once prepared
        void pause(Entity entity);
        void stop(Entity entity);           // to time 0, the decoder released; the texture keeps its last frame
        // every player stopped and not yet awoken (playOnAwake runs at the next update): the
        // editor's Play stop, which keeps the scene
        void resetPlayers();
        // EntityCloner::move: `entity`'s playback (its source, clock, sound, events) goes on in
        // `to`'s scene as `toEntity` (a persistent player keeps playing across scene loads); the
        // component there shares the same render texture
        void handOver(Entity entity, VideoPlayerSystem& to, Entity toEntity);
        void stepForward(Entity entity);    // paused: the next frame
        void setTime(Entity entity, double seconds);
        void setFrame(Entity entity, int64_t frame);

        struct Status {
            bool prepared = false;
            bool preparing = false;
            bool playing = false;
            bool paused = false;
            bool canSetTime = false;
            double time = 0.0;
            int64_t frame = -1;             // the frame shown (-1: none)
            int64_t frameCount = 0;
            double frameRate = 0.0;
            double length = 0.0;
            int width = 0, height = 0;      // displayed size
            double prepareProgress = 0.0;   // 0..1: the download (a URL), then the opening
            double bufferedSeconds = 0.0;
            std::string backend;            // "webm", "ffmpeg", ...
            std::string videoCodec;         // display names ("H.264", "AV1")
            std::string audioCodec;
            std::string error;              // the last error ("" when none)
        };
        Status getStatus(Entity entity) const;

        // the player's render texture (made when missing: links bind it before anything plays)
        VideoTextureConverter* getOutput(Entity entity);

        // RenderSystem, before the frame's first pass: the frames of this update into their textures
        void processConversions();
        // RenderSystem, in a colour pass of `camera`: the players drawn on its far (or near)
        // plane (targetCamera, NULL_ENTITY: the main camera), over the pass's viewport
        bool hasCameraPlanes(bool farPlane) const;
        void drawCameraPlanes(Entity camera, bool farPlane, const RenderTargetFormat& target, CameraRender& pass, const Rect& viewport, bool flipY);

        // tests: no SoLoud voice is made (the audio ring is read as the mixer would)
        static void setAudioOutputForTesting(bool enabled);
        // players with an open source (leak tests)
        int getOpenSources() const;

    private:
        struct State;
        State* state;
    };

}

#endif // VIDEO_PLAYER_SYSTEM_H
