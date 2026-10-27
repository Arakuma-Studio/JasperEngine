// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The texture corpus of the cook tests (spec S14 AC4-AC6), generated: photo-like colour noise,
// a tiled albedo, UI art with soft alpha, cut-out foliage, a normal map from a height field, an
// occlusion mask, a smooth gradient and an HDR sky. Every image is a pure function of its name
// and size, so the files the tests write are identical everywhere.

#include "texture/TextureImportSettings.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor::test {

    struct CorpusImage {
        std::string name;               // "photo", "foliage"...
        TextureImportType type = TextureImportType::Default;
        bool alpha = false;
        bool hdr = false;
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;      // 8-bit RGBA (LDR)
        std::vector<float> rgbaFloat;   // linear float RGBA (HDR)
    };

    // the names, in a fixed order
    const std::vector<std::string>& corpusNames();
    CorpusImage corpusImage(const std::string& name, int width = 512, int height = 512);
    // written as <dir>/<name>.png (or .hdr); returns the path
    std::filesystem::path writeCorpusImage(const CorpusImage& image, const std::filesystem::path& dir);

    // a scratch directory of its own under the system temp dir, removed by the destructor
    struct ScratchDir {
        std::filesystem::path path;
        explicit ScratchDir(const std::string& name);
        ~ScratchDir();
        ScratchDir(const ScratchDir&) = delete;
        ScratchDir& operator=(const ScratchDir&) = delete;
    };

}
