// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LatticePresets.h"

#include "lattice/LatticeSolver.h"
#include "lattice/LatticeTypes.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <locale>
#include <sstream>

using namespace jasperengine;

namespace{

    struct PresetEntry{
        lattice::LatticePreset preset;
        const char* name;
    };

    const PresetEntry presets[] = {
        {lattice::LatticePreset::SQUASH, "squash"},
        {lattice::LatticePreset::STRETCH, "stretch"},
        {lattice::LatticePreset::TAPER, "taper"},
        {lattice::LatticePreset::TWIST, "twist"},
        {lattice::LatticePreset::BEND, "bend"},
        {lattice::LatticePreset::BULGE, "bulge"},
        {lattice::LatticePreset::SHEAR, "shear"},
        {lattice::LatticePreset::NOISE, "noise"}
    };

    const double PI = 3.14159265358979323846;

    int clampResolution(int r){
        return std::min(std::max(r, LATTICE_MIN_RESOLUTION), LATTICE_MAX_RESOLUTION);
    }

    struct Grid{
        int rx, ry, rz;
        int count;

        Grid(int x, int y, int z): rx(clampResolution(x)), ry(clampResolution(y)), rz(clampResolution(z)), count(rx * ry * rz) {}

        int index(int i, int j, int k) const { return i + rx * (j + ry * k); }
        int resolution(int axis) const { return axis == 0 ? rx : (axis == 1 ? ry : rz); }
        void coords(int index, int* c) const {
            c[0] = index % rx;
            c[1] = (index / rx) % ry;
            c[2] = index / (rx * ry);
        }
    };

    int axisIndex(lattice::LatticeAxis axis){
        return std::min(std::max(int(axis), 0), 2);
    }

    // one flag per point: the valid entries of selection, or every point when it is empty
    std::vector<char> selectionMask(const Grid& grid, const std::vector<int>& selection){
        std::vector<char> mask(size_t(grid.count), selection.empty() ? 1 : 0);
        for (int index : selection){
            if (index >= 0 && index < grid.count){
                mask[size_t(index)] = 1;
            }
        }
        return mask;
    }

    std::vector<int> maskToList(const std::vector<char>& mask){
        std::vector<int> list;
        for (size_t i = 0; i < mask.size(); i++){
            if (mask[i]){
                list.push_back(int(i));
            }
        }
        return list;
    }

    // pcg_hash (Jarzynski and Olano, "Hash Functions for GPU Rendering", 2020): one PCG step
    // and its output permutation, pure 32-bit integer arithmetic, so the same on every platform
    uint32_t pcgHash(uint32_t v){
        const uint32_t state = v * 747796405u + 2891336453u;
        const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
        return (word >> 22u) ^ word;
    }

    // Uniform in the ball of radius `amount`: rejection sampling of points of a 2^24 integer grid
    // over the cube [-1, 1)^3 drawn from the hash of (seed, index, attempt). The test is integer
    // math and the result one exact conversion and one multiply per component, so the value is
    // bit-identical everywhere (no libm calls whose last bit varies between platforms).
    Vector3 noiseOffset(unsigned int seed, int index, float amount){
        const int64_t half = int64_t(1) << 23;
        const uint32_t base = pcgHash(pcgHash(uint32_t(seed)) + uint32_t(index));
        const float scale = amount / 8388608.0f; // amount / 2^23
        for (uint32_t attempt = 0; attempt < 64; attempt++){
            int64_t c[3];
            for (uint32_t a = 0; a < 3; a++){
                c[a] = int64_t(pcgHash(base + attempt * 3u + a) >> 8) - half;
            }
            if (c[0] * c[0] + c[1] * c[1] + c[2] * c[2] < half * half){
                return Vector3(float(c[0]) * scale, float(c[1]) * scale, float(c[2]) * scale);
            }
        }
        // 64 rejections in a row (probability about 1e-20): the centre, still deterministic
        return Vector3(0.0f, 0.0f, 0.0f);
    }

