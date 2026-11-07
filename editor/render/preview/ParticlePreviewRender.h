// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Scene.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/ParticleSystem.h"
#include "object/environment/Skybox.h"
#include "texture/Texture.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor{

    // The Inspector's preview of a .particles asset (docs/specs/S20-particles.md R28): the effect
    // playing in a scene of its own, looped (a one-shot restarts when its last particle dies),
    // framed by its particles' bounds. Each update queues one more frame of it.
    //
    // The stepped mode is the Particle Effect window's Preview Stage (docs/specs/S91-effect-
    // authoring.md D3, R17, R20, R21): several systems under one root (a GameObject subtree, or a
    // file), their clock driven by the caller instead of the frame. The scene's particle
    // subsystem stays paused and every tick goes through the subsystem's preview path: seek()
    // re-simulates to a time over frames (checkpoints, at most budgetMs a frame) with the seed
    // pinned, advance() plays on, step() is one tick. The moving emitter circles the root as a
    // function of the stage time, so a seek with it on replays the motion tick by tick instead
    // of from a checkpoint. Main thread.
    class ParticlePreviewRender{
    public:
        // the stage's background: the scene's sky is the selected scene's first Skybox
        enum class Background { Dark, Light, Checker, SceneSky, Black };

        // a system of the stage: the effect (a key the effect library holds) at a place under the root
        struct StageSystem{
            std::string name;
            std::string effect;
            Vector3 position = Vector3(0.0f, 0.0f, 0.0f);
            Quaternion rotation;
            Vector3 scale = Vector3(1.0f, 1.0f, 1.0f);
            bool operator==(const StageSystem& other) const;
        };

        struct StageStats{
            uint32_t live = 0;
            uint32_t capacity = 0;
            double simulateMs = 0.0;
            double renderMs = 0.0;
            uint64_t stateHash = 0;
        };

    private:
        Scene* scene = nullptr;
        Camera* camera = nullptr;
        Light* light = nullptr;
        std::unique_ptr<ParticleSystem> emitter;
        std::string path;
        // the framing eases to the bounds the particles reach
        Vector3 center = Vector3(0.0f, 1.0f, 0.0f);
        float radius = 3.0f;

        // ---- the stage
        bool stepped = false;
        std::unique_ptr<GameObject> root;
        std::vector<StageSystem> stageSystems;
        std::vector<std::unique_ptr<ParticleSystem>> systems;
        std::unique_ptr<Skybox> sky;
        unsigned int width = 0;
        unsigned int height = 0;
        double time = 0.0;
        uint32_t seed = 1;
        bool moving = false;
        // a seek: through the preview's checkpoints, or (moving emitter) tick by tick from 0
        bool seeking = false;
        bool manualSeek = false;
        double seekTarget = 0.0;
        double manualTime = 0.0;
        float yaw = 0.0f;
        float pitch = 0.0f;
        float zoomFactor = 1.0f;
        Background background = Background::Dark;
        Vector4 skyColor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);

        void createScene(unsigned int width, unsigned int height);
        void placeRoot(double at);
        double tickSeconds() const;
        void frameCamera(bool follow);

    public:
        ParticlePreviewRender() = default;
        ~ParticlePreviewRender();

        ParticlePreviewRender(const ParticlePreviewRender&) = delete;
        ParticlePreviewRender& operator=(const ParticlePreviewRender&) = delete;

        // a scene playing the effect asset (assets-relative path); the same path keeps it
        void build(const std::string& assetPath, unsigned int size);
        void update();
        void restart();
        void clear();

        const std::string& getPath() const { return path; }
        uint32_t getParticleCount() const;
        Texture getTexture();
        Scene* getScene() { return scene; }

        // ---- the stepped stage
        // builds the stage (the same systems keep it, only the size follows); time 0, not seeking
        void buildStage(const std::vector<StageSystem>& list, unsigned int width, unsigned int height);
        bool isStage() const { return stepped; }
        const std::vector<StageSystem>& getStageSystems() const { return stageSystems; }
        size_t systemCount() const { return systems.size(); }
        Entity systemEntity(size_t index) const;

        // the seed every system plays with (system i: seed + i), pinned: an edit keeps it
        void setSeed(uint32_t seed);
        uint32_t getSeed() const { return seed; }
        // the stage time: what seek() went to, plus what advance() and step() added
        double getTime() const { return time; }
        // starts re-simulating to `time` (a scrub, an edit, a loop): the same seed, the same frame
        void seek(double time);
        // the seek's work for at most budgetMs; true when the stage is at its time
        bool updateSeek(double budgetMs);
        bool isSeeking() const { return seeking; }
        // 0 to 1: how far the seek is
        float seekProgress() const;
        double seekTargetTime() const { return seekTarget; }
        // the effects changed (an edit): the checkpoints no longer hold their runs
        void invalidate();
        // plays `dt` seconds on (nothing while seeking)
        void advance(double dt);
        // one tick of the root's tick rate
        void step();
        double tickLength() const { return tickSeconds(); }

        void setMovingEmitter(bool moving);
        bool isMovingEmitter() const { return moving; }
        // the emitter's offset from the stage's centre at a time (the moving emitter's circle)
        static Vector3 movingOffset(double time);
        void setBackground(Background background, Scene* sceneWithSky);
        Background getBackground() const { return background; }
        // the camera around the particles: degrees, and a factor of the framed distance
        void orbit(float yawDegrees, float pitchDegrees);
        void zoom(float factor);
        void resetView();

        // the live systems' sum (children included)
        StageStats stats();
        // an exposed parameter or a trigger of every system that has it (preview only, the files untouched)
        void setExposed(const std::string& name, const Vector4& value);
        void sendTrigger(const std::string& name);

        // frames the camera and queues the stage's frame
        void render();
    };

}
