// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CreateMaterialFileCmd.h"
#include "DocumentFile.h"
#include "Out.h"

#include "command/type/LinkMaterialCmd.h"
#include "command/type/PropertyCmd.h"
#include "Stream.h"

#include "yaml-cpp/yaml.h"

#include <fstream>
#include <system_error>

using namespace jasperengine;

fs::path editor::CreateMaterialFileCmd::findAvailableTargetFile(const fs::path& directory) {
    std::string baseName = "Material";
    std::string fileName = baseName + ".material";
    fs::path targetFile = directory / fileName;
    int counter = 1;
    while (fs::exists(targetFile)) {
        fileName = baseName + "_" + std::to_string(counter) + ".material";
        targetFile = directory / fileName;
        counter++;
    }

    return targetFile;
}

editor::CreateMaterialFileCmd::CreateMaterialFileCmd(Project* project, const fs::path& directory,
                                                     const char* materialContent, size_t contentLen,
                                                     const MaterialPayload* sourceMaterial) {
    this->project = project;
    this->targetFile = findAvailableTargetFile(directory);
    this->payload.assign(materialContent, contentLen);

    Material decodedMaterial;
    bool hasDecodedMaterial = false;

    try {
        YAML::Node materialNode = YAML::Load(this->payload);
        Material material = Stream::decodeMaterial(materialNode);

        std::error_code ec;
        fs::path relativePath = fs::relative(targetFile, project->getProjectPath(), ec);
        if (!ec) {
            material.name = relativePath.lexically_normal().generic_string();
        }

        decodedMaterial = material;
        hasDecodedMaterial = true;
        this->payload = DocumentFiles::materialText(Stream::encodeMaterial(material), fs::path());
    } catch (const std::exception&) {
    }

    if (sourceMaterial && hasDecodedMaterial) {
        std::string propertyName = "submeshes[" + std::to_string(sourceMaterial->submeshIndex) + "].material";
        if (!decodedMaterial.name.empty()) {
            materialCmd = std::make_unique<LinkMaterialCmd>(
                project, sourceMaterial->sceneId, sourceMaterial->entity,
                ComponentType::MeshComponent, propertyName, sourceMaterial->submeshIndex, decodedMaterial);
        } else {
            materialCmd = std::make_unique<PropertyCmd<Material>>(
                project, sourceMaterial->sceneId, sourceMaterial->entity,
                ComponentType::MeshComponent, propertyName, decodedMaterial);
        }
    }
}

bool editor::CreateMaterialFileCmd::execute() {
    // the file's identity: a new GUID the first time, the same one again on redo
    std::string content = payload;
    try {
        YAML::Node root = YAML::Load(payload);
        if (root.IsMap()) {
            if (!guid.isNull()) {
                root["guid"] = guid.str();
            }
            guid = project->prepareDocumentIdentity(root, targetFile);
            // no trailing newline: material files never had one (DocumentFiles::materialText)
            content = YAML::Dump(root);
        }
    } catch (const std::exception&) {
    }
    const io::WriteResult written = DocumentFiles::write(targetFile, content);
    if (!written.ok()) {
        Out::error(DocumentFiles::saveFailureMessage(targetFile, written));
        return false;
    }
    project->getAssetDatabase().registerAsset(targetFile, guid.str());

    if (materialCmd && !materialCmd->execute()) {
        std::error_code ec;
        fs::remove(targetFile, ec);
        return false;
    }

    return true;
}

void editor::CreateMaterialFileCmd::undo() {
    if (materialCmd) {
        materialCmd->undo();
    }

    std::error_code ec;
    fs::remove(targetFile, ec);
    project->getAssetDatabase().removeAssets(targetFile);
}