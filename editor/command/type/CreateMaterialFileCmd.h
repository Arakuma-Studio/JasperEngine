// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "Project.h"
#include "util/EntityPayload.h"
#include <filesystem>
#include <memory>
#include <string>

namespace fs = std::filesystem;

namespace jasperengine::editor {

    class CreateMaterialFileCmd: public Command {
    private:
        Project* project;
        fs::path targetFile;
        std::string payload;
        std::unique_ptr<Command> materialCmd;

        static fs::path findAvailableTargetFile(const fs::path& directory);

    public:
        CreateMaterialFileCmd(Project* project, const fs::path& directory, const char* materialContent,
                              size_t contentLen, const MaterialPayload* sourceMaterial = nullptr);

        bool execute() override;
        void undo() override;

        // "Create Material New Material.material"
        std::string describe() const override { return "Create Material " + CommandNames::file(targetFile); }
        const char* typeName() const override { return "CreateMaterialFileCmd"; }
        size_t memoryBytes() const override { return sizeof(*this) + payload.capacity() + (materialCmd ? materialCmd->memoryBytes() : 0); }

    };

}