// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "asset/Guid.h"

#include <filesystem>
#include <string>

namespace jasperengine {
    struct FogProfileData;
}

namespace jasperengine::editor{

    class Project;

    // An edit of a .fogprofile asset (docs/specs/S101-vapor.md R5, R24, R26, R33): the file's canonical
    // JSON (FogProfileIO::write) before and after. Executing writes the file and hands the profile
    // to the engine's library, so every Fog and Fog Zone using it shows it on the next frame. A
    // drag merges into one step. An empty `before` creates the file, with a GUID that stays the
    // same on redo; undoing it deletes the file and its .meta. The Fog and Fog Zone inspectors,
    // the .fogprofile inspector, Create > Fog Profile, LinkFogProfileCmd and MCP use it.
    class FogProfileAssetCmd: public Command{
    private:
        Project* project;
        std::string path;       // assets-relative: what Fog.profile stores, the library's key
        std::string before;
        std::string after;
        Guid guid;              // a created file's identity
        bool createdFolder = false;

        bool apply(const std::string& text, bool undoing);
        std::filesystem::path file() const;

    public:
        // `path` assets-relative ("FogProfiles/night.fogprofile"); `before` "" creates the file
        FogProfileAssetCmd(Project* project, std::string path, std::string before, std::string after, Guid guid = Guid());

        // the edit from one profile to another, nullptr when they write the same text
        static FogProfileAssetCmd* edit(Project* project, const std::string& path, const FogProfileData& before, const FogProfileData& after);
        // a new file holding `data`, with a new GUID
        static FogProfileAssetCmd* create(Project* project, const std::string& path, const FogProfileData& data);
        // "<folder>/<name>.fogprofile", or "<folder>/<name> 1.fogprofile", ... when taken
        // (assets-relative; folder "" is the assets folder)
        static std::string freePath(const Project& project, const std::string& folder, const std::string& name);

        bool execute() override;
        void undo() override;
        bool mergeWith(Command* olderCommand) override;
        std::vector<Touched> touchedPaths(bool undo) const override;

        const std::string& getPath() const { return path; }
        const std::string& getBefore() const { return before; }
        const std::string& getAfter() const { return after; }
        const Guid& getGuid() const { return guid; }
        bool creates() const { return before.empty(); }

        bool affectsStructure() const override { return before.empty(); }
        std::string describe() const override;
        const char* typeName() const override { return "FogProfileAssetCmd"; }
        bool isNoOp() const override { return before == after; }
        size_t memoryBytes() const override { return sizeof(*this) + before.capacity() + after.capacity() + path.capacity(); }
    };

}
