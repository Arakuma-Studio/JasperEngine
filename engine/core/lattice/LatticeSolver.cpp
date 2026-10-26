// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LatticeSolver.h"

#include "lattice/LatticeTypes.h"
#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace{

    // a Jacobian determinant at or under this counts as folded (or flattened) in findFoldedCells
    const float FOLDED_STRETCH = 1e-6f;

    int clampResolution(int r){
        return std::min(std::max(r, LATTICE_MIN_RESOLUTION), LATTICE_MAX_RESOLUTION);
    }

    size_t paddedSize(int rx, int ry, int rz){
        return size_t(rx + 2) * size_t(ry + 2) * size_t(rz + 2);
    }

    // The nodes one axis contributes and their weights (spec §3.2). `first` is a padded-grid
    // coordinate (node n sits at n + 1); dw is already multiplied by the derivative factor.
    struct AxisWeights{
        int first;
        int count;
        float w[4];
        float dw[4];
    };

    // weights inside cell c at parameter t in [0, 1]; factor is (r - 1), or 0 when clamped
    void cellWeights(LatticeInterpolation interpolation, int c, float t, float factor, AxisWeights& out){
        if (interpolation == LatticeInterpolation::SHARP || interpolation == LatticeInterpolation::SMOOTH){
            out.first = c + 1;
            out.count = 2;
            float s = t;
            float ds = 1.0f;
            if (interpolation == LatticeInterpolation::SMOOTH){
                s = t * t * (3.0f - 2.0f * t);
                ds = 6.0f * t * (1.0f - t);
            }
            out.w[0] = 1.0f - s;
            out.w[1] = s;
            out.w[2] = 0.0f;
            out.w[3] = 0.0f;
            out.dw[0] = -ds * factor;
            out.dw[1] = ds * factor;
            out.dw[2] = 0.0f;
            out.dw[3] = 0.0f;
        }else{
            // uniform cubic B-spline over nodes c-1 .. c+2
            const float it = 1.0f - t;
            const float t2 = t * t;
            const float t3 = t2 * t;
            out.first = c;
            out.count = 4;
            out.w[0] = it * it * it / 6.0f;
            out.w[1] = (3.0f * t3 - 6.0f * t2 + 4.0f) / 6.0f;
            out.w[2] = (-3.0f * t3 + 3.0f * t2 + 3.0f * t + 1.0f) / 6.0f;
            out.w[3] = t3 / 6.0f;
            out.dw[0] = -(it * it) / 2.0f * factor;
            out.dw[1] = (3.0f * t2 - 4.0f * t) / 2.0f * factor;
            out.dw[2] = (-3.0f * t2 + 2.0f * t + 1.0f) / 2.0f * factor;
            out.dw[3] = t2 / 2.0f * factor;
        }
    }

    // weights at normalized coordinate x (not NaN) on an axis of resolution r
    void axisWeights(LatticeInterpolation interpolation, int r, float x, AxisWeights& out){
        const float xc = std::min(std::max(x, 0.0f), 1.0f);
        const float g = xc * float(r - 1);
        const int c = std::min(int(std::floor(g)), r - 2);
        const float t = g - float(c);
        const float factor = (x >= 0.0f && x <= 1.0f) ? float(r - 1) : 0.0f;
        cellWeights(interpolation, c, t, factor, out);
    }

    // D and its derivatives along x, y, z from the padded grid
    void sampleField(const AxisWeights* ax, const std::vector<Vector3>& padded, int px, int py,
                     Vector3& d, Vector3* dd){
        float sum[3] = {0.0f, 0.0f, 0.0f};
        float sx[3] = {0.0f, 0.0f, 0.0f};
        float sy[3] = {0.0f, 0.0f, 0.0f};
        float sz[3] = {0.0f, 0.0f, 0.0f};
        for (int kz = 0; kz < ax[2].count; kz++){
            const int c = ax[2].first + kz;
            for (int jy = 0; jy < ax[1].count; jy++){
                const int b = ax[1].first + jy;
                const float wyz = ax[1].w[jy] * ax[2].w[kz];
                const float dyz = ax[1].dw[jy] * ax[2].w[kz];
                const float ydz = ax[1].w[jy] * ax[2].dw[kz];
                const size_t row = size_t(px) * (size_t(b) + size_t(py) * size_t(c)) + size_t(ax[0].first);
                for (int ix = 0; ix < ax[0].count; ix++){
                    const Vector3& p = padded[row + size_t(ix)];
                    const float w = ax[0].w[ix] * wyz;
                    const float wx = ax[0].dw[ix] * wyz;
                    const float wy = ax[0].w[ix] * dyz;
                    const float wz = ax[0].w[ix] * ydz;
                    for (int a = 0; a < 3; a++){
                        const float v = p[a];
                        sum[a] += w * v;
                        sx[a] += wx * v;
                        sy[a] += wy * v;
                        sz[a] += wz * v;
                    }
                }
            }
        }
        d = Vector3(sum[0], sum[1], sum[2]);
        dd[0] = Vector3(sx[0], sx[1], sx[2]);
        dd[1] = Vector3(sy[0], sy[1], sy[2]);
        dd[2] = Vector3(sz[0], sz[1], sz[2]);
    }

    // spec §3.3: the outside factor f and its gradient
    float outsideFactor(const LatticeComponent& lattice, const Vector3& u, Vector3& grad){
        grad = Vector3(0.0f, 0.0f, 0.0f);
        if (lattice.outside == LatticeOutside::EXTEND){
            return 1.0f;
        }
        const Vector3 uc(std::min(std::max(u.x, 0.0f), 1.0f),
                         std::min(std::max(u.y, 0.0f), 1.0f),
                         std::min(std::max(u.z, 0.0f), 1.0f));
        const Vector3 diff = u - uc;
        const float d = diff.length();
        if (d <= 0.0f){
            return 1.0f;
        }
        const float fade = lattice.fadeDistance;
        if (!(fade > 0.0f) || d >= fade){
            return 0.0f;
        }
        const float q = d / fade;
        grad = diff * (-(6.0f * q * (1.0f - q) / fade) / d);
        return 1.0f - q * q * (3.0f - 2.0f * q);
    }

    lattice::Evaluation restEvaluation(){
        lattice::Evaluation e;
        e.delta = Vector3(0.0f, 0.0f, 0.0f);
        e.stretch = 1.0f;
        return e;
    }

    // delta = w f D; J = I + w (f dD/du + D (x) grad f), column a = derivative along axis a
    lattice::Evaluation combine(const Vector3& d, const Vector3* dd, float f, const Vector3& grad, float weight){
        lattice::Evaluation e;
        const float wf = weight * f;
        e.delta = d * wf;
        for (int a = 0; a < 3; a++){
            Vector3 column = dd[a] * wf + d * (weight * grad[a]);
            column[a] += 1.0f;
            e.jacobian.setColumn(a, column);
        }
        e.stretch = e.jacobian.determinant();
        return e;
    }

    // compared as numbers (Matrix3::operator== compares bytes, and -0 is still 0 here)
    bool isRest(const lattice::Evaluation& e){
        if (e.delta.x != 0.0f || e.delta.y != 0.0f || e.delta.z != 0.0f){
            return false;
        }
        for (int c = 0; c < 3; c++){
            for (int r = 0; r < 3; r++){
                if (e.jacobian.get(c, r) != (c == r ? 1.0f : 0.0f)){
                    return false;
                }
            }
        }
        return true;
    }

    // inverse of an affine model matrix; false when it is singular (zero scale) or not finite
    bool invertModel(const Matrix4& model, Matrix4& inverse){
        const float det = model.linear().determinant();
        if (!std::isfinite(det) || det == 0.0f){
            return false;
        }
        inverse = model.inverse();
        return inverse.isValid();
    }

    // clamped to [0, 1] (float vertex colours can leave it), so the culling bound of a
    // masked binding (weight * largest offset) holds; includes/lattice.glsl clamps the same
    float maskValue(LatticeMaskChannel mask, const Vector4* color){
        if (!color){
            return 1.0f;
        }
        float value;
        switch (mask){
            case LatticeMaskChannel::RED: value = color->x; break;
            case LatticeMaskChannel::GREEN: value = color->y; break;
            case LatticeMaskChannel::BLUE: value = color->z; break;
            case LatticeMaskChannel::ALPHA: value = color->w; break;
            case LatticeMaskChannel::INVERTED_RED: value = 1.0f - color->x; break;
            case LatticeMaskChannel::INVERTED_GREEN: value = 1.0f - color->y; break;
            case LatticeMaskChannel::INVERTED_BLUE: value = 1.0f - color->z; break;
            case LatticeMaskChannel::INVERTED_ALPHA: value = 1.0f - color->w; break;
            default: return 1.0f;
        }
        return std::min(std::max(value, 0.0f), 1.0f);
    }

}

