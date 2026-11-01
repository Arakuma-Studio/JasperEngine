// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VIDEOPLAYER_H
#define VIDEOPLAYER_H

// The script and C++ face of a VideoPlayerComponent (Unity's VideoPlayer;
// docs/superpowers/specs/2026-10-31-video-mp4-design.md §6.5). The settings are the component's
// fields; the state (time, frame, isPrepared, ...) and the controls go through the scene's
// VideoPlayerSystem, which plays it. The events are called with the player (on the main thread,
// during the system's update).

#include "EntityHandle.h"
#include "component/VideoPlayerComponent.h"
#include "texture/Texture.h"

#include <cstdint>
#include <optional>
#include <string>

namespace jasperengine{

    class Framebuffer;

    class JASPERENGINE_API VideoPlayer: public EntityHandle{
    public:
        // adds the component when the entity has none
        VideoPlayer(Scene* scene);
        VideoPlayer(Scene* scene, Entity entity);
        virtual ~VideoPlayer();

        VideoPlayer(const VideoPlayer& rhs);
        VideoPlayer& operator=(const VideoPlayer& rhs);
        VideoPlayer(VideoPlayer&& rhs) noexcept;
        VideoPlayer& operator=(VideoPlayer&& rhs) noexcept;

        using EntityHandle::getComponent;
        VideoPlayerComponent& getComponent() const;

        // ---- settings (the component's fields)

        // the component's checkbox: a disabled player holds
        void setEnabled(bool value);
        bool isEnabled() const;
        // CLIP (the clip asset) or URL (the url)
        void setSource(VideoPlayerSource value);
        VideoPlayerSource getSource() const;
        // a video asset (.mp4, .m4v, .mov, .webm); a new one stops the player
        void setClip(const std::string& value);
        std::string getClip() const;
        // http(s)://, data://, an absolute or asset path; a new one stops the player
        void setUrl(const std::string& value);
        std::string getUrl() const;
        void setPlayOnAwake(bool value);
        bool isPlayOnAwake() const;
        void setWaitForFirstFrame(bool value);
        bool isWaitForFirstFrame() const;
        void setLooping(bool value);
        bool isLooping() const;
        void setSkipOnDrop(bool value);
        bool isSkipOnDrop() const;
        // 0.0625 .. 16 (clamped)
        void setPlaybackSpeed(float value);
        float getPlaybackSpeed() const;
        void setTimeUpdateMode(VideoTimeUpdateMode value);
        VideoTimeUpdateMode getTimeUpdateMode() const;
        void setRenderMode(VideoRenderMode value);
        VideoRenderMode getRenderMode() const;
        // NULL_ENTITY: the main camera
        void setTargetCamera(Entity value);
        Entity getTargetCamera() const;
        // 0 .. 1 (clamped)
        void setTargetCameraAlpha(float value);
        float getTargetCameraAlpha() const;
        void setAspectRatio(VideoAspectRatio value);
        VideoAspectRatio getAspectRatio() const;
        // NULL_ENTITY: this GameObject
        void setTargetMaterialRenderer(Entity value);
        Entity getTargetMaterialRenderer() const;
        void setTargetMaterialProperty(const std::string& value);
        std::string getTargetMaterialProperty() const;
        // -1: every submesh
        void setTargetSubmesh(int value);
        int getTargetSubmesh() const;
        // 0: the video's
        void setRenderTextureWidth(unsigned int value);
        unsigned int getRenderTextureWidth() const;
        // 0: the video's
        void setRenderTextureHeight(unsigned int value);
        unsigned int getRenderTextureHeight() const;
        // a change takes effect at the next prepare
        void setAudioOutputMode(VideoAudioOutputMode value);
        VideoAudioOutputMode getAudioOutputMode() const;
        // 0 .. 1 (clamped)
        void setDirectVolume(float value);
        float getDirectVolume() const;
        void setDirectMute(bool value);
        bool isDirectMute() const;
        // NULL_ENTITY: this GameObject
        void setTargetAudioSource(Entity value);
        Entity getTargetAudioSource() const;

        // ---- control

        // opens the source (a URL downloads first); onPrepareCompleted when its first frame can show
        void prepare();
        // prepares first when needed and plays once prepared (onStarted with the first frame)
        void play();
        void pause();
        // back to time 0, the decoder released; the texture keeps its last frame until the next prepare
        void stop();
        // paused: the next frame
        void stepForward();

        // ---- state

        // seconds into the video; setting it seeks (onSeekCompleted when the frame shows)
        void setTime(double seconds);
        double getTime() const;
        bool canSetTime() const;
        // the frame shown (-1 before one); setting it seeks to that frame
        void setFrame(int64_t frame);
        int64_t getFrame() const;
        // the frame count (an estimate where the file has no index: duration x frame rate, as Unity)
        int64_t getFrameCount() const;
        double getFrameRate() const;
        double getLength() const;
        // the displayed size (0 before prepared)
        int getWidth() const;
        int getHeight() const;
        bool isPrepared() const;
        bool isPlaying() const;
        bool isPaused() const;
        // 0..1: the download of a URL, then the opening
        double getPrepareProgress() const;
        // what decodes it: "webm", "ffmpeg", "mediacodec", "avfoundation", "mediafoundation", "browser"
        std::string getBackend() const;
        std::string getVideoCodec() const;      // "H.264", "HEVC", "AV1", ...
        std::string getAudioCodec() const;
        // the last error ("" when none)
        std::string getError() const;

        // the render texture (UI Images, materials: image:setTexture(player:getFramebuffer()))
        Framebuffer* getFramebuffer() const;
        // the render texture as a Texture; nil before the player is prepared
        std::optional<Texture> getTexture() const;

        // ---- static

        // "probably", "maybe" or "": by the probe for a file, by the extension for a URL
        static std::string canPlay(const std::string& pathOrUrl);
        // deletes the media cache's files (of one URL; every one without), those not playing;
        // the files removed
        static int clearCache(const std::string& url = std::string());

        // ---- events

        FunctionSubscribe<void(VideoPlayer)>& getOnPrepareCompleted();
        FunctionSubscribe<void(VideoPlayer)>& getOnStarted();
        // each wrap of a loop, and the end of a play that does not loop
        FunctionSubscribe<void(VideoPlayer)>& getOnLoopPointReached();
        FunctionSubscribe<void(VideoPlayer, const std::string&)>& getOnErrorReceived();
        FunctionSubscribe<void(VideoPlayer)>& getOnSeekCompleted();
        // (player, frame index) each frame shown; only set it when needed: a call per frame
        FunctionSubscribe<void(VideoPlayer, int)>& getOnFrameReady();
    };

}

#endif //VIDEOPLAYER_H
