// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/type/MultiPropertyCmd.h"
#include "component/FogComponent.h"
#include "render/FogProfile.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // Link, New, Duplicate and Unlink of a Fog's or Fog Zone's profile (docs/specs/S101-vapor.md R23,
    // R27, R32) as one undo step: the reference set by a PropertyCmd on every GameObject given,
    // a built-in's first use copied into assets/FogProfiles/<id>.fogprofile (FogProfileAssetCmd)
    // and a Fog not of Type Height switched to Height when the profile sets height fields, all in
    // that step. Undoing it undoes the file too. The Fog and Fog Zone inspectors and MCP
    // apply_atmosphere_preset use it. Every builder returns nullptr with `error` when the scene,
    // a component or the profile is wrong, or nothing would change (error empty then).
    class LinkFogProfileCmd : public MultiPropertyCmd {
    private:
        std::string label;

    public:
        explicit LinkFogProfileCmd(std::string label);

        // `profile`: "builtin:<id>" (a bare built-in id too) or a .fogprofile path, project- or
        // assets-relative; `component` FogComponent or FogZoneComponent
        static LinkFogProfileCmd* link(Project* project, uint32_t sceneId, const std::vector<Entity>& entities, ComponentType component,
                                       const std::string& profile, std::string& error);
        // New and Duplicate: a new file at `assetPath` (assets-relative) holding `data`, with a
        // new GUID, linked as link() links
        static LinkFogProfileCmd* linkNew(Project* project, uint32_t sceneId, const std::vector<Entity>& entities, ComponentType component,
                                          const std::string& assetPath, const FogProfileData& data, std::string& error);
        // A Fog takes the values it shows (resolvedLook) as its own and loses the link, so its
        // look stays; a Fog Zone loses its profile
        static LinkFogProfileCmd* unlink(Project* project, uint32_t sceneId, const std::vector<Entity>& entities, ComponentType component,
                                         std::string& error);

        // the Fog's own fields with the profile's overrides applied (none: its own)
        static FogComponent resolvedLook(const FogComponent& fog, const FogProfileData* profile);
        // the profile sets a field only a Height fog reads (anything but Color, Density and the
        // linear range)
        static bool setsHeightFields(const FogProfileData& profile);
        static bool isHeightField(FogField field);

        std::string describe() const override { return label; }
        const char* typeName() const override { return "LinkFogProfileCmd"; }
        bool mergeWith(Command*) override { return false; }
    };

    // The PropertyCmd that sets `field` of a Fog to the value `values` holds (the Inspector's
    // unticked rows, Unlink)
    std::unique_ptr<Command> fogFieldPropertyCmd(Project* project, uint32_t sceneId, Entity entity, FogField field, const FogComponent& values);

}
