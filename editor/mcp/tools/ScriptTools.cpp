// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// W4: Lua/C++ scripts, compile check, code editor sync
#include "camera/ShakePreset.h"
#include "io/watch/LinkedFolders.h"
#include "media/VideoFormats.h"
#include "McpTools.h"
#include "DocumentFile.h"
#include "jobs/JobSystem.h"
#include "W4Shared.h"
#include "PlayViewCommon.h"

#include "mcp/McpAutoSync.h"
#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"
#include "util/SafeWrite.h"

#include "App.h"
#include "AppSettings.h"
#include "Catalog.h"
#include "Factory.h"
#include "Generator.h"
#include "Out.h"
#include "Project.h"
#include "asset/AssetTypes.h"
#include "util/ExternalCodeEditor.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/CommandNames.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/ScriptComponent.h"
#include "component/UIDocumentComponent.h"
#include "htmlui/HtmlUIDependencies.h"
#include "pool/TextureDataPool.h"
#include "pool/TexturePool.h"
#include "script/LuaBinding.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/UIDocumentSystem.h"
#include "texture/Material.h"
#include "texture/Texture.h"
#include "util/FileUtils.h"
#include "io/watch/FileWatchService.h"
#include "util/ProjectChecks.h"
#include "vat/VatBakeSession.h"
#include "animation/VertexAnimation.h"
#include "util/ProjectUtils.h"
#include "util/ScriptParser.h"
#include "util/Util.h"
#include "window/CodeEditor.h"
#include "window/ProjectWindow.h"
#include "reload/HotReloadService.h"
#include "reload/handlers/ReloadHandlers.h"
#include "window/dialog/ScriptCreateDialog.h"
#include "util/ScriptTemplates.h"

#include "lua.hpp"
#include "util/FileUtils.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

