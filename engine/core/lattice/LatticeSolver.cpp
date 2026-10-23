// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// CONTRACT STUB: WP-SOLVER replaces every body (spec §3).

#include "LatticeSolver.h"

using namespace jasperengine;

int lattice::pointCount(int rx, int ry, int rz){ return rx * ry * rz; }
int lattice::pointIndex(int rx, int ry, int i, int j, int k){ return i + rx * (j + ry * k); }
Vector3 lattice::restPosition(int, int, int, int, int, int){ return Vector3(0, 0, 0); }
void lattice::buildPaddedGrid(int, int, int, const std::vector<Vector3>&, const std::vector<Vector3>*, std::vector<Vector3>& padded){ padded.clear(); }
void lattice::buildPaddedGrid(const LatticeComponent&, std::vector<Vector3>& padded){ padded.clear(); }
lattice::Evaluation lattice::evaluate(const LatticeComponent&, const std::vector<Vector3>&, const Vector3&, float){ return {Vector3(0, 0, 0), Matrix3(), 1.0f}; }
Vector3 lattice::deformWorldPoint(const LatticeComponent&, const std::vector<Vector3>&, const Matrix4&, const Vector3& worldPoint, float){ return worldPoint; }
Matrix3 lattice::worldJacobian(const LatticeComponent&, const std::vector<Vector3>&, const Matrix4&, const Vector3&, float){ return Matrix3(); }
Matrix4 lattice::objectToNormalized(const Matrix4&, const Matrix4&){ return Matrix4(); }
void lattice::deformVertex(const ObjectSpaceLattice&, Vector3&, Vector3*, Vector3*, float*, const Vector4*){}
float lattice::maxPaddedOffset(const std::vector<Vector3>&){ return 0.0f; }
std::vector<std::array<int, 3>> lattice::findFoldedCells(const LatticeComponent&){ return {}; }
