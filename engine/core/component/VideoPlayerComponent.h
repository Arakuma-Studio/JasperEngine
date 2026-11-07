// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VIDEO_PLAYER_COMPONENT_H
#define VIDEO_PLAYER_COMPONENT_H

// Unity's Video Player on a GameObject (docs/superpowers/specs/2026-10-31-video-mp4-design.md
// §6.4): plays a video asset (`clip`) or a URL / user file (`url`) into its own RGBA render
// texture (the `video|<entity>` texture link, VideoPlayer::getTexture), a renderer's material
// texture, a camera's far or near plane, or nowhere (API only); its sound plays directly, through
// an AudioSource, or not at all. VideoPlayerSystem runs it; VideoPlayer is its script face.
// WebM AV1/Opus decodes in software everywhere, MP4 (H.264, HEVC...) through each platform's
// decoders (media/VideoSourceFactory).

#include "Export.h"
#include "Entity.h"
#include "util/FunctionSubscribe.h"

#include <memory>
#include <string>

namespace jasperengine{

    class VideoPlayer;
    class VideoTextureConverter;

    // Unity's VideoSource
    enum class VideoPlayerSource{
        CLIP,           // `clip`: a video asset of the project
        URL             // `url`: http(s)://, data://, an absolute path or an asset path
    };

    // Unity's VideoTimeUpdateMode (DSPTime is not offered: the audio follows the picture)
    enum class VideoTimeUpdateMode{
        GAME_TIME,              // scaled time: timeScale slows or pauses the video (Unity's default)
        UNSCALED_GAME_TIME      // real time: ads and menus play on at timeScale 0
    };

    // Unity's VideoRenderMode
    enum class VideoRenderMode{
        CAMERA_FAR_PLANE,       // behind the scene, in the target camera (after its clear, before the opaque queue)
        CAMERA_NEAR_PLANE,      // over the scene, under the UI, in the target camera
        RENDER_TEXTURE,         // only the player's texture (video|<entity> links, getTexture)
        MATERIAL_OVERRIDE,      // a texture of a renderer's material (restored when the player stops)
        API_ONLY                // decoded, nothing drawn (the texture still has the frames)
    };

    // Unity's VideoAspectRatio: how the picture fits a camera plane or a fixed-size render texture
    enum class VideoAspectRatio{
        NO_SCALING,
        FIT_VERTICALLY,
        FIT_HORIZONTALLY,
        FIT_INSIDE,             // letterboxed: the bars are transparent
        FIT_OUTSIDE,            // cropped
        STRETCH
    };

    // Unity's VideoAudioOutputMode (APIOnly is not offered)
    enum class VideoAudioOutputMode{
        NONE,                   // no audio decoded
        AUDIO_SOURCE,           // through the target AudioSource's volume, 3D settings and bus
        DIRECT                  // straight to the output at directVolume
    };

    struct JASPERENGINE_API VideoPlayerComponent{
        bool enabled = true;

        VideoPlayerSource source = VideoPlayerSource::CLIP;
        std::string clip;                       // asset path (.mp4, .m4v, .mov, .webm)
        std::string url;                        // http(s)://, data://, absolute or asset path

        bool playOnAwake = true;
        bool waitForFirstFrame = true;          // the clock starts once the first frame shows
        bool isLooping = false;
        bool skipOnDrop = true;                 // late frames are skipped (false: the video waits for them)
        float playbackSpeed = 1.0f;             // 0.0625 .. 16
        VideoTimeUpdateMode timeUpdateMode = VideoTimeUpdateMode::GAME_TIME;

        VideoRenderMode renderMode = VideoRenderMode::RENDER_TEXTURE;
        Entity targetCamera = NULL_ENTITY;      // camera planes: NULL_ENTITY is the main camera
        float targetCameraAlpha = 1.0f;
        VideoAspectRatio aspectRatio = VideoAspectRatio::FIT_INSIDE;
        Entity targetMaterialRenderer = NULL_ENTITY;            // MaterialOverride: NULL_ENTITY is this entity
        std::string targetMaterialProperty = "baseColorTexture"; // a material texture or a shader @texture uniform
        int targetSubmesh = -1;                 // -1: every submesh
        unsigned int renderTextureWidth = 0;    // 0 x 0: the video's own size
        unsigned int renderTextureHeight = 0;

        VideoAudioOutputMode audioOutputMode = VideoAudioOutputMode::DIRECT;
        float directVolume = 1.0f;              // 0 .. 1 (Unity's SetDirectAudioVolume)
        bool directMute = false;
        Entity targetAudioSource = NULL_ENTITY; // AudioSource mode: NULL_ENTITY is this entity

        // runtime only, not serialized
        // the texture the frames go into: made with the component (or by a texture link first),
        // its GPU side at the first frame, so links bind to it before anything plays
        std::shared_ptr<VideoTextureConverter> output;
        // events (Unity's prepareCompleted, started, loopPointReached, errorReceived,
        // seekCompleted, frameReady), on the main thread from VideoPlayerSystem's update
        FunctionSubscribe<void(VideoPlayer)> onPrepareCompleted;
        FunctionSubscribe<void(VideoPlayer)> onStarted;
        FunctionSubscribe<void(VideoPlayer)> onLoopPointReached;
        FunctionSubscribe<void(VideoPlayer, const std::string&)> onErrorReceived;
        FunctionSubscribe<void(VideoPlayer)> onSeekCompleted;
        FunctionSubscribe<void(VideoPlayer, int)> onFrameReady;      // only when set: a call per frame

        // every event, for the clears at Stop and on tag removal (tools/check_event_clears.py)
        template<typename F> void forEachEvent(F&& f) {
            f(onPrepareCompleted); f(onStarted); f(onLoopPointReached); f(onErrorReceived); f(onSeekCompleted);
            f(onFrameReady);
        }
    };

}

#endif // VIDEO_PLAYER_COMPONENT_H
