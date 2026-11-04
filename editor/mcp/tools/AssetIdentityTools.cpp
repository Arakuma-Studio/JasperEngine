// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Asset identity (docs/specs/S04-asset-identity.md §6.9): GUID resolution, the dependency
// index (find_references, get_asset_dependencies), the Missing Assets lists and their
// actions (retarget_asset_references, clear_asset_references).

#include "McpTools.h"
#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "Project.h"
#include "command/CommandHistory.h"
#include "command/type/AssetRefCmds.h"
#include "window/ProjectWindow.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::mcp;

namespace {

const char* resolvedByName(AssetDatabase::ResolvedBy by) {
    switch (by) {
        case AssetDatabase::ResolvedBy::Live: return "live";
        case AssetDatabase::ResolvedBy::Former: return "former";
        case AssetDatabase::ResolvedBy::SubAsset: return "sub_asset";
        case AssetDatabase::ResolvedBy::LegacyPath: return "legacy_path";
        case AssetDatabase::ResolvedBy::PathFallback: return "path_fallback";
        case AssetDatabase::ResolvedBy::Missing: break;
    }
    return "missing";
}

// An asset named by its GUID or its project-relative path ("<model>#<key>" for a sub-asset)
AssetDatabase::Resolution resolveArg(Project* project, const std::string& asset, const std::string& hint, AssetType expected) {
    const AssetDatabase& database = project->getAssetDatabase();
    if (Guid::isValidText(asset)) {
        return database.resolve(Guid::parse(asset), hint, expected, expected != AssetType::Prefab);
    }
    const std::string path = project->normalizeToProjectRelative(asset).generic_string();
    AssetDatabase::Resolution found = database.lookupPath(path);
    if (!found.found()) {
        found = database.resolve(Guid(), path, expected, false);
    }
    return found;
}

Json identityAssetJson(Project* project, const Guid& guid, const std::string& fallbackPath) {
    AssetDatabase::AssetInfo info;
    if (!guid.isNull() && project->getAssetDatabase().getInfo(guid, info)) {
        return Json{{"path", info.path}, {"guid", info.guid.str()}, {"type", assetTypeName(info.type)}};
    }
    const AssetDatabase::Resolution found = project->getAssetDatabase().resolve(guid, fallbackPath, AssetType::Unknown, false);
    if (found.found()) {
        return Json{{"path", found.path}, {"guid", found.guid.str()}, {"type", assetTypeName(found.type)}};
    }
    return Json{{"path", fallbackPath}, {"guid", guid.isNull() ? Json(nullptr) : Json(guid.str())}, {"missing", true}};
}

std::string fieldOf(const std::string& locator, const std::string& component) {
    const size_t slash = locator.find('/');
    std::string field = slash == std::string::npos ? locator : locator.substr(slash + 1);
    if (!component.empty() && field.compare(0, component.size() + 1, component + ".") == 0) {
        field = field.substr(component.size() + 1);
    }
    return field;
}

Json identityEdgeJson(Project* project, const DepEdge& edge, bool withTarget) {
    const AssetIndex& index = project->getAssetIndex();
    Json item;
    item["referrer"] = edge.referrer == AssetIndex::projectReferrer()
        ? Json{{"path", "project.yaml"}, {"guid", nullptr}, {"type", "project"}}
        : identityAssetJson(project, edge.referrer, index.referrerPath(edge.referrer));
    if (withTarget) item["target"] = identityAssetJson(project, edge.target, edge.targetPath);
    if (edge.entity != 0) item["entity"] = Json{{"id", edge.entity}, {"path", edge.entityPath}};
    if (!edge.component.empty()) item["component"] = edge.component;
    item["field"] = edge.line ? "line " + std::to_string(edge.line) : fieldOf(edge.field, edge.component);
    item["locator"] = edge.field;
    if (edge.ordinal) item["ordinal"] = edge.ordinal;
    item["kind"] = depEdgeKindName(edge.kind);
    return item;
}

Json referrerJson(const MissingAssetTracker::Referrer& referrer) {
    Json item{{"owner", referrer.ownerPath}, {"field", referrer.field}, {"locator", referrer.locator},
              {"kind", depEdgeKindName(referrer.kind)}};
    if (referrer.entity != 0) item["entity"] = Json{{"id", referrer.entity}, {"path", referrer.entityPath}};
    if (!referrer.component.empty()) item["component"] = referrer.component;
    return item;
}

// Runs once the dependency index has its first build (a fresh project indexes in the background)
void whenIndexed(const CallPtr& call, std::function<void()> then) {
    Project* project = call->ctx().project();
    // files an agent changed with its own tools since the last frame's look count now
    project->refreshAssetDatabase(true);
    if (project->getAssetIndex().isReady()) {
        then();
        return;
    }
    call->waitUntil([project] { return project->getAssetIndex().isReady(); }, 60000, [call, then](bool timedOut) {
        if (timedOut) {
            call->fail("The asset index is still being built; try again in a moment.");
            return;
        }
        then();
    });
}

bool identityRunProjectCommand(const CallPtr& call, Command* command) {
    command->setNoMerge();
    const bool ok = call->ctx().project()->getProjectCommandHistory()->addCommand(command);
    if (ProjectWindow* window = call->ctx().projectWindow()) window->refreshCurrentDirectory();
    return ok;
}

// ------------------------------------------------------------------ resolve_asset

void resolveAssetTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    const std::string asset = call->args().value("asset", "");
    const std::string hint = call->args().value("path", "");
    const AssetType expected = assetTypeFromName(call->args().value("type", ""));
    if (asset.empty()) {
        call->fail("asset must be a project-relative path or a 32-hex-digit GUID.");
        return;
    }
    const AssetDatabase::Resolution found = resolveArg(project, asset, hint, expected);
    Json data{{"resolved_by", resolvedByName(found.by)}, {"exists", found.found()}};
    data["guid"] = found.found() ? Json(found.guid.str()) : (Guid::isValidText(asset) ? Json(asset) : Json(nullptr));
    data["path"] = found.found() ? found.path : (Guid::isValidText(asset) ? hint : asset);
    data["type"] = assetTypeName(found.type);
    Json former = Json::array();
    Json subAssets = Json::array();
    AssetDatabase::AssetInfo info;
    if (found.found() && found.by != AssetDatabase::ResolvedBy::SubAsset && project->getAssetDatabase().getInfo(found.guid, info)) {
        for (const Guid& guid : info.formerGuids) former.push_back(guid.str());
        for (const AssetDatabase::SubAsset& sub : info.subAssets) {
            subAssets.push_back({{"key", sub.key}, {"guid", sub.guid.str()}, {"name", sub.name}, {"kind", subAssetKindName(sub.kind)}});
        }
    }
    data["former_guids"] = former;
    data["sub_assets"] = subAssets;
    const std::string text = found.found()
        ? asset + " -> " + found.path + " (" + found.guid.str() + ", " + resolvedByName(found.by) + ")"
        : asset + " is missing: no asset has that " + (Guid::isValidText(asset) ? "GUID." : "path.");
    call->ok(text, data);
}