namespace jasperengine::editor::mcp {

namespace fs = std::filesystem;

// ======================================================================== shared (W4Shared.h)

namespace w4 {

// A failed write keeps the file as it was
bool writeFile(const fs::path& path, const std::string& content) {
    // agent writes are user documents: atomic, synced, never over a read-only file
    const io::WriteResult written = DocumentFiles::write(path, content);
    if (!written.ok()) {
        Out::error(DocumentFiles::saveFailureMessage(path, written));
    }
    return written.ok();
}

std::string projectRelative(Project* project, const fs::path& absolute) {
    // UTF-8 for the agent (generic_string() is the ANSI code page on Windows)
    if (!project) return FileUtils::pathToGenericUtf8(absolute);
    const fs::path rel = absolute.lexically_normal().lexically_relative(project->getProjectPath().lexically_normal());
    if (rel.empty() || *rel.begin() == "..") return FileUtils::pathToGenericUtf8(absolute);
    if (rel == ".") return "";
    return FileUtils::pathToGenericUtf8(rel);
}

std::string extensionOf(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::string assetType(const fs::path& path) {
    const std::string p = path.string();
    if (Util::isImageFile(p)) return "texture";
    if (Util::isColorLutFile(p)) return "color_lut";
    if (Util::isParticleEffectFile(p)) return "particle_effect";
    if (Util::isModelFile(p)) return "model";
    if (Util::isAudioFile(p)) return "audio";
    // .webm, .mkv, .mp4, .m4v, .mov (the probe decides what plays where)
    if (jasperengine::VideoFormats::isPlayableExtension(p)) return "video";
    if (Util::isFontFile(p)) return "font";
    if (Util::isHtmlFile(p)) return "html";
    if (Util::isCssFile(p)) return "css";
    if (Util::isScriptFile(p)) return "script";
    if (Util::isSceneFile(p)) return "scene";
    if (Util::isPrefabFile(p)) return "prefab";
    if (Util::isMaterialFile(p)) return "material";
    if (Util::isShaderFile(p)) return "shader";
    // clips (.anim) and pose animations (.poseanim), spec 9.R47
    if (Util::isAnimationClipFile(p) || Util::isPoseAnimationFile(p) || Util::isAnimatorAssetFile(p)) return "animation";
    // Vertex Animations (.vat; the generated .vat.glb is a model)
    if (Util::isVertexAnimationFile(p)) return "vertex_animation";
    // Shake Presets (.shake, camera shake spec 6)
    if (Util::isShakePresetFile(p)) return "shake";
    return "other";
}

bool isTextFile(const fs::path& path) {
    static const std::set<std::string> text = {
        ".lua", ".cpp", ".cc", ".cxx", ".c", ".h", ".hh", ".hpp", ".hxx", ".inl",
        ".material", ".scene", ".prefab", ".yaml", ".yml", ".json", ".txt", ".md", ".csv", ".ini", ".cfg",
        ".glsl", ".vert", ".frag", ".hlsl", ".cmake", ".xml", ".gltf", ".obj", ".mtl", ".svg", ".tsx", ".tmx",
        ".html", ".htm", ".css", ".rml", ".rcss", ".anim", ".poseanim", ".vat", ".mask", ".controller", ".shake"
    };
    if (path.filename() == "CMakeLists.txt" || path.filename() == "AGENTS.md") return true;
    return text.count(extensionOf(path)) > 0;
}

bool isEditorOwnedFile(Project* project, const fs::path& absolute, std::string& why) {
    // hidden folders, project.yaml (any folder), CMakeLists.txt: as named and where it resolves,
    // names compared as the file system may (letter case, Windows aliases)
    if (const std::string place = McpRefs::protectedPlace(project, absolute); !place.empty()) {
        why = projectRelative(project, absolute) + ": " + place + ".";
        return true;
    }
    // scenes and prefabs, also through a link or under an alias ("a.SCENE.")
    for (const fs::path& name : {absolute, safewrite::resolvedPath(absolute)}) {
        if (name.empty()) continue;
        const std::string ext = extensionOf(fs::path(safewrite::comparableName(name)));
        if (ext == ".scene" || ext == ".prefab") {
            why = ext + " files are written by the editor from its in-memory state: edit them with the scene, entity and prefab tools.";
            return true;
        }
    }
    return false;
}

bool writeFilesUndoable(McpContext& ctx, std::vector<WriteFilesCmd::Item> items, std::string& error,
                        std::vector<fs::path> alsoSync) {
    for (const WriteFilesCmd::Item& item : items) {
        std::error_code ec;
        if (fs::is_directory(item.path, ec)) {
            error = FileUtils::pathToGenericUtf8(item.path) + " is a directory.";
            return false;
        }
    }
    McpContext* context = &ctx;
    auto* cmd = new WriteFilesCmd(std::move(items), [context, alsoSync](const std::vector<fs::path>& changed) {
        std::vector<fs::path> files = changed;
        files.insert(files.end(), alsoSync.begin(), alsoSync.end());
        syncFiles(*context, files, false);
    });
    cmd->setNoMerge();
    auto failure = std::make_shared<std::string>();
    cmd->reportFailureTo(failure);
    // addCommand runs execute() and deletes the command when it fails (execute then changed nothing)
    if (!ctx.project()->getProjectCommandHistory()->addCommand(cmd)) {
        error = "Writing the file(s) failed: " +
                (failure->empty() ? std::string("permissions, a read-only disk, or a parent path that is a file") : *failure) + ".";
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ syncFiles

namespace {

struct FileStamp {
    fs::file_time_type time{};
    std::uintmax_t size = 0;
};

std::mutex snapshotMutex;
fs::path snapshotProject;
std::map<std::string, FileStamp> snapshot;   // project-relative path -> stamp
bool snapshotValid = false;
uint64_t editorWritesCursor = 0;             // the file watcher's journal, read up to here

std::map<std::string, FileStamp> scanProject(Project* project) {
    std::map<std::string, FileStamp> files;
    const fs::path root = project->getProjectPath();
    std::error_code ec;
    for (watch::LinkedDirectoryIterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (it->is_directory(ec)) {
            if (ProjectChecks::isSkippedDir(it->path())) it.disable_recursion_pending();
            continue;
        }
        const std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        if (!it->is_regular_file(ec)) continue;
        FileStamp stamp;
        stamp.time = it->last_write_time(ec);
        stamp.size = it->file_size(ec);
        files[projectRelative(project, it->path())] = stamp;
    }
    return files;
}

// Takes the new stamps of `paths`; with the lists, says which of them were added, modified or
// removed since the snapshot (a path the snapshot already has with the same stamp: neither)
void updateSnapshotEntries(Project* project, const std::vector<fs::path>& paths, Json* added = nullptr,
                           Json* modified = nullptr, Json* removed = nullptr) {
    std::lock_guard<std::mutex> lock(snapshotMutex);
    if (!snapshotValid || snapshotProject != project->getProjectPath()) return;
    std::set<std::string> seen;
    for (const fs::path& path : paths) {
        const std::string rel = projectRelative(project, path);
        if (!seen.insert(rel).second) continue;
        const bool inside = !rel.empty() && rel != FileUtils::pathToGenericUtf8(path);
        std::error_code ec;
        if (fs::is_regular_file(path, ec)) {
            FileStamp stamp;
            stamp.time = fs::last_write_time(path, ec);
            stamp.size = fs::file_size(path, ec);
            auto it = snapshot.find(rel);
            if (inside && added && it == snapshot.end()) added->push_back(rel);
            else if (inside && modified && it != snapshot.end() && (it->second.time != stamp.time || it->second.size != stamp.size)) {
                modified->push_back(rel);
            }
            snapshot[rel] = stamp;
        } else {
            if (inside && removed && snapshot.erase(rel)) removed->push_back(rel);
            snapshot.erase(rel);
            // a removed directory: forget what it held
            const std::string prefix = rel + "/";
            for (auto it = snapshot.lower_bound(prefix); it != snapshot.end() && it->first.rfind(prefix, 0) == 0;) {
                if (inside && removed && !fs::is_directory(path, ec)) removed->push_back(it->first);
                it = snapshot.erase(it);
            }
        }
    }
}

// The editor's own writes since the last look (announced with ScopedExpectedWrite, S05 R10: the
// watcher's journal has them with origin Editor; an inferred texture import setting written
// into a meta after the project opened, say): the snapshot takes their stamps, so a full sync
// does not list them as changed. Only while the file still has what the editor wrote (an edit
// made after it stays a change), and only the files scanProject looks at.
void absorbEditorWrites(Project* project) {
    FileWatchService& watcher = FileWatchService::get();
    if (!watcher.isAttached()) return;
    uint64_t cursor = 0;
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        if (!snapshotValid || snapshotProject != project->getProjectPath()) return;
        cursor = editorWritesCursor;
    }
    const std::vector<FileChange> journal = watcher.journalSince(cursor);
    std::lock_guard<std::mutex> lock(snapshotMutex);
    editorWritesCursor = cursor;
    if (!snapshotValid || snapshotProject != project->getProjectPath()) return;
    // the project-relative path of a file scanProject lists, else empty
    auto scanned = [&](const fs::path& path) {
        const std::string rel = projectRelative(project, path);
        const fs::path relative = FileUtils::pathFromUtf8(rel);
        const std::string name = relative.filename().string();
        if (rel.empty() || relative.is_absolute() || *relative.begin() == ".." || ProjectChecks::isInSkippedDir(relative) ||
            name.empty() || name[0] == '.') {
            return std::string();
        }
        return rel;
    };
    for (const FileChange& change : journal) {
        if (change.origin != ChangeOrigin::Editor || change.directory || change.kind == FileChangeKind::Rescan) continue;
        watch::PathStat now;
        const bool present = watch::statPath(change.path, now, false);
        if (change.kind == FileChangeKind::Moved) {
            const std::string from = scanned(change.oldPath);
            watch::PathStat old;
            if (!from.empty() && !watch::statPath(change.oldPath, old, false)) snapshot.erase(from);
        }
        const std::string rel = scanned(change.path);
        if (rel.empty()) continue;
        if (change.kind == FileChangeKind::Removed) {
            if (!present) snapshot.erase(rel);
            continue;
        }
        if (!present || now.directory || now.size != change.size || now.mtimeNs != change.mtimeNs) continue;
        std::error_code ec;
        FileStamp stamp;
        stamp.time = fs::last_write_time(change.path, ec);
        stamp.size = fs::file_size(change.path, ec);
        if (!ec) snapshot[rel] = stamp;
    }
}

} // namespace

void takeSyncSnapshot(Project* project, const std::set<std::string>& changedAlready) {
    if (!project || project->getProjectPath().empty()) return;
    std::map<std::string, FileStamp> now = scanProject(project);
    // changed already (since the server started): a full sync lists them, unless the
    // automatic pick-up reports them first
    for (const std::string& rel : changedAlready) {
        auto it = now.find(rel);
        if (it != now.end()) it->second.size = static_cast<std::uintmax_t>(-1);
    }
    std::lock_guard<std::mutex> lock(snapshotMutex);
    snapshot = std::move(now);
    snapshotProject = project->getProjectPath();
    snapshotValid = true;
    editorWritesCursor = FileWatchService::get().journalHead();
}

bool isHtmlUiFile(const std::string& path) {
    return reload::isHtmlUiFile(path);
}

Json syncHtmlUi(McpContext& ctx, const std::vector<fs::path>& changed) {
    return reload::syncHtmlUi(ctx.project(), changed);
}

// SYNC4: upstream (camera-shake) reports an invalid .shake as a sync problem; on our S05 funnel that
// belongs in editor/reload (classification + reload::addAnimationFileProblems), not here yet.
Json syncFiles(McpContext& ctx, const std::vector<fs::path>& changedIn, bool full, const SyncOptions& sync) {
    Project* project = ctx.project();
    Json notes = Json::array();
    Json added = Json::array(), modified = Json::array(), removed = Json::array();

    std::set<fs::path> changed;
    for (const fs::path& path : changedIn) changed.insert(path.lexically_normal());

    // the snapshot of a project opened since the last frame
    McpAutoSync::get().prepare(ctx);

    // What changed on disk since the last sync (full syncs scan the project)
    if (full) {
        absorbEditorWrites(project);
        std::map<std::string, FileStamp> now = scanProject(project);
        std::lock_guard<std::mutex> lock(snapshotMutex);
        if (snapshotValid && snapshotProject == project->getProjectPath()) {
            for (const auto& [rel, stamp] : now) {
                auto it = snapshot.find(rel);
                if (it == snapshot.end()) {
                    added.push_back(rel);
                    changed.insert((project->getProjectPath() / rel).lexically_normal());
                } else if (it->second.time != stamp.time || it->second.size != stamp.size) {
                    modified.push_back(rel);
                    changed.insert((project->getProjectPath() / rel).lexically_normal());
                }
            }
            for (const auto& [rel, stamp] : snapshot) {
                if (now.find(rel) == now.end()) {
                    removed.push_back(rel);
                    changed.insert((project->getProjectPath() / rel).lexically_normal());
                }
            }
        } else {
            notes.push_back("First sync of this session: no earlier snapshot to compare, so every attached script was "
                            "re-parsed and shader forks were invalidated; later syncs list exactly what changed.");
        }
        snapshot = std::move(now);
        snapshotProject = project->getProjectPath();
        snapshotValid = true;
    } else {
        updateSnapshotEntries(project, changedIn, &added, &modified, &removed);
    }
    const bool firstFull = full && !notes.empty();
    if (firstFull) project->invalidateCustomShaders();

    // the one reload funnel (S05 D3): the watcher, Ctrl+R and McpAutoSync come here too
    ApplyOptions options;
    options.full = firstFull;
    options.fromMcp = true;
    options.force = sync.force;
    options.rebuildModels = sync.rebuildModels;
    // a tool's own writes reload what reads them, quietly
    options.quiet = !sync.external;
    ChangeSet set = ChangeSet::fromPaths(std::vector<fs::path>(changed.begin(), changed.end()));
    for (FileChange& change : set.changes) {
        auto it = sync.sequences.find(change.path);
        if (it != sync.sequences.end()) change.sequence = it->second;
    }
    const ApplyResult applied = HotReloadService::get().apply(set, options);
    // the schema changes the watcher found when it reloaded an edited script before this sync
    // (a change it reloaded already is not reloaded again: its report comes from the watcher)
    Json scriptProperties = applied.scriptProperties;
    for (const Json& item : HotReloadService::get().takeLateScriptProperties()) scriptProperties.push_back(item);

    Json allNotes = notes;
    for (const Json& note : applied.notes) allNotes.push_back(note);
    // a changed .vat that doesn't match its .vat.glb does not load (vat-layout-mismatch,
    // vat-mesh-mismatch): reported whether this call or the watcher reloaded it (upstream's
    // vatFileChanged; the animation handler reports it with the reload it runs)
    Json problems = applied.problems;
    // and the sprite normal maps made from a changed image (their recipe's source SHA-1 no longer
    // matches: the editor's watcher marks them stale, its badge and check --normals say so)
    if (Project* project = ctx.project()) {
        for (const fs::path& path : changed) spriteNormalsFileChanged(project, path, problems);
    }
    if (Project* project = ctx.project()) {
        for (const fs::path& path : changed) {
            const std::string text = FileUtils::pathToUtf8(path);
            std::error_code ec;
            if (!Util::isVertexAnimationFile(text) || !fs::is_regular_file(path, ec)) continue;
            std::string content;
            if (!ProjectChecks::readFile(path, content) || !::jasperengine::vat::parseVat(content).ok()) continue;
            const std::string rel = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(path));
            for (const ::jasperengine::editor::vat::VatIssue& issue : ::jasperengine::editor::vat::checkVatStaleness(project, rel)) {
                if (!issue.error) continue;
                const Json problem = {{"path", rel}, {"message", issue.kind + ": " + issue.message + ". Fix: " + issue.fix + "."}};
                if (std::find(problems.begin(), problems.end(), problem) == problems.end()) problems.push_back(problem);
            }
        }
    }
    Json result = {
        {"changed", {{"added", added}, {"modified", modified}, {"removed", removed}}},
        {"code_editor", applied.codeEditor},
        {"script_properties", scriptProperties},
        {"problems", problems},
        {"notes", allNotes},
        {"reloaded", applied.reloaded},
        {"pending", applied.pending}
    };
    if (applied.texturesReloaded > 0) result["textures_reloaded"] = applied.texturesReloaded;
    if (applied.shadersInvalidated) result["shaders_invalidated"] = true;
    if (!applied.uiReloaded.empty()) result["ui_reloaded"] = applied.uiReloaded;
    if (applied.uiCurrent > 0) result["ui_current"] = applied.uiCurrent;
    // Handled now: not an external change for the automatic pick-up before the next call
    McpAutoSync::get().filesSynced(ctx, std::vector<fs::path>(changed.begin(), changed.end()), full);
    return result;
}

} // namespace w4

// ======================================================================== script tools

namespace {

using namespace w4;

// ------------------------------------------------------------------ helpers

struct Target {
    SceneProject* scene = nullptr;
    Entity entity = NULL_ENTITY;
};

bool resolveTarget(const CallPtr& call, Target& target, bool entityRequired = true) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    target.scene = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!target.scene) {
        call->fail(error);
        return false;
    }
    if (!target.scene->scene) {
        call->fail("Scene \"" + target.scene->name + "\" is not loaded; open it first.");
        return false;
    }
    if (!args.contains("entity") || args["entity"].is_null()) {
        if (entityRequired) {
            call->fail("entity is required: a GameObject id or path (\"Player\", \"Level/Enemy#2\").");
            return false;
        }
        return true;
    }
    target.entity = McpRefs::resolveEntity(target.scene, args["entity"], error);
    if (target.entity == NULL_ENTITY) {
        call->fail(error);
        return false;
    }
    return true;
}

std::string scriptPropertyTypeName(const ScriptProperty& prop) {
    switch (prop.type) {
        case ScriptPropertyType::Bool: return "bool";
        case ScriptPropertyType::Int: return "int";
        case ScriptPropertyType::Float: return "float";
        case ScriptPropertyType::String: return "string";
        case ScriptPropertyType::Vector2: return "vector2";
        case ScriptPropertyType::Vector3: return "vector3";
        case ScriptPropertyType::Vector4: return "vector4";
        case ScriptPropertyType::Color3: return "color3";
        case ScriptPropertyType::Color4: return "color4";
        case ScriptPropertyType::EntityReference:
            return prop.ptrTypeName.empty() ? "entity" : "entity:" + prop.ptrTypeName;
        case ScriptPropertyType::Prefab: return "prefab";
        case ScriptPropertyType::Asset: return prop.assetType.empty() ? "asset" : "asset:" + prop.assetType;
    }
    return "unknown";
}

// Project-relative files of an entry: path (the .lua or .cpp) and header
std::string entryPath(Project* project, const ScriptEntry& entry) {
    if (entry.type == ScriptType::LUA) {
        if (entry.path.empty()) return "";
        return projectRelative(project, project->resolveLuaPath(entry.path));
    }
    fs::path source = entry.path;
    if (source.is_relative()) source = project->getProjectPath() / source;
    return entry.path.empty() ? "" : projectRelative(project, source);
}

std::string entryHeader(Project* project, const ScriptEntry& entry) {
    if (entry.headerPath.empty()) return "";
    fs::path header = entry.headerPath;
    if (header.is_relative()) header = project->getProjectPath() / header;
    return projectRelative(project, header);
}

Json scriptEntryJson(Project* project, SceneProject* sceneProject, Entity entity, size_t index, const ScriptEntry& entry,
                     bool withProperties) {
    Json item = {
        {"index", index},
        {"type", entry.type == ScriptType::LUA ? "lua" : "cpp"},
        {"class_name", entry.className},
        {"path", entryPath(project, entry)},
        {"enabled", entry.enabled}
    };
    if (entry.type == ScriptType::CPP) item["header_path"] = entryHeader(project, entry);
    if (withProperties) {
        Json props = Json::array();
        McpValues::ReadContext ctx;
        ctx.project = project;
        ctx.scene = sceneProject;
        ctx.entity = entity;
        ctx.component = ComponentType::ScriptComponent;
        for (const ScriptProperty& prop : entry.properties) {
            const std::string key = "scripts[" + std::to_string(index) + "]." + prop.name;
            PropertyData data = Catalog::getProperty(sceneProject->scene, entity, ComponentType::ScriptComponent, key);
            Json p = {{"name", prop.name}, {"type", scriptPropertyTypeName(prop)}, {"property", key}};
            if (prop.displayName != prop.name && !prop.displayName.empty()) p["display_name"] = prop.displayName;
            if (data.ref) {
                try {
                    p["value"] = McpValues::readProperty(ctx, key, data);
                } catch (...) {
                }
            }
            props.push_back(p);
        }
        item["properties"] = props;
    }
    return item;
}

// `script` selector: index, class name, or project-relative file path
bool resolveScriptIndex(Project* project, const std::vector<ScriptEntry>& scripts, const Json& selector, size_t& index,
                        std::string& error) {
    auto listing = [&]() {
        std::string text;
        for (size_t i = 0; i < scripts.size(); i++) {
            text += (i ? ", " : "") + std::to_string(i) + " = " + scripts[i].className + " (" + entryPath(project, scripts[i]) + ")";
        }
        return text.empty() ? std::string("none") : text;
    };
    if (scripts.empty()) {
        error = "The GameObject has no scripts.";
        return false;
    }
    if (selector.is_null()) {
        if (scripts.size() == 1) {
            index = 0;
            return true;
        }
        error = "The GameObject has " + std::to_string(scripts.size()) + " scripts; pass script (index, class name or path): " + listing() + ".";
        return false;
    }
    if (selector.is_number_integer()) {
        const long long i = selector.get<long long>();
        if (i < 0 || static_cast<size_t>(i) >= scripts.size()) {
            error = "script index " + std::to_string(i) + " is out of range. Scripts: " + listing() + ".";
            return false;
        }
        index = static_cast<size_t>(i);
        return true;
    }
    if (!selector.is_string()) {
        error = "script must be an index, a class name or a path.";
        return false;
    }
    const std::string text = selector.get<std::string>();
    std::vector<size_t> matches;
    for (size_t i = 0; i < scripts.size(); i++) {
        if (scripts[i].className == text || entryPath(project, scripts[i]) == text || entryHeader(project, scripts[i]) == text) {
            matches.push_back(i);
        }
    }
    if (matches.size() == 1) {
        index = matches[0];
        return true;
    }
    error = matches.empty() ? "No script \"" + text + "\" on the GameObject. Scripts: " + listing() + "."
                            : "\"" + text + "\" matches several scripts; pass the index. Scripts: " + listing() + ".";
    return false;
}

bool addSceneCommand(uint32_t sceneId, Command* cmd) {
    cmd->setNoMerge();
    return CommandHandle::get(sceneId)->addCommand(cmd);
}

std::string luaRootRelative(Project* project) {
    const std::string rel = projectRelative(project, project->getLuaPath());
    return rel.empty() ? "." : rel;
}

// A ScriptEntry for a script file (absolute path of a .lua, .h/.hpp or .cpp)
bool makeEntry(Project* project, const fs::path& file, const std::string& classNameArg, ScriptEntry& entry, Json& warnings,
               std::string& error) {
    const std::string ext = extensionOf(file);
    std::error_code ec;
    entry = ScriptEntry();
    entry.enabled = true;
    if (ext == ".lua") {
        if (!Util::isInsidePath(file, project->getLuaPath())) {
            error = "Lua scripts must be inside the project's Lua directory \"" + luaRootRelative(project) +
                    "\" (Project Settings > Directories); move the file there first (move_asset).";
            return false;
        }
        entry.type = ScriptType::LUA;
        entry.path = project->normalizeToLuaRelative(file).generic_string();
        entry.className = file.stem().string();
        if (!classNameArg.empty() && classNameArg != entry.className) {
            warnings.push_back("A Lua script's name is its file name: using \"" + entry.className + "\".");
        }
        return true;
    }
    fs::path header, source;
    if (Util::isHeaderFile(file.string())) {
        header = file;
        for (const char* candidate : {".cpp", ".cc", ".cxx"}) {
            fs::path s = file;
            s.replace_extension(candidate);
            if (fs::is_regular_file(s, ec)) {
                source = s;
                break;
            }
        }
        if (source.empty()) {
            error = "No source file next to " + projectRelative(project, file) + " (expected " + FileUtils::pathToUtf8(file.stem()) + ".cpp).";
            return false;
        }
    } else if (Util::isSourceFile(file.string())) {
        source = file;
        for (const char* candidate : {".h", ".hpp", ".hh", ".hxx"}) {
            fs::path h = file;
            h.replace_extension(candidate);
            if (fs::is_regular_file(h, ec)) {
                header = h;
                break;
            }
        }
        if (header.empty()) {
            error = "No header next to " + projectRelative(project, file) + " (expected " + FileUtils::pathToUtf8(file.stem()) + ".h).";
            return false;
        }
    } else {
        error = "path must be a .lua script or a C++ script (.h/.hpp header or .cpp source).";
        return false;
    }
    entry.type = ScriptType::CPP;
    // Stored as the editor stores them (projectRelative is the agent's UTF-8)
    entry.headerPath = ProjectChecks::projectRelative(project->getProjectPath(), header);
    entry.path = ProjectChecks::projectRelative(project->getProjectPath(), source);
    std::string className = classNameArg;
    if (className.empty()) {
        if (auto found = ScriptParser::findScriptClassName(header)) className = *found;
    }
    if (className.empty()) {
        error = "Couldn't tell the script class in " + entry.headerPath + " (several or none); pass class_name.";
        return false;
    }
    if (!ScriptParser::inheritsScriptBase(header, className).has_value()) {
        warnings.push_back("Class " + className + " was not found in " + entry.headerPath + "; the build will fail until it exists.");
    }
    entry.className = className;
    return true;
}

// Attaches `entry` (AddComponent if needed) as one undo step. index < 0 appends.
bool attachEntry(const CallPtr& call, SceneProject* sceneProject, Entity entity, ScriptEntry entry, long long position,
                 size_t& outIndex, std::string& error) {
    Project* project = call->ctx().project();
    ScriptComponent* component = sceneProject->scene->findComponent<ScriptComponent>(entity);
    std::vector<ScriptEntry> scripts = component ? component->scripts : std::vector<ScriptEntry>();
    for (size_t i = 0; i < scripts.size(); i++) {
        const bool samePath = entryPath(project, scripts[i]) == entryPath(project, entry);
        if (samePath && scripts[i].type == entry.type) {
            error = "The GameObject already has " + entry.className + " (script " + std::to_string(i) +
                    "); change it with update_script_entry or set_script_properties.";
            return false;
        }
    }
    size_t index = scripts.size();
    if (position >= 0 && static_cast<size_t>(position) < scripts.size()) index = static_cast<size_t>(position);
    scripts.insert(scripts.begin() + static_cast<std::ptrdiff_t>(index), entry);
    project->updateScriptProperties(sceneProject, entity, scripts);

    auto* multi = new MultiPropertyCmd();
    if (!component) {
        multi->addCommand(std::make_unique<AddComponentCmd>(project, sceneProject->id, entity, ComponentType::ScriptComponent));
    }
    multi->addCommand(std::make_unique<PropertyCmd<std::vector<ScriptEntry>>>(
        project, sceneProject->id, entity, ComponentType::ScriptComponent, "scripts", scripts));
    if (!addSceneCommand(sceneProject->id, multi)) {
        error = "The editor refused the change (is the GameObject part of a prefab instance that can't take it?).";
        return false;
    }
    outIndex = index;
    return true;
}

Json lintFile(Project* project, const fs::path& file) {
    Json problems = Json::array();
    std::string code;
    if (Util::isLuaFile(file.string()) && ProjectChecks::readFile(file, code)) {
        ProjectChecks::LuaSyntax syntax = ProjectChecks::checkLuaSyntax(code, projectRelative(project, file));
        if (!syntax.ok) {
            problems.push_back({{"path", projectRelative(project, file)}, {"line", syntax.line}, {"message", syntax.message}});
        }
    }
    return problems;
}

// ------------------------------------------------------------------ create_script

void createScriptTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    const std::string kind = args.value("kind", "");
    ScriptCreateDialog::CreationKind creation;
    if (kind == "lua") creation = ScriptCreateDialog::CreationKind::LUA;
    else if (kind == "cpp_script") creation = ScriptCreateDialog::CreationKind::CPP_SCRIPT_CLASS;
    else if (kind == "cpp_subclass") creation = ScriptCreateDialog::CreationKind::CPP_SUBCLASS;
    else {
        call->fail("kind must be lua, cpp_script (a ScriptBase subclass) or cpp_subclass (a subclass of the GameObject's own class).");
        return;
    }
    const bool lua = creation == ScriptCreateDialog::CreationKind::LUA;

