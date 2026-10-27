// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "asset/Guid.h"

#include <string>

namespace jasperengine::editor{

    class Project;

    // An edit of a .particles asset (docs/specs/S20-particles.md R27): the file's canonical JSON
    // before and after. Executing writes the file and hands the document to the effect library,
    // so every Particle System playing the asset rebinds at once (keeping its particles when only
    // parameters changed). A drag merges into one step. An empty `before` creates the file
    // (Save as Asset), with a GUID that stays the same on redo; undoing it deletes the file.
    class ParticleEffectAssetCmd: public Command{
    private:
        Project* project;
        std::string path;       // assets-relative, the effect library's key
        std::string before;
        std::string after;
        Guid guid;              // a created file's identity

        bool apply(const std::string& text);

    public:
        ParticleEffectAssetCmd(Project* project, std::string path, std::string before, std::string after, Guid guid = Guid());

        bool execute() override;
        void undo() override;
        bool mergeWith(Command* olderCommand) override;

        const std::string& getPath() const { return path; }

        bool affectsStructure() const override { return before.empty(); }
        std::string describe() const override;
        const char* typeName() const override { return "ParticleEffectAssetCmd"; }
        bool isNoOp() const override { return before == after; }
        size_t memoryBytes() const override { return sizeof(*this) + before.capacity() + after.capacity(); }
    };

}
