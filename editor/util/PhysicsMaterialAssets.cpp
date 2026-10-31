// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PhysicsMaterialAssets.h"

#include "Project.h"
#include "command/type/WriteFilesCmd.h"
#include "pool/PhysicsMaterialPool.h"
#include "util/FileUtils.h"
#include "util/ProjectFileWrites.h"

#include <cctype>
#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    void afterWrite(Project* project, const std::vector<fs::path>& changed){
        for (const fs::path& path : changed){
            PhysicsMaterialPool::invalidate(path.string());
        }
        ProjectFileWrites::filesWritten(project, changed);
    }

}

bool PhysicsMaterialAssets::isPhysicsMaterialFile(const std::string& path){
    const fs::path file(path);
    std::string extension = file.extension().string();
    for (char& c : extension) c = (char)std::tolower((unsigned char)c);
    return extension == PHYSICS_MATERIAL_EXTENSION;
}

bool PhysicsMaterialAssets::read(const fs::path& file, PhysicsMaterialData& data, std::string& error){
    std::ifstream in(file, std::ios::binary);
    if (!in){
        error = "cannot read " + FileUtils::pathToGenericUtf8(file);
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    PhysicsMaterialParseResult parsed = physicsmaterial::parse(text.str());
    if (!parsed.ok){
        error = parsed.error;
        return false;
    }
    data = parsed.data;
    return true;
}

bool PhysicsMaterialAssets::write(Project* project, const fs::path& file, const PhysicsMaterialData& data,
                                  std::string& error, const std::string& mergeKey){
    if (!project){
        error = "No project is open to write into.";
        return false;
    }
    std::vector<WriteFilesCmd::Item> items = {{file, physicsmaterial::write(data)}};
    if (mergeKey.empty()){
        return ProjectFileWrites::writeUndoable(project, std::move(items), error,
            [project](const std::vector<fs::path>& changed){ afterWrite(project, changed); });
    }
    // a drag: one undo step for all its writes
    auto failure = std::make_shared<std::string>();
    WriteFilesCmd* cmd = new WriteFilesCmd(std::move(items), [project](const std::vector<fs::path>& changed){ afterWrite(project, changed); });
    cmd->setMergeKey(mergeKey);
    cmd->reportFailureTo(failure);
    if (!project->getProjectCommandHistory()->addCommand(cmd)){
        error = "Writing the physics material failed" + (failure->empty() ? std::string() : ": " + *failure);
        return false;
    }
    return true;
}

fs::path PhysicsMaterialAssets::create(Project* project, const fs::path& directory, const std::string& baseName,
                                       const PhysicsMaterialData& data, std::string& error){
    fs::path file;
    for (int i = 0; i < 10000; i++){
        const std::string name = i == 0 ? baseName : baseName + " " + std::to_string(i);
        file = directory / (name + PHYSICS_MATERIAL_EXTENSION);
        std::error_code ec;
        if (!fs::exists(file, ec)) break;
    }
    PhysicsMaterialData created = data;
    if (created.guid.empty()){
        created.guid = physicsmaterial::generateGuid();
    }
    if (!write(project, file, created, error)){
        return fs::path();
    }
    return file;
}