// ------------------------------------------------------------------ find_references

void findReferencesTool(const CallPtr& call) {
    whenIndexed(call, [call] {
        Project* project = call->ctx().project();
        const std::string asset = call->args().value("asset", "");
        const bool transitive = call->args().value("transitive", false);
        const size_t limit = static_cast<size_t>(std::clamp(call->args().value("limit", 500), 1, 5000));
        const AssetDatabase::Resolution found = resolveArg(project, asset, std::string(), AssetType::Unknown);
        const Guid guid = found.found() ? found.guid : (Guid::isValidText(asset) ? Guid::parse(asset) : Guid());
        const std::string path = found.found() ? found.path : project->normalizeToProjectRelative(asset).generic_string();
        Json list = Json::array();
        for (const DepEdge& edge : project->getAssetIndex().referencesTo(guid, Guid::isValidText(asset) && !found.found() ? std::string() : path, transitive, limit)) {
            list.push_back(identityEdgeJson(project, edge, transitive));
        }
        std::string text = std::to_string(list.size()) + " reference(s) to " + path + ":";
        for (const Json& item : list) {
            text += "\n" + item["referrer"]["path"].get<std::string>() + " " + item["locator"].get<std::string>();
        }
        call->ok(text, Json{{"asset", identityAssetJson(project, guid, path)}, {"references", list}, {"truncated", list.size() >= limit}});
    });
}

