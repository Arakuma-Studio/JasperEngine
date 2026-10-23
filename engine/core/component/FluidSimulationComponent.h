// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FLUID_SIMULATION_COMPONENT_H
#define FLUID_SIMULATION_COMPONENT_H

// A GPU fluid on a GameObject: Pavel Dobryakov's WebGL-Fluid-Simulation (MIT) as an engine
// feature. A stable-fluids solve (curl, vorticity confinement, divergence, Jacobi pressure,
// gradient subtract, semi-Lagrangian advection) on a velocity grid of `simResolution` cells on
// its short side, advecting a dye image of `dyeResolution` texels on its short side, drawn with
// his look (dye-gradient shading, soft-knee HDR bloom, sunrays) into an output texture.
// FluidSystem owns the GPU side (FluidSimulator); defaults are his. Positions are normalised
// fluid coordinates: (0,0) bottom-left, (1,1) top-right of the output.

#include "Export.h"
#include "math/Vector2.h"
#include "math/Vector3.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace jasperengine{

    class FluidSimulator;

    // where the output goes
    enum class FluidOutput{
        // drawn over the main camera's image (after post-processing), the size of the view;
        // `interactive` takes the mouse and touches over the whole view
        SCREEN,
        // the entity's own Sprite, Image or Mesh (submesh 0's base colour) texture; `interactive`
        // takes the pointer over a Sprite or an Image (a Mesh takes splats from scripts)
        MATERIAL,
        // only the texture (`fluid|<entity>` texture links, FluidSimulation::getTexture())
        TEXTURE
    };

    // one splat: velocity is Pavel's force (a pointer delta in fluid coordinates x splatForce),
    // colour is added to the dye (HDR, his pointer colours are ~0.15, random bursts ~1.5)
    struct JASPERENGINE_API FluidSplat{
        Vector2 position = Vector2(0.5f, 0.5f);
        Vector2 velocity = Vector2(0.0f, 0.0f);
        Vector3 color = Vector3(0.15f, 0.15f, 0.15f);
        float radius = 0.0f;        // splatRadius units (0: the component's splatRadius)
    };

    struct JASPERENGINE_API FluidSimulationComponent{
        bool enabled = true;

        FluidOutput output = FluidOutput::MATERIAL;
        bool interactive = true;        // pointer drags splat (Pavel's mouse and touches)
        // width / height of the fluid; 0: automatic (the view in SCREEN, the Sprite or Image size
        // in MATERIAL, else 1)
        float aspectRatio = 0.0f;

        // simulation
        unsigned int simResolution = 128;   // velocity grid, short side (32, 64, 128, 256)
        unsigned int dyeResolution = 1024;  // dye image, short side (128, 256, 512, 1024)
        float densityDissipation = 1.0f;    // dye fade, value / (1 + k dt)
        float velocityDissipation = 0.2f;
        float pressure = 0.8f;              // the previous pressure kept as the Jacobi warm start
        unsigned int pressureIterations = 20;
        float curl = 30.0f;                 // vorticity confinement
        // the gradient subtracted from the velocity x this: 1 is Pavel's (his gradient has no
        // 0.5, it projects twice the central difference), 0.5 the textbook projection
        float gradientScale = 1.0f;
        float simulationRate = 60.0f;       // fixed steps per second
        unsigned int maxSubsteps = 4;       // steps a frame at most (the rest of the time dropped)
        float timeScale = 1.0f;
        bool paused = false;                // no steps; splats still paint (Pavel's pause)

        // splats
        float splatRadius = 0.25f;          // Pavel's SPLAT_RADIUS (used / 100)
        float splatForce = 6000.0f;
        bool colorful = true;               // pointer colours cycle
        float colorUpdateSpeed = 10.0f;
        bool splatsOnStart = true;          // a random burst when the simulation (re)starts
        unsigned int randomSplatCount = 0;  // a burst's splats (0: random 5-24, Pavel's)
        uint32_t seed = 0;                  // every random choice (0: a new seed each start)

        // look
        bool shading = true;
        Vector3 backColor = Vector3(0.0f, 0.0f, 0.0f);     // display space, like the dye
        bool transparent = false;           // no background: alpha = max(r, g, b)
        bool bloom = true;
        unsigned int bloomIterations = 8;
        unsigned int bloomResolution = 256;
        float bloomIntensity = 0.8f;
        float bloomThreshold = 0.6f;
        float bloomSoftKnee = 0.7f;
        bool sunrays = true;
        unsigned int sunraysResolution = 196;
        float sunraysWeight = 1.0f;

        // runtime only, not serialized
        std::shared_ptr<FluidSimulator> simulator;
        std::vector<FluidSplat> pendingSplats;  // applied at the next step (FluidSimulation::splat)
        std::vector<unsigned int> pendingBursts; // random bursts asked for: each one's splat count (0: Pavel's 5-24)
        unsigned int pendingSteps = 0;          // FluidSimulation::step(): fixed steps to run at the next frame (also paused)
        bool needReset = true;                  // restart (seed, sizes) at the next update
        unsigned int revision = 0;              // bumped by FluidSystem when the output is recreated
    };

}

#endif // FLUID_SIMULATION_COMPONENT_H
