// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Every unresolved asset reference of the project (S04 R16), from the dependency index:
// open and closed scenes, prefabs, materials and project.yaml, grouped by the missing GUID.
// It also lists the references that resolved only by their path (R11, "Resolved by path")
// and the orphan metas (R5). The entry shape follows OpenEngine's MissingAssetTracker
// (Apps/Editor/Include/MissingAssetTracker.h, MIT): GUID, expected type, last path, and
// per referrer the owner, GameObject, component and field; theirs scans the active world,
// this one the whole project.

#pragma once

#include "asset/AssetIndex.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    class MissingAssetTracker {
    public:
        struct Referrer {
            Guid owner;
            std::string ownerPath;     // project-relative ("project.yaml" for the project)
            AssetType ownerType = AssetType::Unknown;
            uint64_t entity = 0;
            std::string entityPath;
            std::string component;
            std::string field;
            std::string locator;
            DepEdgeKind kind = DepEdgeKind::Other;
        };

        struct Group {
            Guid guid;                 // null for a path-only reference (written before GUIDs)
            std::string lastPath;      // project-relative, the hint the referrers hold
            AssetType expected = AssetType::Unknown;
            std::vector<Referrer> referrers;
        };

        struct ResolvedByPath {
            Guid referencedGuid;       // the GUID the reference holds (unknown to the project)
            Guid currentGuid;          // the GUID of the file at the hint
            std::string path;
            std::vector<Referrer> referrers;
        };

        // Recomputes everything when the database or the index changed since the last call
        void update(const AssetDatabase& database, const AssetIndex& index, bool force = false);

        const std::vector<Group>& getGroups() const { return groups; }
        const std::vector<ResolvedByPath>& getResolvedByPath() const { return resolvedByPath; }
        const std::vector<AssetDatabase::OrphanMeta>& getOrphanMetas() const { return orphanMetas; }
        // Assets in a folder linked in from out of the project whose identity cannot be written
        // there (a read-only target, S68 R7): they work, with GUIDs the database keeps
        const std::vector<std::string>& getReadOnlyLinked() const { return readOnlyLinked; }
        // The status bar badge: missing groups and orphan metas, and one for the read-only files
        size_t count() const { return groups.size() + orphanMetas.size() + (readOnlyLinked.empty() ? 0 : 1); }
        const Group* findGroup(const Guid& guid, const std::string& lastPath = std::string()) const;
        // Bumped whenever the lists change
        uint64_t getVersion() const { return version; }

    private:
        std::vector<Group> groups;
        std::vector<ResolvedByPath> resolvedByPath;
        std::vector<AssetDatabase::OrphanMeta> orphanMetas;
        std::vector<std::string> readOnlyLinked;
        uint64_t databaseVersion = UINT64_MAX;
        uint64_t indexVersion = UINT64_MAX;
        uint64_t version = 0;
    };

}
