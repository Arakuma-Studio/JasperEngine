// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vat/VatBakeCompute.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::vat;

namespace jasperengine::editor::vat{

// ---- half floats ----

uint16_t floatToHalf(float value){
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const uint32_t exponent = (bits >> 23) & 0xffu;
    uint32_t mantissa = bits & 0x7fffffu;
    if (exponent == 0xffu){
        // inf / NaN (a quiet NaN keeps a mantissa bit)
        return (uint16_t)(sign | 0x7c00u | (mantissa ? 0x200u : 0u));
    }
    const int e = (int)exponent - 127 + 15;
    if (e >= 31) return (uint16_t)(sign | 0x7c00u);
    if (e <= 0){
        // subnormal half (or zero)
        if (e < -10) return (uint16_t)sign;
        mantissa |= 0x800000u;
        const int shift = 14 - e;
        uint32_t half = mantissa >> shift;
        const uint32_t rest = mantissa & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1);
        if (rest > halfway || (rest == halfway && (half & 1u))) half++;
        return (uint16_t)(sign | half);
    }
    uint32_t half = ((uint32_t)e << 10) | (mantissa >> 13);
    const uint32_t rest = mantissa & 0x1fffu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u))) half++;   // may carry into the exponent: correct
    return (uint16_t)(sign | half);
}

float halfToFloat(uint16_t value){
    const uint32_t sign = ((uint32_t)value & 0x8000u) << 16;
    const uint32_t exponent = ((uint32_t)value >> 10) & 0x1fu;
    uint32_t mantissa = (uint32_t)value & 0x3ffu;
    uint32_t bits;
    if (exponent == 0){
        if (mantissa == 0){
            bits = sign;
        }else{
            int e = -1;
            do{
                e++;
                mantissa <<= 1;
            }while ((mantissa & 0x400u) == 0);
            bits = sign | ((uint32_t)(127 - 15 - e) << 23) | ((mantissa & 0x3ffu) << 13);
        }
    }else if (exponent == 31){
        bits = sign | 0x7f800000u | (mantissa << 13);
    }else{
        bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    }
    float out;
    std::memcpy(&out, &bits, 4);
    return out;
}

namespace{

    // ---- double precision affine math (rows 0..2 of a 4x4 matrix, m[row][col]) ----

    struct Affine{
        double m[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};

        static Affine from(const Matrix4& matrix){
            Affine a;
            for (int r = 0; r < 3; r++){
                for (int c = 0; c < 4; c++) a.m[r][c] = matrix.get(c, r);
            }
            return a;
        }

        static Affine zero(){
            Affine a;
            for (int r = 0; r < 3; r++){
                for (int c = 0; c < 4; c++) a.m[r][c] = 0.0;
            }
            return a;
        }

        Affine operator*(const Affine& b) const{
            Affine out;
            for (int r = 0; r < 3; r++){
                for (int c = 0; c < 4; c++){
                    double v = m[r][0] * b.m[0][c] + m[r][1] * b.m[1][c] + m[r][2] * b.m[2][c];
                    if (c == 3) v += m[r][3];
                    out.m[r][c] = v;
                }
            }
            return out;
        }

        void point(const double p[3], double out[3]) const{
            for (int r = 0; r < 3; r++) out[r] = m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3];
        }

        void vector(const double v[3], double out[3]) const{
            for (int r = 0; r < 3; r++) out[r] = m[r][0] * v[0] + m[r][1] * v[1] + m[r][2] * v[2];
        }

        double determinant() const{
            return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                   m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                   m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        }

