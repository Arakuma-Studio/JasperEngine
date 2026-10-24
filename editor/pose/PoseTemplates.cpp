// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PoseTemplates.h"

#include "command/CommandHistory.h"
#include "command/type/RemoveFilesCmd.h"
#include "command/type/WriteFilesCmd.h"
#include "pose/PoseRanges.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>

using namespace jasperengine;

namespace fs = std::filesystem;

namespace {

    std::function<void(const std::vector<fs::path>&)>& filesChangedHook(){
        static std::function<void(const std::vector<fs::path>&)> hook;
        return hook;
    }

    void filesChanged(const std::vector<fs::path>& paths){
        if (filesChangedHook()){
            filesChangedHook()(paths);
        }
    }

    std::string lower(std::string text){
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        return text;
    }

    std::string trim(const std::string& text){
        const size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos){
            return "";
        }
        const size_t last = text.find_last_not_of(" \t\r\n");
        return text.substr(first, last - first + 1);
    }

    bool sameName(const std::string& a, const std::string& b){
        return lower(trim(a)) == lower(trim(b));
    }

    bool readWhole(const fs::path& path, std::string& text){
        std::ifstream in(path, std::ios::binary);
        if (!in){
            return false;
        }
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return !in.bad();
    }

    // A template from file text; false when the text is not a pose animation or has no
    // "template" object. The name falls back to `fallbackName` when the object has none.
    bool parseTemplate(const std::string& text, const std::string& fallbackName, editor::PoseTemplate& out){
        std::optional<pose::PoseAnimationTemplateInfo> info;
        pose::PoseAnimationData data;
        std::vector<std::string> warnings;
        std::string error;
        if (!pose::parsePoseAnimation(text, data, warnings, &error, &info) || !info){
            return false;
        }
        data.previewModel.clear();
        out.info.name = trim(info->name).empty() ? fallbackName : trim(info->name);
        out.info.description = info->description;
        out.info.category = info->category;
        out.info.poseCount = data.poses.size();
        out.info.loop = data.settings.loop;
        out.data = std::move(data);
        out.warnings = std::move(warnings);
        return true;
    }

    bool lessInfo(const editor::PoseTemplateInfo& a, const editor::PoseTemplateInfo& b){
        if (a.user != b.user){
            return !a.user;
        }
        const std::string ca = lower(a.category), cb = lower(b.category);
        if (ca != cb){
            return ca < cb;
        }
        const std::string na = lower(a.name), nb = lower(b.name);
        if (na != nb){
            return na < nb;
        }
        return a.file < b.file;
    }

    // The user templates of a project, unsorted
    std::vector<editor::PoseTemplate> userTemplates(const fs::path& projectPath){
        std::vector<editor::PoseTemplate> out;
        const fs::path dir = editor::PoseTemplates::userDirectory(projectPath);
        std::error_code ec;
        if (!fs::is_directory(dir, ec)){
            return out;
        }
        std::vector<fs::path> files;
        for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)){
            if (lower(it->path().extension().string()) == ".poseanim" && fs::is_regular_file(it->path(), ec)){
                files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const fs::path& file : files){
            std::string text;
            editor::PoseTemplate parsed;
            if (!readWhole(file, text) || !parseTemplate(text, file.stem().string(), parsed)){
                continue;
            }
            parsed.info.user = true;
            parsed.info.file = file;
            // two files with the same name: the first (by file name) wins
            bool duplicate = false;
            for (const editor::PoseTemplate& other : out){
                duplicate = duplicate || sameName(other.info.name, parsed.info.name);
            }
            if (!duplicate){
                out.push_back(std::move(parsed));
            }
        }
        return out;
    }

    std::string safeFileStem(const std::string& name){
        std::string stem;
        for (unsigned char c : trim(name)){
            const bool safe = std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c >= 0x80;
            stem += safe ? static_cast<char>(c) : '_';
        }
        // no leading dots or spaces (hidden files, trimmed names)
        while (!stem.empty() && (stem.front() == ' ' || stem.front() == '.')){
            stem.erase(stem.begin());
        }
        return stem.empty() ? std::string("template") : stem;
    }

}

const std::vector<editor::PoseTemplate>& editor::PoseTemplates::builtins(){
    static const std::vector<PoseTemplate> parsed = [](){
        std::vector<PoseTemplate> out;
        for (const PoseTemplateResource& resource : builtinResources()){
            PoseTemplate t;
            if (!parseTemplate(resource.text, fs::path(resource.fileName).stem().string(), t)){
                continue;
            }
            t.info.user = false;
            t.info.file = resource.fileName;
            out.push_back(std::move(t));
        }
        std::sort(out.begin(), out.end(), [](const PoseTemplate& a, const PoseTemplate& b){ return lessInfo(a.info, b.info); });
        return out;
    }();
    return parsed;
}

fs::path editor::PoseTemplates::userDirectory(const fs::path& projectPath){
    return projectPath / USER_FOLDER;
}

std::vector<editor::PoseTemplateInfo> editor::PoseTemplates::list(const fs::path& projectPath){
    const std::vector<PoseTemplate> user = userTemplates(projectPath);
    std::vector<PoseTemplateInfo> out;
    for (const PoseTemplate& t : builtins()){
        const bool shadowed = std::any_of(user.begin(), user.end(), [&](const PoseTemplate& u){ return sameName(u.info.name, t.info.name); });
        if (!shadowed){
            out.push_back(t.info);
        }
    }
    for (const PoseTemplate& t : user){
        out.push_back(t.info);
    }
    std::stable_sort(out.begin(), out.end(), lessInfo);
    return out;
}