int lattice::pointCount(int rx, int ry, int rz){
    return clampResolution(rx) * clampResolution(ry) * clampResolution(rz);
}

int lattice::pointIndex(int rx, int ry, int i, int j, int k){
    return i + clampResolution(rx) * (j + clampResolution(ry) * k);
}

Vector3 lattice::restPosition(int rx, int ry, int rz, int i, int j, int k){
    rx = clampResolution(rx);
    ry = clampResolution(ry);
    rz = clampResolution(rz);
    return Vector3(float(i) / float(rx - 1) - 0.5f, float(j) / float(ry - 1) - 0.5f, float(k) / float(rz - 1) - 0.5f);
}

void lattice::buildPaddedGrid(int rx, int ry, int rz, const std::vector<Vector3>& offsets,
                              const std::vector<Vector3>* dynamicOffsets, std::vector<Vector3>& padded){
    rx = clampResolution(rx);
    ry = clampResolution(ry);
    rz = clampResolution(rz);
    const int px = rx + 2;
    const int py = ry + 2;
    padded.resize(paddedSize(rx, ry, rz));
    const std::vector<Vector3>* dynamic = (dynamicOffsets && !dynamicOffsets->empty()) ? dynamicOffsets : nullptr;

    // a = -1 .. r maps to a + 1
    auto at = [&](int a, int b, int c) -> Vector3& {
        return padded[size_t(a + 1) + size_t(px) * (size_t(b + 1) + size_t(py) * size_t(c + 1))];
    };

    // interior: offsets (+ dynamic offsets); entries missing from a short vector count as zero
    for (int k = 0; k < rz; k++){
        for (int j = 0; j < ry; j++){
            for (int i = 0; i < rx; i++){
                const size_t index = size_t(i + rx * (j + ry * k));
                Vector3 v = index < offsets.size() ? offsets[index] : Vector3(0.0f, 0.0f, 0.0f);
                if (dynamic && index < dynamic->size()){
                    v += (*dynamic)[index];
                }
                at(i, j, k) = v;
            }
        }
    }

    // ghosts by linear extrapolation, x first, then y over the x-padded rows, then z over everything
    for (int k = 0; k < rz; k++){
        for (int j = 0; j < ry; j++){
            at(-1, j, k) = at(0, j, k) * 2.0f - at(1, j, k);
            at(rx, j, k) = at(rx - 1, j, k) * 2.0f - at(rx - 2, j, k);
        }
    }
    for (int k = 0; k < rz; k++){
        for (int i = -1; i <= rx; i++){
            at(i, -1, k) = at(i, 0, k) * 2.0f - at(i, 1, k);
            at(i, ry, k) = at(i, ry - 1, k) * 2.0f - at(i, ry - 2, k);
        }
    }
    for (int j = -1; j <= ry; j++){
        for (int i = -1; i <= rx; i++){
            at(i, j, -1) = at(i, j, 0) * 2.0f - at(i, j, 1);
            at(i, j, rz) = at(i, j, rz - 1) * 2.0f - at(i, j, rz - 2);
        }
    }
}

