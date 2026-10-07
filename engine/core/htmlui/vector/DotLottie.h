// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef DOTLOTTIE_H
#define DOTLOTTIE_H

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine {

    // A minimal ZIP reader: end-of-central-directory scan, central directory, local headers,
    // entries stored (method 0) or deflated (method 8, stb_image's zlib), CRC-32 checked.
    // ZIP64, encrypted and multi-disk archives are rejected.
    class JASPERENGINE_API ZipArchive {
    public:
        // false with error set; the archive keeps a copy of the bytes
        bool open(const uint8_t* data, size_t size, std::string& error);
        bool has(const std::string& name) const;
        // the uncompressed entry; false with error set
        bool read(const std::string& name, std::string& out, std::string& error) const;
        std::vector<std::string> names() const;

    private:
        struct Entry {
            uint16_t method = 0;
            uint32_t crc = 0;
            uint32_t compressedSize = 0;
            uint32_t uncompressedSize = 0;
            uint32_t localHeaderOffset = 0;
        };
        std::vector<uint8_t> bytes;
        std::map<std::string, Entry> entries;
    };

    // A .lottie (dotLottie v1 or v2) archive: manifest.json, the animations
    // (animations/<id>.json in v1, a/<id>.json in v2) and their images (images/ or i/).
    class JASPERENGINE_API DotLottie {
    public:
        // playback settings a manifest may give (v1: per animation or top level; the
        // element's attributes override them)
        struct Settings {
            std::optional<bool> autoplay;
            std::optional<bool> loop;
            std::optional<int> loopCount;       // a numeric `loop`
            std::optional<float> speed;
            std::optional<int> direction;
            std::optional<std::string> playMode; // "normal" / "bounce"
        };

        struct Animation {
            std::string id;
            Settings settings;
        };

        // parses the archive and its manifest; false with error set
        bool open(const uint8_t* data, size_t size, std::string& error);

        int getVersion() const;                           // 1 or 2
        const std::vector<Animation>& getAnimations() const;
        // activeAnimationId (v1) / initial.animation (v2) if it names an animation, else the first
        std::string getDefaultAnimationId() const;
        const Animation* findAnimation(const std::string& id) const;
        // manifest settings for an animation: the animation's own over the top-level ones
        Settings getSettings(const std::string& id) const;

        // the animation JSON; false with error set
        bool readAnimation(const std::string& id, std::string& json, std::string& error) const;
        // an image an animation references (its "u" + "p" path, any leading "/" or "./"),
        // looked up as given, then by file name under images/ (v1) and i/ (v2)
        bool readImage(const std::string& path, std::string& data) const;

        // true if the bytes start like a ZIP archive (local file header)
        static bool looksLikeZip(const uint8_t* data, size_t size);

    private:
        ZipArchive archive;
        int version = 1;
        std::vector<Animation> animations;
        Settings topLevel;
        std::string activeAnimationId;
    };

}

#endif //DOTLOTTIE_H
