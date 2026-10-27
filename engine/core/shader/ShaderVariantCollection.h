// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SHADERVARIANTCOLLECTION_H
#define SHADERVARIANTCOLLECTION_H

#include "Export.h"
#include "ShaderBuildTypes.h"
#include "render/Render.h"
#include "ecs/Entity.h"

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {

    class ShaderRender;
    class Scene;

    // A set of shader variants to have ready before they are needed (Unity:
    // ShaderVariantCollection): a loading screen warms up what the next level's scripts
    // will build, so nothing pops in or stalls when it appears. Each variant comes from
    // the build when it ships it precompiled, otherwise from the runtime compiler.
    //
    //   ShaderVariantCollection variants;
    //   variants.add("mesh:Uv1,Vc4,Fog");
    //   variants.addEntityVariants(scene, entity);   // what an object draws with now
    //   ...every frame: if (variants.warmUp()) { /* all ready */ }
    //
    // The collection keeps the programs it warmed up alive until it is cleared or
    // destroyed.
    class JASPERENGINE_API ShaderVariantCollection {
    private:
        std::vector<ShaderKey> variants;
        std::map<ShaderKey, std::shared_ptr<ShaderRender>> held;

        void releaseVariant(ShaderKey key);

    public:
        ShaderVariantCollection();
        ShaderVariantCollection(const ShaderVariantCollection& other);
        ShaderVariantCollection& operator=(const ShaderVariantCollection& other);
        virtual ~ShaderVariantCollection();

        // customShader: a forked shader's project-relative base path, "" for built-in.
        // False when the variant is already in or the spec does not parse.
        bool add(ShaderType type, uint64_t properties, const std::string& customShader = "");
        // "mesh:Ult,Vc4", "depth:Ski", "mesh:0x41" (ShaderPool::parseShaderSpec)
        bool add(const std::string& spec, const std::string& customShader = "");
        bool addKey(ShaderKey key);
        // The variants an object of a scene draws with as it is now (meshes, with their
        // shadow and G-buffer passes, UI, points, lines, sky); how many were new
        size_t addEntityVariants(Scene* scene, Entity entity);

        bool remove(ShaderType type, uint64_t properties, const std::string& customShader = "");
        bool remove(const std::string& spec, const std::string& customShader = "");
        bool contains(ShaderType type, uint64_t properties, const std::string& customShader = "") const;
        bool contains(const std::string& spec, const std::string& customShader = "") const;
        void clear();

        size_t getVariantCount() const;
        const std::vector<ShaderKey>& getVariants() const;

        // Requests every variant not ready yet; true once all are ready or failed.
        // Call it again, once a frame, until it returns true.
        bool warmUp();
        // Requests at most maxRequests variants not requested yet (the ones in flight
        // keep going); true once all are ready or failed (Unity: WarmUpProgressively)
        bool warmUpProgressively(size_t maxRequests);

        // every variant ready or failed
        bool isWarmedUp() const;
        size_t getWarmedUpVariantCount() const;
        size_t getFailedVariantCount() const;
    };

}

#endif // SHADERVARIANTCOLLECTION_H