void lattice::buildPaddedGrid(const LatticeComponent& lattice, std::vector<Vector3>& padded){
    buildPaddedGrid(lattice.resolutionX, lattice.resolutionY, lattice.resolutionZ, lattice.offsets,
                    &lattice.dynamicOffsets, padded);
}

lattice::Evaluation lattice::evaluate(const LatticeComponent& lattice, const std::vector<Vector3>& padded,
                                      const Vector3& u, float weight){
    if (!lattice.enabled || weight == 0.0f || std::isnan(u.x) || std::isnan(u.y) || std::isnan(u.z)){
        return restEvaluation();
    }
    const int rx = clampResolution(lattice.resolutionX);
    const int ry = clampResolution(lattice.resolutionY);
    const int rz = clampResolution(lattice.resolutionZ);
    if (padded.size() != paddedSize(rx, ry, rz)){
        return restEvaluation();
    }

    Vector3 grad;
    const float f = outsideFactor(lattice, u, grad);
    if (f == 0.0f){
        return restEvaluation();
    }

    AxisWeights ax[3];
    axisWeights(lattice.interpolation, rx, u.x, ax[0]);
    axisWeights(lattice.interpolation, ry, u.y, ax[1]);
    axisWeights(lattice.interpolation, rz, u.z, ax[2]);

    Vector3 d;
    Vector3 dd[3];
    sampleField(ax, padded, rx + 2, ry + 2, d, dd);
    return combine(d, dd, f, grad, weight);
}

Vector3 lattice::deformWorldPoint(const LatticeComponent& lattice, const std::vector<Vector3>& padded,
                                  const Matrix4& latticeModel, const Vector3& worldPoint, float weight){
    Matrix4 inverse;
    if (!invertModel(latticeModel, inverse)){
        return worldPoint;
    }
    const Vector3 u = inverse * worldPoint + Vector3(0.5f, 0.5f, 0.5f);
    const Evaluation e = evaluate(lattice, padded, u, weight);
    // latticeModel * (u + delta - 0.5) = worldPoint + Lm * delta (exact at rest)
    return worldPoint + latticeModel.linear() * e.delta;
}

