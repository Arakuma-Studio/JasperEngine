// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Basis Universal KTX2 files (ETC1S, UASTC, UASTC HDR: the web family) transcoded at load to the
// format TextureFallback picked (spec S14 R10, D5). Built with JASPERENGINE_BASIS_TRANSCODER (the
// web runtime, the editor, the tests); native games never load Basis files.

#ifndef BASISTRANSCODE_H
#define BASISTRANSCODE_H

#include "Export.h"
#include "texture/Ktx2.h"
#include "texture/TextureFallback.h"

#include <span>
#include <string>
#include <vector>

namespace jasperengine {

    class JASPERENGINE_API BasisTranscode {
    public:
        static bool isAvailable();

        // A complete KTX2 of levels image.firstLevel.. (the levels Ktx2::read kept) that the
        // transcoder accepts: the header of the smaller base, ETC1S image descriptions of the
        // kept levels only. A full read (firstLevel 0) is rebuilt the same way.
        static bool rebase(const Ktx2Image& image, std::vector<uint8_t>& out, std::string& error);

        // Opens a (rebased) file for level-by-level transcoding
        class JASPERENGINE_API Session {
        public:
            Session();
            ~Session();
            Session(const Session&) = delete;
            Session& operator=(const Session&) = delete;

            bool open(std::vector<uint8_t> file, std::string& error);
            uint32_t levelCount() const;
            uint32_t faces() const;
            // every face of `level`, faces contiguous, in `target` (R8 and RG8 are taken from an
            // RGBA transcode: the single channel's R, a normal map's RG)
            bool transcodeLevel(uint32_t level, GpuTextureFormat target, TextureContent content,
                                std::vector<uint8_t>& out, std::string& error);
        private:
            struct State;
            State* state = nullptr;
        };
    };

}

#endif // BASISTRANSCODE_H
