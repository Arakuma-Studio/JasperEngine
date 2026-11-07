// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef WATER_WAKE_COMPONENT_H
#define WATER_WAKE_COMPONENT_H

namespace jasperengine{

    // Foam and waves left on the water (shaders with @require waterwake: library:water) while the
    // GameObject moves through its surface: churn behind the stern, spray along the hull, the
    // diverging and transverse waves of a Kelvin wake. They live in the scene's wake map
    // (WaterInteractionSystem) and fade over the scene's Wake Fade; only a running scene makes them.
    struct WaterWakeComponent{
        bool enabled = true;

        float foam = 1.0f;      // 0..2: stern churn, hull spray, breaking bow waves
        float waves = 1.0f;     // 0..2: the diverging and transverse waves
        float width = 0.0f;     // local X extent at the waterline (metres); 0: the Water Excluder's or the mesh's
        float length = 0.0f;    // local Z extent (metres); 0: as width
    };

}

#endif //WATER_WAKE_COMPONENT_H
