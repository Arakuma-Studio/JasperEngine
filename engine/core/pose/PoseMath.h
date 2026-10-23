// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Rotation helpers of the pose core (spec: pose animation phase 1, section 4.7). Everything here
// works in radians and never goes through Angle::defaultToRad, so the pose maths does not depend
// on the engine's default angle unit. The double-precision Vec3d/Quatd are the working types of
// the pose core (the round-trip tolerances need more than float); the float overloads on the
// engine's Vector3/Quaternion convert at the boundary.
//
// Conventions: quaternions are (w, x, y, z), right-handed, unit length; q1 * q2 applies q2 first;
// q * v rotates v.

#ifndef POSE_MATH_H
#define POSE_MATH_H

#include "math/Quaternion.h"
#include "math/Vector3.h"

#include <algorithm>
#include <cmath>

namespace jasperengine::pose {

    constexpr double POSE_PI = 3.14159265358979323846;

    inline double degToRad(double degrees){ return degrees * (POSE_PI / 180.0); }
    inline double radToDeg(double radians){ return radians * (180.0 / POSE_PI); }

    struct Vec3d {
        double x = 0, y = 0, z = 0;

        Vec3d() = default;
        Vec3d(double px, double py, double pz): x(px), y(py), z(pz) {}
        explicit Vec3d(const Vector3& v): x(v.x), y(v.y), z(v.z) {}