    const std::string requested = args.value("name", "");
    if (requested.empty()) {
        call->fail("name is required (the class / Lua module name, e.g. \"PlayerController\").");
        return;
    }
    const std::string name = Factory::toIdentifier(requested);
    Json warnings = Json::array();
    if (name != requested) warnings.push_back("Name \"" + requested + "\" is not a valid identifier; using \"" + name + "\".");

    Target target;
    const bool attach = args.contains("entity") && !args["entity"].is_null();
    if (attach || creation == ScriptCreateDialog::CreationKind::CPP_SUBCLASS) {
        if (!resolveTarget(call, target, true)) return;
    }

    std::string dirArg = args.value("directory", "");
    if (dirArg.empty()) dirArg = lua ? luaRootRelative(project) : "scripts";
    fs::path dir;
    std::string error;
    if (dirArg == ".") dir = project->getProjectPath();
    else if (!McpRefs::resolveProjectWritePath(project, dirArg, dir, error)) {
        call->fail(error);
        return;
    }
    dir = dir.lexically_normal();

    const fs::path header = lua ? fs::path() : dir / (name + ".h");
    const fs::path source = dir / (name + (lua ? ".lua" : ".cpp"));
    if (lua && !Util::isInsidePath(source, project->getLuaPath())) {
        call->fail("Lua scripts must be inside the project's Lua directory \"" + luaRootRelative(project) +
                   "\"; pass a directory inside it (or omit directory).");
        return;
    }
    const bool overwrite = args.value("overwrite", false);
    std::error_code ec;
    for (const fs::path& file : {header, source}) {
        if (!file.empty() && fs::exists(file, ec) && !overwrite) {
            call->fail(projectRelative(project, file) + " already exists. Pick another name, pass overwrite: true, "
                       "or attach the existing script with attach_script.");
            return;
        }
    }

