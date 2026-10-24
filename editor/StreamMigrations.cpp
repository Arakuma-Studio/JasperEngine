// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "StreamMigrations.h"

using namespace jasperengine::editor;

namespace {

    // Scene 0 -> 1: the first versioned scene format is today's shape; only the version is new
    void sceneV0ToV1(YAML::Node&, MigrationLog&){
    }

    void projectV0ToV1(YAML::Node&, MigrationLog&){
    }

    void materialV0ToV1(YAML::Node&, MigrationLog&){
    }

    // Every light component below node (scenes nest entities in children; prefabs list their
    // members): a shadowBias saved in NDC depth units (default 0.002) means a bias of
    // shadowBias shadow texels now, so it is reset to the new default of 1.0. The old units
    // cannot be converted: their size depended on each cascade's depth range.
    int resetLightShadowBias(YAML::Node node){
        int reset = 0;
        if (node.IsMap()){
            for (auto it = node.begin(); it != node.end(); ++it){
                YAML::Node value = it->second;
                if (it->first.IsScalar() && it->first.Scalar() == "light" && value.IsMap() && value["type"] && value["shadowBias"]){
                    bool alreadyDefault = false;
                    try {
                        alreadyDefault = value["shadowBias"].as<float>() == 1.0f;
                    } catch (const YAML::Exception&) {
                    }
                    if (!alreadyDefault){
                        value["shadowBias"] = 1.0f;
                        reset++;
                    }
                }else{
                    reset += resetLightShadowBias(value);
                }
            }
        }else if (node.IsSequence()){
            for (YAML::Node child : node){
                reset += resetLightShadowBias(child);
            }
        }
        return reset;
    }

    void logShadowBiasReset(int reset, MigrationLog& log){
        if (reset > 0){
            log.info("Shadow bias is in shadow texels now: " + std::to_string(reset) +
                     (reset == 1 ? " light's" : " lights'") + " Bias reset to 1 (docs/rendering/shadows.md)");
        }
    }

    // Scene 1 -> 2 (S10 shadows): light shadow biases in texels; the old default quality Low
    // becomes Auto (the platform's tier)
    void sceneV1ToV2(YAML::Node& root, MigrationLog& log){
        YAML::Node settings = root["scene"];
        if (settings && settings.IsMap() && settings["shadowQuality"] && settings["shadowQuality"].IsScalar() &&
                settings["shadowQuality"].Scalar() == "low"){
            settings["shadowQuality"] = "auto";
            log.info("Shadow quality Low (the old default) is Auto now: the platform's quality tier");
        }
        logShadowBiasReset(resetLightShadowBias(root["entities"]), log);
    }

    // Prefab 3 -> 4 (S10 shadows): its lights' shadow biases in texels
    void prefabV3ToV4(YAML::Node& root, MigrationLog& log){
        logShadowBiasReset(resetLightShadowBias(root["members"]), log);
    }

}

const char* StreamMigrations::versionKey(DocumentKind kind){
    return kind == DocumentKind::Prefab ? "version" : "formatVersion";
}

int StreamMigrations::currentVersion(DocumentKind kind){
    switch (kind){
        case DocumentKind::Scene: return 2;
        case DocumentKind::Project: return 1;
        case DocumentKind::Material: return 1;
        // Prefab::FILE_VERSION
        case DocumentKind::Prefab: return 4;
    }
    return 0;
}

int StreamMigrations::absentVersion(DocumentKind kind){
    return kind == DocumentKind::Prefab ? 1 : 0;
}

const char* StreamMigrations::kindName(DocumentKind kind){
    switch (kind){
        case DocumentKind::Scene: return "scene";
        case DocumentKind::Project: return "project";
        case DocumentKind::Material: return "material";
        case DocumentKind::Prefab: return "prefab";
    }
    return "document";
}

int StreamMigrations::readVersion(DocumentKind kind, const YAML::Node& root){
    if (!root || !root.IsMap())
        return absentVersion(kind);
    const YAML::Node value = root[versionKey(kind)];
    if (!value || !value.IsScalar())
        return absentVersion(kind);
    int version = 0;
    if (!YAML::convert<int>::decode(value, version) || version < 0)
        return absentVersion(kind);
    return version;
}

const std::vector<Migration>& StreamMigrations::table(DocumentKind kind){
    static const std::vector<Migration> scene = {
        {0, 1, "stamp the scene format version", sceneV0ToV1},
        {1, 2, "shadow bias in shadow texels, shadow quality Low to Auto", sceneV1ToV2},
    };
    static const std::vector<Migration> project = {
        {0, 1, "stamp the project format version", projectV0ToV1},
    };
    static const std::vector<Migration> material = {
        {0, 1, "stamp the material format version", materialV0ToV1},
    };
    // Versions 1 and 2 are upgraded in place by Prefab::loadRegistry (needsUpgrade)
    static const std::vector<Migration> prefab = {
        {3, 4, "shadow bias in shadow texels", prefabV3ToV4},
    };

    switch (kind){
        case DocumentKind::Scene: return scene;
        case DocumentKind::Project: return project;
        case DocumentKind::Material: return material;
        case DocumentKind::Prefab: return prefab;
    }
    return scene;
}

MigrationResult StreamMigrations::migrate(DocumentKind kind, YAML::Node& root){
    MigrationResult result;
    result.versionRead = readVersion(kind, root);
    result.versionCurrent = currentVersion(kind);
    if (!root || !root.IsMap() || result.newer() || result.versionRead == result.versionCurrent)
        return result;

    int version = result.versionRead;
    for (const Migration& migration : table(kind)){
        if (migration.from != version)
            continue;
        migration.apply(root, result.log);
        result.applied.push_back(migration.name);
        version = migration.to;
    }
    if (result.migrated())
        stampVersion(kind, root, version);
    return result;
}

void StreamMigrations::stampVersion(DocumentKind kind, YAML::Node& root, int version){
    const char* key = versionKey(kind);
    YAML::Node ordered(YAML::NodeType::Map);
    ordered[key] = version;
    for (auto it = root.begin(); it != root.end(); ++it){
        if (it->first.IsScalar() && it->first.Scalar() == key)
            continue;
        ordered[it->first] = it->second;
    }
    root = ordered;
}

void StreamMigrations::stampCurrentVersion(DocumentKind kind, YAML::Node& root){
    stampVersion(kind, root, currentVersion(kind));
}

std::string StreamMigrations::newerFormatMessage(DocumentKind kind, int versionRead){
    return std::string("Written by a newer JasperEngine (") + kindName(kind) + " format " + std::to_string(versionRead) +
            "; this editor writes " + std::to_string(currentVersion(kind)) + ").";
}