    // R' - R of one point for a preset (spec §4). axis a, the perpendicular axes p1 = a+1 and
    // p2 = a+2 (cyclic, so (a, p1, p2) is right-handed) and b = toward.
    Vector3 presetDisplacement(const lattice::PresetParams& params, int a, int b, const Vector3& rest, int index){
        const double amount = params.amount;
        const int p1 = (a + 1) % 3;
        const int p2 = (a + 2) % 3;
        double r[3] = {rest.x, rest.y, rest.z};
        const double original[3] = {r[0], r[1], r[2]};
        const double along = r[a] + 0.5; // 0 at the -0.5 end, 1 at the +0.5 end

        switch (params.preset){
            case lattice::LatticePreset::SQUASH:
            case lattice::LatticePreset::STRETCH:{
                // scale along the axis, perpendicular 1/sqrt(scale) keeps the volume; the scale
                // stays >= 0.05 (squash amount <= 0.95, stretch amount >= -0.95)
                const double s = params.preset == lattice::LatticePreset::SQUASH
                    ? 1.0 - std::min(amount, 0.95)
                    : 1.0 + std::max(amount, -0.95);
                const double perpendicular = 1.0 / std::sqrt(s);
                r[a] *= s;
                r[p1] *= perpendicular;
                r[p2] *= perpendicular;
                break;
            }
            case lattice::LatticePreset::TAPER:{
                const double s = 1.0 - amount * along;
                r[p1] *= s;
                r[p2] *= s;
                break;
            }
            case lattice::LatticePreset::TWIST:{
                // counterclockwise seen from +axis (p1 toward p2), amount degrees at the +0.5 end
                const double angle = amount * PI / 180.0 * along;
                const double c = std::cos(angle);
                const double s = std::sin(angle);
                const double x = r[p1];
                const double y = r[p2];
                r[p1] = x * c - y * s;
                r[p2] = x * s + y * c;
                break;
            }
            case lattice::LatticePreset::BEND:{
                // In the (axis, toward) plane, with s = a + 0.5 the distance along the axis from
                // the -0.5 end and theta = amount in radians: the line through the centre of the
                // -0.5 end, (a, b) = (-0.5, 0), along +axis becomes the circular arc of length 1
                // that starts there tangent to +axis and turns by theta toward +toward (negative
                // amounts toward -toward), radius R = 1/theta, centre (-0.5, R). The -0.5 end
                // stays put and length along the axis is kept. A point at toward-coordinate b
                // stays at distance b from the arc along its normal:
                //   a' = -0.5 + (R - b) sin(theta s)
                //   b' = R - (R - b) cos(theta s)
                // the third coordinate is unchanged. Written below without the R - R cancellation.
                const double theta = amount * PI / 180.0;
                const double phi = theta * along;
                const double sinPhi = std::sin(phi);
                const double cosPhi = std::cos(phi);
                const double halfSin = std::sin(phi * 0.5);
                const double bv = r[b];
                r[a] = -0.5 + sinPhi / theta - bv * sinPhi;
                r[b] = 2.0 * halfSin * halfSin / theta + bv * cosPhi;
                break;
            }
            case lattice::LatticePreset::BULGE:{
                const double s = 1.0 + amount * (1.0 - (2.0 * r[a]) * (2.0 * r[a]));
                r[p1] *= s;
                r[p2] *= s;
                break;
            }
            case lattice::LatticePreset::SHEAR:{
                r[b] += amount * along;
                break;
            }
            case lattice::LatticePreset::NOISE:
                return noiseOffset(params.seed, index, params.amount);
            default:
                break;
        }
        return Vector3(float(r[0] - original[0]), float(r[1] - original[1]), float(r[2] - original[2]));
    }

    // shortest text that reads back as the same float, in the classic locale (Lua syntax)
    std::string luaNumber(float value){
        if (std::isnan(value)){
            return "(0/0)";
        }
        if (std::isinf(value)){
            return value > 0 ? "math.huge" : "-math.huge";
        }
        if (value == 0.0f){
            return "0"; // also -0
        }
        std::string text;
        for (int precision = 1; precision <= 9; precision++){
            std::ostringstream out;
            out.imbue(std::locale::classic());
            out.precision(precision);
            out << double(value);
            text = out.str();
            std::istringstream in(text);
            in.imbue(std::locale::classic());
            double back = 0.0;
            in >> back;
            if (float(back) == value){
                break;
            }
        }
        return text;
    }

}