// ------------------------------------------------------------------ get_asset_dependencies

void getAssetDependenciesTool(const CallPtr& call) {
    whenIndexed(call, [call] {
        Project* project = call->ctx().project();
        const std::string asset = call->args().value("asset", "");
        const int depth = std::clamp(call->args().value("depth", 1), 1, 16);
        Guid referrer;
        if (asset == "project.yaml") {
            referrer = AssetIndex::projectReferrer();
        } else {
            const AssetDatabase::Resolution found = resolveArg(project, asset, std::string(), AssetType::Unknown);
            if (!found.found()) {
                call->fail(asset + " is not an asset of the project.");
                return;
            }
            referrer = found.guid;
        }
        Json list = Json::array();
        for (const DepEdge& edge : project->getAssetIndex().dependenciesOf(referrer, depth)) {
            list.push_back(identityEdgeJson(project, edge, true));
        }
        std::string text = std::to_string(list.size()) + " dependenc" + (list.size() == 1 ? "y" : "ies") + ":";
        for (const Json& item : list) {
            text += "\n" + item["locator"].get<std::string>() + " -> " + item["target"]["path"].get<std::string>();
        }
        call->ok(text, Json{{"dependencies", list}});
    });
}

// ------------------------------------------------------------------ list_missing_assets

void listMissingAssetsTool(const CallPtr& call) {
    whenIndexed(call, [call] {
        Project* project = call->ctx().project();
        const bool openOnly = call->args().value("scope", "project") == "open_scenes";
        MissingAssetTracker& tracker = project->getMissingAssets();
        tracker.update(project->getAssetDatabase(), project->getAssetIndex(), true);

        std::vector<std::string> openOwners;
        if (openOnly) {
            for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
                if (sceneProject->scene && !sceneProject->filepath.empty()) {
                    openOwners.push_back(project->normalizeToProjectRelative(sceneProject->filepath).generic_string());
                }
            }
        }
        auto inScope = [&](const MissingAssetTracker::Referrer& referrer) {
            return !openOnly || std::find(openOwners.begin(), openOwners.end(), referrer.ownerPath) != openOwners.end();
        };

        Json missing = Json::array();
        for (const MissingAssetTracker::Group& group : tracker.getGroups()) {
            Json referrers = Json::array();
            for (const MissingAssetTracker::Referrer& referrer : group.referrers) {
                if (inScope(referrer)) referrers.push_back(referrerJson(referrer));
            }
            if (referrers.empty()) continue;
            missing.push_back({{"guid", group.guid.isNull() ? Json(nullptr) : Json(group.guid.str())},
                               {"expected_type", assetTypeName(group.expected)}, {"last_path", group.lastPath},
                               {"referrers", referrers}});
        }
        Json healed = Json::array();
        for (const MissingAssetTracker::ResolvedByPath& entry : tracker.getResolvedByPath()) {
            Json referrers = Json::array();
            for (const MissingAssetTracker::Referrer& referrer : entry.referrers) {
                if (inScope(referrer)) referrers.push_back(referrerJson(referrer));
            }
            if (referrers.empty()) continue;
            healed.push_back({{"referenced_guid", entry.referencedGuid.str()}, {"guid", entry.currentGuid.str()},
                              {"path", entry.path}, {"referrers", referrers}});
        }
        Json orphans = Json::array();
        if (!openOnly) {
            for (const AssetDatabase::OrphanMeta& orphan : tracker.getOrphanMetas()) {
                orphans.push_back({{"meta", orphan.metaPath}, {"asset", orphan.assetPath}, {"guid", orphan.guid.str()}});
            }
        }
        std::string text = std::to_string(missing.size()) + " missing asset(s), " + std::to_string(healed.size()) +
                           " resolved by path, " + std::to_string(orphans.size()) + " orphan meta(s).";
        for (const Json& group : missing) {
            text += "\n" + group["last_path"].get<std::string>() + " (" + std::to_string(group["referrers"].size()) + " reference(s))";
        }
        Json result{{"missing", missing}, {"resolved_by_path", healed}, {"orphan_metas", orphans}};
        // only when a linked folder can't be written: a project without one reads as it did
        if (!openOnly && !tracker.getReadOnlyLinked().empty()) {
            result["read_only_linked"] = tracker.getReadOnlyLinked();
            // named here: a client like Claude Code shows the model this text of a result like this one
            constexpr size_t SHOWN = 20;
            const std::vector<std::string>& readOnly = tracker.getReadOnlyLinked();
            text += "\n" + std::to_string(readOnly.size()) + " asset(s) in a read-only linked folder (they work; their GUIDs are not saved beside them):";
            for (size_t i = 0; i < std::min(readOnly.size(), SHOWN); i++) text += "\n" + readOnly[i];
            if (readOnly.size() > SHOWN) text += "\n... and " + std::to_string(readOnly.size() - SHOWN) + " more";
        }
        call->ok(text, result);
    });
}

