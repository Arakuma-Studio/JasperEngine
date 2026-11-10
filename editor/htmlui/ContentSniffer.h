// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <string>

namespace jasperengine::editor::htmlui {

    // What a downloaded body is, from its magic bytes first, then the Content-Type and the URL
    // (phase-2 §3.13.3: URLs like images.unsplash.com/photo-…?w=800 have no extension)
    struct SniffedContent {
        enum class Type { Unknown, Png, Jpeg, Gif, Bmp, Tga, WebP, Avif, Svg, TrueType, OpenType, Woff, Woff2, Zip,
                          Json, Css, Html,
                          // videos (phase 6): EBML (WebM or Matroska: the probe tells which plays), ISO
                          // base media (MP4, M4V; QuickTime), Ogg
                          WebM, Mp4, QuickTime, Ogg };
        Type type = Type::Unknown;

        bool isImage() const;          // png jpeg gif bmp tga webp avif svg
        bool isDecodableImage() const; // what the engine draws: png jpeg gif bmp tga svg
        bool isFont() const;           // TrueType, OpenType, WOFF and WOFF2
        bool isVideo() const;          // webm mp4 quicktime ogg
        std::string extension() const; // "png", "jpg", "svg", "ttf", "otf", "woff2", "json", "lottie", "css", "webm", "mp4", ...
        const char* name() const;      // "PNG", "WebP", "WOFF2", ...
    };

    // contentType: lowercase media type without parameters ("" when unknown)
    SniffedContent sniffContent(const std::string& body, const std::string& contentType, const std::string& url);

    // the extension of a URL's last path segment, lowercase, without the dot ("" when none)
    std::string urlExtension(const std::string& url);
    // the last path segment of a URL without its extension ("photo-123"), "" when none
    std::string urlBaseName(const std::string& url);

}
