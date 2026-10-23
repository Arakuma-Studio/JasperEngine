// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LATTICE_PRESETS_H
#define LATTICE_PRESETS_H

// Presets, resample, mirror and point selection helpers (spec §4). Shared by the editor,
// MCP and Lua so one name gives one result everywhere. All functions are pure.

#include "Engine.h"
#include "math/Vector3.h"
#include "math/Matrix4.h"
#include <string>
#include <vector>

namespace jasperengine{ namespace lattice{

    enum class LatticePreset{
        SQUASH,
        STRETCH,
        TAPER,
        TWIST,
        BEND,
        BULGE,
        SHEAR,
        NOISE
    };

    enum class LatticeAxis{ X = 0, Y = 1, Z = 2 };

    struct JASPERENGINE_API PresetParams{
        LatticePreset preset = LatticePreset::SQUASH;
        LatticeAxis axis = LatticeAxis::Y;
        LatticeAxis toward = LatticeAxis::X; // bend, shear
        float amount = 0.25f;                // see spec §4 per preset (degrees for twist and bend)
        unsigned int seed = 1;               // noise
        bool replace = false;                // false: add to the current offsets
    };

    // name <-> enum ("squash", "stretch", "taper", "twist", "bend", "bulge", "shear", "noise")
    JASPERENGINE_API bool presetFromName(const std::string& name, LatticePreset& out);
    JASPERENGINE_API const char* presetName(LatticePreset preset);
    JASPERENGINE_API std::vector<std::string> presetNames();

    // applies to the selected points (flat indices; empty = all)
    JASPERENGINE_API void applyPreset(int rx, int ry, int rz, std::vector<Vector3>& offsets,
                                      const PresetParams& params, const std::vector<int>& selection = {});

    // offsets of the old grid evaluated (trilinear) at the new grid's rest positions
    JASPERENGINE_API std::vector<Vector3> resample(int oldRx, int oldRy, int oldRz, const std::vector<Vector3>& offsets,
                                                   int newRx, int newRy, int newRz);

    // copies one half onto the other, mirrored across the axis plane; the middle plane loses its axis component
    JASPERENGINE_API void mirror(int rx, int ry, int rz, std::vector<Vector3>& offsets, LatticeAxis axis, bool fromPositive);

    // selection helpers, flat index lists in ascending order
    JASPERENGINE_API std::vector<int> selectAll(int rx, int ry, int rz);
    JASPERENGINE_API std::vector<int> selectFace(int rx, int ry, int rz, LatticeAxis axis, bool positive);
    JASPERENGINE_API std::vector<int> selectExterior(int rx, int ry, int rz);
    JASPERENGINE_API std::vector<int> selectInterior(int rx, int ry, int rz);
    // inclusive ranges; a negative lo means any value on that axis
    JASPERENGINE_API std::vector<int> selectRange(int rx, int ry, int rz, int iLo, int iHi, int jLo, int jHi, int kLo, int kHi);
    JASPERENGINE_API std::vector<int> expandSelection(int rx, int ry, int rz, const std::vector<int>& selection);
    JASPERENGINE_API std::vector<int> shrinkSelection(int rx, int ry, int rz, const std::vector<int>& selection);
    JASPERENGINE_API std::vector<int> invertSelection(int rx, int ry, int rz, const std::vector<int>& selection);
    // current point positions (rest + offsets) tested in world space
    JASPERENGINE_API std::vector<int> selectInWorldBox(int rx, int ry, int rz, const std::vector<Vector3>& offsets,
                                                       const Matrix4& latticeModel, const Vector3& min, const Vector3& max);
    JASPERENGINE_API std::vector<int> selectInWorldSphere(int rx, int ry, int rz, const std::vector<Vector3>& offsets,
                                                          const Matrix4& latticeModel, const Vector3& center, float radius);

    // Lua lines `<var>:setOffset(i, j, k, Vector3(x, y, z))` for the non-zero selected points
    JASPERENGINE_API std::string copyAsLua(const std::string& var, int rx, int ry, int rz,
                                           const std::vector<Vector3>& offsets, const std::vector<int>& selection = {});

}}

#endif //LATTICE_PRESETS_H
