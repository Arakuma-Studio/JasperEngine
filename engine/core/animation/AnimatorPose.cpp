// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimatorPose.h"

#include "math/simd/SimdFloat4.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <new>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::anim;

namespace{

    std::atomic<int> gBackend{(int)PoseBackend::Native};

    size_t roundUp4(size_t n){
        return (n + 3) & ~(size_t)3;
    }

    float* allocateFloats(size_t count){
        if (count == 0) return nullptr;
        return static_cast<float*>(::operator new(count * sizeof(float), std::align_val_t(16)));
    }

    void freeFloats(float* data){
        if (data) ::operator delete(data, std::align_val_t(16));
    }

    // ---------------------------------------------------------------- kernels

    template<class B>
    struct Kernels{
        typedef typename B::f4 f4;
        typedef typename B::u4 u4;

        static f4 dot4(f4 ax, f4 ay, f4 az, f4 aw, f4 bx, f4 by, f4 bz, f4 bw){
            return B::add(B::add(B::mul(ax, bx), B::mul(ay, by)), B::add(B::mul(az, bz), B::mul(aw, bw)));
        }

        // 1 or -1 per lane: -1 where d < 0
        static f4 signOf(f4 d){
            return B::select(B::cmplt(d, B::zero()), B::set1(-1.0f), B::set1(1.0f));
        }

        // normalises (x, y, z, w); a zero quaternion takes (fx, fy, fz, fw)
        static void normalize(f4& x, f4& y, f4& z, f4& w, f4 fx, f4 fy, f4 fz, f4 fw){
            const f4 len2 = dot4(x, y, z, w, x, y, z, w);
            const u4 tiny = B::cmplt(len2, B::set1(1e-24f));
            const f4 len = B::sqrt(B::select(tiny, B::set1(1.0f), len2));
            x = B::select(tiny, fx, B::div(x, len));
            y = B::select(tiny, fy, B::div(y, len));
            z = B::select(tiny, fz, B::div(z, len));
            w = B::select(tiny, fw, B::div(w, len));
        }

        static void clear(Pose& out){
            std::memset(out.raw(), 0, out.floatCount() * sizeof(float));
        }

        static void accumulate(Pose& acc, const Pose& src, float weight){
            const size_t n = acc.stride();
            const f4 w = B::set1(weight);
            float* a[Pose::ARRAY_COUNT];
            const float* s[Pose::ARRAY_COUNT];
            for (int k = 0; k < Pose::ARRAY_COUNT; k++){
                a[k] = acc.array((Pose::Array)k);
                s[k] = src.array((Pose::Array)k);
            }
            for (size_t i = 0; i < n; i += 4){
                const f4 sqx = B::load(s[Pose::QX] + i), sqy = B::load(s[Pose::QY] + i), sqz = B::load(s[Pose::QZ] + i), sqw = B::load(s[Pose::QW] + i);
                const f4 aqx = B::load(a[Pose::QX] + i), aqy = B::load(a[Pose::QY] + i), aqz = B::load(a[Pose::QZ] + i), aqw = B::load(a[Pose::QW] + i);
                const f4 ws = B::mul(w, signOf(dot4(aqx, aqy, aqz, aqw, sqx, sqy, sqz, sqw)));
                B::store(a[Pose::QX] + i, B::add(aqx, B::mul(ws, sqx)));
                B::store(a[Pose::QY] + i, B::add(aqy, B::mul(ws, sqy)));
                B::store(a[Pose::QZ] + i, B::add(aqz, B::mul(ws, sqz)));
                B::store(a[Pose::QW] + i, B::add(aqw, B::mul(ws, sqw)));
                for (int k : {Pose::TX, Pose::TY, Pose::TZ, Pose::SX, Pose::SY, Pose::SZ}){
                    B::store(a[k] + i, B::add(B::load(a[k] + i), B::mul(w, B::load(s[k] + i))));
                }
            }
            float* ac = acc.channelData();
            const float* sc = src.channelData();
            for (size_t i = 0; i < acc.channelStride(); i += 4){
                B::store(ac + i, B::add(B::load(ac + i), B::mul(w, B::load(sc + i))));
            }
        }

