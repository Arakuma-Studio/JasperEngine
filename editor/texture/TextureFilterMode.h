// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Unity's Filter Mode (docs/specs/S42-pixel-art-mode.md R3, R7): a texture's import default and the
// project's Default Filter Mode. A reference's own min/mag filters stay the truth for sampling
// (S14 §2); this is what an assignment, or a runtime-constructed Texture, starts with.

#pragma once

#include <cstdint>
#include <string>

namespace jasperengine {
    enum class TextureFilter;   // render/Render.h
}

namespace jasperengine::editor {

    enum class TextureFilterMode : uint8_t { Point, Bilinear, Trilinear };

    const char* textureFilterModeName(TextureFilterMode mode);    // "point", "bilinear", "trilinear"
    bool textureFilterModeFromName(const std::string& name, TextureFilterMode& mode);
    // the min/mag filters a reference gets: NEAREST/NEAREST, LINEAR/LINEAR, LINEAR_MIPMAP_LINEAR/LINEAR
    void textureFilterModeFilters(TextureFilterMode mode, TextureFilter& minFilter, TextureFilter& magFilter);
    // the engine's filter of a project Default Filter Mode (Point: NEAREST, else LINEAR)
    TextureFilter textureFilterModeEngineFilter(TextureFilterMode mode);

}
