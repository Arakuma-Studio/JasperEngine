// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXELART_PIXELFXLUA_H
#define PIXELART_PIXELFXLUA_H

// The C++ side of Lua's PixelFX table (docs/specs/S48-pixel-fx.md R19, R24): static members
// only. Every binding file that adds to PixelFX reopens beginClass<PixelFxLua>("PixelFX"), so
// the table stays one class the Lua API tools document.

namespace jasperengine::pixelart {

    struct PixelFxLua {};

}

#endif // PIXELART_PIXELFXLUA_H
