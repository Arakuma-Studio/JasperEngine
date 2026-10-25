// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TEXTUREPACK_H
#define TEXTUREPACK_H

#include "Export.h"
#include "texture/TextureData.h"

#include <cstdint>
#include <string>

namespace jasperengine{

    // Two images packed into one RGBA8 image, channel by channel: what a material lobe takes
    // as its one map when glTF gives its values in two textures (KHR_materials_clearcoat's
    // factor and roughness, sheen's colour and roughness, specular's colour and strength,
    // iridescence's factor and thickness, transmission's factor and the volume's thickness).
    // The result is at the larger size of the two; a smaller source is resampled bilinearly.
    // Runs on a job (MaterialPbr's maps, TextureDataPool::setGenerator), never on the main thread.
    class JASPERENGINE_API TexturePack{
    public:
        // A source image: 1 to 4 channels of 8 or 16 bits (16-bit takes its high byte)
        struct Source{
            const unsigned char* pixels = nullptr;
            int width = 0;
            int height = 0;
            int components = 4;
            int bytesPerComponent = 1;

            bool valid() const;
        };

        enum class Channel : uint8_t{ R, G, B, A };

        // where one output channel comes from: source 0 or 1 and its channel, or (source -1)
        // the constant 255, which a factor multiplies by as 1
        struct Pick{
            int source = -1;
            Channel channel = Channel::R;
        };

        struct Layout{
            const char* name;   // part of the packed image's pool id
            Pick channels[4];   // R, G, B, A of the result
        };

        // the layouts of the glTF lobes (MaterialPbr's channel layout of each map)
        static const Layout CLEARCOAT;     // R clearcoatTexture.r, G clearcoatRoughnessTexture.g
        static const Layout SHEEN;         // RGB sheenColorTexture.rgb, A sheenRoughnessTexture.a
        static const Layout SPECULAR;      // RGB specularColorTexture.rgb (source 1), A specularTexture.a (source 0)
        static const Layout IRIDESCENCE;   // R iridescenceTexture.r, G iridescenceThicknessTexture.g
        static const Layout TRANSMISSION;  // R transmissionTexture.r, G the volume's thicknessTexture.g

        // The packed RGBA8 image (either source may be null or invalid: its channels are 255).
        // Its pixels are malloc'ed and not owned (TextureData copies share them): the copy
        // that keeps them sets setDataOwned(true), or they are freed with releaseImageData.
        static TextureData pack(const Source* first, const Source* second, const Layout& layout);
        // The pool id of a packed image: both sources' keys and the layout
        static std::string makeId(const std::string& firstKey, const std::string& secondKey, const Layout& layout);
        // an id makeId made (a texture generated at load, never written to a file)
        static bool isPackedId(const std::string& id){ return id.rfind("pack|", 0) == 0; }
    };

}

#endif //TEXTUREPACK_H
