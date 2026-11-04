// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PaletteAssets.h"

#include "Project.h"
#include "pixelart/BuiltinPalettes.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::pixelart;

namespace fs = std::filesystem;
namespace pxc = jasperengine::pixelart;

// file-local, in a namespace of its own: a unity build merges this directory's sources
namespace jasperengine::editor::pixelart::paletteassets {
namespace {

    bool readPalette(const fs::path& file, pxc::Palette& out) {
        std::ifstream in(file, std::ios::binary);
        if (!in) return false;
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<pxc::Diagnostic> diagnostics;
        return pxc::PaletteCodec::read(text, out, diagnostics);
    }

    std::string relativeTo(const fs::path& file, const fs::path& root) {
        return FileUtils::pathToGenericUtf8(file.lexically_relative(root));
    }

    bool isPalettePath(const std::string& name) {
        return name.size() > 8 && name.compare(name.size() - 8, 8, ".palette") == 0;
    }

}
}

using namespace jasperengine::editor::pixelart::paletteassets;

std::vector<ProjectPalette> jasperengine::editor::pixelart::listProjectPalettes(const Project& project) {
    std::vector<ProjectPalette> palettes;
    const fs::path assets = project.getAssetsPath();
    std::error_code error;
    if (!fs::is_directory(assets, error)) return palettes;
    for (fs::recursive_directory_iterator it(assets, fs::directory_options::skip_permission_denied, error), end; it != end; it.increment(error)) {
        if (error) break;
        const fs::path& file = it->path();
        // hidden folders (.git, editor caches) hold no project palettes
        if (it->is_directory(error) && !file.filename().empty() && file.filename().string()[0] == '.') {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(error) || file.extension() != ".palette") continue;
        ProjectPalette entry;
        if (!readPalette(file, entry.palette)) continue;
        entry.path = relativeTo(file, project.getProjectPath());
        entry.assetPath = relativeTo(file, assets);
        palettes.push_back(std::move(entry));
    }
    std::sort(palettes.begin(), palettes.end(), [](const ProjectPalette& a, const ProjectPalette& b) { return a.path < b.path; });
    return palettes;
}

std::string jasperengine::editor::pixelart::findBuiltinCopy(const Project& project, const std::string& id) {
    for (const ProjectPalette& entry : listProjectPalettes(project)) {
        if (entry.palette.source.kind == "builtin" && entry.palette.source.id == id) return entry.path;
    }
    return {};
}

std::string jasperengine::editor::pixelart::builtinCopyPath(const Project& project, const std::string& id) {
    const fs::path folder = project.getAssetsPath() / "Palettes";
    fs::path file = folder / (id + ".palette");
    std::error_code error;
    for (int n = 2; fs::exists(file, error); n++) file = folder / (id + " " + std::to_string(n) + ".palette");
    return relativeTo(file, project.getProjectPath());
}

std::string jasperengine::editor::pixelart::defaultPalettePath(const Project& project) {
    return FileUtils::pathToGenericUtf8(project.getDefaultPalette());
}

ResolvedPalette jasperengine::editor::pixelart::resolvePaletteName(const Project& project, const std::string& given, bool allowBareBuiltin) {
    ResolvedPalette result;
    std::string name = given;
    if (name.empty()) {
        name = defaultPalettePath(project);
        if (name.empty()) {
            result.error = {"not_found", "palette", "no palette given and the project has no Default Palette",
                            "name a palette (a .palette path or builtin:<id>), or set Project Settings > Default Palette (palettes.default)"};
            return result;
        }
    }

    // a built-in: through the project's copy of it
    std::string id;
    if (name.rfind("builtin:", 0) == 0) id = name.substr(8);
    else if (allowBareBuiltin && !isPalettePath(name) && findBuiltinPalette(name)) id = name;
    if (!id.empty()) {
        // "builtin:fire#ramp:fire" names the copy's ramp: the caller splits it first
        const BuiltinPalette* builtin = findBuiltinPalette(id);
        if (!builtin) {
            std::string ids;
            for (const BuiltinPalette& entry : builtinPalettes()) ids += (ids.empty() ? "" : ", ") + entry.id;
            result.error = {"not_found", "palette", "no built-in palette '" + id + "'", "use one of: " + ids};
            return result;
        }
        result.builtin = true;
        result.path = findBuiltinCopy(project, id);
        if (result.path.empty()) {
            result.path = builtinCopyPath(project, id);
            result.needsCopy = true;
            result.copyText = std::string(builtinPaletteText(id));
            result.palette = builtin->palette;
        } else if (!readPalette(project.getProjectPath() / result.path, result.palette)) {
            result.error = {"parse_error", "palette", "the project's copy of builtin:" + id + " (" + result.path + ") cannot be read",
                            "fix or delete " + result.path + " (a new copy is made on the next use)"};
            return result;
        }
        result.assetPath = relativeTo(project.getProjectPath() / result.path, project.getAssetsPath());
        result.ok = true;
        return result;
    }

    // a path: project-relative first, else relative to the assets directory
    fs::path file = project.getProjectPath() / FileUtils::pathFromUtf8(name);
    std::error_code error;
    if (!fs::is_regular_file(file, error)) file = project.getAssetsPath() / FileUtils::pathFromUtf8(name);
    if (!fs::is_regular_file(file, error)) {
        result.error = {"not_found", "palette", "no palette at '" + name + "'",
                        "give a .palette path (list them with edit_palette and no arguments) or builtin:<id>"};
        return result;
    }
    if (!readPalette(file, result.palette)) {
        result.error = {"parse_error", "palette", "'" + name + "' is not a readable palette", "read it with edit_palette to see its errors"};
        return result;
    }
    result.path = relativeTo(file, project.getProjectPath());
    result.assetPath = relativeTo(file, project.getAssetsPath());
    result.ok = true;
    return result;
}