    // The editor's templates (the Project window and the Inspector write the same)
    std::string headerText, sourceText;
    if (lua) {
        sourceText = ScriptTemplates::lua(name);
    } else {
        const std::string headerFileName = FileUtils::pathToUtf8(header.filename());
        const ScriptTemplates::Files files = creation == ScriptCreateDialog::CreationKind::CPP_SUBCLASS
            ? ScriptTemplates::cppSubclass(target.scene ? target.scene->scene : nullptr, target.entity, name, headerFileName)
            : ScriptTemplates::cppScript(name, headerFileName);
        headerText = files.header;
        sourceText = files.source;
    }
    if (lua && args.contains("content") && args["content"].is_string()) sourceText = args["content"].get<std::string>();
    if (!lua && args.contains("header_content") && args["header_content"].is_string()) headerText = args["header_content"].get<std::string>();
    if (!lua && args.contains("source_content") && args["source_content"].is_string()) sourceText = args["source_content"].get<std::string>();
    if (lua && (args.contains("header_content") || args.contains("source_content"))) {
        warnings.push_back("header_content/source_content are for C++ scripts; a Lua script takes content.");
    }
    if (!lua && args.contains("content")) {
        warnings.push_back("content is for Lua scripts; a C++ script takes header_content and source_content.");
    }

    std::vector<WriteFilesCmd::Item> items;
    if (!lua) items.push_back({header, headerText});
    items.push_back({source, sourceText});
    if (!writeFilesUndoable(call->ctx(), std::move(items), error)) {
        call->fail(error);
        return;
    }

    Json data = {
        {"kind", kind},
        {"class_name", name},
        {"path", projectRelative(project, source)}
    };
    if (!lua) data["header_path"] = projectRelative(project, header);
    Json problems = Json::array();
    if (lua) {
        ProjectChecks::LuaSyntax syntax = ProjectChecks::checkLuaSyntax(sourceText, projectRelative(project, source));
        if (!syntax.ok) problems.push_back({{"path", projectRelative(project, source)}, {"line", syntax.line}, {"message", syntax.message}});
    } else {
        const auto found = ScriptParser::findScriptClassNameFromString(headerText);
        if (!found || *found != name) {
            warnings.push_back("The header doesn't declare a script class named " + name +
                               (found ? " (found " + *found + ")" : std::string()) + ".");
        }
    }

    std::string text = "Created " + projectRelative(project, source) + (lua ? "" : " and " + projectRelative(project, header)) + ".";
    if (attach) {
        ScriptEntry entry;
        if (!makeEntry(project, lua ? source : header, name, entry, warnings, error)) {
            call->fail(text + " Not attached: " + error, data);
            return;
        }
        size_t index = 0;
        if (!attachEntry(call, target.scene, target.entity, entry, -1, index, error)) {
            call->fail(text + " Not attached: " + error, data);
            return;
        }
        ScriptComponent& component = target.scene->scene->getComponent<ScriptComponent>(target.entity);
        data["entity"] = McpRefs::entityJson(target.scene, target.entity);
        data["script"] = scriptEntryJson(project, target.scene, target.entity, index, component.scripts[index], true);
        text += " Attached to " + McpRefs::entityPath(target.scene, target.entity) + " as script " + std::to_string(index) + ".";
    } else if (!lua) {
        text += " Not attached: a C++ file compiles only when attached to a GameObject or inside a script directory.";
    }
    if (!lua) text += " Check it compiles with compile_scripts.";
    if (!problems.empty()) text += " SYNTAX ERROR: " + problems[0]["message"].get<std::string>() + " (line " + std::to_string(problems[0]["line"].get<int>()) + ").";
    data["problems"] = problems;
    if (!warnings.empty()) data["warnings"] = warnings;
    if (ProjectWindow* window = call->ctx().projectWindow()) window->refreshCurrentDirectory();
    call->ok(text, data);
}

// ------------------------------------------------------------------ attach / detach / update

void attachScriptTool(const CallPtr& call) {
    Target target;
    if (!resolveTarget(call, target)) return;
    Project* project = call->ctx().project();
    const Json& args = call->args();
    fs::path file;
    std::string error;
    if (!McpRefs::resolveProjectPath(project, args.value("path", ""), file, error, true)) {
        call->fail(error);
        return;
    }
    Json warnings = Json::array();
    ScriptEntry entry;
    if (!makeEntry(project, file.lexically_normal(), args.value("class_name", ""), entry, warnings, error)) {
        call->fail(error);
        return;
    }
    entry.enabled = args.value("enabled", true);
    size_t index = 0;
    const long long position = args.contains("index") && args["index"].is_number_integer() ? args["index"].get<long long>() : -1;
    if (!attachEntry(call, target.scene, target.entity, entry, position, index, error)) {
        call->fail(error);
        return;
    }
    ScriptComponent& component = target.scene->scene->getComponent<ScriptComponent>(target.entity);
    Json data = {
        {"entity", McpRefs::entityJson(target.scene, target.entity)},
        {"script", scriptEntryJson(project, target.scene, target.entity, index, component.scripts[index], true)},
        {"problems", lintFile(project, file)}
    };
    if (!warnings.empty()) data["warnings"] = warnings;
    call->ok("Attached " + entry.className + " to " + McpRefs::entityPath(target.scene, target.entity) + " as script " +
             std::to_string(index) + ".", data);
}

void detachScriptTool(const CallPtr& call) {
    Target target;
    if (!resolveTarget(call, target)) return;
    Project* project = call->ctx().project();
    ScriptComponent* component = target.scene->scene->findComponent<ScriptComponent>(target.entity);
    std::vector<ScriptEntry> scripts = component ? component->scripts : std::vector<ScriptEntry>();
    size_t index = 0;
    std::string error;
    if (!resolveScriptIndex(project, scripts, call->args().contains("script") ? call->args()["script"] : Json(), index, error)) {
        call->fail(error);
        return;
    }
    const std::string className = scripts[index].className;
    scripts.erase(scripts.begin() + static_cast<std::ptrdiff_t>(index));
    if (!addSceneCommand(target.scene->id, new PropertyCmd<std::vector<ScriptEntry>>(
            project, target.scene->id, target.entity, ComponentType::ScriptComponent, "scripts", scripts))) {
        call->fail("The editor refused the change.");
        return;
    }
    Json remaining = Json::array();
    for (size_t i = 0; i < scripts.size(); i++) remaining.push_back(scriptEntryJson(project, target.scene, target.entity, i, scripts[i], false));
    call->ok("Detached " + className + " from " + McpRefs::entityPath(target.scene, target.entity) + " (files kept).",
             Json{{"entity", McpRefs::entityJson(target.scene, target.entity)}, {"scripts", remaining}});
}