        // false when singular
        bool inverse(Affine& out) const{
            const double det = determinant();
            if (!(std::fabs(det) > 1e-30) || !std::isfinite(det)) return false;
            const double inv = 1.0 / det;
            double l[3][3];
            l[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * inv;
            l[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * inv;
            l[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * inv;
            l[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * inv;
            l[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * inv;
            l[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * inv;
            l[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * inv;
            l[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * inv;
            l[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * inv;
            for (int r = 0; r < 3; r++){
                for (int c = 0; c < 3; c++) out.m[r][c] = l[r][c];
                out.m[r][3] = -(l[r][0] * m[0][3] + l[r][1] * m[1][3] + l[r][2] * m[2][3]);
            }
            return true;
        }

        // the inverse transpose of the linear part (normals); identity when singular
        void normalMatrix(double out[3][3]) const{
            Affine inv;
            if (!inverse(inv)){
                for (int r = 0; r < 3; r++){
                    for (int c = 0; c < 3; c++) out[r][c] = r == c ? 1.0 : 0.0;
                }
                return;
            }
            for (int r = 0; r < 3; r++){
                for (int c = 0; c < 3; c++) out[r][c] = inv.m[c][r];
            }
        }

        bool nearlyEqual(const Affine& o, double tolerance) const{
            for (int r = 0; r < 3; r++){
                for (int c = 0; c < 4; c++){
                    if (!(std::fabs(m[r][c] - o.m[r][c]) <= tolerance)) return false;
                }
            }
            return true;
        }
    };

    void apply3(const double n[3][3], const double v[3], double out[3]){
        for (int r = 0; r < 3; r++) out[r] = n[r][0] * v[0] + n[r][1] * v[1] + n[r][2] * v[2];
    }

    double length3(const double v[3]){
        return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    }

    bool normalize3(double v[3]){
        const double l = length3(v);
        if (!(l > 1e-20) || !std::isfinite(l)) return false;
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
        return true;
    }

    void cross3(const double a[3], const double b[3], double out[3]){
        out[0] = a[1] * b[2] - a[2] * b[1];
        out[1] = a[2] * b[0] - a[0] * b[2];
        out[2] = a[0] * b[1] - a[1] * b[0];
    }

    double dot3(const double a[3], const double b[3]){
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    // unit quaternion (x, y, z, w) of an orthonormal rotation matrix r[row][col]
    void quaternionFromRotation(const double r[3][3], double q[4]){
        const double trace = r[0][0] + r[1][1] + r[2][2];
        if (trace > 0.0){
            const double s = std::sqrt(trace + 1.0) * 2.0;
            q[3] = 0.25 * s;
            q[0] = (r[2][1] - r[1][2]) / s;
            q[1] = (r[0][2] - r[2][0]) / s;
            q[2] = (r[1][0] - r[0][1]) / s;
        }else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]){
            const double s = std::sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]) * 2.0;
            q[3] = (r[2][1] - r[1][2]) / s;
            q[0] = 0.25 * s;
            q[1] = (r[0][1] + r[1][0]) / s;
            q[2] = (r[0][2] + r[2][0]) / s;
        }else if (r[1][1] > r[2][2]){
            const double s = std::sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]) * 2.0;
            q[3] = (r[0][2] - r[2][0]) / s;
            q[0] = (r[0][1] + r[1][0]) / s;
            q[1] = 0.25 * s;
            q[2] = (r[1][2] + r[2][1]) / s;
        }else{
            const double s = std::sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]) * 2.0;
            q[3] = (r[1][0] - r[0][1]) / s;
            q[0] = (r[0][2] + r[2][0]) / s;
            q[1] = (r[1][2] + r[2][1]) / s;
            q[2] = 0.25 * s;
        }
        double l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (!(l > 0.0) || !std::isfinite(l)){
            q[0] = q[1] = q[2] = 0.0;
            q[3] = 1.0;
            return;
        }
        for (int i = 0; i < 4; i++) q[i] /= l;
    }

    // rotate v by the unit quaternion q (x, y, z, w)
    void rotate(const double q[4], const double v[3], double out[3]){
        const double u[3] = {q[0], q[1], q[2]};
        double c1[3];
        cross3(u, v, c1);
        for (int i = 0; i < 3; i++) c1[i] += q[3] * v[i];
        double c2[3];
        cross3(u, c1, c2);
        for (int i = 0; i < 3; i++) out[i] = v[i] + 2.0 * c2[i];
    }

    // decomposition into rotation, translation and uniform scale (spec 2.2)
    struct BonePose{
        double q[4] = {0, 0, 0, 1};
        double t[3] = {0, 0, 0};
        double s = 1.0;
    };

    BonePose decompose(const Affine& a){
        BonePose pose;
        const double det = a.determinant();
        double s = std::cbrt(std::fabs(det));
        if (det < 0.0) s = -s;
        if (!(std::fabs(s) > 1e-12) || !std::isfinite(s)){
            // a collapsed bone: scale 0 at its translation
            pose.s = 0.0;
            for (int i = 0; i < 3; i++) pose.t[i] = a.m[i][3];
            return pose;
        }
        // the linear part over the scale, orthonormalised (Gram-Schmidt on the columns, the
        // third from the first two so it stays a proper rotation)
        double x[3] = {a.m[0][0] / s, a.m[1][0] / s, a.m[2][0] / s};
        double y[3] = {a.m[0][1] / s, a.m[1][1] / s, a.m[2][1] / s};
        if (!normalize3(x)){
            x[0] = 1.0; x[1] = 0.0; x[2] = 0.0;
        }
        const double d = dot3(x, y);
        for (int i = 0; i < 3; i++) y[i] -= d * x[i];
        if (!normalize3(y)){
            const double axis[3] = {std::fabs(x[0]) < 0.9 ? 1.0 : 0.0, std::fabs(x[0]) < 0.9 ? 0.0 : 1.0, 0.0};
            double z0[3];
            cross3(x, axis, z0);
            normalize3(z0);
            cross3(z0, x, y);
        }
        double z[3];
        cross3(x, y, z);
        const double r[3][3] = {{x[0], y[0], z[0]}, {x[1], y[1], z[1]}, {x[2], y[2], z[2]}};
        quaternionFromRotation(r, pose.q);
        for (int i = 0; i < 3; i++) pose.t[i] = a.m[i][3];
        pose.s = s;
        return pose;
    }

    void applyPose(const BonePose& pose, const double p[3], double out[3]){
        double scaled[3] = {p[0] * pose.s, p[1] * pose.s, p[2] * pose.s};
        rotate(pose.q, scaled, out);
        for (int i = 0; i < 3; i++) out[i] += pose.t[i];
    }

    // the shader's blend of two bone poses: nlerp (b on a's side), lerp of translation and scale
    BonePose blendPoses(const BonePose& a, const BonePose& b, double t){
        BonePose out;
        const double dot = a.q[0] * b.q[0] + a.q[1] * b.q[1] + a.q[2] * b.q[2] + a.q[3] * b.q[3];
        const double sign = dot < 0.0 ? -1.0 : 1.0;
        double l = 0.0;
        for (int i = 0; i < 4; i++){
            out.q[i] = a.q[i] + (sign * b.q[i] - a.q[i]) * t;
            l += out.q[i] * out.q[i];
        }
        l = std::sqrt(l);
        if (l > 0.0){
            for (int i = 0; i < 4; i++) out.q[i] /= l;
        }else{
            out.q[0] = out.q[1] = out.q[2] = 0.0;
            out.q[3] = 1.0;
        }
        for (int i = 0; i < 3; i++) out.t[i] = a.t[i] + (b.t[i] - a.t[i]) * t;
        out.s = a.s + (b.s - a.s) * t;
        return out;
    }

    // the tangent frame quaternion (columns tangent, normal x tangent, normal)
    void frameQuaternion(const double normal[3], const double tangent[3], double q[4]){
        double n[3] = {normal[0], normal[1], normal[2]};
        if (!normalize3(n)){
            q[0] = q[1] = q[2] = 0.0;
            q[3] = 1.0;
            return;
        }
        double t[3] = {tangent[0], tangent[1], tangent[2]};
        const double d = dot3(n, t);
        for (int i = 0; i < 3; i++) t[i] -= d * n[i];
        if (!normalize3(t)){
            // any tangent across the normal
            const double axis[3] = {std::fabs(n[0]) < 0.9 ? 1.0 : 0.0, std::fabs(n[0]) < 0.9 ? 0.0 : 1.0, 0.0};
            const double e = dot3(n, axis);
            for (int i = 0; i < 3; i++) t[i] = axis[i] - e * n[i];
            normalize3(t);
        }
        double b[3];
        cross3(n, t, b);
        const double r[3][3] = {{t[0], b[0], n[0]}, {t[1], b[1], n[1]}, {t[2], b[2], n[2]}};
        quaternionFromRotation(r, q);
    }

    // a rest tangent for a vertex without one: the axis least along the normal, across it
    void restTangentFor(const double normal[3], double out[3]){
        double n[3] = {normal[0], normal[1], normal[2]};
        if (!normalize3(n)){
            out[0] = 1.0; out[1] = 0.0; out[2] = 0.0;
            return;
        }
        const double ax = std::fabs(n[0]), ay = std::fabs(n[1]), az = std::fabs(n[2]);
        double axis[3] = {0, 0, 0};
        if (ax <= ay && ax <= az) axis[0] = 1.0;
        else if (ay <= az) axis[1] = 1.0;
        else axis[2] = 1.0;
        const double d = dot3(n, axis);
        for (int i = 0; i < 3; i++) out[i] = axis[i] - d * n[i];
        normalize3(out);
    }

    void floatBounds(VatBounds& bounds, const double p[3]){
        bounds.extend(Vector3((float)p[0], (float)p[1], (float)p[2]));
    }

    double boundsDiagonal(const VatBounds& bounds){
        if (!bounds.valid) return 0.0;
        const double dx = (double)bounds.max.x - bounds.min.x;
        const double dy = (double)bounds.max.y - bounds.min.y;
        const double dz = (double)bounds.max.z - bounds.min.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    std::string mm(double value){
        char text[32];
        if (value >= 100.0) std::snprintf(text, sizeof(text), "%.0f", value);
        else if (value >= 1.0) std::snprintf(text, sizeof(text), "%.2f", value);
        else std::snprintf(text, sizeof(text), "%.3f", value);
        return text;
    }

    bool sameMaterial(const VatMaterialData& a, const VatMaterialData& b){
        return a.name == b.name && std::equal(a.baseColor, a.baseColor + 4, b.baseColor) && a.metallic == b.metallic &&
               a.roughness == b.roughness && std::equal(a.emissive, a.emissive + 3, b.emissive) && a.alphaMode == b.alphaMode &&
               a.alphaCutoff == b.alphaCutoff && a.doubleSided == b.doubleSided && a.baseColorTexture == b.baseColorTexture &&
               a.normalTexture == b.normalTexture && a.metallicRoughnessTexture == b.metallicRoughnessTexture &&
               a.occlusionTexture == b.occlusionTexture && a.emissiveTexture == b.emissiveTexture;
    }

    // ---- the bake ----

    // a bone candidate: a joint of a skinned renderer, or a renderer's unskinned vertices
    struct Candidate{
        int renderer = 0;
        int joint = -1;     // -1 = rigid
    };

    struct MergedVertex{
        int renderer = 0;
        uint32_t vertex = 0;        // the representative source vertex
        bool normal = false;
        bool tangent = false;
        int candidates[4] = {-1, -1, -1, -1};
        float weights[4] = {0, 0, 0, 0};
    };

    class Baker{
    public:
        Baker(const VatBakeInput& input, VatBakeOutput& output, const std::atomic<bool>* cancel,
              const std::function<void(float)>& progress)
            : in(input), out(output), cancel(cancel), progress(progress){}

        bool run(std::string& error);

    private:
        const VatBakeInput& in;
        VatBakeOutput& out;
        const std::atomic<bool>* cancel;
        const std::function<void(float)>& progress;

        size_t totalWork = 1;
        size_t doneWork = 0;

        // topology
        std::vector<MergedVertex> merged;
        std::vector<std::vector<int>> mergedOf;    // per renderer, per source vertex: merged index or -1
        std::vector<Affine> restToRoot;
        std::vector<Affine> rootToRest;            // inverse(restToRoot)
        std::vector<int> identitySize;             // per renderer: identity skin matrices for its bone ids

        // rest (root space)
        std::vector<double> bindPositions;         // 3 per merged vertex: the pre-skin rest (Bones rest)
        std::vector<double> bindNormals;
        std::vector<double> bindTangents;          // 4 (w = sign)
        std::vector<double> restTangentDir;        // 3: the frame's tangent for vertices without one
        std::vector<double> vertexRest;            // 3: the Vertices rest (bind, or the first frame)
        bool mixedRest = false;

        // candidates
        std::vector<Candidate> candidates;
        std::vector<std::map<int, int>> jointCandidate;    // per renderer: joint -> candidate
        std::vector<int> rigidCandidate;                   // per renderer, -1 until used
        bool rigidOnly = true;

        // merged bones: candidate -> bone, and each bone's first candidate
        std::vector<int> boneOf;
        std::vector<int> representative;

        // decisions
        bool doVertices = true;
        bool doBones = true;
        double tolerance = 1.0;                    // mm, scaled

        // per sample buffers
        std::vector<cpuskin::CpuSkinResult> deformed;
        std::vector<double> exactPositions;
        std::vector<double> exactNormals;
        std::vector<double> exactTangents;
        std::vector<Affine> sampleToRoot;

        // frames pass results
        int samples = 0;
        std::vector<int> clipStart;
        std::vector<float> staging;                // Vertices: samples x V x S x 4 exact
        std::vector<BonePose> candidatePoses;      // samples x candidates (decomposed, continuous)
        std::vector<Affine> candidateMatrices;     // samples x candidates
        std::vector<VatBounds> clipBounds;
        VatBounds totalBounds;
        std::vector<float> socketData;
        double rawMax = 0.0, rawSum = 0.0;
        double decompMax = 0.0, decompSum = 0.0;
        double halfMax = 0.0, halfSum = 0.0;
        uint64_t frameSamplesMeasured = 0;
        std::vector<double> clipFrameErrorHalf;    // per clip, Vertices half error at its frames
        std::vector<double> clipFrameErrorBones;
        std::vector<float> vertexErrorHalf;        // per vertex, the same at every frame
        std::vector<float> vertexErrorBones;

        bool cancelled() const{ return cancel && cancel->load(); }
        void tick(){
            doneWork++;
            if (progress) progress(std::min(1.0f, (float)doneWork / (float)totalWork));
        }

        bool buildTopology(std::string& error);
        bool restDeform(std::string& error);
        bool deformSample(const VatSampleState& sample, const char* what, std::string& error);
        Affine candidateMatrix(int candidate, const VatSampleState& sample) const;
        bool framesPass(std::string& error);
        // candidates with equal matrices at every frame share one bone (hashed, then compared)
        void mergeCandidates();
        bool decide(std::string& error);
        bool midsPass(std::string& error);
        bool build(std::string& error);

        int texelsPerSample() const{ return in.normals ? 2 : 1; }
        // the frame's error of Vertices at the chosen precision (staging offsets read back)
        void vertexValue(int globalFrame, size_t vertex, double offset[3]) const;
    };

    bool Baker::buildTopology(std::string& error){
        const size_t rendererCount = in.renderers.size();
        mergedOf.assign(rendererCount, {});
        restToRoot.resize(rendererCount);
        rootToRest.resize(rendererCount);
        identitySize.assign(rendererCount, 0);
        jointCandidate.assign(rendererCount, {});
        rigidCandidate.assign(rendererCount, -1);

        // limited lattice bindings: no sharing between submeshes of that renderer
        std::vector<bool> limited(rendererCount, false);
        auto markLimited = [&](size_t r, const cpuskin::CpuSkinLattice& lattice){
            for (const cpuskin::CpuSkinLatticeBinding& binding : lattice.bindings){
                if (binding.submesh >= 0) limited[r] = true;
            }
        };
        for (size_t r = 0; r < rendererCount; r++) markLimited(r, in.renderers[r].restLattice);
        for (const VatBakeClipInput& clip : in.clips){
            for (const std::vector<VatSampleState>* list : {&clip.samples, &clip.mids}){
                for (const VatSampleState& sample : *list){
                    for (size_t r = 0; r < sample.renderers.size() && r < rendererCount; r++) markLimited(r, sample.renderers[r].lattice);
                }
            }
        }

        // materials in first use order
        std::vector<int> materialMap(in.materials.size(), -1);
        std::vector<std::vector<uint32_t>> groups;      // per output material slot (+ the default)
        std::vector<int> groupMaterial;
        std::map<int, size_t> groupOf;                  // output material (-1 default) -> group
        bool droppedPrimitives = false;

        for (size_t r = 0; r < rendererCount; r++){
            const VatBakeRenderer& renderer = in.renderers[r];
            const cpuskin::CpuSkinSource& source = renderer.source;
            restToRoot[r] = Affine::from(renderer.restToRoot);
            if (!restToRoot[r].inverse(rootToRest[r])){
                error = "the transform of '" + renderer.name + "' collapses it (a scale of 0): it can't be baked";
                return false;
            }
            mergedOf[r].assign(source.vertexCount(), -1);
            int maxBone = -1;
            for (const std::array<int, 4>& ids : source.boneIds){
                for (int id : ids) maxBone = std::max(maxBone, id);
            }
            identitySize[r] = maxBone + 1;

            for (size_t s = 0; s < source.submeshes.size(); s++){
                const cpuskin::CpuSkinSubmesh& submesh = source.submeshes[s];
                // the triangle list it draws
                std::vector<uint32_t> triangles;
                const std::vector<uint32_t>& indices = submesh.indices;
                if (submesh.primitive == PrimitiveType::TRIANGLES){
                    triangles.assign(indices.begin(), indices.begin() + (long)(indices.size() / 3 * 3));
                }else if (submesh.primitive == PrimitiveType::TRIANGLE_STRIP){
                    for (size_t i = 2; i < indices.size(); i++){
                        if (i % 2 == 0) triangles.insert(triangles.end(), {indices[i - 2], indices[i - 1], indices[i]});
                        else triangles.insert(triangles.end(), {indices[i - 1], indices[i - 2], indices[i]});
                    }
                }else{
                    droppedPrimitives = true;
                    continue;
                }
                if (triangles.empty()) continue;
                int material = -1;
                if (s < renderer.submeshMaterial.size()) material = renderer.submeshMaterial[s];
                int slot = -1;
                if (material >= 0 && (size_t)material < in.materials.size()){
                    // equal materials of different renderers share a slot
                    if (materialMap[material] < 0){
                        int found = -1;
                        for (size_t m = 0; m < out.mesh.materials.size(); m++){
                            if (sameMaterial(out.mesh.materials[m], in.materials[material])) found = (int)m;
                        }
                        if (found < 0){
                            out.mesh.materials.push_back(in.materials[material]);
                            found = (int)out.mesh.materials.size() - 1;
                        }
                        materialMap[material] = found;
                    }
                    slot = materialMap[material];
                }
                auto it = groupOf.find(slot);
                size_t group;
                if (it == groupOf.end()){
                    group = groups.size();
                    groups.emplace_back();
                    groupMaterial.push_back(slot);
                    groupOf[slot] = group;
                }else{
                    group = it->second;
                }
                for (uint32_t index : triangles){
                    if (index >= source.vertexCount()){
                        error = "submesh " + std::to_string(submesh.submesh) + " of '" + renderer.name + "' indexes vertex " +
                                std::to_string(index) + " of " + std::to_string(source.vertexCount());
                        return false;
                    }
                    const uint32_t key = limited[r] ? index : source.canonical[index];
                    int& slotIndex = mergedOf[r][key];
                    if (slotIndex < 0){
                        if (merged.size() >= (size_t)VAT_MAX_VERTICES){
                            VatIssue issue;
                            issue.kind = "vertex_limit";
                            issue.error = true;
                            issue.message = "the merged mesh has more than " + std::to_string(VAT_MAX_VERTICES) + " vertices";
                            issue.fix = "bake fewer or simpler meshes (a LOD of the model)";
                            out.report.warnings.push_back(issue);
                            error = issue.message;
                            return false;
                        }
                        MergedVertex vertex;
                        vertex.renderer = (int)r;
                        vertex.vertex = key;
                        const cpuskin::CpuSkinSubmesh& owner = source.submeshOf(key);
                        vertex.normal = owner.hasNormal;
                        vertex.tangent = owner.hasTangent;
                        slotIndex = (int)merged.size();
                        merged.push_back(vertex);
                    }
                    if (key != index) mergedOf[r][index] = slotIndex;
                    groups[group].push_back((uint32_t)slotIndex);
                }
            }
        }
        if (merged.empty()){
            VatIssue issue;
            issue.kind = "no_mesh";
            issue.error = true;
            issue.message = "nothing under the source draws triangles";
            issue.fix = "bake a GameObject or model with a mesh";
            out.report.warnings.push_back(issue);
            error = issue.message;
            return false;
        }
        if (droppedPrimitives){
            VatIssue issue;
            issue.kind = "material_simplified";
            issue.message = "line and point submeshes are not baked (a Vertex Animation draws triangles)";
            issue.fix = "draw them with a separate GameObject";
            out.report.warnings.push_back(issue);
        }
        for (size_t g = 0; g < groups.size(); g++){
            VatSubmesh submesh;
            submesh.indices = std::move(groups[g]);
            submesh.material = groupMaterial[g];
            out.mesh.submeshes.push_back(std::move(submesh));
        }

        // the bone influences of every merged vertex
        for (MergedVertex& vertex : merged){
            const cpuskin::CpuSkinSource& source = in.renderers[vertex.renderer].source;
            const cpuskin::CpuSkinSubmesh& submesh = source.submeshOf(vertex.vertex);
            int count = 0;
            if (submesh.hasSkinning){
                double sum = 0.0;
                for (int k = 0; k < 4; k++) sum += source.boneWeights[vertex.vertex][k];
                if (std::fabs(sum) > 1e-12){
                    const float components[4] = {source.boneWeights[vertex.vertex].x, source.boneWeights[vertex.vertex].y,
                                                 source.boneWeights[vertex.vertex].z, source.boneWeights[vertex.vertex].w};
                    for (int k = 0; k < 4; k++){
                        if (components[k] == 0.0f) continue;
                        const int joint = source.boneIds[vertex.vertex][k];
                        auto it = jointCandidate[vertex.renderer].find(joint);
                        int candidate;
                        if (it == jointCandidate[vertex.renderer].end()){
                            candidate = (int)candidates.size();
                            candidates.push_back({vertex.renderer, joint});
                            jointCandidate[vertex.renderer][joint] = candidate;
                        }else{
                            candidate = it->second;
                        }
                        // a joint listed twice adds up
                        int slot = -1;
                        for (int c = 0; c < count; c++){
                            if (vertex.candidates[c] == candidate) slot = c;
                        }
                        if (slot < 0){
                            slot = count++;
                            vertex.candidates[slot] = candidate;
                            vertex.weights[slot] = 0.0f;
                        }
                        vertex.weights[slot] += (float)(components[k] / sum);
                    }
                }
            }
            if (count == 0){
                int& rigid = rigidCandidate[vertex.renderer];
                if (rigid < 0){
                    rigid = (int)candidates.size();
                    candidates.push_back({vertex.renderer, -1});
                }
                vertex.candidates[0] = rigid;
                vertex.weights[0] = 1.0f;
                count = 1;
            }
            if (count > 1 || std::fabs(vertex.weights[0] - 1.0f) > 1e-6f) rigidOnly = false;
        }
        return true;
    }

    bool Baker::restDeform(std::string& error){
        const size_t vertexCount = merged.size();
        bindPositions.assign(vertexCount * 3, 0.0);
        bindNormals.assign(vertexCount * 3, 0.0);
        bindTangents.assign(vertexCount * 4, 0.0);
        restTangentDir.assign(vertexCount * 3, 0.0);
        deformed.resize(in.renderers.size());
        for (size_t r = 0; r < in.renderers.size(); r++){
            const VatBakeRenderer& renderer = in.renderers[r];
            std::vector<Matrix4> identity((size_t)std::max(identitySize[r], 1));
            std::string failure;
            if (!cpuskin::cpuDeform(renderer.source, identity, renderer.restMorph.data(), &renderer.restLattice, deformed[r], &failure)){
                error = "'" + renderer.name + "' at rest: " + failure;
                return false;
            }
        }
        for (size_t v = 0; v < vertexCount; v++){
            const MergedVertex& vertex = merged[v];
            const cpuskin::CpuSkinResult& result = deformed[vertex.renderer];
            const Affine& a = restToRoot[vertex.renderer];
            double normal[3][3];
            a.normalMatrix(normal);
            const Vector3& p = result.positions[vertex.vertex];
            const double pd[3] = {p.x, p.y, p.z};
            a.point(pd, &bindPositions[v * 3]);
            const Vector3& n = result.normals[vertex.vertex];
            const double nd[3] = {n.x, n.y, n.z};
            double nr[3];
            apply3(normal, nd, nr);
            if (!normalize3(nr)){
                nr[0] = 0.0; nr[1] = 1.0; nr[2] = 0.0;
            }
            std::copy(nr, nr + 3, &bindNormals[v * 3]);
            const Vector4& t = result.tangents[vertex.vertex];
            const double td[3] = {t.x, t.y, t.z};
            double tr[3];
            a.vector(td, tr);
            if (vertex.tangent && normalize3(tr)){
                std::copy(tr, tr + 3, &bindTangents[v * 4]);
                bindTangents[v * 4 + 3] = t.w < 0.0f ? -1.0 : 1.0;
            }else{
                restTangentFor(nr, tr);
                std::copy(tr, tr + 3, &bindTangents[v * 4]);
                bindTangents[v * 4 + 3] = 1.0;
            }
            std::copy(tr, tr + 3, &restTangentDir[v * 3]);
            for (int i = 0; i < 3; i++){
                if (!std::isfinite(bindPositions[v * 3 + i])){
                    error = "'" + in.renderers[vertex.renderer].name + "' has non-finite rest positions";
                    return false;
                }
            }
        }
        vertexRest = bindPositions;
        return true;
    }

    bool Baker::deformSample(const VatSampleState& sample, const char* what, std::string& error){
        if (sample.renderers.size() != in.renderers.size()){
            error = std::string("internal: a sample of ") + what + " has " + std::to_string(sample.renderers.size()) +
                    " renderers, the bake " + std::to_string(in.renderers.size());
            return false;
        }
        sampleToRoot.resize(in.renderers.size());
        for (size_t r = 0; r < in.renderers.size(); r++){
            const VatRendererState& state = sample.renderers[r];
            sampleToRoot[r] = Affine::from(state.toRoot);
            std::string failure;
            if (!cpuskin::cpuDeform(in.renderers[r].source, state.skin, state.morph.data(), &state.lattice, deformed[r], &failure)){
                error = "'" + in.renderers[r].name + "' in " + what + ": " + failure;
                return false;
            }
        }
        const size_t vertexCount = merged.size();
        exactPositions.resize(vertexCount * 3);
        exactNormals.resize(vertexCount * 3);
        exactTangents.resize(vertexCount * 3);
        std::vector<std::array<double, 9>> normalMatrices(in.renderers.size());
        for (size_t r = 0; r < in.renderers.size(); r++){
            double n[3][3];
            sampleToRoot[r].normalMatrix(n);
            for (int i = 0; i < 9; i++) normalMatrices[r][(size_t)i] = n[i / 3][i % 3];
        }
        for (size_t v = 0; v < vertexCount; v++){
            const MergedVertex& vertex = merged[v];
            const cpuskin::CpuSkinResult& result = deformed[vertex.renderer];
            const Affine& a = sampleToRoot[vertex.renderer];
            const Vector3& p = result.positions[vertex.vertex];
            const double pd[3] = {p.x, p.y, p.z};
            a.point(pd, &exactPositions[v * 3]);
            if (!std::isfinite(exactPositions[v * 3]) || !std::isfinite(exactPositions[v * 3 + 1]) || !std::isfinite(exactPositions[v * 3 + 2])){
                error = "'" + in.renderers[vertex.renderer].name + "' has non-finite positions in " + what +
                        " (a skinned vertex without weights, or a bone scaled to 0)";
                return false;
            }
            const std::array<double, 9>& nm = normalMatrices[vertex.renderer];
            const double n3[3][3] = {{nm[0], nm[1], nm[2]}, {nm[3], nm[4], nm[5]}, {nm[6], nm[7], nm[8]}};
            const Vector3& n = result.normals[vertex.vertex];
            const double nd[3] = {n.x, n.y, n.z};
            apply3(n3, nd, &exactNormals[v * 3]);
            normalize3(&exactNormals[v * 3]);
            if (vertex.tangent){
                const Vector4& t = result.tangents[vertex.vertex];
                const double td[3] = {t.x, t.y, t.z};
                a.vector(td, &exactTangents[v * 3]);
            }else{
                std::copy(&restTangentDir[v * 3], &restTangentDir[v * 3] + 3, &exactTangents[v * 3]);
            }
        }
        return true;
    }

    Affine Baker::candidateMatrix(int index, const VatSampleState& sample) const{
        const Candidate& candidate = candidates[(size_t)index];
        const Affine& now = sampleToRoot[(size_t)candidate.renderer];
        const Affine& restInverse = rootToRest[(size_t)candidate.renderer];
        if (candidate.joint < 0) return now * restInverse;
        const std::vector<Matrix4>& skin = sample.renderers[(size_t)candidate.renderer].skin;
        // a joint past the matrices reads zero, as robust buffer access does on the GPU
        const Affine joint = candidate.joint >= 0 && (size_t)candidate.joint < skin.size() ? Affine::from(skin[(size_t)candidate.joint])
                                                                                           : Affine::zero();
        return now * joint * restInverse;
    }

    void Baker::vertexValue(int globalFrame, size_t vertex, double offset[3]) const{
        const size_t at = (((size_t)globalFrame * merged.size() + vertex) * (size_t)texelsPerSample()) * 4;
        for (int i = 0; i < 3; i++){
            const float value = staging[at + (size_t)i];
            offset[i] = out.data.layout.precision == VatPrecision::Half ? (double)halfToFloat(floatToHalf(value)) : (double)value;
        }
    }

    bool Baker::framesPass(std::string& error){
        const size_t vertexCount = merged.size();
        const int perSample = texelsPerSample();
        clipStart.assign(in.clips.size(), 0);
        samples = 0;
        for (size_t c = 0; c < in.clips.size(); c++){
            clipStart[c] = samples;
            samples += (int)in.clips[c].samples.size();
        }
        if (doVertices) staging.assign((size_t)samples * vertexCount * (size_t)perSample * 4, 0.0f);
        if (doBones){
            candidatePoses.assign((size_t)samples * candidates.size(), BonePose());
            candidateMatrices.assign((size_t)samples * candidates.size(), Affine());
        }
        clipBounds.assign(in.clips.size(), VatBounds());
        clipFrameErrorHalf.assign(in.clips.size(), 0.0);
        clipFrameErrorBones.assign(in.clips.size(), 0.0);
        vertexErrorHalf.assign(doVertices ? vertexCount : 0, 0.0f);
        vertexErrorBones.assign(doBones ? vertexCount : 0, 0.0f);
        const size_t socketCount = in.socketPaths.size();
        socketData.assign((size_t)samples * socketCount * 7, 0.0f);

        std::vector<Affine> matrices(candidates.size());
        std::vector<BonePose> poses(candidates.size());
        for (size_t c = 0; c < in.clips.size(); c++){
            const VatBakeClipInput& clip = in.clips[c];
            for (size_t f = 0; f < clip.samples.size(); f++){
                if (cancelled()){
                    error = "cancelled";
                    return false;
                }
                const VatSampleState& sample = clip.samples[f];
                const int global = clipStart[c] + (int)f;
                const std::string what = "clip '" + clip.name + "' frame " + std::to_string(f);
                if (!deformSample(sample, what.c_str(), error)) return false;
                for (size_t v = 0; v < vertexCount; v++){
                    floatBounds(clipBounds[c], &exactPositions[v * 3]);
                }

                if (doVertices){
                    for (size_t v = 0; v < vertexCount; v++){
                        float* texel = &staging[(((size_t)global * vertexCount + v) * (size_t)perSample) * 4];
                        double error2 = 0.0;
                        for (int i = 0; i < 3; i++){
                            const float offset = (float)(exactPositions[v * 3 + (size_t)i] - vertexRest[v * 3 + (size_t)i]);
                            texel[i] = offset;
                            const double d = (double)halfToFloat(floatToHalf(offset)) - (double)offset;
                            error2 += d * d;
                        }
                        texel[3] = 0.0f;
                        const double e = std::sqrt(error2) * 1000.0;
                        halfMax = std::max(halfMax, e);
                        halfSum += e;
                        clipFrameErrorHalf[c] = std::max(clipFrameErrorHalf[c], e);
                        vertexErrorHalf[v] = std::max(vertexErrorHalf[v], (float)e);
                        if (perSample > 1){
                            double q[4];
                            if (merged[v].normal){
                                frameQuaternion(&exactNormals[v * 3], &exactTangents[v * 3], q);
                            }else{
                                q[0] = q[1] = q[2] = 0.0;
                                q[3] = 1.0;
                            }
                            // hemisphere continuity along the clip
                            if (f > 0){
                                const float* previous = &staging[(((size_t)(global - 1) * vertexCount + v) * (size_t)perSample + 1) * 4];
                                const double dot = q[0] * previous[0] + q[1] * previous[1] + q[2] * previous[2] + q[3] * previous[3];
                                if (dot < 0.0){
                                    for (double& x : q) x = -x;
                                }
                            }
                            for (int i = 0; i < 4; i++) texel[4 + i] = (float)q[i];
                        }
                    }
                }

                if (doBones){
                    for (size_t b = 0; b < candidates.size(); b++){
                        matrices[b] = candidateMatrix((int)b, sample);
                        poses[b] = decompose(matrices[b]);
                        if (f > 0){
                            const BonePose& previous = candidatePoses[(size_t)(global - 1) * candidates.size() + b];
                            const double dot = poses[b].q[0] * previous.q[0] + poses[b].q[1] * previous.q[1] +
                                               poses[b].q[2] * previous.q[2] + poses[b].q[3] * previous.q[3];
                            if (dot < 0.0){
                                for (double& x : poses[b].q) x = -x;
                            }
                        }
                        candidatePoses[(size_t)global * candidates.size() + b] = poses[b];
                        candidateMatrices[(size_t)global * candidates.size() + b] = matrices[b];
                    }
                    for (size_t v = 0; v < vertexCount; v++){
                        const MergedVertex& vertex = merged[v];
                        double raw[3] = {0, 0, 0};
                        double decomposed[3] = {0, 0, 0};
                        for (int k = 0; k < 4; k++){
                            if (vertex.candidates[k] < 0) continue;
                            const double w = vertex.weights[k];
                            double p[3];
                            matrices[(size_t)vertex.candidates[k]].point(&bindPositions[v * 3], p);
                            double q[3];
                            applyPose(poses[(size_t)vertex.candidates[k]], &bindPositions[v * 3], q);
                            for (int i = 0; i < 3; i++){
                                raw[i] += w * p[i];
                                decomposed[i] += w * q[i];
                            }
                        }
                        double dr[3], dd[3];
                        for (int i = 0; i < 3; i++){
                            dr[i] = raw[i] - exactPositions[v * 3 + (size_t)i];
                            dd[i] = decomposed[i] - exactPositions[v * 3 + (size_t)i];
                        }
                        const double er = length3(dr) * 1000.0;
                        const double ed = length3(dd) * 1000.0;
                        rawMax = std::max(rawMax, er);
                        rawSum += er;
                        decompMax = std::max(decompMax, ed);
                        decompSum += ed;
                        clipFrameErrorBones[c] = std::max(clipFrameErrorBones[c], ed);
                        vertexErrorBones[v] = std::max(vertexErrorBones[v], (float)ed);
                    }
                }

                for (size_t s = 0; s < socketCount; s++){
                    BonePose pose = decompose(Affine::from(sample.sockets[s]));
                    float* row = &socketData[((size_t)global * socketCount + s) * 7];
                    if (f > 0){
                        const float* previous = &socketData[((size_t)(global - 1) * socketCount + s) * 7];
                        const double dot = pose.q[0] * previous[3] + pose.q[1] * previous[4] + pose.q[2] * previous[5] + pose.q[3] * previous[6];
                        if (dot < 0.0){
                            for (double& x : pose.q) x = -x;
                        }
                    }
                    for (int i = 0; i < 3; i++) row[i] = (float)pose.t[i];
                    for (int i = 0; i < 4; i++) row[3 + i] = (float)pose.q[i];
                }
                frameSamplesMeasured += vertexCount;
                tick();
            }
            totalBounds.extend(clipBounds[c]);
        }
        return true;
    }

    void Baker::mergeCandidates(){
        // candidates sorted by the sum of their matrices over every frame: two that are equal
        // (within 1e-6 per value) have sums within 12 x samples x 1e-6, so each one is compared
        // only to the representatives just before it in that order
        const size_t count = candidates.size();
        const double tolerance = 1e-6;
        std::vector<double> signature(count, 0.0);
        for (size_t b = 0; b < count; b++){
            for (int s = 0; s < samples; s++){
                const Affine& m = candidateMatrices[(size_t)s * count + b];
                for (int r = 0; r < 3; r++){
                    for (int c = 0; c < 4; c++) signature[b] += m.m[r][c];
                }
            }
        }
        std::vector<size_t> order(count);
        for (size_t b = 0; b < count; b++) order[b] = b;
        std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y){ return signature[x] < signature[y]; });
        const double window = 12.0 * (double)std::max(samples, 1) * tolerance;
        std::vector<int> group(count, -1);
        std::vector<size_t> heads;           // representatives in signature order
        for (size_t b : order){
            for (size_t h = heads.size(); h-- > 0;){
                const size_t other = heads[h];
                if (signature[b] - signature[other] > window) break;
                bool same = true;
                for (int s = 0; s < samples && same; s++){
                    same = candidateMatrices[(size_t)s * count + b].nearlyEqual(candidateMatrices[(size_t)s * count + other], tolerance);
                }
                if (same){
                    group[b] = (int)other;
                    break;
                }
            }
            if (group[b] < 0){
                group[b] = (int)b;
                heads.push_back(b);
            }
        }
        // bones numbered by their first candidate
        boneOf.assign(count, -1);
        representative.clear();
        std::vector<int> boneOfHead(count, -1);
        for (size_t b = 0; b < count; b++){
            const size_t head = (size_t)group[b];
            if (boneOfHead[head] < 0){
                boneOfHead[head] = (int)representative.size();
                representative.push_back((int)head);
            }
            boneOf[b] = boneOfHead[head];
        }
    }

