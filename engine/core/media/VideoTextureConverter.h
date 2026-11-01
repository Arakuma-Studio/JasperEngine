// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VIDEOTEXTURECONVERTER_H
#define VIDEOTEXTURECONVERTER_H

#include "Export.h"
#include "VideoTypes.h"
#include "component/VideoPlayerComponent.h"
#include "math/Rect.h"
#include "render/PipelineState.h"

#include <cstdint>
#include <memory>

namespace jasperengine {

    class CameraRender;
    class Framebuffer;
    class IVideoSource;

    // The render texture of a Video Player (docs/superpowers/specs/2026-10-31-video-mp4-design.md
    // §6.7): a frame's Y/U/V planes go up into three R8 textures and one HTMLUI_POST Yuv pass
    // converts them into an RGBA8 target (the WebM path's conversion, BT.709/BT.601 by the
    // frame's colour), letterboxed or cropped by the aspect ratio when the target has a fixed
    // size (the bars are transparent). The target's Framebuffer object exists from the start, so
    // `video|<entity>` texture links, UI Images and materials bind it before the first frame;
    // its GPU side is made (and remade on a size change) by the first conversion.
    //
    // Main thread: submit() during the update; RenderSystem runs the conversion before the
    // frame's first pass (processVideoConversions) and draws camera planes in the camera's pass.
    class JASPERENGINE_API VideoTextureConverter {
    public:
        VideoTextureConverter();
        ~VideoTextureConverter();
        VideoTextureConverter(const VideoTextureConverter&) = delete;
        VideoTextureConverter& operator=(const VideoTextureConverter&) = delete;

        // the stable target object (its version changes when its GPU side is remade)
        Framebuffer* getFramebuffer();

        // a frame for the target: targetWidth x targetHeight (0 x 0: the frame's size), the
        // picture fitted by `fit` when they differ
        void submit(VideoFrame frame, int targetWidth, int targetHeight, VideoAspectRatio fit);
        // a source that draws its own frames (the browser's element) into a target of that size
        void submitSource(IVideoSource* source, int width, int height);
        // the source going away before the conversion ran
        void forgetSource(IVideoSource* source);
        bool hasPending() const;

        // RenderSystem, outside any pass: the pending frame into the target; false when there
        // was none or the shaders are still building (the frame waits)
        bool convert();
        // a frame has been converted into the target since it was made
        bool hasPicture() const;
        int getWidth() const;
        int getHeight() const;
        uint64_t getConversionCount() const;

        // Inside a pass: the target over `viewport` (the pass's), fitted by `fit`, times `alpha`
        // (premultiplied blending); flipY for a destination stored bottom row first
        bool drawPlane(const RenderTargetFormat& target, CameraRender& pass, const Rect& viewport, VideoAspectRatio fit, float alpha, bool flipY);

        // frees the GPU side (the component's removal, a view loss); the next frame makes it again
        void release();

        // the rectangle (x, y, w, h) a picture of srcW x srcH takes in dstW x dstH (it may
        // exceed it: cropping fits)
        static void fitRect(float srcW, float srcH, float dstW, float dstH, VideoAspectRatio fit, float out[4]);

        // targets made (tests)
        static int getLiveCount();
        // tests without a GPU: as if a frame of that size had been converted (the frame waiting is dropped)
        void setPictureForTesting(int width, int height);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

}

#endif //VIDEOTEXTURECONVERTER_H
