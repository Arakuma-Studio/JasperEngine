// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef UIPARTICLESYSTEMELEMENT_H
#define UIPARTICLESYSTEMELEMENT_H

#include "Export.h"
#include "Entity.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "math/Rect.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/Texture.h>

#include <memory>
#include <string>
#include <vector>

namespace jasperengine {

    class Scene;

    // One particle as the element draws it: the emitter's local position (y up, prefab units),
    // its diameter, rotation (radians), colour (linear) and sprite frame (uv rect)
    struct UIParticleQuad {
        Vector3 position;
        float size = 1.0f;
        float rotation = 0.0f;
        Vector4 color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        Rect textureRect = Rect(0.0f, 0.0f, 1.0f, 1.0f);
    };

    // The particles of an emitter entity, whatever renders them: a Points emitter (render
    // points), or an InstancedMesh one (instance data) once the particles feature makes quad
    // instances the default. The seam the <particle-system> element reads through.
    namespace UIParticleSource {
        // false when `emitter` has neither (nothing drawn)
        JASPERENGINE_API bool read(Scene* scene, Entity emitter, std::vector<UIParticleQuad>& out);
        // the texture path and whether it blends additively
        JASPERENGINE_API bool material(Scene* scene, Entity emitter, std::string& texturePath, bool& additive);
    }

    // <particle-system src="fx/confetti.prefab" autoplay loop time="ui|game" scale="1">: a
    // replaced inline-block element that plays a prefab's Particle System on a hidden runtime
    // entity of its document's scene (its ActionSystem simulates it) and draws the particles as
    // one mesh in document order, inside the element's clip and transform. Default size 0x0,
    // the emitter at the content box's centre, one prefab unit = `scale` CSS px, y up.
    class JASPERENGINE_INTERNAL UIParticleSystemElement : public Rml::Element {
    public:
        RMLUI_RTTI_DeclareWithParent(UIParticleSystemElement, Rml::Element)

        explicit UIParticleSystemElement(const Rml::String& tag);
        ~UIParticleSystemElement() override;

        static void registerInstancer();
        static void releaseInstancer();

        bool GetIntrinsicDimensions(Rml::Vector2f& dimensions, float& ratio) override;

        void play();
        void stop();
        void pause();
        void clear();
        bool isPlaying() const;
        // the hidden entity's Particle System (NULL_ENTITY before it is made or without one)
        Scene* getScene() const;
        Entity getParticleEntity() const;
        // the source's load error ("" = none)
        const std::string& getError() const;

        // the quads of the last frame, in the element's content space (tests)
        size_t getDrawnParticleCount() const;

    protected:
        void OnUpdate() override;
        void OnRender() override;
        void OnAttributeChange(const Rml::ElementAttributes& changed_attributes) override;

    private:
        void destroyEntity();
        bool instantiate();
        void applyAttributes();
        void fail(const std::string& message);

        Scene* scene = nullptr;
        std::weak_ptr<char> sceneLifetime;
        Entity root = NULL_ENTITY;      // the prefab's root (hidden)
        Entity system = NULL_ENTITY;    // the entity with the Particle System
        Entity emitter = NULL_ENTITY;   // its action target (Points or InstancedMesh)
        bool sourceDirty = true;
        bool completeSent = false;
        bool wasRunning = false;
        std::string error;
        std::string texturePath;
        bool additive = false;

        std::vector<UIParticleQuad> quads;
        size_t drawn = 0;
        Rml::Geometry geometry;
        Rml::Texture texture;
        std::string loadedTexture;
    };

}

#endif // UIPARTICLESYSTEMELEMENT_H