bool editor::PoseTemplates::load(const fs::path& projectPath, const std::string& name, PoseTemplate& out, std::string* error){
    for (PoseTemplate& t : userTemplates(projectPath)){
        if (sameName(t.info.name, name)){
            out = std::move(t);
            return true;
        }
    }
    for (const PoseTemplate& t : builtins()){
        if (sameName(t.info.name, name)){
            out = t;
            return true;
        }
    }
    if (error){
        std::string names;
        for (const PoseTemplateInfo& info : list(projectPath)){
            names += (names.empty() ? "" : ", ") + info.name;
        }
        *error = "no pose template named '" + name + "' (templates: " + (names.empty() ? std::string("none") : names) + ")";
    }
    return false;
}

bool editor::PoseTemplates::validName(const std::string& name, std::string* error){
    const std::string trimmed = trim(name);
    if (trimmed.empty()){
        if (error) *error = "the template name is empty";
        return false;
    }
    if (trimmed.size() > MAX_NAME_LENGTH){
        if (error) *error = "the template name is longer than " + std::to_string(MAX_NAME_LENGTH) + " characters";
        return false;
    }
    for (unsigned char c : trimmed){
        if (c < 0x20 || c == 0x7f){
            if (error) *error = "the template name has control characters";
            return false;
        }
    }
    return true;
}

fs::path editor::PoseTemplates::userFile(const fs::path& projectPath, const std::string& name){
    for (const PoseTemplate& t : userTemplates(projectPath)){
        if (sameName(t.info.name, name)){
            return t.info.file;
        }
    }
    const fs::path dir = userDirectory(projectPath);
    const std::string stem = safeFileStem(name);
    // a different template (or a non-template file) may already use the derived file name
    fs::path file = dir / (stem + ".poseanim");
    std::error_code ec;
    for (int n = 2; fs::exists(file, ec); n++){
        file = dir / (stem + " " + std::to_string(n) + ".poseanim");
    }
    return file;
}

std::string editor::PoseTemplates::fileText(const pose::PoseAnimationData& data, const pose::PoseAnimationTemplateInfo& info){
    pose::PoseAnimationData copy = data;
    copy.previewModel.clear();
    std::vector<pose::ClampReport> reports;
    pose::clampData(copy, reports);
    pose::PoseAnimationTemplateInfo trimmed = info;
    trimmed.name = trim(info.name);
    return pose::writePoseAnimation(copy, &trimmed);
}

void editor::PoseTemplates::apply(const pose::PoseAnimationData& templateData, pose::PoseAnimationData& target, PoseTemplateApply mode){
    if (mode == PoseTemplateApply::Replace){
        const std::string preview = target.previewModel;
        target = templateData;
        target.previewModel = preview;
    }else{
        target.poses.insert(target.poses.end(), templateData.poses.begin(), templateData.poses.end());
    }
}

pose::PoseAnimationData editor::PoseTemplates::newAnimation(const pose::PoseAnimationData& templateData, const std::string& previewModel){
    pose::PoseAnimationData data = templateData;
    data.previewModel = previewModel;
    return data;
}

editor::Command* editor::PoseTemplates::saveCommand(const fs::path& projectPath, const std::string& name,
                                                    const std::string& description, const std::string& category,
                                                    const pose::PoseAnimationData& data, std::string* error, fs::path* file){
    if (!validName(name, error)){
        return nullptr;
    }
    const fs::path target = userFile(projectPath, name);
    if (file){
        *file = target;
    }
    pose::PoseAnimationTemplateInfo info{trim(name), description, category};
    auto* command = new WriteFilesCmd({{target, fileText(data, info)}}, filesChanged);
    command->setNoMerge();
    return command;
}

editor::Command* editor::PoseTemplates::removeCommand(const fs::path& projectPath, const std::string& name, std::string* error,
                                                      fs::path* file){
    for (const PoseTemplate& t : userTemplates(projectPath)){
        if (sameName(t.info.name, name)){
            if (file){
                *file = t.info.file;
            }
            auto* command = new RemoveFilesCmd({t.info.file}, filesChanged);
            command->setNoMerge();
            return command;
        }
    }
    if (error){
        const bool builtin = std::any_of(builtins().begin(), builtins().end(), [&](const PoseTemplate& t){ return sameName(t.info.name, name); });
        *error = builtin ? "'" + name + "' is a built-in template: only project templates can be deleted"
                         : "no project template named '" + name + "'";
    }
    return nullptr;
}

bool editor::PoseTemplates::save(CommandHistory& history, const fs::path& projectPath, const std::string& name,
                                 const std::string& description, const std::string& category,
                                 const pose::PoseAnimationData& data, std::string* error, fs::path* file){
    Command* command = saveCommand(projectPath, name, description, category, data, error, file);
    if (!command){
        return false;
    }
    // addCommand deletes a command whose execute() failed
    if (!history.addCommand(command)){
        if (error) *error = "writing the template failed (permissions, a read-only disk, or a file in the way)";
        return false;
    }
    return true;
}

bool editor::PoseTemplates::remove(CommandHistory& history, const fs::path& projectPath, const std::string& name, std::string* error){
    Command* command = removeCommand(projectPath, name, error);
    if (!command){
        return false;
    }
    if (!history.addCommand(command)){
        if (error) *error = "deleting the template failed";
        return false;
    }
    return true;
}

void editor::PoseTemplates::setFilesChangedHook(std::function<void(const std::vector<fs::path>&)> hook){
    filesChangedHook() = std::move(hook);
}
