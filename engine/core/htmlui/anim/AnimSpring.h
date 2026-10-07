// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANIMSPRING_H
#define ANIMSPRING_H

// Spring easing, ported from Anime.js 4.5.0 (easings/spring/index.js, after WebKit's
// spring.js): either physical parameters (mass, stiffness, damping, velocity) or a perceived
// duration and a bounce (SwiftUI). Used as an animation's ease, it also sets the animation's
// duration to the settling duration.

#include "Export.h"
#include "AnimEasing.h"

#include <functional>
#include <memory>
#include <optional>

namespace jasperengine {

    class AnimTimer;

    struct AnimSpringParams {
        std::optional<double> mass, stiffness, damping, velocity;   // defaults 1, 100, 10, 0
        std::optional<double> bounce, duration;                     // defaults .5, 628 ms; either one set: they win
        std::function<void(AnimTimer*)> onComplete;                  // once the perceived duration is reached
    };

    class JASPERENGINE_API AnimSpring : public std::enable_shared_from_this<AnimSpring> {
    public:
        explicit AnimSpring(const AnimSpringParams& params = AnimSpringParams());

        // the spring position at solver time (seconds)
        double solve(double time) const;
        // the easing: t in 0..1 of the settling duration; calls onComplete at the perceived duration
        double ease(double t);
        // an EaseFn calling ease() (keeps the spring alive)
        EaseFn getEaseFunction();

        double getSettlingDuration() const { return settlingDuration; }   // ms
        double getSolverDuration() const { return solverDuration; }       // seconds

        double getBounce() const { return bn; }
        void setBounce(double bounce);
        double getDuration() const { return pd; }                          // perceived, ms
        void setDuration(double duration);
        double getStiffness() const { return s; }
        void setStiffness(double stiffness);
        double getDamping() const { return d; }
        void setDamping(double damping);
        double getMass() const { return m; }
        void setMass(double mass);
        double getVelocity() const { return v; }
        void setVelocity(double velocity);

        // the animation onComplete receives (set when the spring eases an animation)
        void setParent(AnimTimer* parent){ this->parent = parent; }
        void setOnComplete(std::function<void(AnimTimer*)> callback){ onComplete = std::move(callback); }

    private:
        void calculateSDFromBD();
        void calculateBDFromSD();
        void compute();

        double timeStep = .02;
        double restThreshold = .0005;
        double restDuration = 200;
        double maxDuration = 60000;
        double maxRestSteps = 0;
        double maxIterations = 0;
        double bn = .5, pd = 628, m = 1, s = 100, d = 10, v = 0;
        double w0 = 0, zeta = 0, wd = 0, b = 0;
        bool completed = false;
        double solverDuration = 0;
        double settlingDuration = 0;
        AnimTimer* parent = nullptr;
        std::function<void(AnimTimer*)> onComplete;
    };

}

#endif //ANIMSPRING_H
