// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
// Portions ported from OpenEngine, Copyright (c) 2026 Gamecrafters Guild Limited and contributors (MIT):
// Engine/Modules/Rendering/Source/Common/CubeLutFileParser.cpp, CubeLutColorDomain.cpp (and their
// headers) and the curve maths of Shaders/Includes/cube_lut.glsl

#ifndef CUBELUT_H
#define CUBELUT_H

#include "Export.h"
#include "SceneSettings.h"
#include "math/Vector3.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine{

    // Where a user LUT applies in PostColorLut (PostProcessingSettings::lutTexture):
    //  - DISPLAY: on the graded display colour, where a PNG strip sits (sRGB, or linear with
    //    an input range up to 1)
    //  - SCENE: on the exposed linear scene colour before the tone mapping curve (the log
    //    encodings, or linear with an input range above 1)
    enum class LutStage{
        DISPLAY,
        SCENE
    };

    // A colour LUT: a Resolve-style .cube file (a 1D shaper, a 3D table, or the shaper then the
    // table), its cooked .jlut form, or a PNG strip converted to a 3D table. Values are float RGB
    // triples; the 3D table is red-fastest (index r + g * N + b * N * N), as .cube files list it.
    struct JASPERENGINE_API CubeLut{
        std::string title;              // the TITLE line's text, empty when the file has none
        int size1D = 0;                 // 0 without a 1D table, else 2..65536
        int size3D = 0;                 // 0 without a 3D table, else 2..129
        float in1D[2] = {0.0f, 1.0f};   // input range (min, max) of the 1D table
        float in3D[2] = {0.0f, 1.0f};   // input range (min, max) of the 3D table
        std::vector<float> lut1D;       // size1D * 3
        std::vector<float> lut3D;       // size3D^3 * 3
        bool fromStrip = false;         // converted from a PNG strip: always display colour (sRGB)

        // no table at all
        bool empty() const;
        // the upper end of the input ranges of the tables it has
        float inputMax() const;
        // the 1D table then the 3D table at a colour in the LUT's input encoding: each input is
        // normalised by the table's range and clamped to it, the 3D table is sampled trilinearly
        Vector3 sample(const Vector3& encoded) const;
    };

    // the largest 3D table of a cooked .jlut (larger .cube tables are resampled to it)
    constexpr int JLUT_MAX_3D_SIZE = 33;

    // parses .cube text (TITLE, LUT_1D_SIZE, LUT_3D_SIZE, LUT_1D/3D_INPUT_RANGE, scalar
    // DOMAIN_MIN/MAX, # comments, a UTF-8 BOM); false with a one-line error "line N: ..." otherwise
    JASPERENGINE_API bool parseCubeLut(std::string_view text, CubeLut& out, std::string& error);

    // the input encoding a LUT's file suggests (a .cube has no field for it), with the reason in
    // words: an encoding the title names (as whole words, case-insensitive, only before the first
    // "to" of a title "X to Y"; one the engine lacks, such as S-Log3, falls back to sRGB and the
    // reason says so), else LINEAR when an input range reaches above 1, else SRGB. A strip is SRGB
    JASPERENGINE_API LutInputEncoding proposeEncoding(const CubeLut& lut, std::string& reason);
    // the encoding a setting means for a LUT: AUTO is the proposal, a strip is always SRGB
    JASPERENGINE_API LutInputEncoding resolveEncoding(LutInputEncoding setting, const CubeLut& lut, std::string* reason = nullptr);
    // the stage a LUT applies at in that encoding (AUTO is resolved first)
    JASPERENGINE_API LutStage stageOf(LutInputEncoding encoding, const CubeLut& lut);

    // the display stage: an engine display colour (pow 1/2.2 of linear, 0..1) through the LUT
    // read in its encoding (SRGB: piecewise sRGB, LINEAR: linear display), engine display out
    JASPERENGINE_API Vector3 applyDisplayStage(const CubeLut& lut, LutInputEncoding encoding, const Vector3& engineDisplay);
    // the scene stage: a linear Rec. 709 scene colour converted to the encoding's gamut and
    // curve, through the LUT, then decoded back to linear Rec. 709 (LINEAR: the LUT alone)
    JASPERENGINE_API Vector3 applySceneStage(const CubeLut& lut, LutInputEncoding encoding, const Vector3& sceneLinear);

    // cooks a LUT into a .jlut (little-endian: "JLUT", version, 1D size, 3D size, the four range
    // ends, the title, then float16 RGB of the 1D and 3D tables): the 3D table is resampled
    // trilinearly to at most JLUT_MAX_3D_SIZE per side, the 1D table is kept. False for an empty LUT
    JASPERENGINE_API bool writeJlut(const CubeLut& lut, std::vector<uint8_t>& out);
    // reads a .jlut written by writeJlut; false with an error otherwise
    JASPERENGINE_API bool readJlut(const uint8_t* data, size_t size, CubeLut& out, std::string& error);

    // loads a LUT file through the engine's file io (project, packed and Android asset paths):
    // a .cube (in a game its sibling <name>.jlut instead when one exists, as exports ship it), a
    // .jlut, or any other image as a PNG strip (PostColorLut::loadStrip). False with a logged
    // error otherwise, also returned in error
    JASPERENGINE_API bool loadColorLut(const std::string& path, CubeLut& out, std::string* error = nullptr);

    // the LUT of a path, loaded (loadColorLut) the first time any thread asks for it and shared
    // afterwards; null for an empty path or a LUT that failed to load (logged once)
    JASPERENGINE_API std::shared_ptr<const CubeLut> getCachedColorLut(const std::string& path);
    // why the cached load of a path failed (empty when it loaded or was not asked for)
    JASPERENGINE_API std::string getCachedColorLutError(const std::string& path);
    // drops the cached LUT of a path (the file changed), or every cached LUT for an empty path
    JASPERENGINE_API void invalidateColorLutCache(const std::string& path = "");

}

#endif //CUBELUT_H
