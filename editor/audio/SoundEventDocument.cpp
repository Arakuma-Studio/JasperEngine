// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SoundEventDocument.h"

#include "Out.h"
#include "Project.h"
#include "command/type/WriteFilesCmd.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/ProjectFileWrites.h"

#include <fstream>
#include <iterator>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    bool fail(std::string* error, const std::string& message) {
        if (error) *error = message;
        return false;
    }

    bool readText(const fs::path& file, std::string& text) {
        std::ifstream in(file, std::ios::binary);
        if (!in) return false;
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    }

}

bool SoundEventDocument::read(const fs::path& file, SoundEventDef& def, std::string* error) {
    std::string text;
    if (!readText(file, text)) return fail(error, "cannot read " + FileUtils::pathToGenericUtf8(file));
    return SoundEventIO::parse(text, def, error);
}

bool SoundEventDocument::commit(Project* project, const fs::path& file, const SoundEventDef& def, const std::string& undoName,
                                const std::string& mergeKey, std::string* error) {
    if (!project) return fail(error, "no project is open");
    const std::vector<std::string> problems = SoundEventIO::validate(def);
    if (!problems.empty()) return fail(error, problems.front());
    const std::string text = SoundEventIO::write(def);

    std::string current;
    std::error_code ec;
    if (fs::is_regular_file(file, ec) && readText(file, current) && current == text) return true;

    auto* command = new WriteFilesCmd({{file, text}}, [project](const std::vector<fs::path>& changed) {
        ProjectFileWrites::filesWritten(project, changed);
    });
    command->setName(undoName);
    if (mergeKey.empty()) command->setNoMerge();
    else command->setMergeKey("sound-event\n" + FileUtils::pathToGenericUtf8(file) + "\n" + mergeKey);
    auto failure = std::make_shared<std::string>();
    command->reportFailureTo(failure);
    if (!project->getProjectCommandHistory()->addCommand(command)) {
        return fail(error, "writing " + FileUtils::pathToGenericUtf8(file) + " failed" + (failure->empty() ? "" : ": " + *failure));
    }
    return true;
}

std::vector<fs::path> SoundEventDocument::relinkGuids(Project* project, const std::map<std::string, fs::path>& moved) {
    std::vector<fs::path> written;
    if (!project || project->getProjectPath().empty() || moved.empty()) return written;
    for (const fs::path& file : ProjectChecks::findProjectFiles(project->getProjectPath().lexically_normal(), {".soundevent"})) {
        std::string text;
        if (!ProjectChecks::readFile(file, text)) continue;
        bool named = false;
        for (const auto& entry : moved) named = named || text.find(entry.first) != std::string::npos;
        if (!named) continue;
        SoundEventDef def;
        if (!SoundEventIO::parse(text, def)) continue;
        bool changed = false;
        for (SoundEventClip& clip : def.clips) {
            const auto found = moved.find(clip.clip.guid.str());
            if (found == moved.end()) continue;
            const std::string form = project->normalizeToAssetsRelative(found->second).generic_string();
            if (fs::path(clip.clip.path).lexically_normal().generic_string() == form) continue;
            clip.clip.path = form;
            changed = true;
        }
        if (!changed) continue;
        if (!FileUtils::writeFileReplacing(file, SoundEventIO::write(def))) {
            Out::warning("Could not update the clip paths of %s", file.string().c_str());
            continue;
        }
        written.push_back(file);
    }
    if (!written.empty()) ProjectFileWrites::filesWritten(project, written);
    return written;
}
