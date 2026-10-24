// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Shader forks (S05 R11): only the forks whose entry points or #include closure hold a
// changed file are compiled, on a worker, before anything is swapped. All of them compiled:
// their builds go into the editor's cache and only they are invalidated, so the scene draws
// the new programs without a gap. One failed: every program stays as it was and the errors
// are reported with their file and line.

#include "ReloadHandlers.h"

#include "Project.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderBuilder.h"
#include "shader/ShaderVariantCompiler.h"
#include "util/ProjectChecks.h"
#include "util/Util.h"

#include <fstream>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    // a reference's "#A,B" feature suffix is not part of the fork
    std::string shaderForkBase(const std::string& name) {
        const size_t hash = name.find('#');
        return hash == std::string::npos ? name : name.substr(0, hash);
    }

    bool readShaderText(const fs::path& path, std::string& content) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return false;
        std::ostringstream stream;
        stream << file.rdbuf();
        content = stream.str();
        return true;
    }

}

bool reload::shaderSourcesReach(Project* project, const std::string& base, int shaderType, const std::set<std::string>& changed) {
    const fs::path projectRoot = project->getProjectPath();
    const fs::path assetsRoot = project->getAssetsPath();
    // a project UI effect is named by its asset path (ShaderBuilder::resolveCustomSources)
    const bool fragmentOnly = base.find('|') == std::string::npos && base.size() > 5 && base.compare(base.size() - 5, 5, ".frag") == 0;
    // the files the resolution reads, by the path it read them at (an include's key is relative
    // to its entry point's folder first)
    bool reached = false;
    auto reader = [&](const std::string& relativePath, std::string& content) {
        const fs::path path = fragmentOnly ? Project::findShaderFile(assetsRoot, projectRoot, relativePath)
                                           : Project::findShaderFile(projectRoot, assetsRoot, relativePath);
        if (path.empty() || !readShaderText(path, content)) return false;
        if (changed.count(fs::path(relativePath).lexically_normal().generic_string())) reached = true;
        return true;
    };
    const CustomShaderSources sources = ShaderVariantCompiler::resolveCustomShaderSources(base, reader, static_cast<ShaderType>(std::max(shaderType, 0)));
    if (reached || changed.count(sources.vertFile) || changed.count(sources.fragFile)) return true;
    // an entry point that does not resolve any more (renamed away) still names its fork
    return !sources.valid;
}

void reload::reloadShaders(Context& ctx, const std::vector<fs::path>& files) {
    if (files.empty()) return;
    Project* project = &ctx.project;
    for (const fs::path& path : files) {
        const std::string relative = rel(ctx.project, path);
        std::vector<ProjectChecks::ShaderUse> forks = ProjectChecks::shaderForksUsingFiles(project, {relative});
        if (forks.empty()) continue;   // nothing uses it
        std::vector<uint64_t> keys;
        for (ProjectChecks::ShaderUse& fork : forks) keys.push_back(ProjectChecks::prepareShaderCheck(project, fork));
        ctx.result.shadersInvalidated = true;

        std::set<std::string> changed = {relative};
        const std::string assetsRelative = ProjectChecks::projectRelative(project->getAssetsPath(), path);
        if (!assetsRelative.empty()) changed.insert(assetsRelative);

        ctx.service.startJob("shader", path, [project, forks, keys, changed, relative]() -> HotReloadService::AdoptFn {
            struct Built {
                ProjectChecks::ShaderUse use;
                uint64_t key = 0;
                ShaderData data;
                ProjectChecks::ShaderCheck check;
            };
            auto built = std::make_shared<std::vector<Built>>();
            for (size_t i = 0; i < forks.size(); i++) {
                if (!shaderSourcesReach(project, forks[i].base, forks[i].shaderType, changed)) continue;
                // the check reads the files on disk, not the snapshot of the last build
                ShaderBuilder::dropSourceSnapshot(forks[i].base);
                Built item;
                item.use = forks[i];
                item.key = keys[i];
                item.check = ProjectChecks::runShaderCheck(project, forks[i], keys[i], &item.data);
                built->push_back(std::move(item));
            }
            return [project, built, relative](std::string& error) {
                if (built->empty()) return true;   // an include no fork in use reaches
                bool ok = true;
                Json results = Json::array();
                for (const Built& item : *built) {
                    results.push_back(ProjectChecks::toJson(item.check));
                    ok = ok && item.check.ok;
                }
                if (!ok) {
                    for (const Json& problem : shaderProblems(project, results, {relative})) {
                        if (error.empty()) {
                            error = problem.value("path", std::string()) +
                                    (problem.value("line", 0) > 0 ? ":" + std::to_string(problem.value("line", 0)) : std::string()) + ": " +
                                    problem.value("message", std::string());
                        }
                        HotReloadService::get().addLateProblem(problem);
                    }
                }
                // a compile error keeps every program bound (R11)
                if (!ok) return false;
                std::set<std::string> bases;
                for (const Built& item : *built) bases.insert(shaderForkBase(item.use.base));
                project->invalidateCustomShaders([bases](const std::string& fork) { return bases.count(shaderForkBase(fork)) > 0; });
                // the scene's next draw finds the new programs built: no fallback frame
                for (const Built& item : *built) ShaderBuilder::primeCache(item.key, item.data);
                return true;
            };
        });
    }
}

namespace {

    std::string shortShaderMessage(std::string text, size_t max) {
        for (char& ch : text) {
            if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
        }
        if (text.size() > max) text = text.substr(0, max) + "...";
        return text;
    }

}

Json reload::shaderProblems(Project* project, const Json& results, const std::vector<std::string>& changedFiles) {
    Json problems = Json::array();
    // shared by the results: a UI effect's forms (phase 5) report the same error once
    std::set<std::string> seen;
    for (const Json& result : results) {
        if (result.value("ok", false)) continue;
        const std::string base = result.value("shader", std::string());
        const Util::CustomShaderPaths paths = project->resolveCustomShaderFiles(base);
        // the changed file of the fork (else its fragment shader)
        std::string path = paths.frag.empty() ? base : paths.frag;
        for (const std::string& file : changedFiles) {
            if (file == paths.vert || file == paths.frag) {
                path = file;
                break;
            }
        }
        const Json errors = result.value("errors", Json::array());
        size_t added = 0;
        // errors with a place, reported here or by an earlier result (a UI effect's other form)
        size_t placed = 0;
        for (const Json& error : errors) {
            if (error.value("severity", std::string("error")) != "error") continue;
            const std::string message = shortShaderMessage(error.value("message", std::string()), 300);
            if (message.find("compilation terminated") != std::string::npos) continue;
            placed++;
            // glslang names the file (an include, or the stage file) when it can
            const std::string source = error.value("source", std::string());
            const bool namesFile = source.find('.') != std::string::npos || source.find('/') != std::string::npos;
            const std::string where = namesFile ? source : path;
            const int line = error.value("line", 0);
            if (!seen.insert(where + ":" + std::to_string(line) + ":" + message).second) continue;
            Json problem = {{"path", where}, {"line", line},
                            {"message", "Shader " + base + " (" + result.value("type", std::string()) + ") does not compile: " + message}};
            // a UI effect (phase 5): effect-shader-compile, or effect-manifest
            if (result.contains("code")) problem["code"] = result["code"];
            problems.push_back(problem);
            if (++added >= 10) break;
        }
        // no file:line at all: the log (not when every error was already reported)
        if (placed == 0) {
            problems.push_back({{"path", path}, {"message", "Shader " + base + " does not compile: " +
                                                                shortShaderMessage(result.value("log", std::string("unknown error")), 400)}});
        }
    }
    return problems;
}
