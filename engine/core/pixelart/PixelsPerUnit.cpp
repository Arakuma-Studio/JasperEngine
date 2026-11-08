// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Pixels Per Unit of sprites and tilemaps (docs/specs/S41-pixel-perfect-camera.md R6)

#include "component/SpriteComponent.h"

namespace jasperengine {

    float resolvePixelsPerUnit(float pixelsPerUnit){
        return pixelsPerUnit > 0.0f ? pixelsPerUnit : (float)Engine::getPixelsPerUnit();
    }

    Vector2 spriteWorldSize(const SpriteComponent& sprite){
        const float ppu = resolvePixelsPerUnit(sprite.pixelsPerUnit);
        return Vector2((float)sprite.width / ppu, (float)sprite.height / ppu);
    }

}
