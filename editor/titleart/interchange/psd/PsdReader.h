// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R32, R33, R39, R44, R47, D10)
// - Purpose: our reader of Photoshop documents: PSD (version 1) and PSB (version 2), every colour
//   mode's structure (only RGB and Grayscale 8- and 16-bit layers decode their pixels), the image
//   resources Title Art uses (0x03ED resolution, 0x040D / 0x0419 global light, 0x040F ICC
//   profile, 0x0421 version info), every layer record (bounds, blend key, opacity, clipping,
//   visibility, the user mask) with the additional information blocks the importer and the style
//   import read (lsct / lsdk, luni, lyid, iOpa, knko, lfx2, lmfx, TySh, the adjustment keys, the
//   fill layers SoCo / GdFl / PtFl, smart objects, vmsk / vsms), 16-bit layers from `Lr16`,
//   channels raw, PackBits, ZIP and ZIP with prediction, and the merged image. Written from the
//   Adobe Photoshop File Formats Specification; PsdWriter's round trip and tools/mcp/psdread.py
//   (an independent reader) test each other.
// - Layers are in file order (bottom first), group end markers (lsct 3) included; psdTree()
//   nests them.
// - Untrusted input: every length is checked against what is left, sizes are capped (300,000 px
//   per side, 4,096 layers, 64 MiB per channel); a malformed file fails with a message naming
//   the offset, never a crash.
// - Thread-safety: pure functions.

#pragma once

#include "PsdDescriptor.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor::psd {

    enum class PsdColorMode : uint16_t { Bitmap = 0, Grayscale = 1, Indexed = 2, Rgb = 3, Cmyk = 4, Multichannel = 7, Duotone = 8, Lab = 9 };
    const char* psdColorModeName(uint16_t mode);

    struct PsdRect {
        int32_t top = 0, left = 0, bottom = 0, right = 0;
        int width() const { return right > left ? right - left : 0; }
        int height() const { return bottom > top ? bottom - top : 0; }
        bool empty() const { return width() == 0 || height() == 0; }
    };

    // one decoded plane: width x height samples of 1 (8-bit) or 2 (16-bit, native uint16) bytes
    struct PsdPlane {
        std::vector<uint8_t> samples;
        bool present() const { return !samples.empty(); }
    };

    struct PsdLayerRecord {
        std::string name;               // `luni` when present, else the Pascal name
        PsdRect bounds;
        std::string blendKey = "norm";
        uint8_t opacity = 255;
        bool clipping = false;          // clipped to the layer below (non-base)
        bool visible = true;
        bool transparencyProtected = false;
        int section = 0;                // lsct / lsdk: 0 layer, 1 open group, 2 closed group, 3 group end
        std::string sectionBlend;       // a group's own blend key (pass: pass-through)
        uint32_t id = 0;                // lyid
        uint8_t fillOpacity = 255;      // iOpa
        bool knockout = false;          // knko
        Json effects;                   // the lmfx descriptor when present, else lfx2 (null: none)
        // text (TySh)
        bool text = false;
        double transform[6] = {1, 0, 0, 1, 0, 0};
        Json textDescriptor;
        bool warp = false;              // the text has a warp style other than none
        // adjustment layers: the key and its raw data (and its descriptor when it has one)
        std::string adjustmentKey;
        std::vector<uint8_t> adjustmentData;
        // fill layers: SoCo, GdFl, PtFl and the descriptor
        std::string fillKey;
        Json fillDescriptor;
        bool smartObject = false;
        // vector mask (vmsk / vsms): the raw path records
        std::vector<uint8_t> vectorMask;
        bool vectorMaskInvert = false, vectorMaskDisabled = false;
        // user mask (channel -2)
        bool hasMask = false;
        PsdRect maskBounds;
        uint8_t maskDefault = 0;
        bool maskDisabled = false;
        bool maskInvert = false;
        PsdPlane maskPlane;
        // colour planes (RGB: 0, 1, 2; Grayscale: 0) and transparency (-1)
        PsdPlane planes[3];
        PsdPlane alpha;
        std::vector<std::string> keys; // every additional information key, in order (diagnostics)
    };

    struct PsdFile {
        uint16_t version = 1;           // 2 = PSB
        uint16_t channels = 0;
        int width = 0, height = 0;
        uint16_t depth = 8;
        uint16_t mode = 3;
        double dpi = 72.0;
        int globalAngle = 120, globalAltitude = 30;
        std::vector<uint8_t> icc;
        bool hasRealMergedData = true;
        std::vector<PsdLayerRecord> layers;     // file order: bottom first
        bool layersFrom16 = false;              // the layers came from `Lr16`
        std::vector<uint8_t> patterns;          // the document's Patt / Pat2 / Pat3 block (AslReader parses it)
        PsdPlane merged[4];                     // RGB(A) or grey(+A); planes present as the file has them
        uint16_t mergedCompression = 0;

        bool rgbOrGray() const { return mode == (uint16_t)PsdColorMode::Rgb || mode == (uint16_t)PsdColorMode::Grayscale; }
        int bytesPerSample() const { return depth == 16 ? 2 : 1; }
    };

    struct PsdReadOptions {
        bool layerPixels = true;
        bool mergedImage = true;
    };

    // false with `error` (naming the offset); a CMYK, Lab, indexed, bitmap or 32-bit file reads its
    // structure and styles but no pixels (the caller refuses its layers)
    bool readPsd(const uint8_t* data, size_t size, PsdFile& out, std::string& error, const PsdReadOptions& options = {});

    // the layer tree: indexes into PsdFile::layers, each list bottom first
    struct PsdNode {
        int layer = -1;
        std::vector<PsdNode> children;
    };
    std::vector<PsdNode> psdTree(const PsdFile& file);

    // a layer's straight RGBA (8-bit, rows top first) of its bounds; 16-bit samples rounded
    std::vector<uint8_t> psdLayerRgba8(const PsdFile& file, const PsdLayerRecord& layer);
    // the merged image as RGBA, 8 or 16 bits per sample (16: native uint16)
    std::vector<uint8_t> psdMergedRgba(const PsdFile& file, int bitDepth);

    // the profile description of an ICC profile (its `desc` tag), "" when none
    std::string iccDescription(const std::vector<uint8_t>& profile);

    // PackBits (one row); false when the input is malformed or does not fill `rowBytes`
    bool unpackBits(ByteReader& in, size_t packedBytes, uint8_t* out, size_t rowBytes);
    void packBits(const uint8_t* row, size_t n, std::vector<uint8_t>& out);

}