        Vector3 toVector3() const { return Vector3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)); }

        Vec3d operator+(const Vec3d& o) const { return Vec3d(x + o.x, y + o.y, z + o.z); }
        Vec3d operator-(const Vec3d& o) const { return Vec3d(x - o.x, y - o.y, z - o.z); }
        Vec3d operator-() const { return Vec3d(-x, -y, -z); }
        Vec3d operator*(double s) const { return Vec3d(x * s, y * s, z * s); }
        Vec3d operator/(double s) const { return Vec3d(x / s, y / s, z / s); }
        Vec3d& operator+=(const Vec3d& o){ x += o.x; y += o.y; z += o.z; return *this; }
        Vec3d& operator-=(const Vec3d& o){ x -= o.x; y -= o.y; z -= o.z; return *this; }
        Vec3d& operator*=(double s){ x *= s; y *= s; z *= s; return *this; }

        double dot(const Vec3d& o) const { return x * o.x + y * o.y + z * o.z; }
        Vec3d cross(const Vec3d& o) const { return Vec3d(y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x); }
        double length() const { return std::sqrt(x * x + y * y + z * z); }
        double squaredLength() const { return x * x + y * y + z * z; }
        // zero vector stays zero
        Vec3d normalized() const { const double l = length(); return l > 0.0 ? Vec3d(x / l, y / l, z / l) : Vec3d(); }
    };

    struct Quatd {
        double w = 1, x = 0, y = 0, z = 0;

        Quatd() = default;
        Quatd(double qw, double qx, double qy, double qz): w(qw), x(qx), y(qy), z(qz) {}
        explicit Quatd(const Quaternion& q): w(q.w), x(q.x), y(q.y), z(q.z) {}

        Quaternion toQuaternion() const {
            return Quaternion(static_cast<float>(w), static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
        }

        Quatd operator*(const Quatd& r) const {
            return Quatd(w * r.w - x * r.x - y * r.y - z * r.z,
                         w * r.x + x * r.w + y * r.z - z * r.y,
                         w * r.y + y * r.w + z * r.x - x * r.z,
                         w * r.z + z * r.w + x * r.y - y * r.x);
        }
        Vec3d operator*(const Vec3d& v) const {
            const Vec3d q(x, y, z);
            const Vec3d uv = q.cross(v) * (2.0 * w);
            const Vec3d uuv = q.cross(q.cross(v)) * 2.0;
            return v + uv + uuv;
        }
        Quatd operator-() const { return Quatd(-w, -x, -y, -z); }

        double dot(const Quatd& r) const { return w * r.w + x * r.x + y * r.y + z * r.z; }
        double norm() const { return std::sqrt(w * w + x * x + y * y + z * z); }
        // the inverse of a unit quaternion
        Quatd inverse() const { return Quatd(w, -x, -y, -z); }
        // degenerate (zero) quaternions become the identity
        Quatd normalized() const {
            const double n = norm();
            return n > 0.0 ? Quatd(w / n, x / n, y / n, z / n) : Quatd();
        }
        Vec3d vec() const { return Vec3d(x, y, z); }
        // the same rotation with w >= 0
        Quatd positive() const { return w < 0.0 ? -*this : *this; }
    };

    // Affine transform (3x3 linear part + translation), double precision: p' = m * p + t
    struct Affine3d {
        double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};   // [row][column]
        Vec3d t;

        static Affine3d fromTRS(const Vec3d& position, const Quatd& rotation, const Vec3d& scale){
            Affine3d a;
            const Quatd& q = rotation;
            const double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
            const double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
            const double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
            const double r[3][3] = {
                {1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy)},
                {2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx)},
                {2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)}};
            const double s[3] = {scale.x, scale.y, scale.z};
            for (int i = 0; i < 3; i++){
                for (int j = 0; j < 3; j++){
                    a.m[i][j] = r[i][j] * s[j];
                }
            }
            a.t = position;
            return a;
        }

        Vec3d apply(const Vec3d& p) const {
            return Vec3d(m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + t.x,
                         m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + t.y,
                         m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + t.z);
        }
        Vec3d applyLinear(const Vec3d& v) const {
            return Vec3d(m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
                         m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                         m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z);
        }
        Affine3d operator*(const Affine3d& o) const {
            Affine3d a;
            for (int i = 0; i < 3; i++){
                for (int j = 0; j < 3; j++){
                    a.m[i][j] = m[i][0] * o.m[0][j] + m[i][1] * o.m[1][j] + m[i][2] * o.m[2][j];
                }
            }
            a.t = apply(o.t);
            return a;
        }
        // solves m * v = w for v (the linear part must be invertible)
        Vec3d solveLinear(const Vec3d& w) const {
            const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
                             - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
                             + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
            if (std::fabs(det) < 1e-300){
                return Vec3d();
            }
            const double inv = 1.0 / det;
            const Vec3d c0(m[0][0], m[1][0], m[2][0]);
            const Vec3d c1(m[0][1], m[1][1], m[2][1]);
            const Vec3d c2(m[0][2], m[1][2], m[2][2]);
            // Cramer's rule
            return Vec3d(w.dot(c1.cross(c2)) * inv, c0.dot(w.cross(c2)) * inv, c0.dot(c1.cross(w)) * inv);
        }
    };

    // ---- double-precision helpers (radians) ----

    inline double clampd(double v, double lo, double hi){ return std::min(std::max(v, lo), hi); }

    // x mod m in [0, m) (m > 0)
    inline double positiveFmod(double x, double m){
        double r = std::fmod(x, m);
        if (r < 0.0) r += m;
        if (r >= m) r -= m;
        return r;
    }
    inline float positiveFmod(float x, float m){
        return static_cast<float>(positiveFmod(static_cast<double>(x), static_cast<double>(m)));
    }

    // clamped Hermite 3x^2 - 2x^3 and Perlin 6x^5 - 15x^4 + 10x^3 of (x - edge0) / (edge1 - edge0)
    inline double smoothstep(double edge0, double edge1, double x){
        const double t = clampd((x - edge0) / (edge1 - edge0), 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    }
    inline float smoothstep(float edge0, float edge1, float x){
        return static_cast<float>(smoothstep(static_cast<double>(edge0), static_cast<double>(edge1), static_cast<double>(x)));
    }
    inline double smootherstep(double edge0, double edge1, double x){
        const double t = clampd((x - edge0) / (edge1 - edge0), 0.0, 1.0);
        return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
    }
    inline float smootherstep(float edge0, float edge1, float x){
        return static_cast<float>(smootherstep(static_cast<double>(edge0), static_cast<double>(edge1), static_cast<double>(x)));
    }

    // rotation of `radians` about `axis` (normalised here; a zero axis gives the identity)
    inline Quatd axisAngleRad(const Vec3d& axis, double radians){
        const Vec3d n = axis.normalized();
        const double s = std::sin(radians * 0.5);
        return Quatd(std::cos(radians * 0.5), n.x * s, n.y * s, n.z * s);
    }

    // rotation vector (axis * angle, radians) <-> quaternion
    inline Quatd fromRotationVector(const Vec3d& v){
        const double angle = v.length();
        if (angle < 1e-12){
            return Quatd(1.0, v.x * 0.5, v.y * 0.5, v.z * 0.5).normalized();
        }
        return axisAngleRad(v / angle, angle);
    }
    // the shortest-path rotation vector (angle in [0, pi])
    inline Vec3d toRotationVector(const Quatd& q){
        const Quatd p = q.positive();
        const double s = p.vec().length();
        if (s < 1e-12){
            return p.vec() * 2.0;
        }
        const double angle = 2.0 * std::atan2(s, p.w);
        return p.vec() * (angle / s);
    }

    // the shortest arc that turns direction `from` into direction `to` (any lengths; a zero
    // vector gives the identity, opposite directions turn 180 degrees about a perpendicular axis)
    inline Quatd fromToRotation(const Vec3d& from, const Vec3d& to){
        const Vec3d a = from.normalized();
        const Vec3d b = to.normalized();
        if (a.squaredLength() == 0.0 || b.squaredLength() == 0.0){
            return Quatd();
        }
        const double d = a.dot(b);
        if (d < -1.0 + 1e-12){
            Vec3d axis = Vec3d(1, 0, 0).cross(a);
            if (axis.squaredLength() < 1e-12){
                axis = Vec3d(0, 1, 0).cross(a);
            }
            return axisAngleRad(axis, POSE_PI);
        }
        const Vec3d c = a.cross(b);
        return Quatd(1.0 + d, c.x, c.y, c.z).normalized();
    }

    // angle between two rotations (radians, 0..pi; q and -q are the same rotation)
    inline double angleBetween(const Quatd& a, const Quatd& b){
        const Quatd d = a.inverse() * b;
        return 2.0 * std::atan2(d.vec().length(), std::fabs(d.w));
    }
    // angle between two directions (radians, 0..pi)
    inline double angleBetween(const Vec3d& a, const Vec3d& b){
        return std::atan2(a.cross(b).length(), a.dot(b));
    }

    // q^t along the shortest path (q is flipped to w >= 0 first); t may be < 0 or > 1
    inline Quatd powRotation(const Quatd& q, double t){
        return fromRotationVector(toRotationVector(q) * t);
    }

    // a * (a^-1 * b)^t along the shortest path; t may be < 0 or > 1
    inline Quatd slerpUnclamped(const Quatd& a, const Quatd& b, double t){
        return (a * powRotation(a.inverse() * b, t)).normalized();
    }

    // q = swing * twist with twist about `axis` and swing about an axis perpendicular to it
    // (twist is the identity when q is a 180-degree swing, where it is undefined)
    inline void swingTwist(const Quatd& q, const Vec3d& axis, Quatd& swing, Quatd& twist){
        const Vec3d n = axis.normalized();
        const Vec3d proj = n * q.vec().dot(n);
        const Quatd t(q.w, proj.x, proj.y, proj.z);
        if (t.norm() < 1e-12){
            twist = Quatd();
        }else{
            twist = t.normalized();
        }
        swing = q * twist.inverse();
    }

    // rotates `from` towards `to` by at most `maxRadians` (shortest path)
    inline Quatd rotateTowards(const Quatd& from, const Quatd& to, double maxRadians){
        const double angle = angleBetween(from, to);
        if (angle <= maxRadians || angle < 1e-12){
            return to;
        }
        return slerpUnclamped(from, to, std::max(0.0, maxRadians) / angle);
    }

    // ---- float overloads on the engine types (radians) ----

    inline Quaternion axisAngleRad(const Vector3& axis, float radians){
        return axisAngleRad(Vec3d(axis), static_cast<double>(radians)).toQuaternion();
    }
    inline Quaternion fromToRotation(const Vector3& from, const Vector3& to){
        return fromToRotation(Vec3d(from), Vec3d(to)).toQuaternion();
    }
    inline float angleBetween(const Quaternion& a, const Quaternion& b){
        return static_cast<float>(angleBetween(Quatd(a).normalized(), Quatd(b).normalized()));
    }
    inline float angleBetween(const Vector3& a, const Vector3& b){
        return static_cast<float>(angleBetween(Vec3d(a), Vec3d(b)));
    }
    inline Quaternion slerpUnclamped(const Quaternion& a, const Quaternion& b, float t){
        return slerpUnclamped(Quatd(a).normalized(), Quatd(b).normalized(), static_cast<double>(t)).toQuaternion();
    }
    inline Quaternion powRotation(const Quaternion& q, float t){
        return powRotation(Quatd(q).normalized(), static_cast<double>(t)).toQuaternion();
    }
    inline void swingTwist(const Quaternion& q, const Vector3& axis, Quaternion& swing, Quaternion& twist){
        Quatd s, t;
        swingTwist(Quatd(q).normalized(), Vec3d(axis), s, t);
        swing = s.toQuaternion();
        twist = t.toQuaternion();
    }
    inline Quaternion rotateTowards(const Quaternion& from, const Quaternion& to, float maxRadians){
        return rotateTowards(Quatd(from).normalized(), Quatd(to).normalized(), static_cast<double>(maxRadians)).toQuaternion();
    }

}

#endif // POSE_MATH_H