// ------------------------------------------------------------------ retarget / clear

// The referrers of a missing group the call names (all of them, or the listed locators: a
// locator, or "<owner>|<locator>")
bool identityGroupTargets(const CallPtr& call, const std::string& from, std::vector<AssetRefTarget>& targets) {
    Project* project = call->ctx().project();
    MissingAssetTracker& tracker = project->getMissingAssets();
    tracker.update(project->getAssetDatabase(), project->getAssetIndex(), true);
    const Guid guid = Guid::isValidText(from) ? Guid::parse(from) : Guid();
    const MissingAssetTracker::Group* group = tracker.findGroup(guid, guid.isNull() ? project->normalizeToProjectRelative(from).generic_string() : std::string());
    if (!group) {
        call->fail("No missing asset " + from + " is referenced (list_missing_assets lists them).");
        return false;
    }
    std::vector<std::string> wanted;
    if (call->args().contains("referrers")) {
        for (const Json& item : call->args()["referrers"]) {
            if (item.is_string()) wanted.push_back(item.get<std::string>());
        }
    }
    for (const MissingAssetTracker::Referrer& referrer : group->referrers) {
        const bool chosen = wanted.empty() || std::any_of(wanted.begin(), wanted.end(), [&](const std::string& item) {
            return item == referrer.locator || item == referrer.ownerPath + "|" + referrer.locator;
        });
        if (chosen) targets.push_back({referrer.ownerPath, referrer.locator, 0});
    }
    if (targets.empty()) {
        call->fail("None of the referrers given reference " + from + ".");
        return false;
    }
    return true;
}

void retargetAssetReferencesTool(const CallPtr& call) {
    whenIndexed(call, [call] {
        Project* project = call->ctx().project();
        const std::string from = call->args().value("from", "");
        const std::string to = call->args().value("to", "");
        const std::string mode = call->args().value("mode", "redirect");
        const AssetDatabase::Resolution target = resolveArg(project, to, std::string(), AssetType::Unknown);
        if (!target.found() || target.by == AssetDatabase::ResolvedBy::SubAsset) {
            call->fail("to must be an existing asset (a path or a GUID).");
            return;
        }
        auto report = std::make_shared<AssetRefCmdReport>();
        if (mode == "redirect") {
            if (!Guid::isValidText(from)) {
                call->fail("A redirect needs from to be the missing GUID; use mode rewrite for a reference by path.");
                return;
            }
            if (call->args().contains("referrers")) {
                call->fail("A redirect retargets every reference; give referrers with mode rewrite.");
                return;
            }
            if (!identityRunProjectCommand(call, new AddFormerGuidCmd(project, target.path, Guid::parse(from), report))) {
                call->fail("Couldn't retarget: " + report->error);
                return;
            }
            call->ok("References to " + from + " resolve to " + target.path + " (its former GUID). Undo with scope project.",
                     Json{{"mode", "redirect"}, {"to", Json{{"path", target.path}, {"guid", target.guid.str()}}},
                          {"files_written", report->filesWritten}});
            return;
        }
        std::vector<AssetRefTarget> targets;
        if (!identityGroupTargets(call, from, targets)) return;
        if (!identityRunProjectCommand(call, new RetargetAssetRefsCmd(project, targets, AssetRef(target.path, target.guid), report))) {
            call->fail("Couldn't retarget: " + report->error);
            return;
        }
        project->relinkMissingPrefabs();
        call->ok("Retargeted " + std::to_string(targets.size()) + " reference(s) to " + target.path + ". Undo with scope project.",
                 Json{{"mode", "rewrite"}, {"retargeted", targets.size()}, {"files_written", report->filesWritten}});
    });
}