// While playing only `enabled` changes, on the running GameObject: onEnable / onDisable at once
// (as the Inspector checkbox), undone with the other play-time edits at Stop
void updateScriptEntryInPlay(const CallPtr& call) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    for (auto it = args.begin(); it != args.end(); ++it) {
        if (it.key() != "scene" && it.key() != "entity" && it.key() != "script" && it.key() != "enabled") {
            call->fail("While playing, update_script_entry changes only enabled (it runs onEnable / onDisable); stop Play "
                       "(play action \"stop\") to change " + it.key() + ".");
            return;
        }
    }
    if (!args.contains("enabled") || !args["enabled"].is_boolean()) {
        call->fail("Nothing to change: pass enabled (true/false); while playing it is the only field.");
        return;
    }
    std::string error;
    SceneProject* sp = w3::resolveRuntimeScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!sp) {
        call->fail(error);
        return;
    }
    const Entity entity = args.contains("entity") ? w3::resolveLiveEntity(project, sp, args["entity"], error) : NULL_ENTITY;
    if (entity == NULL_ENTITY) {
        call->fail(args.contains("entity") ? error : "entity is required: a GameObject id or path.");
        return;
    }
    ScriptComponent* component = sp->scene->findComponent<ScriptComponent>(entity);
    std::vector<ScriptEntry> scripts = component ? component->scripts : std::vector<ScriptEntry>();
    size_t index = 0;
    if (!resolveScriptIndex(project, scripts, args.contains("script") ? args["script"] : Json(), index, error)) {
        call->fail(error);
        return;
    }
    const std::string key = "scripts[" + std::to_string(index) + "].enabled";
    if (!addSceneCommand(sp->id, new PropertyCmd<bool>(project, sp->id, entity, ComponentType::ScriptComponent, key,
                                                      args["enabled"].get<bool>()))) {
        call->fail("The editor refused the change.");
        return;
    }
    ScriptComponent* updated = sp->scene->isEntityCreated(entity) ? sp->scene->findComponent<ScriptComponent>(entity) : nullptr;
    Json list = Json::array();
    for (size_t i = 0; updated && i < updated->scripts.size(); i++) {
        list.push_back(scriptEntryJson(project, sp, entity, i, updated->scripts[i], i == index));
    }
    Json data = {{"entity", McpRefs::entityJson(sp, entity)}, {"scripts", list}};
    call->ok("Set script " + std::to_string(index) + " enabled = " + (args["enabled"].get<bool>() ? "true" : "false") +
             " on the live GameObject (onEnable / onDisable ran). Play-time change: reverted when play stops.", data);
}

void updateScriptEntryTool(const CallPtr& call) {
    if (call->ctx().project()->isAnyScenePlaying()) {
        updateScriptEntryInPlay(call);
        return;
    }
    Target target;
    if (!resolveTarget(call, target)) return;
    Project* project = call->ctx().project();
    const Json& args = call->args();
    ScriptComponent* component = target.scene->scene->findComponent<ScriptComponent>(target.entity);
    std::vector<ScriptEntry> scripts = component ? component->scripts : std::vector<ScriptEntry>();
    size_t index = 0;
    std::string error;
    if (!resolveScriptIndex(project, scripts, args.contains("script") ? args["script"] : Json(), index, error)) {
        call->fail(error);
        return;
    }
    Json warnings = Json::array();
    bool changed = false;
    if (args.contains("path") && args["path"].is_string()) {
        fs::path file;
        if (!McpRefs::resolveProjectPath(project, args["path"].get<std::string>(), file, error, true)) {
            call->fail(error);
            return;
        }
        ScriptEntry repointed;
        if (!makeEntry(project, file.lexically_normal(), args.value("class_name", ""), repointed, warnings, error)) {
            call->fail(error);
            return;
        }
        if (repointed.type != scripts[index].type) {
            call->fail("path must be the same language as the script it replaces (detach it and attach the other one instead).");
            return;
        }
        scripts[index].path = repointed.path;
        scripts[index].headerPath = repointed.headerPath;
        scripts[index].className = repointed.className;
        changed = true;
    } else if (args.contains("class_name") && args["class_name"].is_string()) {
        const std::string className = Factory::toIdentifier(args["class_name"].get<std::string>());
        if (scripts[index].type == ScriptType::LUA) {
            call->fail("A Lua script's class name is its file name; rename the file (move_asset) and pass path instead.");
            return;
        }
        scripts[index].className = className;
        changed = true;
    }
    if (args.contains("enabled") && args["enabled"].is_boolean()) {
        scripts[index].enabled = args["enabled"].get<bool>();
        changed = true;
    }
    size_t newIndex = index;
    if (args.contains("move_to") && args["move_to"].is_number_integer()) {
        long long to = args["move_to"].get<long long>();
        if (to < 0 || static_cast<size_t>(to) >= scripts.size()) {
            call->fail("move_to must be 0.." + std::to_string(scripts.size() - 1) + ".");
            return;
        }
        ScriptEntry moved = scripts[index];
        scripts.erase(scripts.begin() + static_cast<std::ptrdiff_t>(index));
        scripts.insert(scripts.begin() + to, moved);
        newIndex = static_cast<size_t>(to);
        changed = true;
    }
    if (!changed) {
        call->fail("Nothing to change: pass enabled, move_to, path or class_name.");
        return;
    }
    project->updateScriptProperties(target.scene, target.entity, scripts);
    if (!addSceneCommand(target.scene->id, new PropertyCmd<std::vector<ScriptEntry>>(
            project, target.scene->id, target.entity, ComponentType::ScriptComponent, "scripts", scripts))) {
        call->fail("The editor refused the change.");
        return;
    }
    ScriptComponent& updated = target.scene->scene->getComponent<ScriptComponent>(target.entity);
    Json list = Json::array();
    for (size_t i = 0; i < updated.scripts.size(); i++) {
        list.push_back(scriptEntryJson(project, target.scene, target.entity, i, updated.scripts[i], i == newIndex));
    }
    Json data = {{"entity", McpRefs::entityJson(target.scene, target.entity)}, {"scripts", list}};
    if (!warnings.empty()) data["warnings"] = warnings;
    call->ok("Updated script " + std::to_string(newIndex) + " (" + updated.scripts[newIndex].className + ") of " +
             McpRefs::entityPath(target.scene, target.entity) + ".", data);
}

// ------------------------------------------------------------------ set_script_properties

void setScriptPropertiesTool(const CallPtr& call) {
    Target target;
    if (!resolveTarget(call, target)) return;
    Project* project = call->ctx().project();
    const Json& args = call->args();
    if (!args.contains("values") || !args["values"].is_object() || args["values"].empty()) {
        call->fail("values must be an object {\"propertyName\": value, ...}.");
        return;
    }
    ScriptComponent* component = target.scene->scene->findComponent<ScriptComponent>(target.entity);
    std::vector<ScriptEntry> scripts = component ? component->scripts : std::vector<ScriptEntry>();
    size_t index = 0;
    std::string error;
    if (!resolveScriptIndex(project, scripts, args.contains("script") ? args["script"] : Json(), index, error)) {
        call->fail(error);
        return;
    }
    const ScriptEntry& entry = scripts[index];
    auto available = [&]() {
        std::string text;
        for (const ScriptProperty& prop : entry.properties) text += (text.empty() ? "" : ", ") + prop.name + " (" + scriptPropertyTypeName(prop) + ")";
        return text.empty() ? std::string("none; declare them in the script (picked up before your next call)") : text;
    };

    auto* multi = new MultiPropertyCmd();
    size_t count = 0;
    for (auto it = args["values"].begin(); it != args["values"].end(); ++it) {
        const std::string key = (it.key() == "enabled") ? "scripts[" + std::to_string(index) + "].enabled"
                                                        : "scripts[" + std::to_string(index) + "]." + it.key();
        PropertyData data = Catalog::getProperty(target.scene->scene, target.entity, ComponentType::ScriptComponent, key);
        if (!data.ref) {
            delete multi;
            call->fail("Script " + entry.className + " has no property \"" + it.key() + "\". Properties: " + available() + ".");
            return;
        }
        Command* cmd = McpValues::buildPropertyCommand(project, target.scene->id, target.entity, ComponentType::ScriptComponent,
                                                       key, data, it.value(), error);
        if (!cmd) {
            delete multi;
            call->fail("\"" + it.key() + "\": " + error);
            return;
        }
        multi->addCommand(std::unique_ptr<Command>(cmd));
        count++;
    }
    if (!addSceneCommand(target.scene->id, multi)) {
        call->fail("The editor refused the change.");
        return;
    }
    ScriptComponent& updated = target.scene->scene->getComponent<ScriptComponent>(target.entity);
    call->ok("Set " + std::to_string(count) + " propert" + (count == 1 ? "y" : "ies") + " of " + entry.className + " on " +
             McpRefs::entityPath(target.scene, target.entity) + ".",
             Json{{"entity", McpRefs::entityJson(target.scene, target.entity)},
                  {"script", scriptEntryJson(project, target.scene, target.entity, index, updated.scripts[index], true)}});
}