    bool Baker::decide(std::string& error){
        const double diagonalMm = boundsDiagonal(totalBounds) * 1000.0;
        tolerance = std::max((double)in.toleranceMm, 0.0005 * diagonalMm);
        out.report.toleranceMm = (float)tolerance;
        out.bonesRawErrorMm = (float)rawMax;
        out.bonesErrorMm = (float)decompMax;
        out.halfErrorMm = (float)halfMax;

        // what moves vertices beyond the bones (for the reasons)
        for (const VatBakeClipInput& clip : in.clips){
            for (const std::vector<VatSampleState>* list : {&clip.samples, &clip.mids}){
                for (const VatSampleState& sample : *list){
                    for (size_t r = 0; r < sample.renderers.size() && r < in.renderers.size(); r++){
                        const VatRendererState& state = sample.renderers[r];
                        const cpuskin::CpuSkinSource& source = in.renderers[r].source;
                        int targets = 0;
                        for (const cpuskin::CpuSkinSubmesh& submesh : source.submeshes) targets = std::max(targets, submesh.morphTargets);
                        for (int t = 0; t < targets && t < MAX_MORPHTARGETS; t++){
                            if (state.morph[(size_t)t] != in.renderers[r].restMorph[(size_t)t]) out.morphAnimated = true;
                        }
                        for (const cpuskin::CpuSkinLatticeBinding& binding : state.lattice.bindings){
                            if (binding.moving) out.latticeAnimated = true;
                        }
                    }
                }
            }
        }

        // unique bones: candidates equal at every frame merge
        if (doBones) mergeCandidates();
        const int bones = (int)representative.size();

        VatMode mode = VatMode::Vertices;
        std::string reason;
        if (in.mode == VatModeChoice::Vertices){
            reason = "Vertices mode was chosen";
        }else if (in.mode == VatModeChoice::Bones){
            if (bones > VAT_MAX_BONES){
                VatIssue issue;
                issue.kind = "bone_limit";
                issue.error = true;
                issue.message = std::to_string(bones) + " bones, a Bones bake holds at most " + std::to_string(VAT_MAX_BONES);
                issue.fix = "bake in Vertices mode";
                issue.fixes.push_back({"Bake in Vertices mode", "mode", "\"vertices\""});
                out.report.warnings.push_back(issue);
                error = issue.message;
                return false;
            }
            mode = VatMode::Bones;
            reason = "Bones mode was chosen";
        }else{
            const bool vertexFits = doVertices;
            if (bones > VAT_MAX_BONES){
                reason = std::to_string(bones) + " bones are over the " + std::to_string(VAT_MAX_BONES) + " a Bones bake holds";
            }else if (decompMax <= tolerance){
                mode = VatMode::Bones;
                reason = std::string(rigidOnly ? "rigid parts only" : "skin and rigid parts only") + ": " + std::to_string(bones) +
                         (bones == 1 ? " bone reproduces" : " bones reproduce") + " every vertex within " + mm(decompMax) +
                         " mm (tolerance " + mm(tolerance) + " mm)";
            }else if (rawMax <= tolerance){
                reason = "non-uniform scale or shear on a bone (nonuniform_bone_scale): its rotation, translation and uniform "
                         "scale leave " + mm(decompMax) + " mm of error (tolerance " + mm(tolerance) + " mm)";
            }else{
                std::string cause = "the deformation";
                if (out.morphAnimated && out.latticeAnimated) cause = "morph targets and lattices";
                else if (out.morphAnimated) cause = "morph targets";
                else if (out.latticeAnimated) cause = "lattice deformation";
                reason = cause + " move" + (cause == "the deformation" ? "s" : "") + " vertices up to " + mm(rawMax) +
                         " mm beyond what the bones explain (tolerance " + mm(tolerance) + " mm)";
            }
            if (mode == VatMode::Vertices && !vertexFits){
                VatSizeInputs size;
                size.mode = VatMode::Vertices;
                size.vertices = (int)merged.size();
                size.bones = bones;
                size.normals = in.normals;
                size.bonesPossible = false;
                size.preset = in.preset;
                for (const VatBakeClipInput& clip : in.clips){
                    VatClipEstimate estimate;
                    estimate.name = clip.name;
                    estimate.length = clip.length;
                    estimate.wrap = clip.wrap;
                    estimate.requestedFps = clip.fps;
                    size.clips.push_back(estimate);
                }
                size.fps = in.clips.empty() ? 30.0f : in.clips[0].fps;
                VatIssue issue = tooLargeIssue(size);
                issue.message = "Vertices mode is needed (" + reason + ") and " + issue.message;
                out.report.warnings.push_back(issue);
                error = issue.message;
                return false;
            }
        }
        out.data.mode = mode;
        out.report.mode = mode;
        out.report.modeReason = reason;
        out.data.layout.bones = mode == VatMode::Bones ? bones : 0;

        // precision
        if (mode == VatMode::Bones){
            out.data.layout.precision = VatPrecision::Float;
        }else if (in.precision == VatPrecisionChoice::Float){
            out.data.layout.precision = VatPrecision::Float;
        }else if (in.precision == VatPrecisionChoice::Half){
            out.data.layout.precision = VatPrecision::Half;
        }else if (halfMax > tolerance){
            out.data.layout.precision = VatPrecision::Float;
            VatIssue issue;
            issue.kind = "precision_switched";
            issue.message = "half precision would move vertices up to " + mm(halfMax) + " mm (tolerance " + mm(tolerance) +
                            " mm): the payload is float (twice the memory)";
            issue.fix = "accept float, or raise tolerance_mm to keep half";
            issue.fixes.push_back({"Keep half precision", "precision", "\"half\""});
            out.report.warnings.push_back(issue);
        }else{
            out.data.layout.precision = VatPrecision::Half;
        }
        out.report.precision = out.data.layout.precision;
        if (mixedRest && mode == VatMode::Vertices){
            VatIssue issue;
            issue.kind = "mixed_rest";
            issue.message = "the source has no usable bind pose: the rest mesh is the first frame of '" + in.clips[0].name + "'";
            issue.fix = "give the model a bind pose to rest in its own shape (only the rest mesh, drawn without the animation, changes)";
            out.report.warnings.push_back(issue);
        }
        out.report.quantizationErrorMm = mode == VatMode::Vertices && out.data.layout.precision == VatPrecision::Half ? (float)halfMax : 0.0f;
        return true;
    }

