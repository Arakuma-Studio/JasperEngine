// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLES_PARTICLE_UPGRADE_H
#define PARTICLES_PARTICLE_UPGRADE_H

// "Upgrade to modules" (particles spec 5.1, 6.7): a version 1 Particle System (the legacy
// initializers and modifiers) as version 2 modules, with a report of how exact each value
// is. Pure (the Inspector, MCP create_particles {upgrade}, Lua ps:upgrade() apply it with
// their own undo); gtested (ParticleUpgradeTest).

#include "Export.h"
#include "component/ParticleSystemComponent.h"

#include <string>
#include <vector>

namespace jasperengine {
namespace particles {

    enum class UpgradeStatus { Exact, Approximated, Dropped };

    struct JASPERENGINE_API UpgradeItem {
        std::string field;     // the legacy field ("lifeInitializer", "sizeModifier", ...)
        UpgradeStatus status = UpgradeStatus::Exact;
        float maxError = 0.0f; // approximated: the largest difference found (the value's units)
        std::string message;
    };

    struct JASPERENGINE_API UpgradeReport {
        std::vector<UpgradeItem> items;
        // false when something has no module equivalent (a position modifier) and force was off:
        // the component is then not upgraded
        bool upgraded = true;

        bool hasStatus(UpgradeStatus status) const;
        const UpgradeItem* find(const std::string& field) const;
    };

    struct JASPERENGINE_API UpgradeResult {
        ParticleSystemComponent component;
        UpgradeReport report;
    };

    // what a version 1 system drew on: Points, a plain instanced mesh (its own mesh at each
    // particle, rotated in 3D), or an instanced mesh turned to the camera (instancedBillboard)
    // or around Y only (instancedCylindricalBillboard)
    enum class LegacyTarget { Points, Mesh, Billboard, VerticalBillboard };

    // `target`: what the system draws on (Points: its size initializer is the particle size;
    // an instanced mesh: its scale initializer is); `force`: upgrade even when a value would be
    // dropped (reported)
    JASPERENGINE_API UpgradeResult upgradeLegacy(const ParticleSystemComponent& legacy, LegacyTarget target, bool force = false);
    // the same with only Points or not (a mesh target is taken as a camera-facing quad)
    JASPERENGINE_API UpgradeResult upgradeLegacy(const ParticleSystemComponent& legacy, bool targetIsPoints, bool force = false);

    JASPERENGINE_API const char* upgradeStatusName(UpgradeStatus status);

}
}

#endif //PARTICLES_PARTICLE_UPGRADE_H