bool lattice::presetFromName(const std::string& name, LatticePreset& out){
    for (const PresetEntry& entry : presets){
        if (name == entry.name){
            out = entry.preset;
            return true;
        }
    }
    return false;
}

const char* lattice::presetName(LatticePreset preset){
    for (const PresetEntry& entry : presets){
        if (entry.preset == preset){
            return entry.name;
        }
    }
    return "";
}

std::vector<std::string> lattice::presetNames(){
    std::vector<std::string> names;
    for (const PresetEntry& entry : presets){
        names.push_back(entry.name);
    }
    return names;
}

void lattice::applyPreset(int rx, int ry, int rz, std::vector<Vector3>& offsets,
                          const PresetParams& params, const std::vector<int>& selection){
    const Grid grid(rx, ry, rz);
    offsets.resize(size_t(grid.count), Vector3(0.0f, 0.0f, 0.0f));

    // amount 0 (or not a number) is the rest shape for every preset: nothing to add
    const bool rest = params.amount == 0.0f || !std::isfinite(params.amount);
    if (rest && !params.replace){
        return;
    }

    const int a = axisIndex(params.axis);
    int b = axisIndex(params.toward);
    if (b == a){
        b = (a + 1) % 3; // toward the axis itself means nothing: the next axis
    }

    const std::vector<char> mask = selectionMask(grid, selection);
    for (int index = 0; index < grid.count; index++){
        if (!mask[size_t(index)]){
            continue;
        }
        Vector3 displacement(0.0f, 0.0f, 0.0f);
        if (!rest){
            int c[3];
            grid.coords(index, c);
            displacement = presetDisplacement(params, a, b, restPosition(grid.rx, grid.ry, grid.rz, c[0], c[1], c[2]), index);
        }
        if (params.replace){
            offsets[size_t(index)] = displacement;
        }else{
            offsets[size_t(index)] += displacement;
        }
    }
}

std::vector<Vector3> lattice::resample(int oldRx, int oldRy, int oldRz, const std::vector<Vector3>& offsets,
                                       int newRx, int newRy, int newRz){
    const Grid from(oldRx, oldRy, oldRz);
    const Grid to(newRx, newRy, newRz);
    std::vector<Vector3> result(size_t(to.count), Vector3(0.0f, 0.0f, 0.0f));

    auto old = [&](int i, int j, int k) -> Vector3 {
        const size_t index = size_t(from.index(i, j, k));
        return index < offsets.size() ? offsets[index] : Vector3(0.0f, 0.0f, 0.0f);
    };

    // Trilinear on the old grid. The new point n of an axis sits at n * (old - 1) / (new - 1)
    // old cells: computed in integers, so where the grids coincide t is exactly 0 and the old
    // offset is copied bit for bit.
    int cell[3][LATTICE_MAX_RESOLUTION];
    float param[3][LATTICE_MAX_RESOLUTION];
    for (int axis = 0; axis < 3; axis++){
        const int ro = from.resolution(axis);
        const int rn = to.resolution(axis);
        for (int n = 0; n < rn; n++){
            const int numerator = n * (ro - 1);
            int c = numerator / (rn - 1);
            float t = float(numerator % (rn - 1)) / float(rn - 1);
            if (c > ro - 2){
                c = ro - 2;
                t = 1.0f;
            }
            cell[axis][n] = c;
            param[axis][n] = t;
        }
    }

    for (int k = 0; k < to.rz; k++){
        for (int j = 0; j < to.ry; j++){
            for (int i = 0; i < to.rx; i++){
                const int ci = cell[0][i], cj = cell[1][j], ck = cell[2][k];
                const float tx = param[0][i], ty = param[1][j], tz = param[2][k];
                const float wx[2] = {1.0f - tx, tx};
                const float wy[2] = {1.0f - ty, ty};
                const float wz[2] = {1.0f - tz, tz};
                Vector3 sum(0.0f, 0.0f, 0.0f);
                for (int dz = 0; dz < 2; dz++){
                    for (int dy = 0; dy < 2; dy++){
                        for (int dx = 0; dx < 2; dx++){
                            const float w = wx[dx] * wy[dy] * wz[dz];
                            if (w != 0.0f){
                                sum += old(ci + dx, cj + dy, ck + dz) * w;
                            }
                        }
                    }
                }
                result[size_t(to.index(i, j, k))] = sum;
            }
        }
    }
    return result;
}

