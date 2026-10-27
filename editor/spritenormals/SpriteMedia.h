// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Sources of the sprite normal generator: still images (PNG 8/16-bit, palette, grey; JPEG, TGA,
// BMP through stb_image) become RGBA8; animated GIFs and APNGs are decoded with spec-exact frame
// compositing (GIF disposal 0-3, APNG dispose and blend ops) and packed row-major into one RGBA8
// atlas (columns = ceil(sqrt(frames)), unused cells transparent), with their delays and loop
// count. Limits are checked from headers and before each frame is decoded (bomb-safe).

#include "SpriteNormals.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor::spritenormals {

    // GCC's -Wshadow takes the scoped enumerator Image for a shadow of the Image alias
    // (SpriteNormals.h); they never collide (SourceKind::Image is always qualified)
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#endif
    enum class SourceKind { Image, Gif, Apng };
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    const char* toString(SourceKind kind);    // "image", "gif", "apng"

    // A frame delay as an exact ratio of milliseconds (GIF: centiseconds * 10 / 1; APNG:
    // delay_num * 1000 / delay_den, den 0 meaning 100, reduced by their gcd)
    struct Delay {
        uint32_t numerator = 100;
        uint32_t denominator = 1;
        double ms() const;
        // What playback uses: ms() rounded; below 10 ms (0 included) -> 100, as browsers do
        int playbackMs() const;
    };

    struct Source {
        SourceKind kind = SourceKind::Image;
        Image color;                    // RGBA8: the image, or the atlas of the frames
        // Animation (kind Gif/Apng): frame size, layout and timing; empty cells for an Image
        int frameWidth = 0;
        int frameHeight = 0;
        int columns = 0;
        int rows = 0;
        int frameCount = 0;
        std::vector<Cell> cells;        // frameCount cells, row-major (the frames in the atlas)
        std::vector<Delay> delays;      // one per frame
        int loopCount = 0;              // 0: forever (GIF NETSCAPE 0 / no extension -> 1; APNG num_plays)
        // The file as it was read
        std::string sha1;               // of the file bytes, 40 hex digits ("" from memory without hashing)
        int fileWidth = 0;              // the image (or the animation's canvas)
        int fileHeight = 0;
        int fileChannels = 0;          // 1..4 (stills; 4 for animations)
        int fileBitDepth = 8;           // 8 or 16
    };

    // What a file is without decoding pixels: its kind, canvas size and frame count (GIF: the
    // image descriptors counted by walking the blocks; APNG: acTL num_frames). For planning
    // output names and refusing oversized inputs early.
    struct SourceInfo {
        SourceKind kind = SourceKind::Image;
        int width = 0;
        int height = 0;
        int frameCount = 1;
    };

    inline constexpr int64_t kMaxInputBytes = 64LL * 1024 * 1024;
    inline constexpr int kMaxFrames = 256;
    // kMaxPixels (SpriteNormals.h) bounds the still image, the canvas x frames decoded so far,
    // and the atlas

    // False with problems (kind invalid_source for unreadable / unsupported / malformed files,
    // too_large for the limits, naming the limit and the actual numbers)
    bool probeSource(const std::filesystem::path& path, SourceInfo& info, std::vector<Problem>& problems);
    bool loadSource(const std::filesystem::path& path, Source& out, std::vector<Problem>& problems);
    bool loadSourceFromMemory(const uint8_t* bytes, size_t size, Source& out, std::vector<Problem>& problems,
                              bool hash = true);

    // Packs equal-size RGBA8 frames row-major (columns = ceil(sqrt(n)), rows = ceil(n / columns)),
    // unused cells transparent black; cells receives the frame rects. False when the atlas would
    // exceed kMaxPixels.
    bool packAtlas(const std::vector<Image>& frames, Image& atlas, int& columns, int& rows, std::vector<Cell>& cells,
                   std::string& error);

    // PNG bytes without ancillary chunks: RGBA8 (normal maps, atlases) or 1-channel 16-bit (height
    // maps); never gAMA/sRGB/iCCP
    bool encodePng(const Image& image, std::vector<uint8_t>& png, std::string& error);
}
