// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// T1 R54: the runtime terrain edit module (the operation engine behind Terrain:applyOps and
// setHeights, the edit targets' uploads, the collider refresh). CMake defines JASPERENGINE_TERRAIN_EDIT
// 0 or 1 (a player variant or a Compile build without runtime edits: 0); a build system that defines
// nothing, as the Xcode workspace, keeps the module.
#ifndef JASPERENGINE_TERRAIN_EDIT
#define JASPERENGINE_TERRAIN_EDIT 1
#endif