// ------------------------------------------------------------------ list_scripts

void listScriptsTool(const CallPtr& call) {
    Target target;
    if (!resolveTarget(call, target, false)) return;
    Project* project = call->ctx().project();
    const Json& args = call->args();
    Json data = Json::object();
    std::string text;
    if (target.entity != NULL_ENTITY) {
        ScriptComponent* component = target.scene->scene->findComponent<ScriptComponent>(target.entity);
        Json list = Json::array();
        if (component) {
            for (size_t i = 0; i < component->scripts.size(); i++) {
                list.push_back(scriptEntryJson(project, target.scene, target.entity, i, component->scripts[i], true));
            }
        }
        data["entity"] = McpRefs::entityJson(target.scene, target.entity);
        data["scripts"] = list;
        text = McpRefs::entityPath(target.scene, target.entity) + " has " + std::to_string(list.size()) + " script(s).";
    } else {
        Json entities = Json::array();
        for (Entity entity : target.scene->entities) {
            ScriptComponent* component = target.scene->scene->findComponent<ScriptComponent>(entity);
            if (!component || component->scripts.empty()) continue;
            Json list = Json::array();
            for (size_t i = 0; i < component->scripts.size(); i++) {
                list.push_back(scriptEntryJson(project, target.scene, entity, i, component->scripts[i], false));
            }
            entities.push_back({{"entity", McpRefs::entityJson(target.scene, entity)}, {"scripts", list}});
        }
        data["scene"] = McpRefs::sceneJson(project, *target.scene);
        data["entities"] = entities;
        text = std::to_string(entities.size()) + " GameObject(s) with scripts in scene \"" + target.scene->name + "\".";
    }
    if (args.value("include_files", false)) {
        std::set<std::string> attached;
        for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
            if (!sceneProject->scene) continue;
            for (Entity entity : sceneProject->entities) {
                if (ScriptComponent* component = sceneProject->scene->findComponent<ScriptComponent>(entity)) {
                    for (const ScriptEntry& entry : component->scripts) {
                        attached.insert(entryPath(project, entry));
                        attached.insert(entryHeader(project, entry));
                    }
                }
            }
        }
        Json files = Json::array();
        for (const auto& [rel, stamp] : scanProject(project)) {
            (void)stamp;
            if (!(Util::isLuaFile(rel) || Util::isHeaderFile(rel) || Util::isSourceFile(rel))) continue;
            files.push_back({{"path", rel}, {"attached_in_loaded_scenes", attached.count(rel) > 0}});
        }
        data["files"] = files;
        data["lua_dir"] = luaRootRelative(project);
        Json dirs = Json::array();
        for (const fs::path& dir : project->getScriptDirs()) dirs.push_back(FileUtils::pathToGenericUtf8(dir));
        data["script_dirs"] = dirs;
        text += " " + std::to_string(files.size()) + " script file(s) in the project.";
    }
    call->ok(text, data);
}

// ------------------------------------------------------------------ check_script

// Lifecycle mistakes a static scan finds (script-lifecycle 8.2): Unity method names the engine
// never calls, update methods never registered, reserved property names
Json luaLifecycleWarnings(const std::string& code, const std::vector<ScriptProperty>& properties) {
    static const std::vector<std::pair<std::string, std::string>> renamed = {
        {"Awake", "init"}, {"awake", "init"}, {"Start", "start"}, {"OnEnable", "onEnable"}, {"OnDisable", "onDisable"},
        {"OnDestroy", "onDestroy"}, {"OnApplicationQuit", "onApplicationQuit"}, {"OnApplicationPause", "onApplicationPause"},
        {"OnApplicationFocus", "onApplicationFocus"},
        {"Update", "onUpdate"}, {"update", "onUpdate"}, {"FixedUpdate", "onFixedUpdate"}, {"fixedUpdate", "onFixedUpdate"},
        {"LateUpdate", "onPostUpdate"}, {"lateUpdate", "onPostUpdate"},
    };
    static const std::set<std::string> registered = {"onUpdate", "onFixedUpdate", "onPostUpdate"};
    Json warnings = Json::array();
    // without comments: a commented-out method is not one
    std::vector<std::string> lines;
    {
        std::istringstream in(code);
        std::string line;
        while (std::getline(in, line)) {
            const size_t comment = line.find("--");
            lines.push_back(comment == std::string::npos ? line : line.substr(0, comment));
        }
    }
    std::string text;
    for (const std::string& line : lines) text += line + "\n";
    static const std::regex method(R"(function\s+[A-Za-z_][A-Za-z0-9_]*\s*[:.]\s*([A-Za-z_][A-Za-z0-9_]*)\s*\()");
    for (size_t i = 0; i < lines.size(); i++) {
        std::smatch match;
        if (!std::regex_search(lines[i], match, method)) continue;
        const std::string name = match[1].str();
        for (const auto& [unity, engine] : renamed) {
            if (name != unity) continue;
            std::string message = "function " + name + " is never called by the engine: name it " + engine;
            if (registered.count(engine)) message += " and register it in init with RegisterEngineEvent(self, \"" + engine + "\")";
            warnings.push_back({{"line", (int)i + 1}, {"message", message + "."}});
        }
        if (registered.count(name) && text.find("\"" + name + "\"") == std::string::npos && text.find("'" + name + "'") == std::string::npos) {
            warnings.push_back({{"line", (int)i + 1}, {"message", name + " is defined but never registered: the engine calls it only after "
                                                                   "RegisterEngineEvent(self, \"" + name + "\") (in init)."}});
        }
    }
    for (const ScriptProperty& prop : properties) {
        if (prop.name == "enabled" || prop.name == "isActiveAndEnabled" || prop.name == "gameObject") {
            warnings.push_back({{"line", 0}, {"message", "property \"" + prop.name + "\" uses a reserved name (every script has enabled, "
                                                         "isActiveAndEnabled and gameObject): the script will not run until it is renamed."}});
        }
    }
    return warnings;
}

void checkScriptTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string code, name, language = args.value("language", "");
    fs::path file;
    if (args.contains("path") && args["path"].is_string()) {
        std::string error;
        if (!McpRefs::resolveProjectPath(project, args["path"].get<std::string>(), file, error, true)) {
            call->fail(error);
            return;
        }
        if (!ProjectChecks::readFile(file, code)) {
            call->fail("Can't read " + args["path"].get<std::string>() + ".");
            return;
        }
        name = projectRelative(project, file);
        if (language.empty()) {
            if (Util::isLuaFile(name)) language = "lua";
            else if (Util::isHeaderFile(name)) language = "cpp_header";
            else if (Util::isSourceFile(name)) language = "cpp_source";
        }
    } else if (args.contains("code") && args["code"].is_string()) {
        code = args["code"].get<std::string>();
        name = "code";
    } else {
        call->fail("Pass path (a project script file) or code (+ language).");
        return;
    }

    Json data = {{"path", name}, {"language", language}};
    if (language == "lua") {
        // Syntax, then the properties the Inspector will show: like the Inspector and the CLI, this
        // runs the chunk's top level in an isolated Lua state, not the game's
        lua_State* L = ProjectChecks::newIsolatedLuaState(project->getLuaPath(), ProjectChecks::engineLuaGlobals());
        const ProjectChecks::LuaScriptCheck check = ProjectChecks::checkLuaScript(code, name, L);
        ProjectChecks::closeIsolatedLuaState(L);
        data["ok"] = check.findings.empty();
        data["findings"] = ProjectChecks::toJson(check.findings);
        if (!check.syntaxOk) {
            const ProjectChecks::Finding& error = check.findings.front();
            data["errors"] = Json::array({{{"line", error.line}, {"message", error.message}}});
            call->ok("Lua syntax error at line " + std::to_string(error.line) + ": " + error.message, data);
            return;
        }
        Json props = Json::array();
        for (const ScriptProperty& prop : check.properties) props.push_back({{"name", prop.name}, {"type", scriptPropertyTypeName(prop)}});
        data["properties"] = props;
        const Json warnings = luaLifecycleWarnings(code, check.properties);
        std::string warningText;
        if (!warnings.empty()) {
            data["warnings"] = warnings;
            warningText = " WARNINGS:";
            for (const Json& warning : warnings) {
                const int line = warning.value("line", 0);
                warningText += "\n- " + (line > 0 ? "line " + std::to_string(line) + ": " : std::string()) + warning.value("message", std::string());
            }
        }
        if (!check.findings.empty()) {
            const ProjectChecks::Finding& error = check.findings.front();
            data["errors"] = Json::array({{{"line", error.line}, {"message", error.message}}});
            call->ok("Lua syntax OK, but loading the script failed" +
                     (error.line > 0 ? " at line " + std::to_string(error.line) : std::string()) + ": " + error.message + warningText, data);
            return;
        }
        call->ok("Lua syntax OK; " + std::to_string(props.size()) + " propert" + (props.size() == 1 ? "y" : "ies") +
                 " declared. Runtime errors only show in Play (read the Console)." + warningText, data);
        return;
    }
    if (language == "cpp_header") {
        const ProjectChecks::CppHeaderInfo header = ProjectChecks::checkCppHeader(code, name);
        data["class_name"] = header.className ? Json(*header.className) : Json(nullptr);
        if (header.directScript) data["kind"] = *header.directScript ? "cpp_script" : "cpp_subclass";
        Json props = Json::array();
        for (const ScriptProperty& prop : header.properties) {
            props.push_back({{"name", prop.name}, {"type", scriptPropertyTypeName(prop)}});
        }
        data["properties"] = props;
        call->ok(header.className ? "Script class " + *header.className + " with " + std::to_string(props.size()) +
                                        " DPROPERTY field(s). Compile with compile_scripts to check the code."
                                  : "No single script class found in the header (none, or several).", data);
        return;
    }
    if (language == "cpp_source") {
        call->ok("C++ sources are checked by compiling: call compile_scripts.", data);
        return;
    }
    call->fail("language must be lua, cpp_header or cpp_source (or pass a path with one of those extensions).");
}