        static void normalizeAcc(Pose& acc, float total, const Pose& fallback){
            const size_t n = acc.stride();
            const f4 t = B::set1(total);
            for (int k : {Pose::TX, Pose::TY, Pose::TZ, Pose::SX, Pose::SY, Pose::SZ}){
                float* p = acc.array((Pose::Array)k);
                for (size_t i = 0; i < n; i += 4) B::store(p + i, B::div(B::load(p + i), t));
            }
            float* qx = acc.array(Pose::QX); float* qy = acc.array(Pose::QY);
            float* qz = acc.array(Pose::QZ); float* qw = acc.array(Pose::QW);
            const float* fx = fallback.array(Pose::QX); const float* fy = fallback.array(Pose::QY);
            const float* fz = fallback.array(Pose::QZ); const float* fw = fallback.array(Pose::QW);
            for (size_t i = 0; i < n; i += 4){
                f4 x = B::load(qx + i), y = B::load(qy + i), z = B::load(qz + i), w = B::load(qw + i);
                normalize(x, y, z, w, B::load(fx + i), B::load(fy + i), B::load(fz + i), B::load(fw + i));
                B::store(qx + i, x); B::store(qy + i, y); B::store(qz + i, z); B::store(qw + i, w);
            }
            float* c = acc.channelData();
            for (size_t i = 0; i < acc.channelStride(); i += 4) B::store(c + i, B::div(B::load(c + i), t));
        }

        static f4 maskLanes(const float* mask, size_t i, f4 weight){
            return mask ? B::mul(weight, B::loadu(mask + i)) : weight;
        }

        static void blend(Pose& out, const Pose& base, const Pose& layer, float weight, const float* mask, const float* channelMask){
            const size_t n = out.stride();
            const f4 w0 = B::set1(weight);
            const f4 one = B::set1(1.0f);
            for (size_t i = 0; i < n; i += 4){
                const f4 w = maskLanes(mask, i, w0);
                const u4 keep = B::cmpeq(w, B::zero());
                for (int k : {Pose::TX, Pose::TY, Pose::TZ, Pose::SX, Pose::SY, Pose::SZ}){
                    const f4 b = B::load(base.array((Pose::Array)k) + i);
                    const f4 l = B::load(layer.array((Pose::Array)k) + i);
                    B::store(out.array((Pose::Array)k) + i, B::select(keep, b, B::add(b, B::mul(B::sub(l, b), w))));
                }
                const f4 bx = B::load(base.array(Pose::QX) + i), by = B::load(base.array(Pose::QY) + i);
                const f4 bz = B::load(base.array(Pose::QZ) + i), bw = B::load(base.array(Pose::QW) + i);
                f4 lx = B::load(layer.array(Pose::QX) + i), ly = B::load(layer.array(Pose::QY) + i);
                f4 lz = B::load(layer.array(Pose::QZ) + i), lw = B::load(layer.array(Pose::QW) + i);
                const f4 sign = signOf(dot4(bx, by, bz, bw, lx, ly, lz, lw));
                const f4 ws = B::mul(w, sign);
                const f4 iw = B::sub(one, w);
                f4 x = B::add(B::mul(bx, iw), B::mul(lx, ws));
                f4 y = B::add(B::mul(by, iw), B::mul(ly, ws));
                f4 z = B::add(B::mul(bz, iw), B::mul(lz, ws));
                f4 q = B::add(B::mul(bw, iw), B::mul(lw, ws));
                normalize(x, y, z, q, bx, by, bz, bw);
                B::store(out.array(Pose::QX) + i, B::select(keep, bx, x));
                B::store(out.array(Pose::QY) + i, B::select(keep, by, y));
                B::store(out.array(Pose::QZ) + i, B::select(keep, bz, z));
                B::store(out.array(Pose::QW) + i, B::select(keep, bw, q));
            }
            for (size_t i = 0; i < out.channelStride(); i += 4){
                const f4 w = maskLanes(channelMask, i, w0);
                const u4 keep = B::cmpeq(w, B::zero());
                const f4 b = B::load(base.channelData() + i);
                const f4 l = B::load(layer.channelData() + i);
                B::store(out.channelData() + i, B::select(keep, b, B::add(b, B::mul(B::sub(l, b), w))));
            }
        }

