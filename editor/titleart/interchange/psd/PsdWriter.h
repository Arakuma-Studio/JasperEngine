// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R39, R44, R49, D7, D10)
// - Purpose: our layered PSD writer (version 1, RGB, 8 or 16 bits, at most 30,000 px a side):
//   the merged image (8-bit PackBits, 16-bit raw), resources 0x03ED (resolution), 0x040F (the
//   embedded ICC profile, IccSrgb.h by default) and 0x0421 (version info, hasRealMergedData 1),
//   one record per layer (Pascal ASCII name with '?' for other characters plus `luni`, `lyid`
//   sequential from 1, opacity, fill opacity `iOpa`, visibility, clipping, blend key, a raster
//   user mask covering the document region it is given), groups as `lsct` section dividers (the
//   end marker below the children, the open folder above them), native adjustment layers from
//   their key and data (PsdAdjustments.h), channels 8-bit PackBits with row byte counts or 16-bit
//   ZIP with prediction inside an `Lr16` block (the 16-bit files' layer info).
// - Pixels are straight RGBA, rows top first (16-bit: native uint16 samples); each layer is
//   trimmed to its alpha, and the colour of transparent pixels is written 0 (smaller PackBits;
//   Photoshop ignores it).
// - Deterministic: the same document gives the same bytes (no time, no random id; miniz level 6).
// - Streaming (R49): psdSpoolPixels / psdSpoolMask / psdSpoolMerged compress a layer's channels
//   into a PsdChannelSpool on disk and free its pixels, and writePsdFile assembles the file from
//   it, with the same bytes as writePsd; a large export holds one render at a time.
// - Thread-safety: pure functions; a spool belongs to one thread.

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iosfwd>
#include <string>
#include <vector>

namespace jasperengine::editor::psd {

    constexpr int PSD_MAX_SIDE = 30000;

    // a compressed channel on disk (compression tag and payload)
    struct PsdSpooledChannel {
        int16_t id = 0;
        uint64_t offset = 0;
        uint64_t length = 0;
    };

    // the channels waiting to go into the file; the file is removed with the object
    class PsdChannelSpool {
    public:
        PsdChannelSpool() = default;
        ~PsdChannelSpool();
        PsdChannelSpool(const PsdChannelSpool&) = delete;
        PsdChannelSpool& operator=(const PsdChannelSpool&) = delete;

        bool open(const std::filesystem::path& path, std::string& error);
        bool append(const std::vector<uint8_t>& data, uint64_t& offset, std::string& error);
        bool copy(uint64_t offset, uint64_t length, std::ostream& to, std::string& error);

    private:
        std::filesystem::path path;
        std::fstream file;
        uint64_t size = 0;
    };

    struct PsdWriteLayer {
        std::string name;
        bool group = false;
        bool groupOpen = true;
        std::vector<PsdWriteLayer> children;       // groups: bottom first
        // pixels: `width` x `height` straight RGBA placed with their top-left at (x, y) of the document
        int x = 0, y = 0, width = 0, height = 0;
        std::vector<uint8_t> rgba;                 // 4 samples a pixel, 1 or 2 bytes each (the document's depth)
        std::string blendKey = "norm";             // a group's "pass" is pass-through
        uint8_t opacity = 255;
        uint8_t fillOpacity = 255;
        bool visible = true;
        bool clipping = false;
        // a raster user mask: `maskWidth` x `maskHeight` grey samples at (maskX, maskY)
        bool hasMask = false;
        int maskX = 0, maskY = 0, maskWidth = 0, maskHeight = 0;
        std::vector<uint8_t> mask;
        uint8_t maskDefault = 0;                   // the mask's value outside its rectangle
        // a native adjustment layer: its key (levl, curv, hue2, brit, nvrt, post, thrs) and data
        std::string adjustmentKey;
        std::vector<uint8_t> adjustmentData;
        // set by psdSpoolPixels / psdSpoolMask, with `rgba` / `mask` emptied
        bool pixelsSpooled = false;
        int spooledLeft = 0, spooledTop = 0, spooledRight = 0, spooledBottom = 0;
        std::vector<PsdSpooledChannel> spooledPixels;   // alpha, R, G, B
        bool maskSpooled = false;
        PsdSpooledChannel spooledMask;
    };

    struct PsdWriteDocument {
        int width = 0, height = 0;
        int bitDepth = 8;                          // 8 or 16
        double dpi = 72.0;
        std::vector<uint8_t> icc;                  // empty: the generated sRGB profile
        std::vector<PsdWriteLayer> layers;         // bottom first
        std::vector<uint8_t> merged;               // straight RGBA of the whole document
        bool mergedSpooled = false;                // set by psdSpoolMerged, with `merged` emptied
        uint64_t mergedOffset = 0, mergedLength = 0;
    };

    // false with `error` (psd_too_large, bad_bit_depth, bad_pixels: one sentence)
    bool writePsd(const PsdWriteDocument& doc, std::vector<uint8_t>& out, std::string& error);

    // the layer's pixel channels, trimmed as writePsd trims them, into the spool
    bool psdSpoolPixels(PsdWriteLayer& layer, int bitDepth, PsdChannelSpool& spool, std::string& error);
    bool psdSpoolMask(PsdWriteLayer& layer, int bitDepth, PsdChannelSpool& spool, std::string& error);
    bool psdSpoolMerged(PsdWriteDocument& doc, PsdChannelSpool& spool, std::string& error);
    // writes the document, whose channels are in `spool`, to `output`
    bool writePsdFile(const PsdWriteDocument& doc, PsdChannelSpool& spool, const std::filesystem::path& output, std::string& error);

}
