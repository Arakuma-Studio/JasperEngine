// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_TEST_READBACK_H
#define HTMLUI_TEST_READBACK_H

#include "render/TextureRender.h"

#include <cstdint>
#include <vector>

namespace jasperengine { namespace test {

    // Synchronous GPU readback of an RGBA8 texture (a render target's color attachment),
    // width * height * 4 bytes, rows top first: offscreen targets are top-left origin on
    // every backend (the `orientation` and `harness_component_ui` goldens check it).
    bool readTexture(TextureRender& texture, int width, int height, std::vector<uint8_t>& rgba);

} }

#endif //HTMLUI_TEST_READBACK_H
