// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef Box2DExport_h
#define Box2DExport_h

// Forced into Box2D sources on MinGW, where base.h never uses dllexport
#include "box2d/base.h"

#undef BOX2D_EXPORT
#define BOX2D_EXPORT __declspec(dllexport)

#endif /* Box2DExport_h */
