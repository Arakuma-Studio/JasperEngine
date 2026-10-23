// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// CONTRACT STUB: WP-EDITOR-SCENE replaces every body (spec §9.3).

#include "LatticeEditorOps.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace{
    const char* stubError = "lattice editing is not implemented yet";
    bool fail(std::string* error){ if (error) *error = stubError; return false; }
}

const std::vector<int>& LatticeEditorOps::getSelectedPoints(uint32_t, Entity){ static const std::vector<int> none; return none; }
void LatticeEditorOps::setSelectedPoints(uint32_t, Entity, const std::vector<int>&){}
void LatticeEditorOps::clearSelectedPoints(uint32_t){}
int LatticeEditorOps::getLastSelectedPoint(uint32_t, Entity){ return -1; }
LatticeFalloff& LatticeEditorOps::falloff(){ static LatticeFalloff f; return f; }
bool LatticeEditorOps::setOffsets(Project*, uint32_t, Entity, const std::vector<int>&, const std::vector<Vector3>&, std::string* e){ return fail(e); }
bool LatticeEditorOps::transformPoints(Project*, uint32_t, Entity, const std::vector<int>&, const Matrix4&, const Vector3&, const LatticeFalloff&, std::string* e){ return fail(e); }
bool LatticeEditorOps::resetPoints(Project*, uint32_t, Entity, const std::vector<int>&, std::string* e){ return fail(e); }
bool LatticeEditorOps::applyPreset(Project*, uint32_t, Entity, const lattice::PresetParams&, const std::vector<int>&, std::string* e){ return fail(e); }
bool LatticeEditorOps::mirror(Project*, uint32_t, Entity, lattice::LatticeAxis, bool, std::string* e){ return fail(e); }
bool LatticeEditorOps::setResolution(Project*, uint32_t, Entity, int, int, int, std::string* e){ return fail(e); }
bool LatticeEditorOps::computeFit(Scene*, const std::vector<Entity>&, float, LatticeFit&, std::string* e){ return fail(e); }
bool LatticeEditorOps::fitTo(Project*, uint32_t, Entity, const std::vector<Entity>&, float, std::string* e){ return fail(e); }
bool LatticeEditorOps::bind(Project*, uint32_t, Entity, Entity, const LatticeBinding&, bool, std::string* e){ return fail(e); }
bool LatticeEditorOps::unbind(Project*, uint32_t, Entity, Entity, std::string* e){ return fail(e); }
std::vector<Entity> LatticeEditorOps::findTargets(Scene*, Entity){ return {}; }
bool LatticeEditorOps::savePose(Project*, uint32_t, Entity, const std::string&, std::string* e){ return fail(e); }
bool LatticeEditorOps::applyPose(Project*, uint32_t, Entity, const std::string&, float, std::string* e){ return fail(e); }
bool LatticeEditorOps::renamePose(Project*, uint32_t, Entity, const std::string&, const std::string&, std::string* e){ return fail(e); }
bool LatticeEditorOps::deletePose(Project*, uint32_t, Entity, const std::string&, std::string* e){ return fail(e); }
Entity LatticeEditorOps::createLattice(Project*, uint32_t, const std::vector<Entity>&, const Vector3&, const std::string&, std::string* e){ fail(e); return NULL_ENTITY; }
