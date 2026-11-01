// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The wire shape of a version 2 Particle System's Shape module (particles spec 6.8): spheres
// and hemispheres as three circles, the cone's base and far circles (at `length`) with four
// sides, the box's twelve edges, circle, edge, rectangle and donut outlines; in the system's
// local space (the shape's position, rotation and scale applied, as the simulation places
// them). Drawn by the Scene view on the selected system (SceneRender::updateParticleShapeLines).

#include "math/Vector3.h"
#include "particles/ParticleModules.h"

#include <utility>
#include <vector>

namespace jasperengine::editor {

    std::vector<std::pair<Vector3, Vector3>> particleShapeGizmoSegments(const ParticleShapeModule& shape);

}
