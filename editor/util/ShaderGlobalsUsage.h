// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "shader/ShaderGlobals.h"
#include "ecs/Entity.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace jasperengine {
    class Scene;
}

namespace jasperengine::editor {

    class Project;
    struct SceneProject;

    // Who uses the project's global shader values (Project Settings > Graphics > Shader Globals
    // "Used by", MCP): the material and component bindings of the open scenes, and the loaded
    // shaders of their objects whose globals blocks or texture uniforms read them. Scanned on
    // demand, not every frame.
    class ShaderGlobalsUsage {
    public:
        // where a bindings list lives: a submesh material, a component, a post-process pass
        enum class Owner { Material, UI, Points, Lines, Sky, Trail, PostProcess };
        struct BindingsRef {
            uint32_t sceneId = 0;
            Entity entity = NULL_ENTITY;
            Owner owner = Owner::Material;
            int index = 0;          // the submesh, the pass
        };

        struct Use {
            std::string global;
            bool binding = false;   // a binding (member follows the global), else a shader reads it
            uint32_t sceneId = 0;
            std::string scene;
            Entity entity = NULL_ENTITY;
            std::string object;     // the GameObject's name ("" for a post-process pass)
            std::string where;      // "submesh 0 material", "UI", "post-process pass 1", ...
            std::string member;     // the bound member, or the shader's member / texture
            std::string shader;     // the shader reference ("" the built-in)
            std::string stage;      // a shader use: "vertex", "fragment" or "texture"
        };

        // every use of `global` ("" = of every global), bindings first
        static std::vector<Use> find(Project* project, const std::string& global = "");
        // "Material of Cube (submesh 0, tint) in Main", for lists
        static std::string describe(const Use& use);

        // every bindings list of the open scenes
        static void forEachBindings(Project* project, const std::function<void(const BindingsRef&, const ShaderGlobalBindings&)>& visit);
        static ShaderGlobalBindings* bindingsOf(Project* project, const BindingsRef& ref);
        // replaces a list and makes the object write its blocks again (the scene is modified)
        static bool setBindings(Project* project, const BindingsRef& ref, const ShaderGlobalBindings& bindings);

        // a list with the globals renamed (old -> new); unchanged when none is bound
        static ShaderGlobalBindings renamed(const ShaderGlobalBindings& bindings, const std::map<std::string, std::string>& renames);
    };

}