void lattice::mirror(int rx, int ry, int rz, std::vector<Vector3>& offsets, LatticeAxis axis, bool fromPositive){
    const Grid grid(rx, ry, rz);
    offsets.resize(size_t(grid.count), Vector3(0.0f, 0.0f, 0.0f));
    const int a = axisIndex(axis);
    const int r = grid.resolution(a);

    for (int index = 0; index < grid.count; index++){
        int c[3];
        grid.coords(index, c);
        const int n = c[a];
        const int m = r - 1 - n;
        Vector3& target = offsets[size_t(index)];
        if (n == m){
            target[a] = 0.0f; // the middle plane
            continue;
        }
        // the target half: the negative half (n < m) when copying from the positive one
        if ((n < m) != fromPositive){
            continue;
        }
        int source[3] = {c[0], c[1], c[2]};
        source[a] = m;
        Vector3 v = offsets[size_t(grid.index(source[0], source[1], source[2]))];
        v[a] = -v[a];
        target = v;
    }
}

std::vector<int> lattice::selectAll(int rx, int ry, int rz){
    const Grid grid(rx, ry, rz);
    std::vector<int> list(size_t(grid.count));
    for (int i = 0; i < grid.count; i++){
        list[size_t(i)] = i;
    }
    return list;
}

std::vector<int> lattice::selectFace(int rx, int ry, int rz, LatticeAxis axis, bool positive){
    const Grid grid(rx, ry, rz);
    const int a = axisIndex(axis);
    const int wanted = positive ? grid.resolution(a) - 1 : 0;
    std::vector<int> list;
    for (int index = 0; index < grid.count; index++){
        int c[3];
        grid.coords(index, c);
        if (c[a] == wanted){
            list.push_back(index);
        }
    }
    return list;
}

std::vector<int> lattice::selectExterior(int rx, int ry, int rz){
    const Grid grid(rx, ry, rz);
    std::vector<int> list;
    for (int index = 0; index < grid.count; index++){
        int c[3];
        grid.coords(index, c);
        for (int a = 0; a < 3; a++){
            if (c[a] == 0 || c[a] == grid.resolution(a) - 1){
                list.push_back(index);
                break;
            }
        }
    }
    return list;
}

std::vector<int> lattice::selectInterior(int rx, int ry, int rz){
    return invertSelection(rx, ry, rz, selectExterior(rx, ry, rz));
}

std::vector<int> lattice::selectRange(int rx, int ry, int rz, int iLo, int iHi, int jLo, int jHi, int kLo, int kHi){
    const Grid grid(rx, ry, rz);
    const int lo[3] = {iLo, jLo, kLo};
    const int hi[3] = {iHi, jHi, kHi};
    std::vector<int> list;
    for (int index = 0; index < grid.count; index++){
        int c[3];
        grid.coords(index, c);
        bool inside = true;
        for (int a = 0; a < 3 && inside; a++){
            if (lo[a] >= 0){
                inside = c[a] >= lo[a] && c[a] <= hi[a];
            }
        }
        if (inside){
            list.push_back(index);
        }
    }
    return list;
}

