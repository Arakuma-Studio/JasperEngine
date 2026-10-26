// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Scene.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/ParticleSystem.h"
#include "texture/Texture.h"

#include <memory>
#include <string>

namespace jasperengine::editor{

    // The Inspector's preview of a .particles asset (docs/specs/S20-particles.md R28): the effect
    // playing in a scene of its own, looped (a one-shot restarts when its last particle dies),
    // framed by its particles' bounds. Each update queues one more frame of it.
    class ParticlePreviewRender{
    private:
        Scene* scene = nullptr;
        Camera* camera = nullptr;
        Light* light = nullptr;
        std::unique_ptr<ParticleSystem> emitter;
        std::string path;
        // the framing eases to the bounds the particles reach
        Vector3 center = Vector3(0.0f, 1.0f, 0.0f);
        float radius = 3.0f;

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
    };

}