        static void additive(Pose& out, const Pose& base, const Pose& delta, float weight, const float* mask, const float* channelMask){
            const size_t n = out.stride();
            const f4 w0 = B::set1(weight);
            const f4 one = B::set1(1.0f);
            const f4 zero = B::zero();
            for (size_t i = 0; i < n; i += 4){
                const f4 w = maskLanes(mask, i, w0);
                const u4 keep = B::cmpeq(w, zero);
                for (int k : {Pose::TX, Pose::TY, Pose::TZ}){
                    const f4 b = B::load(base.array((Pose::Array)k) + i);
                    const f4 d = B::load(delta.array((Pose::Array)k) + i);
                    B::store(out.array((Pose::Array)k) + i, B::select(keep, b, B::add(b, B::mul(d, w))));
                }
                for (int k : {Pose::SX, Pose::SY, Pose::SZ}){
                    const f4 b = B::load(base.array((Pose::Array)k) + i);
                    const f4 d = B::load(delta.array((Pose::Array)k) + i);
                    B::store(out.array((Pose::Array)k) + i, B::select(keep, b, B::mul(b, B::add(one, B::mul(B::sub(d, one), w)))));
                }
                // nl = normalize(nlerp(I, d, w)) with d in I's hemisphere
                f4 dx = B::load(delta.array(Pose::QX) + i), dy = B::load(delta.array(Pose::QY) + i);
                f4 dz = B::load(delta.array(Pose::QZ) + i), dw = B::load(delta.array(Pose::QW) + i);
                const f4 sign = signOf(dw);
                const f4 ws = B::mul(w, sign);
                f4 nx = B::mul(dx, ws), ny = B::mul(dy, ws), nz = B::mul(dz, ws);
                f4 nw = B::add(B::sub(one, w), B::mul(dw, ws));
                normalize(nx, ny, nz, nw, zero, zero, zero, one);
                const f4 bx = B::load(base.array(Pose::QX) + i), by = B::load(base.array(Pose::QY) + i);
                const f4 bz = B::load(base.array(Pose::QZ) + i), bw = B::load(base.array(Pose::QW) + i);
                // base * nl (Hamilton)
                f4 rw = B::sub(B::sub(B::mul(bw, nw), B::mul(bx, nx)), B::add(B::mul(by, ny), B::mul(bz, nz)));
                f4 rx = B::add(B::add(B::mul(bw, nx), B::mul(bx, nw)), B::sub(B::mul(by, nz), B::mul(bz, ny)));
                f4 ry = B::add(B::add(B::mul(bw, ny), B::mul(by, nw)), B::sub(B::mul(bz, nx), B::mul(bx, nz)));
                f4 rz = B::add(B::add(B::mul(bw, nz), B::mul(bz, nw)), B::sub(B::mul(bx, ny), B::mul(by, nx)));
                normalize(rx, ry, rz, rw, bx, by, bz, bw);
                B::store(out.array(Pose::QX) + i, B::select(keep, bx, rx));
                B::store(out.array(Pose::QY) + i, B::select(keep, by, ry));
                B::store(out.array(Pose::QZ) + i, B::select(keep, bz, rz));
                B::store(out.array(Pose::QW) + i, B::select(keep, bw, rw));
            }
            for (size_t i = 0; i < out.channelStride(); i += 4){
                const f4 w = maskLanes(channelMask, i, w0);
                const u4 keep = B::cmpeq(w, zero);
                const f4 b = B::load(base.channelData() + i);
                const f4 d = B::load(delta.channelData() + i);
                B::store(out.channelData() + i, B::select(keep, b, B::add(b, B::mul(d, w))));
            }
        }