void clearAssetReferencesTool(const CallPtr& call) {
    whenIndexed(call, [call] {
        Project* project = call->ctx().project();
        const std::string guid = call->args().value("guid", "");
        std::vector<AssetRefTarget> targets;
        if (!identityGroupTargets(call, guid, targets)) return;
        auto report = std::make_shared<AssetRefCmdReport>();
        if (!identityRunProjectCommand(call, new RetargetAssetRefsCmd(project, targets, AssetRef(), report))) {
            call->fail("Couldn't clear: " + report->error);
            return;
        }
        call->ok("Cleared " + std::to_string(targets.size()) + " reference(s). Undo with scope project.",
                 Json{{"cleared", targets.size()}, {"files_written", report->filesWritten}});
    });
}

} // namespace

namespace jasperengine::editor::mcp {

void registerAssetIdentityTools(McpRegistry& registry) {
    using namespace schema;

    {
        ToolDef tool;
        tool.name = "resolve_asset";
        tool.title = "Resolve Asset";
        tool.description =
            "An asset by path or GUID: its GUID, current path, type, how it resolved (live, former, sub_asset, "
            "legacy_path, path_fallback with a path hint, missing), former GUIDs and sub-assets (model meshes...).";
        tool.inputSchema = object({
            {"asset", string("Project-relative path, \"<model>#<key>\", or GUID.")},
            {"path", string("Path hint for a GUID (a reference's).")},
            {"type", string("Expected type of the hint (\"texture\"...).")}
        }, {"asset"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = resolveAssetTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "find_references";
        tool.title = "Find References";
        tool.description =
            "Who references an asset (open and closed scenes, prefabs, materials, project.yaml, HTML/CSS/Lua/shader "
            "text): referrer, GameObject, component, field, locator, kind. transitive also lists who uses the referrers.";
        tool.inputSchema = object({
            {"asset", string("Path or GUID.")},
            {"transitive", withDefault(boolean(""), false)},
            {"limit", withDefault(integer("", 1, 5000), 500)}
        }, {"asset"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = findReferencesTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "get_asset_dependencies";
        tool.title = "Get Asset Dependencies";
        tool.description = "What an asset references (\"project.yaml\" for the project), as find_references entries with target.";
        tool.inputSchema = object({
            {"asset", string("Path or GUID.")},
            {"depth", withDefault(integer("", 1, 16), 1)}
        }, {"asset"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getAssetDependenciesTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "list_missing_assets";
        tool.title = "List Missing Assets";
        tool.description =
            "Every reference that resolves to no asset, by missing GUID (expected type, last path, referrers); "
            "references bound only by path (resolved_by_path); metas whose asset is gone (orphan_metas).";
        tool.inputSchema = object({
            {"scope", withDefault(stringEnum("", {"project", "open_scenes"}), "project")}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = listMissingAssetsTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "retarget_asset_references";
        tool.title = "Retarget Asset References";
        tool.description =
            "Points the references to a missing asset at another. redirect: from becomes a former GUID of to (no "
            "referrer written). rewrite: the fields are rewritten (all, or the referrers given as locators or "
            "\"owner|locator\"). Undoable; files_written.";
        tool.inputSchema = object({
            {"from", string("Missing GUID (or last path of a reference by path).")},
            {"to", string("Path or GUID.")},
            {"mode", withDefault(stringEnum("", {"redirect", "rewrite"}), "redirect")},
            {"referrers", array(string(""), "")}
        }, {"from", "to"});
        tool.allowedWhilePlaying = false;
        tool.handler = retargetAssetReferencesTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "clear_asset_references";
        tool.title = "Clear Asset References";
        tool.description = "Clears the references to a missing asset (all, or the referrers given as for retarget). Undoable; files_written.";
        tool.inputSchema = object({
            {"guid", string("Missing GUID (or last path).")},
            {"referrers", array(string(""), "")}
        }, {"guid"});
        tool.annotations.destructive = true;
        tool.allowedWhilePlaying = false;
        tool.handler = clearAssetReferencesTool;
        registry.addTool(std::move(tool));
    }
}

} // namespace jasperengine::editor::mcp
