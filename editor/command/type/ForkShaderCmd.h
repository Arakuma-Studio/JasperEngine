// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "util/ProjectUtils.h"
#include "ecs/Entity.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace jasperengine::editor {

    // Forks a built-in (or library) shader and points a component's customShader (customDepthShader
    // for a DEPTH fork, or a scene's default shader property) at the new fork as a single
    // undoable step: execute() writes the .vert/.frag (and optional private include) files
    // and sets the property; undo() restores the property and deletes the forked files.
    class ForkShaderCmd : public Command {
    private:
        Project* project;
        ProjectUtils::ShaderForkPlan plan;
        std::unique_ptr<Command> propertyCmd;  // sets customShader (or scene property) to plan.base
        bool executed = false;

    public:
        ForkShaderCmd(Project* project, uint32_t sceneId, Entity entity, ComponentType cpType,
                      ShaderType shaderType, const std::filesystem::path& targetDirRel,
                      const std::string& baseName, bool forkIncludes = false);

        // scene-level variant: sets a scene default shader property (e.g. "default_mesh_shader")
        ForkShaderCmd(Project* project, SceneProject* sceneProject, ShaderType shaderType,
                      const std::string& scenePropertyName,
                      const std::filesystem::path& targetDirRel,
                      const std::string& baseName, bool forkIncludes = false);

        // generic variant: the caller builds the command that points a property at the
        // fork, given its base path (the post-process shader lives inside a list value)
        // (librarySource: a fork of that library shader, "library:water", instead of the
        // built-in one of shaderType, which is then the library shader's type)
        ForkShaderCmd(Project* project, ShaderType shaderType,
                      const std::function<std::unique_ptr<Command>(const std::string&)>& makePropertyCmd,
                      const std::filesystem::path& targetDirRel,
                      const std::string& baseName, bool forkIncludes = false,
                      const std::string& librarySource = std::string());

        bool isValid() const { return plan.valid; }
        const std::string& getBase() const { return plan.base; }
        const std::string& getError() const { return plan.error; }

        bool execute() override;

        std::vector<Touched> touchedPaths(bool undo) const override;
        void undo() override;

        // "Fork Shader shaders/myMesh"
        std::string describe() const override { return "Fork Shader " + plan.base; }
        const char* typeName() const override { return "ForkShaderCmd"; }
        size_t memoryBytes() const override {
            size_t bytes = sizeof(*this) + plan.vertContent.capacity() + plan.fragContent.capacity() + (propertyCmd ? propertyCmd->memoryBytes() : 0);
            for (const auto& file : plan.includeFiles) {
                bytes += sizeof(file) + file.content.capacity();
            }
            return bytes;
        }
    };

}