        static void makeDelta(Pose& delta, const Pose& pose, const Pose& ref){
            const size_t n = delta.stride();
            const f4 one = B::set1(1.0f);
            for (size_t i = 0; i < n; i += 4){
                for (int k : {Pose::TX, Pose::TY, Pose::TZ}){
                    B::store(delta.array((Pose::Array)k) + i, B::sub(B::load(pose.array((Pose::Array)k) + i), B::load(ref.array((Pose::Array)k) + i)));
                }
                for (int k : {Pose::SX, Pose::SY, Pose::SZ}){
                    const f4 r = B::load(ref.array((Pose::Array)k) + i);
                    const u4 zero = B::cmpeq(r, B::zero());
                    const f4 safe = B::select(zero, one, r);
                    B::store(delta.array((Pose::Array)k) + i, B::select(zero, one, B::div(B::load(pose.array((Pose::Array)k) + i), safe)));
                }
                const f4 rx = B::load(ref.array(Pose::QX) + i), ry = B::load(ref.array(Pose::QY) + i);
                const f4 rz = B::load(ref.array(Pose::QZ) + i), rw = B::load(ref.array(Pose::QW) + i);
                const f4 qx = B::load(pose.array(Pose::QX) + i), qy = B::load(pose.array(Pose::QY) + i);
                const f4 qz = B::load(pose.array(Pose::QZ) + i), qw = B::load(pose.array(Pose::QW) + i);
                // conj(ref) * q
                const f4 w = B::add(B::add(B::mul(rw, qw), B::mul(rx, qx)), B::add(B::mul(ry, qy), B::mul(rz, qz)));
                const f4 x = B::add(B::sub(B::mul(rw, qx), B::mul(rx, qw)), B::sub(B::mul(rz, qy), B::mul(ry, qz)));
                const f4 y = B::add(B::sub(B::mul(rw, qy), B::mul(ry, qw)), B::sub(B::mul(rx, qz), B::mul(rz, qx)));
                const f4 z = B::add(B::sub(B::mul(rw, qz), B::mul(rz, qw)), B::sub(B::mul(ry, qx), B::mul(rx, qy)));
                B::store(delta.array(Pose::QX) + i, x);
                B::store(delta.array(Pose::QY) + i, y);
                B::store(delta.array(Pose::QZ) + i, z);
                B::store(delta.array(Pose::QW) + i, w);
            }
            for (size_t i = 0; i < delta.channelStride(); i += 4){
                B::store(delta.channelData() + i, B::sub(B::load(pose.channelData() + i), B::load(ref.channelData() + i)));
            }
        }
    };

    bool useScalar(){
        return gBackend.load(std::memory_order_relaxed) == (int)PoseBackend::Scalar;
    }

}

// ---------------------------------------------------------------- Pose

anim::Pose::Pose(size_t bones, size_t channels){
    resize(bones, channels);
}

anim::Pose::~Pose(){
    freeFloats(data);
}

anim::Pose::Pose(Pose&& other) noexcept
    : data(other.data), bones(other.bones), channels(other.channels), boneStride(other.boneStride), chanStride(other.chanStride){
    other.data = nullptr;
    other.bones = other.channels = other.boneStride = other.chanStride = 0;
}

anim::Pose& anim::Pose::operator=(Pose&& other) noexcept{
    if (this != &other){
        freeFloats(data);
        data = other.data;
        bones = other.bones;
        channels = other.channels;
        boneStride = other.boneStride;
        chanStride = other.chanStride;
        other.data = nullptr;
        other.bones = other.channels = other.boneStride = other.chanStride = 0;
    }
    return *this;
}

void anim::Pose::resize(size_t newBones, size_t newChannels){
    if (data && newBones == bones && newChannels == channels) return;
    freeFloats(data);
    bones = newBones;
    channels = newChannels;
    boneStride = roundUp4(std::max<size_t>(newBones, 1));
    chanStride = roundUp4(newChannels);
    data = allocateFloats(floatCount());
    setIdentity();
}

void anim::Pose::setIdentity(){
    if (!data) return;
    std::memset(data, 0, floatCount() * sizeof(float));
    std::fill(array(QW), array(QW) + boneStride, 1.0f);
    std::fill(array(SX), array(SX) + boneStride, 1.0f);
    std::fill(array(SY), array(SY) + boneStride, 1.0f);
    std::fill(array(SZ), array(SZ) + boneStride, 1.0f);
}

Vector3 anim::Pose::position(size_t bone) const{
    return Vector3(array(TX)[bone], array(TY)[bone], array(TZ)[bone]);
}

Quaternion anim::Pose::rotation(size_t bone) const{
    return Quaternion(array(QW)[bone], array(QX)[bone], array(QY)[bone], array(QZ)[bone]);
}

