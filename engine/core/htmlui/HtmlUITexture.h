// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUITEXTURE_H
#define HTMLUITEXTURE_H

#include "Export.h"
#include "render/Render.h"
#include "render/TextureRender.h"
#include "texture/Framebuffer.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace jasperengine {

    // What every Rml::TextureHandle points to (phase 1). Every UI texture holds premultiplied
    // RGBA8 (or R8 coverage): each upload path premultiplies, the shaders never do.
    // RmlRenderer::ReleaseTexture deletes the struct, which destroys what it owns.
    struct JASPERENGINE_API HtmlUITexture {
        enum class Kind : uint8_t { STATIC, DYNAMIC, RENDER_TARGET, EXTERNAL };
        // how draws read it (S16): premultiplied RGBA; R8 coverage whose .r tints every channel
        // (glyph atlas pages); an MTSDF glyph page (RGBA8 distances, the HTMLUI_PRIM glyph mode)
        enum class Sample : uint8_t { RGBA, COVERAGE, MSDF };
        Kind kind = Kind::STATIC;
        Sample sample = Sample::RGBA;
        // a block of texels that sample as exactly 1.0 (a glyph page's reserved white block): the
        // batched replay draws untextured geometry with this texture at that UV
        bool hasWhiteTexel = false;
        float whiteU = 0.0f, whiteV = 0.0f;
        // a saved layer (SaveLayerAsTexture/SaveLayerIntoTexture: box shadows drawn as textures)
        bool savedLayer = false;
        TextureRender* render = nullptr;            // sampled texture
        std::unique_ptr<TextureRender> ownedRender; // STATIC/DYNAMIC created by the UI code
        std::unique_ptr<Framebuffer> ownedTarget;   // RENDER_TARGET (SaveLayerAsTexture)
        int width = 0, height = 0;
        bool dead = false;                          // GPU object dropped by view loss (phase-0 releaseGpuResources)
        uint64_t lastUpdateFrame = UINT64_MAX;      // DYNAMIC: one sg_update_image per frame
        // RENDER_TARGET of a <video> (phase 6): the dynamic R8 Y, U and V planes its frames are
        // converted from (RmlRenderer::queueVideoConversion), owned and released with it
        std::unique_ptr<TextureRender> planes[3];

        HtmlUITexture() = default;
        HtmlUITexture(const HtmlUITexture&) = delete;
        HtmlUITexture& operator=(const HtmlUITexture&) = delete;
        // the owned texture's GPU objects, unless dead (they went with the view: no GPU call);
        // ownedTarget destroys its own. EXTERNAL owns nothing
        ~HtmlUITexture();

        // A DYNAMIC texture updated in place (Lottie frames, phase-5 effects): RGBA -> RGBA8,
        // RED -> R8 (TextureRender::createDynamicTexture, LINEAR or NEAREST, CLAMP_TO_EDGE).
        // nullptr (logged) without a view, for another format or a non-positive size.
        static std::unique_ptr<HtmlUITexture> createDynamic(const std::string& label, int width, int height,
                                                            ColorFormat format = ColorFormat::RGBA,
                                                            TextureFilter filter = TextureFilter::LINEAR);

        // DYNAMIC: uploads width * height * bpp bytes (premultiplied, rows top first). Sokol
        // takes one update per image per frame: false, and nothing uploaded, when this frame
        // (SystemRender::getFrameCount) already updated it, or for a dead/non-dynamic texture
        bool update(const void* data, size_t size);
        // DYNAMIC: update() would upload this frame
        bool canUpdate() const;
    };

}

#endif //HTMLUITEXTURE_H