std::vector<int> lattice::expandSelection(int rx, int ry, int rz, const std::vector<int>& selection){
    const Grid grid(rx, ry, rz);
    std::vector<char> selected(size_t(grid.count), 0);
    for (int index : selection){
        if (index >= 0 && index < grid.count){
            selected[size_t(index)] = 1;
        }
    }
    std::vector<char> result = selected;
    for (int index = 0; index < grid.count; index++){
        if (!selected[size_t(index)]){
            continue;
        }
        int c[3];
        grid.coords(index, c);
        for (int a = 0; a < 3; a++){
            for (int step = -1; step <= 1; step += 2){
                int n[3] = {c[0], c[1], c[2]};
                n[a] += step;
                if (n[a] >= 0 && n[a] < grid.resolution(a)){
                    result[size_t(grid.index(n[0], n[1], n[2]))] = 1;
                }
            }
        }
    }
    return maskToList(result);
}

std::vector<int> lattice::shrinkSelection(int rx, int ry, int rz, const std::vector<int>& selection){
    // the dual of expand: a point stays when none of its grid neighbours is unselected
    // (the outside of the grid does not count, so shrinking everything keeps everything)
    return invertSelection(rx, ry, rz, expandSelection(rx, ry, rz, invertSelection(rx, ry, rz, selection)));
}

std::vector<int> lattice::invertSelection(int rx, int ry, int rz, const std::vector<int>& selection){
    const Grid grid(rx, ry, rz);
    std::vector<char> result(size_t(grid.count), 1);
    for (int index : selection){
        if (index >= 0 && index < grid.count){
            result[size_t(index)] = 0;
        }
    }
    return maskToList(result);
}

std::vector<int> lattice::selectInWorldBox(int rx, int ry, int rz, const std::vector<Vector3>& offsets,
                                           const Matrix4& latticeModel, const Vector3& min, const Vector3& max){
    const Grid grid(rx, ry, rz);
    const Vector3 lo(std::min(min.x, max.x), std::min(min.y, max.y), std::min(min.z, max.z));
    const Vector3 hi(std::max(min.x, max.x), std::max(min.y, max.y), std::max(min.z, max.z));
    std::vector<int> list;
    for (int index = 0; index < grid.count; index++){
        int c[3];
        grid.coords(index, c);
        Vector3 local = restPosition(grid.rx, grid.ry, grid.rz, c[0], c[1], c[2]);
        if (size_t(index) < offsets.size()){
            local += offsets[size_t(index)];
        }
        const Vector3 p = latticeModel * local;
        if (p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y && p.z >= lo.z && p.z <= hi.z){
            list.push_back(index);
        }
    }
    return list;
}

std::vector<int> lattice::selectInWorldSphere(int rx, int ry, int rz, const std::vector<Vector3>& offsets,
                                              const Matrix4& latticeModel, const Vector3& center, float radius){
    const Grid grid(rx, ry, rz);
    std::vector<int> list;
    if (!(radius >= 0.0f)){
        return list;
    }
    const float radius2 = radius * radius;
    for (int index = 0; index < grid.count; index++){
        int c[3];
        grid.coords(index, c);
        Vector3 local = restPosition(grid.rx, grid.ry, grid.rz, c[0], c[1], c[2]);
        if (size_t(index) < offsets.size()){
            local += offsets[size_t(index)];
        }
        const Vector3 p = latticeModel * local;
        if (p.squaredDistance(center) <= radius2){
            list.push_back(index);
        }
    }
    return list;
}

std::string lattice::copyAsLua(const std::string& var, int rx, int ry, int rz,
                               const std::vector<Vector3>& offsets, const std::vector<int>& selection){
    const Grid grid(rx, ry, rz);
    const std::vector<char> mask = selectionMask(grid, selection);
    std::string text;
    for (int index = 0; index < grid.count && size_t(index) < offsets.size(); index++){
        const Vector3& v = offsets[size_t(index)];
        if (!mask[size_t(index)] || (v.x == 0.0f && v.y == 0.0f && v.z == 0.0f)){
            continue;
        }
        int c[3];
        grid.coords(index, c);
        text += var + ":setOffset(" + std::to_string(c[0]) + ", " + std::to_string(c[1]) + ", " + std::to_string(c[2]) +
                ", Vector3(" + luaNumber(v.x) + ", " + luaNumber(v.y) + ", " + luaNumber(v.z) + "))\n";
    }
    return text;
}
