// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// CONTRACT STUB: WP-SOLVER replaces every body (spec §4).

#include "LatticePresets.h"

using namespace jasperengine;

bool lattice::presetFromName(const std::string&, LatticePreset&){ return false; }
const char* lattice::presetName(LatticePreset){ return ""; }
std::vector<std::string> lattice::presetNames(){ return {}; }
void lattice::applyPreset(int, int, int, std::vector<Vector3>&, const PresetParams&, const std::vector<int>&){}
std::vector<Vector3> lattice::resample(int, int, int, const std::vector<Vector3>& offsets, int, int, int){ return offsets; }
void lattice::mirror(int, int, int, std::vector<Vector3>&, LatticeAxis, bool){}
std::vector<int> lattice::selectAll(int, int, int){ return {}; }
std::vector<int> lattice::selectFace(int, int, int, LatticeAxis, bool){ return {}; }
std::vector<int> lattice::selectExterior(int, int, int){ return {}; }
std::vector<int> lattice::selectInterior(int, int, int){ return {}; }
std::vector<int> lattice::selectRange(int, int, int, int, int, int, int, int, int){ return {}; }
std::vector<int> lattice::expandSelection(int, int, int, const std::vector<int>& s){ return s; }
std::vector<int> lattice::shrinkSelection(int, int, int, const std::vector<int>& s){ return s; }
std::vector<int> lattice::invertSelection(int, int, int, const std::vector<int>& s){ return s; }
std::vector<int> lattice::selectInWorldBox(int, int, int, const std::vector<Vector3>&, const Matrix4&, const Vector3&, const Vector3&){ return {}; }
std::vector<int> lattice::selectInWorldSphere(int, int, int, const std::vector<Vector3>&, const Matrix4&, const Vector3&, float){ return {}; }
std::string lattice::copyAsLua(const std::string&, int, int, int, const std::vector<Vector3>&, const std::vector<int>&){ return ""; }
