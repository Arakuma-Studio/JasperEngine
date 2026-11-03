// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Basis Universal KTX2 files (ETC1S, UASTC, UASTC HDR: the web family) transcoded at load to the
// format TextureFallback picked (spec S14 R10, D5). Built with JASPERENGINE_BASIS_TRANSCODER (the
// web runtime, the editor, the tests); native games never load Basis files.
//
// The levels Ktx2::read kept go straight to Basis's container-independent transcoders (ETC1S,
// UASTC LDR 4x4, UASTC HDR 4x4): its KTX2 container class, and every source format the cook never
// writes, stay out of the binary (spec S14 R21, engine/libs/basisu/README.jasper.md).

#ifndef BASISTRANSCODE_H
#define BASISTRANSCODE_H

#include "Export.h"
#include "texture/Ktx2.h"
#include "texture/TextureFallback.h"

#include <string>
#include <vector>

namespace jasperengine {

    class JASPERENGINE_API BasisTranscode {
    public:
        static bool isAvailable();
        // builds the transcoder's tables (once; the first Session::open does it otherwise), so a
        // loader thread can take that cost before the first file
        static void warmUp();

        // One file's levels, transcoded level by level
        class JASPERENGINE_API Session {
        public:
            Session();
            ~Session();
            Session(const Session&) = delete;
            Session& operator=(const Session&) = delete;

            // levels image.firstLevel.. of a Basis KTX2 (as Ktx2::read keeps them: Zstandard
            // inflated, BasisLZ as stored); level 0 of the session is image.firstLevel
            bool open(Ktx2Image image, std::string& error);
            uint32_t levelCount() const;
            uint32_t faces() const;
            // every face of `level`, faces contiguous, in `target` (R8 and RG8 are taken from an
            // RGBA transcode: the single channel's R, a normal map's RG)
            bool transcodeLevel(uint32_t level, GpuTextureFormat target, TextureContent content,
                                std::vector<uint8_t>& out, std::string& error);

            // UASTC and UASTC HDR (blocks independent of each other) also transcode a band of a
            // level at a time, for the main thread's time slices: block rows
            // [firstRow, firstRow + rows) of every face into `out`, which holds the whole level
            // (levelBytes) and fills over the calls
            bool transcodesInRows() const;
            uint32_t blockRows(uint32_t level) const;
            size_t levelBytes(uint32_t level, GpuTextureFormat target) const;
            bool transcodeRows(uint32_t level, uint32_t firstRow, uint32_t rows, GpuTextureFormat target, TextureContent content,
                               std::vector<uint8_t>& out, std::string& error);
        private:
            struct State;
            State* state = nullptr;
        };
    };

}

#endif // BASISTRANSCODE_H
