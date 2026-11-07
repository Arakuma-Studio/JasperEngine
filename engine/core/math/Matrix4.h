// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef matrix4_h
#define matrix4_h

#include "math/Vector3.h"
#include "math/Matrix3.h"
#include "math/Vector4.h"

namespace jasperengine {

    class Quaternion;
    class AABB;
    class OBB;
    class Plane;

    class JASPERENGINE_API Matrix4{

    private:

        float matrix[4][4]; //[col][row]

    public:

        // inline and trivially copyable: Transform holds four (S07 R25)
        Matrix4(): matrix{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}} {}
        Matrix4(const Matrix4 &matrix) = default;
        Matrix4 (float fEntry00, float fEntry10, float fEntry20, float fEntry30,
                float fEntry01, float fEntry11, float fEntry21, float fEntry31,
                float fEntry02, float fEntry12, float fEntry22, float fEntry32,
                float fEntry03, float fEntry13, float fEntry23, float fEntry33);
        Matrix4(const float **matrix);

        std::string toString() const;

        Matrix4 &operator=(const Matrix4 &) = default;
        Matrix4 operator*(const Matrix4 &) const;
        Matrix4 operator*(float scalar) const;
        Matrix4 operator+(const Matrix4 &) const;
        Matrix4 operator-(const Matrix4 &) const;
        Matrix4 &operator*=(const Matrix4 &);
        Matrix4 &operator*=(float scalar);
        Vector3 operator*(const Vector3 &) const;
        Vector4 operator*(const Vector4 &) const;
        AABB operator*(const AABB &) const;
        OBB operator*(const OBB &) const;
        const float *operator[](int iCol) const;
        float *operator[](int iCol);
        bool operator==(const Matrix4 &) const;
        bool operator!=(const Matrix4 &) const;
        operator float * ();
        operator const float * () const;

        Vector4 row(const unsigned int row) const;
        Vector4 column(const unsigned int column) const;
        void set(const int col, const int row, const float val);
        float get(const int col, const int row) const;

        void setRow(const unsigned int row, const Vector4& vec);
        void setColumn(const unsigned int column, const Vector4& vec);

        bool isValid() const;

        Matrix4& identity();
        Matrix4& translateInPlace(float x, float y, float z);

        Matrix3 linear() const;

        Matrix4 inverse() const;
        Matrix4 transpose() const;
        float determinant() const;

        // Affine matrices (the last row 0 0 0 1: every Transform's) take shorter forms of the
        // general operations, each computing the general one's non-zero terms in its order, so the
        // results are the general ones' (zeros' sign aside)
        bool isAffine() const;
        // the upper-left 3x3's determinant: the sign of determinant() for an affine matrix
        float determinant3x3() const;
        // *this * rhs, both affine
        Matrix4 affineMultiply(const Matrix4& rhs) const;
        // *this * v of an affine matrix (no divide by w, which is 1)
        Vector3 transformAffine(const Vector3& v) const;
        // inverse() of an affine matrix
        Matrix4 affineInverse() const;
        // translateMatrix(t) * r.getRotationMatrix() * scaleMatrix(s) without the products
        static Matrix4 composeTRS(const Vector3& t, const Quaternion& r, const Vector3& s);

        static Matrix4 translateMatrix(const float x, const float y, const float z);
        static Matrix4 translateMatrix(const Vector3& position);

        static Matrix4 rotateMatrix(const float angle, const Vector3 &axis);
        static Matrix4 rotateMatrix(const float azimuth, const float elevation);
        static Matrix4 rotateXMatrix(const float angle);
        static Matrix4 rotateYMatrix(const float angle);
        static Matrix4 rotateZMatrix(const float angle);

        static Matrix4 scaleMatrix(const float sf);
        static Matrix4 scaleMatrix(const Vector3& sf);

        static Matrix4 lookAtMatrix(Vector3 eye, Vector3 center, Vector3 up);
        static Matrix4 reflectMatrix(const Plane& plane);

        // Lengyel oblique near-plane: returns this projection with its near plane
        // moved onto clipPlane (given in view space). Only the depth (3rd) row
        // changes, so clip x/y/w are preserved. Perspective or orthographic. Used for
        // planar-reflection clipping.
        Matrix4 obliqueNearClip(const Vector4& clipPlane) const;
        static Matrix4 frustumMatrix(float left, float right, float bottom, float top, float near, float far);
        static Matrix4 orthoMatrix(float l, float r, float b, float t, float n, float f);
        static Matrix4 perspectiveMatrix(float yfov, float aspect, float near, float far);

        void decomposeStandard(Vector3& position, Vector3& scale, Quaternion& rotation) const;
        void decomposeQDU(Vector3& position, Vector3& scale, Quaternion& rotation) const;
        void decompose(Vector3& position, Vector3& scale, Quaternion& rotation) const;
    };
    
}

#endif