    bool Baker::midsPass(std::string& error){
        const size_t vertexCount = merged.size();
        const bool bones = out.data.mode == VatMode::Bones;
        double maxError = bones ? decompMax : out.report.quantizationErrorMm;
        double sumError = bones ? decompSum : (out.data.layout.precision == VatPrecision::Half ? halfSum : 0.0);
        uint64_t measured = frameSamplesMeasured;
        // per vertex: the frames' error of the chosen mode and precision, then the mid-frames'
        std::vector<float>& perVertex = out.vertexErrorMm;
        if (bones) perVertex = vertexErrorBones;
        else if (out.data.layout.precision == VatPrecision::Half) perVertex = vertexErrorHalf;
        perVertex.resize(vertexCount, 0.0f);
        if (!bones && out.data.layout.precision != VatPrecision::Half) std::fill(perVertex.begin(), perVertex.end(), 0.0f);
        std::vector<float>().swap(vertexErrorHalf);
        std::vector<float>().swap(vertexErrorBones);

        for (size_t c = 0; c < in.clips.size(); c++){
            const VatBakeClipInput& clip = in.clips[c];
            VatClipReport report;
            report.name = clip.name;
            report.frames = clip.frames;
            report.fps = clip.fps;
            report.maxErrorMm = (float)(bones ? clipFrameErrorBones[c]
                                              : (out.data.layout.precision == VatPrecision::Half ? clipFrameErrorHalf[c] : 0.0));
            double clipInterpolation = 0.0;
            const int frames = (int)clip.samples.size();
            for (size_t m = 0; m < clip.mids.size() && m < clip.midFrames.size(); m++){
                if (cancelled()){
                    error = "cancelled";
                    return false;
                }
                const int f0 = clip.midFrames[m];
                const int f1 = f0 + 1 >= frames ? 0 : f0 + 1;
                const int g0 = clipStart[c] + f0;
                const int g1 = clipStart[c] + f1;
                const std::string what = "clip '" + clip.name + "' between frames " + std::to_string(f0) + " and " + std::to_string(f1);
                if (!deformSample(clip.mids[m], what.c_str(), error)) return false;
                for (size_t v = 0; v < vertexCount; v++){
                    double predicted[3];
                    if (bones){
                        const MergedVertex& vertex = merged[v];
                        for (double& x : predicted) x = 0.0;
                        for (int k = 0; k < 4; k++){
                            if (vertex.candidates[k] < 0) continue;
                            const size_t b = (size_t)vertex.candidates[k];
                            const BonePose& a = candidatePoses[(size_t)g0 * candidates.size() + b];
                            const BonePose& z = candidatePoses[(size_t)g1 * candidates.size() + b];
                            const BonePose blend = clip.interpolate ? blendPoses(a, z, 0.5) : a;
                            double p[3];
                            applyPose(blend, &bindPositions[v * 3], p);
                            for (int i = 0; i < 3; i++) predicted[i] += vertex.weights[k] * p[i];
                        }
                    }else{
                        double o0[3], o1[3];
                        vertexValue(g0, v, o0);
                        vertexValue(g1, v, o1);
                        for (int i = 0; i < 3; i++){
                            const double offset = clip.interpolate ? (o0[i] + o1[i]) * 0.5 : o0[i];
                            predicted[i] = vertexRest[v * 3 + (size_t)i] + offset;
                        }
                    }
                    double d[3];
                    for (int i = 0; i < 3; i++) d[i] = predicted[i] - exactPositions[v * 3 + (size_t)i];
                    const double e = length3(d) * 1000.0;
                    clipInterpolation = std::max(clipInterpolation, e);
                    perVertex[v] = std::max(perVertex[v], (float)e);
                    sumError += e;
                    measured++;
                    // the mid-frames' positions are shown too: culling bounds include them
                    floatBounds(clipBounds[c], &exactPositions[v * 3]);
                }
                tick();
            }
            totalBounds.extend(clipBounds[c]);
            report.interpolationErrorMm = (float)clipInterpolation;
            report.maxErrorMm = std::max(report.maxErrorMm, (float)clipInterpolation);
            maxError = std::max(maxError, clipInterpolation);
            if (clip.interpolate && clipInterpolation > tolerance){
                // error falls with the square of the sample rate for smooth motion
                const double ratio = std::sqrt(clipInterpolation / tolerance);
                const int fps = (int)std::min(240.0, std::ceil((double)clip.fps * ratio));
                VatIssue issue;
                issue.kind = "large_interpolation_error";
                issue.message = "between its frames '" + clip.name + "' is up to " + mm(clipInterpolation) +
                                " mm off the animation (tolerance " + mm(tolerance) + " mm)";
                issue.fix = "raise fps to " + std::to_string(fps) + ", or turn interpolation off for '" + clip.name + "'";
                issue.fixes.push_back({"Raise to " + std::to_string(fps) + " fps", "fps", std::to_string(fps)});
                issue.fixes.push_back({"Interpolation off for " + clip.name, "interpolate", nlohmann::json{{"clip", clip.name}, {"value", false}}.dump()});
                out.report.warnings.push_back(issue);
            }
            out.report.clips.push_back(report);
        }
        out.report.maxErrorMm = (float)maxError;
        out.report.meanErrorMm = measured ? (float)(sumError / (double)measured) : 0.0f;
        return true;
    }

