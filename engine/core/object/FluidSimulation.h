// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FLUIDSIMULATION_H
#define FLUIDSIMULATION_H

// The script and C++ face of a FluidSimulationComponent (GameObject > Effects > Fluid
// Simulation). Setters of a value the targets depend on (resolutions) recreate them at the next
// frame; reset() restarts the simulation. setSeed() takes effect at the next (re)start.
//
// Validation: setters reject (Log::error, the old value kept) NaN/infinite floats and negative
// values; resolutions, simulationRate, maxSubsteps and splatRadius must also be > 0. Other values
// are kept as given: resolutions outside Pavel's sets (sim 32/64/128/256, dye 128/256/512/1024)
// work, the simulator sizes its targets from them (short side, the long side from the aspect).
// splat() rejects non-finite positions/velocities and negative or non-finite colours/radii.
// Fields set directly on the component (Inspector, MCP, Lua component access) are not checked.

#include "object/GameObject.h"
#include "component/FluidSimulationComponent.h"
#include "fluid/FluidSimulator.h"

#include <string>
#include <vector>

namespace jasperengine{

    class JASPERENGINE_API FluidSimulation: public GameObject{
    public:
        FluidSimulation(Scene* scene);
        FluidSimulation(Scene* scene, Entity entity);
        virtual ~FluidSimulation();

        FluidSimulation(const FluidSimulation& rhs);
        FluidSimulation& operator=(const FluidSimulation& rhs);
        FluidSimulation(FluidSimulation&& rhs) noexcept;
        FluidSimulation& operator=(FluidSimulation&& rhs) noexcept;

        using GameObject::getComponent;
        FluidSimulationComponent& getComponent() const;

        // the component's `enabled`: a disabled fluid neither steps nor draws (its state is kept)
        void setEnabled(bool enabled);
        bool isEnabled() const;

        void setOutput(FluidOutput output);
        FluidOutput getOutput() const;
        void setInteractive(bool interactive);
        bool isInteractive() const;
        void setAspectRatio(float aspectRatio);
        float getAspectRatio() const;

        void setSimResolution(unsigned int simResolution);
        unsigned int getSimResolution() const;
        void setDyeResolution(unsigned int dyeResolution);
        unsigned int getDyeResolution() const;
        void setDensityDissipation(float value);
        float getDensityDissipation() const;
        void setVelocityDissipation(float value);
        float getVelocityDissipation() const;
        void setPressure(float pressure);
        float getPressure() const;
        void setPressureIterations(unsigned int iterations);
        unsigned int getPressureIterations() const;
        void setCurl(float curl);
        float getCurl() const;
        void setGradientScale(float scale);
        float getGradientScale() const;
        void setSimulationRate(float hz);
        float getSimulationRate() const;
        void setMaxSubsteps(unsigned int substeps);
        unsigned int getMaxSubsteps() const;
        void setTimeScale(float timeScale);
        float getTimeScale() const;
        void setPaused(bool paused);
        bool isPaused() const;

        void setSplatRadius(float radius);
        float getSplatRadius() const;
        void setSplatForce(float force);
        float getSplatForce() const;
        void setColorful(bool colorful);
        bool isColorful() const;
        void setColorUpdateSpeed(float speed);
        float getColorUpdateSpeed() const;
        void setSplatsOnStart(bool splatsOnStart);
        bool isSplatsOnStart() const;
        void setRandomSplatCount(unsigned int count);
        unsigned int getRandomSplatCount() const;
        void setSeed(uint32_t seed);
        uint32_t getSeed() const;

        void setShading(bool shading);
        bool isShading() const;
        void setBackColor(Vector3 color);
        void setBackColor(const float r, const float g, const float b);
        Vector3 getBackColor() const;
        void setTransparent(bool transparent);
        bool isTransparent() const;
        void setBloom(bool bloom);
        bool isBloom() const;
        void setBloomIterations(unsigned int iterations);
        unsigned int getBloomIterations() const;
        void setBloomResolution(unsigned int resolution);
        unsigned int getBloomResolution() const;
        void setBloomIntensity(float intensity);
        float getBloomIntensity() const;
        void setBloomThreshold(float threshold);
        float getBloomThreshold() const;
        void setBloomSoftKnee(float knee);
        float getBloomSoftKnee() const;
        void setSunrays(bool sunrays);
        bool isSunrays() const;
        void setSunraysResolution(unsigned int resolution);
        unsigned int getSunraysResolution() const;
        void setSunraysWeight(float weight);
        float getSunraysWeight() const;

        // a splat at the next step, also while paused (fluid coordinates, Pavel's force, HDR
        // colour; radius 0: the component's splatRadius). A fluid that does not run (disabled,
        // inactive) keeps the latest FLUID_MAX_PENDING_SPLATS (1024), the oldest dropped
        void splat(Vector2 position, Vector2 velocity, Vector3 color, float radius = 0.0f);
        void splat(const float x, const float y, const float dx, const float dy, const float r, const float g, const float b);
        // a random burst at the next step (0: Pavel's random 5-24 splats; at most
        // FLUID_MAX_BURST_SPLATS, 1000), from the seeded source; the latest
        // FLUID_MAX_PENDING_BURSTS (64) are kept while the fluid does not run
        void randomSplats(unsigned int count = 0);
        // restarts at the next frame: zero fields, the seed, the start burst (splatsOnStart)
        void reset();
        // queues `steps` fixed steps run at the next frame, also while paused
        // (deterministic stepping; tests and tools); steps <= 0 do nothing
        void step(int steps = 1);

        // the preset ids of getFluidPresets(); false: no such preset
        bool applyPreset(const std::string& id);
        std::string getPreset() const;      // the matching preset, "" when none
        static std::vector<std::string> getPresetIds();    // getFluidPresets()' ids, in order

        // the output (RGBA8); its version changes when it is recreated
        Texture& getTexture() const;
        FluidStats getStats() const;
    };

}

#endif // FLUIDSIMULATION_H
