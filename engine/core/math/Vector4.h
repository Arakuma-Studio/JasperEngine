// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef vector4_h
#define vector4_h

#include "Export.h"
#include <algorithm>
#include <string>
#include <assert.h>
#include <math.h>

namespace jasperengine { class Vector4; }
jasperengine::Vector4 operator * ( const float fScalar, const jasperengine::Vector4& rkVector );
jasperengine::Vector4 operator / ( const float fScalar, const jasperengine::Vector4& rkVector );
jasperengine::Vector4 operator + (const jasperengine::Vector4& lhs, const float rhs);
jasperengine::Vector4 operator + (const float lhs, const jasperengine::Vector4& rhs);
jasperengine::Vector4 operator - (const jasperengine::Vector4& lhs, float rhs);
jasperengine::Vector4 operator - (const float lhs, const jasperengine::Vector4& rhs);


namespace jasperengine {

    class Vector2;
    class Vector3;

    class JASPERENGINE_API Vector4
    {
    public:
        float x, y, z, w;
        
        static const Vector4 ZERO;
        static const Vector4 UNIT_X;
        static const Vector4 UNIT_Y;
        static const Vector4 UNIT_Z;
        static const Vector4 UNIT_W;
        static const Vector4 UNIT_SCALE;

        Vector4();
        Vector4( const float fX, const float fY, const float fZ, const float fW );
        Vector4( const Vector4& rhs );
        explicit Vector4( const float afCoordinate[4] );
        explicit Vector4( const int afCoordinate[4] );
        explicit Vector4( float* const r );
        explicit Vector4( const float scaler );
        explicit Vector4( const Vector2& rhs, const float fZ, const float fW );
        explicit Vector4( const Vector3& rhs, const float fW );

        std::string toString() const;

        float operator [] ( const size_t i ) const;
        float& operator [] ( const size_t i );
        Vector4& operator = ( const Vector4& rkVector );
        Vector4& operator = ( const float fScalar);
        bool operator == ( const Vector4& rkVector ) const;
        bool operator != ( const Vector4& rkVector ) const;
        Vector4& operator = (const Vector3& rhs);

        // arithmetic operations
        Vector4 operator + ( const Vector4& rkVector ) const;
        Vector4 operator - ( const Vector4& rkVector ) const;
        Vector4 operator * ( const float fScalar ) const;
        Vector4 operator * ( const Vector4& rhs) const;
        Vector4 operator / ( const float fScalar ) const;
        Vector4 operator / ( const Vector4& rhs) const;
        const Vector4& operator + () const;
        Vector4 operator - () const;

        friend Vector4 (::operator *) ( const float fScalar, const Vector4& rkVector );
        friend Vector4 (::operator /) ( const float fScalar, const Vector4& rkVector );
        friend Vector4 (::operator +) (const Vector4& lhs, const float rhs);
        friend Vector4 (::operator +) (const float lhs, const Vector4& rhs);
        friend Vector4 (::operator -) (const Vector4& lhs, float rhs);
        friend Vector4 (::operator -) (const float lhs, const Vector4& rhs);

        Vector4& operator += ( const Vector4& rkVector );
        Vector4& operator -= ( const Vector4& rkVector );
        Vector4& operator *= ( const float fScalar );
        Vector4& operator += ( const float fScalar );
        Vector4& operator -= ( const float fScalar );
        Vector4& operator *= ( const Vector4& rkVector );
        Vector4& operator /= ( const float fScalar );
        Vector4& operator /= ( const Vector4& rkVector );

        bool operator < ( const Vector4& v ) const;
        bool operator > ( const Vector4& v ) const;

        bool isValid() const;

        void swap(Vector4& other);

        void divideByW();

        float dotProduct(const Vector4& vec) const;

        // Move toward target by at most maxDistanceDelta, never overshooting it (a negative
        // delta moves away). Frame-rate independent stepping: pass speed * deltaTime.
        Vector4 moveTowards( const Vector4& target, float maxDistanceDelta ) const;
        // Unity's Lerp: from this vector toward target, t clamped to [0, 1]
        Vector4 lerp( const Vector4& target, float t ) const;
        // Unity's LerpUnclamped: t outside [0, 1] extrapolates
        Vector4 lerpUnclamped( const Vector4& target, float t ) const;

        bool isNaN() const;

    };
    
}


#endif