    bool Baker::build(std::string& error){
        const size_t vertexCount = merged.size();
        const bool bones = out.data.mode == VatMode::Bones;
        VatLayout& layout = out.data.layout;
        const VatPresetLimits limits = presetLimits(in.preset);
        layout.width = limits.width;
        layout.samples = samples;
        layout.normals = in.normals;
        layout.colors = false;
        layout.vertices = bones ? 0 : (int)vertexCount;

        // bones: the merged bone list (mergeCandidates)
        if (bones){
            for (int b : representative){
                const Candidate& candidate = candidates[(size_t)b];
                out.boneNames.push_back(in.renderers[(size_t)candidate.renderer].name +
                                        (candidate.joint < 0 ? std::string(" (rigid)") : "/joint " + std::to_string(candidate.joint)));
            }
            layout.bones = (int)representative.size();
        }

        const int elements = bones ? layout.bones : (int)vertexCount;
        const uint64_t texels = payloadTexels(out.data.mode, samples, elements, in.normals);
        if (texels > limits.maxTexels || payloadHeight(texels, limits.width) > limits.maxHeight){
            VatSizeInputs size;
            size.mode = out.data.mode;
            size.vertices = (int)vertexCount;
            size.bones = layout.bones;
            size.normals = in.normals;
            size.bonesPossible = bones || decompMax <= tolerance;
            size.preset = in.preset;
            size.fps = in.clips.empty() ? 30.0f : in.clips[0].fps;
            for (const VatBakeClipInput& clip : in.clips){
                VatClipEstimate estimate;
                estimate.name = clip.name;
                estimate.length = clip.length;
                estimate.wrap = clip.wrap;
                estimate.requestedFps = clip.fps;
                size.clips.push_back(estimate);
            }
            VatIssue issue = tooLargeIssue(size);
            out.report.warnings.push_back(issue);
            error = issue.message;
            return false;
        }
        layout.height = payloadHeight(texels, limits.width);

        // the payload
        VatPayload& payload = out.payload;
        payload.precision = layout.precision;
        payload.width = layout.width;
        payload.height = layout.height;
        const size_t texelCount = (size_t)layout.width * (size_t)layout.height;
        if (layout.precision == VatPrecision::Half){
            payload.bytes.assign(texelCount * 8, 0);
            uint16_t* target = reinterpret_cast<uint16_t*>(payload.bytes.data());
            for (size_t i = 0; i < staging.size(); i++){
                const uint16_t half = floatToHalf(staging[i]);
                std::memcpy(target + i, &half, 2);
            }
        }else if (!bones){
            payload.bytes.assign(texelCount * 16, 0);
            std::memcpy(payload.bytes.data(), staging.data(), staging.size() * sizeof(float));
        }else{
            payload.bytes.assign(texelCount * 16, 0);
            float* target = reinterpret_cast<float*>(payload.bytes.data());
            for (int s = 0; s < samples; s++){
                for (size_t b = 0; b < representative.size(); b++){
                    const BonePose& pose = candidatePoses[(size_t)s * candidates.size() + (size_t)representative[b]];
                    float texel[8] = {(float)pose.q[0], (float)pose.q[1], (float)pose.q[2], (float)pose.q[3],
                                      (float)pose.t[0], (float)pose.t[1], (float)pose.t[2], (float)pose.s};
                    const uint64_t at = vatTexelIndex(VatMode::Bones, layout, s, (int)b, 0);
                    std::memcpy(target + at * 4, texel, sizeof(texel));
                }
            }
        }
        staging.clear();
        staging.shrink_to_fit();

        // the mesh
        VatMeshData& mesh = out.mesh;
        const std::vector<double>& rest = bones ? bindPositions : vertexRest;
        bool anyNormal = false, anyTangent = false, anyUv0 = false, anyUv1 = false, anyColor = false;
        for (const MergedVertex& vertex : merged){
            const cpuskin::CpuSkinSubmesh& submesh = in.renderers[(size_t)vertex.renderer].source.submeshOf(vertex.vertex);
            anyNormal |= submesh.hasNormal;
            anyTangent |= submesh.hasTangent;
            anyUv0 |= submesh.hasUv0;
            anyUv1 |= submesh.hasUv1;
            anyColor |= submesh.colorComponents > 0;
        }
        mesh.positions.resize(vertexCount * 3);
        if (anyNormal) mesh.normals.resize(vertexCount * 3);
        if (anyTangent) mesh.tangents.resize(vertexCount * 4);
        if (anyUv0) mesh.uv0.resize(vertexCount * 2);
        if (anyUv1) mesh.uv1.resize(vertexCount * 2);
        if (anyColor) mesh.colors.resize(vertexCount * 4);
        mesh.vatIndex.resize(vertexCount);
        if (bones){
            mesh.joints.resize(vertexCount * 4);
            mesh.weights.resize(vertexCount * 4);
        }
        for (size_t v = 0; v < vertexCount; v++){
            const MergedVertex& vertex = merged[v];
            const cpuskin::CpuSkinSource& source = in.renderers[(size_t)vertex.renderer].source;
            for (int i = 0; i < 3; i++) mesh.positions[v * 3 + (size_t)i] = (float)rest[v * 3 + (size_t)i];
            if (anyNormal){
                for (int i = 0; i < 3; i++) mesh.normals[v * 3 + (size_t)i] = (float)bindNormals[v * 3 + (size_t)i];
            }
            if (anyTangent){
                for (int i = 0; i < 4; i++) mesh.tangents[v * 4 + (size_t)i] = (float)bindTangents[v * 4 + (size_t)i];
            }
            if (anyUv0){
                mesh.uv0[v * 2] = source.uv0[vertex.vertex].x;
                mesh.uv0[v * 2 + 1] = source.uv0[vertex.vertex].y;
            }
            if (anyUv1){
                mesh.uv1[v * 2] = source.uv1[vertex.vertex].x;
                mesh.uv1[v * 2 + 1] = source.uv1[vertex.vertex].y;
            }
            if (anyColor){
                const Vector4& color = source.colors[vertex.vertex];
                mesh.colors[v * 4] = color.x;
                mesh.colors[v * 4 + 1] = color.y;
                mesh.colors[v * 4 + 2] = color.z;
                mesh.colors[v * 4 + 3] = color.w;
            }
            mesh.vatIndex[v] = (float)v;
            if (bones){
                // the influences on the merged bones (two candidates merged into one bone add up)
                int ids[4] = {0, 0, 0, 0};
                float weights[4] = {0, 0, 0, 0};
                int count = 0;
                for (int k = 0; k < 4; k++){
                    if (vertex.candidates[k] < 0) continue;
                    const int bone = boneOf[(size_t)vertex.candidates[k]];
                    int slot = -1;
                    for (int c = 0; c < count; c++){
                        if (ids[c] == bone) slot = c;
                    }
                    if (slot < 0){
                        slot = count++;
                        ids[slot] = bone;
                    }
                    weights[slot] += vertex.weights[k];
                }
                for (int k = 0; k < 4; k++){
                    mesh.joints[v * 4 + (size_t)k] = (uint16_t)ids[k];
                    mesh.weights[v * 4 + (size_t)k] = weights[k];
                }
            }
        }

        // clips, bounds, sockets, stats
        out.data.clips.clear();
        for (size_t c = 0; c < in.clips.size(); c++){
            const VatBakeClipInput& source = in.clips[c];
            VatClip clip;
            clip.name = source.name;
            clip.start = clipStart[c];
            clip.frames = (int)source.samples.size();
            clip.fps = source.fps;
            clip.wrap = source.wrap;
            clip.speed = source.speed;
            clip.interpolate = source.interpolate;
            clip.bounds = clipBounds[c];
            for (const ClipEventData& event : source.events){
                // the events of the played span (a loop's last frame is its length)
                if (event.time < -1e-6f || event.time > clip.length() + 1e-4f) continue;
                ClipEventData kept = event;
                kept.time = std::max(0.0f, std::min(kept.time, clip.length()));
                clip.events.push_back(kept);
            }
            std::stable_sort(clip.events.begin(), clip.events.end(), [](const ClipEventData& a, const ClipEventData& b){
                return a.time != b.time ? a.time < b.time : a.function < b.function;
            });
            if (clip.events.size() < source.events.size()){
                VatIssue issue;
                issue.kind = "events_dropped";
                issue.message = std::to_string(source.events.size() - clip.events.size()) + " event(s) of '" + clip.name +
                                "' lie past its length and are not kept";
                issue.fix = "move them inside the clip's length in its source";
                out.report.warnings.push_back(issue);
            }
            out.data.clips.push_back(clip);
            VatClipReport& report = out.report.clips[c];
            report.frames = clip.frames;
            report.fps = clip.fps;
            report.bytes = payloadTexels(out.data.mode, clip.frames, elements, in.normals) *
                           (layout.precision == VatPrecision::Float ? 16u : 8u);
        }
        out.data.bounds = totalBounds;

        out.socketTracks.sockets = (int)in.socketPaths.size();
        out.socketTracks.frames = in.socketPaths.empty() ? 0 : samples;
        out.socketTracks.data = in.socketPaths.empty() ? std::vector<float>() : socketData;
        out.data.sockets.clear();
        std::vector<std::string> used;
        for (const std::string& path : in.socketPaths){
            VatSocket socket;
            socket.path = path;
            const size_t slash = path.find_last_of('/');
            std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
            if (base.empty()) base = "Root";
            std::string name = base;
            for (int n = 2; std::find(used.begin(), used.end(), name) != used.end(); n++) name = base + " " + std::to_string(n);
            used.push_back(name);
            socket.name = name;
            out.data.sockets.push_back(socket);
        }

        out.data.stats.bytes = payload.bytes.size();
        out.data.stats.maxErrorMm = out.report.maxErrorMm;
        out.data.stats.meanErrorMm = out.report.meanErrorMm;

        out.report.vertices = (int)vertexCount;
        out.report.bones = layout.bones;
        out.report.width = layout.width;
        out.report.height = layout.height;
        out.report.bytes = payload.bytes.size();
        out.report.fetchesPerVertex = fetchesPerVertex(out.data.mode, in.normals, rigidOnly);
        if (payload.bytes.size() > limits.memoryWarningBytes){
            VatIssue issue;
            issue.kind = "memory_high";
            issue.message = "the payload takes " + std::to_string(payload.bytes.size() / (1024 * 1024)) + " MB, over the " +
                            std::to_string(limits.memoryWarningBytes / (1024 * 1024)) + " MB the " + presetName(in.preset) +
                            " preset suggests";
            issue.fix = "lower fps, drop normals, leave out clips or use Bones mode";
            out.report.warnings.push_back(issue);
        }
        return true;
    }

