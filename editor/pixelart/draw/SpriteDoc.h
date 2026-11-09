// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// One sprite file as edit_sprite sees it (docs/specs/S47-agent-sprite-drawing.md R1): an
// `.aseprite`/`.ase` through S44's codec, or a plain PNG as a one-frame, one-layer document.
// Both are an aseprite::Document, so every op works on one model; a PNG also remembers its
// chunks, so writing it back rewrites only IHDR, PLTE, tRNS and IDAT and keeps every other chunk
// (the editor's `jasper.*` recipes included) byte for byte, in place.
//
// Pure data and codecs: no Project, no editor state, any thread.

#pragma once

#include "pixelart/PixelImage.h"
#include "pixelart/PixelProblem.h"
#include "pixelart/aseprite/AsepriteDocument.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace jasperengine::editor::pixelart::draw {

    // R5: what an edit may make or keep editing
    inline constexpr int kMaxEditSide = 4096;
    inline constexpr int kMaxFrames = 256;
    inline constexpr int kMaxLayers = 256;

    enum class SpriteKind : uint8_t { Aseprite, Png };

    // The chunks of a PNG other than the ones its pixels decide, in file order. `critical` marks
    // where IHDR, PLTE, tRNS and the first IDAT were, so they are written back in place.
    struct PngChunk {
        enum class Role : uint8_t { Keep, Ihdr, Plte, Trns, Idat };
        Role role = Role::Keep;
        std::vector<uint8_t> bytes;       // the whole chunk (length, type, data, CRC) for Keep
    };

    struct PngLayout {
        std::vector<PngChunk> chunks;     // up to and excluding IEND
        std::vector<uint8_t> iend;        // the IEND chunk as read
        std::vector<uint8_t> trailing;    // bytes after IEND
        bool hadPalette = false;          // a PLTE chunk (RGBA PNGs keep a suggested palette as it was)
        int colorType = 6;                // IHDR's, kept when the PNG is written back
        bool animated = false;            // acTL/fcTL/fdAT: an APNG, whose frames the writer can't keep
        bool colorKey = false;            // a tRNS on a non-palette PNG
    };

    struct SpriteDoc {
        SpriteKind kind = SpriteKind::Aseprite;
        aseprite::Document doc;
        PngLayout png;

        bool indexed() const { return doc.mode == aseprite::ColorMode::Indexed; }
        bool isPng() const { return kind == SpriteKind::Png; }
    };

    // ".aseprite"/".ase"/".png" (case-insensitive)
    bool isSpritePath(const std::filesystem::path& path);
    bool isPngPath(const std::filesystem::path& path);

    // Decodes a file's bytes. `provenance` keeps S44's record of the bytes (the writer re-emits
    // unchanged chunks from it); a read-only use turns it off. False with `problem`.
    bool decodeSprite(const std::filesystem::path& path, std::span<const uint8_t> bytes, SpriteDoc& out, PixelProblem& problem,
                      bool provenance = true);
    // The file bytes of a document (deterministic). False with `problem` (invalid_document).
    bool encodeSprite(const SpriteDoc& sprite, std::vector<uint8_t>& out, PixelProblem& problem);

    // An `unsupported_feature` problem when edits could not be written back losslessly, else empty
    PixelProblem editRefusal(const SpriteDoc& sprite);

    // The canvas-sized image of a cel (following links): the cel's pixels at its position, the
    // rest transparent (indexed: the transparent index, or the palette's colour on a background
    // layer). Empty for a group or tilemap layer.
    PixelImage celCanvas(const aseprite::Document& doc, int layer, int frame);
    // A canvas-sized transparent image in the document's model for a layer
    PixelImage blankCanvas(const aseprite::Document& doc, int layer);
    // ... and any size (a background layer's clear colour is its palette entry's)
    PixelImage blankImage(const aseprite::Document& doc, int layer, int width, int height);

    // "#rrggbb" or "#rrggbbaa" (alpha written only when not 255)
    std::string hexColor(const PixelRgba& c);

}
