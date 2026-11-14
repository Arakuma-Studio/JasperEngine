// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef vector3_h
#define vector3_h

#include "Export.h"
#include <math.h>
#include <string>

namespace jasperengine { class Vector3; }
JASPERENGINE_API jasperengine::Vector3 operator * ( float f, const jasperengine::Vector3& v );


namespace jasperengine {

    class Vector2;
    class Vector4;

    class JASPERENGINE_API Vector3
    {
    public:
        float x, y, z;

        static const Vector3 ZERO;
        static const Vector3 UNIT_X;
        static const Vector3 UNIT_Y;
        static const Vector3 UNIT_Z;
        static const Vector3 UNIT_SCALE;

        // inline and trivially copyable: Transform (and the component arrays holding it)
        // construct, move and copy with plain stores (S07 R25)
        Vector3(): x(0), y(0), z(0) {}
        Vector3( const float nx, const float ny, const float nz ): x(nx), y(ny), z(nz) {}
        Vector3( const Vector3& v ) = default;
        explicit Vector3( const float v[3] );
        explicit Vector3( const int v[3] );
        explicit Vector3( float* const v );
        explicit Vector3( const float scaler );
        explicit Vector3( const Vector2& vec2, const float nz );
        explicit Vector3( const Vector4& vec4 );

        std::string toString() const;

        // the arithmetic is inline: out of line, every operator of a trivially copyable Vector3
        // is a call returning it in two SSE registers (CubeLutBench.Bake32 10.1 ms instead of 7.0)
        float operator [] ( unsigned i ) const { return *(&x+i); }
        float& operator [] ( unsigned i ) { return *(&x+i); }
        Vector3& operator = ( const Vector3& v ) = default;
        bool operator == ( const Vector3& v ) const { return x == v.x && y == v.y && z == v.z; }
        bool operator != ( const Vector3& v ) const { return x != v.x || y != v.y || z != v.z; }

        Vector3 operator + ( const Vector3& v ) const { return Vector3(x + v.x, y + v.y, z + v.z); }
        Vector3 operator - ( const Vector3& v ) const { return Vector3(x - v.x, y - v.y, z - v.z); }
        Vector3 operator * ( float f ) const { return Vector3(x * f, y * f, z * f); }
        Vector3 operator * ( const Vector3& v) const { return Vector3(x * v.x, y * v.y, z * v.z); }
        Vector3 operator / ( float f ) const { const float r = 1.0f / f; return Vector3(x * r, y * r, z * r); }
        Vector3 operator - () const { return Vector3(-x, -y, -z); }
        friend Vector3 (::operator *) ( float f, const Vector3& v );

        Vector3& operator += ( const Vector3& v ) { x += v.x; y += v.y; z += v.z; return *this; }
        Vector3& operator -= ( const Vector3& v ) { x -= v.x; y -= v.y; z -= v.z; return *this; }
        Vector3& operator *= ( float f ) { x *= f; y *= f; z *= f; return *this; }
        Vector3& operator /= ( float f ) { const float r = 1.0f / f; x *= r; y *= r; z *= r; return *this; }

        bool operator < ( const Vector3& v ) const;
        bool operator > ( const Vector3& v ) const;

        bool isValid() const;

        float length () const;
        float squaredLength () const { return x * x + y * y + z * z; }
        float dotProduct(const Vector3& v) const { return x * v.x + y * v.y + z * v.z; }
        float absDotProduct(const Vector3& v) const;
        float distance(const Vector3& rhs) const;
        float squaredDistance(const Vector3& rhs) const;

        Vector3& normalize();
        Vector3 normalized() const;
        float normalizeL();

        Vector3 crossProduct( const Vector3& v ) const;
        Vector3 midPoint( const Vector3& v ) const;

        // Move toward target by at most maxDistanceDelta, never overshooting it.
        // Frame-rate independent stepping: pass speed * deltaTime as maxDistanceDelta.
        Vector3 moveTowards( const Vector3& target, float maxDistanceDelta ) const;

        // Unity's Lerp: from this vector toward target, t clamped to [0, 1]
        Vector3 lerp( const Vector3& target, float t ) const;
        // Unity's LerpUnclamped: t outside [0, 1] extrapolates
        Vector3 lerpUnclamped( const Vector3& target, float t ) const;

        void makeFloor( const Vector3& v );
        void makeCeil( const Vector3& v );

        Vector3 perpendicular() const;

        Vector3 reflect(const Vector3& normal) const;

    };
    
}

#endif