    bool Baker::run(std::string& error){
        if (in.clips.empty()){
            VatIssue issue;
            issue.kind = "no_animation";
            issue.error = true;
            issue.message = "no clip to bake";
            issue.fix = "choose a source with animations";
            out.report.warnings.push_back(issue);
            error = issue.message;
            return false;
        }
        if (in.clips.size() > (size_t)VAT_MAX_CLIPS){
            VatIssue issue;
            issue.kind = "clip_limit";
            issue.error = true;
            issue.message = std::to_string(in.clips.size()) + " clips, a Vertex Animation holds at most " + std::to_string(VAT_MAX_CLIPS);
            issue.fix = "bake the clips in two assets";
            out.report.warnings.push_back(issue);
            error = issue.message;
            return false;
        }
        for (const VatBakeClipInput& clip : in.clips){
            if ((int)clip.samples.size() != clip.frames || clip.frames < 1 || clip.mids.size() != clip.midFrames.size()){
                error = "internal: clip '" + clip.name + "' has " + std::to_string(clip.samples.size()) + " samples for " +
                        std::to_string(clip.frames) + " frames";
                return false;
            }
            for (const std::vector<VatSampleState>* list : {&clip.samples, &clip.mids}){
                for (const VatSampleState& sample : *list){
                    if (sample.sockets.size() != in.socketPaths.size()){
                        error = "internal: a sample of clip '" + clip.name + "' has " + std::to_string(sample.sockets.size()) + " sockets";
                        return false;
                    }
                }
            }
        }
        if (!buildTopology(error)) return false;
        if (!restDeform(error)) return false;

        for (const VatBakeClipInput& clip : in.clips) totalWork += clip.samples.size() + clip.mids.size();

        // the Vertices rest: degenerate bind poses take the first frame
        if (in.mode != VatModeChoice::Bones){
            bool useFirstFrame = in.restFromFirstFrame;
            if (!useFirstFrame){
                if (!deformSample(in.clips[0].samples[0], "the first frame", error)) return false;
                VatBounds restBounds, firstBounds;
                for (size_t v = 0; v < merged.size(); v++){
                    floatBounds(restBounds, &bindPositions[v * 3]);
                    floatBounds(firstBounds, &exactPositions[v * 3]);
                }
                useFirstFrame = boundsDiagonal(restBounds) < 0.01 * boundsDiagonal(firstBounds);
            }
            if (useFirstFrame){
                if (!deformSample(in.clips[0].samples[0], "the first frame", error)) return false;
                vertexRest = exactPositions;
                mixedRest = true;
            }
        }

        // which payloads the frames pass fills
        const VatPresetLimits limits = presetLimits(in.preset);
        int sampleCount = 0;
        for (const VatBakeClipInput& clip : in.clips) sampleCount += (int)clip.samples.size();
        const uint64_t vertexTexels = payloadTexels(VatMode::Vertices, sampleCount, (int)merged.size(), in.normals);
        doVertices = in.mode != VatModeChoice::Bones &&
                     (in.mode == VatModeChoice::Vertices || vertexTexels <= limits.maxTexels);
        doBones = in.mode != VatModeChoice::Vertices;
        if (in.mode == VatModeChoice::Vertices && (vertexTexels > limits.maxTexels ||
                                                   payloadHeight(vertexTexels, limits.width) > limits.maxHeight)){
            VatSizeInputs size;
            size.mode = VatMode::Vertices;
            size.vertices = (int)merged.size();
            size.bones = (int)candidates.size();
            size.normals = in.normals;
            size.preset = in.preset;
            size.fps = in.clips[0].fps;
            for (const VatBakeClipInput& clip : in.clips){
                VatClipEstimate estimate;
                estimate.name = clip.name;
                estimate.length = clip.length;
                estimate.wrap = clip.wrap;
                estimate.requestedFps = clip.fps;
                size.clips.push_back(estimate);
            }
            VatIssue issue = tooLargeIssue(size);
            out.report.warnings.push_back(issue);
            error = issue.message;
            return false;
        }

        if (!framesPass(error)) return false;
        if (!decide(error)) return false;
        if (!midsPass(error)) return false;
        return build(error);
    }

}

