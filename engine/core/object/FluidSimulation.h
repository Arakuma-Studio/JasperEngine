// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FLUIDSIMULATION_H
#define FLUIDSIMULATION_H

// The script and C++ face of a FluidSimulationComponent (GameObject > Effects > Fluid
// Simulation). Setters of a value the targets depend on (resolutions) recreate them at the next
// frame; reset() restarts the simulation.

#include "object/GameObject.h"
#include "component/FluidSimulationComponent.h"
#include "fluid/FluidSimulator.h"

#include <string>

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

        FluidSimulationComponent& getComponent() const;

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

        // a splat at the next step (fluid coordinates, Pavel's force, HDR colour; radius 0: the
        // component's)
        void splat(Vector2 position, Vector2 velocity, Vector3 color, float radius = 0.0f);
        void splat(const float x, const float y, const float dx, const float dy, const float r, const float g, const float b);
        // a random burst (0: Pavel's random 5-24 splats), seeded
        void randomSplats(unsigned int count = 0);
        void reset();
        // runs `steps` fixed steps now (deterministic stepping; tests and tools)
        void step(int steps = 1);

        // the preset ids of getFluidPresets(); false: no such preset
        bool applyPreset(const std::string& id);
        std::string getPreset() const;      // the matching preset, "" when none

        // the output (RGBA8); its version changes when it is recreated
        Texture& getTexture() const;
        FluidStats getStats() const;
    };

}

#endif // FLUIDSIMULATION_H