// ------------------------------------------------------------------ sync_files

void syncFilesTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::vector<fs::path> paths;
    if (args.contains("paths") && args["paths"].is_array()) {
        for (const Json& item : args["paths"]) {
            if (!item.is_string()) continue;
            fs::path abs;
            std::string error;
            if (!McpRefs::resolveProjectPath(project, item.get<std::string>(), abs, error)) {
                call->fail(error);
                return;
            }
            paths.push_back(abs.lexically_normal());
        }
    }
    // Auto Refresh (Edit > Preferences > Asset Pipeline): off, external changes wait for a
    // call or Assets > Refresh
    if (args.contains("auto_refresh") && args["auto_refresh"].is_boolean()) {
        AppSettings::setAutoRefresh(args["auto_refresh"].get<bool>());
        AppSettings::saveSettings();
        HotReloadService::get().setAutoRefresh(args["auto_refresh"].get<bool>());
    }
    const bool full = args.value("full", true);
    const bool wait = args.value("wait", true);
    const bool rebuildModels = args.value("rebuild_models", false);
    HotReloadService& reloads = HotReloadService::get();
    const uint64_t serial = reloads.lastSerial();
    reloads.takeLateProblems();
    SyncOptions sync;
    sync.force = !paths.empty();
    sync.rebuildModels = rebuildModels;
    sync.external = true;
    auto result = std::make_shared<Json>(syncFiles(call->ctx(), paths, full, sync));

    auto finish = [call, result, serial](bool timedOut) {
        HotReloadService& reloads = HotReloadService::get();
        Json& out = *result;
        // what finished on a worker: reloaded, or a problem (the old version is kept)
        Json stillPending = Json::array();
        for (const Json& path : out["pending"]) {
            HotReloadService::Record record;
            const bool done = reloads.lastRecordOf(path.get<std::string>(), record) && record.serial > serial;
            if (!done) stillPending.push_back(path);
            else if (record.ok) out["reloaded"].push_back({{"path", record.path}, {"kind", record.kind}, {"ms", std::round(record.ms)}});
        }
        out["pending"] = stillPending;
        for (const Json& problem : reloads.takeLateProblems()) out["problems"].push_back(problem);
        const Json& changed = out["changed"];
        std::string text = "Synced: " + std::to_string(changed["added"].size()) + " added, " +
                           std::to_string(changed["modified"].size()) + " modified, " + std::to_string(changed["removed"].size()) +
                           " removed; " + std::to_string(out["code_editor"]["reloaded"].size()) + " editor buffer(s) reloaded; " +
                           std::to_string(out["script_properties"].size()) + " script(s) with new properties; " +
                           std::to_string(out["reloaded"].size()) + " file(s) reloaded.";
        if (!stillPending.empty()) text += " Still reloading" + std::string(timedOut ? " after 30 s" : "") + ": " + std::to_string(stillPending.size()) + ".";
        if (!out["problems"].empty()) {
            text += " PROBLEMS:";
            for (const Json& problem : out["problems"]) {
                text += "\n- " + problem.value("path", std::string()) +
                        (problem.contains("line") ? ":" + std::to_string(problem["line"].get<int>()) : std::string()) + ": " +
                        problem.value("message", std::string());
            }
        }
        call->ok(text, out);
    };
    if (!wait || (*result)["pending"].empty()) {
        finish(false);
        return;
    }
    // the reloads it started run on workers: the reply waits for them (R32, at most 30 s)
    std::vector<fs::path> pending;
    for (const Json& path : (*result)["pending"]) pending.push_back((project->getProjectPath() / path.get<std::string>()).lexically_normal());
    call->waitUntil([pending]() { return HotReloadService::get().idleFor(pending); }, 30000, finish);
}

// ------------------------------------------------------------------ compile_scripts

std::mutex compileMutex;
std::weak_ptr<McpJob> runningCompile;

void compileScriptsTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    const Json& args = call->args();
    const bool wait = args.value("wait", true);
    const double timeout = args.value("timeout_seconds", 300.0);

    {
        std::lock_guard<std::mutex> lock(compileMutex);
        if (JobPtr running = runningCompile.lock()) {
            if (!running->finished()) {
                replyWhenJobDone(call, running, wait, timeout);
                return;
            }
        }
    }
    if (project->isAnyScenePlaying()) {
        call->fail("A scene is playing (Play builds and loads the scripts itself): stop Play first.");
        return;
    }

    // the build writes generated sources and its build tree into the internal folder (CMake and
    // the compiler with their own writers): it must stay inside the project, checked up front
    if (const std::string escape = McpRefs::internalFoldersEscape(project); !escape.empty()) {
        call->fail(escape);
        return;
    }
    ProjectChecks::CppBuildPlan plan;
    std::string error;
    switch (ProjectChecks::prepareCppBuild(project, plan, error)) {
        case ProjectChecks::CppPrepare::Failed:
            call->fail(error);
            return;
        case ProjectChecks::CppPrepare::MissingTools:
            call->fail(error, Json{{"success", false}});
            return;
        case ProjectChecks::CppPrepare::NothingToBuild:
            call->ok("Nothing to compile: no enabled C++ script is attached to a GameObject and the project has no script "
                     "directories. (Lua scripts are not compiled: use check_script for their syntax.)",
                     Json{{"success", true}, {"scripts", Json::array()}});
            return;
        case ProjectChecks::CppPrepare::Ready:
            break;
    }

    Json scriptList = Json::array();
    for (const SceneScriptSource& script : plan.scripts) {
        scriptList.push_back({{"class_name", script.className}, {"path", FileUtils::pathToGenericUtf8(script.path)},
                              {"header_path", FileUtils::pathToGenericUtf8(script.headerPath)}});
    }

    JobPtr job = ctx.jobs().create("compile_scripts", "Compile C++ scripts", JobBusy::ScriptCompile);
    {
        std::lock_guard<std::mutex> lock(compileMutex);
        runningCompile = job;
    }
    auto generator = std::make_shared<Generator>();
    const uint64_t firstLog = Out::lastLogId();
    const fs::path projectPath = project->getProjectPath();
    const auto started = std::chrono::steady_clock::now();
    job->setCancelHandler([generator]() { generator->cancelBuild(); });
    job->setProgress(-1.0, "Configuring and building " + std::to_string(plan.scripts.size()) + " script(s)");
    ProjectChecks::startCppBuild(project, *generator, plan);

    // polls the build (on the process channel) until it ends: a channel of its own, which
    // cannot hold the process slot the build waits for
    static jobs::JobChannel* watchChannel = new jobs::JobChannel("script-build-watch", 1);
    jobs::SubmitOptions watchOptions;
    watchOptions.name = "script-build-watch";
    watchChannel->submit([job, generator, firstLog, projectPath, scriptList, started]() {
        const std::vector<std::string> output =
            ProjectChecks::waitCppBuild(*generator, firstLog, [&job](const std::string& line) { job->log(line); });
        const bool success = generator->didLastBuildSucceed();
        const ProjectChecks::CppBuildReport report = ProjectChecks::summarizeCppBuild(projectPath, success, output);
        Json errors = Json::array(), warnings = Json::array();
        for (const auto& [list, out] : {std::make_pair(&report.errors, &errors), std::make_pair(&report.warnings, &warnings)}) {
            for (const ProjectChecks::CppDiagnostic& d : *list) {
                out->push_back({{"file", d.file}, {"line", d.line}, {"column", d.column}, {"message", d.message}});
            }
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        Json result = {{"success", success}, {"errors", errors}, {"warnings", warnings}, {"scripts", scriptList},
                       {"seconds", std::round(seconds * 10.0) / 10.0}, {"log_tail", report.logTail}};
        if (job->cancelRequested()) {
            job->markCancelled("Build cancelled.");
        } else if (success) {
            job->succeed("Compiled " + std::to_string(scriptList.size()) + " C++ script(s) with " +
                         std::to_string(warnings.size()) + " warning(s).", result);
        } else {
            std::string message = errors.empty() ? "Build failed (see log_tail; CMake configuration errors have no file:line)."
                                                 : "Build failed with " + std::to_string(errors.size()) + " error(s). First: " +
                                                       errors[0]["file"].get<std::string>() + ":" + std::to_string(errors[0]["line"].get<int>()) +
                                                       ": " + errors[0]["message"].get<std::string>();
            job->fail(message, result);
        }
    }, watchOptions);

    replyWhenJobDone(call, job, wait, timeout, [](const McpJob& finished) {
        Json result = finished.result();
        result["job_id"] = finished.id();
        result["status"] = jobStatusName(finished.status());
        std::string text = finished.message();
        if (result.contains("errors")) {
            for (const Json& error : result["errors"]) {
                text += "\n" + error["file"].get<std::string>() + ":" + std::to_string(error["line"].get<int>()) + ":" +
                        std::to_string(error["column"].get<int>()) + ": " + error["message"].get<std::string>();
            }
        }
        // A failed compile is a result, not a tool error: the agent reads the errors and fixes them
        if (finished.status() == JobStatus::Cancelled) return ToolResult::error(text, result);
        return ToolResult::success(text, result);
    });
}

// ------------------------------------------------------------------ code_editor

void codeEditorTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    CodeEditor* editor = call->ctx().codeEditor();
    if (!editor) {
        call->fail("The code editor is not available.");
        return;
    }
    const std::string action = args.value("action", "status");
    auto status = [&]() {
        Json buffers = Json::array();
        for (const CodeEditor::BufferState& state : editor->getBufferStates()) {
            buffers.push_back({{"path", FileUtils::pathToUtf8(fs::path(state.path))}, {"unsaved", state.modified}, {"cursor_line", state.cursorLine}});
        }
        return buffers;
    };
    if (action == "status") {
        Json buffers = status();
        size_t unsaved = 0;
        for (const Json& b : buffers) if (b["unsaved"].get<bool>()) unsaved++;
        call->ok(std::to_string(buffers.size()) + " open file(s), " + std::to_string(unsaved) + " with unsaved edits.",
                 Json{{"buffers", buffers}});
        return;
    }
    const std::string path = args.value("path", "");
    fs::path file;
    std::string error;
    // a save writes the file: inside the project once links are resolved (an open only reads)
    if (action == "save" ? !McpRefs::resolveProjectWritePath(project, path, file, error, false)
                         : !McpRefs::resolveProjectPath(project, path, file, error, action == "open")) {
        call->fail(error.empty() ? "path is required." : error);
        return;
    }
    const std::string rel = projectRelative(project, file);
    if (action == "open") {
        if (!isTextFile(file)) {
            call->fail(rel + " is not a text file the code editor opens.");
            return;
        }
        // target preferred: Preferences > External Tools > External Script Editor (S23 R28)
        const std::string target = args.value("target", std::string("builtin"));
        if (target != "builtin" && target != "preferred") {
            call->fail("target is builtin or preferred.");
            return;
        }
        std::string openError;
        const bool external = target == "preferred" && ExternalCodeEditor::hasExternalPreference();
        if (!ExternalCodeEditor::open(file, args.value("line", 0), args.value("column", 0), target == "preferred", &openError)) {
            call->fail("Couldn't open " + rel + ": " + openError);
            return;
        }
        call->ok("Opened " + rel + (args.value("line", 0) > 0 ? " at line " + std::to_string(args.value("line", 0)) : std::string()) +
                 (external ? " in the external editor (" + AppSettings::getExternalScriptEditor() + ")." : " in the code editor."),
                 Json{{"buffers", status()}, {"opened_in", external ? AppSettings::getExternalScriptEditor() : std::string("builtin")}});
        return;
    }
    if (!editor->isFileOpen(file.string())) {
        call->fail(rel + " is not open in the code editor.", Json{{"buffers", status()}});
        return;
    }
    bool done = false;
    if (action == "save") done = editor->saveFile(file.string());
    else if (action == "reload") done = editor->reloadFile(file.string());
    else if (action == "close") {
        if (editor->isFileModified(file.string()) && !args.value("discard_unsaved", false)) {
            call->fail(rel + " has unsaved edits: save or reload it first, or pass discard_unsaved: true.");
            return;
        }
        editor->closeFile(file.string());
        done = true;
    } else {
        call->fail("action must be status, open, save, reload or close.");
        return;
    }
    if (!done) {
        call->fail("Couldn't " + action + " " + rel + ".");
        return;
    }
    if (action == "save") syncFiles(call->ctx(), {file}, false);
    call->ok(action == "save" ? "Saved " + rel + "." : action == "reload" ? "Reloaded " + rel + " from disk." : "Closed " + rel + ".",
             Json{{"buffers", status()}});
}

} // namespace

void registerScriptTools(McpRegistry& registry) {
    using namespace schema;

    const Json scriptSelector = any("Index, class or file (optional with one script).");

    {
        ToolDef tool;
        tool.name = "create_script";
        tool.title = "Create Script";
        tool.description =
            "Create a script from the engine template, optionally attached to entity; then edit the file "
            "yourself. kind lua: module table in the Lua dir; cpp_script: ScriptBase subclass; cpp_subclass: "
            "subclass of the GameObject's class (e.g. Mesh), needs entity. content (Lua) or header_content + "
            "source_content (C++) replace the template.";
        tool.inputSchema = object({
            {"kind", stringEnum("", {"lua", "cpp_script", "cpp_subclass"})},
            {"name", string("Class/module name = file name.")},
            {"directory", assetPath("Default: Lua dir, or \"scripts\" for C++.")},
            {"content", string("")},
            {"header_content", string("")},
            {"source_content", string("")},
            {"scene", sceneRef()},
            {"entity", entityRef("")},
            {"overwrite", withDefault(boolean(""), false)}
        }, {"kind", "name"});
        tool.allowedWhilePlaying = false;
        tool.handler = createScriptTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "attach_script";
        tool.title = "Attach Script";
        tool.description =
            "Attach a script file to a GameObject (adds the Script component if needed). path: a .lua in the Lua "
            "dir, or a C++ script's .h or .cpp (pair side by side). Fails if already attached.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"path", assetPath("")},
            {"class_name", string("C++: the class, if several in the header.")},
            {"index", integer("Position (default: last); run order.")},
            {"enabled", withDefault(boolean(""), true)}
        }, {"entity", "path"});
        tool.allowedWhilePlaying = false;
        tool.handler = attachScriptTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "detach_script";
        tool.title = "Detach Script";
        tool.description =
            "Remove one script from a GameObject (files are kept); returns the remaining scripts.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"script", scriptSelector}
        }, {"entity"});
        tool.annotations.destructive = true;
        tool.allowedWhilePlaying = false;
        tool.handler = detachScriptTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "update_script_entry";
        tool.title = "Update Script Entry";
        tool.description =
            "Change one attached script: enabled (also in Play: onEnable/onDisable), move_to (reorder = run order), "
            "path (another file of the same language, e.g. after a rename; property values kept), class_name (C++ "
            "class renamed).";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"script", scriptSelector},
            {"enabled", boolean("")},
            {"move_to", integer("New 0-based index.")},
            {"path", assetPath("")},
            {"class_name", string("")}
        }, {"entity"});
        // in Play: enabled only (the handler refuses the rest)
        tool.allowedWhilePlaying = true;
        tool.handler = updateScriptEntryTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "set_script_properties";
        tool.title = "Set Script Properties";
        tool.description =
            "Set a script's exposed properties (Lua `properties`, C++ DPROPERTY) on one GameObject (= "
            "set_properties scripts[i].<name>, same values); \"enabled\" toggles the script. Properties added to "
            "the file are picked up before your next call.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"script", scriptSelector},
            {"values", anyObject("")}
        }, {"entity", "values"});
        tool.allowedWhilePlaying = false;
        tool.handler = setScriptPropertiesTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "list_scripts";
        tool.title = "List Scripts";
        tool.description =
            "With entity: its scripts in order (class, files, enabled, properties: name, type, value, path). "
            "Without: the scene's scripted GameObjects. include_files adds the project's script files, Lua dir and "
            "C++ script dirs.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef("")},
            {"include_files", withDefault(boolean(""), false)}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = listScriptsTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "check_script";
        tool.title = "Check Script";
        tool.description =
            "Check a script without playing: Lua syntax (not run; ok false + error line), properties, lifecycle "
            "warnings; C++ header: class, kind, DPROPERTY fields (code: compile_scripts).";
        tool.inputSchema = object({
            {"path", assetPath("")},
            {"code", string("")},
            {"language", stringEnum("Default: from the extension.", {"lua", "cpp_header", "cpp_source"})}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = checkScriptTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "sync_files";
        tool.title = "Sync Files";
        tool.description =
            "Rarely needed (files edited on disk are picked up before every call: external_changes): a full "
            "rescan of code editor buffers, script properties, Lua syntax, materials, shaders, textures, assets; "
            "returns what changed and the problems. Not for .scene/.prefab/project.yaml.";
        tool.inputSchema = object({
            {"paths", array(string(""), "Changed files (optional).")},
            {"full", withDefault(boolean("Scan the project, recheck all scripts."), true)},
            {"wait", withDefault(boolean("Return after the reloads it started finish (30 s max)."), true)},
            {"rebuild_models", withDefault(boolean("Rebuild a model whose node hierarchy changed (its GameObjects are made again)."), false)},
            {"auto_refresh", boolean("Set Auto Refresh: off, external edits wait for a call or Assets > Refresh.")}
        });
        tool.annotations.idempotent = true;
        tool.handler = syncFilesTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "compile_scripts";
        tool.activity = ToolActivity::ScriptCompile;  // busy policy (McpServer::busyRefusal)
        tool.title = "Compile C++ Scripts";
        tool.description =
            "Build the C++ scripts without playing (Play's CMake build; the next Play is incremental): "
            "success, errors and warnings [{file, line, column, message}], log tail; compile errors are no "
            "tool error. Don't Play meanwhile.";
        tool.inputSchema = object({
            {"wait", withDefault(boolean(""), true)},
            {"timeout_seconds", withDefault(number("", 1, 3600), 300)}
        });
        tool.allowedWhilePlaying = false;
        tool.handler = compileScriptsTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "code_editor";
        tool.title = "Code Editor";
        tool.description =
            "The editor's code editor (for the human): open at line/column, status, save, reload (drop buffer "
            "edits), close. An unsaved buffer means the human is editing it: don't overwrite that file.";
        tool.inputSchema = object({
            {"action", withDefault(stringEnum("", {"status", "open", "save", "reload", "close"}), "status")},
            {"path", assetPath("All actions but status.")},
            {"line", integer("open: 1-based.")},
            {"column", integer("open: 1-based.")},
            {"target", withDefault(stringEnum("open: preferred = the External Script Editor of the preferences.", {"builtin", "preferred"}), "builtin")},
            {"discard_unsaved", withDefault(boolean("close: drop unsaved edits."), false)}
        });
        tool.annotations.destructive = true;
        tool.handler = codeEditorTool;
        registry.addTool(std::move(tool));
    }
}

}