Vector3 anim::Pose::scale(size_t bone) const{
    return Vector3(array(SX)[bone], array(SY)[bone], array(SZ)[bone]);
}

void anim::Pose::setPosition(size_t bone, const Vector3& v){
    array(TX)[bone] = v.x;
    array(TY)[bone] = v.y;
    array(TZ)[bone] = v.z;
}

void anim::Pose::setRotation(size_t bone, const Quaternion& q){
    array(QX)[bone] = q.x;
    array(QY)[bone] = q.y;
    array(QZ)[bone] = q.z;
    array(QW)[bone] = q.w;
}

void anim::Pose::setScale(size_t bone, const Vector3& v){
    array(SX)[bone] = v.x;
    array(SY)[bone] = v.y;
    array(SZ)[bone] = v.z;
}

void anim::Pose::copyFrom(const Pose& other){
    resize(other.bones, other.channels);
    if (data) std::memcpy(data, other.data, floatCount() * sizeof(float));
}

// ---------------------------------------------------------------- dispatch

PoseBackend anim::poseOpsBackend(){
    return (PoseBackend)gBackend.load(std::memory_order_relaxed);
}

void anim::setPoseOpsBackend(PoseBackend backend){
    gBackend.store((int)backend, std::memory_order_relaxed);
}

const char* anim::poseNativeBackendName(){
    return simd::Native::name;
}

void anim::poseClear(Pose& out){
    Kernels<simd::Scalar>::clear(out);
}

void anim::poseAccumulate(Pose& acc, const Pose& src, float weight){
    if (useScalar()) Kernels<simd::Scalar>::accumulate(acc, src, weight);
    else Kernels<simd::Native>::accumulate(acc, src, weight);
}

void anim::poseNormalize(Pose& acc, float total, const Pose& fallback){
    if (useScalar()) Kernels<simd::Scalar>::normalizeAcc(acc, total, fallback);
    else Kernels<simd::Native>::normalizeAcc(acc, total, fallback);
}

void anim::poseBlend(Pose& out, const Pose& base, const Pose& layer, float weight, const float* mask, const float* channelMask){
    if (useScalar()) Kernels<simd::Scalar>::blend(out, base, layer, weight, mask, channelMask);
    else Kernels<simd::Native>::blend(out, base, layer, weight, mask, channelMask);
}

void anim::poseAdditive(Pose& out, const Pose& base, const Pose& delta, float weight, const float* mask, const float* channelMask){
    if (useScalar()) Kernels<simd::Scalar>::additive(out, base, delta, weight, mask, channelMask);
    else Kernels<simd::Native>::additive(out, base, delta, weight, mask, channelMask);
}

void anim::poseMakeDelta(Pose& delta, const Pose& pose, const Pose& reference){
    if (useScalar()) Kernels<simd::Scalar>::makeDelta(delta, pose, reference);
    else Kernels<simd::Native>::makeDelta(delta, pose, reference);
}

void anim::poseIdentityDelta(Pose& delta){
    delta.setIdentity();
}

// ---------------------------------------------------------------- Affine and rig space

Vector3 anim::Affine::transformPoint(const Vector3& p) const{
    return Vector3(m[0] * p.x + m[3] * p.y + m[6] * p.z + m[9],
                   m[1] * p.x + m[4] * p.y + m[7] * p.z + m[10],
                   m[2] * p.x + m[5] * p.y + m[8] * p.z + m[11]);
}

Vector3 anim::Affine::transformVector(const Vector3& v) const{
    return Vector3(m[0] * v.x + m[3] * v.y + m[6] * v.z,
                   m[1] * v.x + m[4] * v.y + m[7] * v.z,
                   m[2] * v.x + m[5] * v.y + m[8] * v.z);
}

anim::Affine anim::Affine::operator*(const Affine& b) const{
    Affine r;
    for (int c = 0; c < 3; c++){
        for (int row = 0; row < 3; row++){
            r.m[c * 3 + row] = m[row] * b.m[c * 3] + m[3 + row] * b.m[c * 3 + 1] + m[6 + row] * b.m[c * 3 + 2];
        }
    }
    for (int row = 0; row < 3; row++){
        r.m[9 + row] = m[row] * b.m[9] + m[3 + row] * b.m[10] + m[6 + row] * b.m[11] + m[9 + row];
    }
    return r;
}

