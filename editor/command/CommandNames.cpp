// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CommandNames.h"

#include "texture/MaterialPbr.h"

#include "Project.h"

#include <cctype>

using namespace jasperengine;

namespace {

    // Bytes of the UTF-8 sequence starting with `lead`
    size_t utf8Length(unsigned char lead){
        if (lead < 0x80) return 1;
        if ((lead >> 5) == 0x6) return 2;
        if ((lead >> 4) == 0xE) return 3;
        if ((lead >> 3) == 0x1E) return 4;
        return 1;
    }

}

std::string editor::CommandNames::clip(const std::string& name){
    // "…" counts as one of the MAX_LENGTH code points
    size_t points = 0;
    size_t cut = std::string::npos;
    for (size_t i = 0; i < name.size(); i += utf8Length(static_cast<unsigned char>(name[i]))){
        if (points == MAX_LENGTH - 1){
            cut = i;
        }
        if (++points > MAX_LENGTH){
            return name.substr(0, cut) + "\xE2\x80\xA6";
        }
    }
    return name;
}

std::string editor::CommandNames::objects(const std::vector<std::string>& names){
    if (names.empty()){
        return "Nothing";
    }
    return count(names.size(), names.front());
}

std::string editor::CommandNames::entity(Project* project, uint32_t sceneId, Entity entity){
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject || !sceneProject->scene || entity == NULL_ENTITY || !sceneProject->scene->isEntityCreated(entity)){
        return "GameObject";
    }
    std::string name = sceneProject->scene->getEntityName(entity);
    return name.empty() ? std::string("GameObject") : name;
}

std::string editor::CommandNames::entities(Project* project, uint32_t sceneId, const std::vector<Entity>& entities){
    if (entities.size() == 1){
        return entity(project, sceneId, entities.front());
    }
    return count(entities.size(), std::string());
}

std::string editor::CommandNames::scene(Project* project, uint32_t sceneId){
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    return sceneProject && !sceneProject->name.empty() ? sceneProject->name : std::string("Scene");
}

std::string editor::CommandNames::count(size_t count, const std::string& one, const char* plural){
    if (count == 1){
        return one;
    }
    return std::to_string(count) + " " + plural;
}

std::string editor::CommandNames::property(const std::string& propertyName){
    // a field of a material's extended lobes is named with its lobe ("Clear Coat Roughness")
    const size_t pbr = propertyName.find(".pbr.");
    if (pbr != std::string::npos){
        if (const PbrField* field = MaterialPbr::findField(propertyName.substr(pbr + 5)))
            return MaterialPbr::getFieldLabel(*field);
    }
    // the last path segment, without its [index]
    std::string segment = propertyName;
    const size_t dot = segment.rfind('.');
    if (dot != std::string::npos){
        segment = segment.substr(dot + 1);
    }
    const size_t bracket = segment.find('[');
    if (bracket != std::string::npos){
        segment = segment.substr(0, bracket);
    }
    if (segment.empty()){
        segment = propertyName;
    }

    std::string label;
    bool wordStart = true;
    for (size_t i = 0; i < segment.size(); i++){
        const char c = segment[i];
        if (c == '_' || c == '-' || c == ' '){
            wordStart = true;
            continue;
        }
        const bool upper = std::isupper(static_cast<unsigned char>(c)) != 0;
        const bool digit = std::isdigit(static_cast<unsigned char>(c)) != 0;
        // camelCase and acronym boundaries: "baseColor" -> "Base Color", "UVScale" -> "UV Scale"
        if (!label.empty() && !wordStart && upper){
            const char prev = segment[i - 1];
            const bool nextLower = i + 1 < segment.size() && std::islower(static_cast<unsigned char>(segment[i + 1]));
            if (std::islower(static_cast<unsigned char>(prev)) || std::isdigit(static_cast<unsigned char>(prev)) || nextLower){
                wordStart = true;
            }
        }
        if (!label.empty() && !wordStart && digit && std::isalpha(static_cast<unsigned char>(segment[i - 1]))){
            wordStart = true;
        }
        if (wordStart && !label.empty()){
            label += ' ';
        }
        label += wordStart ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        wordStart = false;
    }
    return label.empty() ? propertyName : label;
}

std::string editor::CommandNames::component(ComponentType type){
    return Catalog::getComponentDisplayName(type);
}

std::string editor::CommandNames::file(const std::filesystem::path& path){
    std::filesystem::path name = path.filename();
    if (name.empty()){
        name = path.parent_path().filename();  // "dir/"
    }
    return name.string();
}

std::string editor::CommandNames::files(const std::vector<std::filesystem::path>& paths){
    if (paths.size() == 1){
        return file(paths.front());
    }
    return count(paths.size(), std::string(), "Files");
}

std::string editor::CommandNames::verb(const std::string& verb, const std::string& subject){
    if (subject.empty()){
        return verb;
    }
    return verb + " " + subject;
}