Matrix3 lattice::worldJacobian(const LatticeComponent& lattice, const std::vector<Vector3>& padded,
                               const Matrix4& latticeModel, const Vector3& worldPoint, float weight){
    Matrix4 inverse;
    if (!invertModel(latticeModel, inverse)){
        return Matrix3();
    }
    const Vector3 u = inverse * worldPoint + Vector3(0.5f, 0.5f, 0.5f);
    const Evaluation e = evaluate(lattice, padded, u, weight);
    // p' = p + Lm delta(Lm^-1 p + c), so dp'/dp = I + Lm (J - I) Lm^-1 = Lm J Lm^-1 (exactly I at rest)
    return Matrix3() + latticeModel.linear() * (e.jacobian - Matrix3()) * inverse.linear();
}

Matrix4 lattice::objectToNormalized(const Matrix4& latticeModel, const Matrix4& objectModel){
    Matrix4 inverse;
    if (!invertModel(latticeModel, inverse)){
        const float nan = std::nanf("");
        return Matrix4(nan, nan, nan, nan, nan, nan, nan, nan, nan, nan, nan, nan, nan, nan, nan, nan);
    }
    return Matrix4::translateMatrix(0.5f, 0.5f, 0.5f) * inverse * objectModel;
}

void lattice::deformVertex(const ObjectSpaceLattice& binding, Vector3& position, Vector3* normal,
                           Vector3* tangent, float* stretch, const Vector4* color){
    if (!binding.lattice || !binding.padded){
        return;
    }
    const float weight = binding.weight * maskValue(binding.mask, color);
    if (weight == 0.0f){
        return;
    }

    const Vector3 u = binding.objectToNormalized * position;
    const Evaluation e = evaluate(*binding.lattice, *binding.padded, u, weight);
    if (isRest(e)){
        return;
    }

    const Matrix3 a = binding.objectToNormalized.linear();
    const Matrix3 minv = binding.normalizedToObject.linear();

    // Minv * (M p + delta) = p + mat3(Minv) * delta
    position = position + minv * e.delta;

    if (normal){
        const Vector3 n1 = minv.transpose() * (*normal);
        const Vector3 j1 = e.jacobian.column(0);
        const Vector3 j2 = e.jacobian.column(1);
        const Vector3 j3 = e.jacobian.column(2);
        // cof(J) n1 with cof(J) columns (j2 x j3, j3 x j1, j1 x j2): no inverse, safe when J is singular
        const Vector3 n2 = j2.crossProduct(j3) * n1.x + j3.crossProduct(j1) * n1.y + j1.crossProduct(j2) * n1.z;
        Vector3 n = a.transpose() * n2;
        // a direction the deformation collapses to nothing keeps the old normal
        if (n.normalizeL() > 0.0f){
            *normal = n;
        }
    }

    if (tangent){
        Vector3 t = minv * (e.jacobian * (a * (*tangent)));
        if (t.normalizeL() > 0.0f){
            *tangent = t;
        }
    }

    if (stretch){
        *stretch *= e.stretch;
    }
}

float lattice::maxPaddedOffset(const std::vector<Vector3>& padded){
    float result = 0.0f;
    for (const Vector3& p : padded){
        const float length = p.length();
        if (length > result){
            result = length;
        }
    }
    return result;
}

std::vector<std::array<int, 3>> lattice::findFoldedCells(const LatticeComponent& lattice){
    std::vector<std::array<int, 3>> folded;
    const int rx = clampResolution(lattice.resolutionX);
    const int ry = clampResolution(lattice.resolutionY);
    const int rz = clampResolution(lattice.resolutionZ);

    std::vector<Vector3> padded;
    buildPaddedGrid(lattice, padded);

    // the 8 corners and the centre, evaluated inside the cell itself (so a sharp lattice's
    // corners use this cell's one-sided derivatives, not the neighbour's)
    static const float samples[9][3] = {
        {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
        {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1},
        {0.5f, 0.5f, 0.5f}
    };
    const Vector3 noGrad(0.0f, 0.0f, 0.0f);

    for (int ck = 0; ck < rz - 1; ck++){
        for (int cj = 0; cj < ry - 1; cj++){
            for (int ci = 0; ci < rx - 1; ci++){
                bool cellFolded = false;
                for (int s = 0; s < 9 && !cellFolded; s++){
                    AxisWeights ax[3];
                    cellWeights(lattice.interpolation, ci, samples[s][0], float(rx - 1), ax[0]);
                    cellWeights(lattice.interpolation, cj, samples[s][1], float(ry - 1), ax[1]);
                    cellWeights(lattice.interpolation, ck, samples[s][2], float(rz - 1), ax[2]);
                    Vector3 d;
                    Vector3 dd[3];
                    sampleField(ax, padded, rx + 2, ry + 2, d, dd);
                    const float stretch = combine(d, dd, 1.0f, noGrad, 1.0f).stretch;
                    // NaN counts as folded too
                    cellFolded = !(stretch > FOLDED_STRETCH);
                }
                if (cellFolded){
                    folded.push_back({ci, cj, ck});
                }
            }
        }
    }
    return folded;
}