anim::Affine anim::Affine::inverse() const{
    const float a = m[0], b = m[3], c = m[6];
    const float d = m[1], e = m[4], f = m[7];
    const float g = m[2], h = m[5], i = m[8];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    Affine r;
    if (!(std::fabs(det) > 1e-30f) || !std::isfinite(det)) return r;
    const float inv = 1.0f / det;
    // inverse 3x3 (row-major cofactors transposed into column-major storage)
    const float i00 = A * inv, i01 = -(b * i - c * h) * inv, i02 = (b * f - c * e) * inv;
    const float i10 = B * inv, i11 = (a * i - c * g) * inv, i12 = -(a * f - c * d) * inv;
    const float i20 = C * inv, i21 = -(a * h - b * g) * inv, i22 = (a * e - b * d) * inv;
    r.m[0] = i00; r.m[3] = i01; r.m[6] = i02;
    r.m[1] = i10; r.m[4] = i11; r.m[7] = i12;
    r.m[2] = i20; r.m[5] = i21; r.m[8] = i22;
    const float tx = m[9], ty = m[10], tz = m[11];
    r.m[9] = -(i00 * tx + i01 * ty + i02 * tz);
    r.m[10] = -(i10 * tx + i11 * ty + i12 * tz);
    r.m[11] = -(i20 * tx + i21 * ty + i22 * tz);
    return r;
}

anim::Affine anim::Affine::compose(const Vector3& t, const Quaternion& q, const Vector3& s){
    // the engine's Quaternion::getRotationMatrix, columns scaled (T * R * S)
    const float xx = q.x * q.x, xy = q.x * q.y, xz = q.x * q.z, xw = q.x * q.w;
    const float yy = q.y * q.y, yz = q.y * q.z, yw = q.y * q.w;
    const float zz = q.z * q.z, zw = q.z * q.w;
    Affine r;
    r.m[0] = (1 - 2 * (yy + zz)) * s.x;
    r.m[1] = (2 * (xy + zw)) * s.x;
    r.m[2] = (2 * (xz - yw)) * s.x;
    r.m[3] = (2 * (xy - zw)) * s.y;
    r.m[4] = (1 - 2 * (xx + zz)) * s.y;
    r.m[5] = (2 * (yz + xw)) * s.y;
    r.m[6] = (2 * (xz + yw)) * s.z;
    r.m[7] = (2 * (yz - xw)) * s.z;
    r.m[8] = (1 - 2 * (xx + yy)) * s.z;
    r.m[9] = t.x;
    r.m[10] = t.y;
    r.m[11] = t.z;
    return r;
}

void anim::poseToRigSpaceFrom(const Pose& local, const int* parents, size_t from, RigSpace out){
    const size_t n = local.boneCount();
    const float* tx = local.array(Pose::TX); const float* ty = local.array(Pose::TY); const float* tz = local.array(Pose::TZ);
    const float* qx = local.array(Pose::QX); const float* qy = local.array(Pose::QY);
    const float* qz = local.array(Pose::QZ); const float* qw = local.array(Pose::QW);
    const float* sx = local.array(Pose::SX); const float* sy = local.array(Pose::SY); const float* sz = local.array(Pose::SZ);
    for (size_t i = from; i < n; i++){
        const Vector3 t(tx[i], ty[i], tz[i]);
        const Quaternion q(qw[i], qx[i], qy[i], qz[i]);
        const Vector3 s(sx[i], sy[i], sz[i]);
        const Affine localAffine = Affine::compose(t, q, s);
        const int p = parents[i];
        if (p >= 0){
            out.world[i] = out.world[p] * localAffine;
            out.rotation[i] = out.rotation[p] * q;
            out.scale[i] = out.scale[p] * s;
        }else{
            out.world[i] = localAffine;
            out.rotation[i] = q;
            out.scale[i] = s;
        }
        out.position[i] = out.world[i].translation();
    }
}

void anim::poseToRigSpace(const Pose& local, const int* parents, RigSpace out){
    poseToRigSpaceFrom(local, parents, 0, out);
}
