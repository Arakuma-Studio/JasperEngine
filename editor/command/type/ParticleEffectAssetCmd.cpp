// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleEffectAssetCmd.h"

#include "Out.h"
#include "Project.h"
#include "asset/AssetTypes.h"
#include "command/CommandNames.h"
#include "io/AtomicFile.h"
#include "particles/ParticleEffectCodec.h"
#include "particles/ParticleEffectLibrary.h"

#include <filesystem>
#include <system_error>

using namespace jasperengine;

namespace fs = std::filesystem;

editor::ParticleEffectAssetCmd::ParticleEffectAssetCmd(Project* project, std::string path, std::string before, std::string after, Guid guid)
    : project(project), path(std::move(path)), before(std::move(before)), after(std::move(after)), guid(guid) {
}

bool editor::ParticleEffectAssetCmd::apply(const std::string& text) {
    const fs::path file = project->resolveAssetPath(path);
    if (text.empty()) {
        // undoing the creation: the file and its meta go, the asset leaves the database
        std::error_code ec;
        const fs::path relative = project->getAssetDatabase().toRelative(file);
        fs::remove(file, ec);
        fs::remove(project->getProjectPath() / AssetTypes::metaPathFor(relative.generic_string()), ec);
        project->getAssetDatabase().removeAssets(file);
        particles::ParticleEffectLibrary::reload(path);
        return true;
    }

    const bool created = !fs::exists(file);
    const io::WriteResult written = io::writeFileAtomic(file, text);
    if (!written.ok()) {
        Out::error("Could not write %s: %s", file.generic_string().c_str(), written.message.c_str());
        return false;
    }
    if (created && !guid.isNull()) {
        project->getAssetDatabase().registerAsset(file, guid.str());
    }

    particles::ParticleEffectDocument document;
    std::vector<particles::Diagnostic> diagnostics;
    if (particles::ParticleEffectCodec::read(text, document, diagnostics)) {
        particles::ParticleEffectLibrary::set(path, document);
    } else {
        particles::ParticleEffectLibrary::reload(path);
    }
    return true;
}

bool editor::ParticleEffectAssetCmd::execute() {
    return apply(after);
}

void editor::ParticleEffectAssetCmd::undo() {
    apply(before);
}

bool editor::ParticleEffectAssetCmd::mergeWith(Command* olderCommand) {
    // the history merges a newer command into the step before it: this one takes over its state
    ParticleEffectAssetCmd* older = dynamic_cast<ParticleEffectAssetCmd*>(olderCommand);
    if (!older || older->project != project || older->path != path || older->before.empty() || before.empty()) {
        return false;
    }
    before = older->before;
    return true;
}

std::string editor::ParticleEffectAssetCmd::describe() const {
    return (before.empty() ? "Create Particle Effect " : "Edit Particle Effect ") + CommandNames::file(path);
}