std::vector<int> midFramesFor(int frames, ClipWrapMode wrap){
    std::vector<int> out;
    const int pairs = wrap == ClipWrapMode::Loop ? (frames > 1 ? frames : 0) : frames - 1;
    if (pairs <= 0) return out;
    if (pairs <= VAT_MAX_MID_SAMPLES){
        for (int i = 0; i < pairs; i++) out.push_back(i);
        return out;
    }
    for (int i = 0; i < VAT_MAX_MID_SAMPLES; i++){
        const int frame = (int)std::floor(((double)i + 0.5) * (double)pairs / (double)VAT_MAX_MID_SAMPLES);
        if (out.empty() || out.back() != frame) out.push_back(std::min(frame, pairs - 1));
    }
    return out;
}

bool computeVatBake(const VatBakeInput& input, VatBakeOutput& output, std::string& error, const std::atomic<bool>* cancel,
                    const std::function<void(float)>& progress){
    output = VatBakeOutput();
    Baker baker(input, output, cancel, progress);
    return baker.run(error);
}

void decomposeBone(const Matrix4& matrix, float rotation[4], float translation[3], float& scale){
    const BonePose pose = decompose(Affine::from(matrix));
    for (int i = 0; i < 4; i++) rotation[i] = (float)pose.q[i];
    for (int i = 0; i < 3; i++) translation[i] = (float)pose.t[i];
    scale = (float)pose.s;
}

Vector3 applyBone(const float rotation[4], const float translation[3], float scale, const Vector3& point){
    BonePose pose;
    for (int i = 0; i < 4; i++) pose.q[i] = rotation[i];
    for (int i = 0; i < 3; i++) pose.t[i] = translation[i];
    pose.s = scale;
    const double p[3] = {point.x, point.y, point.z};
    double out[3];
    applyPose(pose, p, out);
    return Vector3((float)out[0], (float)out[1], (float)out[2]);
}

void tangentFrame(const Vector3& normal, const Vector3& tangent, float out[4]){
    const double n[3] = {normal.x, normal.y, normal.z};
    const double t[3] = {tangent.x, tangent.y, tangent.z};
    double q[4];
    frameQuaternion(n, t, q);
    for (int i = 0; i < 4; i++) out[i] = (float)q[i];
}

}
