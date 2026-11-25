// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

namespace jasperengine {
    class Scene;
}

namespace jasperengine::editor {

    // Whether a scene's Ambient Light 2D is bright enough to wash Light 2Ds and normal maps out
    // (docs/specs/S78-sprite-normals-improvements.md R15). Every surface that warns about it asks
    // here (the Sprite Normal Map window's and generate_sprite_normals' ambient_high, the Light 2D
    // Inspector, the MCP light hints), so they agree. Brightness is the intensity times the
    // largest channel of the linear colour: a dark-blue ambient at intensity 1 is not bright.
    inline constexpr float kBrightAmbient2D = 0.8f;
    // what Darken Ambient sets: the colour (sRGB) of a dim night, intensity 1
    inline constexpr float kDarkAmbient2DColor[3] = {0.24f, 0.25f, 0.33f};

    float ambientLight2DBrightness(const Scene& scene);
    bool isAmbientLight2DBright(const Scene& scene);

}
